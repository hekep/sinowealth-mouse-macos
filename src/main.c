/*
 * nos-m700 — userspace utility for the NOS M700 (Sinowealth 258a:0029) mouse.
 *
 * Diagnostic phase: enumerate HID interfaces, dump/parse report
 * descriptors and read feature reports via IOHIDManager. No kernel driver.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define NOS_VID 0x258A
#define NOS_PID 0x0029

/* Other Sinowealth mice with the same report 4/5 protocol can be tried with --vid/--pid. */
static int g_vid = NOS_VID, g_pid = NOS_PID;

/* Sinowealth vendor reports (from the interface-1 descriptor). */
#define REPORT_CMD     0x05 /* feature, 6 bytes incl. ID */
#define REPORT_CONFIG  0x04 /* feature, 520 bytes incl. ID */
#define CMD_FW_VERSION  0x01
#define CMD_PROFILE     0x02
#define CMD_READ_CONFIG 0x11 /* profile 1; 0x21 / 0x31 for profiles 2 / 3 (libratbag) */
#define CMD_READ_BUTTONS 0x12

static int g_profile = 1; /* -p N */
#define PROFILE_CMD(base) ((uint8_t)((base) + 0x10 * (g_profile - 1)))

static int g_verbose = 0;

/* ---------- helpers ---------- */

static long prop_long(IOHIDDeviceRef dev, CFStringRef key, long def)
{
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    long out = def;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)v, kCFNumberLongType, &out);
    return out;
}

static void prop_str(IOHIDDeviceRef dev, CFStringRef key, char *buf, size_t n)
{
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    buf[0] = '\0';
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)v, buf, (CFIndex)n, kCFStringEncodingUTF8);
}

static void hexdump(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        printf("  %04zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < n) printf("%02x ", p[i + j]);
            else printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++) {
            uint8_t c = p[i + j];
            putchar(c >= 0x20 && c < 0x7f ? c : '.');
        }
        printf("|\n");
    }
}

static const char *ioret_str(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess:      return "success";
    case kIOReturnNotPermitted: return "not permitted";
    case kIOReturnNotPrivileged:return "not privileged";
    case kIOReturnExclusiveAccess: return "exclusive access";
    case kIOReturnNotOpen:      return "not open";
    case kIOReturnTimeout:      return "timeout";
    case kIOReturnUnsupported:  return "unsupported";
    case kIOReturnError:        return "general error";
    default:                    return "unknown";
    }
}

/* ---------- device enumeration ---------- */

typedef struct {
    IOHIDDeviceRef dev;
    long usage_page, usage, location;
    long max_in, max_out, max_feat;
    const uint8_t *desc;
    size_t desc_len;
} hid_iface;

static IOHIDManagerRef g_mgr;

static int cmp_iface(const void *a, const void *b)
{
    const hid_iface *x = a, *y = b;
    /* order by descriptor length so interface 0 (mouse) comes first */
    return (int)x->desc_len - (int)y->desc_len;
}

static int enumerate(hid_iface **out)
{
    g_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = g_vid, pid = g_pid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *k[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *v[] = { nv, np };
    CFDictionaryRef match = CFDictionaryCreate(NULL, k, v, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(g_mgr, match);
    CFRelease(match); CFRelease(nv); CFRelease(np);

    CFSetRef set = IOHIDManagerCopyDevices(g_mgr);
    if (!set) { *out = NULL; return 0; }
    CFIndex n = CFSetGetCount(set);
    IOHIDDeviceRef *devs = calloc((size_t)n, sizeof *devs);
    CFSetGetValues(set, (const void **)devs);

    hid_iface *arr = calloc((size_t)n, sizeof *arr);
    for (CFIndex i = 0; i < n; i++) {
        hid_iface *f = &arr[i];
        f->dev = (IOHIDDeviceRef)CFRetain(devs[i]);
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

/* The vendor interface is the one exposing feature reports. */
static hid_iface *find_vendor(hid_iface *arr, int n)
{
    for (int i = 0; i < n; i++)
        if (arr[i].max_feat >= 520) return &arr[i];
    return NULL;
}

/* ---------- report descriptor parser ---------- */

static const char *main_item(uint8_t tag)
{
    switch (tag) {
    case 0x8: return "Input";
    case 0x9: return "Output";
    case 0xB: return "Feature";
    case 0xA: return "Collection";
    case 0xC: return "End Collection";
    default:  return "Main?";
    }
}

static const char *global_item(uint8_t tag)
{
    static const char *n[] = { "Usage Page", "Logical Minimum", "Logical Maximum",
        "Physical Minimum", "Physical Maximum", "Unit Exponent", "Unit",
        "Report Size", "Report ID", "Report Count", "Push", "Pop" };
    return tag < 12 ? n[tag] : "Global?";
}

static const char *local_item(uint8_t tag)
{
    static const char *n[] = { "Usage", "Usage Minimum", "Usage Maximum" };
    return tag < 3 ? n[tag] : "Local?";
}

/* Per-report-ID bit totals, to compute report sizes ourselves. */
typedef struct { unsigned in_bits, out_bits, feat_bits; int used; } rid_sizes;

static void parse_descriptor(const uint8_t *d, size_t len)
{
    rid_sizes sz[256] = {0};
    unsigned rsize = 0, rcount = 0, rid = 0;
    int depth = 0;

    for (size_t i = 0; i < len;) {
        uint8_t b = d[i];
        if (b == 0xFE) { /* long item */
            size_t l = i + 1 < len ? d[i + 1] : 0;
            printf("  %*s(long item, %zu bytes)\n", depth * 2, "", l);
            i += 3 + l;
            continue;
        }
        uint8_t size = b & 3; if (size == 3) size = 4;
        uint8_t type = (b >> 2) & 3, tag = b >> 4;
        uint32_t val = 0;
        for (uint8_t j = 0; j < size && i + 1 + j < len; j++)
            val |= (uint32_t)d[i + 1 + j] << (8 * j);
        int32_t sval = size == 1 ? (int8_t)val : size == 2 ? (int16_t)val : (int32_t)val;

        if (type == 0 && tag == 0xC) depth--;
        printf("  ");
        for (uint8_t j = 0; j < 5; j++)
            printf(j <= size ? "%02x " : "   ", j <= size ? d[i + j] : 0);
        printf("%*s", depth * 2, "");

        switch (type) {
        case 0:
            printf("%s", main_item(tag));
            if (tag == 0xA) printf(" (%s)", val == 0 ? "Physical" : val == 1 ? "Application" :
                                            val == 2 ? "Logical" : "other");
            else if (tag != 0xC) {
                printf(" (%s,%s,%s)", val & 1 ? "Const" : "Data", val & 2 ? "Var" : "Array",
                       val & 4 ? "Rel" : "Abs");
                unsigned bits = rsize * rcount;
                sz[rid].used = 1;
                if (tag == 0x8) sz[rid].in_bits += bits;
                if (tag == 0x9) sz[rid].out_bits += bits;
                if (tag == 0xB) sz[rid].feat_bits += bits;
            }
            break;
        case 1:
            printf("%s", global_item(tag));
            if (tag == 0x0) printf(" (0x%04x)", val);
            else if (tag == 1 || tag == 2) printf(" (%d)", sval);
            else printf(" (%u)", val);
            if (tag == 7) rsize = val;
            if (tag == 8) rid = val & 0xff;
            if (tag == 9) rcount = val;
            break;
        case 2:
            printf("%s (0x%x)", local_item(tag), val);
            break;
        default:
            printf("Reserved");
        }
        printf("\n");
        if (type == 0 && tag == 0xA) depth++;
        i += 1 + size;
    }

    printf("\n  Report sizes (bytes, excluding report-ID byte):\n");
    printf("  %-8s %8s %8s %8s\n", "ReportID", "Input", "Output", "Feature");
    for (int r = 0; r < 256; r++) {
        if (!sz[r].used) continue;
        printf("  0x%02x     %8u %8u %8u\n", r,
               (sz[r].in_bits + 7) / 8, (sz[r].out_bits + 7) / 8, (sz[r].feat_bits + 7) / 8);
    }
}

/* ---------- feature report I/O ---------- */

/* Interface 1 carries a keyboard collection, so macOS gates IOHIDDeviceOpen
 * behind the Input Monitoring privacy permission (or root). */
static IOReturn open_dev(IOHIDDeviceRef dev)
{
    IOReturn r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (r == kIOReturnNotPermitted)
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent); /* shows the system prompt once */
    if (r != kIOReturnSuccess)
        fprintf(stderr, "IOHIDDeviceOpen failed: 0x%08x (%s)\n"
                "Grant Input Monitoring to your terminal (System Settings > Privacy & Security)\n"
                "or run with sudo.\n", r, ioret_str(r));
    return r;
}

/* buf[0] must hold the report ID; returns bytes read (incl. ID) or -1. */
static long get_feature(IOHIDDeviceRef dev, uint8_t rid, uint8_t *buf, size_t len)
{
    CFIndex l = (CFIndex)len;
    buf[0] = rid;
    IOReturn r = IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, rid, buf, &l);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "GetReport(feature 0x%02x) failed: 0x%08x (%s)\n", rid, r, ioret_str(r));
        return -1;
    }
    return l;
}

static int set_feature(IOHIDDeviceRef dev, const uint8_t *buf, size_t len)
{
    IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeFeature, buf[0], buf, (CFIndex)len);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "SetReport(feature 0x%02x) failed: 0x%08x (%s)\n", buf[0], r, ioret_str(r));
        return -1;
    }
    if (g_verbose) { printf("-> SET_FEATURE %zu bytes\n", len); hexdump(buf, len); }
    return 0;
}

/* ---------- commands ---------- */

static int cmd_list(hid_iface *a, int n)
{
    if (n == 0) { fprintf(stderr, "No device %04x:%04x found (try --pid).\n", g_vid, g_pid); return 1; }
    char prod[128], manu[128], serial[128], transport[32];
    prop_str(a[0].dev, CFSTR(kIOHIDProductKey), prod, sizeof prod);
    prop_str(a[0].dev, CFSTR(kIOHIDManufacturerKey), manu, sizeof manu);
    prop_str(a[0].dev, CFSTR(kIOHIDSerialNumberKey), serial, sizeof serial);
    prop_str(a[0].dev, CFSTR(kIOHIDTransportKey), transport, sizeof transport);
    long ver = prop_long(a[0].dev, CFSTR(kIOHIDVersionNumberKey), 0);

    printf("Device       : %s / %s\n", manu, prod);
    printf("VID:PID      : %04x:%04x\n", g_vid, g_pid);
    printf("bcdDevice    : %lx.%02lx\n", ver >> 8, ver & 0xff);
    printf("Serial       : %s\n", serial[0] ? serial : "(none)");
    printf("Transport    : %s\n", transport);
    printf("LocationID   : 0x%08lx\n", a[0].location);
    printf("HID interfaces: %d\n\n", n);
    for (int i = 0; i < n; i++) {
        printf("  [%d] usage %04lx:%04lx  desc %3zu B  maxIn %3ld  maxOut %3ld  maxFeature %3ld%s\n",
               i, a[i].usage_page, a[i].usage, a[i].desc_len, a[i].max_in, a[i].max_out,
               a[i].max_feat, &a[i] == find_vendor(a, n) ? "  <- vendor/config" : "");
        CFTypeRef pairs = IOHIDDeviceGetProperty(a[i].dev, CFSTR(kIOHIDDeviceUsagePairsKey));
        if (pairs && CFGetTypeID(pairs) == CFArrayGetTypeID()) {
            printf("      usage pairs:");
            for (CFIndex j = 0; j < CFArrayGetCount(pairs); j++) {
                CFDictionaryRef p = CFArrayGetValueAtIndex(pairs, j);
                long up = 0, u = 0;
                CFNumberGetValue(CFDictionaryGetValue(p, CFSTR(kIOHIDDeviceUsagePageKey)), kCFNumberLongType, &up);
                CFNumberGetValue(CFDictionaryGetValue(p, CFSTR(kIOHIDDeviceUsageKey)), kCFNumberLongType, &u);
                printf(" %04lx:%04lx", up, u);
            }
            printf("\n");
        }
    }
    return 0;
}

static int cmd_descriptors(hid_iface *a, int n, int raw_only)
{
    for (int i = 0; i < n; i++) {
        printf("=== Interface [%d] usage %04lx:%04lx, %zu-byte report descriptor ===\n",
               i, a[i].usage_page, a[i].usage, a[i].desc_len);
        hexdump(a[i].desc, a[i].desc_len);
        if (!raw_only) { printf("\n"); parse_descriptor(a[i].desc, a[i].desc_len); }
        printf("\n");
    }
    return 0;
}

static int cmd_get_feature(hid_iface *a, int n, uint8_t rid, size_t len)
{
    hid_iface *v = find_vendor(a, n);
    if (!v) { fprintf(stderr, "Vendor interface not found\n"); return 1; }
    if (open_dev(v->dev) != kIOReturnSuccess) return 1;
    uint8_t *buf = calloc(1, len);
    long got = get_feature(v->dev, rid, buf, len);
    if (got >= 0) { printf("GET_FEATURE 0x%02x -> %ld bytes\n", rid, got); hexdump(buf, (size_t)got); }
    free(buf);
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    return got < 0;
}

/* ---------- config block (Glorious Model O compatible layout) ---------- */

#define CFG_LEN        131  /* bytes the device actually returns for report 4 */
#define CFG_WRITE_FLAG 0x7b /* byte 3 on write: CFG_LEN - 8 */

/* Effect IDs at offset 0x35, as named by OpenRGB (GLORIOUS_MODE_*). */
static const char *effect_names[] = { "off", "rainbow", "static", "spectrum breathing",
    "tail", "spectrum cycle", "(unknown 6)", "rave", "epilepsy", "wave", "breathing" };

static long read_config(IOHIDDeviceRef dev, uint8_t *cfg, size_t len)
{
    uint8_t cmd[6] = { REPORT_CMD, PROFILE_CMD(CMD_READ_CONFIG), 0, 0, 0, 0 };
    if (set_feature(dev, cmd, sizeof cmd) != 0) return -1;
    return get_feature(dev, REPORT_CONFIG, cfg, len);
}

/* Writes back a config block read earlier; pads to the 520-byte report. */
static int write_config(IOHIDDeviceRef dev, const uint8_t *cfg, size_t len)
{
    uint8_t buf[520] = {0};
    memcpy(buf, cfg, len < sizeof buf ? len : sizeof buf);
    buf[0] = REPORT_CONFIG;
    buf[1] = PROFILE_CMD(CMD_READ_CONFIG);
    buf[3] = CFG_WRITE_FLAG;
    return set_feature(dev, buf, sizeof buf);
}

/* Effect colours: OpenRGB/gloriousctl store them R,B,G, but the M700's default
 * palette reads naturally as R,G,B, so print raw bytes until a capture settles it. */
static void print_rbg(const uint8_t *p) { printf("%02x%02x%02x", p[0], p[1], p[2]); }

static void decode_config(const uint8_t *c, size_t n)
{
    if (n < CFG_LEN) { fprintf(stderr, "config too short (%zu bytes)\n", n); return; }
    /* 0x09/0x0a per libratbag driver-sinowealth.c */
    printf("Sensor [0x09]: 0x%02x (libratbag IDs: 06 PMW3360, 08 PMW3212, 0e PMW3327, 0f PMW3389)\n", c[0x09]);
    printf("[0x0a]       : report-rate index %d, flags 0x%x\n", c[0x0a] & 0x0f, c[0x0a] >> 4);
    printf("DPI stages   : %d (active nibble %d, disabled mask 0x%02x)\n",
           c[0x0b] & 0x0f, c[0x0b] >> 4, c[0x0c]);
    for (int i = 0; i < 8; i++)
        printf("  stage %d    : raw 0x%02x (~%5d DPI?)  colour %02x%02x%02x%s\n", i + 1, c[0x0d + i], (c[0x0d + i] + 1) * 100,
               c[0x1d + 3 * i], c[0x1e + 3 * i], c[0x1f + 3 * i],
               c[0x0c] & (1 << i) ? "  (disabled)" : "");
    uint8_t e = c[0x35];
    printf("RGB effect   : 0x%02x %s\n", e,
           e < sizeof effect_names / sizeof *effect_names ? effect_names[e] : "(unknown)");
    printf("  rainbow    [0x36] 0x%02x dir %d\n", c[0x36], c[0x37]);
    printf("  static     [0x38] 0x%02x colour ", c[0x38]); print_rbg(&c[0x39]); printf("\n");
    printf("  spec.breath[0x3c] 0x%02x colours:", c[0x3c]);
    for (int i = 0; i < 7; i++) { printf(" "); print_rbg(&c[0x3e + 3 * i]); }
    printf("\n  tail       [0x53] 0x%02x\n", c[0x53]);
    printf("  spec.cycle [0x54] 0x%02x\n", c[0x54]);
    printf("  rave       [0x74] 0x%02x colours ", c[0x74]);
    print_rbg(&c[0x75]); printf(" "); print_rbg(&c[0x78]); printf("\n");
    printf("  wave       [0x7c] 0x%02x\n", c[0x7c]);
    printf("  breathing  [0x7d] 0x%02x colour ", c[0x7d]); print_rbg(&c[0x7e]); printf("\n");
    printf("  (mode byte: high nibble brightness 1/2/4, low nibble speed 1-3; colours raw byte order)\n");
    printf("[0x81]       : 0x%02x (OpenRGB: \"mode 0 either 0x00 or 0x03\")\n", c[0x81]);
    printf("[0x82]       : 0x%02x (gloriousctl: lift-off distance)\n", c[0x82]);
}

static hid_iface *open_vendor(hid_iface *a, int n)
{
    hid_iface *v = find_vendor(a, n);
    if (!v) { fprintf(stderr, "Vendor interface not found\n"); return NULL; }
    return open_dev(v->dev) == kIOReturnSuccess ? v : NULL;
}

static int save_file(const char *path, const uint8_t *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fwrite(p, 1, n, f);
    fclose(f);
    return 0;
}

static int cmd_read_config(hid_iface *a, int n, const char *save)
{
    hid_iface *v = open_vendor(a, n);
    if (!v) return 1;
    uint8_t cfg[520];
    long got = read_config(v->dev, cfg, sizeof cfg);
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    if (got <= 0) return 1;
    printf("Config block (report 0x04, %ld bytes):\n", got);
    hexdump(cfg, (size_t)got);
    printf("\n");
    decode_config(cfg, (size_t)got);
    if (save && save_file(save, cfg, (size_t)got) == 0) printf("Saved to %s\n", save);
    return 0;
}

static int cmd_decode(const char *path)
{
    uint8_t cfg[520];
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    size_t got = fread(cfg, 1, sizeof cfg, f);
    fclose(f);
    decode_config(cfg, got);
    return 0;
}

/* Experimental: read config, back it up, patch <offset>=<value> pairs, write, verify. */
static int cmd_poke(hid_iface *a, int n, int argc, char **argv)
{
    hid_iface *v = open_vendor(a, n);
    if (!v) return 1;
    uint8_t cfg[520], after[520];
    long got = read_config(v->dev, cfg, sizeof cfg);
    int rc = 1;
    if (got < CFG_LEN) { fprintf(stderr, "unexpected config length %ld\n", got); goto out; }
    if (save_file("config-before-poke.bin", cfg, (size_t)got) == 0)
        printf("Backup: config-before-poke.bin\n");

    for (int i = 0; i + 1 < argc; i += 2) {
        unsigned long off = strtoul(argv[i], NULL, 0), val = strtoul(argv[i + 1], NULL, 0);
        if (off < 4 || off >= (unsigned long)got || val > 0xff) {
            fprintf(stderr, "bad pair %s=%s (offset must be 4..%ld)\n", argv[i], argv[i + 1], got - 1);
            goto out;
        }
        printf("  [0x%02lx] %02x -> %02lx\n", off, cfg[off], val);
        cfg[off] = (uint8_t)val;
    }
    if (write_config(v->dev, cfg, (size_t)got) != 0) goto out;
    usleep(100000);
    long got2 = read_config(v->dev, after, sizeof after);
    if (got2 > 0) {
        int diffs = 0;
        for (long i = 4; i < got2 && i < got; i++) diffs += after[i] != cfg[i];
        printf("Read-back: %s\n", diffs ? "differs from what was written" : "matches");
        if (diffs && g_verbose) hexdump(after, (size_t)got2);
    }
    rc = 0;
out:
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    return rc;
}

static int cmd_restore(hid_iface *a, int n, const char *path)
{
    uint8_t cfg[520];
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    size_t got = fread(cfg, 1, sizeof cfg, f);
    fclose(f);
    if (got < CFG_LEN || cfg[0] != REPORT_CONFIG) { fprintf(stderr, "%s is not a config dump\n", path); return 1; }
    hid_iface *v = open_vendor(a, n);
    if (!v) return 1;
    int rc = write_config(v->dev, cfg, got);
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    if (rc == 0) printf("Restored %s\n", path);
    return rc != 0;
}

/* ---------- lighting commands ---------- */

#define DPI_COLOR_OFF 0x1d  /* 8 stages x R,G,B */
#define DPI_COLOR_LEN 24
#define EFFECT_OFF    0x35

/* Factory DPI colours read from this mouse (config-baseline.bin). */
static const uint8_t factory_dpi_colors[DPI_COLOR_LEN] = {
    0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* Saved state: 24 DPI colour bytes + 1 effect byte. Under sudo, use the invoking
 * user's home so the file is found again with or without sudo. */
static void state_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && *sudo_user) {
        snprintf(buf, n, "/Users/%s/.nos-m700-state.bin", sudo_user);
        return;
    }
    snprintf(buf, n, "%s/.nos-m700-state.bin", home ? home : ".");
}

typedef struct { uint8_t dpi[DPI_COLOR_LEN]; uint8_t effect; int valid; } led_state;

static led_state load_state(void)
{
    led_state st = {0};
    char path[512];
    state_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t b[DPI_COLOR_LEN + 1];
        if (fread(b, 1, sizeof b, f) == sizeof b) {
            memcpy(st.dpi, b, DPI_COLOR_LEN);
            st.effect = b[DPI_COLOR_LEN];
            st.valid = 1;
        }
        fclose(f);
    }
    return st;
}

static void save_state(const led_state *st)
{
    char path[512];
    state_path(path, sizeof path);
    uint8_t b[DPI_COLOR_LEN + 1];
    memcpy(b, st->dpi, DPI_COLOR_LEN);
    b[DPI_COLOR_LEN] = st->effect;
    if (save_file(path, b, sizeof b) == 0) {
        const char *uid = getenv("SUDO_UID"), *gid = getenv("SUDO_GID");
        if (uid && gid) chown(path, (uid_t)atoi(uid), (gid_t)atoi(gid));
    }
}

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* A black stage colour would keep the wheel dark after "wheel on", so treat
 * black stages as unset and use the factory colour for them. */
static void fill_black_stages(uint8_t *dpi)
{
    for (int i = 0; i < DPI_COLOR_LEN; i += 3)
        if (all_zero(&dpi[i], 3)) memcpy(&dpi[i], &factory_dpi_colors[i], 3);
}

enum { ZONE_WHEEL = 1, ZONE_SIDE = 2 };

static int cmd_light(hid_iface *a, int n, int zones, int on)
{
    hid_iface *v = open_vendor(a, n);
    if (!v) return 1;
    uint8_t cfg[520];
    int rc = 1;
    long got = read_config(v->dev, cfg, sizeof cfg);
    if (got < CFG_LEN) { fprintf(stderr, "unexpected config length %ld\n", got); goto out; }

    led_state st = load_state();
    uint8_t *dpi = &cfg[DPI_COLOR_OFF];

    if (!on) {
        /* Remember what we are turning off, unless it is already off. */
        int changed = 0;
        if ((zones & ZONE_WHEEL) && !all_zero(dpi, DPI_COLOR_LEN)) {
            memcpy(st.dpi, dpi, DPI_COLOR_LEN);
            fill_black_stages(st.dpi);
            changed = 1;
        }
        if ((zones & ZONE_SIDE) && cfg[EFFECT_OFF] != 0x00) {
            st.effect = cfg[EFFECT_OFF]; changed = 1;
        }
        if (changed) {
            if (!st.valid) {
                if (!(zones & ZONE_WHEEL)) memcpy(st.dpi, factory_dpi_colors, DPI_COLOR_LEN);
                if (!(zones & ZONE_SIDE)) st.effect = 0x00;
            }
            st.valid = 1;
            save_state(&st);
        }
        if (zones & ZONE_WHEEL) memset(dpi, 0, DPI_COLOR_LEN);
        if (zones & ZONE_SIDE) cfg[EFFECT_OFF] = 0x00;
    } else {
        if (zones & ZONE_WHEEL) {
            memcpy(dpi, st.valid ? st.dpi : factory_dpi_colors, DPI_COLOR_LEN);
            fill_black_stages(dpi);
        }
        if (zones & ZONE_SIDE)
            /* No saved effect: fall back to static (0x02) with the stored static colour. */
            cfg[EFFECT_OFF] = st.valid && st.effect ? st.effect : 0x02;
    }

    if (write_config(v->dev, cfg, (size_t)got) != 0) goto out;
    usleep(100000);
    uint8_t after[520];
    long got2 = read_config(v->dev, after, sizeof after);
    if (got2 < CFG_LEN || memcmp(after + 4, cfg + 4, CFG_LEN - 4) != 0) {
        fprintf(stderr, "warning: read-back does not match what was written\n");
        goto out;
    }
    if (zones & ZONE_WHEEL) printf("wheel: %s\n", on ? "on" : "off");
    if (zones & ZONE_SIDE)  printf("side : %s (effect 0x%02x)\n", on ? "on" : "off", cfg[EFFECT_OFF]);
    rc = 0;
out:
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    return rc;
}

static int cmd_status(hid_iface *a, int n)
{
    hid_iface *v = open_vendor(a, n);
    if (!v) return 1;
    uint8_t cfg[520];
    long got = read_config(v->dev, cfg, sizeof cfg);
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    if (got < CFG_LEN) return 1;
    int stage = cfg[0x0b] >> 4;
    const uint8_t *c = &cfg[DPI_COLOR_OFF + 3 * (stage > 0 ? stage - 1 : 0)];
    uint8_t e = cfg[EFFECT_OFF];
    printf("DPI stage : %d of %d\n", stage, cfg[0x0b] & 0x0f);
    printf("wheel     : %s (stage colour %02x%02x%02x)\n",
           all_zero(&cfg[DPI_COLOR_OFF], DPI_COLOR_LEN) ? "off" : "on", c[0], c[1], c[2]);
    printf("effect    : 0x%02x %s\n", e,
           e < sizeof effect_names / sizeof *effect_names ? effect_names[e] : "(unknown)");
    return 0;
}

/* Command-channel query: SET 05 <cmd> 00.., then GET report 5 (libratbag query_read). */
static long query_cmd(IOHIDDeviceRef dev, uint8_t c, uint8_t out[6])
{
    uint8_t cmd[6] = { REPORT_CMD, c, 0, 0, 0, 0 };
    if (set_feature(dev, cmd, sizeof cmd) != 0) return -1;
    return get_feature(dev, REPORT_CMD, out, 6);
}

/* Read-only survey: firmware, active profile, and every profile's config. */
static int cmd_probe(hid_iface *a, int n)
{
    hid_iface *v = open_vendor(a, n);
    if (!v) return 1;
    uint8_t r[6];

    if (query_cmd(v->dev, CMD_FW_VERSION, r) > 0)
        printf("Firmware     : %02x %02x %02x %02x  \"%c%c%c%c\"\n", r[2], r[3], r[4], r[5],
               r[2] >= 0x20 && r[2] < 0x7f ? r[2] : '.', r[3] >= 0x20 && r[3] < 0x7f ? r[3] : '.',
               r[4] >= 0x20 && r[4] < 0x7f ? r[4] : '.', r[5] >= 0x20 && r[5] < 0x7f ? r[5] : '.');
    if (query_cmd(v->dev, CMD_PROFILE, r) > 0)
        printf("Active profile: %d  (raw %02x %02x %02x %02x %02x %02x)\n", r[2], r[0], r[1], r[2], r[3], r[4], r[5]);

    int saved = g_profile;
    for (g_profile = 1; g_profile <= 3; g_profile++) {
        uint8_t cfg[520];
        long got = read_config(v->dev, cfg, sizeof cfg);
        printf("\n=== Profile %d (cmd 0x%02x): %ld bytes ===\n", g_profile, PROFILE_CMD(CMD_READ_CONFIG), got);
        if (got <= 0) continue;
        char name[32];
        snprintf(name, sizeof name, "config-p%d.bin", g_profile);
        if (save_file(name, cfg, (size_t)got) == 0) printf("Saved %s\n", name);
        if (got >= CFG_LEN) {
            printf("  DPI colours:");
            for (int i = 0; i < 6; i++) printf(" %02x%02x%02x", cfg[0x1d + 3 * i], cfg[0x1e + 3 * i], cfg[0x1f + 3 * i]);
            printf("\n  active/count 0x%02x, effect 0x%02x, [0x81] 0x%02x\n", cfg[0x0b], cfg[0x35], cfg[0x81]);
        }
    }
    g_profile = saved;

    uint8_t btn[520];
    uint8_t cmd[6] = { REPORT_CMD, CMD_READ_BUTTONS, 0, 0, 0, 0 };
    if (set_feature(v->dev, cmd, sizeof cmd) == 0) {
        long got = get_feature(v->dev, REPORT_CONFIG, btn, sizeof btn);
        if (got > 0) {
            printf("\n=== Buttons, profile 1 (cmd 0x12): %ld bytes ===\n", got);
            hexdump(btn, (size_t)got);
            save_file("buttons-p1.bin", btn, (size_t)got);
        }
    }
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    return 0;
}

/* Print every input report the vendor interface sends (report 7 = change notice). */
static void on_input(void *ctx, IOReturn r, void *sender, IOHIDReportType type,
                     uint32_t rid, uint8_t *report, CFIndex len)
{
    (void)ctx; (void)r; (void)sender; (void)type;
    if (rid == 1 || rid == 2) return; /* skip keyboard / consumer noise unless -v */
    printf("report 0x%02x (%ld B):", rid, (long)len);
    for (CFIndex i = 0; i < len; i++) printf(" %02x", report[i]);
    printf("\n");
    fflush(stdout);
}

static void on_input_verbose(void *ctx, IOReturn r, void *sender, IOHIDReportType type,
                             uint32_t rid, uint8_t *report, CFIndex len)
{
    if (rid == 1 || rid == 2) {
        printf("report 0x%02x (%ld B):", rid, (long)len);
        for (CFIndex i = 0; i < len; i++) printf(" %02x", report[i]);
        printf("\n");
        fflush(stdout);
        return;
    }
    on_input(ctx, r, sender, type, rid, report, len);
}

static int cmd_monitor(hid_iface *a, int n, double seconds)
{
    hid_iface *v = open_vendor(a, n);
    if (!v) return 1;
    static uint8_t buf[520];
    IOHIDDeviceScheduleWithRunLoop(v->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(v->dev, buf, sizeof buf,
                                           g_verbose ? on_input_verbose : on_input, NULL);
    printf("Listening on the vendor interface for %.0f s. Press DPI / LED / side buttons...\n", seconds);
    fflush(stdout);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    IOHIDDeviceClose(v->dev, kIOHIDOptionsTypeNone);
    return 0;
}

static int cmd_diff(const char *pa, const char *pb)
{
    uint8_t a[520], b[520];
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    if (!fa || !fb) { perror(!fa ? pa : pb); return 1; }
    size_t na = fread(a, 1, sizeof a, fa), nb = fread(b, 1, sizeof b, fb);
    fclose(fa); fclose(fb);
    int d = 0;
    for (size_t i = 0; i < na && i < nb; i++)
        if (a[i] != b[i]) { printf("  [0x%02zx] %02x -> %02x\n", i, a[i], b[i]); d++; }
    if (na != nb) printf("  length %zu -> %zu\n", na, nb);
    printf("%d byte(s) differ\n", d);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: nos-m700 [-v] [-p N] [--vid 258a] [--pid 0029] <command>\n\n"
        "Lighting (saved in the mouse, survives re-plugging):\n"
        "  light off|on             scroll-wheel LED only (scrolling is unaffected)\n"
        "  wheel off|on             same as light\n"
        "  side  off|on             main RGB effect (0x35)\n"
        "  all   off|on             both\n"
        "  status                   current DPI stage and lighting state\n\n"
        "Diagnostics:\n"
        "  info                     device + HID interface summary\n"
        "  descriptors [--raw]      dump and parse all HID report descriptors\n"
        "  get-feature <id> [len]   GET_FEATURE on the vendor interface (default len 520)\n"
        "  read-config [file]       read + decode the config block (cmd 05 11, report 04)\n"
        "  decode <file>            decode a saved config dump (no device needed)\n"
        "  monitor [seconds]        print vendor input reports (report 7 etc.), default 30 s\n"
        "  diff <a> <b>             byte-by-byte diff of two dumps (no device needed)\n"
        "  probe                    firmware, active profile, all 3 profile configs, buttons\n\n"
        "  -p N                     profile 1-3 for read-config / poke / restore (default 1)\n\n"
        "Experimental (writes to the mouse):\n"
        "  poke <off> <val> ...     patch config bytes; backs up to config-before-poke.bin\n"
        "  restore <file>           write a saved config dump back to the mouse\n");
}

int main(int argc, char **argv)
{
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        if (!strcmp(argv[ai], "-v")) g_verbose = 1;
        else if (!strcmp(argv[ai], "-p") && ai + 1 < argc) g_profile = atoi(argv[++ai]);
        else if (!strcmp(argv[ai], "--pid") && ai + 1 < argc) g_pid = (int)strtol(argv[++ai], NULL, 16);
        else if (!strcmp(argv[ai], "--vid") && ai + 1 < argc) g_vid = (int)strtol(argv[++ai], NULL, 16);
        else break;
    }
    if (g_profile < 1 || g_profile > 3) { fprintf(stderr, "profile must be 1-3\n"); return 2; }
    if (ai >= argc) { usage(); return 2; }
    const char *cmd = argv[ai++];

    if (!strcmp(cmd, "diff") && ai + 1 < argc)
        return cmd_diff(argv[ai], argv[ai + 1]);
    if (!strcmp(cmd, "decode") && ai < argc)
        return cmd_decode(argv[ai]);

    hid_iface *ifs;
    int n = enumerate(&ifs);
    if (n == 0) { fprintf(stderr, "No device %04x:%04x found (try --pid).\n", g_vid, g_pid); return 1; }

    if (!strcmp(cmd, "light")) cmd = "wheel"; /* alias: it only switches the LED, not the wheel */
    if ((!strcmp(cmd, "wheel") || !strcmp(cmd, "side") || !strcmp(cmd, "all")) && ai < argc &&
        (!strcmp(argv[ai], "on") || !strcmp(argv[ai], "off"))) {
        int zones = !strcmp(cmd, "wheel") ? ZONE_WHEEL : !strcmp(cmd, "side") ? ZONE_SIDE
                                                                              : ZONE_WHEEL | ZONE_SIDE;
        return cmd_light(ifs, n, zones, !strcmp(argv[ai], "on"));
    }
    if (!strcmp(cmd, "status"))
        return cmd_status(ifs, n);
    if (!strcmp(cmd, "info") || !strcmp(cmd, "list"))
        return cmd_list(ifs, n);
    if (!strcmp(cmd, "descriptors"))
        return cmd_descriptors(ifs, n, ai < argc && !strcmp(argv[ai], "--raw"));
    if (!strcmp(cmd, "get-feature") && ai < argc)
        return cmd_get_feature(ifs, n, (uint8_t)strtoul(argv[ai], NULL, 0),
                               ai + 1 < argc ? strtoul(argv[ai + 1], NULL, 0) : 520);
    if (!strcmp(cmd, "read-config"))
        return cmd_read_config(ifs, n, ai < argc ? argv[ai] : NULL);
    if (!strcmp(cmd, "monitor"))
        return cmd_monitor(ifs, n, ai < argc ? atof(argv[ai]) : 30.0);
    if (!strcmp(cmd, "probe"))
        return cmd_probe(ifs, n);
    if (!strcmp(cmd, "poke") && argc - ai >= 2 && (argc - ai) % 2 == 0)
        return cmd_poke(ifs, n, argc - ai, argv + ai);
    if (!strcmp(cmd, "restore") && ai < argc)
        return cmd_restore(ifs, n, argv[ai]);

    usage();
    return 2;
}
