#include "board_camera.h"

#include "esp_log.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"

static const char *TAG = "camera";

/*
 * Pin mapping for ESP32-S3-WROOM-1 dev board with camera FPC connector.
 * Based on ESP32-S3-EYE / CAMERA_MODEL_ESP32S3_EYE layout from schematic.
 * PWDN not connected (-1). RESET active low.
 */
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5

#define CAM_PIN_D7      16  // Y9
#define CAM_PIN_D6      17  // Y8
#define CAM_PIN_D5      18  // Y7
#define CAM_PIN_D4      12  // Y6
#define CAM_PIN_D3      10  // Y5
#define CAM_PIN_D2      8   // Y4
#define CAM_PIN_D1      9   // Y3
#define CAM_PIN_D0      11  // Y2

#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

static bool s_inited = false;
static int  s_res_level = 1; // VGA default
static camera_fb_t *s_fb = nullptr;

static framesize_t level_to_framesize(int level) {
    switch (level) {
        case 0: return FRAMESIZE_QVGA;   // 320x240
        case 1: return FRAMESIZE_VGA;    // 640x480
        case 2: return FRAMESIZE_SVGA;   // 800x600
        case 3: return FRAMESIZE_XGA;    // 1024x768
        case 4: return FRAMESIZE_SXGA;   // 1280x1024
        default: return FRAMESIZE_VGA;
    }
}

bool board_camera_init(void) {
    if (s_inited) return true;

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = level_to_framesize(s_res_level);
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: 0x%x", err);
        return false;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init OK, resolution level=%d", s_res_level);
    return true;
}

bool board_camera_deinit(void) {
    if (!s_inited) return true;
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = nullptr;
    }
    esp_camera_deinit();
    s_inited = false;
    return true;
}

bool board_camera_ready(void) { return s_inited; }

bool board_camera_capture_jpeg(uint8_t **jpg_buf, size_t *jpg_len) {
    if (!s_inited) return false;
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = nullptr;
    }
    s_fb = esp_camera_fb_get();
    if (!s_fb) {
        ESP_LOGE(TAG, "fb_get failed");
        return false;
    }
    *jpg_buf = s_fb->buf;
    *jpg_len = s_fb->len;
    return true;
}

void board_camera_fb_return(void) {
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = nullptr;
    }
}

bool board_camera_set_resolution(int level) {
    if (level < 0 || level > 4) return false;
    s_res_level = level;
    if (!s_inited) return true;
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return false;
    return s->set_framesize(s, level_to_framesize(level)) == 0;
}

int board_camera_get_resolution(void) { return s_res_level; }
