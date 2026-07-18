#include "camera_board.h"
#include "board_config.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camera";
static bool camOk = false;
static camera_fb_t *heldFb = nullptr;

bool cameraPower(XL9555 &xl, bool on) { return xl.setPin(XL_CAM_PWDN, !on); }

bool cameraOk() { return camOk; }

bool cameraBegin(XL9555 &xl) {
  if (camOk) return true;
  if (!cameraPower(xl, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(200));

  const bool hasPsram = esp_psram_is_initialized();

  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = PIN_CAM_D0;
  cfg.pin_d1 = PIN_CAM_D1;
  cfg.pin_d2 = PIN_CAM_D2;
  cfg.pin_d3 = PIN_CAM_D3;
  cfg.pin_d4 = PIN_CAM_D4;
  cfg.pin_d5 = PIN_CAM_D5;
  cfg.pin_d6 = PIN_CAM_D6;
  cfg.pin_d7 = PIN_CAM_D7;
  cfg.pin_xclk = PIN_CAM_XCLK;
  cfg.pin_pclk = PIN_CAM_PCLK;
  cfg.pin_vsync = PIN_CAM_VSYNC;
  cfg.pin_href = PIN_CAM_HREF;
  cfg.pin_sccb_sda = PIN_CAM_SDA;
  cfg.pin_sccb_scl = PIN_CAM_SCL;
  cfg.pin_pwdn = -1;
  cfg.pin_reset = -1;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode = CAMERA_GRAB_LATEST;

  if (hasPsram) {
    cfg.xclk_freq_hz = 20000000;
    cfg.frame_size = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count = 2;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    // 无 PSRAM 时只能用内部 DRAM 试 FPC/SCCB；分辨率必须很小
    ESP_LOGW(TAG, "PSRAM unavailable — camera fallback QQVGA in DRAM");
    cfg.xclk_freq_hz = 10000000;
    cfg.frame_size = FRAMESIZE_QQVGA;
    cfg.jpeg_quality = 20;
    cfg.fb_count = 1;
    cfg.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init failed: %s (PSRAM=%d)", esp_err_to_name(err), (int)hasPsram);
    cameraPower(xl, false);
    camOk = false;
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, hasPsram ? FRAMESIZE_QVGA : FRAMESIZE_QQVGA);
    s->set_quality(s, hasPsram ? 12 : 20);
  }
  camOk = true;
  return true;
}

void cameraEnd(XL9555 &xl) {
  cameraReleaseFrame();
  if (camOk) {
    esp_camera_deinit();
    camOk = false;
  }
  cameraPower(xl, false);
}

bool cameraCaptureJpeg(uint8_t *&buf, size_t &len) {
  buf = nullptr;
  len = 0;
  if (!camOk) return false;
  cameraReleaseFrame();
  heldFb = esp_camera_fb_get();
  if (!heldFb || heldFb->format != PIXFORMAT_JPEG) {
    cameraReleaseFrame();
    return false;
  }
  buf = heldFb->buf;
  len = heldFb->len;
  return true;
}

void cameraReleaseFrame() {
  if (heldFb) {
    esp_camera_fb_return(heldFb);
    heldFb = nullptr;
  }
}
