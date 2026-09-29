/*
 * Known Sinowealth mice and how far each is trusted.
 * Add a model by adding a row to the table in devices.c.
 */
#ifndef DEVICES_H
#define DEVICES_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SUPPORT_UNKNOWN,        /* not in the table */
    SUPPORT_UNSUPPORTED,    /* known to use a different protocol: never write */
    SUPPORT_EXPERIMENTAL,   /* expected compatible, weaker evidence */
    SUPPORT_PROTOCOL_MATCH, /* same protocol per upstream projects, not hardware-tested here */
    SUPPORT_CONFIRMED,      /* tested on real hardware with this tool */
} support_level;

typedef struct {
    uint16_t vid, pid;
    const char *firmware;   /* 4-char firmware string this row applies to; NULL = any */
    const char *name;
    support_level level;
    uint8_t config_len;     /* expected config length; 0 = any in SW_CFG_MIN..SW_CFG_MAX */
    int has_dpi_led;        /* DPI indicator colours at SW_OFF_DPI_COLOR */
    const char *notes;
} device_def;

/* Best match for vid:pid; a row whose firmware matches wins over a firmware-less row.
 * fw may be NULL when the firmware could not be read. Returns NULL if unknown. */
const device_def *device_lookup(uint16_t vid, uint16_t pid, const char *fw);

const device_def *device_table(size_t *count);
const char *support_name(support_level level);

/* Writes are allowed without --experimental only for CONFIRMED devices. */
int support_allows_write(support_level level, int experimental_opt_in);

/* Palette used by "light on" when nothing was saved (the NOS M-700 factory colours,
 * which match the Glorious defaults). */
extern const uint8_t default_dpi_colors[24];

#endif
