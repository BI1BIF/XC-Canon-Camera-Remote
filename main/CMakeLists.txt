idf_component_register(
    SRCS "xc_main.c" "xc_navigation.c" "xc_ui.c" "xc_wifi.c" "xc_client.c" "xc_protocol.c" "xc_portal.c"
         "canon_ble.c" "canon_ble_protocol.c"
         "../assets/fonts/xc_font_14.c" "../assets/fonts/xc_font_20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi lwip nvs_flash
    PRIV_REQUIRES bt
)
