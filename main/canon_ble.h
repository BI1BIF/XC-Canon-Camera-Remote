#pragma once

#include "canon_ble_protocol.h"

typedef enum {
    CANON_BLE_OFF, CANON_BLE_STARTING, CANON_BLE_UNPAIRED, CANON_BLE_SCANNING,
    CANON_BLE_PAIRING, CANON_BLE_VERIFYING, CANON_BLE_CONNECTING, CANON_BLE_READY,
    CANON_BLE_SENDING, CANON_BLE_SENT, CANON_BLE_DISCONNECTED, CANON_BLE_NOT_FOUND,
    CANON_BLE_PAIR_FAILED, CANON_BLE_LINK_FAILED, CANON_BLE_SEND_FAILED,
    CANON_BLE_SAVE_FAILED, CANON_BLE_UNAVAILABLE
} canon_ble_state_t;

void canon_ble_enable(bool enable);
void canon_ble_action(canon_ble_action_t action);
canon_ble_state_t canon_ble_state(void);
