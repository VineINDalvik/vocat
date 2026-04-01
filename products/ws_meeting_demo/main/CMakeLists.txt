file(GLOB_RECURSE MAIN_SRCS ./*.c ./*.cpp)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        driver
        esp_driver_i2s
        esp_driver_i2c
        espressif__esp_codec_dev
        esp_vocat
        espressif__esp_lvgl_port
        lvgl__lvgl
        esp_websocket_client
        esp_http_client
        json
        esp-tls
        mbedtls
        esp_timer
)
