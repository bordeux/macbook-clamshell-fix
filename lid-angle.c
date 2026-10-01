// lid-angle: read the MacBook lid angle sensor ("las" HID device) and the
// AppleClamshellState flag, to see whether they disagree.
//
// Usage:
//   lid-angle           print the angle once
//   lid-angle watch     print every 0.5 s until Ctrl+C
//
// Build: clang -O2 -o lid-angle lid-angle.c -framework IOKit -framework CoreFoundation

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LAS_VENDOR_ID   0x05AC
#define LAS_PRODUCT_ID  0x8104
#define LAS_USAGE_PAGE  0x0020 // HID sensor page
#define LAS_USAGE       0x008A // orientation (lid angle)

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

// Feature report 1: byte 0 = report id, bytes 1-2 = angle in degrees (little endian).
static int read_angle(IOHIDDeviceRef dev) {
    uint8_t report[8] = {0};
    CFIndex len = sizeof(report);
    if (IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, 1, report, &len) != kIOReturnSuccess || len < 3)
        return -1;
    return report[1] | (report[2] << 8);
}

static const char *clamshell_state(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
    if (!svc) return "unknown";
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR("AppleClamshellState"), kCFAllocatorDefault, 0);
    IOObjectRelease(svc);
    if (!v) return "unknown";
    bool closed = CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue(v);
    CFRelease(v);
    return closed ? "closed" : "open";
}

static void print_line(IOHIDDeviceRef dev) {
    char ts[16];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
    int angle = read_angle(dev);
    if (angle < 0)
        printf("%s  angle: read error   clamshell: %s\n", ts, clamshell_state());
    else
        printf("%s  angle: %3d°   clamshell: %s\n", ts, angle, clamshell_state());
    fflush(stdout);
}

int main(int argc, char **argv) {
    IOHIDManagerRef mgr = NULL;
    IOHIDDeviceRef dev = open_sensor(&mgr);
    if (!dev) {
        fprintf(stderr, "error: lid angle sensor (VID 0x%04X PID 0x%04X, usage 0x%02X/0x%02X) not found or not openable\n",
                LAS_VENDOR_ID, LAS_PRODUCT_ID, LAS_USAGE_PAGE, LAS_USAGE);
        return 1;
    }

    bool watch = argc > 1 && !strcmp(argv[1], "watch");
    do {
        print_line(dev);
        if (watch) usleep(500000);
    } while (watch);

    IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
    CFRelease(dev);
    CFRelease(mgr);
    return 0;
}
