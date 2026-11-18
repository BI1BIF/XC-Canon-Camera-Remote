#include "xc_client.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// One application worker owns the client and its bounded response buffer.
static char body[8192];
static size_t length;
static int livescope;
static bool overflow;
static esp_http_client_handle_t http;
static xc_endpoint_t http_target;
static xc_credentials_t http_login;
static char sid[48];
static bool session_required;
static int64_t session_idle_at;

static void close_http(void)
{
    if (http) esp_http_client_cleanup(http);
    http = NULL;
}

void xc_client_reset(void)
{
    close_http();
    memset(&http_login, 0, sizeof(http_login));
    sid[0] = 0;
    session_required = false;
    session_idle_at = 0;
}

static esp_err_t on_http(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(ev->header_key, "Livescope-Status")) {
        char *end;
        const long value = strtol(ev->header_value, &end, 10);
        if (end != ev->header_value && !*end && value >= 0 && value <= 999) livescope = value;
    } else if (ev->event_id == HTTP_EVENT_ON_DATA &&
               esp_http_client_get_status_code(ev->client) == 200) {
        if ((size_t)ev->data_len >= sizeof(body) - length) overflow = true;
        if (!overflow) {
            memcpy(body + length, ev->data, ev->data_len);
            length += ev->data_len;
            body[length] = 0;
        }
    }
    return ESP_OK;
}

static xc_result_t request(const xc_endpoint_t *ep, const xc_credentials_t *login,
                           const char *path, int timeout_ms)
{
    length = 0;
    body[0] = 0;
    overflow = false;
    livescope = -1;
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &ep->addr, ip, sizeof(ip))) return XC_WRONG_DEVICE;
    const bool authenticated = login && login->user[0];
    char url[640];
    int written = snprintf(url, sizeof(url), "http://%s:%u%s", ip, ep->port, path);
    if (written < 0 || (size_t)written >= sizeof(url)) return XC_WRONG_DEVICE;
    if (http && (http_target.addr != ep->addr || http_target.port != ep->port ||
                 strcmp(http_login.user, authenticated ? login->user : "") ||
                 strcmp(http_login.password, authenticated ? login->password : ""))) xc_client_reset();
    esp_http_client_config_t cfg = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .username = authenticated ? login->user : NULL,
        .password = authenticated ? login->password : NULL,
        .event_handler = on_http, .timeout_ms = timeout_ms,
        .disable_auto_redirect = true,
        .max_authorization_retries = authenticated ? 1 : -1,
        .buffer_size = 512, .buffer_size_tx = 1024,
    };
    if (!http) {
        http = esp_http_client_init(&cfg);
        http_target = *ep;
        if (authenticated) http_login = *login;
        else memset(&http_login, 0, sizeof(http_login));
    } else if (esp_http_client_set_url(http, url) != ESP_OK ||
               esp_http_client_set_timeout_ms(http, timeout_ms) != ESP_OK) {
        close_http();
        return XC_NO_LINK;
    }
    if (!http) return XC_NO_LINK;
    const esp_err_t err = esp_http_client_perform(http);
    const int status = esp_http_client_get_status_code(http);
    if (status == 401 || status == 403 || livescope == 507) {
        close_http();
        return XC_AUTH;
    }
    if (err != ESP_OK) {
        close_http();
        return XC_NO_LINK;
    }
    if (status != 200 || overflow) return XC_WRONG_DEVICE;
    if (livescope > 0) return XC_DENIED;
    return XC_OK;
}

xc_result_t xc_read(const xc_endpoint_t *ep, const xc_credentials_t *login, xc_camera_t *state)
{
    xc_result_t result = request(ep, login, XC_INFO, 900);
    if (result == XC_OK && !xc_parse(body, state)) return XC_WRONG_DEVICE;
    return result;
}

xc_result_t xc_read_tally(const xc_endpoint_t *ep, const xc_credentials_t *login, xc_tally_t *tally)
{
    const xc_result_t r = request(ep, login, XC_TALLY_INFO, 250);
    if (r != XC_OK) return r;
    *tally = xc_parse_tally(body);
    return *tally == XC_UNKNOWN ? XC_WRONG_DEVICE : XC_OK;
}

void xc_control_idle(const xc_endpoint_t *ep, const xc_credentials_t *login, bool release)
{
    if (!sid[0] || (!release && esp_timer_get_time() < session_idle_at)) return;
    char path[96];
    snprintf(path, sizeof(path), XC_ROOT "close.cgi?s=%s", sid);
    sid[0] = 0;
    (void)request(ep, login, path, 160);
}

static xc_result_t open_control(const xc_endpoint_t *ep, const xc_credentials_t *login)
{
    // Ask for normal priority and verify that the camera grants control.
    char path[96], claim[48], session[sizeof(sid)];
    xc_result_t r = request(ep, login, XC_ROOT "open.cgi?priority=0&v=null", 450);
    if (r != XC_OK) return r;
    if (!xc_value(body, "s", session, sizeof(session)) ||
        strspn(session, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.~") != strlen(session))
        return XC_DENIED;
    snprintf(path, sizeof(path), XC_ROOT "claim.cgi?s=%s", session);
    r = request(ep, login, path, 450);
    strcpy(sid, session);
    if (r == XC_OK && xc_value(body, "s.control", claim, sizeof(claim)) &&
        (!strcmp(claim, "enabled") || !strncmp(claim, "enabled:", 8))) return XC_OK;
    xc_control_idle(ep, login, true);
    return r == XC_OK ? XC_DENIED : r;
}

static xc_result_t control(const xc_endpoint_t *ep, const xc_credentials_t *login,
                           const char *key, const char *value)
{
    char path[160];
    if (!session_required) {
        snprintf(path, sizeof(path), XC_CONTROL "%s=%s", key, value);
        const xc_result_t r = request(ep, login, path, 450);
        if (r != XC_DENIED || (livescope != 301 && livescope != 406))
            return r;
        session_required = true;
    }
    if (!sid[0]) {
        const xc_result_t r = open_control(ep, login);
        if (r != XC_OK) return r;
    }
    snprintf(path, sizeof(path), XC_CONTROL "s=%s&%s=%s", sid, key, value);
    const xc_result_t r = request(ep, login, path, 450);
    session_idle_at = esp_timer_get_time() + (r == XC_OK ? 900000 : 0);
    return r;
}

xc_result_t xc_adjust(const xc_endpoint_t *ep, const xc_credentials_t *login,
                      const char *key, int value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    const xc_result_t r = control(ep, login, key, text);
    return r == XC_OK && !xc_write_applied(body, key, value) ? XC_DENIED : r;
}

xc_result_t xc_set_record(const xc_endpoint_t *ep, const xc_credentials_t *login, xc_record_t target)
{
    const char *value = xc_record_value(target);
    if (!value) return XC_DENIED;
    const xc_result_t r = control(ep, login, XC_RECORD_KEY, value);
    return r == XC_OK && !xc_record_acknowledged(body, target) ? XC_DENIED : r;
}

static void probe(xc_endpoint_t ep, xc_endpoint_t *found, int *count, bool announced)
{
    if (*count == XC_MAX_CAMERAS) return;
    for (int i = 0; i < *count; ++i)
        if (ep.addr == found[i].addr && ep.port == found[i].port) return;
    // Discovery candidates use one fresh HTTP connection each; keep reuse for active control.
    close_http();
    xc_result_t r = request(&ep, NULL, XC_ID, 900);
    close_http();
    if (r == XC_OK && xc_is_identity(body, ep.name, sizeof(ep.name))) {
        found[(*count)++] = ep;
    } else if (r == XC_AUTH && (announced || livescope >= 0)) {
        strcpy(ep.name, "XC (login)");
        found[(*count)++] = ep;
    }
}

static unsigned discovery_mdns(const esp_netif_ip_info_t *lan, xc_endpoint_t *found, int *count)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    const struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = lan->ip.addr};
    const struct timeval timeout = {.tv_usec = 70000};
    const struct sockaddr_in group = {.sin_family = AF_INET, .sin_port = htons(5353),
                                      .sin_addr.s_addr = htonl(0xe00000fbu)};
    // _http._tcp.local PTR, request a unicast reply to this temporary socket.
    static const uint8_t query[] = {0,0,0,0,0,1,0,0,0,0,0,0,
        5,'_','h','t','t','p',4,'_','t','c','p',5,'l','o','c','a','l',0,0,12,0x80,1};
    if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &local.sin_addr, sizeof(local.sin_addr)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        sendto(fd, query, sizeof(query), 0, (const struct sockaddr *)&group, sizeof(group)) < 0) {
        close(fd);
        return 0;
    }
    xc_endpoint_t candidates[XC_MAX_CAMERAS];
    unsigned n = 0;
    const int64_t deadline = esp_timer_get_time() + 650000;
    while (n < XC_MAX_CAMERAS && esp_timer_get_time() < deadline) {
        uint8_t packet[1024];
        struct sockaddr_in src;
        socklen_t size = sizeof(src);
        const int bytes = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&src, &size);
        if (bytes <= 0 || src.sin_port != htons(5353) || src.sin_addr.s_addr == lan->ip.addr ||
            ((src.sin_addr.s_addr ^ lan->ip.addr) & lan->netmask.addr)) continue;
        const int port = xc_mdns_port(packet, (size_t)bytes);
        if (!port) continue;
        unsigned i = 0;
        while (i < n && (candidates[i].addr != src.sin_addr.s_addr || candidates[i].port != port)) ++i;
        if (i == n) candidates[n++] = (xc_endpoint_t){.addr = src.sin_addr.s_addr, .port = (uint16_t)port};
    }
    close(fd);
    for (unsigned i = 0; i < n; ++i) probe(candidates[i], found, count, true);
    return n;
}

static void discovery_broadcast(const esp_netif_ip_info_t *lan, xc_endpoint_t *found, int *count)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    const int enabled = 1;
    struct timeval timeout = {.tv_usec = 70000};
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(52220),
                              .sin_addr.s_addr = lan->ip.addr | ~lan->netmask.addr};
    const char query[] = "MV-SEARCH * HTTPU\r\nST: VB\r\n";
    sendto(fd, query, sizeof(query) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (dst.sin_addr.s_addr != INADDR_BROADCAST) {
        dst.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(fd, query, sizeof(query) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
    }
    xc_endpoint_t candidates[XC_MAX_CAMERAS] = {0};
    int n = 0;
    const int64_t deadline = esp_timer_get_time() + 700000;
    while (n < XC_MAX_CAMERAS && esp_timer_get_time() < deadline) {
        char packet[512];
        struct sockaddr_in src;
        socklen_t len = sizeof(src);
        const int bytes = recvfrom(fd, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&src, &len);
        if (bytes <= 0) continue;
        packet[bytes] = 0;
        const int port = xc_discovery_port(packet);
        if (!port || src.sin_addr.s_addr == lan->ip.addr ||
            ((src.sin_addr.s_addr ^ lan->ip.addr) & lan->netmask.addr)) continue;
        bool duplicate = false;
        for (int i = 0; i < n; ++i)
            duplicate |= candidates[i].addr == src.sin_addr.s_addr && candidates[i].port == port;
        if (!duplicate)
            candidates[n++] = (xc_endpoint_t){.addr = src.sin_addr.s_addr, .port = port};
    }
    close(fd);
    for (int i = 0; i < n; ++i) probe(candidates[i], found, count, true);
}

int xc_discover(const esp_netif_ip_info_t *lan, xc_endpoint_t *found,
                bool (*progress)(unsigned percent))
{
    int count = 0;
    xc_client_reset();
    const unsigned mdns = discovery_mdns(lan, found, &count);
    ESP_LOGI("xc", "Discovery mDNS: %u Canon replies, %d XC matches", mdns, count);
    if (!progress(5)) return count;
    discovery_broadcast(lan, found, &count);
    if (!progress(10)) return count;
    // shortcut: Probe HTTP/80 only within the local /24 on larger LANs.
    const uint32_t local = ntohl(lan->ip.addr), mask = ntohl(lan->netmask.addr) | 0xffffff00u;
    const uint32_t first = local & mask, hosts = ~mask;
    if (hosts < 2) return count;
    // Keep a socket available for HTTP and stay below the default lwIP socket cap.
    enum { BATCH = 8 };
    unsigned tcp_ready = 0;
    for (uint32_t start = 1; start < hosts && count < XC_MAX_CAMERAS; start += BATCH) {
        if (!progress(10 + 89 * start / hosts)) return count;
        int fds[BATCH], highest = -1;
        fd_set writable;
        FD_ZERO(&writable);
        for (unsigned i = 0; i < BATCH; ++i) {
            fds[i] = -1;
            const uint32_t ip = first + start + i;
            if (start + i >= hosts || ip == local) continue;
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
                close(fd);
                continue;
            }
            struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(80),
                                          .sin_addr.s_addr = htonl(ip)};
            if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 && errno != EINPROGRESS) {
                close(fd);
                continue;
            }
            fds[i] = fd;
            FD_SET(fd, &writable);
            if (fd > highest) highest = fd;
        }
        bool ready[BATCH] = {0};
        const int64_t deadline = esp_timer_get_time() + 280000;
        while (highest >= 0) {
            int64_t remaining = deadline - esp_timer_get_time();
            if (remaining <= 0) break;
            struct timeval wait = {.tv_sec = 0, .tv_usec = remaining};
            fd_set events = writable;
            if (select(highest + 1, NULL, &events, NULL, &wait) <= 0) break;
            highest = -1;
            for (unsigned i = 0; i < BATCH; ++i) {
                const int fd = fds[i];
                if (fd < 0 || !FD_ISSET(fd, &writable)) continue;
                if (FD_ISSET(fd, &events)) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    ready[i] = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0;
                    FD_CLR(fd, &writable);
                } else if (fd > highest) {
                    highest = fd;
                }
            }
        }
        for (unsigned i = 0; i < BATCH; ++i) if (fds[i] >= 0) close(fds[i]);
        for (unsigned i = 0; i < BATCH; ++i)
            if (ready[i]) {
                ++tcp_ready;
                probe((xc_endpoint_t){.addr = htonl(first + start + i), .port = 80}, found, &count, false);
            }
    }
    ESP_LOGI("xc", "Discovery done: %u TCP candidates, %d XC matches", tcp_ready, count);
    return count;
}
