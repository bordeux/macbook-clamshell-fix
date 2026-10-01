// display-on: force the MacBook built-in display on (or off) through the
// private CGSConfigureDisplayEnabled / SLSConfigureDisplayEnabled API.
//
// Usage:
//   display-on            enable the built-in display
//   display-on list       show displays known to CoreGraphics
//   display-on off        disable the built-in display
//   display-on on <id>    enable a specific display id
//
// Build: clang -O2 -o display-on display-on.c -framework CoreGraphics -framework IOKit -framework CoreFoundation

#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef CGError (*ConfigureEnabledFn)(CGDisplayConfigRef, CGDirectDisplayID, bool);
typedef CGError (*GetDisplayListFn)(uint32_t, CGDirectDisplayID *, uint32_t *);

#define MAX_DISPLAYS 32

static void *lookup(const char *cg_name, const char *sls_name) {
    static void *cg, *sls;
    if (!cg) cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    if (!sls) sls = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    void *fn = cg ? dlsym(cg, cg_name) : NULL;
    if (!fn && sls) fn = dlsym(sls, sls_name);
    return fn;
}

static const char *clamshell_state(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
    if (!svc) return "unknown";
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("AppleClamshellState"), kCFAllocatorDefault, 0);
    IOObjectRelease(svc);
    if (!v) return "unknown";
    bool closed = CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    CFRelease(v);
    return closed ? "closed (Yes)" : "open (No)";
}

// Collect every display id we can find: online list, the private full list,
// and low ids (the built-in panel is usually 1 even while it is offline).
static uint32_t collect_displays(CGDirectDisplayID *out) {
    uint32_t n = 0, cnt = 0;
    CGDirectDisplayID tmp[MAX_DISPLAYS];

    if (CGGetOnlineDisplayList(MAX_DISPLAYS, tmp, &cnt) == kCGErrorSuccess)
        for (uint32_t i = 0; i < cnt && n < MAX_DISPLAYS; i++) out[n++] = tmp[i];

    GetDisplayListFn get_list = (GetDisplayListFn)lookup("CGSGetDisplayList", "SLSGetDisplayList");
    if (get_list && get_list(MAX_DISPLAYS, tmp, &cnt) == kCGErrorSuccess)
        for (uint32_t i = 0; i < cnt && n < MAX_DISPLAYS; i++) {
            bool dup = false;
            for (uint32_t j = 0; j < n; j++) dup |= out[j] == tmp[i];
            if (!dup) out[n++] = tmp[i];
        }

    for (CGDirectDisplayID id = 1; id <= 8 && n < MAX_DISPLAYS; id++) {
        bool dup = false;
        for (uint32_t j = 0; j < n; j++) dup |= out[j] == id;
        if (!dup && CGDisplayIsBuiltin(id)) out[n++] = id;
    }
    return n;
}

static CGDirectDisplayID find_builtin(void) {
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t n = collect_displays(ids);
    for (uint32_t i = 0; i < n; i++)
        if (CGDisplayIsBuiltin(ids[i]) && CGDisplayPixelsWide(ids[i]) > 0) return ids[i];
    return kCGNullDirectDisplay;
}

static int cmd_list(void) {
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t n = collect_displays(ids);
    printf("lid sensor: %s\n", clamshell_state());
    printf("%-10s %-8s %-7s %-7s %-6s %s\n", "id", "builtin", "online", "active", "asleep", "size");
    for (uint32_t i = 0; i < n; i++) {
        CGDirectDisplayID d = ids[i];
        printf("%-10u %-8s %-7s %-7s %-6s %zux%zu\n", d,
               CGDisplayIsBuiltin(d) ? "yes" : "no",
               CGDisplayIsOnline(d) ? "yes" : "no",
               CGDisplayIsActive(d) ? "yes" : "no",
               CGDisplayIsAsleep(d) ? "yes" : "no",
               CGDisplayPixelsWide(d), CGDisplayPixelsHigh(d));
    }
    return 0;
}

static int set_enabled(CGDirectDisplayID id, bool enable) {
    ConfigureEnabledFn configure =
        (ConfigureEnabledFn)lookup("CGSConfigureDisplayEnabled", "SLSConfigureDisplayEnabled");
    if (!configure) {
        fprintf(stderr, "error: CGSConfigureDisplayEnabled / SLSConfigureDisplayEnabled not found in this macOS\n");
        return 1;
    }

    CGDisplayConfigRef config;
    CGError err = CGBeginDisplayConfiguration(&config);
    if (err != kCGErrorSuccess) {
        fprintf(stderr, "error: CGBeginDisplayConfiguration failed (%d)\n", err);
        return 1;
    }
    err = configure(config, id, enable);
    if (err != kCGErrorSuccess) {
        fprintf(stderr, "error: ConfigureDisplayEnabled(%u, %s) failed (%d)\n", id, enable ? "on" : "off", err);
        CGCancelDisplayConfiguration(config);
        return 1;
    }
    err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    if (err != kCGErrorSuccess) {
        fprintf(stderr, "error: CGCompleteDisplayConfiguration failed (%d)\n", err);
        return 1;
    }
    printf("display %u: %s requested (lid sensor: %s)\n", id, enable ? "ON" : "OFF", clamshell_state());
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "on";

    if (!strcmp(cmd, "list")) return cmd_list();
    if (strcmp(cmd, "on") && strcmp(cmd, "off")) {
        fprintf(stderr, "usage: %s [on [id] | off [id] | list]\n", argv[0]);
        return 2;
    }

    CGDirectDisplayID id = argc > 2 ? (CGDirectDisplayID)strtoul(argv[2], NULL, 10) : find_builtin();
    if (id == kCGNullDirectDisplay) {
        fprintf(stderr, "error: built-in display not found; run '%s list' and pass the id explicitly\n", argv[0]);
        return 1;
    }
    return set_enabled(id, !strcmp(cmd, "on"));
}
