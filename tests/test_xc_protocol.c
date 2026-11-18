#include "xc_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CAMERA "timestamp=26.4\r\ns.hardware:=Canon CR-N500\r\ns.protocol:=7.0.0\r\n"
#define IRIS "c.1.me.diaphragm:=400\r\nc.1.me.diaphragm.list:=560,320,400,450,280,440\r\n"
#define SHUTTER "c.1.me.shutter:=125\r\nc.1.me.shutter.list:=100,125,150\r\n"
#define KELVIN "c.1.wb.kelvin:=5600\r\nc.1.wb.kelvin.list:=5500,5600,5700\r\n"

static void test_mdns(void)
{
    // C80-style reply: echoed question, compressed owner name, and SRV target.
    static const uint8_t reply[] =
        "\x00\x00\x84\x00\x00\x01\x00\x02\x00\x00\x00\x00"
        "\x05_http\x04_tcp\x05local\x00\x00\x0c\x80\x01"
        "\xc0\x0c\x00\x0c\x00\x01\x00\x00\x00\x0a\x00\x15"
        "\x12" "canon-012345abcdef" "\xc0\x0c"
        "\xc0\x2e\x00\x21\x00\x01\x00\x00\x00\x0a\x00\x1b"
        "\x00\x00\x00\x00\x1f\x90\x12" "canon-012345abcdef" "\xc0\x17";
    assert(xc_mdns_port(reply, sizeof(reply) - 1) == 8080);
    for (size_t i = 0; i < sizeof(reply) - 1; ++i) assert(xc_mdns_port(reply, i) == 0);
    uint8_t changed[sizeof(reply)];
    memcpy(changed, reply, sizeof(changed));
    changed[sizeof(changed) - 2] = 0xff;  // Invalid compression pointer in the target.
    assert(!xc_mdns_port(changed, sizeof(changed) - 1));
    memcpy(changed, reply, sizeof(changed));
    changed[sizeof(changed) - 21] = 'x';  // Non-Canon host with an HTTP service.
    assert(!xc_mdns_port(changed, sizeof(changed) - 1));
}

static void test_fast_edits(void)
{
    xc_camera_t initial;
    assert(xc_parse(CAMERA IRIS, &initial));
    xc_setting_t shown = initial.settings[XC_IRIS];
    xc_edit_t edit = {0};
    assert(xc_edit_step(&edit, &shown, false, 10000));
    assert(shown.value == 440 && xc_edit_due(&edit, 10000));
    const unsigned first = edit.revision;
    assert(xc_edit_step(&edit, &shown, false, 30000));
    assert(xc_edit_step(&edit, &shown, false, 50000));
    assert(!xc_edit_step(&edit, &shown, false, 60000));
    assert(!xc_edit_reply(&edit, first, false, 70000));
    assert(xc_edit_due(&edit, 70000) && shown.value == 560);

    xc_setting_t actual = initial.settings[XC_IRIS];
    assert(!xc_edit_merge(&edit, &shown, &actual, 75000) && actual.value == 560);
    assert(xc_edit_reply(&edit, edit.revision, true, 90000));
    assert(!xc_edit_due(&edit, 90000));
    actual = initial.settings[XC_IRIS];
    xc_setting_confirm(&actual, 440);
    assert(!xc_edit_merge(&edit, &shown, &actual, 200000) && actual.value == 560);
    assert(!xc_edit_merge(&edit, &shown, &actual, 300000) && edit.phase == XC_EDIT_NONE);
    actual = initial.settings[XC_IRIS];
    assert(!xc_edit_merge(&edit, &shown, &actual, 400000) && actual.value == 400);

    assert(xc_edit_step(&edit, &shown, true, 410000) && shown.value == 450);
    assert(xc_edit_reply(&edit, edit.revision, true, 450000));
    assert(xc_edit_merge(&edit, &shown, &actual, 1350000) && actual.value == 400);
    assert(xc_edit_step(&edit, &shown, true, 1400000) && shown.value == 440);
    assert(xc_edit_reply(&edit, edit.revision, false, 1500000));
    assert(xc_edit_merge(&edit, &shown, &actual, 1510000) && actual.value == 400);

    assert(xc_parse(CAMERA IRIS SHUTTER KELVIN "c.1.wb:=kelvin\n", &initial));
    const int expected_up[] = {320, 100, 5500};
    const int start[] = {400, 125, 5600};
    for (unsigned i = 0; i < XC_FIELDS; ++i) {
        xc_edit_t direction = {0};
        xc_setting_t *setting = &initial.settings[i];
        assert(xc_edit_step(&direction, setting, true, 0) && setting->value == expected_up[i]);
        assert(xc_edit_step(&direction, setting, false, 0) && setting->value == start[i]);
    }
}

static void test_recording(void)
{
    xc_camera_t cam;
    assert(xc_parse(CAMERA "f.rec.status:=idle\nf.rec.media.main:=2\n"
                           "f.rec.media1.remainingtime:=42\nf.rec.media2.remainingtime:=0\n", &cam));
    assert(cam.recording == XC_REC_IDLE && cam.record_minutes == 0);
    assert(xc_record_target(&cam) == XC_REC_ACTIVE);
    assert(xc_parse(CAMERA "f.rec.status:=rec\nf.rec:=off\nf.rec.media.main:=1\n"
                           "f.rec.media1.remainingtime:=9999\nf.rec.media2.remainingtime:=17\n", &cam));
    assert(cam.recording == XC_REC_ACTIVE && cam.record_minutes == 9999);
    assert(xc_record_target(&cam) == XC_REC_IDLE);
    assert(xc_parse(CAMERA "f.rec:=on\nf.rec.media1.remainingtime:=?\nf.rec.media2.remainingtime:=17\n", &cam));
    assert(cam.recording == XC_REC_ACTIVE && cam.record_minutes == 17);
    assert(xc_parse(CAMERA "f.rec:=off\nf.standby:=standby\nf.rec.media1.remainingtime:=-1\n", &cam));
    assert(cam.recording == XC_REC_IDLE && cam.record_minutes == -1 && xc_record_target(&cam) == XC_REC_UNKNOWN);
    assert(xc_parse(CAMERA "f.rec.status:=?\nf.rec.media1.remainingtime:=15\nf.rec.media2.remainingtime:=20\n", &cam));
    assert(cam.recording == XC_REC_UNKNOWN && cam.record_minutes == -1 && xc_record_target(&cam) == XC_REC_UNKNOWN);
    assert(!strcmp(xc_record_value(XC_REC_ACTIVE), "on"));
    assert(!strcmp(xc_record_value(XC_REC_IDLE), "off"));
    assert(!xc_record_value(XC_REC_UNKNOWN));
    assert(xc_record_acknowledged("f.rec:=on\n", XC_REC_ACTIVE));
    assert(xc_record_acknowledged("f.rec==off\n", XC_REC_IDLE));
    assert(!xc_record_acknowledged("f.rec==?\n", XC_REC_ACTIVE));
    assert(!xc_record_acknowledged("f.rec:=on\n", XC_REC_IDLE));
}

int main(void)
{
    xc_camera_t camera;
    assert(xc_parse(CAMERA IRIS SHUTTER KELVIN
                    "s.firmware:=1.2.0\r\ns.hardware.address:=123456789abc\r\nf.tally:=on\r\nf.tally.mode:=preview\r\n"
                    "c.1.me.shutter.mode:=speed\r\nc.1.wb:=kelvin\r\nf.rec.status:=rec\r\n"
                    "c.1.me.iso:=800\r\nc.1.me.gain:=-5", &camera));
    assert(!strcmp(camera.model, "Canon CR-N500") && !strcmp(camera.firmware, "1.2.0"));
    assert(!strcmp(camera.address, "123456789abc"));
    assert(!camera.resolution[0] && !camera.voltage[0]);
    assert(xc_camera_matches(&camera, "Canon CR-N500", "123456789ABC"));
    assert(xc_camera_matches(&camera, "Canon CR-N500", ""));
    assert(!xc_camera_matches(&camera, "Canon CR-N500", "123456789abd"));
    assert(!xc_camera_matches(&camera, "Canon CR-N700", "123456789abc"));
    assert(camera.recording == XC_REC_ACTIVE && camera.tally == XC_PREVIEW && !strcmp(camera.iso, "800"));
    assert(!strcmp(camera.settings[XC_IRIS].display, "F4.0"));
    assert(camera.settings[XC_IRIS].lower == 320 && camera.settings[XC_IRIS].higher == 440);
    assert(!strcmp(camera.settings[XC_SHUTTER].display, "1/125"));
    assert(camera.settings[XC_SHUTTER].lower == 100 && camera.settings[XC_SHUTTER].higher == 150);
    assert(!strcmp(camera.settings[XC_KELVIN].display, "5600 K"));
    assert(camera.settings[XC_KELVIN].lower == 5500 && camera.settings[XC_KELVIN].higher == 5700);

    xc_setting_t *iris = &camera.settings[XC_IRIS];
    xc_setting_confirm(iris, iris->higher);
    assert(!strcmp(iris->display, "F4.4") && iris->lower == 400 && iris->higher == 450);
    xc_setting_confirm(iris, iris->higher);
    assert(!strcmp(iris->display, "F4.5") && iris->lower == 440 && iris->higher == 560);
    xc_setting_confirm(iris, iris->higher);
    assert(!iris->higher && iris->lower == 450);
    xc_setting_t *shutter = &camera.settings[XC_SHUTTER], *kelvin = &camera.settings[XC_KELVIN];
    xc_setting_confirm(shutter, shutter->lower);
    xc_setting_confirm(kelvin, kelvin->higher);
    assert(!strcmp(shutter->display, "1/100") && shutter->higher == 125);
    assert(!strcmp(kelvin->display, "5700 K") && kelvin->lower == 5600);
    assert(xc_parse_tally("f.tally:=on\nf.tally.mode:=preview\n") == XC_PREVIEW);
    assert(xc_parse_tally("f.tally:=on\nf.tally.mode:=program\n") == XC_PROGRAM);
    assert(xc_parse_tally("f.tally:=off\n") == XC_OFF);
    assert(xc_parse_tally("f.tally:=on\n") == XC_UNKNOWN);
    assert(xc_parse_tally("f.tally.mode:=program\n") == XC_UNKNOWN);

    assert(xc_parse(CAMERA IRIS SHUTTER KELVIN
                    "f.tally:=off\nf.tally.mode:=program\nc.1.shooting:=fullauto\n"
                    "c.1.me.fno:=360\nc.1.wb:=auto\nc.1.me.isogain.mode:=gain\n"
                    "c.1.me.iso:=800\nc.1.me.gain:=-5\n", &camera));
    assert(camera.tally == XC_OFF && camera.recording == XC_REC_UNKNOWN && !strcmp(camera.iso, "-0.5 dB"));
    assert(!strcmp(camera.settings[XC_IRIS].display, "F3.6"));
    assert(!strcmp(camera.settings[XC_KELVIN].display, "AWB"));
    for (unsigned i = 0; i < XC_FIELDS; ++i) assert(!camera.settings[i].writable);

    assert(xc_parse(CAMERA "f.tally:=on\nf.tally.mode:=preview_program\n"
                    "c.1.me.shutter.mode:=angle\nc.1.me.angle:=18000\n"
                    "c.1.me.shutter.angle.list:=9000,18000,36000\n", &camera));
    assert(camera.tally == XC_BOTH && camera.settings[XC_SHUTTER].higher == 36000);
    assert(!strcmp(camera.settings[XC_SHUTTER].display, "180.0°"));
    assert(xc_parse(CAMERA "f.tally:=on\nf.tally.mode:=program\nf.standby:=standby\n" IRIS, &camera));
    assert(camera.tally == XC_PROGRAM && camera.standby && !camera.settings[XC_IRIS].writable);
    assert(xc_parse(CAMERA "f.tally:=on\nf.tally.mode:=unknown\n", &camera));
    assert(camera.tally == XC_UNKNOWN);
    assert(!xc_parse("<html>Not a camera</html>", &camera));
    assert(!xc_parse("s.hardware:=Canon\ns.protocol.list:=7.0\n", &camera));
    assert(xc_write_applied("c.1.me.shutter:=125\n", "c.1.me.shutter", 125));
    assert(xc_write_applied("c.1.me.shutter==125\n", "c.1.me.shutter", 125));
    assert(!xc_write_applied("c.1.me.shutter==?\n", "c.1.me.shutter", 125));
    assert(!xc_write_applied("c.1.me.shutter:=125x\n", "c.1.me.shutter", 125));
    assert(xc_next_preset(5, XC_IRIS) == XC_KELVIN);
    assert(xc_next_preset(5, XC_KELVIN) == XC_IRIS);
    assert(xc_next_preset(2, XC_SHUTTER) == XC_SHUTTER);
    assert(xc_discovery_port("HTTPU 200 OK\r\nSERVER: CR-N500/1.2\r\nPORT: 8080\r\n") == 8080);
    assert(xc_discovery_port("HTTP/1.1 200 OK\r\nPORT: 80\r\n") == 0);
    assert(xc_discovery_port("HTTPU 200 OK\r\nPORT: 65536\r\n") == 0);
    assert(xc_parse("s.hardware:=Canon EOS C80\ns.protocol:=7.0.0\n"
                    "s.power.source:=dc\ns.power.volt:=244\ns.power.minute:=0\nf.rec.media.main:=2\n"
                    "f.rec.media1.videoinfo:=1920x1080:59940:P\nf.rec.media2.videoinfo:=3840x2160:29970:P\n", &camera));
    assert(!strcmp(camera.resolution, "3840x2160") && !strcmp(camera.voltage, "24.4 V"));
    assert(camera.power == XC_POWER_DC && camera.battery_minutes == -1);
    assert(xc_parse(CAMERA "s.power.source:=battery\ns.power.volt:=144\ns.power.minute:=97\n", &camera));
    assert(camera.power == XC_POWER_BATTERY && camera.battery_minutes == 97);
    assert(xc_parse(CAMERA "s.power.source:=battery\ns.power.minute:=0\n", &camera));
    assert(camera.power == XC_POWER_BATTERY && camera.battery_minutes == 0);
    assert(xc_parse(CAMERA "s.power.source:=battery\ns.power.minute:=?\n", &camera));
    assert(camera.power == XC_POWER_BATTERY && camera.battery_minutes == -1);
    assert(xc_parse(CAMERA "s.power.source:=checking\ns.power.minute:=97\n", &camera));
    assert(camera.power == XC_POWER_CHECKING && camera.battery_minutes == -1);
    assert(xc_parse(CAMERA "s.power.volt:=?\nf.rec.media.main:=1\nf.rec.media1.videoinfo:=0x2160:29970:P\n"
                    "f.rec.media2.videoinfo:=3840x2160:29970:P\n", &camera));
    assert(!camera.resolution[0] && !camera.voltage[0]);
    test_fast_edits();
    test_recording();
    test_mdns();
    puts("XC protocol and preset tests: PASS");
    return 0;
}
