#pragma once

#include <stdbool.h>

typedef enum { XC_SETUP_HOME, XC_SETUP_WIFI, XC_SETUP_CAMERA, XC_SETUP_DISPLAY } xc_setup_mode_t;

#define XC_TITLE_DEFAULT "XC REMOTE"
#define XC_TITLE_SIZE 37

typedef struct {
    xc_setup_mode_t mode;
    bool display, submitted, lan;
    const char *nonce, *ip, *title, *error;
    unsigned port;
} xc_portal_t;

// Dynamic strings come from a restricted title alphabet, IPv4/port, a nonce, or fixed messages.
bool xc_portal_render(const xc_portal_t *page, bool (*send)(void *, const char *), void *ctx);
bool xc_title_normalize(char *title);
xc_setup_mode_t xc_portal_lan_route(xc_setup_mode_t active, const char *uri);
bool xc_portal_host_matches(const char *host, const char *ip);
