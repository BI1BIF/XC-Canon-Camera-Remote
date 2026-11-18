#include "xc_navigation.h"

#include <assert.h>
#include <stdio.h>

static void test_ok_policy(void)
{
    static const struct {
        xc_page_t page;
        xc_nav_action_t single, double_click, long_press;
    } expected[] = {
        {LAUNCHER, XC_NAV_ACTIVATE, XC_NAV_IGNORE, XC_NAV_IGNORE},
        {HOME, XC_NAV_ACTIVATE, XC_NAV_SETTINGS, XC_NAV_LAUNCHER},
        {SCAN, XC_NAV_IGNORE, XC_NAV_IGNORE, XC_NAV_IGNORE},
        {LIST, XC_NAV_ACTIVATE, XC_NAV_IGNORE, XC_NAV_BACK},
        {LINK, XC_NAV_IGNORE, XC_NAV_IGNORE, XC_NAV_IGNORE},
        {LIVE, XC_NAV_ACTIVATE, XC_NAV_SETTINGS, XC_NAV_RECORD},
        {SETTINGS, XC_NAV_ACTIVATE, XC_NAV_IGNORE, XC_NAV_LAUNCHER},
        {PRESETS, XC_NAV_ACTIVATE, XC_NAV_IGNORE, XC_NAV_BACK},
        {STATUS, XC_NAV_ACTIVATE, XC_NAV_IGNORE, XC_NAV_BACK},
        {SETUP, XC_NAV_IGNORE, XC_NAV_IGNORE, XC_NAV_BACK},
        {FORGET, XC_NAV_ACTIVATE, XC_NAV_IGNORE, XC_NAV_BACK},
        {DEVICE, XC_NAV_ACTIVATE, XC_NAV_IGNORE, XC_NAV_BACK},
        {BLE_REMOTE, XC_NAV_ACTIVATE, XC_NAV_SETTINGS, XC_NAV_LAUNCHER},
    };
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        assert(xc_navigation_ok(expected[i].page, XC_OK_SINGLE) == expected[i].single);
        assert(xc_navigation_ok(expected[i].page, XC_OK_DOUBLE) == expected[i].double_click);
        assert(xc_navigation_ok(expected[i].page, XC_OK_LONG) == expected[i].long_press);
    }
    assert(xc_navigation_ok(FORGET, XC_OK_DOUBLE) != XC_NAV_ACTIVATE);
    assert(xc_navigation_ok(LAUNCHER, XC_OK_LONG) != XC_NAV_ACTIVATE);
    assert(xc_navigation_ok(LIVE, (xc_ok_t)99) == XC_NAV_IGNORE);
    assert(xc_navigation_ok((xc_page_t)99, XC_OK_SINGLE) == XC_NAV_IGNORE);
}

static void assert_menu(bool bluetooth, bool has_camera,
                        const xc_menu_item_t *expected, unsigned expected_count)
{
    unsigned count;
    const xc_menu_item_t *menu = xc_settings_menu(bluetooth, has_camera, &count);
    assert(count == expected_count);
    for (unsigned i = 0; i < count; ++i) {
        assert(menu[i] == expected[i]);
        for (unsigned j = 0; j < i; ++j) assert(menu[i] != menu[j]);
    }
}

static void test_mode_specific_settings(void)
{
    static const xc_menu_item_t ble[] = {XC_MENU_RETURN, XC_MENU_MODE, XC_MENU_DEVICE};
    static const xc_menu_item_t network[] = {
        XC_MENU_RETURN, XC_MENU_MODE, XC_MENU_DEVICE, XC_MENU_WIFI, XC_MENU_FORGET,
    };
    static const xc_menu_item_t camera[] = {
        XC_MENU_RETURN, XC_MENU_MODE, XC_MENU_DEVICE, XC_MENU_PRESETS,
        XC_MENU_STATUS, XC_MENU_CAMERA, XC_MENU_DISCONNECT, XC_MENU_WIFI, XC_MENU_FORGET,
    };
    assert_menu(false, false, network, sizeof(network) / sizeof(network[0]));
    assert_menu(false, true, camera, sizeof(camera) / sizeof(camera[0]));
    assert_menu(true, true, ble, sizeof(ble) / sizeof(ble[0]));
    assert_menu(true, false, ble, sizeof(ble) / sizeof(ble[0]));
    assert_menu(false, true, camera, sizeof(camera) / sizeof(camera[0]));
}

int main(void)
{
    test_ok_policy();
    test_mode_specific_settings();
    puts("XC navigation gestures and mode-specific settings: PASS");
    return 0;
}
