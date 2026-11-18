#pragma once

#include <stdbool.h>

typedef enum {
    LAUNCHER, HOME, SCAN, LIST, LINK, LIVE, SETTINGS, PRESETS,
    STATUS, SETUP, FORGET, DEVICE, BLE_REMOTE,
} xc_page_t;

typedef struct {
    xc_page_t page;
    unsigned focus;
} xc_location_t;

typedef enum { XC_OK_SINGLE, XC_OK_DOUBLE, XC_OK_LONG } xc_ok_t;

typedef enum {
    XC_NAV_IGNORE, XC_NAV_ACTIVATE, XC_NAV_SETTINGS,
    XC_NAV_LAUNCHER, XC_NAV_BACK, XC_NAV_RECORD,
} xc_nav_action_t;

typedef enum {
    XC_MENU_RETURN, XC_MENU_MODE, XC_MENU_DEVICE, XC_MENU_PRESETS,
    XC_MENU_STATUS, XC_MENU_CAMERA, XC_MENU_DISCONNECT,
    XC_MENU_WIFI, XC_MENU_FORGET,
} xc_menu_item_t;

xc_nav_action_t xc_navigation_ok(xc_page_t page, xc_ok_t gesture);
const xc_menu_item_t *xc_settings_menu(bool bluetooth, bool has_camera, unsigned *count);
