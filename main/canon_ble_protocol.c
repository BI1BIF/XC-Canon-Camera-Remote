// BR-E1 commands: maxmacstn/ESP32-Canon-BLE-Remote and pulsar-trigger's hardware capture.
#include "canon_ble_protocol.h"

#include <string.h>

bool canon_ble_advertises_remote(const uint8_t *data, size_t size)
{
    static const uint8_t uuid[] = {CANON_BLE_UUID(0)};
    while (size && *data) {
        const size_t length = *data++;
        if (length >= size) return false;
        // Incomplete and complete lists of 128-bit service UUIDs.
        if (data[0] == 0x06 || data[0] == 0x07) {
            for (size_t i = 1; i + sizeof(uuid) <= length; i += sizeof(uuid))
                if (!memcmp(data + i, uuid, sizeof(uuid))) return true;
        }
        data += length;
        size -= length + 1;
    }
    return false;
}

bool canon_ble_frames(canon_ble_action_t action, uint8_t frames[2][2])
{
    uint8_t press;
    switch (action) {
    case CANON_BLE_PHOTO: press = 0x8c; break;
    case CANON_BLE_FOCUS: press = 0x4c; break;
    case CANON_BLE_MOVIE: press = 0x88; break;
    default: return false;
    }
    // The hardware BR-E1 sends [button | mode, 0] and then [mode, 0].
    frames[0][0] = press;
    frames[1][0] = press & 0x0f;
    frames[0][1] = frames[1][1] = 0;
    return true;
}
