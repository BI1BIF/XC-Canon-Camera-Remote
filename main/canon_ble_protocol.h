#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { CANON_BLE_PHOTO, CANON_BLE_FOCUS, CANON_BLE_MOVIE, CANON_BLE_PAIR } canon_ble_action_t;

// Canon BR-E1 service and characteristic UUIDs in BLE wire order.
#define CANON_BLE_UUID(kind) 0x21, 0xa8, 0xff, 0x2f, 0x49, 0xd8, 0, 0, 0, 0x10, 0, 0, kind, 0, 5, 0
#define CANON_BLE_DEVICE_NAME "AI Passport"
#define CANON_BLE_PRESS_MS 200

bool canon_ble_advertises_remote(const uint8_t *data, size_t size);
bool canon_ble_frames(canon_ble_action_t action, uint8_t frames[2][2]);
