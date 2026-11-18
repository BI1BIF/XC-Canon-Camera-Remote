#pragma once

#include "xc_protocol.h"

#define XC_MENU_ROWS 4

typedef struct {
    bool connected, power, subtitle_is_ssid, screen_off, launcher;
    xc_tally_t tally;
    int64_t tally_time_us;
    const char *badge, *title, *subtitle, *detail, *footer, *note, *qr;
    const char *footer_alt; // When set, both footer strings have static lifetime.
    const char *row[XC_MENU_ROWS];
    const char *row_hint[2];
    unsigned row_count, focus;
    const xc_camera_t *camera;
    unsigned preset;
    int roll_direction;
} xc_view_t;

extern const char *const xc_field_name[XC_FIELDS];
void xc_ui_init(void);
void xc_ui_draw(const xc_view_t *view);
void xc_ui_power(int soc, int mv);
bool xc_ui_title_fits(const char *value);
void xc_ui_title(const char *value);
