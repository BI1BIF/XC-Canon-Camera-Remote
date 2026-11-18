#pragma once

#include "xc_client.h"
#include "xc_portal.h"
#include "esp_err.h"

#define XC_SETUP_SSID "Passport-XC"

typedef struct {
    char ssid[33], password[65];
} xc_wifi_login_t;

esp_err_t xc_wifi_init(void);
bool xc_wifi_ip(esp_netif_ip_info_t *ip);
void xc_wifi_ssid(char out[33]);
bool xc_wifi_signal(int *rssi, unsigned *channel);
esp_err_t xc_wifi_connect(const xc_wifi_login_t *login);
void xc_wifi_forget(void);
esp_err_t xc_setup_start(xc_setup_mode_t mode, const xc_endpoint_t *target, bool lan, char password[11]);
void xc_setup_stop(void);
void xc_web_idle(bool online);
bool xc_setup_wifi(xc_wifi_login_t *login);
bool xc_setup_camera(xc_endpoint_t *target, xc_credentials_t *login);
void xc_setup_title_load(char title[XC_TITLE_SIZE]);
bool xc_setup_title_changed(void);
