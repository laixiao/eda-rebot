if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/wifi_config.h")
    message(FATAL_ERROR
        "main/wifi_config.h is required before build/flash/OTA. "
        "Copy wifi_config.example.h -> wifi_config.h and set WIFI_SSID / WIFI_PASS.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/srmodels.bin")
    message(FATAL_ERROR
        "main/srmodels.bin missing. Build once with esp-sr movemodel.py and copy "
        "build/srmodels/srmodels.bin -> main/srmodels.bin (see docs/烧录-监视.txt).")
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
        "voice_sr.cpp"
        "wake_reply.cpp"
        "board_camera.cpp"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_BINARY_DIR}"
    EMBED_FILES
        "font_cjk.bin"
        "srmodels.bin"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        lwip
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
        espressif__esp-sr
        espressif__esp32-camera
)

# 早期展开阶段不能 GLOB / target_add_binary_data
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()

# 唤醒随机回复：firmware/assets/*.wav（PCM16，任意常见采样率，运行时重采样到 16k）
file(GLOB WAKE_REPLY_WAVS "${CMAKE_CURRENT_LIST_DIR}/../assets/*.wav")
list(LENGTH WAKE_REPLY_WAVS _wake_reply_n)
if(_wake_reply_n EQUAL 0)
    message(WARNING "firmware/assets/*.wav empty — wake reply falls back to tone chime")
endif()

set(_wake_reply_inc "${CMAKE_CURRENT_BINARY_DIR}/wake_reply_assets.inc")
file(WRITE "${_wake_reply_inc}" "// Auto-generated from firmware/assets/*.wav — do not edit\n")
foreach(_wav ${WAKE_REPLY_WAVS})
    get_filename_component(_wav_name "${_wav}" NAME)
    string(REPLACE "." "_" _wav_sym "${_wav_name}")
    file(APPEND "${_wake_reply_inc}"
        "extern const uint8_t _binary_${_wav_sym}_start[];\n"
        "extern const uint8_t _binary_${_wav_sym}_end[];\n")
endforeach()
file(APPEND "${_wake_reply_inc}" "static const WakeReplyAsset kWakeReplyAssets[] = {\n")
foreach(_wav ${WAKE_REPLY_WAVS})
    get_filename_component(_wav_name "${_wav}" NAME)
    string(REPLACE "." "_" _wav_sym "${_wav_name}")
    file(APPEND "${_wake_reply_inc}"
        "  { _binary_${_wav_sym}_start, _binary_${_wav_sym}_end, \"${_wav_name}\" },\n")
endforeach()
file(APPEND "${_wake_reply_inc}" "};\n")
message(STATUS "Wake reply WAVs (${_wake_reply_n}): ${WAKE_REPLY_WAVS}")

foreach(_wav ${WAKE_REPLY_WAVS})
    target_add_binary_data(${COMPONENT_LIB} "${_wav}" BINARY)
endforeach()
