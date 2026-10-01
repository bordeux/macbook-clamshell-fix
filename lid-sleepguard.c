// lid-sleepguard: root daemon that uses the real lid angle sensor to decide whether
// macOS may sleep, working around a stuck AppleClamshellState = Yes.
//
//   angle >= OPEN_DEG          -> pmset disablesleep 1 (no false Clamshell Sleep while open)
//   angle <= CLOSE_DEG         -> pmset disablesleep 0 (closing the lid sleeps normally)
//   sensor error / exit signal -> pmset disablesleep 0 (fail safe)
//
// Must run as root (LaunchDaemon). Usage:
//   lid-sleepguard             run forever
//   lid-sleepguard --dry-run   log decisions without calling pmset
//
// Build: clang -O2 -o lid-sleepguard lid-sleepguard.c -framework IOKit -framework CoreFoundation

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OPEN_DEG        20
#define CLOSE_DEG       5
#define POLL_USEC       1000000
#define MAX_READ_FAILS  5

#define LAS_VENDOR_ID   0x05AC
#define LAS_PRODUCT_ID  0x8104
#define LAS_USAGE_PAGE  0x0020
#define LAS_USAGE       0x008A

static bool dry_run;
static int sleep_disabled = -1; // -1 unknown, 0 sleep allowed, 1 sleep disabled

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

static void set_sleep_disabled(int disabled, const char *why) {
    if (sleep_disabled == disabled) return;
    logf_("disablesleep %d (%s)", disabled, why);
    if (!dry_run) {
        int rc = system(disabled ? "/usr/bin/pmset -a disablesleep 1" : "/usr/bin/pmset -a disablesleep 0");
        if (rc != 0) {
            logf_("error: pmset exited with %d", rc);
            return;
        }
    }
    sleep_disabled = disabled;
}

static void on_exit_signal(int sig) {
    (void)sig;
    if (!dry_run) system("/usr/bin/pmset -a disablesleep 0");
    _exit(0);
}

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

int main(int argc, char **argv) {
    dry_run = argc > 1 && !strcmp(argv[1], "--dry-run");
    if (!dry_run && geteuid() != 0) {
        fprintf(stderr, "error: must run as root (or use --dry-run)\n");
        return 1;
    }
    signal(SIGTERM, on_exit_signal);
    signal(SIGINT, on_exit_signal);
    signal(SIGHUP, on_exit_signal);

    logf_("lid-sleepguard started (open >= %d°, close <= %d°%s)", OPEN_DEG, CLOSE_DEG, dry_run ? ", dry run" : "");

    IOHIDManagerRef mgr = NULL;
    IOHIDDeviceRef dev = NULL;
    int fails = 0;

    for (;;) {
        if (!dev) {
            dev = open_sensor(&mgr);
            if (!dev) {
                set_sleep_disabled(0, "sensor not available");
                sleep(10);
                continue;
            }
        }

        int angle = read_angle(dev);
        if (angle < 0) {
            if (++fails >= MAX_READ_FAILS) set_sleep_disabled(0, "sensor read failing");
            IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
            CFRelease(dev);
            CFRelease(mgr);
            dev = NULL;
            usleep(POLL_USEC);
            continue;
        }
        fails = 0;

        char why[48];
        if (angle >= OPEN_DEG) {
            snprintf(why, sizeof(why), "lid open, %d°", angle);
            set_sleep_disabled(1, why);
        } else if (angle <= CLOSE_DEG) {
            snprintf(why, sizeof(why), "lid closed, %d°", angle);
            set_sleep_disabled(0, why);
        }
        usleep(POLL_USEC);
    }
}
