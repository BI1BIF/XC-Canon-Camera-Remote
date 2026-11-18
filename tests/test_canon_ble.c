#include "canon_ble_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const uint8_t canon[] = {
        2, 0x01, 0x06, 17, 0x07, 0x21, 0xa8, 0xff, 0x2f, 0x49, 0xd8,
        0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00,
    };
    uint8_t adv[sizeof(canon)];
    assert(canon_ble_advertises_remote(canon, sizeof(canon)));
    for (size_t i = 0; i < sizeof(canon); ++i)
        assert(!canon_ble_advertises_remote(canon, i));
    memcpy(adv, canon, sizeof(canon));
    adv[4] = 0x06;
    assert(canon_ble_advertises_remote(adv, sizeof(adv)));
    adv[4] = 0x09; // A local name containing the bytes is not a service.
    assert(!canon_ble_advertises_remote(adv, sizeof(adv)));
    adv[4] = 0x07;
    adv[19] = 1; // Canon smartphone service must not be treated as BR-E1.
    assert(!canon_ble_advertises_remote(adv, sizeof(adv)));
    adv[19] = 5;
    adv[3] = 255; // Overrun in a malformed advertisement.
    assert(!canon_ble_advertises_remote(adv, sizeof(adv)));

    uint8_t frames[2][2];
    const uint8_t photo[] = {0x8c, 0, 0x0c, 0};
    const uint8_t focus[] = {0x4c, 0, 0x0c, 0};
    const uint8_t movie[] = {0x88, 0, 0x08, 0};
    assert(canon_ble_frames(CANON_BLE_PHOTO, frames) && !memcmp(frames, photo, sizeof(frames)));
    assert(canon_ble_frames(CANON_BLE_FOCUS, frames) && !memcmp(frames, focus, sizeof(frames)));
    assert(canon_ble_frames(CANON_BLE_MOVIE, frames) && !memcmp(frames, movie, sizeof(frames)));
    assert(!canon_ble_frames(CANON_BLE_PAIR, frames));
    puts("Canon BR-E1 advertisements and shutter/focus/movie frames: PASS");
    return 0;
}
