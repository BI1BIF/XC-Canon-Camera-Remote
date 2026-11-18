#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { XC_SCREEN_NORMAL = 65, XC_SCREEN_DIM = 15 };

typedef struct {
    int64_t activity_us;
} xc_screen_t;

static inline unsigned xc_screen_level(const xc_screen_t *s, int64_t now, bool setup)
{
    if (setup) return XC_SCREEN_NORMAL;
    if (now - s->activity_us >= 90000000) return 0;
    return now - s->activity_us >= 30000000 ? XC_SCREEN_DIM : XC_SCREEN_NORMAL;
}

static inline bool xc_screen_press(xc_screen_t *s, int64_t now, bool setup)
{
    const bool wake_only = xc_screen_level(s, now, setup) == 0;
    *s = (xc_screen_t){.activity_us = now};
    return wake_only;
}
