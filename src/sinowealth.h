/*
 * Generic Sinowealth gaming-mouse HID transport and config protocol
 * (the Glorious Model O family), over IOKit's IOHIDManager.
 *
 * Protocol references: gloriousctl, OpenRGB SinowealthController,
 * libratbag driver-sinowealth.c (see README).
 */
#ifndef SINOWEALTH_H
#define SINOWEALTH_H

#include <IOKit/hid/IOHIDManager.h>
#include <stddef.h>
#include <stdint.h>

#define SW_VID 0x258A

/* Vendor reports on the configuration interface. */
#define SW_REPORT_CONFIG 0x04 /* feature, 520 bytes incl. ID: config / button data */
#define SW_REPORT_CMD    0x05 /* feature, 6 bytes incl. ID: command channel */
#define SW_REPORT_LEN    520
#define SW_CMD_LEN       6

/* Command IDs (libratbag). Config / button reads are per profile: +0x10 per profile. */
#define SW_CMD_FW_VERSION  0x01
#define SW_CMD_PROFILE     0x02
#define SW_CMD_READ_CONFIG 0x11
#define SW_CMD_READ_BUTTONS 0x12
#define SW_PROFILE_CMD(base, profile) ((uint8_t)((base) + 0x10 * ((profile) - 1)))

/* Config sizes seen in the family (libratbag: 123..167). */
#define SW_CFG_MIN 123
#define SW_CFG_MAX 167

/* Config block layout (offsets into the report-4 buffer, report ID at 0). */
#define SW_OFF_WRITE_FLAG 0x03 /* 0x00 on read, config_len - 8 on write */
#define SW_OFF_SENSOR     0x09
#define SW_OFF_RATE       0x0a /* low nibble report-rate index, high nibble flags */
#define SW_OFF_STAGES     0x0b /* low nibble stage count, high nibble active stage (1-based) */
#define SW_OFF_DISABLED   0x0c /* bit set = DPI stage disabled */
#define SW_OFF_DPI        0x0d /* 8 (or 8 x/y pairs) DPI values */
#define SW_OFF_DPI_COLOR  0x1d /* 8 stages x R,G,B: DPI indicator colours */
#define SW_DPI_COLOR_LEN  24
#define SW_OFF_EFFECT     0x35 /* main RGB effect */

typedef struct {
    IOHIDDeviceRef dev;
    long vid, pid;
    long usage_page, usage, location;
    long max_in, max_out, max_feat;
    const uint8_t *desc;
    size_t desc_len;
} sw_iface;

extern int sw_verbose;

/* All HID interfaces for vid:pid (pid < 0 = any PID of that vendor), sorted by PID
 * then descriptor length. Returns the count; *out must be freed by the caller. */
int sw_enumerate(long vid, long pid, sw_iface **out);

/* The configuration interface: the one with a >= 520-byte feature report. */
sw_iface *sw_find_config_iface(sw_iface *ifs, int n);

/* Opens the device. The config interface also carries a keyboard collection, so
 * macOS gates this behind Input Monitoring (or root). quiet = no error message. */
IOReturn sw_open(IOHIDDeviceRef dev, int quiet);
void sw_close(IOHIDDeviceRef dev);
const char *sw_ioreturn_str(IOReturn r);

/* buf[0] carries the report ID. get returns bytes received (incl. ID) or -1. */
long sw_get_feature(IOHIDDeviceRef dev, uint8_t rid, uint8_t *buf, size_t len);
int sw_set_feature(IOHIDDeviceRef dev, const uint8_t *buf, size_t len);

/* Command channel: SET 05 <cmd> 00 00 00 00, then GET report 5. */
long sw_query(IOHIDDeviceRef dev, uint8_t cmd, uint8_t out[SW_CMD_LEN]);

/* Firmware version as 4 printable characters (e.g. "2616"). 0 on success. */
int sw_read_firmware(IOHIDDeviceRef dev, char out[5]);

/* Config for profile 1..3 into buf (>= SW_REPORT_LEN). Returns length or -1. */
long sw_read_config(IOHIDDeviceRef dev, int profile, uint8_t *buf, size_t len);

/* Writes back a full config block previously read: sets the command ID and
 * write marker (config_len - 8) and pads to the 520-byte report. */
int sw_write_config(IOHIDDeviceRef dev, int profile, const uint8_t *cfg, size_t cfg_len);

#endif
