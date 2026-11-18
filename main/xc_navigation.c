#include "xc_navigation.h"

xc_nav_action_t xc_navigation_ok(xc_page_t page, xc_ok_t gesture)
{
    if (gesture == XC_OK_DOUBLE)
        return page == HOME || page == LIVE || page == BLE_REMOTE ? XC_NAV_SETTINGS : XC_NAV_IGNORE;

    if (gesture == XC_OK_LONG) {
        switch (page) {
        case HOME:
        case SETTINGS:
        case BLE_REMOTE: return XC_NAV_LAUNCHER;
        case LIVE: return XC_NAV_RECORD;
        case LIST:
        case PRESETS:
        case STATUS:
        case SETUP:
        case FORGET:
        case DEVICE: return XC_NAV_BACK;
        default: return XC_NAV_IGNORE;
        }
    }

    if (gesture == XC_OK_SINGLE) {
        switch (page) {
        case LAUNCHER:
        case HOME:
        case LIST:
        case LIVE:
        case SETTINGS:
        case PRESETS:
        case STATUS:
        case FORGET:
        case DEVICE:
        case BLE_REMOTE: return XC_NAV_ACTIVATE;
        default: return XC_NAV_IGNORE;
        }
    }
    return XC_NAV_IGNORE;
}

const xc_menu_item_t *xc_settings_menu(bool bluetooth, bool has_camera, unsigned *count)
{
    static const xc_menu_item_t ble[] = {
        XC_MENU_RETURN, XC_MENU_MODE, XC_MENU_DEVICE,
    };
    static const xc_menu_item_t network[] = {
        XC_MENU_RETURN, XC_MENU_MODE, XC_MENU_DEVICE, XC_MENU_WIFI, XC_MENU_FORGET,
    };
    static const xc_menu_item_t camera[] = {
        XC_MENU_RETURN, XC_MENU_MODE, XC_MENU_DEVICE, XC_MENU_PRESETS,
        XC_MENU_STATUS, XC_MENU_CAMERA, XC_MENU_DISCONNECT, XC_MENU_WIFI, XC_MENU_FORGET,
    };
    if (bluetooth) {
        *count = sizeof(ble) / sizeof(ble[0]);
        return ble;
    }
    if (has_camera) {
        *count = sizeof(camera) / sizeof(camera[0]);
        return camera;
    }
    *count = sizeof(network) / sizeof(network[0]);
    return network;
}
