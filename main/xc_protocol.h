#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { XC_IRIS, XC_SHUTTER, XC_KELVIN, XC_FIELDS } xc_field_t;
typedef enum { XC_UNKNOWN, XC_OFF, XC_PREVIEW, XC_PROGRAM, XC_BOTH } xc_tally_t;
typedef enum { XC_POWER_UNKNOWN, XC_POWER_DC, XC_POWER_BATTERY, XC_POWER_CHECKING } xc_power_t;
typedef enum { XC_REC_UNKNOWN, XC_REC_IDLE, XC_REC_ACTIVE } xc_record_t;
#define XC_SETTING_STEPS 192
typedef struct {
    int value, lower, higher;
    bool writable, partial;
    uint16_t count;
    int steps[XC_SETTING_STEPS];
    char display[20];
    const char *key;
} xc_setting_t;
typedef struct {
    char model[48], address[32], firmware[20], iso[20], resolution[16], voltage[10];
    xc_tally_t tally;
    xc_power_t power;
    int battery_minutes;  // -1 when the camera has no estimate.
    xc_record_t recording;
    int record_minutes;   // -1 when the camera has no estimate for the active card.
    bool standby;
    xc_setting_t settings[XC_FIELDS];
} xc_camera_t;

typedef enum { XC_EDIT_NONE, XC_EDIT_QUEUED, XC_EDIT_VERIFY } xc_edit_phase_t;
typedef struct {
    int64_t at;
    unsigned revision;
    xc_edit_phase_t phase;
} xc_edit_t;

#define XC_ROOT "/-wvhttp-01-/"
#define XC_ID XC_ROOT "info.cgi?item=s.hardware,s.protocol"
#define XC_TALLY_INFO XC_ROOT "info.cgi?item=f.tally,f.tally.mode"
#define XC_INFO XC_ROOT "info.cgi?item=s.hardware,s.hardware.address,s.protocol,s.firmware,f.tally,f.tally.mode,f.rec.status,f.standby," \
    "c.1.shooting,c.1.exp,c.1.me.fno,c.1.me.diaphragm,c.1.me.shutter,c.1.me.angle," \
    "c.1.me.shutter.angle.list,c.1.me.clearscan,c.1.me.isogain.mode,c.1.me.iso,c.1.me.gain,c.1.wb," \
    "f.rec.media.main,f.rec.media1.videoinfo,f.rec.media2.videoinfo,s.power.source,s.power.volt,s.power.minute," \
    "f.rec.media1.remainingtime,f.rec.media2.remainingtime,f.rec"
#define XC_RECORD_KEY "f.rec"
#define XC_CONTROL XC_ROOT "control.cgi?"

bool xc_parse(const char *body, xc_camera_t *camera);
xc_tally_t xc_parse_tally(const char *body);
void xc_setting_confirm(xc_setting_t *setting, int value);
bool xc_edit_step(xc_edit_t *edit, xc_setting_t *shown, bool up, int64_t now);
bool xc_edit_due(const xc_edit_t *edit, int64_t now);
bool xc_edit_reply(xc_edit_t *edit, unsigned revision, bool accepted, int64_t now);
bool xc_edit_merge(xc_edit_t *edit, const xc_setting_t *shown, xc_setting_t *actual, int64_t now);
bool xc_value(const char *body, const char *key, char *out, size_t size);
bool xc_write_applied(const char *body, const char *key, int value);
xc_record_t xc_record_target(const xc_camera_t *camera);
const char *xc_record_value(xc_record_t target);
bool xc_record_acknowledged(const char *body, xc_record_t target);
bool xc_is_identity(const char *body, char *model, size_t size);
bool xc_camera_matches(const xc_camera_t *camera, const char *model, const char *address);
unsigned xc_next_preset(unsigned mask, unsigned current);
int xc_discovery_port(const char *reply);
int xc_mdns_port(const uint8_t *packet, size_t size);
