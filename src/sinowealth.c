#include "sinowealth.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sw_verbose = 0;

static IOHIDManagerRef g_mgr;

static long prop_long(IOHIDDeviceRef dev, CFStringRef key, long def)
{
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    long out = def;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &out);
    return out;
}

static int cmp_iface(const void *a, const void *b)
{
    const sw_iface *x = a, *y = b;
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    /* interface 0 (mouse) has the shorter descriptor and comes first */
    return (int)x->desc_len - (int)y->desc_len;
}

int sw_enumerate(long vid, long pid, sw_iface **out)
{
    *out = NULL;
    g_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int v = (int)vid, p = (int)pid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &p);
    const void *k[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *val[] = { nv, np };
    CFDictionaryRef match = CFDictionaryCreate(NULL, k, val, pid < 0 ? 1 : 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(g_mgr, match);
    CFRelease(match); CFRelease(nv); CFRelease(np);

    CFSetRef set = IOHIDManagerCopyDevices(g_mgr);
    if (!set) return 0;
    CFIndex n = CFSetGetCount(set);
    IOHIDDeviceRef *devs = calloc((size_t)n, sizeof *devs);
    CFSetGetValues(set, (const void **)devs);

    sw_iface *arr = calloc((size_t)n, sizeof *arr);
    for (CFIndex i = 0; i < n; i++) {
        sw_iface *f = &arr[i];
        f->dev = (IOHIDDeviceRef)CFRetain(devs[i]);
        f->vid        = prop_long(f->dev, CFSTR(kIOHIDVendorIDKey), 0);
        f->pid        = prop_long(f->dev, CFSTR(kIOHIDProductIDKey), 0);
        f->usage_page = prop_long(f->dev, CFSTR(kIOHIDPrimaryUsagePageKey), 0);
        f->usage      = prop_long(f->dev, CFSTR(kIOHIDPrimaryUsageKey), 0);
        f->location   = prop_long(f->dev, CFSTR(kIOHIDLocationIDKey), 0);
        f->max_in     = prop_long(f->dev, CFSTR(kIOHIDMaxInputReportSizeKey), 0);
        f->max_out    = prop_long(f->dev, CFSTR(kIOHIDMaxOutputReportSizeKey), 0);
        f->max_feat   = prop_long(f->dev, CFSTR(kIOHIDMaxFeatureReportSizeKey), 0);
        CFTypeRef d = IOHIDDeviceGetProperty(f->dev, CFSTR(kIOHIDReportDescriptorKey));
        if (d && CFGetTypeID(d) == CFDataGetTypeID()) {
            f->desc = CFDataGetBytePtr((CFDataRef)d);
            f->desc_len = (size_t)CFDataGetLength((CFDataRef)d);
        }
    }
    free(devs);
    CFRelease(set);
    qsort(arr, (size_t)n, sizeof *arr, cmp_iface);
    *out = arr;
    return (int)n;
}

sw_iface *sw_find_config_iface(sw_iface *ifs, int n)
{
    for (int i = 0; i < n; i++)
        if (ifs[i].max_feat >= SW_REPORT_LEN) return &ifs[i];
    return NULL;
}

const char *sw_ioreturn_str(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess:         return "success";
    case kIOReturnNotPermitted:    return "not permitted";
    case kIOReturnNotPrivileged:   return "not privileged";
    case kIOReturnExclusiveAccess: return "exclusive access";
    case kIOReturnNotOpen:         return "not open";
    case kIOReturnTimeout:         return "timeout";
    case kIOReturnUnsupported:     return "unsupported";
    case kIOReturnError:           return "general error";
    default:                       return "unknown";
    }
}

IOReturn sw_open(IOHIDDeviceRef dev, int quiet)
{
    IOReturn r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (r == kIOReturnNotPermitted && !quiet)
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent); /* shows the system prompt once */
    if (r != kIOReturnSuccess && !quiet)
        fprintf(stderr, "IOHIDDeviceOpen failed: 0x%08x (%s)\n"
                "Grant Input Monitoring to your terminal (System Settings > Privacy & Security)\n"
                "or run with sudo.\n", r, sw_ioreturn_str(r));
    return r;
}

void sw_close(IOHIDDeviceRef dev)
{
    IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
}

static void dump(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        printf("  %04zx:", i);
        for (size_t j = i; j < i + 16 && j < n; j++) printf(" %02x", p[j]);
        printf("\n");
    }
}

long sw_get_feature(IOHIDDeviceRef dev, uint8_t rid, uint8_t *buf, size_t len)
{
    CFIndex l = (CFIndex)len;
    buf[0] = rid;
    IOReturn r = IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, rid, buf, &l);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "GetReport(feature 0x%02x) failed: 0x%08x (%s)\n", rid, r, sw_ioreturn_str(r));
        return -1;
    }
    return l;
}

int sw_set_feature(IOHIDDeviceRef dev, const uint8_t *buf, size_t len)
{
    IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, buf[0], buf, (CFIndex)len);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "SetReport(feature 0x%02x) failed: 0x%08x (%s)\n", buf[0], r, sw_ioreturn_str(r));
        return -1;
    }
    if (sw_verbose) { printf("-> SET_FEATURE %zu bytes\n", len); dump(buf, len); }
    return 0;
}

long sw_query(IOHIDDeviceRef dev, uint8_t cmd, uint8_t out[SW_CMD_LEN])
{
    uint8_t req[SW_CMD_LEN] = { SW_REPORT_CMD, cmd, 0, 0, 0, 0 };
    if (sw_set_feature(dev, req, sizeof req) != 0) return -1;
    return sw_get_feature(dev, SW_REPORT_CMD, out, SW_CMD_LEN);
}

int sw_read_firmware(IOHIDDeviceRef dev, char out[5])
{
    uint8_t r[SW_CMD_LEN];
    if (sw_query(dev, SW_CMD_FW_VERSION, r) < SW_CMD_LEN) return -1;
    for (int i = 0; i < 4; i++) out[i] = r[2 + i] >= 0x20 && r[2 + i] < 0x7f ? (char)r[2 + i] : '.';
    out[4] = '\0';
    return 0;
}

long sw_read_config(IOHIDDeviceRef dev, int profile, uint8_t *buf, size_t len)
{
    uint8_t req[SW_CMD_LEN] = { SW_REPORT_CMD, SW_PROFILE_CMD(SW_CMD_READ_CONFIG, profile), 0, 0, 0, 0 };
    if (sw_set_feature(dev, req, sizeof req) != 0) return -1;
    return sw_get_feature(dev, SW_REPORT_CONFIG, buf, len);
}

int sw_write_config(IOHIDDeviceRef dev, int profile, const uint8_t *cfg, size_t cfg_len)
{
    if (cfg_len < SW_CFG_MIN || cfg_len > SW_CFG_MAX) {
        fprintf(stderr, "refusing to write: config length %zu outside %d..%d\n",
                cfg_len, SW_CFG_MIN, SW_CFG_MAX);
        return -1;
    }
    uint8_t buf[SW_REPORT_LEN] = {0};
    memcpy(buf, cfg, cfg_len);
    buf[0] = SW_REPORT_CONFIG;
    buf[1] = SW_PROFILE_CMD(SW_CMD_READ_CONFIG, profile);
    buf[SW_OFF_WRITE_FLAG] = (uint8_t)(cfg_len - 8); /* 0x7b for 131-byte configs */
    return sw_set_feature(dev, buf, sizeof buf);
}
