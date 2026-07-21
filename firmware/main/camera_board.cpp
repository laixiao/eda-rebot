#include "camera_board.h"
#include "board_config.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"

static const char *TAG = "camera";
static bool camOk = false;
static camera_fb_t *heldFb = nullptr;

// PWDN 高=掉电；由 XL9555 IO0_4 驱动（板载电阻建议上拉到 3V3）
bool cameraPower(XL9555 &xl, bool on) { return xl.setPin(XL_CAM_PWDN, !on); }

bool cameraOk() { return camOk; }

bool cameraBegin(XL9555 &xl) {
  if (camOk) return true;

  // 上电时序：先确保掉电，再释放 PWDN，给 OV5640 核电/内部 LDO 留稳定时间
  if (!cameraPower(xl, false)) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  if (!cameraPower(xl, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(300));

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
  cfg.pin_pwdn = -1;   // 经 XL9555，不用库内 GPIO
  cfg.pin_reset = -1;  // 硬件接 EN，固件不单独复位
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode = CAMERA_GRAB_LATEST;

  if (hasPsram) {
    cfg.xclk_freq_hz = 20000000;
    cfg.frame_size = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count = 2;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    ESP_LOGW(TAG, "PSRAM unavailable — camera fallback QQVGA in DRAM");
    cfg.xclk_freq_hz = 10000000;
    cfg.frame_size = FRAMESIZE_QQVGA;
    cfg.jpeg_quality = 20;
    cfg.fb_count = 1;
    cfg.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init failed: %s (PSRAM=%d) — expect OV5640, CAM_1V2≈1.2V",
             esp_err_to_name(err), (int)hasPsram);
    cameraPower(xl, false);
    camOk = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    ESP_LOGE(TAG, "sensor_get null after init");
    esp_camera_deinit();
    cameraPower(xl, false);
    camOk = false;
    return false;
  }

  const uint16_t pid = s->id.PID;
  if (pid == OV5640_PID) {
    ESP_LOGI(TAG, "sensor OV5640 (PID=0x%04x)", pid);
  } else if (pid == OV2640_PID) {
    ESP_LOGW(TAG, "sensor OV2640 (PID=0x%04x) — expected OV5640", pid);
  } else {
    ESP_LOGW(TAG, "unknown sensor PID=0x%04x (expected OV5640)", pid);
  }

  s->set_framesize(s, hasPsram ? FRAMESIZE_QVGA : FRAMESIZE_QQVGA);
  s->set_quality(s, hasPsram ? 12 : 20);
  if (pid == OV5640_PID && s->set_vflip) s->set_vflip(s, 1);
  if (pid == OV5640_PID && s->set_hmirror) s->set_hmirror(s, 0);

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
