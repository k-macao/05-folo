idf_component_register(
    SRCS "main.c"
         "intel_logic.c"
         "intel_feed.c"
         "ui_crt.c"
         "app_wifi.c"
         "app_feed.c"
         "fonts/crt_font_12.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES
        bsp
        esp_http_client esp-tls mbedtls
        esp_wifi esp_netif esp_event
        nvs_flash json
        esp_timer esp_system
)
