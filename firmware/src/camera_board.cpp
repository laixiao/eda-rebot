#include "camera_board.h"
#include "board_config.h"

#include "esp_camera.h"

static bool camOk = false;
static camera_fb_t *heldFb = nullptr;

bool cameraPower(XL9555 &xl, bool on) {
  // PWDN 高=掉电，低=工作
  return xl.setPin(XL_CAM_PWDN, !on);
}

bool cameraOk() { return camOk; }

bool cameraBegin(XL9555 &xl) {
  if (camOk) return true;
  if (!cameraPower(xl, true)) return false;
  delay(50);

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
  cfg.xclk_freq_hz = 20000000;
  cfg.frame_size = FRAMESIZE_QVGA;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.jpeg_quality = 12;
  cfg.fb_count = 2;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    cameraPower(xl, false);
    camOk = false;
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, 12);
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
