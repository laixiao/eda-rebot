#include "camera_board.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"

static const char *TAG = "camera";
static bool camOk = false;
static camera_fb_t *heldFb = nullptr;

// PWDN 高=掉电。XL9555 供电 3V3，只能开漏：拉低=工作，高阻靠 R22→CAM_2V8
bool cameraPower(XL9555 &xl, bool on) {
  if (on) return xl.driveLow(XL_CAM_PWDN);
  return xl.releasePin(XL_CAM_PWDN);
}

static bool cameraPulseReset(XL9555 &xl) {
  if (!xl.driveLow(XL_CAM_RST)) return false;
  vTaskDelay(pdMS_TO_TICKS(2)); // RESETB 低 ≥1ms
  if (!xl.releasePin(XL_CAM_RST)) return false;
  vTaskDelay(pdMS_TO_TICKS(20)); // 释放后等 SCCB 就绪
  return true;
}

static void logPwdnRegs(XL9555 &xl, const char *where) {
  uint8_t in = 0, out = 0, cfg = 0;
  bool pwdnH = true, rstH = true;
  const bool lvl = cameraCtrlLevels(xl, pwdnH, rstH);
  const bool dump = xl.dumpPort0(in, out, cfg);
  ESP_LOGI(TAG,
           "%s: PWDN=%s RST=%s | P0 in=0x%02x out=0x%02x cfg=0x%02x "
           "(bit4 PWDN: in=%d out=%d cfg_in=%d) lvl_ok=%d dump_ok=%d",
           where, lvl ? (pwdnH ? "HIGH" : "LOW") : "?", lvl ? (rstH ? "HIGH" : "LOW") : "?", in, out,
           cfg, (in >> 4) & 1, (out >> 4) & 1, (cfg >> 4) & 1, (int)lvl, (int)dump);
  ESP_LOGW(TAG, "measure VOLTAGE U6.17→GND (not ohms!). Expect ~0V while held. 11.6kΩ is just R22.");
}

/** 开漏：高=释放靠外部上拉，低=推挽拉低 */
static void odHigh(int pin) {
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)pin, GPIO_FLOATING);
}
static void odLow(int pin) {
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)pin, 0);
}
static int odRead(int pin) { return gpio_get_level((gpio_num_t)pin); }

static void bbDelay() {
  esp_rom_delay_us(5);
}

/** 软件 I2C：发 7bit 地址写，返回是否读到 ACK（SDA 被拉低） */
static bool bitbangProbeAddr(uint8_t addr7) {
  const int sda = PIN_CAM_SDA;
  const int scl = PIN_CAM_SCL;
  odHigh(sda);
  odHigh(scl);
  bbDelay();
  // START
  odLow(sda);
  bbDelay();
  odLow(scl);
  bbDelay();
  const uint8_t byte = (uint8_t)((addr7 << 1) | 0); // write
  for (int i = 7; i >= 0; --i) {
    if ((byte >> i) & 1) odHigh(sda);
    else odLow(sda);
    bbDelay();
    odHigh(scl);
    bbDelay();
    odLow(scl);
    bbDelay();
  }
  // ACK bit：释放 SDA，拉高 SCL 采样
  odHigh(sda);
  bbDelay();
  odHigh(scl);
  bbDelay();
  const bool ack = (odRead(sda) == 0);
  odLow(scl);
  bbDelay();
  // STOP
  odLow(sda);
  bbDelay();
  odHigh(scl);
  bbDelay();
  odHigh(sda);
  bbDelay();
  return ack;
}

static void sccbBusSelfTest() {
  const int sda = PIN_CAM_SDA;
  const int scl = PIN_CAM_SCL;
  odHigh(sda);
  odHigh(scl);
  vTaskDelay(pdMS_TO_TICKS(2));
  const int idleSda = odRead(sda);
  const int idleScl = odRead(scl);
  ESP_LOGI(TAG, "bus test idle: SDA=%d SCL=%d (want 1,1) — FPC unplugged => later probes must be NACK",
           idleSda, idleScl);

  odLow(sda);
  bbDelay();
  const int sdaDriven = odRead(sda);
  const int sclWhileSdaLow = odRead(scl);
  odHigh(sda);
  bbDelay();
  odLow(scl);
  bbDelay();
  const int sclDriven = odRead(scl);
  const int sdaWhileSclLow = odRead(sda);
  odHigh(scl);
  bbDelay();

  ESP_LOGI(TAG, "bus test drive: SDA_low→SDA=%d SCL=%d | SCL_low→SCL=%d SDA=%d", sdaDriven,
           sclWhileSdaLow, sclDriven, sdaWhileSclLow);
  if (sdaDriven != 0) ESP_LOGE(TAG, "cannot pull SDA low — IO4/path broken");
  if (sclDriven != 0) ESP_LOGE(TAG, "cannot pull SCL low — IO5/path broken");
  if (sclWhileSdaLow == 0) ESP_LOGE(TAG, "SDA↔SCL SHORT (pulling SDA also pulls SCL)");
  if (sdaWhileSclLow == 0 && idleSda == 1)
    ESP_LOGW(TAG, "SCL low pulled SDA — possible short or strong coupling");

  const uint8_t addrs[] = {0x3C, 0x30, 0x21, 0x3D, 0x00};
  for (uint8_t a : addrs) {
    const bool ack = bitbangProbeAddr(a);
    ESP_LOGI(TAG, "bitbang probe 0x%02x → %s", a, ack ? "ACK" : "NACK");
  }
}

/** 在 esp_camera_init 前：总线自检 + 手动 XCLK + HW I2C 对照 */
static void sccbPreflight(uint32_t xclk_hz) {
  // 先不做 XCLK，纯 GPIO/软件 I2C（拔排线时应全 NACK）
  sccbBusSelfTest();

  // XCLK on IO15
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_1_BIT;
  t.timer_num = LEDC_TIMER_1;
  t.freq_hz = xclk_hz;
  t.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&t) != ESP_OK) {
    ESP_LOGW(TAG, "preflight XCLK timer fail");
    return;
  }
  ledc_channel_config_t ch = {};
  ch.gpio_num = PIN_CAM_XCLK;
  ch.speed_mode = LEDC_LOW_SPEED_MODE;
  ch.channel = LEDC_CHANNEL_1;
  ch.timer_sel = LEDC_TIMER_1;
  ch.duty = 1;
  ch.hpoint = 0;
  if (ledc_channel_config(&ch) != ESP_OK) {
    ESP_LOGW(TAG, "preflight XCLK channel fail");
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(50));

  // PCLK/VSYNC 边沿计数：有时钟+模组上电时常有活动；全 0 则接触/供电/XCLK 未到模组
  gpio_reset_pin((gpio_num_t)PIN_CAM_PCLK);
  gpio_reset_pin((gpio_num_t)PIN_CAM_VSYNC);
  gpio_set_direction((gpio_num_t)PIN_CAM_PCLK, GPIO_MODE_INPUT);
  gpio_set_direction((gpio_num_t)PIN_CAM_VSYNC, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_CAM_PCLK, GPIO_FLOATING);
  gpio_set_pull_mode((gpio_num_t)PIN_CAM_VSYNC, GPIO_FLOATING);
  uint32_t pclkEdges = 0, vsEdges = 0;
  int prevP = gpio_get_level((gpio_num_t)PIN_CAM_PCLK);
  int prevV = gpio_get_level((gpio_num_t)PIN_CAM_VSYNC);
  const int64_t t0 = esp_timer_get_time();
  while (esp_timer_get_time() - t0 < 100000) { // 100ms
    const int p = gpio_get_level((gpio_num_t)PIN_CAM_PCLK);
    const int v = gpio_get_level((gpio_num_t)PIN_CAM_VSYNC);
    if (p != prevP) {
      ++pclkEdges;
      prevP = p;
    }
    if (v != prevV) {
      ++vsEdges;
      prevV = v;
    }
  }
  ESP_LOGI(TAG, "DVP activity 100ms: PCLK_edges=%u VSYNC_edges=%u (0/0 => module not clocking)",
           (unsigned)pclkEdges, (unsigned)vsEdges);

  i2c_master_bus_config_t busCfg = {};
  busCfg.i2c_port = I2C_NUM_1;
  busCfg.sda_io_num = (gpio_num_t)PIN_CAM_SDA;
  busCfg.scl_io_num = (gpio_num_t)PIN_CAM_SCL;
  busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
  busCfg.glitch_ignore_cnt = 7;
  busCfg.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  esp_err_t busErr = i2c_new_master_bus(&busCfg, &bus);
  if (busErr != ESP_OK) {
    ESP_LOGW(TAG, "preflight I2C bus fail: %s", esp_err_to_name(busErr));
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    return;
  }

  auto probeList = [&](const char *tag) {
    const uint8_t addrs[] = {0x3C, 0x30, 0x21, 0x3D};
    for (uint8_t a : addrs) {
      const bool ack = i2c_master_probe(bus, a, 100) == ESP_OK;
      ESP_LOGI(TAG, "%s probe 0x%02x → %s", tag, a, ack ? "ACK" : "NACK");
    }
  };
  probeList("hw-i2c");

  // 试读 PID
  i2c_device_config_t devCfg = {};
  devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  devCfg.device_address = 0x3C;
  devCfg.scl_speed_hz = 50000;
  i2c_master_dev_handle_t dev = nullptr;
  if (i2c_master_bus_add_device(bus, &devCfg, &dev) == ESP_OK) {
    uint8_t regH[2] = {0x30, 0x0A};
    uint8_t regL[2] = {0x30, 0x0B};
    uint8_t h = 0xFF, l = 0xFF;
    const bool rh = i2c_master_transmit_receive(dev, regH, 2, &h, 1, 200) == ESP_OK;
    const bool rl = i2c_master_transmit_receive(dev, regL, 2, &l, 1, 200) == ESP_OK;
    ESP_LOGI(TAG, "hw-i2c OV5640 PID @0x3C: ok=%d/%d PID=0x%02x%02x (want 0x5640)", (int)rh, (int)rl,
             h, l);
    i2c_master_bus_rm_device(dev);
  }
  i2c_del_master_bus(bus);

  // 诊断：SDA/SCL 对调再扫一次（若对调出现 ACK，说明模组/座子线序与原理图交叉）
  busCfg.sda_io_num = (gpio_num_t)PIN_CAM_SCL;
  busCfg.scl_io_num = (gpio_num_t)PIN_CAM_SDA;
  if (i2c_new_master_bus(&busCfg, &bus) == ESP_OK) {
    probeList("hw-i2c-SWAP");
    i2c_del_master_bus(bus);
  }

  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
  gpio_reset_pin((gpio_num_t)PIN_CAM_XCLK);
  gpio_reset_pin((gpio_num_t)PIN_CAM_SDA);
  gpio_reset_pin((gpio_num_t)PIN_CAM_SCL);
}

bool cameraOk() { return camOk; }

bool cameraBegin(XL9555 &xl) {
  if (camOk) return true;

  // 上电时序：PWDN 高(掉电) → 拉低 PWDN → 脉冲 RESETB → 再 init
  if (!xl.releasePin(XL_CAM_RST)) return false;
  if (!cameraPower(xl, false)) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  if (!cameraPower(xl, true)) {
    ESP_LOGE(TAG, "driveLow(PWDN) failed — XL9555 IO0_4");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(10));
  if (!cameraPulseReset(xl)) return false;
  vTaskDelay(pdMS_TO_TICKS(500)); // 模组上电稳定

  logPwdnRegs(xl, "pre-init");
  sccbPreflight(10000000);

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
  cfg.pin_pwdn = -1;  // XL9555 IO0_4 开漏
  cfg.pin_reset = -1; // XL9555 IO0_7 开漏
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode = CAMERA_GRAB_LATEST;
  // 先用 10MHz，分压后边沿更干净；成功后再可由 sensor 侧维持
  cfg.xclk_freq_hz = 10000000;
  cfg.frame_size = hasPsram ? FRAMESIZE_QVGA : FRAMESIZE_QQVGA;
  cfg.jpeg_quality = hasPsram ? 12 : 20;
  cfg.fb_count = hasPsram ? 2 : 1;
  cfg.fb_location = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "init @10MHz failed (%s), retry @20MHz", esp_err_to_name(err));
    cfg.xclk_freq_hz = 20000000;
    err = esp_camera_init(&cfg);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init failed: %s (PSRAM=%d) — expect OV5640, CAM_1V5/CAM_2V8",
             esp_err_to_name(err), (int)hasPsram);
    logPwdnRegs(xl, "after-fail-hold");
    ESP_LOGW(TAG, "leaving PWDN driven LOW — measure VOLTAGE U6.17→GND now (expect ~0V)");
    camOk = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    ESP_LOGE(TAG, "sensor_get null after init");
    esp_camera_deinit();
    logPwdnRegs(xl, "after-fail-hold");
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
  xl.releasePin(XL_CAM_RST);
  cameraPower(xl, false);
}

bool cameraCtrlLevels(XL9555 &xl, bool &pwdnHigh, bool &rstHigh) {
  if (!xl.getPin(XL_CAM_PWDN, pwdnHigh)) return false;
  if (!xl.getPin(XL_CAM_RST, rstHigh)) return false;
  return true;
}

bool cameraHoldPower(XL9555 &xl) {
  if (camOk) {
    cameraReleaseFrame();
    esp_camera_deinit();
    camOk = false;
  }
  if (!xl.releasePin(XL_CAM_RST)) return false;
  if (!cameraPower(xl, false)) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  if (!cameraPower(xl, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(10));
  if (!cameraPulseReset(xl)) return false;
  vTaskDelay(pdMS_TO_TICKS(50));
  logPwdnRegs(xl, "hold");
  bool pwdnH = true, rstH = true;
  cameraCtrlLevels(xl, pwdnH, rstH);
  return !pwdnH && rstH;
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
