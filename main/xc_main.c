#include "xc_client.h"
#include "xc_navigation.h"
#include "xc_screen.h"
#include "xc_ui.h"
#include "xc_wifi.h"
#include "canon_ble.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct { bsp_btn_t key; bsp_btn_ev_t event; int64_t pressed_us; } input_t;
typedef struct { bsp_btn_t key; bsp_btn_ev_t event; } raw_key_t;
typedef enum { NET_READ, NET_TALLY, NET_WRITE, NET_RECORD, NET_IDLE } net_action_t;
typedef struct {
    net_action_t action;
    unsigned link, field, revision;
    const char *key;
    int value;
    uint32_t local_ip;
    xc_endpoint_t endpoint;
    xc_credentials_t credentials;
} net_job_t;
typedef struct {
    net_action_t action;
    unsigned link, field, revision;
    int64_t start;
    xc_result_t result;
    xc_tally_t tally;
} net_reply_t;

static QueueHandle_t inputs, raw_keys, net_jobs, net_replies;
static atomic_bool keep_screen_on, screen_awake = true;
static xc_page_t page = LAUNCHER;
static xc_location_t setup_return, device_return, settings_return, list_return;
static unsigned settings_focus;
static bool ble_mode;
static canon_ble_state_t ble_status;
static esp_netif_ip_info_t lan;
static bool online, connected, joining, network_ready, retry_wifi, restore_pending = true;
static bool network_attempted;
static esp_err_t storage_status;
static xc_setup_mode_t setup_mode;
static bool setup_lan;
static bool presets_saved;
static unsigned focus, preset, preset_mask = 7, progress;
static int count;
static int64_t deadline, next_wifi, next_poll, next_tally, tally_time_us, next_device;
static int xc_reply_ms = -1;
static bool broad_tally, verify_camera, auth_pending;
static bool scan_cancelled;
static xc_endpoint_t target, found[XC_MAX_CAMERAS];
static xc_credentials_t login;
static xc_camera_t camera, snapshot;
static xc_edit_t edits[XC_FIELDS];
static xc_record_t record_pending;
static int64_t record_until, record_notice_until;
static const char *record_notice;
static const char *record_failure;
static unsigned net_link = 1;
static bool net_busy;
static int64_t next_idle;
static int roll_direction;
static struct {
    uint32_t addr;
    uint16_t port;
    char ssid[33], model[48], address[32];
} remembered;
static char setup_key[11];
static const char *message;

static const char *const settings_labels[] = {
    [XC_MENU_RETURN] = "返回遥控界面",
    [XC_MENU_MODE] = "切换遥控方式",
    [XC_MENU_DEVICE] = "设备状态",
    [XC_MENU_PRESETS] = "调节项预设",
    [XC_MENU_STATUS] = "查看相机状态",
    [XC_MENU_CAMERA] = "网页设置相机",
    [XC_MENU_DISCONNECT] = "断开并选相机",
    [XC_MENU_WIFI] = "更换 Wi-Fi",
    [XC_MENU_FORGET] = "清除 Wi-Fi",
};

static void start_join(const xc_wifi_login_t *wifi);

static void start_network(void)
{
    if (!network_attempted) {
        network_attempted = true;
        const esp_err_t err = storage_status == ESP_OK ? xc_wifi_init() : storage_status;
        network_ready = err == ESP_OK;
        if (network_ready) start_join(NULL);
        else ESP_LOGE("xc", "Networking init: %s", esp_err_to_name(err));
    }
    if (!network_ready) message = "网络初始化失败";
}

static const char *const ble_labels[] = {
    [CANON_BLE_OFF] = "正在开启蓝牙",
    [CANON_BLE_STARTING] = "正在开启蓝牙",
    [CANON_BLE_UNPAIRED] = "尚未配对",
    [CANON_BLE_SCANNING] = "正在搜索相机",
    [CANON_BLE_PAIRING] = "正在配对",
    [CANON_BLE_VERIFYING] = "正在验证配对",
    [CANON_BLE_CONNECTING] = "正在重连相机",
    [CANON_BLE_READY] = "已连接相机",
    [CANON_BLE_SENDING] = "正在发送指令",
    [CANON_BLE_SENT] = "已连接相机",
    [CANON_BLE_DISCONNECTED] = "蓝牙已断开",
    [CANON_BLE_NOT_FOUND] = "未找到配对中的相机",
    [CANON_BLE_PAIR_FAILED] = "配对未完成",
    [CANON_BLE_LINK_FAILED] = "未能连接相机",
    [CANON_BLE_SEND_FAILED] = "指令发送未完成",
    [CANON_BLE_SAVE_FAILED] = "已连接，配对未保存",
    [CANON_BLE_UNAVAILABLE] = "蓝牙无法启动",
};

static void go(xc_page_t next)
{
    page = next;
    atomic_store(&keep_screen_on, next == SETUP);
    focus = 0;
    roll_direction = 0;
    message = NULL;
}

static xc_location_t location(void)
{
    return (xc_location_t){page, focus};
}

static void return_to(xc_location_t back)
{
    go(back.page);
    focus = back.focus;
}

static void open_settings(void)
{
    settings_return = location();
    settings_focus = 0;
    go(SETTINGS);
}

static void return_settings(void)
{
    go(SETTINGS);
    focus = settings_focus;
}

static void open_device(void)
{
    device_return = location();
    go(DEVICE);
    next_device = esp_timer_get_time() + 2000000;
}

static void show(void)
{
    xc_view_t v = {.connected = connected && online, .tally = online && connected ? camera.tally : XC_UNKNOWN,
                   .screen_off = !atomic_load(&screen_awake),
                   .tally_time_us = tally_time_us, .badge = online ? "Wi-Fi" : "离线",
                   .footer = "上下 选择   确定 进入", .note = message};
    char ip[INET_ADDRSTRLEN] = "", ssid[33] = "", buffer[72], qr[64];
    char labels[XC_MENU_ROWS][80];
    if (target.addr) inet_ntop(AF_INET, &target.addr, ip, sizeof(ip));
    unsigned first = 0;
    switch (page) {
    case LAUNCHER:
        v.launcher = true;
        v.badge = "选择";
        v.title = "佳能相机遥控器";
        v.subtitle = "选择遥控方式";
        v.row[0] = "XC 遥控器";
        v.row[1] = "蓝牙遥控器";
        v.row_hint[0] = "Wi-Fi  参数与 Tally";
        v.row_hint[1] = "BR-E1  拍照与录像";
        v.row_count = 2;
        break;
    case HOME:
        v.title = online ? "连接佳能相机" : "连接本地 Wi-Fi";
        if (online) {
            xc_wifi_ssid(ssid);
            v.subtitle = ssid;
            v.subtitle_is_ssid = true;
            v.row[0] = "发现相机";
            v.row[1] = "手动连接相机";
            v.row[2] = "遥控设置";
            v.row[3] = "返回功能首页";
            v.row_count = 4;
        } else {
            v.subtitle = joining ? "正在获取网络地址" : "需要 2.4 GHz 网络";
            v.row[0] = "设置 Wi-Fi";
            v.row[1] = "设备状态";
            v.row[2] = "编辑屏幕标题";
            v.row[3] = "返回功能首页";
            v.row_count = 4;
        }
        v.footer = "确定 进入   长按 功能首页";
        break;
    case SCAN:
        v.title = "正在发现相机";
        snprintf(buffer, sizeof(buffer), "%u%%", progress);
        v.subtitle = buffer;
        v.detail = "核验本地 XC 接口";
        v.footer = "长按确定 取消";
        break;
    case LIST:
        v.title = count ? "选择相机" : "未发现相机";
        v.subtitle = count ? "长按确定 返回" : "确认同网并开启 XC";
        first = focus / XC_MENU_ROWS * XC_MENU_ROWS;
        for (unsigned i = first; i < (unsigned)count + 2 && i < first + XC_MENU_ROWS; ++i) {
            if (i < (unsigned)count) {
                char addr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &found[i].addr, addr, sizeof(addr));
                snprintf(labels[i-first], sizeof(labels[0]), "%s  %s",
                         !strncmp(found[i].name, "Canon ", 6) ? found[i].name + 6 : found[i].name, addr);
                v.row[i-first] = labels[i-first];
            } else v.row[i-first] = i == (unsigned)count ? "重新发现" : "手动连接相机";
            ++v.row_count;
        }
        break;
    case LINK:
        v.title = "正在连接相机";
        v.subtitle = ip;
        v.footer = "正在连接，请稍候";
        break;
    case LIVE:
        v.title = connected ? camera.model : "相机连接中断";
        v.subtitle = ip;
        v.camera = connected ? &camera : NULL;
        v.preset = preset;
        v.roll_direction = roll_direction;
        v.footer = connected ? "单击切换  上下调节" : "单击重连";
        v.footer_alt = connected && camera.recording == XC_REC_ACTIVE ?
                       "长按停止  双击设置" : connected ? "长按录像  双击设置" : "双击设置";
        if (record_pending != XC_REC_UNKNOWN)
            v.note = record_pending == XC_REC_ACTIVE ? "正在开始录像" : "正在停止录像";
        else if (record_notice) v.note = record_notice;
        break;
    case SETTINGS:
        v.title = ble_mode ? "蓝牙遥控设置" : "XC 遥控设置";
        v.subtitle = "长按确定 返回功能首页";
        {
            unsigned items;
            const xc_menu_item_t *rows = xc_settings_menu(ble_mode, target.addr != 0, &items);
            if (focus >= items) focus = items - 1;
            first = focus >= XC_MENU_ROWS ? focus - XC_MENU_ROWS + 1 : 0;
            for (unsigned i = first; i < items && i < first + XC_MENU_ROWS; ++i)
                v.row[v.row_count++] = settings_labels[rows[i]];
            if (!message && first + XC_MENU_ROWS < items) v.note = "下移查看更多";
        }
        break;
    case BLE_REMOTE:
        v.title = "佳能蓝牙遥控";
        v.subtitle = ble_labels[ble_status];
        v.row[0] = "拍照";
        v.row[1] = "自动对焦";
        v.row[2] = "启停录像";
        v.row[3] = "配对相机";
        v.row_count = 4;
        v.footer = "确定 执行   双击 设置";
        v.footer_alt = "长按确定 返回功能首页";
        v.note = message ? message : ble_status == CANON_BLE_SENT ? "指令已发送" :
                 ble_status == CANON_BLE_SCANNING || ble_status == CANON_BLE_UNPAIRED ||
                 ble_status == CANON_BLE_NOT_FOUND ? "请在相机选无线遥控配对" :
                 ble_status == CANON_BLE_PAIR_FAILED ? "检查相机是否已有遥控器" :
                 ble_status == CANON_BLE_VERIFYING ? "在相机上确认配对" :
                 ble_status == CANON_BLE_LINK_FAILED || ble_status == CANON_BLE_DISCONNECTED ?
                 "唤醒相机后按确定重连" : NULL;
        break;
    case PRESETS:
        v.title = "调节项预设";
        v.subtitle = "勾选确定键循环的项目";
        for (unsigned i = 0; i < XC_FIELDS; ++i) {
            snprintf(labels[i], sizeof(labels[i]), "%s  %s", xc_field_name[i],
                     preset_mask & (1u << i) ? "[ON]" : "[OFF]");
            v.row[i] = labels[i];
        }
        v.row[3] = "保存并返回";
        v.row_count = 4;
        v.footer = "上下 选择   确定 开关";
        break;
    case STATUS:
        v.title = connected ? camera.model : "状态不可用";
        v.subtitle = ip;
        snprintf(buffer, sizeof(buffer), "固件 %s", connected && camera.firmware[0] ? camera.firmware : "--");
        v.row[0] = buffer;
        snprintf(labels[1], sizeof(labels[1]), "ISO / 增益  %s", connected && camera.iso[0] ? camera.iso : "--");
        v.row[1] = labels[1];
        v.row[2] = !connected ? "连接中断" : camera.recording == XC_REC_ACTIVE ? "正在录像" :
                   camera.standby ? "相机休眠" : camera.recording == XC_REC_IDLE ? "录像待机" : "相机在线";
        v.row_count = 3;
        v.footer = "确定 设备状态   长按 返回";
        break;
    case DEVICE: {
        v.title = "设备状态";
        if (online) xc_wifi_ssid(ssid);
        v.subtitle = ssid[0] ? ssid : "Wi-Fi 未连接";
        v.subtitle_is_ssid = ssid[0] != 0;
        if (!focus) {
            if (online) inet_ntop(AF_INET, &lan.ip.addr, ip, sizeof(ip));
            snprintf(labels[2], sizeof(labels[2]), "IP  %s", online ? ip : "--");
            int rssi;
            unsigned channel;
            if (online && xc_wifi_signal(&rssi, &channel))
                snprintf(labels[3], sizeof(labels[3]), "信号 %d dBm / CH %u", rssi, channel);
            else strcpy(labels[3], "信号 --");
            v.row[2] = labels[2];
            v.row[3] = labels[3];
            v.row_count = 4;
            v.power = true;
            if (!message) v.note = online ? "浏览器打开 IP 可改标题" : "充电请看机身绿灯";
            v.footer = "下 网络详情   确定 返回";
        } else {
            char mask[INET_ADDRSTRLEN] = "--", gateway[INET_ADDRSTRLEN] = "--";
            if (online) {
                inet_ntop(AF_INET, &lan.netmask.addr, mask, sizeof(mask));
                inet_ntop(AF_INET, &lan.gw.addr, gateway, sizeof(gateway));
            }
            snprintf(labels[0], sizeof(labels[0]), "网关 %s", gateway);
            snprintf(labels[1], sizeof(labels[1]), "掩码 %s", mask);
            if (connected && xc_reply_ms >= 0) snprintf(labels[2], sizeof(labels[2]), "XC 响应 %d ms", xc_reply_ms);
            else strcpy(labels[2], "XC 响应 --");
            for (unsigned i = 0; i < 3; ++i) v.row[i] = labels[i];
            v.row[3] = "充电请看机身绿灯";
            v.row_count = 4;
            v.footer = "上 状态概览   确定 返回";
        }
        break;
    }
    case SETUP:
        v.title = setup_mode == XC_SETUP_DISPLAY ? "编辑屏幕标题" :
                  setup_mode == XC_SETUP_CAMERA ? "网页设置相机" : "设置本地 Wi-Fi";
        if (setup_lan) {
            inet_ntop(AF_INET, &lan.ip.addr, ip, sizeof(ip));
            snprintf(qr, sizeof(qr), "http://%s/camera", ip);
            snprintf(labels[0], sizeof(labels[0]), "http://%s", ip);
            v.subtitle = labels[0];
            snprintf(buffer, sizeof(buffer), "验证码  %s", setup_key);
            v.footer = "手机保持当前 Wi-Fi";
        } else {
            v.subtitle = "扫码加入 " XC_SETUP_SSID;
            snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:" XC_SETUP_SSID ";P:%s;;", setup_key);
            snprintf(buffer, sizeof(buffer), "密码  %s", setup_key);
            v.footer = "打开 192.168.4.1";
        }
        v.qr = qr;
        v.note = buffer;
        v.footer_alt = "长按确定 取消并返回";
        break;
    case FORGET:
        v.title = "清除已存 Wi-Fi";
        v.subtitle = "清除后需要重新配置";
        v.row[0] = "返回";
        v.row[1] = "确认清除";
        v.row_count = 2;
        v.footer = "单击确认   长按取消";
        break;
    }
    if (ble_mode && page != LAUNCHER) {
        v.connected = false;
        v.tally = XC_UNKNOWN;
        v.tally_time_us = 0;
        v.badge = "BLE";
    }
    v.focus = (page == SETUP || page == STATUS || page == DEVICE) ? XC_MENU_ROWS : focus - first;
    if (bsp_lvgl_lock(200)) {
        xc_ui_draw(&v);
        bsp_lvgl_unlock();
        roll_direction = 0;
    }
}

static void save_presets(void)
{
    if (presets_saved) return;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("xc_remote", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "presets", preset_mask);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    presets_saved = err == ESP_OK;
    if (!presets_saved) message = "预设保存失败";
}

static bool store_camera(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("xc_remote", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = remembered.addr ? nvs_set_blob(nvs, "camera_v1", &remembered, sizeof(remembered)) :
                                nvs_erase_key(nvs, "camera_v1");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) ESP_LOGW("xc", "Camera memory: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static bool remember_camera(void)
{
    char ssid[33];
    xc_wifi_ssid(ssid);
    if (!ssid[0]) return false;
    if (remembered.addr == target.addr && remembered.port == target.port && !strcmp(remembered.ssid, ssid) &&
        !strcmp(remembered.model, camera.model) && !strcasecmp(remembered.address, camera.address)) return true;
    remembered.addr = target.addr;
    remembered.port = target.port;
    strcpy(remembered.ssid, ssid);
    strcpy(remembered.model, camera.model);
    strcpy(remembered.address, camera.address);
    if (store_camera()) return true;
    remembered.addr = 0;
    return false;
}

static void discard_network(void)
{
    if (net_busy) {
        net_reply_t ignored;
        xQueueReceive(net_replies, &ignored, portMAX_DELAY);
        net_busy = false;
    }
    ++net_link;
    memset(edits, 0, sizeof(edits));
    record_pending = XC_REC_UNKNOWN;
    record_notice = NULL;
    next_idle = 0;
}

static void notify_record(const char *notice, int64_t now)
{
    record_notice = notice;
    record_notice_until = now + 3000000;
}

static void forget_camera(void)
{
    discard_network();
    if (online && connected) xc_control_idle(&target, &login, true);
    xc_client_reset();
    target = (xc_endpoint_t){0};
    memset(&login, 0, sizeof(login));
    connected = auth_pending = verify_camera = false;
    if (!remembered.addr) return;
    remembered.addr = 0;
    if (!store_camera()) message = "无法清除相机记忆";
}

static void setup(xc_setup_mode_t mode)
{
    if (!network_ready || (mode == XC_SETUP_CAMERA && !online)) {
        message = "Wi-Fi 未就绪";
        return;
    }
    setup_mode = mode;
    setup_lan = online && mode != XC_SETUP_WIFI;
    setup_return = location();
    if (xc_setup_start(mode, mode == XC_SETUP_CAMERA ? &target : NULL, setup_lan, setup_key) != ESP_OK) {
        message = setup_lan ? "无法打开设置网页" : "无法开启设置热点";
        return;
    }
    deadline = esp_timer_get_time() + 180000000;
    go(SETUP);
}

static void end_setup(void)
{
    xc_setup_stop();
    memset(setup_key, 0, sizeof(setup_key));
    xQueueReset(inputs);
}

static const char *error_note(xc_result_t r)
{
    switch (r) {
    case XC_AUTH: return "需要相机账号或权限";
    case XC_WRONG_DEVICE: return "未识别到 XC 相机";
    case XC_DENIED: return "相机未接受调节";
    default: return "相机没有响应";
    }
}

static xc_result_t apply_camera(xc_result_t r, int64_t start)
{
    if (r == XC_OK && verify_camera && !xc_camera_matches(&snapshot, remembered.model, remembered.address))
        r = XC_WRONG_DEVICE;
    const bool had_tally = connected && !broad_tally && snapshot.tally == XC_UNKNOWN;
    connected = r == XC_OK;
    auth_pending = r == XC_AUTH;
    const int64_t now = esp_timer_get_time();
    xc_reply_ms = connected ? (int)((now - start) / 1000) : -1;
    if (connected) {
        if (record_pending != XC_REC_UNKNOWN && record_until) {
            if (snapshot.recording == record_pending) {
                record_pending = XC_REC_UNKNOWN;
            } else if (now >= record_until) {
                record_pending = XC_REC_UNKNOWN;
                notify_record(record_failure, now);
            }
        }
        if (had_tally) snapshot.tally = camera.tally;
        else tally_time_us = now;
        for (unsigned i = 0; i < XC_FIELDS; ++i) {
            if (xc_edit_merge(&edits[i], &camera.settings[i], &snapshot.settings[i], now) && !message)
                message = "已同步相机实测值";
        }
        camera = snapshot;
    } else {
        tally_time_us = 0;
        ++net_link;
        memset(edits, 0, sizeof(edits));
        record_pending = XC_REC_UNKNOWN;
        record_notice = NULL;
        next_idle = 0;
        xc_client_reset();
    }
    const int64_t interval = broad_tally && atomic_load(&screen_awake) ? 220000 : 850000;
    next_poll = connected ? start + interval : now + (auth_pending ? 10000000 : 3000000);
    next_tally = now + 100000;
    return r;
}

static void apply_tally(xc_result_t r, xc_tally_t tally, int64_t start)
{
    const int64_t now = esp_timer_get_time();
    next_tally = start + 100000;
    if (next_tally < now + 20000) next_tally = now + 20000;
    if (r == XC_OK) {
        camera.tally = tally;
        tally_time_us = now;
        xc_reply_ms = (int)((now - start) / 1000);
    } else if (r == XC_WRONG_DEVICE || r == XC_DENIED) {
        broad_tally = true;
        next_poll = now;
    } else if (r == XC_AUTH || (now - tally_time_us > 800000 && next_poll > now)) {
        next_poll = now;
    }
}

static void network_worker(void *arg)
{
    (void)arg;
    for (;;) {
        net_job_t job;
        xQueueReceive(net_jobs, &job, portMAX_DELAY);
        net_reply_t reply = {.action = job.action, .link = job.link, .field = job.field,
                             .revision = job.revision, .start = esp_timer_get_time(), .result = XC_NO_LINK};
        esp_netif_ip_info_t current;
        if (xc_wifi_ip(&current) && current.ip.addr == job.local_ip) {
            switch (job.action) {
            case NET_READ: reply.result = xc_read(&job.endpoint, &job.credentials, &snapshot); break;
            case NET_TALLY: reply.result = xc_read_tally(&job.endpoint, &job.credentials, &reply.tally); break;
            case NET_WRITE: reply.result = xc_adjust(&job.endpoint, &job.credentials, job.key, job.value); break;
            case NET_RECORD: reply.result = xc_set_record(&job.endpoint, &job.credentials, job.value); break;
            case NET_IDLE: xc_control_idle(&job.endpoint, &job.credentials, false); break;
            }
        }
        memset(&job.credentials, 0, sizeof(job.credentials));
        xQueueSend(net_replies, &reply, portMAX_DELAY);
    }
}

static void queue_network(net_action_t action, unsigned field)
{
    net_job_t job = {.action = action, .link = net_link, .field = field,
                     .local_ip = lan.ip.addr, .endpoint = target, .credentials = login};
    if (action == NET_WRITE) {
        job.key = camera.settings[field].key;
        job.value = camera.settings[field].value;
        job.revision = edits[field].revision;
    } else if (action == NET_RECORD) {
        job.value = record_pending;
    }
    net_busy = xQueueSend(net_jobs, &job, 0) == pdTRUE;
}

static bool take_network(void)
{
    net_reply_t reply;
    if (!net_busy || xQueueReceive(net_replies, &reply, 0) != pdTRUE) return false;
    net_busy = false;
    if (reply.link != net_link) {
        xc_client_reset();
        return false;
    }
    if (reply.action == NET_IDLE) return false;
    if (reply.action == NET_RECORD) {
        const int64_t now = esp_timer_get_time();
        next_idle = now + 900000;
        next_poll = now;
        // A delayed or lost reply can still mean the camera received the command.
        record_until = now + 3000000;
        record_failure = reply.result == XC_AUTH ? "录像需要相机权限" :
                         reply.result == XC_DENIED ? "相机未接受录像操作" : "录像状态未确认";
    } else if (reply.action == NET_WRITE) {
        const int64_t now = esp_timer_get_time();
        next_idle = now + 900000;
        if (xc_edit_reply(&edits[reply.field], reply.revision, reply.result == XC_OK, now)) {
            if (reply.result != XC_OK) message = error_note(reply.result);
            const int64_t verify_at = now + (reply.result == XC_OK ? 200000 : 0);
            if (next_poll > verify_at) next_poll = verify_at;
        }
    } else if (reply.action == NET_TALLY) {
        apply_tally(reply.result, reply.tally, reply.start);
    } else {
        const bool was_connected = connected;
        const xc_result_t result = apply_camera(reply.result, reply.start);
        if (connected && !was_connected) {
            message = NULL;
            if (!remember_camera()) message = "相机记忆保存失败";
            else verify_camera = true;
        } else if (!connected && (was_connected || (page == LIVE && !message))) {
            message = error_note(result);
        }
    }
    return true;
}

static void link_camera(bool manual)
{
    discard_network();
    xc_client_reset();
    broad_tally = false;
    connected = false;
    go(LINK);
    show();
    const int64_t start = esp_timer_get_time();
    const xc_result_t r = apply_camera(online ? xc_read(&target, &login, &snapshot) : XC_NO_LINK, start);
    xQueueReset(inputs);
    go(LIVE);
    if (r == XC_AUTH && manual) {
        setup(XC_SETUP_CAMERA);
    } else if (r != XC_OK) {
        message = error_note(r);
    } else if (!remember_camera()) {
        message = "相机记忆保存失败";
    } else {
        verify_camera = true;
    }
}

static bool restore_camera(void)
{
    char ssid[33];
    if (!remembered.addr) return false;
    xc_wifi_ssid(ssid);
    if (strcmp(ssid, remembered.ssid)) return false;
    target = (xc_endpoint_t){.addr = remembered.addr, .port = remembered.port};
    verify_camera = true;
    link_camera(false);
    return true;
}

static bool scan_progress(unsigned percent)
{
    progress = percent;
    show();
    input_t in;
    while (xQueueReceive(inputs, &in, 0) == pdTRUE) {
        if (in.key == BSP_BTN_OK && in.event == BSP_BTN_LONG) {
            scan_cancelled = true;
            return false;
        }
    }
    esp_netif_ip_info_t current;
    return xc_wifi_ip(&current) && current.ip.addr == lan.ip.addr;
}

static void discover(void)
{
    if (!xc_wifi_ip(&lan)) return;
    const xc_location_t back = location();
    if (page != LIST) list_return = back;
    discard_network();
    // Leave the TCP fallback its full socket budget during discovery.
    xc_web_idle(false);
    progress = 0;
    scan_cancelled = false;
    go(SCAN);
    show();
    wifi_ps_type_t power_save;
    const bool restore_ps = esp_wifi_get_ps(&power_save) == ESP_OK && power_save != WIFI_PS_NONE &&
                            esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    count = xc_discover(&lan, found, scan_progress);
    if (restore_ps) (void)esp_wifi_set_ps(power_save);
    xQueueReset(inputs);
    if (scan_cancelled) {
        return_to(back);
        if (page == LIST && focus >= (unsigned)count + 2) focus = count + 1;
    } else go(LIST);
}

static void selection(int direction, unsigned items)
{
    if (direction > 0 && focus + 1 < items) ++focus;
    if (direction < 0 && focus) --focus;
}

static void pause_camera(void)
{
    discard_network();
    if (online && connected) xc_control_idle(&target, &login, true);
    xc_client_reset();
    connected = false;
    tally_time_us = 0;
    record_pending = XC_REC_UNKNOWN;
}

static void open_launcher(void)
{
    pause_camera();
    const bool from_ble = ble_mode;
    canon_ble_enable(false);
    ble_mode = false;
    go(LAUNCHER);
    focus = from_ble ? 1 : 0;
}

static void select_remote(bool bluetooth)
{
    if (bluetooth != ble_mode) {
        if (bluetooth) {
            pause_camera();
        } else {
            next_poll = esp_timer_get_time();
        }
        ble_mode = bluetooth;
        canon_ble_enable(bluetooth);
    }
    if (!bluetooth && !target.addr) restore_pending = true;
    go(bluetooth ? BLE_REMOTE : target.addr ? LIVE : HOME);
    if (!bluetooth) start_network();
    if (bluetooth && (ble_status == CANON_BLE_OFF || ble_status == CANON_BLE_UNPAIRED ||
                      ble_status == CANON_BLE_NOT_FOUND || ble_status == CANON_BLE_PAIR_FAILED ||
                      ble_status == CANON_BLE_UNAVAILABLE)) focus = CANON_BLE_PAIR;
}

static void key(input_t in)
{
    const int direction = in.key == BSP_BTN_UP ? -1 : in.key == BSP_BTN_DOWN ? 1 : 0;
    if (!direction) {
        const xc_ok_t gesture = in.event == BSP_BTN_CLICK ? XC_OK_SINGLE :
                                in.event == BSP_BTN_DOUBLE ? XC_OK_DOUBLE : XC_OK_LONG;
        switch (xc_navigation_ok(page, gesture)) {
        case XC_NAV_IGNORE: return;
        case XC_NAV_SETTINGS: open_settings(); return;
        case XC_NAV_LAUNCHER: open_launcher(); return;
        case XC_NAV_RECORD:
            if (connected && record_pending == XC_REC_UNKNOWN) {
                record_pending = xc_record_target(&camera);
                record_until = 0;
                record_notice = NULL;
                message = NULL;
                if (record_pending == XC_REC_UNKNOWN)
                    notify_record(camera.standby ? "请先唤醒相机" : "相机未回传录像状态", esp_timer_get_time());
            }
            return;
        case XC_NAV_BACK:
            switch (page) {
            case PRESETS:
                return_settings();
                save_presets();
                break;
            case STATUS:
            case FORGET: return_settings(); break;
            case DEVICE: return_to(device_return); break;
            case LIST: return_to(list_return); break;
            case SETUP:
                end_setup();
                return_to(setup_return);
                break;
            default: break;
            }
            return;
        case XC_NAV_ACTIVATE: break;
        }
    }
    message = NULL;
    switch (page) {
    case LAUNCHER:
        if (direction) selection(direction, 2);
        else select_remote(focus == 1);
        break;
    case HOME:
        if (direction) selection(direction, 4);
        else if (focus == 3) open_launcher();
        else if (!online && focus == 1) open_device();
        else if (!online && focus == 2) setup(XC_SETUP_DISPLAY);
        else if (!online) setup(XC_SETUP_WIFI);
        else if (focus == 0) discover();
        else if (focus == 1) setup(XC_SETUP_CAMERA);
        else open_settings();
        break;
    case LIST:
        if (direction) selection(direction, count + 2);
        else if (focus == (unsigned)count) discover();
        else if (focus > (unsigned)count) setup(XC_SETUP_CAMERA);
        else {
            target = found[focus];
            memset(&login, 0, sizeof(login));
            verify_camera = false;
            link_camera(true);
        }
        break;
    case LIVE:
        if (!direction) {
            if (connected) {
                preset = xc_next_preset(preset_mask, preset);
            } else if (online && auth_pending) setup(XC_SETUP_CAMERA);
            else if (online) link_camera(true);
        } else if (connected) {
            if (esp_timer_get_time() - in.pressed_us > 600000) break;
            xc_setting_t *s = &camera.settings[preset];
            if (!xc_edit_step(&edits[preset], s, direction < 0, in.pressed_us)) {
                message = s->writable ? "已到调节边界" : "当前模式不可调";
            } else {
                roll_direction = direction < 0 ? -1 : 1;
            }
        }
        break;
    case SETTINGS: {
        unsigned items;
        const xc_menu_item_t *rows = xc_settings_menu(ble_mode, target.addr != 0, &items);
        if (direction) selection(direction, items);
        else {
            settings_focus = focus;
            switch (rows[focus]) {
            case XC_MENU_RETURN: return_to(settings_return); break;
            case XC_MENU_MODE: open_launcher(); break;
            case XC_MENU_DEVICE: open_device(); break;
            case XC_MENU_PRESETS: go(PRESETS); break;
            case XC_MENU_STATUS: go(STATUS); break;
            case XC_MENU_CAMERA: setup(XC_SETUP_CAMERA); break;
            case XC_MENU_DISCONNECT:
                go(HOME);
                forget_camera();
                break;
            case XC_MENU_WIFI:
                start_network();
                setup(XC_SETUP_WIFI);
                break;
            case XC_MENU_FORGET: go(FORGET); break;
            }
        }
        break;
    }
    case BLE_REMOTE:
        if (direction) selection(direction, 4);
        else if (ble_status == CANON_BLE_UNPAIRED && focus != CANON_BLE_PAIR) {
            focus = CANON_BLE_PAIR;
            message = "请先配对相机";
        } else {
            message = NULL;
            canon_ble_action((canon_ble_action_t)focus);
        }
        break;
    case PRESETS:
        if (direction) selection(direction, 4);
        else if (focus == 3) {
            return_settings();
            save_presets();
        } else {
            const unsigned next = preset_mask ^ (1u << focus);
            if (!next) message = "至少保留一个调节项";
            else {
                preset_mask = next;
                if (!(next & (1u << preset))) preset = xc_next_preset(next, preset);
                presets_saved = false;
            }
        }
        break;
    case STATUS:
        if (!direction) open_device();
        break;
    case DEVICE:
        if (direction) selection(direction, 2);
        else return_to(device_return);
        break;
    case FORGET:
        if (direction) selection(direction, 2);
        else {
            const bool erase = focus == 1;
            return_settings();
            if (erase) {
                start_network();
                if (!network_ready) break;
                forget_camera();
                xc_wifi_forget();
                online = connected = joining = retry_wifi = false;
                restore_pending = false;
                settings_return = (xc_location_t){HOME, 0};
                focus = settings_focus = 0;
            }
        }
        break;
    default: break;
    }
}

static void button(bsp_btn_t key_id, bsp_btn_ev_t event, void *ctx)
{
    (void)ctx;
    if (key_id == BSP_BTN_OK || event == BSP_BTN_PRESS) {
        const raw_key_t raw = {key_id, event};
        xQueueSend(raw_keys, &raw, 0);
    }
}

static void run_screen(void *arg)
{
    (void)arg;
    xc_screen_t screen = {.activity_us = esp_timer_get_time()};
    unsigned shown = XC_SCREEN_NORMAL;
    bool setup = false, ignore_ok = false;
    int64_t ok_release = 0;
    for (;;) {
        raw_key_t raw;
        const bool have_key = xQueueReceive(raw_keys, &raw, pdMS_TO_TICKS(100)) == pdTRUE;
        const int64_t now = esp_timer_get_time();
        const bool keep = atomic_load(&keep_screen_on);
        if (setup != keep) screen = (xc_screen_t){.activity_us = now};
        setup = keep;
        bool dispatch = false;
        if (have_key && raw.event == BSP_BTN_PRESS) {
            // The first press from black only wakes the display, including any click it emits later.
            const bool wake = xc_screen_press(&screen, now, setup) || shown == 0;
            if (raw.key == BSP_BTN_OK) {
                ignore_ok = wake || (ignore_ok && now - ok_release <= BSP_BTN_SHORT_PRESS_MS * 1000);
            } else {
                dispatch = !wake;
            }
        } else if (have_key && raw.event == BSP_BTN_RELEASE) {
            ok_release = now;
        } else if (have_key) {
            if (!ignore_ok) dispatch = raw.event == BSP_BTN_CLICK || raw.event == BSP_BTN_DOUBLE || raw.event == BSP_BTN_LONG;
            ignore_ok = false;
        }
        const unsigned level = xc_screen_level(&screen, now, setup);
        if (level != shown) {
            shown = level;
            bsp_display_backlight((uint8_t)level);
            atomic_store(&screen_awake, level != 0);
        }
        if (dispatch) {
            const input_t in = {.key = raw.key, .event = raw.event, .pressed_us = now};
            xQueueSend(inputs, &in, 0);
        }
    }
}

static void start_join(const xc_wifi_login_t *wifi)
{
    esp_err_t r = xc_wifi_connect(wifi);
    joining = retry_wifi = r == ESP_OK;
    const int64_t now = esp_timer_get_time();
    deadline = now + 15000000;
    next_wifi = now + 5000000;
    if (r != ESP_OK && r != ESP_ERR_NOT_FOUND) message = "Wi-Fi 连接失败";
}

static void run(void *arg)
{
    (void)arg;
    esp_err_t err = nvs_flash_init();
    storage_status = err;
    bool title_dirty = err == ESP_OK;
    if (err == ESP_OK) {
        nvs_handle_t nvs;
        uint8_t mask;
        if (nvs_open("xc_remote", NVS_READONLY, &nvs) == ESP_OK) {
            if (nvs_get_u8(nvs, "presets", &mask) == ESP_OK && mask && mask < 8) preset_mask = mask;
            size_t size = sizeof(remembered);
            if (nvs_get_blob(nvs, "camera_v1", &remembered, &size) != ESP_OK ||
                size != sizeof(remembered) || !remembered.port || !remembered.ssid[0] || !remembered.model[0] ||
                !memchr(remembered.ssid, 0, sizeof(remembered.ssid)) ||
                !memchr(remembered.model, 0, sizeof(remembered.model)) ||
                !memchr(remembered.address, 0, sizeof(remembered.address))) remembered.addr = 0;
            nvs_close(nvs);
        }
        preset = xc_next_preset(preset_mask, XC_FIELDS - 1);
        presets_saved = true;
    } else {
        ESP_LOGE("xc", "Storage init: %s", esp_err_to_name(err));
        message = "设置存储初始化失败";
    }
    show();
    bool display_awake = atomic_load(&screen_awake);
    int64_t next_web = 0;
    for (;;) {
        input_t in;
        bool redraw = false;
        const bool have_key = xQueueReceive(inputs, &in, pdMS_TO_TICKS(20)) == pdTRUE;
        const xc_page_t input_page = page;
        int64_t now = esp_timer_get_time();
        const bool awake = atomic_load(&screen_awake);
        if (display_awake != awake) {
            display_awake = awake;
            redraw = page == LIVE;
        }
        if (xc_setup_title_changed()) title_dirty = true;
        if (title_dirty) {
            char value[XC_TITLE_SIZE];
            xc_setup_title_load(value);
            if (bsp_lvgl_lock(100)) {
                xc_ui_title(xc_ui_title_fits(value) ? value : "");
                bsp_lvgl_unlock();
                title_dirty = false;
            }
        }
        if (network_ready) {
            esp_netif_ip_info_t current;
            const bool available = xc_wifi_ip(&current);
            if (available != online || (available && current.ip.addr != lan.ip.addr)) {
                online = available;
                connected = false;
                auth_pending = false;
                broad_tally = false;
                ++net_link;
                memset(edits, 0, sizeof(edits));
                record_pending = XC_REC_UNKNOWN;
                record_notice = NULL;
                next_idle = 0;
                if (!net_busy) xc_client_reset();
                if (online) {
                    lan = current;
                    joining = false;
                    if (!ble_mode && page != LAUNCHER) message = NULL;
                    if (page == LIST) go(HOME);
                } else {
                    if (page == HOME) focus = 0;
                    if (!ble_mode && page != LAUNCHER) message = "Wi-Fi 已断开";
                }
                if (page == SETUP && setup_lan) {
                    end_setup();
                    return_to(setup_return);
                    message = "网络变化，请重新打开设置";
                }
                next_wifi = now;
                next_poll = now;
                redraw = true;
            }
            if (available) lan = current;
            if (page == SETUP) {
                xc_wifi_login_t wifi;
                xc_endpoint_t endpoint;
                xc_credentials_t credentials;
                if (setup_mode == XC_SETUP_WIFI && xc_setup_wifi(&wifi)) {
                    end_setup();
                    return_to(setup_return);
                    online = connected = false;
                    restore_pending = true;
                    start_join(&wifi);
                    memset(&wifi, 0, sizeof(wifi));
                    redraw = true;
                } else if (setup_mode == XC_SETUP_CAMERA && xc_setup_camera(&endpoint, &credentials)) {
                    end_setup();
                    verify_camera = verify_camera && endpoint.addr == target.addr && endpoint.port == target.port;
                    target = endpoint;
                    login = credentials;
                    memset(&credentials, 0, sizeof(credentials));
                    link_camera(true);
                    redraw = true;
                } else if (now >= deadline) {
                    end_setup();
                    return_to(setup_return);
                    message = "设置已超时";
                    redraw = true;
                }
            } else if (!online && retry_wifi) {
                if (joining && now >= deadline) {
                    joining = false;
                    if (!ble_mode && page != LAUNCHER) message = "检查 Wi-Fi 名称及密码";
                    redraw = true;
                }
                if (now >= next_wifi) {
                    (void)xc_wifi_connect(NULL);
                    next_wifi = now + 5000000;
                }
            }
        }
        if (have_key && input_page == page) {
            key(in);
            show();
            redraw = false;
        }
        redraw |= take_network();
        if (!ble_mode && online && restore_pending && !target.addr && page == HOME) {
            restore_pending = false;
            redraw |= restore_camera();
        }
        now = esp_timer_get_time();
        if (network_ready && page != SETUP && now >= next_web) {
            xc_web_idle(online);
            next_web = now + 1000000;
        }
        if (page != LAUNCHER && !ble_mode && !net_busy && (page != SETUP || setup_lan) && target.addr && online) {
            const bool fast = atomic_load(&screen_awake) && !broad_tally;
            unsigned field = 0;
            while (field < XC_FIELDS && !xc_edit_due(&edits[field], now)) ++field;
            if (connected && record_pending != XC_REC_UNKNOWN && !record_until) {
                queue_network(NET_RECORD, 0);
            } else if (fast && connected && now >= next_tally && now - tally_time_us >= 400000 &&
                now < next_poll + 350000) {
                queue_network(NET_TALLY, 0);
            } else if (now >= next_poll + 350000) {
                queue_network(NET_READ, 0);
            } else if (connected && field < XC_FIELDS) {
                queue_network(NET_WRITE, field);
            } else if (fast && connected && now >= next_tally && now < next_poll) {
                queue_network(NET_TALLY, 0);
            } else if (now >= next_poll) {
                queue_network(NET_READ, 0);
            } else if (connected && next_idle && now >= next_idle) {
                queue_network(NET_IDLE, 0);
                next_idle = 0;
            }
        }
        if (page == DEVICE && now >= next_device) {
            next_device = now + 2000000;
            redraw = true;
        }
        if (record_notice && now >= record_notice_until) {
            record_notice = NULL;
            redraw |= page == LIVE;
        }
        const canon_ble_state_t next_ble = canon_ble_state();
        if (next_ble != ble_status) {
            if (page == BLE_REMOTE && next_ble == CANON_BLE_UNPAIRED)
                focus = CANON_BLE_PAIR;
            else if (page == BLE_REMOTE && next_ble == CANON_BLE_READY && focus == CANON_BLE_PAIR &&
                     (ble_status == CANON_BLE_VERIFYING || ble_status == CANON_BLE_PAIRING ||
                      ble_status == CANON_BLE_CONNECTING))
                focus = CANON_BLE_PHOTO;
            ble_status = next_ble;
            redraw |= page == BLE_REMOTE;
        }
        if (redraw) show();
    }
}

static void sample_battery(void *arg)
{
    (void)arg;
    esp_err_t err = bsp_battery_init();
    for (;;) {
        if (err == ESP_OK || err == ESP_ERR_TIMEOUT) {
            // After a soft initialization timeout, valid readings can arrive later.
            const int mv = bsp_battery_mv();
            const int soc = mv >= 2500 ? bsp_battery_soc() : -1;
            if (bsp_lvgl_lock(100)) {
                xc_ui_power(soc, mv >= 2500 ? mv : -1);
                bsp_lvgl_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10000));
            err = bsp_battery_init();
        }
    }
}

void app_main(void)
{
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init() || !bsp_lvgl_lock(1000)) {
        ESP_LOGE("xc", "Display initialization failed");
        return;
    }
    xc_ui_init();
    bsp_lvgl_unlock();
    bsp_display_backlight(XC_SCREEN_NORMAL);
    if (xTaskCreate(sample_battery, "xc_battery", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGW("xc", "Battery sampler unavailable");
    inputs = xQueueCreate(16, sizeof(input_t));
    raw_keys = xQueueCreate(12, sizeof(raw_key_t));
    net_jobs = xQueueCreate(1, sizeof(net_job_t));
    net_replies = xQueueCreate(1, sizeof(net_reply_t));
    if (!inputs || !raw_keys || !net_jobs || !net_replies || bsp_button_init(button, NULL) != ESP_OK ||
        xTaskCreate(network_worker, "xc_network", 4096, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(run_screen, "xc_screen", 2048, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(run, "xc_remote", 6144, NULL, 4, NULL) != pdPASS) {
        message = "按键或任务初始化失败";
        show();
    }
}
