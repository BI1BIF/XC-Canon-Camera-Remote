#include "xc_wifi.h"
#include "xc_ui.h"

#include "bsp_display.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static esp_netif_t *sta, *ap;
static httpd_handle_t setup_server;
static QueueHandle_t submissions;
static atomic_bool has_ip;
static atomic_bool title_changed;
static xc_setup_mode_t setup_mode;
static xc_endpoint_t form_target;
static char form_nonce[17];
static char form_code[9];
static bool form_lan;
static unsigned bad_codes;

typedef union {
    xc_wifi_login_t wifi;
    struct {
        xc_endpoint_t endpoint;
        xc_credentials_t login;
    } camera;
} submission_t;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&has_ip, true);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&has_ip, false);
    }
}

esp_err_t xc_wifi_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    if ((err = esp_event_loop_create_default()) != ESP_OK) return err;
    sta = esp_netif_create_default_wifi_sta();
    ap = esp_netif_create_default_wifi_ap();
    if (!sta || !ap) return ESP_ERR_NO_MEM;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK ||
        (err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) return err;
    submissions = xQueueCreate(1, sizeof(submission_t));
    return submissions ? ESP_OK : ESP_ERR_NO_MEM;
}

bool xc_wifi_ip(esp_netif_ip_info_t *ip)
{
    return atomic_load(&has_ip) && esp_netif_get_ip_info(sta, ip) == ESP_OK && ip->ip.addr;
}

void xc_wifi_ssid(char out[33])
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) memcpy(out, ap.ssid, 32);
    else out[0] = 0;
    out[32] = 0;
}

bool xc_wifi_signal(int *rssi, unsigned *channel)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    *rssi = ap.rssi;
    *channel = ap.primary;
    return true;
}

esp_err_t xc_wifi_connect(const xc_wifi_login_t *login)
{
    wifi_config_t cfg = {0};
    if (!login) {
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK || !cfg.sta.ssid[0]) {
            memset(&cfg, 0, sizeof(cfg));
            return ESP_ERR_NOT_FOUND;
        }
    } else {
        atomic_store(&has_ip, false);
        (void)esp_wifi_disconnect();
        memcpy(cfg.sta.ssid, login->ssid, strlen(login->ssid));
        memcpy(cfg.sta.password, login->password, strlen(login->password));
        cfg.sta.threshold.authmode = login->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable = true;
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) {
            memset(&cfg, 0, sizeof(cfg));
            return err;
        }
    }
    memset(&cfg, 0, sizeof(cfg));
    return esp_wifi_connect();
}

void xc_wifi_forget(void)
{
    atomic_store(&has_ip, false);
    (void)esp_wifi_disconnect();
    wifi_config_t empty = {0};
    (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
}

static bool setup_request(httpd_req_t *req)
{
    struct sockaddr_storage local, peer;
    socklen_t size = sizeof(local), peer_size = sizeof(peer);
    esp_netif_ip_info_t info;
    char host[32], ip[INET_ADDRSTRLEN];
    const int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &size) != 0 ||
        (form_lan ? !xc_wifi_ip(&info) : esp_netif_get_ip_info(ap, &info) != ESP_OK) ||
        !inet_ntop(AF_INET, &info.ip.addr, ip, sizeof(ip)) ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    if (!xc_portal_host_matches(host, ip)) return false;
    if (local.ss_family == AF_INET && ((struct sockaddr_in *)&local)->sin_addr.s_addr == info.ip.addr) {
        if (!form_lan) return true;
    } else if (local.ss_family == AF_INET6 &&
               IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)&local)->sin6_addr) &&
               !memcmp(&((struct sockaddr_in6 *)&local)->sin6_addr.s6_addr[12], &info.ip.addr, 4)) {
        if (!form_lan) return true;
    } else return false;
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_size) != 0) return false;
    uint32_t source;
    if (peer.ss_family == AF_INET) source = ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
    else if (peer.ss_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)&peer)->sin6_addr))
        memcpy(&source, &((struct sockaddr_in6 *)&peer)->sin6_addr.s6_addr[12], sizeof(source));
    else return false;
    return !((source ^ info.ip.addr) & info.netmask.addr);
}

void xc_setup_title_load(char title[XC_TITLE_SIZE])
{
    title[0] = 0;
    nvs_handle_t nvs;
    if (nvs_open("xc_remote", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = XC_TITLE_SIZE;
    if (nvs_get_str(nvs, "title", title, &size) != ESP_OK || !xc_title_normalize(title)) title[0] = 0;
    nvs_close(nvs);
}

bool xc_setup_title_changed(void)
{
    return atomic_exchange(&title_changed, false);
}

static bool send_chunk(void *req, const char *str)
{
    return httpd_resp_sendstr_chunk(req, str) == ESP_OK;
}

static xc_setup_mode_t request_mode(const char *uri)
{
    if (form_lan) return xc_portal_lan_route(setup_mode, uri);
    return !strcmp(uri, "/display") ? XC_SETUP_DISPLAY : setup_mode;
}

static esp_err_t render(httpd_req_t *req, xc_setup_mode_t view, bool submitted, const char *error)
{
    char target[INET_ADDRSTRLEN] = "", title[XC_TITLE_SIZE] = "";
    const bool display = view == XC_SETUP_DISPLAY;
    if (display) xc_setup_title_load(title);
    if (view == XC_SETUP_CAMERA && form_target.addr) inet_ntop(AF_INET, &form_target.addr, target, sizeof(target));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (error) httpd_resp_set_status(req, "400 Bad Request");
    const xc_portal_t page = {.mode = !form_lan && display ? setup_mode : view,
                              .display = display, .submitted = submitted, .lan = form_lan,
                              .nonce = form_nonce, .ip = target, .title = title,
                              .port = form_target.port ? form_target.port : 80, .error = error};
    if (!xc_portal_render(&page, send_chunk, req)) return ESP_FAIL;
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t setup_get(httpd_req_t *req)
{
    if (!setup_request(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    return render(req, request_mode(req->uri), false, NULL);
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool decode(const char *form, char key, char *out, size_t cap)
{
    out[0] = 0;
    while (*form) {
        const char *end = form + strcspn(form, "&");
        if (end - form >= 2 && form[0] == key && form[1] == '=') {
            size_t n = 0;
            for (const char *p = form + 2; p < end; ++p) {
                unsigned char c = *p;
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (end - p < 3 || hex(p[1]) < 0 || hex(p[2]) < 0) return false;
                    c = (hex(p[1]) << 4) | hex(p[2]);
                    p += 2;
                }
                if (c < 32 || c == 127 || n + 1 >= cap) return false;
                out[n++] = c;
            }
            out[n] = 0;
            return true;
        }
        form = *end ? end + 1 : end;
    }
    return false;
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (!setup_request(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    const xc_setup_mode_t view = request_mode(req->uri);
    if (view == XC_SETUP_HOME) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open the matching settings menu on Passport");
    char form[768], nonce[17];
    if (!req->content_len || req->content_len >= sizeof(form))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form too long");
    size_t n = 0;
    while (n < req->content_len) {
        const int read = httpd_req_recv(req, form + n, req->content_len - n);
        if (read <= 0) return ESP_FAIL;
        n += read;
    }
    form[n] = 0;
    submission_t s = {0};
    bool valid = decode(form, 'k', nonce, sizeof(nonce)) && !strcmp(nonce, form_nonce);
    if (!valid) return render(req, view, false, "页面已过期，请重新打开设备设置页。");
    if (form_lan && view == XC_SETUP_CAMERA) {
        char code[9];
        if (bad_codes >= 8) return render(req, view, false, "尝试次数过多，请在设备上退出并重新打开设置。");
        if (!decode(form, 'a', code, sizeof(code)) || strcasecmp(code, form_code)) {
            ++bad_codes;
            return render(req, view, false, "验证码不正确，请核对 Passport 屏幕。");
        }
    }
    if (view == XC_SETUP_DISPLAY) {
        char title[XC_TITLE_SIZE];
        if (!decode(form, 'n', title, sizeof(title)) || !xc_title_normalize(title))
            return render(req, view, false, "请使用页面列出的字符，最多输入 12 个字符。");
        if (!bsp_lvgl_lock(200)) return render(req, view, false, "屏幕正忙，请重新提交。");
        const bool fits = xc_ui_title_fits(title);
        bsp_lvgl_unlock();
        if (!fits) return render(req, view, false, "部分字无法显示或标题太宽，请换字或缩短标题。");
        nvs_handle_t nvs;
        esp_err_t err = nvs_open("xc_remote", NVS_READWRITE, &nvs);
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, "title", title);
            if (err == ESP_OK) err = nvs_commit(nvs);
            nvs_close(nvs);
        }
        if (err != ESP_OK) return render(req, view, false, "保存失败，请重新提交。");
        atomic_store(&title_changed, true);
        return render(req, view, true, NULL);
    }
    if (view == XC_SETUP_CAMERA) {
        char ip[INET_ADDRSTRLEN], port[6];
        xc_endpoint_t *ep = &s.camera.endpoint;
        xc_credentials_t *login = &s.camera.login;
        valid = decode(form, 'i', ip, sizeof(ip)) && inet_pton(AF_INET, ip, &ep->addr) == 1 &&
                decode(form, 't', port, sizeof(port)) && port[0] &&
                strspn(port, "0123456789") == strlen(port) &&
                decode(form, 'u', login->user, sizeof(login->user)) &&
                decode(form, 'p', login->password, sizeof(login->password)) &&
                !strchr(login->user, ':') && (login->user[0] || !login->password[0]);
        if (valid) {
            const unsigned p = strtoul(port, NULL, 10);
            const unsigned top = ntohl(ep->addr) >> 24;
            valid = p && p <= UINT16_MAX && top && top != 127 && top < 224;
            ep->port = (uint16_t)p;
        }
    } else {
        valid = decode(form, 's', s.wifi.ssid, sizeof(s.wifi.ssid)) && s.wifi.ssid[0] &&
                decode(form, 'p', s.wifi.password, sizeof(s.wifi.password));
        const size_t len = strlen(s.wifi.password);
        valid = valid && (!len || (len >= 8 && len <= 63) ||
                 (len == 64 && strspn(s.wifi.password, "0123456789abcdefABCDEF") == 64));
    }
    memset(form, 0, sizeof(form));
    if (!valid) {
        memset(&s, 0, sizeof(s));
        return render(req, view, false, view == XC_SETUP_CAMERA ? "请检查相机地址、端口和账号。"
                     : "请检查网络名称。密码应为 8 至 63 字节，开放网络请留空。");
    }
    esp_err_t result = render(req, view, true, NULL);
    xQueueOverwrite(submissions, &s);
    memset(&s, 0, sizeof(s));
    return result;
}

esp_err_t xc_setup_start(xc_setup_mode_t mode, const xc_endpoint_t *target, bool lan, char password[11])
{
    xc_setup_stop();
    setup_mode = mode;
    form_lan = lan;
    bad_codes = 0;
    form_target = target ? *target : (xc_endpoint_t){0};
    esp_err_t err = ESP_OK;
    if (lan) {
        if (mode == XC_SETUP_CAMERA) {
            snprintf(form_code, sizeof(form_code), "%08lx", (unsigned long)esp_random());
            strcpy(password, form_code);
        }
    } else {
        snprintf(password, 11, "%08lx%02lx", (unsigned long)esp_random(), (unsigned long)(esp_random() & 255));
        wifi_config_t cfg = {.ap = {.ssid = XC_SETUP_SSID, .channel = 1, .max_connection = 1,
                                    .authmode = WIFI_AUTH_WPA2_PSK, .pmf_cfg = {.capable = true}}};
        memcpy(cfg.ap.password, password, strlen(password));
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
        memset(&cfg, 0, sizeof(cfg));
    }
    snprintf(form_nonce, sizeof(form_nonce), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.max_open_sockets = 2;
    http.lru_purge_enable = true;
    http.recv_wait_timeout = 3;
    http.send_wait_timeout = 3;
    if (err == ESP_OK) err = httpd_start(&setup_server, &http);
    if (err == ESP_OK) {
        const httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = setup_get};
        const httpd_uri_t post = {.uri = "/", .method = HTTP_POST, .handler = setup_post};
        const httpd_uri_t display_get = {.uri = "/display", .method = HTTP_GET, .handler = setup_get};
        const httpd_uri_t display_post = {.uri = "/display", .method = HTTP_POST, .handler = setup_post};
        const httpd_uri_t camera_get = {.uri = "/camera", .method = HTTP_GET, .handler = setup_get};
        const httpd_uri_t camera_post = {.uri = "/camera", .method = HTTP_POST, .handler = setup_post};
        err = httpd_register_uri_handler(setup_server, &get);
        if (err == ESP_OK) err = httpd_register_uri_handler(setup_server, &post);
        if (err == ESP_OK) err = httpd_register_uri_handler(setup_server, &display_get);
        if (err == ESP_OK) err = httpd_register_uri_handler(setup_server, &display_post);
        if (err == ESP_OK && lan) err = httpd_register_uri_handler(setup_server, &camera_get);
        if (err == ESP_OK && lan) err = httpd_register_uri_handler(setup_server, &camera_post);
    }
    if (err != ESP_OK) xc_setup_stop();
    return err;
}

void xc_setup_stop(void)
{
    if (setup_server) {
        (void)httpd_stop(setup_server);
        setup_server = NULL;
    }
    memset(form_code, 0, sizeof(form_code));
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_APSTA)
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
    if (submissions) {
        submission_t blank = {0};
        xQueueOverwrite(submissions, &blank);
        xQueueReset(submissions);
    }
}

void xc_web_idle(bool online)
{
    if (online && !setup_server) {
        const esp_err_t err = xc_setup_start(XC_SETUP_HOME, NULL, true, NULL);
        if (err != ESP_OK) ESP_LOGW("xc", "LAN settings server: %s", esp_err_to_name(err));
    } else if (!online && setup_server) xc_setup_stop();
}

bool xc_setup_wifi(xc_wifi_login_t *login)
{
    submission_t s;
    if (setup_mode != XC_SETUP_WIFI || xQueueReceive(submissions, &s, 0) != pdTRUE) return false;
    *login = s.wifi;
    memset(&s, 0, sizeof(s));
    return true;
}

bool xc_setup_camera(xc_endpoint_t *target, xc_credentials_t *login)
{
    submission_t s;
    if (setup_mode != XC_SETUP_CAMERA || xQueueReceive(submissions, &s, 0) != pdTRUE) return false;
    *target = s.camera.endpoint;
    *login = s.camera.login;
    memset(&s, 0, sizeof(s));
    return true;
}
