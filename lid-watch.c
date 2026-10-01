// lid-watch: use the real lid angle sensor instead of the (stuck) AppleClamshellState
// to decide whether the built-in display should be on.
//
//   angle >= OPEN_DEG  and built-in off                    -> enable built-in, keep previous main display
//   angle <= CLOSE_DEG and built-in on and another display -> disable built-in
//
// Usage:
//   lid-watch                 run forever (meant for a LaunchAgent)
//   lid-watch --dry-run       log decisions without changing displays
//   lid-watch --once <angle>  internal: one decision in a fresh process (spawned by the loop)
//
// Build: clang -O2 -o lid-watch lid-watch.c -framework CoreGraphics -framework IOKit -framework CoreFoundation

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define OPEN_DEG        20
#define CLOSE_DEG       5
#define POLL_USEC       1000000
#define COOLDOWN_SEC    5
#define MAX_DISPLAYS    32

#define LAS_VENDOR_ID   0x05AC
#define LAS_PRODUCT_ID  0x8104
#define LAS_USAGE_PAGE  0x0020
#define LAS_USAGE       0x008A

typedef CGError (*ConfigureEnabledFn)(CGDisplayConfigRef, CGDirectDisplayID, bool);
typedef CGError (*GetDisplayListFn)(uint32_t, CGDirectDisplayID *, uint32_t *);

static bool dry_run;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("%s  ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ---------- private display API ---------- */

static void *lookup(const char *cg_name, const char *sls_name) {
    static void *cg, *sls;
    if (!cg) cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    if (!sls) sls = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    void *fn = cg ? dlsym(cg, cg_name) : NULL;
    if (!fn && sls) fn = dlsym(sls, sls_name);
    return fn;
}

// Last built-in id seen with a real size. Once the panel is off and no external display
// is attached, every built-in candidate (real and phantom) reports 0x0, so fall back to it.
static CGDirectDisplayID last_builtin = 1;

static CGDirectDisplayID find_sized_builtin(void) {
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t cnt = 0;
    if (CGGetOnlineDisplayList(MAX_DISPLAYS, ids, &cnt) == kCGErrorSuccess)
        for (uint32_t i = 0; i < cnt; i++)
            if (CGDisplayIsBuiltin(ids[i]) && CGDisplayPixelsWide(ids[i]) > 0) return ids[i];

    GetDisplayListFn get_list = (GetDisplayListFn)lookup("CGSGetDisplayList", "SLSGetDisplayList");
    if (get_list && get_list(MAX_DISPLAYS, ids, &cnt) == kCGErrorSuccess)
        for (uint32_t i = 0; i < cnt; i++)
            if (CGDisplayIsBuiltin(ids[i]) && CGDisplayPixelsWide(ids[i]) > 0) return ids[i];

    for (CGDirectDisplayID id = 1; id <= 8; id++)
        if (CGDisplayIsBuiltin(id) && CGDisplayPixelsWide(id) > 0) return id;
    return kCGNullDirectDisplay;
}

// Returns the built-in panel id and whether it is really showing an image.
static CGDirectDisplayID find_builtin(bool *is_on) {
    CGDirectDisplayID id = find_sized_builtin();
    if (id != kCGNullDirectDisplay) {
        last_builtin = id;
        *is_on = CGDisplayIsOnline(id) && CGDisplayIsActive(id);
        return id;
    }
    *is_on = false;
    return last_builtin;
}

static uint32_t other_active_displays(CGDirectDisplayID builtin) {
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t cnt = 0, n = 0;
    if (CGGetActiveDisplayList(MAX_DISPLAYS, ids, &cnt) != kCGErrorSuccess) return 0;
    for (uint32_t i = 0; i < cnt; i++)
        if (ids[i] != builtin) n++;
    return n;
}

static bool set_enabled(CGDirectDisplayID id, bool enable) {
    ConfigureEnabledFn configure =
        (ConfigureEnabledFn)lookup("CGSConfigureDisplayEnabled", "SLSConfigureDisplayEnabled");
    if (!configure) {
        logf_("error: ConfigureDisplayEnabled API not available");
        return false;
    }
    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) return false;
    CGError err = configure(config, id, enable);
    if (err != kCGErrorSuccess) {
        logf_("error: ConfigureDisplayEnabled(%u, %d) = %d", id, enable, err);
        CGCancelDisplayConfiguration(config);
        return false;
    }
    err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    if (err != kCGErrorSuccess) {
        logf_("error: CGCompleteDisplayConfiguration = %d", err);
        return false;
    }
    return true;
}

// Enabling the built-in panel makes it the main display; shift every display so the
// previous main one is back at the origin (0,0).
static void restore_main(CGDirectDisplayID prev_main) {
    if (prev_main == kCGNullDirectDisplay || CGMainDisplayID() == prev_main) return;
    // With no external display, the previous "main" was WindowServer's placeholder,
    // which disappears once the panel is enabled; leave the built-in as main then.
    if (CGDisplayIsBuiltin(prev_main) || !CGDisplayIsOnline(prev_main) || !CGDisplayIsActive(prev_main)) return;
    CGRect b = CGDisplayBounds(prev_main);
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t cnt = 0;
    if (CGGetActiveDisplayList(MAX_DISPLAYS, ids, &cnt) != kCGErrorSuccess) return;

    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess) return;
    for (uint32_t i = 0; i < cnt; i++) {
        CGRect r = CGDisplayBounds(ids[i]);
        CGConfigureDisplayOrigin(config, ids[i],
                                 (int32_t)(r.origin.x - b.origin.x), (int32_t)(r.origin.y - b.origin.y));
    }
    if (CGCompleteDisplayConfiguration(config, kCGConfigureForSession) == kCGErrorSuccess)
        logf_("restored display %u as main", prev_main);
}

/* ---------- lid angle sensor ---------- */

static void dict_set_int(CFMutableDictionaryRef d, CFStringRef key, int value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

static IOHIDDeviceRef open_sensor(IOHIDManagerRef *mgr_out) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    dict_set_int(match, CFSTR(kIOHIDVendorIDKey), LAS_VENDOR_ID);
    dict_set_int(match, CFSTR(kIOHIDProductIDKey), LAS_PRODUCT_ID);
    dict_set_int(match, CFSTR(kIOHIDPrimaryUsagePageKey), LAS_USAGE_PAGE);
    dict_set_int(match, CFSTR(kIOHIDPrimaryUsageKey), LAS_USAGE);
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match);
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);

    CFSetRef devices = IOHIDManagerCopyDevices(mgr);
    if (!devices || CFSetGetCount(devices) == 0) {
        if (devices) CFRelease(devices);
        CFRelease(mgr);
        return NULL;
    }
    IOHIDDeviceRef dev;
    CFSetGetValues(devices, (const void **)&dev);
    CFRetain(dev);
    CFRelease(devices);
    if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(dev);
        CFRelease(mgr);
        return NULL;
    }
    *mgr_out = mgr;
    return dev;
}

static int read_angle(IOHIDDeviceRef dev) {
    uint8_t report[8] = {0};
    CFIndex len = sizeof(report);
    if (IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, 1, report, &len) != kIOReturnSuccess || len < 3)
        return -1;
    return report[1] | (report[2] << 8);
}

/* ---------- main loop ---------- */

// CoreGraphics' per-process view of displays stops updating once the process has applied a
// display configuration (observed on macOS 27.0.1: after enabling the panel, the daemon kept
// seeing it as online although WindowServer had disabled it again). So the long-running parent
// never touches CoreGraphics: it only reads the angle and spawns a short-lived child
// ("lid-watch --once <angle>") that sees fresh display state, acts, and exits.

#define CHILD_CHANGED 10

// Child: one decision with fresh CoreGraphics state. Returns CHILD_CHANGED if it acted.
static int run_once(int angle) {
    bool builtin_on;
    CGDirectDisplayID builtin = find_builtin(&builtin_on);

    if (angle >= OPEN_DEG && !builtin_on) {
        CGDirectDisplayID prev_main = CGMainDisplayID();
        logf_("lid open (%d°) but built-in display %u is off -> enabling", angle, builtin);
        if (!dry_run && set_enabled(builtin, true)) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.5, false);
            restore_main(prev_main);
        }
        return CHILD_CHANGED;
    }
    if (builtin_on && angle <= CLOSE_DEG && other_active_displays(builtin) > 0) {
        logf_("lid closed (%d°) and built-in display %u is on -> disabling", angle, builtin);
        if (!dry_run) set_enabled(builtin, false);
        return CHILD_CHANGED;
    }
    return 0;
}

static int spawn_once(const char *self, int angle) {
    char angle_arg[16];
    snprintf(angle_arg, sizeof(angle_arg), "%d", angle);
    char *args[] = {(char *)self, "--once", angle_arg, dry_run ? "--dry-run" : NULL, NULL};
    pid_t pid;
    if (posix_spawn(&pid, self, NULL, NULL, args, environ) != 0) {
        logf_("error: posix_spawn failed");
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--once")) {
        dry_run = argc > 3 && !strcmp(argv[3], "--dry-run");
        return run_once(atoi(argv[2]));
    }

    dry_run = argc > 1 && !strcmp(argv[1], "--dry-run");
    char self[PATH_MAX];
    uint32_t self_len = sizeof(self);
    if (_NSGetExecutablePath(self, &self_len) != 0) {
        fprintf(stderr, "error: cannot resolve own path\n");
        return 1;
    }
    logf_("lid-watch started (open >= %d°, close <= %d°%s)", OPEN_DEG, CLOSE_DEG, dry_run ? ", dry run" : "");

    IOHIDManagerRef mgr = NULL;
    IOHIDDeviceRef dev = NULL;
    time_t last_change = 0;
    int last_logged_angle = -100;

    for (;;) {
        if (!dev) {
            dev = open_sensor(&mgr);
            if (!dev) {
                logf_("lid angle sensor not available, retrying in 10 s");
                sleep(10);
                continue;
            }
        }

        int angle = read_angle(dev);
        if (angle < 0) {
            // Sensor handle can go stale after sleep; reopen it.
            IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
            CFRelease(dev);
            CFRelease(mgr);
            dev = NULL;
            usleep(POLL_USEC);
            continue;
        }
        if (angle - last_logged_angle >= 10 || last_logged_angle - angle >= 10) {
            logf_("angle %d°", angle);
            last_logged_angle = angle;
        }

        // Only spawn when a change is possible; between the thresholds nothing ever happens.
        time_t now = time(NULL);
        if ((angle >= OPEN_DEG || angle <= CLOSE_DEG) && now - last_change >= COOLDOWN_SEC)
            if (spawn_once(self, angle) == CHILD_CHANGED) last_change = now;

        usleep(POLL_USEC);
    }
}
