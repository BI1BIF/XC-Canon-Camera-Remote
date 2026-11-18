#include "xc_ui.h"
#include "xc_hint.h"
#include "xc_portal.h"

#include "bsp_pins.h"
#include "bsp_display.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/libs/qrcode/lv_qrcode.h"
#include "src/misc/lv_text_private.h"
#include <stdio.h>
#include <string.h>

#define BG 0x131718
#define SURFACE 0x202729
#define TEXT 0xf3f6f2
#define MUTED 0xafbfbd
#define RULE 0x3c4a4a
#define AQUA 0x8fe5d4
#define LOW_BATTERY 0xf7be73
#define REC_RED 0xff657b
#define STBY_GREEN 0x61dc92
#define HEADER_TITLE_WIDTH 84
#define CAMERA_NAME_WIDTH 110
#define LIVE_FOOTER_Y 289

LV_FONT_DECLARE(xc_font_14);
LV_FONT_DECLARE(xc_font_20);

const char *const xc_field_name[XC_FIELDS] = {"光圈", "快门", "色温"};

static lv_obj_t *root, *badge, *dot, *title, *subtitle, *detail, *footer, *footer_rule, *note;
static lv_obj_t *hint_cursor;
static lv_timer_t *hint_timer;
static const char *hint_lines[2];
static unsigned hint_chars[2];
static uint32_t hint_start;
static lv_obj_t *header_title;
static lv_font_t title_font;
static lv_obj_t *card, *active_name, *value_window, *active_value[2], *up, *down;
static lv_obj_t *camera_meta, *camera_res, *camera_power_name, *camera_power_value;
static lv_obj_t *record_pill, *record_dot, *record_status;
static unsigned value_slot, value_preset = XC_FIELDS;
static int roll_dir, roll_from, value_y;
static lv_obj_t *qr, *setup_hint;
static lv_obj_t *battery, *battery_fill, *battery_tip, *battery_pct;
static lv_obj_t *small_name[2], *small_value[2], *row_box[XC_MENU_ROWS], *row_label[XC_MENU_ROWS];
static lv_obj_t *row_hint[2], *row_arrow[2];
static int64_t tally_expires;
static bool power_page;
static int battery_soc = -2, battery_mv = -2;
static char power_rows[2][32];

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t fill)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, const lv_font_t *font,
                       uint32_t color, const char *text)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, width);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_text(o, text);
    return o;
}

static void text(lv_obj_t *o, const char *s)
{
    if (!s) s = "";
    if (strcmp(lv_label_get_text(o), s)) lv_label_set_text(o, s);
}

static void visible(lv_obj_t *o, bool show)
{
    if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void record_dot_opacity(void *dot_obj, int32_t opacity)
{
    lv_obj_set_style_bg_opa(dot_obj, (lv_opa_t)opacity, 0);
}

static void update_record_pulse(bool pulse)
{
    const bool running = lv_anim_get(record_dot, record_dot_opacity) != NULL;
    if (pulse == running) return;
    if (!pulse) {
        lv_anim_delete(record_dot, record_dot_opacity);
        record_dot_opacity(record_dot, LV_OPA_COVER);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, record_dot);
    lv_anim_set_exec_cb(&anim, record_dot_opacity);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_duration(&anim, 900);
    lv_anim_set_reverse_duration(&anim, 900);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

static void draw_hint(lv_timer_t *timer)
{
    const xc_hint_frame_t frame = xc_hint_frame(lv_tick_elaps(hint_start), hint_chars[0], hint_chars[1]);
    const char *line = hint_lines[frame.line];
    char shown[64];
    uint32_t bytes = 0, next = 0;
    for (unsigned i = 0; i < frame.chars && line[next]; ++i) {
        lv_text_encoded_next(line, &next);
        if (next >= sizeof(shown)) break;
        bytes = next;
    }
    memcpy(shown, line, bytes);
    shown[bytes] = 0;
    text(footer, shown);
    if (frame.cursor) {
        const lv_text_attributes_t attributes = {0};
        const int32_t width = lv_text_get_width(shown, bytes, &xc_font_14, &attributes);
        lv_obj_set_x(hint_cursor, width < 194 ? 24 + width : 218);
    }
    visible(hint_cursor, frame.cursor);
    lv_timer_set_period(timer, frame.wait_ms);
}

static void update_footer(const xc_view_t *view)
{
    if (hint_timer && view->footer_alt && !view->note && !view->screen_off) {
        if (hint_lines[0] == view->footer && hint_lines[1] == view->footer_alt) return;
        hint_lines[0] = view->footer;
        hint_lines[1] = view->footer_alt;
        for (unsigned i = 0; i < 2; ++i) hint_chars[i] = lv_text_get_encoded_length(hint_lines[i]);
        hint_start = lv_tick_get();
        draw_hint(hint_timer);
        lv_timer_reset(hint_timer);
        lv_timer_resume(hint_timer);
    } else {
        if (hint_timer && hint_lines[1]) lv_timer_pause(hint_timer);
        hint_lines[0] = hint_lines[1] = NULL;
        visible(hint_cursor, false);
        text(footer, view->footer_alt && view->note ? view->note : view->footer);
    }
}

#define VALUE_WIDTH 163
#define ROLL_PX 12

static int value_center_offset(lv_obj_t *value)
{
    return (lv_font_montserrat_36.line_height - lv_obj_get_style_text_font(value, 0)->line_height) / 2;
}

static void roll_frame(void *unused, int32_t progress)
{
    (void)unused;
    lv_obj_t *old = active_value[value_slot ^ 1U];
    lv_obj_t *current = active_value[value_slot];
    value_y = -roll_dir * ROLL_PX * (1000 - progress) / 1000;
    lv_obj_set_y(current, ROLL_PX + value_center_offset(current) + value_y);
    lv_obj_set_style_text_opa(current, 204 + 51 * progress / 1000, 0);
    if (progress == 1000) {
        visible(old, false);
    } else {
        const int old_y = (roll_from * (1000 - progress) + roll_dir * ROLL_PX * progress) / 1000;
        lv_obj_set_y(old, ROLL_PX + value_center_offset(old) + old_y);
        lv_obj_set_style_text_opa(old, 105 * (1000 - progress) / 1000, 0);
    }
}

static void roll_value(unsigned preset, const char *value, int direction)
{
    if (value_preset == preset && !strcmp(lv_label_get_text(active_value[value_slot]), value)) return;
    lv_anim_delete(value_window, roll_frame);
    const bool animate = direction && value_preset == preset;
    value_preset = preset;
    roll_from = value_y;
    roll_dir = direction;
    if (animate) value_slot ^= 1U;
    lv_obj_t *current = active_value[value_slot];
    const lv_font_t *font = &lv_font_montserrat_36;
    const lv_text_attributes_t attributes = {0};
    const size_t len = strlen(value);
    if (lv_text_get_width(value, len, font, &attributes) > VALUE_WIDTH) font = &lv_font_montserrat_24;
    if (lv_text_get_width(value, len, font, &attributes) > VALUE_WIDTH) font = &lv_font_montserrat_20;
    lv_obj_set_style_text_font(current, font, 0);
    text(current, value);
    visible(current, true);
    if (animate) {
        visible(active_value[value_slot ^ 1U], true);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, value_window);
        lv_anim_set_exec_cb(&anim, roll_frame);
        lv_anim_set_values(&anim, 0, 1000);
        lv_anim_set_duration(&anim, 120);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        if (lv_anim_start(&anim)) return;
    }
    roll_frame(NULL, 1000);
}

static void draw_camera_power(const xc_camera_t *camera)
{
    const char *power = "相机电压";
    const char *value = camera->voltage;
    char minutes[16];
    if (camera->power == XC_POWER_DC) power = "外部供电";
    else if (camera->power == XC_POWER_BATTERY) {
        power = "电池供电";
        value = "--";
        if (camera->battery_minutes >= 0) {
            snprintf(minutes, sizeof(minutes), "%d min", camera->battery_minutes);
            value = minutes;
        }
    } else if (camera->power == XC_POWER_CHECKING) {
        power = "检测供电";
        value = "--";
    }
    text(camera_power_name, power);
    text(camera_power_value, value[0] ? value : "--");
}

static void draw_ssid(const char *value)
{
    char shown[33];
    uint32_t pos = 0;
    size_t n = 0;
    while (value[pos]) {
        const uint32_t start = pos;
        const uint32_t cp = lv_text_encoded_next(value, &pos);
        if (pos == start) ++pos;
        lv_font_glyph_dsc_t glyph;
        if (cp >= 32 && lv_font_get_glyph_dsc(&title_font, &glyph, cp, 0) && !glyph.is_placeholder) {
            memcpy(shown + n, value + start, pos - start);
            n += pos - start;
        } else shown[n++] = '?';
    }
    shown[n] = 0;
    text(subtitle, shown);
}

bool xc_ui_title_fits(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    while (*p) {
        uint32_t cp = *p++;
        if (cp >= 128) {
            if (cp < 0xe4 || cp > 0xe9 || (p[0] & 0xc0) != 0x80 || (p[1] & 0xc0) != 0x80)
                return false;
            cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
            p += 2;
        }
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(&title_font, &glyph, cp, 0) || glyph.is_placeholder) return false;
    }
    lv_point_t size;
    lv_text_get_size(&size, value, &title_font, 0, 0, HEADER_TITLE_WIDTH, LV_TEXT_FLAG_EXPAND);
    return size.x <= HEADER_TITLE_WIDTH;
}

void xc_ui_title(const char *value)
{
    text(header_title, value[0] ? value : XC_TITLE_DEFAULT);
}

static void show_tally(bool connected, xc_tally_t tally, const char *offline)
{
    static const uint32_t edges[] = {RULE, RULE, 0x61dc92, 0xf4546d, 0xf4546d};
    static const char *const tallies[] = {"未知", "待命", "PVW", "PGM", "PGM+"};
    const uint32_t edge = edges[tally];
    const lv_color_t tint = lv_color_hex(tally >= XC_PREVIEW ? edge : (connected ? AQUA : MUTED));
    if (!lv_color_eq(lv_obj_get_style_bg_color(root, 0), lv_color_hex(edge)))
        lv_obj_set_style_bg_color(root, lv_color_hex(edge), 0);
    if (!lv_color_eq(lv_obj_get_style_bg_color(dot, 0), tint)) {
        lv_obj_set_style_bg_color(dot, tint, 0);
        lv_obj_set_style_text_color(badge, tint, 0);
    }
    text(badge, connected ? tallies[tally] : offline);
}

static void expire_tally(lv_timer_t *timer)
{
    (void)timer;
    if (tally_expires && esp_timer_get_time() >= tally_expires) {
        tally_expires = 0;
        show_tally(true, XC_UNKNOWN, NULL);
    }
}

void xc_ui_power(int soc, int mv)
{
    if (soc > 100) soc = -1;
    if (soc != battery_soc) {
        battery_soc = soc;
        char pct[8] = "--%";
        if (soc >= 0) snprintf(pct, sizeof(pct), "%u%%", (unsigned)soc);
        text(battery_pct, pct);
        const lv_color_t tint = lv_color_hex(soc < 0 ? MUTED : soc < 20 ? LOW_BATTERY : TEXT);
        lv_obj_set_style_text_color(battery_pct, tint, 0);
        lv_obj_set_style_border_color(battery, tint, 0);
        lv_obj_set_style_bg_color(battery_tip, tint, 0);
        lv_obj_set_style_bg_color(battery_fill, tint, 0);
        visible(battery_fill, soc > 0);
        if (soc > 0) lv_obj_set_width(battery_fill, (soc * 11 + 99) / 100);
        snprintf(power_rows[0], sizeof(power_rows[0]), "电量  %s", pct);
    }
    if (mv != battery_mv) {
        battery_mv = mv;
        if (mv >= 0) snprintf(power_rows[1], sizeof(power_rows[1]), "电压  %d mV", mv);
        else snprintf(power_rows[1], sizeof(power_rows[1]), "电压  -- mV");
    }
    if (power_page) {
        text(row_label[0], power_rows[0]);
        text(row_label[1], power_rows[1]);
    }
}

void xc_ui_init(void)
{
    root = lv_screen_active();
    lv_obj_remove_style_all(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(RULE), 0);
    box(root, 5, 5, BSP_LCD_W - 10, BSP_LCD_H - 10, BSP_LVGL_SCREEN_RADIUS - 5, BG);
    title_font = xc_font_14;
    title_font.fallback = &lv_font_source_han_sans_sc_14_cjk;
    header_title = label(root, 10, 21, HEADER_TITLE_WIDTH, &title_font, TEXT, XC_TITLE_DEFAULT);
    const int tally_w = 50;
    lv_obj_t *pill = box(root, (BSP_LCD_W - tally_w) / 2, 21, tally_w, 22, 6, SURFACE);
    dot = box(pill, 4, 8, 5, 5, 3, AQUA);
    badge = label(pill, 10, 0, 39, &xc_font_14, AQUA, "");
    lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 10, -2);
    battery = box(root, 174, 27, 15, 10, 2, BG);
    lv_obj_set_style_border_width(battery, 1, 0);
    battery_fill = box(root, 176, 29, 11, 6, 0, TEXT);
    battery_tip = box(root, 189, 30, 2, 4, 0, TEXT);
    battery_pct = label(root, 192, 24, 39, &lv_font_montserrat_14, TEXT, "");
    box(root, 20, 51, 200, 1, 0, RULE);
    title = label(root, 20, 59, 200, &xc_font_20, TEXT, "");
    subtitle = label(root, 20, 85, 200, &title_font, MUTED, "");
    record_pill = box(root, 20, 60, 116, 22, 6, SURFACE);
    record_dot = box(record_pill, 4, 8, 5, 5, 3, MUTED);
    record_status = label(record_pill, 11, 0, 105, &xc_font_14, MUTED, "");
    lv_obj_align(record_status, LV_ALIGN_LEFT_MID, 11, -2);
    camera_meta = box(root, 136, 60, 84, 68, 0, BG);
    lv_obj_set_style_bg_opa(camera_meta, LV_OPA_TRANSP, 0);
    lv_obj_t *res_name = label(camera_meta, 0, 0, 84, &xc_font_14, MUTED, "分辨率");
    camera_res = label(camera_meta, 0, 17, 84, &lv_font_montserrat_14, TEXT, "--");
    camera_power_name = label(camera_meta, 0, 33, 84, &xc_font_14, MUTED, "相机电压");
    camera_power_value = label(camera_meta, 0, 51, 84, &lv_font_montserrat_14, TEXT, "--");
    lv_obj_set_style_text_align(res_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(camera_res, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(camera_power_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(camera_power_value, LV_TEXT_ALIGN_RIGHT, 0);
    detail = label(root, 20, 113, 200, &xc_font_14, MUTED, "");
    note = label(root, 20, 254, 200, &xc_font_14, MUTED, "");
    footer_rule = box(root, 20, 273, 200, 1, 0, RULE);
    footer = label(root, 20, 283, 200, &xc_font_14, MUTED, "");
    hint_cursor = box(root, 20, LIVE_FOOTER_Y + 5, 2, 11, 0, AQUA);
    visible(hint_cursor, false);
    hint_timer = lv_timer_create(draw_hint, XC_HINT_HOLD_MS, NULL);
    if (hint_timer) lv_timer_pause(hint_timer);
    setup_hint = label(root, 20, 295, 200, &xc_font_14, MUTED, "长按确定 取消");

    card = box(root, 20, 136, 200, 86, 9, SURFACE);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(AQUA), 0);
    active_name = label(card, 11, 8, 70, &xc_font_14, AQUA, "");
    value_window = box(card, 11, 15, VALUE_WIDTH, 65, 0, SURFACE);
    lv_obj_set_style_bg_opa(value_window, LV_OPA_TRANSP, 0);
    for (unsigned i = 0; i < 2; ++i) {
        active_value[i] = label(value_window, 0, ROLL_PX, VALUE_WIDTH, &lv_font_montserrat_36, TEXT, "");
        visible(active_value[i], false);
    }
    up = label(card, 176, 11, 17, &lv_font_montserrat_14, AQUA, LV_SYMBOL_UP);
    down = label(card, 176, 57, 17, &lv_font_montserrat_14, AQUA, LV_SYMBOL_DOWN);
    for (int i = 0; i < 2; ++i) {
        small_name[i] = label(root, 20 + 105*i, 229, 95, &xc_font_14, MUTED, "");
        small_value[i] = label(root, 20 + 105*i, 248, 95, &lv_font_montserrat_20, TEXT, "");
    }
    for (int i = 0; i < XC_MENU_ROWS; ++i) {
        row_box[i] = box(root, 20, 122 + i*32, 200, 29, 6, SURFACE);
        row_label[i] = label(row_box[i], 8, 4, 183, &xc_font_14, TEXT, "");
        if (i < 2) {
            row_hint[i] = label(row_box[i], 12, 39, 174, &xc_font_14, MUTED, "");
            row_arrow[i] = label(row_box[i], 176, 13, 14, &lv_font_montserrat_14, MUTED, LV_SYMBOL_RIGHT);
            visible(row_hint[i], false);
            visible(row_arrow[i], false);
        }
    }
    xc_ui_power(-1, -1);
    lv_timer_create(expire_tally, 150, NULL);
}

void xc_ui_draw(const xc_view_t *v)
{
    power_page = v->power;
    tally_expires = v->connected && v->tally_time_us ? v->tally_time_us + 1400000 : 0;
    show_tally(v->connected, tally_expires > esp_timer_get_time() ? v->tally : XC_UNKNOWN, v->badge);
    const bool live = v->camera != NULL;
    const bool recording = live && v->camera->recording == XC_REC_ACTIVE;
    update_record_pulse(recording && v->connected && !v->screen_off);
    lv_obj_set_width(title, live ? CAMERA_NAME_WIDTH : 200);
    lv_obj_set_y(title, live ? 85 : 59);
    lv_obj_set_width(subtitle, live ? 112 : 200);
    lv_obj_set_y(subtitle, live ? 109 : 85);
    const lv_text_attributes_t attributes = {0};
    const lv_font_t *name_font = &xc_font_20;
    if (live && v->title && lv_text_get_width(v->title, strlen(v->title), name_font, &attributes) > CAMERA_NAME_WIDTH)
        name_font = &title_font;
    if (lv_obj_get_style_text_font(title, 0) != name_font) lv_obj_set_style_text_font(title, name_font, 0);
    text(title, v->title);
    if (v->subtitle_is_ssid) draw_ssid(v->subtitle);
    else text(subtitle, v->subtitle);
    text(detail, v->detail);
    update_footer(v);
    text(note, v->footer_alt ? NULL : v->note);
    if (v->qr && !qr) {
        qr = lv_qrcode_create(root);
        lv_obj_set_pos(qr, 54, 112);
        lv_qrcode_set_size(qr, 132);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        if (lv_qrcode_update(qr, v->qr, strlen(v->qr)) != LV_RESULT_OK) visible(qr, false);
    } else if (!v->qr && qr) {
        lv_obj_delete(qr);
        qr = NULL;
    }
    visible(setup_hint, v->qr != NULL);
    lv_obj_set_y(footer_rule, v->footer_alt ? 281 : 273);
    lv_obj_set_y(footer, v->qr ? 275 : v->footer_alt ? LIVE_FOOTER_Y : 283);
    visible(card, live);
    visible(camera_meta, live);
    visible(record_pill, live);
    visible(detail, !v->qr && !live && !v->row_count);
    for (int i = 0; i < 2; ++i) {
        visible(small_name[i], live);
        visible(small_value[i], live);
    }
    if (live) {
        const char *state = recording ? "REC" : v->camera->standby ? "休眠" :
                            v->camera->recording == XC_REC_IDLE ? "STBY" : "未知";
        char line[28];
        const int minutes = v->camera->record_minutes;
        if (minutes >= 0)
            snprintf(line, sizeof(line), "%s  余%d分", state, minutes);
        else
            snprintf(line, sizeof(line), "%s  余--", state);
        const bool ready = !v->camera->standby && v->camera->recording == XC_REC_IDLE;
        const lv_color_t color = lv_color_hex(recording ? REC_RED : ready ? STBY_GREEN : MUTED);
        if (!lv_color_eq(lv_obj_get_style_text_color(record_status, 0), color)) {
            lv_obj_set_style_text_color(record_status, color, 0);
            lv_obj_set_style_bg_color(record_dot, color, 0);
        }
        text(record_status, line);
        const xc_setting_t *s = &v->camera->settings[v->preset];
        text(active_name, xc_field_name[v->preset]);
        roll_value(v->preset, s->display, v->roll_direction);
        text(camera_res, v->camera->resolution[0] ? v->camera->resolution : "--");
        draw_camera_power(v->camera);
        const lv_color_t up_color = lv_color_hex(s->lower ? AQUA : RULE);
        const lv_color_t down_color = lv_color_hex(s->higher ? AQUA : RULE);
        if (!lv_color_eq(lv_obj_get_style_text_color(up, 0), up_color))
            lv_obj_set_style_text_color(up, up_color, 0);
        if (!lv_color_eq(lv_obj_get_style_text_color(down, 0), down_color))
            lv_obj_set_style_text_color(down, down_color, 0);
        unsigned j = 0;
        for (unsigned i = 0; i < XC_FIELDS; ++i) {
            if (i == v->preset) continue;
            text(small_name[j], xc_field_name[i]);
            text(small_value[j++], v->camera->settings[i].display);
        }
    } else if (value_preset != XC_FIELDS) {
        lv_anim_delete(value_window, roll_frame);
        value_preset = XC_FIELDS;
    }
    for (unsigned i = 0; i < XC_MENU_ROWS; ++i) {
        visible(row_box[i], i < v->row_count);
        if (i < 2) {
            visible(row_hint[i], v->launcher);
            visible(row_arrow[i], v->launcher);
        }
        if (i >= v->row_count) continue;
        lv_obj_set_pos(row_box[i], 20, v->launcher ? 115 + i*73 : 122 + i*32);
        lv_obj_set_height(row_box[i], v->launcher ? 64 : 29);
        lv_obj_set_pos(row_label[i], v->launcher ? 12 : 8, v->launcher ? 8 : 4);
        lv_obj_set_width(row_label[i], v->launcher ? 156 : 183);
        lv_obj_set_style_text_font(row_label[i], v->launcher ? &xc_font_20 : &xc_font_14, 0);
        if (v->launcher && i < 2) text(row_hint[i], v->row_hint[i]);
        const int border = i == v->focus ? 1 : 0;
        if (lv_obj_get_style_border_width(row_box[i], 0) != border) {
            lv_obj_set_style_border_width(row_box[i], border, 0);
            lv_obj_set_style_border_color(row_box[i], lv_color_hex(AQUA), 0);
        }
        text(row_label[i], power_page && i < 2 ? power_rows[i] : v->row[i]);
    }
}
