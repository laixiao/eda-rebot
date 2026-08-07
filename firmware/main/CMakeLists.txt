if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/wifi_config.h")
    message(FATAL_ERROR
        "main/wifi_config.h is required before build/flash/OTA. "
        "Copy wifi_config.example.h -> wifi_config.h and set WIFI_SSID / WIFI_PASS.")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "device_log.cpp"
        "xl9555.cpp"
        "pca9685.cpp"
        "ssd1306.cpp"
        "font_cjk.cpp"
        "board_i2c.cpp"
        "board_i2s.cpp"
        "radar_at6010.cpp"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "font_cjk.bin"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        esp_psram
        esp_hw_support
        app_update
        esp_partition
        esp_app_format
        driver
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_uart
)
