/*
 * sinowealth-mouse: userspace macOS configuration utility for Sinowealth-based
 * gaming mice (IOHIDManager; no kernel extension, daemon or vendor software).
 *
 * First confirmed device: NOS M-700 RGB (258a:0029, firmware "2616").
 * Also installed as "nos-m700" (symlink) for backward compatibility.
 */
#include "devices.h"
#include "sinowealth.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <libgen.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SW_MOUSE_VERSION "2.0.0"

static const char *g_prog = "sinowealth-mouse";
static long g_vid = SW_VID, g_pid = -1; /* -1 = auto-detect */
static int g_profile = 1;               /* -p N */
static int g_experimental = 0;          /* --experimental: allow writes to non-confirmed devices */

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

static int all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* Under sudo, files go to the invoking user's home and are chowned back to them. */
static const char *user_home(void)
{
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && *sudo_user) {
        struct passwd *pw = getpwnam(sudo_user);
        if (pw && pw->pw_dir) return pw->pw_dir;
    }
    const char *home = getenv("HOME");
    return home ? home : ".";
}

static void chown_to_user(const char *path)
{
    const char *uid = getenv("SUDO_UID"), *gid = getenv("SUDO_GID");
    if (uid && gid && chown(path, (uid_t)atoi(uid), (gid_t)atoi(gid)) != 0)
        perror(path);
}

static int save_file(const char *path, const uint8_t *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fwrite(p, 1, n, f);
    fclose(f);
    chown_to_user(path);
    return 0;
}

/* ~/.sinowealth-mouse/<kind>-<vid>-<pid>.bin */
static void data_path(char *buf, size_t n, const char *kind, long vid, long pid)
{
    char dir[512];
    snprintf(dir, sizeof dir, "%s/.sinowealth-mouse", user_home());
    if (mkdir(dir, 0755) == 0) chown_to_user(dir);
    snprintf(buf, n, "%s/%s-%04lx-%04lx.bin", dir, kind, vid, pid);
}

/* ---------- device selection ---------- */

typedef struct {
    sw_iface *all;       /* every interface enumerated */
    sw_iface *ifs;       /* interfaces of the selected device (contiguous in all) */
    int n;
    sw_iface *cfg;       /* configuration interface, may be NULL */
    long vid, pid;
    char fw[5];          /* firmware, "" if not read */
    const device_def *def;
    support_level level;
    int opened;
} mouse;

static support_level level_for(long vid, long pid, const char *fw)
{
    const device_def *d = device_lookup((uint16_t)vid, (uint16_t)pid, fw && *fw ? fw : NULL);
    return d ? d->level : SUPPORT_UNKNOWN;
}

/* Picks the device: --pid if given, else the one known, non-unsupported PID present.
 * Unknown PIDs are never auto-selected; they need an explicit --pid. */
static int select_mouse(mouse *m)
{
    memset(m, 0, sizeof *m);
    int nall = sw_enumerate(g_vid, g_pid, &m->all);
    if (nall == 0) {
        if (g_pid >= 0) fprintf(stderr, "No device %04lx:%04lx found.\n", g_vid, g_pid);
        else fprintf(stderr, "No Sinowealth (vendor %04lx) device found.\n", g_vid);
        return -1;
    }

    long chosen = g_pid;
    if (chosen < 0) {
        long candidates[16]; int nc = 0;
        for (int i = 0; i < nall; i++) {
            long pid = m->all[i].pid;
            if (i > 0 && m->all[i - 1].pid == pid) continue;
            support_level l = level_for(g_vid, pid, NULL);
            if (l > SUPPORT_UNSUPPORTED && nc < 16) candidates[nc++] = pid;
        }
        if (nc != 1) {
            fprintf(stderr, nc ? "Several supported Sinowealth devices found; pick one with --pid:\n"
                               : "No known Sinowealth mouse found. Devices present (use --pid to try one):\n");
            for (int i = 0; i < nall; i++) {
                if (i > 0 && m->all[i - 1].pid == m->all[i].pid) continue;
                char prod[128];
                prop_str(m->all[i].dev, CFSTR(kIOHIDProductKey), prod, sizeof prod);
                fprintf(stderr, "  %04lx:%04lx  %-28s %s\n", m->all[i].vid, m->all[i].pid, prod,
                        support_name(level_for(g_vid, m->all[i].pid, NULL)));
            }
            return -1;
        }
        chosen = candidates[0];
    }

    for (int i = 0; i < nall; i++) {
        if (m->all[i].pid != chosen) continue;
        if (!m->ifs) m->ifs = &m->all[i];
        m->n++;
    }
    m->vid = g_vid;
    m->pid = chosen;
    m->cfg = sw_find_config_iface(m->ifs, m->n);
    m->def = device_lookup((uint16_t)m->vid, (uint16_t)m->pid, NULL);
    m->level = m->def ? m->def->level : SUPPORT_UNKNOWN;
    return 0;
}

/* Opens the config interface and refines the device match with the firmware
 * version (e.g. the NOS M-700 is only CONFIRMED with firmware "2616"). */
static int open_mouse(mouse *m, int quiet)
{
    if (m->opened) return 0;
    if (!m->cfg) { if (!quiet) fprintf(stderr, "No configuration interface (520-byte feature report) found.\n"); return -1; }
    if (sw_open(m->cfg->dev, quiet) != kIOReturnSuccess) return -1;
    m->opened = 1;
    if (m->level != SUPPORT_UNSUPPORTED && sw_read_firmware(m->cfg->dev, m->fw) == 0) {
        const device_def *d = device_lookup((uint16_t)m->vid, (uint16_t)m->pid, m->fw);
        if (d) { m->def = d; m->level = d->level; }
    }
    return 0;
}

static void close_mouse(mouse *m)
{
    if (m->opened) sw_close(m->cfg->dev);
    m->opened = 0;
}

static const char *mouse_name(const mouse *m)
{
    return m->def ? m->def->name : "unknown Sinowealth device";
}

/* Vendor commands (05 xx) mean something else on devices using another protocol. */
static int check_protocol(const mouse *m)
{
    if (m->level != SUPPORT_UNSUPPORTED) return 1;
    fprintf(stderr, "%04lx:%04lx (%s) uses a different protocol (%s); refusing to send commands.\n",
            m->vid, m->pid, mouse_name(m), m->def->notes);
    return 0;
}

/* Writes: CONFIRMED devices only, unless the user opts in with --experimental. */
static int check_write(const mouse *m)
{
    if (!check_protocol(m)) return 0;
    if (!support_allows_write(m->level, g_experimental)) {
        fprintf(stderr,
                "%04lx:%04lx firmware \"%s\" (%s) is %s, not hardware-confirmed.\n"
                "Writes are disabled. Read-only commands (info, status, read-config, probe) work.\n"
                "To try anyway, keep a dump (read-config file.bin) and add --experimental.\n",
                m->vid, m->pid, m->fw, mouse_name(m), support_name(m->level));
        return 0;
    }
    if (m->level != SUPPORT_CONFIRMED)
        fprintf(stderr, "warning: experimental write to %04lx:%04lx (%s, %s)\n",
                m->vid, m->pid, mouse_name(m), support_name(m->level));
    return 1;
}

/* A config block is safe to modify and write back only if it looks like one. */
static int check_config(const mouse *m, const uint8_t *cfg, long got)
{
    if (got < SW_CFG_MIN || got > SW_CFG_MAX) {
        fprintf(stderr, "unexpected config length %ld (expected %d..%d)\n", got, SW_CFG_MIN, SW_CFG_MAX);
        return 0;
    }
    if (cfg[0] != SW_REPORT_CONFIG || cfg[1] != SW_PROFILE_CMD(SW_CMD_READ_CONFIG, g_profile)) {
        fprintf(stderr, "unexpected config header %02x %02x\n", cfg[0], cfg[1]);
        return 0;
    }
    if (m->def && m->def->config_len && got != m->def->config_len && !g_experimental) {
        fprintf(stderr, "config length %ld differs from the expected %d for %s\n",
                got, m->def->config_len, m->def->name);
        return 0;
    }
    return 1;
}

/* Saves the full pre-write config so any write can be undone with "restore". */
static void backup_config(const mouse *m, const uint8_t *cfg, long got)
{
    char path[600];
    data_path(path, sizeof path, "backup", m->vid, m->pid);
    if (save_file(path, cfg, (size_t)got) == 0 && sw_verbose)
        printf("Backup: %s\n", path);
}

/* Writes cfg, then reads back and compares everything after the header. */
static int write_verified(const mouse *m, const uint8_t *cfg, long got)
{
    if (sw_write_config(m->cfg->dev, g_profile, cfg, (size_t)got) != 0) return -1;
    usleep(100000);
    uint8_t after[SW_REPORT_LEN];
    long got2 = sw_read_config(m->cfg->dev, g_profile, after, sizeof after);
    if (got2 != got || memcmp(after + 4, cfg + 4, (size_t)got - 4) != 0) {
        fprintf(stderr, "warning: read-back does not match what was written\n");
        if (sw_verbose && got2 > 0) hexdump(after, (size_t)got2);
        return -1;
    }
    return 0;
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

/* ---------- config decoder ---------- */

/* Effect IDs at offset 0x35, as named by OpenRGB (GLORIOUS_MODE_*). */
static const char *effect_names[] = { "off", "rainbow", "static", "spectrum breathing",
    "tail", "spectrum cycle", "(unknown 6)", "rave", "epilepsy", "wave", "breathing" };

static const char *effect_name(uint8_t e)
{
    return e < sizeof effect_names / sizeof *effect_names ? effect_names[e] : "(unknown)";
}

/* Effect colours: OpenRGB/gloriousctl store them R,B,G, but the M-700's default
 * palette reads naturally as R,G,B, so print raw bytes. */
static void print_raw_rgb(const uint8_t *p) { printf("%02x%02x%02x", p[0], p[1], p[2]); }

static void decode_config(const uint8_t *c, size_t n)
{
    if (n < 0x83) { fprintf(stderr, "config too short to decode (%zu bytes)\n", n); return; }
    /* 0x09/0x0a per libratbag driver-sinowealth.c */
    printf("Sensor [0x09]: 0x%02x (libratbag IDs: 06 PMW3360, 08 PMW3212, 0e PMW3327, 0f PMW3389)\n", c[SW_OFF_SENSOR]);
    printf("[0x0a]       : report-rate index %d, flags 0x%x\n", c[SW_OFF_RATE] & 0x0f, c[SW_OFF_RATE] >> 4);
    printf("DPI stages   : %d (active stage %d, disabled mask 0x%02x)\n",
           c[SW_OFF_STAGES] & 0x0f, c[SW_OFF_STAGES] >> 4, c[SW_OFF_DISABLED]);
    for (int i = 0; i < 8; i++) {
        const uint8_t *col = &c[SW_OFF_DPI_COLOR + 3 * i];
        printf("  stage %d    : raw 0x%02x (~%5d DPI?)  colour %02x%02x%02x%s\n", i + 1,
               c[SW_OFF_DPI + i], (c[SW_OFF_DPI + i] + 1) * 100, col[0], col[1], col[2],
               c[SW_OFF_DISABLED] & (1 << i) ? "  (disabled)" : "");
    }
    uint8_t e = c[SW_OFF_EFFECT];
    printf("RGB effect   : 0x%02x %s\n", e, effect_name(e));
    printf("  rainbow    [0x36] 0x%02x dir %d\n", c[0x36], c[0x37]);
    printf("  static     [0x38] 0x%02x colour ", c[0x38]); print_raw_rgb(&c[0x39]); printf("\n");
    printf("  spec.breath[0x3c] 0x%02x colours:", c[0x3c]);
    for (int i = 0; i < 7; i++) { printf(" "); print_raw_rgb(&c[0x3e + 3 * i]); }
    printf("\n  tail       [0x53] 0x%02x\n", c[0x53]);
    printf("  spec.cycle [0x54] 0x%02x\n", c[0x54]);
    printf("  rave       [0x74] 0x%02x colours ", c[0x74]);
    print_raw_rgb(&c[0x75]); printf(" "); print_raw_rgb(&c[0x78]); printf("\n");
    printf("  wave       [0x7c] 0x%02x\n", c[0x7c]);
    printf("  breathing  [0x7d] 0x%02x colour ", c[0x7d]); print_raw_rgb(&c[0x7e]); printf("\n");
    printf("  (mode byte: high nibble brightness 1/2/4, low nibble speed 1-3; colours raw byte order)\n");
    printf("[0x81]       : 0x%02x (OpenRGB: \"mode 0 either 0x00 or 0x03\")\n", c[0x81]);
    printf("[0x82]       : 0x%02x (gloriousctl: lift-off distance)\n", c[0x82]);
}

/* ---------- commands: diagnostics ---------- */

static int cmd_info(mouse *m)
{
    char prod[128], manu[128], serial[128], transport[32];
    IOHIDDeviceRef d0 = m->ifs[0].dev;
    prop_str(d0, CFSTR(kIOHIDProductKey), prod, sizeof prod);
    prop_str(d0, CFSTR(kIOHIDManufacturerKey), manu, sizeof manu);
    prop_str(d0, CFSTR(kIOHIDSerialNumberKey), serial, sizeof serial);
    prop_str(d0, CFSTR(kIOHIDTransportKey), transport, sizeof transport);
    long ver = prop_long(d0, CFSTR(kIOHIDVersionNumberKey), 0);

    int have_fw = open_mouse(m, 1) == 0 && m->fw[0];
    close_mouse(m);

    printf("Device       : %s / %s\n", manu, prod);
    printf("VID:PID      : %04lx:%04lx\n", m->vid, m->pid);
    printf("Firmware     : %s\n", have_fw ? m->fw : "(not read: needs sudo or Input Monitoring)");
    printf("Model        : %s\n", mouse_name(m));
    printf("Support      : %s%s\n", support_name(m->level),
           m->level == SUPPORT_CONFIRMED ? " (tested on real hardware)" :
           m->level == SUPPORT_UNKNOWN ? " (read-only unless --experimental)" : "");
    if (!have_fw) {
        size_t n;
        const device_def *t = device_table(&n);
        for (size_t i = 0; i < n; i++)
            if (t[i].pid == m->pid && t[i].firmware && t[i].level == SUPPORT_CONFIRMED)
                printf("               (%s with firmware \"%s\" is CONFIRMED; firmware is\n"
                       "                checked when the device can be opened)\n", t[i].name, t[i].firmware);
    }
    printf("bcdDevice    : %lx.%02lx\n", ver >> 8, ver & 0xff);
    printf("Serial       : %s\n", serial[0] ? serial : "(none)");
    printf("Transport    : %s\n", transport);
    printf("LocationID   : 0x%08lx\n", m->ifs[0].location);
    printf("HID interfaces: %d\n\n", m->n);
    for (int i = 0; i < m->n; i++) {
        sw_iface *f = &m->ifs[i];
        printf("  [%d] usage %04lx:%04lx  desc %3zu B  maxIn %3ld  maxOut %3ld  maxFeature %3ld%s\n",
               i, f->usage_page, f->usage, f->desc_len, f->max_in, f->max_out,
               f->max_feat, f == m->cfg ? "  <- vendor/config" : "");
        CFTypeRef pairs = IOHIDDeviceGetProperty(f->dev, CFSTR(kIOHIDDeviceUsagePairsKey));
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

static int cmd_descriptors(mouse *m, int raw_only)
{
    for (int i = 0; i < m->n; i++) {
        sw_iface *f = &m->ifs[i];
        printf("=== Interface [%d] usage %04lx:%04lx, %zu-byte report descriptor ===\n",
               i, f->usage_page, f->usage, f->desc_len);
        hexdump(f->desc, f->desc_len);
        if (!raw_only) { printf("\n"); parse_descriptor(f->desc, f->desc_len); }
        printf("\n");
    }
    return 0;
}

static int cmd_devices(void)
{
    size_t n;
    const device_def *t = device_table(&n);
    printf("%-10s %-9s %-15s %s\n", "USB ID", "Firmware", "Support", "Model");
    for (size_t i = 0; i < n; i++)
        printf("%04x:%04x  %-9s %-15s %s\n", t[i].vid, t[i].pid, t[i].firmware ? t[i].firmware : "any",
               support_name(t[i].level), t[i].name);
    printf("\nOnly CONFIRMED devices accept writes by default; others need --experimental.\n");
    return 0;
}

static int cmd_get_feature(mouse *m, uint8_t rid, size_t len)
{
    if (!check_protocol(m) || open_mouse(m, 0) != 0) return 1;
    uint8_t *buf = calloc(1, len);
    long got = sw_get_feature(m->cfg->dev, rid, buf, len);
    if (got >= 0) { printf("GET_FEATURE 0x%02x -> %ld bytes\n", rid, got); hexdump(buf, (size_t)got); }
    free(buf);
    close_mouse(m);
    return got < 0;
}

static int cmd_read_config(mouse *m, const char *save)
{
    if (!check_protocol(m) || open_mouse(m, 0) != 0) return 1;
    uint8_t cfg[SW_REPORT_LEN];
    long got = sw_read_config(m->cfg->dev, g_profile, cfg, sizeof cfg);
    close_mouse(m);
    if (got <= 0) return 1;
    printf("Config block (report 0x04, profile %d, %ld bytes):\n", g_profile, got);
    hexdump(cfg, (size_t)got);
    printf("\n");
    decode_config(cfg, (size_t)got);
    if (save && save_file(save, cfg, (size_t)got) == 0) printf("Saved to %s\n", save);
    return 0;
}

static int load_dump(const char *path, uint8_t *buf, size_t len, size_t *got)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    *got = fread(buf, 1, len, f);
    fclose(f);
    return 0;
}

static int cmd_decode(const char *path)
{
    uint8_t cfg[SW_REPORT_LEN];
    size_t got;
    if (load_dump(path, cfg, sizeof cfg, &got) != 0) return 1;
    decode_config(cfg, got);
    return 0;
}

static int cmd_diff(const char *pa, const char *pb)
{
    uint8_t a[SW_REPORT_LEN], b[SW_REPORT_LEN];
    size_t na, nb;
    if (load_dump(pa, a, sizeof a, &na) != 0 || load_dump(pb, b, sizeof b, &nb) != 0) return 1;
    int d = 0;
    for (size_t i = 0; i < na && i < nb; i++)
        if (a[i] != b[i]) { printf("  [0x%02zx] %02x -> %02x\n", i, a[i], b[i]); d++; }
    if (na != nb) printf("  length %zu -> %zu\n", na, nb);
    printf("%d byte(s) differ\n", d);
    return 0;
}

/* Read-only survey: firmware, active profile, and every profile's config. */
static int cmd_probe(mouse *m)
{
    if (!check_protocol(m) || open_mouse(m, 0) != 0) return 1;
    IOHIDDeviceRef dev = m->cfg->dev;
    uint8_t r[SW_CMD_LEN];

    printf("Device        : %04lx:%04lx %s (%s)\n", m->vid, m->pid, mouse_name(m), support_name(m->level));
    printf("Firmware      : \"%s\"\n", m->fw);
    if (sw_query(dev, SW_CMD_PROFILE, r) > 0)
        printf("Active profile: %d  (raw %02x %02x %02x %02x %02x %02x)\n", r[2], r[0], r[1], r[2], r[3], r[4], r[5]);

    for (int p = 1; p <= 3; p++) {
        uint8_t cfg[SW_REPORT_LEN];
        long got = sw_read_config(dev, p, cfg, sizeof cfg);
        printf("\n=== Profile %d (cmd 0x%02x): %ld bytes ===\n", p, SW_PROFILE_CMD(SW_CMD_READ_CONFIG, p), got);
        if (got <= 0) continue;
        char name[32];
        snprintf(name, sizeof name, "config-p%d.bin", p);
        if (save_file(name, cfg, (size_t)got) == 0) printf("Saved %s\n", name);
        if (got >= SW_OFF_EFFECT + 1) {
            printf("  DPI colours:");
            for (int i = 0; i < 6; i++) {
                const uint8_t *c = &cfg[SW_OFF_DPI_COLOR + 3 * i];
                printf(" %02x%02x%02x", c[0], c[1], c[2]);
            }
            printf("\n  stages 0x%02x, effect 0x%02x\n", cfg[SW_OFF_STAGES], cfg[SW_OFF_EFFECT]);
        }
    }

    uint8_t btn[SW_REPORT_LEN];
    uint8_t req[SW_CMD_LEN] = { SW_REPORT_CMD, SW_CMD_READ_BUTTONS, 0, 0, 0, 0 };
    if (sw_set_feature(dev, req, sizeof req) == 0) {
        long got = sw_get_feature(dev, SW_REPORT_CONFIG, btn, sizeof btn);
        if (got > 0) {
            printf("\n=== Buttons, profile 1 (cmd 0x12): %ld bytes ===\n", got);
            hexdump(btn, (size_t)got);
            save_file("buttons-p1.bin", btn, (size_t)got);
        }
    }
    close_mouse(m);
    return 0;
}

/* Print every input report the vendor interface sends (report 7 = change notice). */
static void on_input(void *ctx, IOReturn r, void *sender, IOHIDReportType type,
                     uint32_t rid, uint8_t *report, CFIndex len)
{
    (void)ctx; (void)r; (void)sender; (void)type;
    if ((rid == 1 || rid == 2) && !sw_verbose) return; /* keyboard / consumer noise */
    printf("report 0x%02x (%ld B):", rid, (long)len);
    for (CFIndex i = 0; i < len; i++) printf(" %02x", report[i]);
    printf("\n");
    fflush(stdout);
}

static int cmd_monitor(mouse *m, double seconds)
{
    if (!m->cfg) { fprintf(stderr, "No configuration interface found.\n"); return 1; }
    if (sw_open(m->cfg->dev, 0) != kIOReturnSuccess) return 1;
    static uint8_t buf[SW_REPORT_LEN];
    IOHIDDeviceScheduleWithRunLoop(m->cfg->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(m->cfg->dev, buf, sizeof buf, on_input, NULL);
    printf("Listening on the vendor interface for %.0f s. Press DPI / LED / side buttons...\n", seconds);
    fflush(stdout);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    sw_close(m->cfg->dev);
    return 0;
}

/* ---------- commands: writes ---------- */

/* Patch <offset>=<value> pairs: read, back up, patch, write, verify. */
static int cmd_poke(mouse *m, int argc, char **argv)
{
    if (open_mouse(m, 0) != 0) return 1;
    int rc = 1;
    uint8_t cfg[SW_REPORT_LEN];
    if (!check_write(m)) goto out;
    long got = sw_read_config(m->cfg->dev, g_profile, cfg, sizeof cfg);
    if (!check_config(m, cfg, got)) goto out;
    backup_config(m, cfg, got);
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
    if (write_verified(m, cfg, got) != 0) goto out;
    printf("Read-back: matches\n");
    rc = 0;
out:
    close_mouse(m);
    return rc;
}

static int cmd_restore(mouse *m, const char *path)
{
    uint8_t dump[SW_REPORT_LEN], cur[SW_REPORT_LEN];
    size_t len;
    if (load_dump(path, dump, sizeof dump, &len) != 0) return 1;
    if (open_mouse(m, 0) != 0) return 1;
    int rc = 1;
    if (!check_write(m)) goto out;
    long got = sw_read_config(m->cfg->dev, g_profile, cur, sizeof cur);
    if (!check_config(m, cur, got)) goto out;
    if ((long)len != got || dump[0] != SW_REPORT_CONFIG) {
        fprintf(stderr, "%s is not a %ld-byte config dump of this device\n", path, got);
        goto out;
    }
    backup_config(m, cur, got);
    if (write_verified(m, dump, got) != 0) goto out;
    printf("Restored %s\n", path);
    rc = 0;
out:
    close_mouse(m);
    return rc;
}

/* ---------- lighting ---------- */

/* Saved LED state: 24 DPI colour bytes + 1 effect byte. */
typedef struct { uint8_t dpi[SW_DPI_COLOR_LEN]; uint8_t effect; int valid; } led_state;

static int read_state_file(const char *path, led_state *st)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t b[SW_DPI_COLOR_LEN + 1];
    int ok = fread(b, 1, sizeof b, f) == sizeof b;
    fclose(f);
    if (!ok) return -1;
    memcpy(st->dpi, b, SW_DPI_COLOR_LEN);
    st->effect = b[SW_DPI_COLOR_LEN];
    st->valid = 1;
    return 0;
}

static led_state load_state(const mouse *m)
{
    led_state st = {0};
    char path[600];
    data_path(path, sizeof path, "state", m->vid, m->pid);
    if (read_state_file(path, &st) == 0) return st;
    /* v1 (nos-m700) kept the NOS M-700 state here */
    if (m->vid == 0x258a && m->pid == 0x0029) {
        snprintf(path, sizeof path, "%s/.nos-m700-state.bin", user_home());
        read_state_file(path, &st);
    }
    return st;
}

static void save_state(const mouse *m, const led_state *st)
{
    char path[600];
    data_path(path, sizeof path, "state", m->vid, m->pid);
    uint8_t b[SW_DPI_COLOR_LEN + 1];
    memcpy(b, st->dpi, SW_DPI_COLOR_LEN);
    b[SW_DPI_COLOR_LEN] = st->effect;
    save_file(path, b, sizeof b);
}

/* A black stage colour would keep the wheel dark after "light on", so treat
 * black stages as unset and use the default colour for them. */
static void fill_black_stages(uint8_t *dpi)
{
    for (int i = 0; i < SW_DPI_COLOR_LEN; i += 3)
        if (all_zero(&dpi[i], 3)) memcpy(&dpi[i], &default_dpi_colors[i], 3);
}

enum { ZONE_WHEEL = 1, ZONE_SIDE = 2 };

/* The wheel LED is the DPI-stage indicator: it shows the colour stored for the active
 * stage (0x1d + 3*(stage-1)), so black in every stage = off. "side" is the main effect. */
static int cmd_light(mouse *m, int zones, int on)
{
    if (open_mouse(m, 0) != 0) return 1;
    int rc = 1;
    uint8_t cfg[SW_REPORT_LEN];
    if (!check_write(m)) goto out;
    if ((zones & ZONE_WHEEL) && m->def && !m->def->has_dpi_led) {
        fprintf(stderr, "%s has no DPI indicator LED\n", m->def->name);
        goto out;
    }
    long got = sw_read_config(m->cfg->dev, g_profile, cfg, sizeof cfg);
    if (!check_config(m, cfg, got)) goto out;
    backup_config(m, cfg, got);

    led_state st = load_state(m);
    uint8_t *dpi = &cfg[SW_OFF_DPI_COLOR];

    if (!on) {
        /* Remember what we are turning off, unless it is already off. */
        int changed = 0;
        if ((zones & ZONE_WHEEL) && !all_zero(dpi, SW_DPI_COLOR_LEN)) {
            memcpy(st.dpi, dpi, SW_DPI_COLOR_LEN);
            fill_black_stages(st.dpi);
            changed = 1;
        }
        if ((zones & ZONE_SIDE) && cfg[SW_OFF_EFFECT] != 0x00) {
            st.effect = cfg[SW_OFF_EFFECT];
            changed = 1;
        }
        if (changed) {
            if (!st.valid) {
                if (!(zones & ZONE_WHEEL)) memcpy(st.dpi, default_dpi_colors, SW_DPI_COLOR_LEN);
                if (!(zones & ZONE_SIDE)) st.effect = 0x00;
            }
            st.valid = 1;
            save_state(m, &st);
        }
        if (zones & ZONE_WHEEL) memset(dpi, 0, SW_DPI_COLOR_LEN);
        if (zones & ZONE_SIDE) cfg[SW_OFF_EFFECT] = 0x00;
    } else {
        if (zones & ZONE_WHEEL) {
            memcpy(dpi, st.valid ? st.dpi : default_dpi_colors, SW_DPI_COLOR_LEN);
            fill_black_stages(dpi);
        }
        if (zones & ZONE_SIDE)
            /* No saved effect: fall back to static (0x02) with the stored static colour. */
            cfg[SW_OFF_EFFECT] = st.valid && st.effect ? st.effect : 0x02;
    }

    if (write_verified(m, cfg, got) != 0) goto out;
    if (zones & ZONE_WHEEL) printf("wheel light: %s\n", on ? "on" : "off");
    if (zones & ZONE_SIDE)  printf("side       : %s (effect 0x%02x)\n", on ? "on" : "off", cfg[SW_OFF_EFFECT]);
    rc = 0;
out:
    close_mouse(m);
    return rc;
}

static int cmd_status(mouse *m)
{
    if (!check_protocol(m) || open_mouse(m, 0) != 0) return 1;
    uint8_t cfg[SW_REPORT_LEN];
    long got = sw_read_config(m->cfg->dev, g_profile, cfg, sizeof cfg);
    close_mouse(m);
    if (got <= SW_OFF_EFFECT) { fprintf(stderr, "could not read config (%ld bytes)\n", got); return 1; }
    int stage = cfg[SW_OFF_STAGES] >> 4;
    const uint8_t *c = &cfg[SW_OFF_DPI_COLOR + 3 * (stage >= 1 && stage <= 8 ? stage - 1 : 0)];
    uint8_t e = cfg[SW_OFF_EFFECT];
    printf("device    : %04lx:%04lx fw \"%s\" %s (%s)\n", m->vid, m->pid, m->fw, mouse_name(m),
           support_name(m->level));
    printf("DPI stage : %d of %d\n", stage, cfg[SW_OFF_STAGES] & 0x0f);
    printf("wheel     : %s (stage colour %02x%02x%02x)\n",
           all_zero(&cfg[SW_OFF_DPI_COLOR], SW_DPI_COLOR_LEN) ? "off" : "on", c[0], c[1], c[2]);
    printf("effect    : 0x%02x %s\n", e, effect_name(e));
    return 0;
}

/* ---------- main ---------- */

static void usage(void)
{
    fprintf(stderr,
        "%s %s: configure Sinowealth gaming mice on macOS\n\n"
        "usage: %s [options] <command>\n\n"
        "Lighting (saved in the mouse, survives re-plugging):\n"
        "  light off|on             scroll-wheel / DPI-indicator LED (scrolling is unaffected)\n"
        "  wheel off|on             same as light\n"
        "  side  off|on             main RGB effect (config 0x35)\n"
        "  all   off|on             both\n"
        "  status                   device, DPI stage and lighting state\n\n"
        "Information:\n"
        "  info                     device, firmware, support level, HID interfaces\n"
        "  devices                  known devices and their support levels\n"
        "  version                  print the version\n\n"
        "Diagnostics (read-only):\n"
        "  descriptors [--raw]      dump and parse all HID report descriptors\n"
        "  read-config [file]       read + decode the config block (cmd 05 11, report 04)\n"
        "  probe                    firmware, active profile, all 3 profile configs, buttons\n"
        "  monitor [seconds]        print vendor input reports (report 7), default 30 s\n"
        "  get-feature <id> [len]   raw GET_FEATURE on the vendor interface\n"
        "  decode <file>            decode a saved config dump (no device needed)\n"
        "  diff <a> <b>             byte diff of two dumps (no device needed)\n\n"
        "Low-level writes:\n"
        "  poke <off> <val> ...     patch config bytes (backs up first, verifies after)\n"
        "  restore <file>           write a saved config dump back\n\n"
        "Options:\n"
        "  --pid <hex>              select a device by product ID (e.g. --pid 0029)\n"
        "  --vid <hex>              vendor ID (default 258a)\n"
        "  -p N                     profile 1-3 (default 1)\n"
        "  --experimental           allow writes to devices not confirmed on hardware\n"
        "  -v                       verbose: print every SET_FEATURE payload\n\n"
        "Writes need sudo or Input Monitoring permission. Only CONFIRMED devices accept\n"
        "writes by default; see '%s devices'.\n",
        g_prog, SW_MOUSE_VERSION, g_prog, g_prog);
}

int main(int argc, char **argv)
{
    g_prog = basename(argv[0]);

    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        if (!strcmp(argv[ai], "-v")) sw_verbose = 1;
        else if (!strcmp(argv[ai], "-p") && ai + 1 < argc) g_profile = atoi(argv[++ai]);
        else if (!strcmp(argv[ai], "--pid") && ai + 1 < argc) g_pid = strtol(argv[++ai], NULL, 16);
        else if (!strcmp(argv[ai], "--vid") && ai + 1 < argc) g_vid = strtol(argv[++ai], NULL, 16);
        else if (!strcmp(argv[ai], "--experimental")) g_experimental = 1;
        else if (!strcmp(argv[ai], "--version")) { printf("%s %s\n", g_prog, SW_MOUSE_VERSION); return 0; }
        else if (!strcmp(argv[ai], "-h") || !strcmp(argv[ai], "--help")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option %s\n\n", argv[ai]); usage(); return 2; }
    }
    if (g_profile < 1 || g_profile > 3) { fprintf(stderr, "profile must be 1-3\n"); return 2; }
    if (g_pid > 0xffff || g_vid < 0 || g_vid > 0xffff) { fprintf(stderr, "bad --vid/--pid\n"); return 2; }
    if (ai >= argc) { usage(); return 2; }
    const char *cmd = argv[ai++];

    /* Commands that need no device. */
    if (!strcmp(cmd, "version")) { printf("%s %s\n", g_prog, SW_MOUSE_VERSION); return 0; }
    if (!strcmp(cmd, "help")) { usage(); return 0; }
    if (!strcmp(cmd, "devices")) return cmd_devices();
    if (!strcmp(cmd, "diff") && ai + 1 < argc) return cmd_diff(argv[ai], argv[ai + 1]);
    if (!strcmp(cmd, "decode") && ai < argc) return cmd_decode(argv[ai]);

    mouse m;
    if (select_mouse(&m) != 0) return 1;

    if (!strcmp(cmd, "light")) cmd = "wheel"; /* it only switches the LED, not the wheel */
    if ((!strcmp(cmd, "wheel") || !strcmp(cmd, "side") || !strcmp(cmd, "all")) && ai < argc &&
        (!strcmp(argv[ai], "on") || !strcmp(argv[ai], "off"))) {
        int zones = !strcmp(cmd, "wheel") ? ZONE_WHEEL : !strcmp(cmd, "side") ? ZONE_SIDE
                                                                              : ZONE_WHEEL | ZONE_SIDE;
        return cmd_light(&m, zones, !strcmp(argv[ai], "on"));
    }
    if (!strcmp(cmd, "status"))
        return cmd_status(&m);
    if (!strcmp(cmd, "info") || !strcmp(cmd, "list"))
        return cmd_info(&m);
    if (!strcmp(cmd, "descriptors"))
        return cmd_descriptors(&m, ai < argc && !strcmp(argv[ai], "--raw"));
    if (!strcmp(cmd, "get-feature") && ai < argc)
        return cmd_get_feature(&m, (uint8_t)strtoul(argv[ai], NULL, 0),
                               ai + 1 < argc ? strtoul(argv[ai + 1], NULL, 0) : SW_REPORT_LEN);
    if (!strcmp(cmd, "read-config"))
        return cmd_read_config(&m, ai < argc ? argv[ai] : NULL);
    if (!strcmp(cmd, "probe"))
        return cmd_probe(&m);
    if (!strcmp(cmd, "monitor"))
        return cmd_monitor(&m, ai < argc ? atof(argv[ai]) : 30.0);
    if (!strcmp(cmd, "poke") && argc - ai >= 2 && (argc - ai) % 2 == 0)
        return cmd_poke(&m, argc - ai, argv + ai);
    if (!strcmp(cmd, "restore") && ai < argc)
        return cmd_restore(&m, argv[ai]);

    usage();
    return 2;
}
