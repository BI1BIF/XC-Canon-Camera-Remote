#include "xc_hint.h"
#include "xc_portal.h"
#include "xc_screen.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool write_page(void *file, const char *text)
{
    return fputs(text, file) >= 0;
}

static void assert_page(const xc_portal_t *page, const char *present, const char *absent)
{
    FILE *html = tmpfile();
    assert(html && xc_portal_render(page, write_page, html));
    rewind(html);
    char text[8192];
    text[fread(text, 1, sizeof(text) - 1, html)] = 0;
    assert(strstr(text, present) && !strstr(text, absent));
    assert(fclose(html) == 0);
}

static void test_hint_cycle(void)
{
    const uint32_t erase_end = XC_HINT_HOLD_MS + 12 * XC_HINT_ERASE_MS;
    const uint32_t switch_at = erase_end + XC_HINT_GAP_MS + 8 * XC_HINT_TYPE_MS;
    assert(xc_hint_span(13, 9) == switch_at);
    xc_hint_frame_t frame = xc_hint_frame(XC_HINT_HOLD_MS - 1, 13, 9);
    assert(frame.line == 0 && frame.chars == 13 && frame.wait_ms == 1 && !frame.cursor);
    frame = xc_hint_frame(XC_HINT_HOLD_MS, 13, 9);
    assert(frame.line == 0 && frame.chars == 12 && frame.wait_ms == XC_HINT_ERASE_MS && frame.cursor);
    frame = xc_hint_frame(erase_end, 13, 9);
    assert(frame.line == 1 && frame.chars == 0 && frame.wait_ms == XC_HINT_GAP_MS && frame.cursor);
    frame = xc_hint_frame(erase_end + XC_HINT_GAP_MS, 13, 9);
    assert(frame.line == 1 && frame.chars == 1);
    frame = xc_hint_frame(switch_at, 13, 9);
    assert(frame.line == 1 && frame.chars == 9 && frame.wait_ms == XC_HINT_BLINK_MS && frame.cursor);
    frame = xc_hint_frame(switch_at + XC_HINT_BLINK_MS, 13, 9);
    assert(frame.line == 1 && frame.chars == 9 && !frame.cursor);
    frame = xc_hint_frame(switch_at + XC_HINT_HOLD_MS - 1, 13, 9);
    assert(frame.line == 1 && frame.chars == 9);
    frame = xc_hint_frame(switch_at + XC_HINT_HOLD_MS, 13, 9);
    assert(frame.line == 1 && frame.chars == 8 && frame.cursor);
    frame = xc_hint_frame(switch_at + xc_hint_span(9, 13), 13, 9);
    assert(frame.line == 0 && frame.chars == 13 && frame.wait_ms == XC_HINT_BLINK_MS && frame.cursor);
}

int main(int argc, char **argv)
{
    test_hint_cycle();
    char name[] = "  一号机 A  ";
    assert(xc_title_normalize(name) && !strcmp(name, "一号机 A"));
    char blank[] = "   ", unsafe[] = "<b>", malformed[] = "\xe5\xb1", emoji[] = "\xf0\x9f\x8e\xa5";
    char too_long[] = "abcdefghijklM", maximum[] = "abcdefghijkl";
    assert(xc_title_normalize(blank) && !blank[0]);
    assert(!xc_title_normalize(unsafe) && !xc_title_normalize(malformed) && !xc_title_normalize(emoji));
    assert(!xc_title_normalize(too_long) && xc_title_normalize(maximum));

    xc_screen_t screen = {.activity_us = 1000};
    assert(xc_screen_level(&screen, 30000999, false) == 65);
    assert(xc_screen_level(&screen, 30001000, false) == 15);
    assert(xc_screen_level(&screen, 90000999, false) == 15);
    assert(xc_screen_level(&screen, 90001000, false) == 0);
    assert(xc_screen_press(&screen, 90001000, false));
    assert(xc_screen_level(&screen, 90001000, false) == 65);
    assert(!xc_screen_press(&screen, 120001000, false));
    assert(xc_screen_level(&screen, 120001000, false) == 65);
    assert(xc_screen_level(&screen, 220001000, false) == 0);
    assert(xc_screen_level(&screen, 220001000, true) == 65);
    assert(xc_screen_press(&screen, 220001000, false));

    assert(xc_portal_host_matches("192.168.1.5:80", "192.168.1.5"));
    assert(!xc_portal_host_matches("192.168.1.55", "192.168.1.5"));
    assert(!xc_portal_host_matches("192.168.1.5.example.com", "192.168.1.5"));
    assert(!xc_portal_host_matches("192.168.1.5:81", "192.168.1.5"));
    assert(xc_portal_lan_route(XC_SETUP_HOME, "/camera") == XC_SETUP_HOME);
    assert(xc_portal_lan_route(XC_SETUP_HOME, "/display") == XC_SETUP_DISPLAY);
    assert(xc_portal_lan_route(XC_SETUP_CAMERA, "/") == XC_SETUP_DISPLAY);
    assert(xc_portal_lan_route(XC_SETUP_CAMERA, "/camera") == XC_SETUP_CAMERA);
    assert(xc_portal_lan_route(XC_SETUP_CAMERA, "/display") == XC_SETUP_DISPLAY);
    assert(xc_portal_lan_route(XC_SETUP_HOME, "/") == XC_SETUP_DISPLAY);

    xc_portal_t web = {.mode = XC_SETUP_HOME, .lan = true, .nonce = "preview", .ip = "", .title = "", .port = 80};
    assert_page(&web, "打开相机设置", "<form");
    web.mode = xc_portal_lan_route(XC_SETUP_HOME, "/");
    assert_page(&web, "保存标题", "设备屏幕上的验证码");
    assert_page(&web, "name=k value='preview'", "id=a name=a");
    web.submitted = true;
    assert_page(&web, "标题已保存", "退出设置");
    web.submitted = false;
    web.mode = XC_SETUP_CAMERA;
    assert_page(&web, "action='/camera'", "设置热点将关闭");
    assert_page(&web, "设备屏幕上的验证码", "Wi-Fi 名称（SSID）");
    web.submitted = true;
    assert_page(&web, "手机可以留在当前 Wi-Fi", "设置热点将关闭");
    web.mode = XC_SETUP_WIFI;
    web.lan = web.submitted = false;
    assert_page(&web, "Wi-Fi 名称（SSID）", "设备屏幕上的验证码");

    if (argc == 3) {
        const bool lan = !strncmp(argv[1], "lan-", 4);
        const xc_portal_t p = {.mode = lan ? XC_SETUP_DISPLAY : !strcmp(argv[1], "camera") ? XC_SETUP_CAMERA : XC_SETUP_WIFI,
                               .lan = lan,
                               .display = !strcmp(argv[1], "display"),
                               .submitted = !strcmp(argv[1], "success") || !strcmp(argv[1], "lan-saved"),
                               .nonce = "preview", .ip = "", .title = "一号机 A", .port = 80};
        FILE *out = fopen(argv[2], "wb");
        assert(out && xc_portal_render(&p, write_page, out));
        assert(fclose(out) == 0);
    } else {
        puts("XC portal routes, title validation and backlight timing tests: PASS");
    }
    return 0;
}
