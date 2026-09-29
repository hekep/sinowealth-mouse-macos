#include "devices.h"

#include <string.h>

/* Order matters only for rows sharing a PID: put firmware-specific rows first. */
static const device_def table[] = {
    { 0x258a, 0x0029, "2616", "NOS M-700 RGB", SUPPORT_CONFIRMED, 131, 1,
      "tested: wheel LED = DPI indicator, black = off" },
    { 0x258a, 0x0029, NULL, "Sinowealth 0029 (Everest GT-100 RGB, Machenike M620, Mad Dog GM905)",
      SUPPORT_PROTOCOL_MATCH, 131, 1, "OpenRGB + libratbag; firmwares V127 / V287 seen upstream" },
    { 0x258a, 0x0036, NULL, "Glorious Model O / O-", SUPPORT_PROTOCOL_MATCH, 0, 1,
      "gloriousctl, OpenRGB, libratbag" },
    { 0x258a, 0x0033, NULL, "Glorious Model D / D-", SUPPORT_PROTOCOL_MATCH, 0, 1,
      "OpenRGB, libratbag" },
    { 0x258a, 0x0027, NULL,
      "Sinowealth 0027 (Glorious Model O old fw, Genesis Xenon 770, DreamMachines DM5 Blink)",
      SUPPORT_PROTOCOL_MATCH, 0, 1, "libratbag; G-Wolves Hati on this PID has no LEDs" },
    { 0x258a, 0x0028, NULL, "Inphic PG2", SUPPORT_EXPERIMENTAL, 0, 1,
      "libratbag issue #1411: LED control had no effect there" },
    { 0x258a, 0x0051, NULL, "T-Dagger Imperial T-TGM310", SUPPORT_EXPERIMENTAL, 0, 1, "libratbag" },
    { 0x258a, 0x1007, NULL, "Sinowealth 1007 (Marvo Scorpion G961)", SUPPORT_EXPERIMENTAL, 0, 1,
      "mixed evidence: Genesis Xenon 200 / ZET Fury Pro on this PID use another protocol (OpenRGB)" },
    { 0x258a, 0x2011, NULL, "Glorious Model O Wireless (cable)", SUPPORT_UNSUPPORTED, 0, 0, "different protocol (OpenRGB GMOW)" },
    { 0x258a, 0x2022, NULL, "Glorious Model O Wireless (dongle)", SUPPORT_UNSUPPORTED, 0, 0, "different protocol (OpenRGB GMOW)" },
    { 0x258a, 0x2012, NULL, "Glorious Model D Wireless (cable)", SUPPORT_UNSUPPORTED, 0, 0, "different protocol (OpenRGB GMOW)" },
    { 0x258a, 0x2023, NULL, "Glorious Model D Wireless (dongle)", SUPPORT_UNSUPPORTED, 0, 0, "different protocol (OpenRGB GMOW)" },
    { 0x258a, 0x0016, NULL, "Sinowealth keyboard", SUPPORT_UNSUPPORTED, 0, 0, "keyboard protocol" },
    { 0x258a, 0x0090, NULL, "Genesis Thor 300 keyboard", SUPPORT_UNSUPPORTED, 0, 0, "keyboard protocol" },
    { 0x258a, 0x010c, NULL, "Sinowealth keyboard", SUPPORT_UNSUPPORTED, 0, 0, "keyboard protocol" },
};

const uint8_t default_dpi_colors[24] = {
    0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

const device_def *device_lookup(uint16_t vid, uint16_t pid, const char *fw)
{
    const device_def *any = NULL;
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
        const device_def *d = &table[i];
        if (d->vid != vid || d->pid != pid) continue;
        if (d->firmware) {
            if (fw && !strcmp(d->firmware, fw)) return d;
        } else if (!any) {
            any = d;
        }
    }
    return any;
}

const device_def *device_table(size_t *count)
{
    *count = sizeof table / sizeof *table;
    return table;
}

const char *support_name(support_level level)
{
    switch (level) {
    case SUPPORT_CONFIRMED:      return "CONFIRMED";
    case SUPPORT_PROTOCOL_MATCH: return "PROTOCOL_MATCH";
    case SUPPORT_EXPERIMENTAL:   return "EXPERIMENTAL";
    case SUPPORT_UNSUPPORTED:    return "UNSUPPORTED";
    default:                     return "UNKNOWN";
    }
}

int support_allows_write(support_level level, int experimental_opt_in)
{
    if (level == SUPPORT_CONFIRMED) return 1;
    if (level == SUPPORT_UNSUPPORTED) return 0;
    return experimental_opt_in;
}
