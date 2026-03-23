/*
 * mouse_fix.c
 * Fixes SteelSeries Rival 3 Gen 2 freezing on macOS via USB-C adapter.
 *
 * Problem: macOS USB idle suspend causes the mouse firmware to hang
 *          after ~15-20 minutes. All HID endpoints stop responding.
 *
 * Solution: Keep the USB connection alive with periodic HID polls,
 *           and automatically reset the device if it still freezes.
 *
 * Build:
 *   clang -o mouse_fix mouse_fix.c -framework IOKit -framework CoreFoundation
 *
 * Run:
 *   ./mouse_fix            (normal - handles most cases)
 *   sudo ./mouse_fix       (if auto-reset needs elevated privileges)
 *
 * To run at login automatically, see the LaunchAgent instructions at the bottom.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

/* SteelSeries Rival 3 Gen 2 identifiers */
#define SS_VID  0x1038
#define SS_PID  0x1870

/*
 * How often to ping the mouse (seconds).
 * Must be shorter than macOS USB idle suspend timeout.
 * 5 seconds is conservative and safe.
 */
#define PING_INTERVAL  5

/*
 * How many consecutive ping failures before attempting USB reset.
 * At 5s intervals, 3 failures = 15 seconds of unresponsiveness.
 */
#define FAIL_THRESHOLD 3

static IOHIDManagerRef  gHidMgr  = NULL;
static CFMutableArrayRef gDevices = NULL;
static int gFailCount = 0;

/* ── Logging ────────────────────────────────────────────────────── */

static void logmsg(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* ── USB Device Reset ───────────────────────────────────────────── */

/*
 * Attempts to reset the mouse at the USB level.
 * This simulates physically unplugging and replugging the device.
 * Uses the IOUSBLib re-enumerate API.
 */
static int try_usb_reset(void) {
    logmsg("Attempting USB device reset...");

    /* Build matching dictionary for the USB device (not HID) */
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (!match) {
        logmsg("  Cannot create matching dictionary");
        return -1;
    }

    int vid = SS_VID, pid = SS_PID;
    CFNumberRef vidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef pidRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR("idVendor"),  vidRef);
    CFDictionarySetValue(match, CFSTR("idProduct"), pidRef);
    CFRelease(vidRef);
    CFRelease(pidRef);

    io_service_t usbDev = IOServiceGetMatchingService(kIOMainPortDefault, match);
    /* match is consumed by IOServiceGetMatchingService */

    if (!usbDev) {
        logmsg("  USB device not found (already disconnected?)");
        return -1;
    }

    /* Create plugin interface for the USB device */
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        usbDev, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
        &plugin, &score);
    IOObjectRelease(usbDev);

    if (kr != kIOReturnSuccess || !plugin) {
        logmsg("  Cannot create USB plugin (0x%x) - try running with sudo", kr);
        return -1;
    }

    /* Get the USB device interface (320+ has ReEnumerate) */
    IOUSBDeviceInterface320 **dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin,
        CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&dev);
    (*plugin)->Release(plugin);

    if (hr != 0 || !dev) {
        logmsg("  Cannot get USB device interface");
        return -1;
    }

    /* Open the USB device */
    kr = (*dev)->USBDeviceOpen(dev);
    if (kr != kIOReturnSuccess) {
        logmsg("  Cannot open USB device (0x%x) - try running with sudo", kr);
        (*dev)->Release(dev);
        return -1;
    }

    /*
     * USBDeviceReEnumerate: simulates a full unplug + replug.
     * This power-cycles the port and forces the mouse firmware to restart.
     * It's the software equivalent of physically pulling the cable.
     */
    logmsg("  Sending re-enumerate (simulated unplug/replug)...");
    kr = (*dev)->USBDeviceReEnumerate(dev, 0);

    if (kr == kIOReturnSuccess) {
        logmsg("  Re-enumerate sent! Mouse should reconnect in a few seconds.");
    } else {
        logmsg("  Re-enumerate failed (0x%x), trying basic reset...", kr);
        kr = (*dev)->ResetDevice(dev);
        if (kr == kIOReturnSuccess)
            logmsg("  Basic reset sent.");
        else
            logmsg("  Basic reset also failed (0x%x). Physical replug needed.", kr);
    }

    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);

    return (kr == kIOReturnSuccess) ? 0 : -1;
}

/* ── HID Callbacks ──────────────────────────────────────────────── */

static void on_device_matched(void *ctx, IOReturn result, void *sender,
                               IOHIDDeviceRef device) {
    char name[256] = "Unknown";
    CFStringRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    if (product)
        CFStringGetCString(product, name, sizeof(name), kCFStringEncodingUTF8);

    IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (r == kIOReturnSuccess) {
        CFArrayAppendValue(gDevices, device);
        logmsg("Connected: %s (keeping alive)", name);
        gFailCount = 0;
    } else {
        logmsg("Found %s but cannot open HID interface (0x%x)", name, r);
    }
}

static void on_device_removed(void *ctx, IOReturn result, void *sender,
                                IOHIDDeviceRef device) {
    CFIndex idx = CFArrayGetFirstIndexOfValue(
        gDevices, CFRangeMake(0, CFArrayGetCount(gDevices)), device);
    if (idx != kCFNotFound)
        CFArrayRemoveValueAtIndex(gDevices, idx);
    logmsg("Disconnected (waiting for reconnection...)");
}

/* ── Keep-Alive Timer ───────────────────────────────────────────── */

/*
 * This fires every PING_INTERVAL seconds.
 * Sends a GetReport to each HID interface to prevent USB idle suspend.
 * If the device is already frozen, the GetReport will fail immediately.
 */
static void on_ping(CFRunLoopTimerRef timer, void *info) {
    CFIndex count = CFArrayGetCount(gDevices);
    if (count == 0) return;

    int errors = 0;
    for (CFIndex i = 0; i < count; i++) {
        IOHIDDeviceRef dev = (IOHIDDeviceRef)CFArrayGetValueAtIndex(gDevices, i);
        uint8_t buf[64];
        CFIndex len = sizeof(buf);
        IOReturn r = IOHIDDeviceGetReport(dev, kIOHIDReportTypeInput, 0, buf, &len);
        if (r != kIOReturnSuccess)
            errors++;
    }

    if (errors == 0) {
        if (gFailCount > 0)
            logmsg("Mouse OK (recovered after %d failed pings)", gFailCount);
        gFailCount = 0;
        return;
    }

    gFailCount++;
    logmsg("Ping failed on %d/%d interfaces (failure %d/%d)",
           errors, (int)count, gFailCount, FAIL_THRESHOLD);

    if (gFailCount >= FAIL_THRESHOLD) {
        logmsg("Mouse frozen! Attempting automatic recovery...");
        try_usb_reset();
        gFailCount = 0;
        /* After reset, device will be removed + re-matched automatically */
    }
}

/* ── Signal Handling ────────────────────────────────────────────── */

static void on_signal(int sig) {
    logmsg("Shutting down...");
    if (gHidMgr) {
        IOHIDManagerClose(gHidMgr, kIOHIDOptionsTypeNone);
        CFRelease(gHidMgr);
    }
    if (gDevices) CFRelease(gDevices);
    exit(0);
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(void) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    logmsg("SteelSeries Rival 3 Gen 2 - USB Fix");
    logmsg("Ping interval: %ds | Auto-reset after: %d failures",
           PING_INTERVAL, FAIL_THRESHOLD);
    logmsg("Waiting for mouse...\n");

    gDevices = CFArrayCreateMutable(kCFAllocatorDefault, 0, NULL);

    /* Create HID manager */
    gHidMgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!gHidMgr) {
        logmsg("ERROR: Cannot create HID manager");
        return 1;
    }

    /* Match only SteelSeries Rival 3 Gen 2 */
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int vid = SS_VID, pid = SS_PID;
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey),  v);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), p);
    CFRelease(v);
    CFRelease(p);

    IOHIDManagerSetDeviceMatching(gHidMgr, match);
    CFRelease(match);

    /* Register callbacks */
    IOHIDManagerRegisterDeviceMatchingCallback(gHidMgr, on_device_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(gHidMgr, on_device_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(gHidMgr, CFRunLoopGetCurrent(),
                                     kCFRunLoopDefaultMode);

    /* Open HID manager */
    IOReturn r = IOHIDManagerOpen(gHidMgr, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        logmsg("ERROR: Cannot open HID manager (0x%x)", r);
        logmsg("");
        logmsg("You need to grant Input Monitoring permission:");
        logmsg("  System Settings > Privacy & Security > Input Monitoring");
        logmsg("  Add this program (mouse_fix) to the list.");
        return 1;
    }

    /* Start keep-alive timer */
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + PING_INTERVAL,
        PING_INTERVAL, 0, 0, on_ping, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    /* Run forever */
    CFRunLoopRun();
    return 0;
}
