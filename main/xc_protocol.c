#include "xc_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *find_value(const char *body, const char *key, size_t *length)
{
    const size_t n = strlen(key);
    for (const char *p = body; *p;) {
        const char *end = p + strcspn(p, "\r\n");
        if ((size_t)(end - p) >= n + 2 && !memcmp(p, key, n) &&
            ((p[n] == ':' || p[n] == '=') && p[n + 1] == '=')) {
            const char *value = p + n + 2;
            while (end > value && end[-1] == ' ') --end;
            *length = (size_t)(end - value);
            return value;
        }
        p = end + strspn(end, "\r\n");
    }
    return NULL;
}

bool xc_value(const char *body, const char *key, char *out, size_t size)
{
    size_t len = 0;
    const char *p = find_value(body, key, &len);
    if (!size) return false;
    if (!p || !len || len >= size) {
        out[0] = 0;
        return false;
    }
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static bool is_value(const char *body, const char *key, const char *value)
{
    size_t n = 0;
    const char *p = find_value(body, key, &n);
    return p && n == strlen(value) && !memcmp(p, value, n);
}

static int number(const char *p, size_t length)
{
    if (!p || !length || length > 7) return 0;
    int value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (p[i] < '0' || p[i] > '9') return 0;
        value = value * 10 + (p[i] - '0');
    }
    return value;
}

static int read_number(const char *body, const char *key)
{
    size_t n = 0;
    const char *p = find_value(body, key, &n);
    return number(p, n);
}

static int read_minutes(const char *body, const char *key)
{
    char minutes[5];
    if (!xc_value(body, key, minutes, sizeof(minutes)) ||
        strspn(minutes, "0123456789") != strlen(minutes)) return -1;
    return number(minutes, strlen(minutes));
}

static bool read_resolution(const char *body, const char *key, char out[16])
{
    size_t n = 0;
    const char *p = find_value(body, key, &n);
    const char *x = p ? memchr(p, 'x', n) : NULL;
    if (!x) return false;
    const size_t rest = n - (size_t)(x + 1 - p);
    const char *colon = memchr(x + 1, ':', rest);
    const int w = number(p, x - p);
    const int h = number(x + 1, colon ? (size_t)(colon - x - 1) : rest);
    if (w <= 0 || h <= 0 || w > 9999 || h > 9999) return false;
    snprintf(out, 16, "%dx%d", w, h);
    return true;
}

bool xc_is_identity(const char *body, char *model, size_t size)
{
    char version[24];
    return xc_value(body, "s.protocol", version, sizeof(version)) &&
           version[0] >= '0' && version[0] <= '9' &&
           xc_value(body, "s.hardware", model, size);
}

bool xc_camera_matches(const xc_camera_t *camera, const char *model, const char *address)
{
    return !strcmp(model, camera->model) && (!address[0] || !strcasecmp(address, camera->address));
}

static void neighbor(xc_setting_t *s, int v)
{
    if (v && v < s->value && v > s->lower) s->lower = v;
    if (v > s->value && (!s->higher || v < s->higher)) s->higher = v;
}

static void setting(xc_setting_t *s, const char *body, const char *key, const char *list_key, bool writable)
{
    s->key = key;
    s->value = read_number(body, key);
    s->lower = s->higher = s->count = 0;
    s->writable = s->partial = false;
    strcpy(s->display, "--");
    char list[48];
    snprintf(list, sizeof(list), "%s.list", key);
    size_t n = 0;
    const char *p = find_value(body, list_key ? list_key : list, &n);
    if (!p || !s->value || !writable) return;
    const char *end = p + n;
    for (; p < end;) {
        const char *comma = memchr(p, ',', (size_t)(end - p));
        if (!comma) comma = end;
        const int v = number(p, (size_t)(comma - p));
        if (v && s->count < XC_SETTING_STEPS) s->steps[s->count++] = v;
        else if (v) s->partial = true;
        neighbor(s, v);
        p = comma == end ? end : comma + 1;
    }
    s->writable = s->lower || s->higher;
}

xc_tally_t xc_parse_tally(const char *body)
{
    if (is_value(body, "f.tally", "off")) return XC_OFF;
    if (!is_value(body, "f.tally", "on")) return XC_UNKNOWN;
    if (is_value(body, "f.tally.mode", "preview")) return XC_PREVIEW;
    if (is_value(body, "f.tally.mode", "program")) return XC_PROGRAM;
    if (is_value(body, "f.tally.mode", "preview_program")) return XC_BOTH;
    return XC_UNKNOWN;
}

static void decimal(char *out, size_t size, int value)
{
    if (value % 10) snprintf(out, size, "%d.%02d", value / 100, value % 100);
    else snprintf(out, size, "%d.%d", value / 100, value / 10 % 10);
}

static void format_iris(xc_setting_t *s, int value)
{
    if (value == 65535) strcpy(s->display, "CLOSE");
    else if (value > 0) {
        s->display[0] = !strcmp(s->key, "c.1.me.diaphragm.t") ? 'T' : 'F';
        decimal(s->display + 1, sizeof(s->display) - 1, value);
    }
}

static void format_shutter(xc_setting_t *s)
{
    if (!s->value) return;
    if (!strcmp(s->key, "c.1.me.angle") || !strcmp(s->key, "c.1.me.clearscan")) {
        decimal(s->display, sizeof(s->display) - 3, s->value);
        strcat(s->display, !strcmp(s->key, "c.1.me.angle") ? "°" : "Hz");
    } else snprintf(s->display, sizeof(s->display), "1/%d", s->value);
}

void xc_setting_confirm(xc_setting_t *s, int value)
{
    s->value = value;
    s->lower = s->higher = 0;
    // An oversized list still gave us the exact first step. Refresh it before the next one.
    if (!s->partial)
        for (unsigned i = 0; i < s->count; ++i) neighbor(s, s->steps[i]);
    if (!strcmp(s->key, "c.1.wb.kelvin")) snprintf(s->display, sizeof(s->display), "%d K", value);
    else if (!strncmp(s->key, "c.1.me.diaphragm", sizeof("c.1.me.diaphragm") - 1)) format_iris(s, value);
    else format_shutter(s);
}

bool xc_edit_step(xc_edit_t *edit, xc_setting_t *shown, bool up, int64_t now)
{
    const int next = up ? shown->lower : shown->higher;
    if (!shown->writable || !next) return false;
    xc_setting_confirm(shown, next);
    ++edit->revision;
    edit->phase = XC_EDIT_QUEUED;
    edit->at = now;
    return true;
}

bool xc_edit_due(const xc_edit_t *edit, int64_t now)
{
    return edit->phase == XC_EDIT_QUEUED && now >= edit->at;
}

bool xc_edit_reply(xc_edit_t *edit, unsigned revision, bool accepted, int64_t now)
{
    if (edit->phase != XC_EDIT_QUEUED || edit->revision != revision) return false;
    edit->phase = XC_EDIT_VERIFY;
    edit->at = now + (accepted ? 900000 : 0);
    return true;
}

bool xc_edit_merge(xc_edit_t *edit, const xc_setting_t *shown, xc_setting_t *actual, int64_t now)
{
    if (edit->phase == XC_EDIT_NONE) return false;
    if (!actual->writable || !actual->key || strcmp(shown->key, actual->key)) {
        edit->phase = XC_EDIT_NONE;
        return true;
    }
    if (edit->phase == XC_EDIT_VERIFY) {
        if (actual->value == shown->value) {
            edit->phase = XC_EDIT_NONE;
            return false;
        }
        if (now >= edit->at) {
            edit->phase = XC_EDIT_NONE;
            return true;
        }
    }
    *actual = *shown;
    return false;
}

bool xc_parse(const char *body, xc_camera_t *camera)
{
    memset(camera, 0, sizeof(*camera));
    if (!xc_is_identity(body, camera->model, sizeof(camera->model))) return false;
    xc_value(body, "s.hardware.address", camera->address, sizeof(camera->address));
    xc_value(body, "s.firmware", camera->firmware, sizeof(camera->firmware));
    const int main_card = read_number(body, "f.rec.media.main");
    const int card_a = read_minutes(body, "f.rec.media1.remainingtime");
    const int card_b = read_minutes(body, "f.rec.media2.remainingtime");
    if (main_card == 1) camera->record_minutes = card_a;
    else if (main_card == 2) camera->record_minutes = card_b;
    else if (card_a < 0) camera->record_minutes = card_b;
    else if (card_b < 0 || card_a == card_b) camera->record_minutes = card_a;
    else camera->record_minutes = -1;  // Both cards report different values, and neither is identified as main.
    if (main_card == 2) {
        read_resolution(body, "f.rec.media2.videoinfo", camera->resolution);
    } else if (!read_resolution(body, "f.rec.media1.videoinfo", camera->resolution) && main_card != 1) {
        read_resolution(body, "f.rec.media2.videoinfo", camera->resolution);
    }
    camera->battery_minutes = -1;
    if (is_value(body, "s.power.source", "dc")) camera->power = XC_POWER_DC;
    else if (is_value(body, "s.power.source", "checking")) camera->power = XC_POWER_CHECKING;
    else if (is_value(body, "s.power.source", "battery")) {
        camera->power = XC_POWER_BATTERY;
        camera->battery_minutes = read_minutes(body, "s.power.minute");
    }
    const int decivolts = read_number(body, "s.power.volt");
    if (decivolts > 0 && decivolts <= 1000)
        snprintf(camera->voltage, sizeof(camera->voltage), "%d.%d V", decivolts / 10, decivolts % 10);
    camera->tally = xc_parse_tally(body);
    if (is_value(body, "f.rec.status", "rec")) camera->recording = XC_REC_ACTIVE;
    else if (is_value(body, "f.rec.status", "idle")) camera->recording = XC_REC_IDLE;
    else if (is_value(body, XC_RECORD_KEY, "on")) camera->recording = XC_REC_ACTIVE;
    else if (is_value(body, XC_RECORD_KEY, "off")) camera->recording = XC_REC_IDLE;
    camera->standby = is_value(body, "f.standby", "standby");
    if (is_value(body, "c.1.me.isogain.mode", "gain") ||
        !xc_value(body, "c.1.me.iso", camera->iso, sizeof(camera->iso))) {
        char gain[12];
        if (xc_value(body, "c.1.me.gain", gain, sizeof(gain))) {
            char *end;
            long v = strtol(gain, &end, 10);
            if (!*end && v >= -1000 && v <= 1000)
                snprintf(camera->iso, sizeof(camera->iso), "%s%ld.%ld dB",
                         v < 0 ? "-" : "", labs(v) / 10, labs(v) % 10);
        }
    }
    const bool full_auto = is_value(body, "c.1.shooting", "fullauto");
    const bool exp_auto = is_value(body, "c.1.exp", "auto");
    const bool iris_auto = is_value(body, "c.1.me.diaphragm.mode", "auto");
    const bool t = is_value(body, "c.1.me.diaphragm.ft", "t");
    xc_setting_t *iris = &camera->settings[XC_IRIS];
    setting(iris, body, t ? "c.1.me.diaphragm.t" : "c.1.me.diaphragm", NULL,
                    !full_auto && !exp_auto && !iris_auto && !camera->standby);
    int f = iris->value;
    if (!t && (iris_auto || exp_auto || full_auto || !f)) {
        int measured = read_number(body, "c.1.me.fno");
        if (measured) f = measured;
    }
    format_iris(iris, f);

    xc_setting_t *shutter = &camera->settings[XC_SHUTTER];
    const char *mode = "c.1.me.shutter.mode";
    if (is_value(body, mode, "angle")) {
        setting(shutter, body, "c.1.me.angle", NULL, !full_auto && !exp_auto && !camera->standby);
        if (!shutter->writable)
            setting(shutter, body, "c.1.me.angle", "c.1.me.shutter.angle.list",
                               !full_auto && !exp_auto && !camera->standby);
        format_shutter(shutter);
    } else if (is_value(body, mode, "clearscan")) {
        setting(shutter, body, "c.1.me.clearscan", NULL, false);
        format_shutter(shutter);
    } else {
        setting(shutter, body, "c.1.me.shutter", NULL,
                           !full_auto && !exp_auto && !camera->standby &&
                           (is_value(body, mode, "speed") || is_value(body, mode, "slow") ||
                            !find_value(body, mode, &(size_t){0})));
        if (is_value(body, mode, "off")) strcpy(shutter->display, "OFF");
        else format_shutter(shutter);
    }

    xc_setting_t *kelvin = &camera->settings[XC_KELVIN];
    setting(kelvin, body, "c.1.wb.kelvin", NULL,
                      !full_auto && is_value(body, "c.1.wb", "kelvin") && !camera->standby);
    if (is_value(body, "c.1.wb", "auto")) strcpy(kelvin->display, "AWB");
    else if (kelvin->value && is_value(body, "c.1.wb", "kelvin"))
        snprintf(kelvin->display, sizeof(kelvin->display), "%d K", kelvin->value);
    return true;
}

bool xc_write_applied(const char *body, const char *key, int value)
{
    return value > 0 && read_number(body, key) == value;
}

xc_record_t xc_record_target(const xc_camera_t *camera)
{
    if (camera->recording == XC_REC_ACTIVE) return XC_REC_IDLE;
    if (!camera->standby && camera->recording == XC_REC_IDLE) return XC_REC_ACTIVE;
    return XC_REC_UNKNOWN;
}

const char *xc_record_value(xc_record_t target)
{
    return target == XC_REC_ACTIVE ? "on" : target == XC_REC_IDLE ? "off" : NULL;
}

bool xc_record_acknowledged(const char *body, xc_record_t target)
{
    const char *value = xc_record_value(target);
    return value && is_value(body, XC_RECORD_KEY, value);
}

unsigned xc_next_preset(unsigned mask, unsigned current)
{
    for (unsigned i = 1; i <= XC_FIELDS; ++i) {
        unsigned next = (current + i) % XC_FIELDS;
        if (mask & (1u << next)) return next;
    }
    return XC_IRIS;
}

int xc_discovery_port(const char *reply)
{
    if (strncmp(reply, "HTTPU 200 OK\r\n", 14)) return 0;
    for (const char *p = reply; *p;) {
        size_t len = strcspn(p, "\r\n");
        if (len > 6 && !strncasecmp(p, "PORT: ", 6)) {
            const int port = number(p + 6, len - 6);
            return port > 0 && port <= UINT16_MAX ? port : 0;
        }
        p += len;
        p += strspn(p, "\r\n");
    }
    return 0;
}

static unsigned dns_u16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static bool dns_name(const uint8_t *p, size_t size, size_t *cursor, char name[256])
{
    size_t at = *cursor, length = 0;
    bool jumped = false;
    while (at < size) {
        const unsigned n = p[at++];
        if (!n) {
            if (!jumped) *cursor = at;
            name[length] = 0;
            return true;
        }
        if ((n & 0xc0) == 0xc0) {
            if (at >= size) return false;
            const size_t offset = (size_t)(n & 0x3f) << 8 | p[at++];
            if (offset >= at - 2) return false;  // DNS compression only refers backward.
            if (!jumped) *cursor = at;
            jumped = true;
            at = offset;
            continue;
        }
        if (n > 63 || n > size - at || length + n + 1 >= 256) return false;
        if (length) name[length++] = '.';
        for (unsigned i = 0; i < n; ++i) {
            const unsigned char ch = p[at++];
            name[length++] = ch <= ' ' || ch == '.' ? '?' : (char)ch;
        }
    }
    return false;
}

int xc_mdns_port(const uint8_t *packet, size_t size)
{
    if (size < 12 || (dns_u16(packet + 2) & 0xf80f) != 0x8000) return 0;
    const unsigned questions = dns_u16(packet + 4);
    const unsigned records = dns_u16(packet + 6) + dns_u16(packet + 8) + dns_u16(packet + 10);
    size_t cursor = 12;
    char name[256];
    for (unsigned i = 0; i < questions; ++i) {
        if (!dns_name(packet, size, &cursor, name) || size - cursor < 4) return 0;
        cursor += 4;
    }
    for (unsigned i = 0; i < records; ++i) {
        if (!dns_name(packet, size, &cursor, name) || size - cursor < 10) return 0;
        const unsigned type = dns_u16(packet + cursor), klass = dns_u16(packet + cursor + 2);
        const unsigned length = dns_u16(packet + cursor + 8);
        cursor += 10;
        if (length > size - cursor) return 0;
        const size_t n = strlen(name);
        const char service[] = "._http._tcp.local";
        if (type == 33 && (klass & 0x7fff) == 1 && length >= 7 && n > sizeof(service) - 1 &&
            !strcasecmp(name + n - (sizeof(service) - 1), service)) {
            size_t target = cursor + 6;
            if (!dns_name(packet, cursor + length, &target, name) || target != cursor + length) return 0;
            if (!strncasecmp(name, "canon-", 6) && strlen(name) == 24 && !strcasecmp(name + 18, ".local")) {
                unsigned i = 6;
                while (i < 18 && isxdigit((unsigned char)name[i])) ++i;
                if (i == 18) return (int)dns_u16(packet + cursor + 4);
            }
        }
        cursor += length;
    }
    return 0;
}
