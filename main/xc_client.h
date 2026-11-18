#pragma once

#include "xc_protocol.h"
#include "esp_netif.h"

#define XC_MAX_CAMERAS 12

typedef struct {
    uint32_t addr;  // network byte order
    uint16_t port;
    char name[48];
} xc_endpoint_t;

typedef enum { XC_OK, XC_AUTH, XC_NO_LINK, XC_WRONG_DEVICE, XC_DENIED } xc_result_t;

typedef struct {
    char user[49], password[65];
} xc_credentials_t;

xc_result_t xc_read(const xc_endpoint_t *ep, const xc_credentials_t *login, xc_camera_t *state);
xc_result_t xc_read_tally(const xc_endpoint_t *ep, const xc_credentials_t *login, xc_tally_t *tally);
xc_result_t xc_adjust(const xc_endpoint_t *ep, const xc_credentials_t *login,
                      const char *key, int value);
xc_result_t xc_set_record(const xc_endpoint_t *ep, const xc_credentials_t *login, xc_record_t target);
void xc_client_reset(void);
void xc_control_idle(const xc_endpoint_t *ep, const xc_credentials_t *login, bool release);
int xc_discover(const esp_netif_ip_info_t *lan, xc_endpoint_t *found,
                bool (*progress)(unsigned percent));
