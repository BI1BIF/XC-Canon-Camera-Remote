#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { XC_HINT_HOLD_MS = 4000, XC_HINT_BLINK_MS = 500, XC_HINT_ERASE_MS = 45, XC_HINT_GAP_MS = 90, XC_HINT_TYPE_MS = 65 };

typedef struct {
    unsigned line, chars;
    uint32_t wait_ms;
    bool cursor;
} xc_hint_frame_t;

static inline uint32_t xc_hint_erase_ms(unsigned chars)
{
    return chars > 1 ? (chars - 1) * XC_HINT_ERASE_MS : 0;
}

static inline uint32_t xc_hint_span(unsigned from, unsigned to)
{
    return XC_HINT_HOLD_MS + xc_hint_erase_ms(from) + XC_HINT_GAP_MS + (to > 1 ? (to - 1) * XC_HINT_TYPE_MS : 0);
}

static inline xc_hint_frame_t xc_hint_frame(uint32_t elapsed, unsigned first, unsigned second)
{
    const uint32_t first_span = xc_hint_span(first, second);
    uint32_t t = elapsed % (first_span + xc_hint_span(second, first));
    const unsigned line = t >= first_span;
    const unsigned from = line ? second : first;
    if (line) t -= first_span;
    if (t < XC_HINT_HOLD_MS) {
        const uint32_t blink = XC_HINT_BLINK_MS - t % XC_HINT_BLINK_MS;
        const uint32_t left = XC_HINT_HOLD_MS - t;
        return (xc_hint_frame_t){line, from, blink < left ? blink : left, t / XC_HINT_BLINK_MS % 2 == 0};
    }
    t -= XC_HINT_HOLD_MS;
    const uint32_t erase = xc_hint_erase_ms(from);
    if (t < erase) return (xc_hint_frame_t){line, from - 1 - t / XC_HINT_ERASE_MS, XC_HINT_ERASE_MS - t % XC_HINT_ERASE_MS, true};
    t -= erase;
    if (t < XC_HINT_GAP_MS) return (xc_hint_frame_t){line ^ 1U, 0, XC_HINT_GAP_MS - t, true};
    t -= XC_HINT_GAP_MS;
    return (xc_hint_frame_t){line ^ 1U, 1 + t / XC_HINT_TYPE_MS, XC_HINT_TYPE_MS - t % XC_HINT_TYPE_MS, true};
}
