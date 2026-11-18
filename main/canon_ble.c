// Canon BR-E1 over ESP-IDF NimBLE. See THIRD_PARTY_NOTICES.txt for protocol references.
#include "canon_ble.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include <stdatomic.h>

static const ble_uuid128_t remote_service = BLE_UUID128_INIT(CANON_BLE_UUID(0));
static const ble_uuid128_t pair_char = BLE_UUID128_INIT(CANON_BLE_UUID(2));
static const ble_uuid128_t trigger_char = BLE_UUID128_INIT(CANON_BLE_UUID(3));
static const uint8_t registration[] = "\x03" CANON_BLE_DEVICE_NAME;

// Callbacks, radio operations and timers all run on the NimBLE host task.
typedef enum { IDLE, SCANNING, CONNECTING, SECURING, DISCOVERING, REGISTERING, RECONNECTING,
               ARMING, CLOSING, READY, PRESSING, HOLDING, RELEASING } phase_t;
typedef struct { uint16_t handle; uint8_t properties; } writable_t;
static phase_t phase;
static bool synced, have_saved, pairing, registered;
static uint8_t own_addr_type, tries;
static uint8_t frames[2][2];
static uint16_t connection = BLE_HS_CONN_HANDLE_NONE, first_handle, last_handle;
static writable_t pair_write, trigger_write;
static ble_addr_t saved, peer;
static struct ble_npl_event work;
static struct ble_npl_callout timer;
static ble_npl_time_t timer_due;
static bool timer_armed;
static atomic_bool wanted;
static atomic_int requested = -1;
static atomic_int state = CANON_BLE_OFF;
static bool started, failed_init;

static int gap_event(struct ble_gap_event *event, void *arg);
static void connect_peer(void);
static void finish_discovery(void);
static void start_pairing(void);
void ble_store_config_init(void);  // NimBLE store backend, as in ESP-IDF's blecent example.

static bool active(void)
{
    return atomic_load(&wanted);
}

static void publish(canon_ble_state_t value)
{
    atomic_store(&state, value);
}

canon_ble_state_t canon_ble_state(void)
{
    return atomic_load(&state);
}

static void later(unsigned ms)
{
    const ble_npl_time_t ticks = ble_npl_time_ms_to_ticks32(ms);
    timer_due = ble_npl_time_get() + ticks;
    timer_armed = true;
    (void)ble_npl_callout_reset(&timer, ticks);
}

static void cancel_timer(void)
{
    timer_armed = false;
    ble_npl_callout_stop(&timer);
}

static bool save_peer(ble_addr_t identity)
{
    identity.type &= 1;  // Connection API expects the public/random identity address type.
    nvs_handle_t nvs;
    if (nvs_open("canon_remote", NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs, "camera_v1", &identity, sizeof(identity));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return false;
    if (have_saved && ble_addr_cmp(&saved, &identity)) (void)ble_gap_unpair(&saved);
    saved = identity;
    have_saved = true;
    return true;
}

static void close_link(void)
{
    cancel_timer();
    phase = CLOSING;
    if (connection != BLE_HS_CONN_HANDLE_NONE) {
        const int rc = ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
        if (!rc || rc == BLE_HS_EALREADY) return;
        if (rc != BLE_HS_ENOTCONN) {
            later(500);
            return;
        }
        connection = BLE_HS_CONN_HANDLE_NONE;
    } else if (ble_gap_conn_active() && !ble_gap_conn_cancel()) return;
    if (ble_gap_disc_active()) (void)ble_gap_disc_cancel();
    phase = IDLE;
}

static void fail(canon_ble_state_t result, int rc)
{
    ESP_LOGW("canon_ble", "Stage %d failed (%d)", phase, rc);
    pairing = false;
    tries = 0;
    publish(active() ? result : CANON_BLE_OFF);
    close_link();
}

static void lost_link(void)
{
    cancel_timer();
    connection = BLE_HS_CONN_HANDLE_NONE;
    const phase_t last = phase;
    phase = IDLE;
    if (!active()) {
        pairing = false;
        publish(CANON_BLE_OFF);
    } else if (last == CLOSING) {
        if (pairing) start_pairing();
        else if (canon_ble_state() == CANON_BLE_OFF) {
            if (have_saved) { peer = saved; tries = 2; connect_peer(); }
            else publish(CANON_BLE_UNPAIRED);
        }
    } else if (tries) {
        --tries;
        phase = RECONNECTING;
        publish(pairing ? CANON_BLE_VERIFYING : CANON_BLE_CONNECTING);
        later(500);
    } else {
        publish(pairing ? CANON_BLE_PAIR_FAILED :
                last == READY ? CANON_BLE_DISCONNECTED :
                last == PRESSING || last == HOLDING || last == RELEASING ?
                CANON_BLE_SEND_FAILED : CANON_BLE_LINK_FAILED);
        pairing = false;
    }
}

static void mark_ready(void)
{
    cancel_timer();
    phase = READY;
    tries = 0;
    const bool saved_ok = !pairing || save_peer(peer);
    pairing = false;
    publish(saved_ok ? CANON_BLE_READY : CANON_BLE_SAVE_FAILED);
}

static canon_ble_state_t write_failure(void)
{
    if (phase == REGISTERING || (phase == ARMING && pairing)) return CANON_BLE_PAIR_FAILED;
    return phase == ARMING ? CANON_BLE_LINK_FAILED : CANON_BLE_SEND_FAILED;
}

static int write_done(uint16_t handle, const struct ble_gatt_error *err,
                      struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (handle != connection) return 0;
    if (phase != REGISTERING && phase != ARMING && phase != PRESSING && phase != RELEASING) return 0;
    if (err->status) {
        fail(write_failure(), err->status);
    } else if (phase == REGISTERING) {
        registered = true;
        phase = RECONNECTING;
        tries = 3;
        later(350);
    } else if (phase == ARMING) {
        if (active()) mark_ready();
        else close_link();
    } else if (phase == PRESSING) {
        phase = HOLDING;
        later(CANON_BLE_PRESS_MS);
    } else if (phase == RELEASING) {
        cancel_timer();
        if (active()) {
            phase = READY;
            publish(CANON_BLE_SENT);
        } else close_link();
    }
    return 0;
}

static void send_frame(writable_t dest, const void *data, uint16_t length)
{
    if (!(dest.properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {
        later(4000);
        const int rc = ble_gattc_write_flat(connection, dest.handle, data, length, write_done, NULL);
        if (rc) fail(write_failure(), rc);
        return;
    }
    const int rc = ble_gattc_write_no_rsp_flat(connection, dest.handle, data, length);
    const struct ble_gatt_error result = {.status = rc};
    write_done(connection, &result, NULL, NULL);
}

static void finish_discovery(void)
{
    if (phase != DISCOVERING || !active()) return;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(connection, &desc)) return;
    if (!desc.sec_state.encrypted || !desc.sec_state.bonded) {
        fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, BLE_HS_EAUTHEN);
        return;
    }
    peer = desc.peer_id_addr;
    peer.type &= 1;
    if (pairing && !registered) {
        phase = REGISTERING;
        send_frame(pair_write, registration, sizeof(registration) - 1);
    } else if (pair_write.handle) {
        // Some bodies expose the registration characteristic on every link.
        // Arm the remote session before allowing shutter commands.
        phase = ARMING;
        send_frame(pair_write, registration, sizeof(registration) - 1);
    } else {
        mark_ready();
    }
}

static int got_characteristic(uint16_t handle, const struct ble_gatt_error *err,
                              const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (handle != connection || phase != DISCOVERING || !active()) return 0;
    if (!err->status && chr && (chr->properties & (BLE_GATT_CHR_PROP_WRITE_NO_RSP | BLE_GATT_CHR_PROP_WRITE))) {
        const writable_t found = {.handle = chr->val_handle, .properties = chr->properties};
        if (!ble_uuid_cmp(&chr->uuid.u, &pair_char.u)) pair_write = found;
        if (!ble_uuid_cmp(&chr->uuid.u, &trigger_char.u)) trigger_write = found;
    } else if (err->status == BLE_HS_EDONE && (pairing && !registered ? pair_write.handle : trigger_write.handle)) {
        finish_discovery();
    } else if (err->status) {
        fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, err->status);
    }
    return 0;
}

static int got_service(uint16_t handle, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (handle != connection || phase != DISCOVERING || !active()) return 0;
    if (!err->status && svc) {
        first_handle = svc->start_handle;
        last_handle = svc->end_handle;
    } else if (err->status == BLE_HS_EDONE && first_handle) {
        const int rc = ble_gattc_disc_all_chrs(handle, first_handle, last_handle, got_characteristic, NULL);
        if (rc) fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, rc);
    } else if (err->status) {
        fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, err->status);
    }
    return 0;
}

static void connect_peer(void)
{
    phase = CONNECTING;
    const int rc = ble_gap_connect(own_addr_type, &peer, 6000, NULL, gap_event, NULL);
    if (rc) lost_link();
}

static void discover_remote(void)
{
    phase = DISCOVERING;
    later(6000);
    const int rc = ble_gattc_disc_svc_by_uuid(connection, &remote_service.u, got_service, NULL);
    if (rc) fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (active() && phase == SCANNING &&
            canon_ble_advertises_remote(event->disc.data, event->disc.length_data)) {
            peer = event->disc.addr;
            if (ble_gap_disc_cancel()) break;
            publish(CANON_BLE_PAIRING);
            connect_peer();
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (phase == SCANNING) fail(CANON_BLE_NOT_FOUND, event->disc_complete.reason);
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) {
            lost_link();
        } else {
            connection = event->connect.conn_handle;
            if (!active() || phase == CLOSING) { close_link(); break; }
            phase = SECURING;
            first_handle = 0;
            pair_write = trigger_write = (writable_t){0};
            later(pairing && !registered ? 30000 : 8000);
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(connection, &desc);
            if (!rc && (!pairing || registered)) {
                struct ble_store_value_sec keys;
                const struct ble_store_key_sec key = {.peer_addr = desc.peer_id_addr};
                rc = ble_store_read_peer_sec(&key, &keys);
                if (!rc && !keys.ltk_present) rc = BLE_HS_EAUTHEN;
            }
            if (!rc && desc.sec_state.encrypted) {
                discover_remote();
            } else {
                if (!rc) rc = ble_gap_security_initiate(connection);
                if (rc && rc != BLE_HS_EALREADY)
                    fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, rc);
            }
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (phase == SECURING && active() && event->enc_change.conn_handle == connection) {
            struct ble_gap_conn_desc desc;
            if (event->enc_change.status || ble_gap_conn_find(connection, &desc) || !desc.sec_state.encrypted)
                fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, event->enc_change.status);
            else discover_remote();
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (active() && pairing && !registered) {
            struct ble_gap_conn_desc desc;
            if (!ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) &&
                !ble_store_util_delete_peer(&desc.peer_id_addr)) return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    case BLE_GAP_EVENT_DISCONNECT:
        if (phase == RELEASING || phase == PRESSING || phase == HOLDING) {
            // The press may already have taken effect. Never replay it after a drop.
            tries = 0;
        }
        lost_link();
        break;
    default: break;
    }
    return 0;
}

static void timer_cb(struct ble_npl_event *event)
{
    (void)event;
    // A previous deadline may already have been queued before a reset or stop.
    if (!timer_armed || (int32_t)(ble_npl_time_get() - timer_due) < 0) return;
    timer_armed = false;
    switch (phase) {
    case RECONNECTING:
        if (connection != BLE_HS_CONN_HANDLE_NONE) {
            if (ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM))
                fail(CANON_BLE_PAIR_FAILED, BLE_HS_ENOTCONN);
            else later(4000);
        } else connect_peer();
        break;
    case CLOSING:
        close_link();
        if (phase == IDLE && pairing && active()) start_pairing();
        break;
    case HOLDING:
        phase = RELEASING;
        send_frame(trigger_write, frames[1], sizeof(frames[1]));
        break;
    case SECURING: case DISCOVERING: case REGISTERING:
        fail(pairing ? CANON_BLE_PAIR_FAILED : CANON_BLE_LINK_FAILED, BLE_HS_ETIMEOUT);
        break;
    case ARMING: case PRESSING: case RELEASING:
        fail(write_failure(), BLE_HS_ETIMEOUT);
        break;
    default: break;
    }
}

static void start_pairing(void)
{
    static const struct ble_gap_disc_params scan = {.filter_duplicates = 1, .itvl = 0x60, .window = 0x30};
    // Keep the working bond until a replacement succeeds, but drop failed candidates.
    ble_addr_t bonds[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    if (!ble_store_util_bonded_peers(bonds, &count, CONFIG_BT_NIMBLE_MAX_BONDS))
        for (int i = 0; i < count; ++i)
            if (!have_saved || ble_addr_cmp(&bonds[i], &saved)) (void)ble_gap_unpair(&bonds[i]);
    pairing = true;
    registered = false;
    tries = 0;
    phase = SCANNING;
    publish(CANON_BLE_SCANNING);
    const int rc = ble_gap_disc(own_addr_type, 15000, &scan, gap_event, NULL);
    if (rc) fail(CANON_BLE_PAIR_FAILED, rc);
}

static void work_cb(struct ble_npl_event *event)
{
    (void)event;
    const int action = atomic_exchange(&requested, -1);
    if (!active()) {
        if (phase == PRESSING || phase == HOLDING || phase == RELEASING) {
            // Finish releasing a button already sent before disconnecting.
            publish(CANON_BLE_OFF);
            return;
        }
        pairing = false;
        tries = 0;
        publish(CANON_BLE_OFF);
        close_link();
    } else if (synced) {
        if (phase == IDLE && action == CANON_BLE_PAIR) {
            start_pairing();
        } else if (phase == IDLE && have_saved) {
            // Reconnection never doubles as a shutter or record press.
            peer = saved;
            tries = 2;
            publish(CANON_BLE_CONNECTING);
            connect_peer();
        } else if (phase == IDLE) {
            publish(CANON_BLE_UNPAIRED);
        } else if (phase == READY && action == CANON_BLE_PAIR) {
            // Disconnect before scanning. Pairing is started by DISCONNECT.
            pairing = true;
            publish(CANON_BLE_SCANNING);
            close_link();
            if (phase == IDLE) start_pairing();
        } else if (phase == READY && canon_ble_frames(action, frames)) {
            phase = PRESSING;
            publish(CANON_BLE_SENDING);
            send_frame(trigger_write, frames[0], sizeof(frames[0]));
        }
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &own_addr_type);
    synced = rc == 0;
    if (synced) work_cb(NULL);
    else publish(CANON_BLE_UNAVAILABLE);
}

static void on_reset(int reason)
{
    ESP_LOGW("canon_ble", "Host reset (%d)", reason);
    synced = false;
    pairing = false;
    phase = IDLE;
    connection = BLE_HS_CONN_HANDLE_NONE;
    cancel_timer();
    publish(active() ? CANON_BLE_STARTING : CANON_BLE_OFF);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}

void canon_ble_enable(bool enable)
{
    atomic_store(&requested, -1);
    atomic_store(&wanted, enable);
    if (started) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &work);
        return;
    }
    if (!enable) return;
    publish(CANON_BLE_STARTING);
    if (failed_init) { publish(CANON_BLE_UNAVAILABLE); return; }
    const esp_err_t init = nimble_port_init();
    if (init != ESP_OK) {
        ESP_LOGW("canon_ble", "Init: %s", esp_err_to_name(init));
        publish(CANON_BLE_UNAVAILABLE);
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open("canon_remote", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(saved);
        have_saved = nvs_get_blob(nvs, "camera_v1", &saved, &size) == ESP_OK &&
                     size == sizeof(saved) && saved.type <= BLE_ADDR_RANDOM_ID;
        if (have_saved) saved.type &= 1;
        nvs_close(nvs);
    }
    ble_npl_event_init(&work, work_cb, NULL);
    if (ble_npl_callout_init(&timer, nimble_port_get_dflt_eventq(), timer_cb, NULL)) {
        ble_npl_event_deinit(&work);
        failed_init = nimble_port_deinit() != ESP_OK;
        publish(CANON_BLE_UNAVAILABLE);
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    if (xTaskCreate(host_task, "canon_ble", NIMBLE_HS_STACK_SIZE, NULL, configMAX_PRIORITIES - 4, NULL) != pdPASS) {
        ble_npl_callout_deinit(&timer);
        ble_npl_event_deinit(&work);
        failed_init = nimble_port_deinit() != ESP_OK;
        publish(CANON_BLE_UNAVAILABLE);
        return;
    }
    started = true;
}

void canon_ble_action(canon_ble_action_t action)
{
    if (!started || !active()) return;
    const canon_ble_state_t current = canon_ble_state();
    if (current == CANON_BLE_STARTING || current == CANON_BLE_SCANNING ||
        current == CANON_BLE_PAIRING || current == CANON_BLE_VERIFYING ||
        current == CANON_BLE_CONNECTING || current == CANON_BLE_SENDING) return;
    int empty = -1;
    if (atomic_compare_exchange_strong(&requested, &empty, action))
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &work);
}
