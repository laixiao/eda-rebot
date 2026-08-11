# Reuse wifi_config.h + OLED drivers from main firmware
set(MAIN_FW "${CMAKE_CURRENT_LIST_DIR}/../../firmware/main")

if(NOT EXISTS "${MAIN_FW}/wifi_config.h")
    message(FATAL_ERROR "Need firmware/main/wifi_config.h (same SSID as main app).")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "${MAIN_FW}/ssd1306.cpp"
        "${MAIN_FW}/font_cjk.cpp"
        "${MAIN_FW}/board_i2c.cpp"
    INCLUDE_DIRS
        "."
        "${MAIN_FW}"
    EMBED_FILES
        "${MAIN_FW}/font_cjk.bin"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        app_update
        esp_partition
        esp_app_format
        driver
        esp_driver_i2c
        esp_driver_gpio
)
