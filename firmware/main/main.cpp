#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "board_config.h"
#include "board_i2c.h"
#include "board_spi.h"
#include "camera_board.h"
#include "st7796.h"
#include "xpt2046.h"
#include "board_i2s.h"
#include "xl9555.h"
#include "pca9685.h"
#include "ssd1306.h"
#include "web_ui.h"
#include "web_radar_ui.h"
#include "radar_at6010.h"
#include "font_cjk.h"
#include "device_log.h"
#include "voice_sr.h"
#include "wake_reply.h"

static const char *TAG = "eda_robot";
static const char *FW_VERSION = "3.7.8-v5";
static volatile bool otaBusy = false;
static volatile bool shutdownPending = false;
static volatile bool cfgDirty = false;
static bool flagVoice = false;
static int servoAngleDeg[5] = {90, 90, 90, 90, 90};
/** 持久化镜像：急停清 spotDutyPct 时仍保留，避免随后 cfgSave 把 NVS 写成全 0 */
static int cfgSnapLed[3] = {0, 0, 0};
static int cfgSnapSrv[5] = {90, 90, 90, 90, 90};
static bool cfgWantPwm = false;
static bool cfgWantAmp = false;
static bool cfgWantRadar = false;
static bool cfgLoaded = false;
static SemaphoreHandle_t cfgMutex = nullptr;

static void cfgMarkDirty();
static void cfgSave();
static void cfgLoad();
static void cfgApplyBoot();
static bool cfgRestoreOutputs();

// 录音 / 播放（16 kHz mono PCM16，缓冲于 PSRAM）
static constexpr size_t REC_MAX_SAMPLES = (size_t)BOARD_I2S_RATE * 12;  // ~12 s
static constexpr size_t PLAY_UPLOAD_MAX = 512 * 1024;
static int16_t *recBuf = nullptr;
static size_t recSamples = 0;
static volatile bool recActive = false;
static volatile bool playBusy = false;
static TaskHandle_t recTaskHandle = nullptr;
static SemaphoreHandle_t audioMutex = nullptr;

static XL9555 xl;
static PCA9685 pcaServo;
static PCA9685 pcaMotor;
static SSD1306 oled;
static ST7796 lcd;
static XPT2046 touch;

static bool flagPwm = false;
static bool flagAmp = false;
static volatile int32_t enc1 = 0;
static volatile int32_t enc2 = 0;
static volatile int32_t enc3 = 0;
static volatile int32_t enc4 = 0;
static uint8_t prevXlA = 0;
static bool enc34Initialized = false;
static portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;
static bool flagStby = false;
static bool lcdOk = false;
static bool touchOk = false;
static volatile uint8_t motorActiveMask = 0;
static int64_t lastMotorCommandUs = 0;
static const int64_t MOTOR_FAILSAFE_US = 1500000;
static SemaphoreHandle_t cameraMutex = nullptr;
static SemaphoreHandle_t streamSlot = nullptr;

static bool flagRadarPwr = false;
/** 每日时刻表（SNTP 校时后生效；分钟 0..1439，-1=未设） */
static bool radarScheduleEnable = false;
static int radarSchedOnMin = -1;
static int radarSchedOffMin = -1;
/** 忽略扇区：可多段，默认一段 0°～-60°（开关默认关） */
static constexpr int RADAR_IGN_MAX = 8;
struct RadarIgnSec {
  int from;
  int to;
};
static bool radarIgnoreEnable = false;
static RadarIgnSec radarIgnoreSec[RADAR_IGN_MAX] = {{0, -60}};
static int radarIgnoreCount = 1;
static int64_t schedLastCheckUs = 0;
static volatile bool timeSynced = false;
static bool sntpStarted = false;
static bool flagPeriphOff = false;  // 「关闭所有外设」开关
static bool savedPwm = false;
static bool savedAmp = false;
static bool savedRadarPwr = false;
static bool savedFanAuto = false;
static bool savedFanGesture = false;
static int spotDutyPct[SPOT_COUNT] = {0, 0, 0};
static bool i2sReady = false;
static bool wifiOk = false;
static char ipStr[16] = {0};

static httpd_handle_t server = nullptr;
static SemaphoreHandle_t actuatorMutex = nullptr;
static SemaphoreHandle_t oledMutex = nullptr;
static bool httpRegistrationOk = true;

static bool actuatorLock() {
  return actuatorMutex && xSemaphoreTakeRecursive(actuatorMutex, portMAX_DELAY) == pdTRUE;
}

static void actuatorUnlock() {
  if (actuatorMutex) xSemaphoreGiveRecursive(actuatorMutex);
}

// ---- HTTP helpers ----
// ---- encoders ----
static void IRAM_ATTR onEnc1(void *) {
  const int a = gpio_get_level((gpio_num_t)PIN_ENC1_A);
  const int b = gpio_get_level((gpio_num_t)PIN_ENC1_B);
  portENTER_CRITICAL_ISR(&encMux);
  enc1 += (a == b) ? 1 : -1;
  portEXIT_CRITICAL_ISR(&encMux);
}

static void IRAM_ATTR onEnc2(void *) {
  const int a = gpio_get_level((gpio_num_t)PIN_ENC2_A);
  const int b = gpio_get_level((gpio_num_t)PIN_ENC2_B);
  portENTER_CRITICAL_ISR(&encMux);
  enc2 += (a == b) ? 1 : -1;
  portEXIT_CRITICAL_ISR(&encMux);
}

static void updateEnc34() {
  uint8_t p0 = 0;
  if (!xl.readPort(0, p0)) return;
  radar_set_gpio_out((p0 >> XL_RADAR_OUT) & 1);
  portENTER_CRITICAL(&encMux);
  if (!enc34Initialized) {
    prevXlA = p0;
    enc34Initialized = true;
    portEXIT_CRITICAL(&encMux);
    return;
  }
  const uint8_t changed = p0 ^ prevXlA;
  if (changed & (1u << XL_ENC3_A)) {
    const bool a = (p0 >> XL_ENC3_A) & 1;
    const bool b = (p0 >> XL_ENC3_B) & 1;
    enc3 += (a == b) ? 1 : -1;
  }
  if (changed & (1u << XL_ENC4_A)) {
    const bool a = (p0 >> XL_ENC4_A) & 1;
    const bool b = (p0 >> XL_ENC4_B) & 1;
    enc4 += (a == b) ? 1 : -1;
  }
  prevXlA = p0;
  portEXIT_CRITICAL(&encMux);
}


static void addCors(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
}

static esp_err_t sendJson(httpd_req_t *req, int code, const std::string &body) {
  addCors(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, code == 200   ? "200 OK"
                             : code == 204 ? "204 No Content"
                             : code == 400 ? "400 Bad Request"
                             : code == 409 ? "409 Conflict"
                             : code == 404 ? "404 Not Found"
                             : code == 503 ? "503 Service Unavailable"
                                            : "500 Internal Server Error");
  return httpd_resp_send(req, body.c_str(), body.size());
}

static std::string readBody(httpd_req_t *req) {
  int total = req->content_len;
  if (total <= 0) return "";
  if (total > 2048) total = 2048;
  std::string body;
  body.resize(total);
  int got = 0;
  while (got < total) {
    int n = httpd_req_recv(req, &body[got], total - got);
    if (n <= 0) break;
    got += n;
  }
  body.resize(got);
  return body;
}

static std::string queryStr(httpd_req_t *req) {
  size_t len = httpd_req_get_url_query_len(req);
  if (len == 0) return "";
  std::string q;
  q.resize(len + 1);
  if (httpd_req_get_url_query_str(req, &q[0], len + 1) != ESP_OK) return "";
  q.resize(strlen(q.c_str()));
  return q;
}

static bool queryGet(const std::string &q, const char *key, char *out, size_t outlen) {
  if (q.empty()) return false;
  return httpd_query_key_value(q.c_str(), key, out, outlen) == ESP_OK;
}

static int bodyInt(const std::string &body, const char *key, int defVal) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = body.find(k);
  if (p == std::string::npos) return defVal;
  p = body.find(':', p);
  if (p == std::string::npos) return defVal;
  p++;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
  return atoi(body.c_str() + p);
}

static bool bodyBool(const std::string &body, const char *key, bool defVal) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = body.find(k);
  if (p == std::string::npos) return defVal;
  p = body.find(':', p);
  if (p == std::string::npos) return defVal;
  std::string rest = body.substr(p + 1);
  size_t t = rest.find("true");
  size_t f = rest.find("false");
  if (t != std::string::npos && (f == std::string::npos || t < f)) return true;
  if (f != std::string::npos) return false;
  return bodyInt(body, key, defVal ? 1 : 0) != 0;
}

static std::string bodyStr(const std::string &body, const char *key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = body.find(k);
  if (p == std::string::npos) return "";
  p = body.find(':', p);
  if (p == std::string::npos) return "";
  p = body.find('"', p);
  if (p == std::string::npos) return "";
  size_t q = body.find('"', p + 1);
  if (q == std::string::npos) return "";
  return body.substr(p + 1, q - p - 1);
}

struct ReqArgs {
  std::string q;
  std::string body;
};

static ReqArgs loadArgs(httpd_req_t *req) {
  ReqArgs a;
  a.q = queryStr(req);
  a.body = readBody(req);
  return a;
}

static int argInt(const ReqArgs &a, const char *key, int defVal) {
  char v[32];
  if (queryGet(a.q, key, v, sizeof(v))) return atoi(v);
  return bodyInt(a.body, key, defVal);
}

static bool argBool(const ReqArgs &a, const char *key, bool defVal) {
  char v[32];
  if (queryGet(a.q, key, v, sizeof(v))) {
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "on") ||
        !strcasecmp(v, "yes"))
      return true;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "off") ||
        !strcasecmp(v, "no"))
      return false;
    return atoi(v) != 0;
  }
  return bodyBool(a.body, key, defVal);
}

static std::string argStr(const ReqArgs &a, const char *key, const char *defVal = "") {
  char v[128];
  if (queryGet(a.q, key, v, sizeof(v))) return v;
  std::string s = bodyStr(a.body, key);
  return s.empty() ? defVal : s;
}

static bool argsHasKey(const ReqArgs &a, const char *key) {
  char v[8];
  if (queryGet(a.q, key, v, sizeof(v))) return true;
  const std::string k = std::string("\"") + key + "\"";
  return a.body.find(k) != std::string::npos;
}

static std::string i2cKnownJson() {
  // 仅用启动时已探测的 present 标志，禁止在 /api/status 里再 probe（空总线易 Interrupt WDT）
  std::string s = "[";
  bool first = true;
  auto add = [&](uint8_t addr, bool ok) {
    if (!ok) return;
    if (!first) s += ',';
    first = false;
    char b[8];
    snprintf(b, sizeof(b), "%u", addr);
    s += b;
  };
  add(ADDR_XL9555, xl.present());
  if (oled.present()) add(oled.addr(), true);
  add(ADDR_PCA_SERVO, pcaServo.present());
  add(ADDR_PCA_MOTOR, pcaMotor.present());
  s += ']';
  return s;
}

static std::string i2cScanJson(bool full = false) {
  std::string s = "[";
  bool first = true;
  auto append = [&](uint8_t addr) {
    if (!board_i2c_probe(addr)) return;
    if (!first) s += ',';
    first = false;
    char b[8];
    snprintf(b, sizeof(b), "%u", addr);
    s += b;
  };
  if (full) {
    for (uint8_t addr = 0x08; addr < 0x78; addr++) append(addr);
  } else {
    static const uint8_t kAddrs[] = {ADDR_XL9555, ADDR_OLED, 0x3D, ADDR_PCA_SERVO, ADDR_PCA_MOTOR};
    for (uint8_t addr : kAddrs) append(addr);
  }
  s += ']';
  return s;
}

static bool oledTryInit(uint8_t &addrOut, uint32_t &hzOut, int &failStep, std::string &diag) {
  static const uint8_t kAddrs[] = {ADDR_OLED, 0x3D};
  static const uint32_t kSpeeds[] = {100000, 400000};
  diag.clear();
  failStep = -1;
  for (uint32_t hz : kSpeeds) {
    for (uint8_t addr : kAddrs) {
      const bool probe = board_i2c_probe(addr);
      const bool ping = board_i2c_oled_ping(addr, hz);
      char item[96];
      snprintf(item, sizeof(item), "0x%02X@%lukHz probe=%s ping=%s", (unsigned)addr,
               (unsigned long)(hz / 1000), probe ? "Y" : "N", ping ? "Y" : "N");
      if (!diag.empty()) diag += ';';
      diag += item;
      if (!ping) continue;
      const int step = oled.beginEx(addr, hz);
      if (step < 0) {
        addrOut = addr;
        hzOut = hz;
        failStep = -1;
        return true;
      }
      failStep = step;
    }
  }
  return false;
}

// ---- actuators ----
static bool pcaAllOffOrAbsent() {
  const bool sOk = !pcaServo.present() || pcaServo.allOff();
  const bool mOk = !pcaMotor.present() || pcaMotor.allOff();
  return sOk && mOk;
}

static bool motorStop(uint8_t id) {
  if (id > 3) return false;
  if (!actuatorLock()) return false;
  const bool a = pcaMotor.setDuty(MOTOR_IN1[id], 0);
  const bool b = pcaMotor.setDuty(MOTOR_IN2[id], 0);
  if (a && b) motorActiveMask &= ~(1u << id);
  else lastMotorCommandUs = 0;
  actuatorUnlock();
  return a && b;
}

static void encoders_init() {
  gpio_config_t io = {};
  io.intr_type = GPIO_INTR_ANYEDGE;
  io.mode = GPIO_MODE_INPUT;
  // ENC1 临时给雷达 UART（IO9/10），仅初始化 ENC2
  const bool radarOwnsEnc1 =
      (PIN_RADAR_UART_RX == PIN_ENC1_A || PIN_RADAR_UART_TX == PIN_ENC1_A ||
       PIN_RADAR_UART_RX == PIN_ENC1_B || PIN_RADAR_UART_TX == PIN_ENC1_B);
  io.pin_bit_mask = (1ULL << PIN_ENC2_A) | (1ULL << PIN_ENC2_B);
  if (!radarOwnsEnc1) io.pin_bit_mask |= (1ULL << PIN_ENC1_A) | (1ULL << PIN_ENC1_B);
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&io);
  gpio_install_isr_service(0);
  if (!radarOwnsEnc1) gpio_isr_handler_add((gpio_num_t)PIN_ENC1_A, onEnc1, nullptr);
  gpio_isr_handler_add((gpio_num_t)PIN_ENC2_A, onEnc2, nullptr);
}

static bool setStby(bool on) {
  if (!actuatorLock()) return false;
  // 与 setPwmEnable 一致：无 XL 时只能关成功；U23 未焊/未上电时 allOff 视为成功
  if (!xl.present()) {
    if (!on) {
      flagStby = false;
      motorActiveMask = 0;
    }
    actuatorUnlock();
    return !on;
  }
  const bool motorSafe = !pcaMotor.present() || pcaMotor.allOff();
  bool ok = true;
  if (on) {
    ok = motorSafe;
    if (ok) {
      motorActiveMask = 0;
      ok = xl.setPin(XL_STBY, true);
    }
  } else {
    const bool stbyOk = xl.setPin(XL_STBY, false);
    if (stbyOk || motorSafe) motorActiveMask = 0;
    ok = stbyOk && motorSafe;
  }
  if (on) {
    if (ok) {
      flagStby = true;
      flagPeriphOff = false;
    }
  } else {
    flagStby = false;
  }
  actuatorUnlock();
  return ok;
}

static bool motorStopAll() {
  if (!actuatorLock()) return false;
  bool ok = true;
  for (uint8_t i = 0; i < 4; i++) {
    const bool a = pcaMotor.setDuty(MOTOR_IN1[i], 0);
    const bool b = pcaMotor.setDuty(MOTOR_IN2[i], 0);
    ok = a && b && ok;
  }
  if (ok) motorActiveMask = 0;
  else lastMotorCommandUs = 0;
  actuatorUnlock();
  return ok;
}

static bool motorDrive(uint8_t id, int dir, int dutyPct) {
  if (id > 3) return false;
  if (dutyPct < 0) dutyPct = 0;
  if (dutyPct > 100) dutyPct = 100;
  uint16_t duty = (uint16_t)((dutyPct * 4095L) / 100);
  if (dir == 0 || duty == 0) return motorStop(id);
  if (!actuatorLock()) return false;
  const bool clearedA = pcaMotor.setDuty(MOTOR_IN1[id], 0);
  const bool clearedB = pcaMotor.setDuty(MOTOR_IN2[id], 0);
  bool driven = false;
  if (clearedA && clearedB) {
    driven = dir > 0 ? pcaMotor.setDuty(MOTOR_IN1[id], duty)
                     : pcaMotor.setDuty(MOTOR_IN2[id], duty);
  }
  if (driven) {
    motorActiveMask |= (1u << id);
    lastMotorCommandUs = esp_timer_get_time();
  } else {
    const bool stoppedA = pcaMotor.setDuty(MOTOR_IN1[id], 0);
    const bool stoppedB = pcaMotor.setDuty(MOTOR_IN2[id], 0);
    if (stoppedA && stoppedB) motorActiveMask &= ~(1u << id);
  }
  actuatorUnlock();
  return driven;
}

static bool setPwmEnable(bool on) {
  if (!actuatorLock()) return false;
  // 无 XL9555 时无法控 OE#；关请求视为成功，开请求失败
  if (!xl.present()) {
    if (!on) {
      flagPwm = false;
      spotDutyPct[0] = spotDutyPct[1] = spotDutyPct[2] = 0;
      cfgSnapLed[0] = cfgSnapLed[1] = cfgSnapLed[2] = 0;
    }
    actuatorUnlock();
    return !on;
  }
  bool ok = true;
  if (on) {
    ok = pcaAllOffOrAbsent();
    if (ok) ok = xl.setPin(XL_OE, false);
  } else {
    ok = xl.setPin(XL_OE, true) && pcaAllOffOrAbsent();
  }
  if (on) {
    if (ok) {
      flagPwm = true;
      flagPeriphOff = false;
    }
  } else if (xl.present()) {
    flagPwm = false;
    // 硬件已 allOff：同步软件占空比，避免 fanIsOn()/OLED 与实况脱节
    spotDutyPct[0] = spotDutyPct[1] = spotDutyPct[2] = 0;
    cfgSnapLed[0] = cfgSnapLed[1] = cfgSnapLed[2] = 0;
  }
  actuatorUnlock();
  return ok;
}

static bool setAmp(bool on) {
  if (!actuatorLock()) return false;
  if (!xl.present()) {
    if (!on) flagAmp = false;
    actuatorUnlock();
    return !on;
  }
  const bool ok = xl.setPin(XL_AMP_SD, on);
  if (ok) {
    flagAmp = on;
    if (on) flagPeriphOff = false;
  }
  actuatorUnlock();
  return ok;
}

static bool ensureRecBuf() {
  if (recBuf) return true;
  recBuf = (int16_t *)heap_caps_malloc(REC_MAX_SAMPLES * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!recBuf)
    recBuf = (int16_t *)heap_caps_malloc(REC_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
  return recBuf != nullptr;
}

static void recTask(void *) {
  ESP_LOGI(TAG, "rec: start");
  while (recActive && recSamples < REC_MAX_SAMPLES) {
    size_t got = 0;
    const size_t room = REC_MAX_SAMPLES - recSamples;
    if (!board_i2s_mic_read_pcm16(recBuf + recSamples, room > 512 ? 512 : room, &got) || got == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    recSamples += got;
  }
  recActive = false;
  ESP_LOGI(TAG, "rec: stop samples=%u", (unsigned)recSamples);
  recTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

static bool recStart() {
  if (!i2sReady || !board_i2s_ready()) return false;
  if (!audioMutex || xSemaphoreTake(audioMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  bool ok = false;
  if (playBusy || recActive) {
    xSemaphoreGive(audioMutex);
    return false;
  }
  voice_sr_pause();
  if (!ensureRecBuf()) {
    voice_sr_resume();
    xSemaphoreGive(audioMutex);
    return false;
  }
  if (!board_i2s_mic_acquire()) {
    voice_sr_resume();
    xSemaphoreGive(audioMutex);
    return false;
  }
  recSamples = 0;
  recActive = true;
  if (xTaskCreate(recTask, "rec", 4096, nullptr, 5, &recTaskHandle) != pdPASS) {
    recActive = false;
    board_i2s_mic_release();
    voice_sr_resume();
    ok = false;
  } else {
    ok = true;
  }
  xSemaphoreGive(audioMutex);
  return ok;
}

static bool recStop() {
  if (!recActive && !recTaskHandle) return true;
  recActive = false;
  for (int i = 0; i < 100 && recTaskHandle; i++) vTaskDelay(pdMS_TO_TICKS(20));
  board_i2s_mic_release();
  voice_sr_resume();
  return !recActive;
}

static bool playPcmWithAmp(const int16_t *mono, size_t n) {
  if (!mono || n == 0 || !i2sReady) return false;
  if (!audioMutex || xSemaphoreTake(audioMutex, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  if (recActive || playBusy) {
    xSemaphoreGive(audioMutex);
    return false;
  }
  playBusy = true;
  xSemaphoreGive(audioMutex);

  const bool wasOn = flagAmp;
  bool ok = true;
  if (!wasOn) {
    ok = setAmp(true);
    if (ok) vTaskDelay(pdMS_TO_TICKS(8));
  }
  if (ok) ok = board_i2s_play_pcm16(mono, n);
  if (!wasOn) {
    if (!setAmp(false)) ok = false;
  }

  if (audioMutex) xSemaphoreTake(audioMutex, portMAX_DELAY);
  playBusy = false;
  if (audioMutex) xSemaphoreGive(audioMutex);
  return ok;
}

static void writeWavHeader(uint8_t *h, uint32_t dataBytes, uint32_t rate) {
  const uint32_t chunk = 36 + dataBytes;
  memcpy(h, "RIFF", 4);
  h[4] = chunk;
  h[5] = chunk >> 8;
  h[6] = chunk >> 16;
  h[7] = chunk >> 24;
  memcpy(h + 8, "WAVEfmt ", 8);
  h[16] = 16;
  h[17] = h[18] = h[19] = 0;  // fmt size
  h[20] = 1;
  h[21] = 0;  // PCM
  h[22] = 1;
  h[23] = 0;  // mono
  h[24] = rate;
  h[25] = rate >> 8;
  h[26] = rate >> 16;
  h[27] = rate >> 24;
  const uint32_t byteRate = rate * 2;
  h[28] = byteRate;
  h[29] = byteRate >> 8;
  h[30] = byteRate >> 16;
  h[31] = byteRate >> 24;
  h[32] = 2;
  h[33] = 0;  // block align
  h[34] = 16;
  h[35] = 0;  // bits
  memcpy(h + 36, "data", 4);
  h[40] = dataBytes;
  h[41] = dataBytes >> 8;
  h[42] = dataBytes >> 16;
  h[43] = dataBytes >> 24;
}

/** 解析 WAV：返回 PCM16 mono 指针与样点数；仅 PCM、16-bit、BOARD_I2S_RATE；立体声则下混。 */
static bool parseWavPcm16(uint8_t *buf, size_t len, int16_t **pcm, size_t *nSamples, bool *owned) {
  *pcm = nullptr;
  *nSamples = 0;
  *owned = false;
  if (!buf || len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return false;

  size_t pos = 12;
  uint16_t audioFmt = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  uint8_t *data = nullptr;
  uint32_t dataLen = 0;

  while (pos + 8 <= len) {
    const char *id = (const char *)(buf + pos);
    uint32_t sz = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
                 ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
    pos += 8;
    if (sz > len - pos) break;
    if (!memcmp(id, "fmt ", 4) && sz >= 16) {
      audioFmt = (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
      channels = (uint16_t)buf[pos + 2] | ((uint16_t)buf[pos + 3] << 8);
      rate = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
             ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
      bits = (uint16_t)buf[pos + 14] | ((uint16_t)buf[pos + 15] << 8);
    } else if (!memcmp(id, "data", 4)) {
      data = buf + pos;
      dataLen = sz;
      break;
    }
    pos += (size_t)((sz + 1) & ~1u);
  }

  if (!data || audioFmt != 1 || bits != 16 || rate != (uint32_t)BOARD_I2S_RATE) return false;
  if (channels != 1 && channels != 2) return false;

  const size_t frameBytes = (size_t)channels * 2;
  if (frameBytes == 0 || dataLen < frameBytes) return false;
  const size_t frames = dataLen / frameBytes;

  if (channels == 1) {
    *pcm = (int16_t *)data;
    *nSamples = frames;
    *owned = false;
    return true;
  }

  int16_t *mono = (int16_t *)heap_caps_malloc(frames * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mono) mono = (int16_t *)malloc(frames * sizeof(int16_t));
  if (!mono) return false;
  const int16_t *src = (const int16_t *)data;
  for (size_t i = 0; i < frames; i++) {
    const int32_t L = src[i * 2];
    const int32_t R = src[i * 2 + 1];
    mono[i] = (int16_t)((L + R) / 2);
  }
  *pcm = mono;
  *nSamples = frames;
  *owned = true;
  return true;
}

static bool setRadarPower(bool on);

static int localTimeMinute() {
  if (!timeSynced) return -1;
  const time_t t = time(nullptr);
  if (t < 1600000000) return -1;
  struct tm lt;
  localtime_r(&t, &lt);
  return lt.tm_hour * 60 + lt.tm_min;
}

/** 当前是否落在 [开, 关) 窗口；跨日（如 22:00→08:00）时窗外为关。关时刻起即关。 */
static bool radarScheduleWantOn(int curMin) {
  if (curMin < 0) return false;
  const int on = radarSchedOnMin;
  const int off = radarSchedOffMin;
  if (on < 0 && off < 0) return false;
  if (on < 0) return curMin < off;
  if (off < 0) return curMin >= on;
  if (on == off) return true;
  if (on < off) return curMin >= on && curMin < off;
  return curMin >= on || curMin < off;
}

static void radarScheduleApply() {
  if (!radarScheduleEnable || !timeSynced || flagPeriphOff) return;
  if (radarSchedOnMin < 0 && radarSchedOffMin < 0) return;
  const int curMin = localTimeMinute();
  if (curMin < 0) return;
  const bool want = radarScheduleWantOn(curMin);
  if (want == flagRadarPwr) return;
  if (setRadarPower(want)) {
    ESP_LOGI(TAG, "radar schedule: %s at %02d:%02d (window %d-%d)", want ? "ON" : "OFF",
             curMin / 60, curMin % 60, radarSchedOnMin, radarSchedOffMin);
    cfgSave();
  } else {
    ESP_LOGW(TAG, "radar schedule: %s failed", want ? "ON" : "OFF");
  }
}

static void localTimeStr(char *buf, size_t n) {
  buf[0] = 0;
  if (!timeSynced) return;
  const time_t t = time(nullptr);
  if (t < 1600000000) return;
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(buf, n, "%H:%M:%S", &lt);
}

static void formatHm(int min, char *buf, size_t n) {
  if (min < 0 || min >= 1440) {
    buf[0] = 0;
    return;
  }
  snprintf(buf, n, "%02d:%02d", min / 60, min % 60);
}

/** 空串或 null → -1（禁用）；否则解析 HH:MM */
static bool parseHm(const char *s, int &outMin) {
  if (!s || !*s) {
    outMin = -1;
    return true;
  }
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  outMin = h * 60 + m;
  return true;
}

static void sntpSyncCb(struct timeval *tv) {
  (void)tv;
  timeSynced = true;
  schedLastCheckUs = 0;
  ESP_LOGI(TAG, "SNTP time synced");
}

static void timeSyncOnGotIp() {
  if (!sntpStarted) {
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntpSyncCb);
    esp_sntp_init();
    sntpStarted = true;
    ESP_LOGI(TAG, "SNTP started (CST-8)");
  } else {
    esp_sntp_restart();
    ESP_LOGI(TAG, "SNTP restart");
  }
}

static void radarScheduleJsonInto(char *buf, size_t buflen) {
  char onHm[8], offHm[8], localTime[16];
  formatHm(radarSchedOnMin, onHm, sizeof(onHm));
  formatHm(radarSchedOffMin, offHm, sizeof(offHm));
  localTimeStr(localTime, sizeof(localTime));
  const bool active = radarScheduleEnable && timeSynced && !flagPeriphOff &&
                      (radarSchedOnMin >= 0 || radarSchedOffMin >= 0);
  const int curMin = localTimeMinute();
  const bool wantOn = active && radarScheduleWantOn(curMin);
  snprintf(buf, buflen,
           ",\"timeSynced\":%s,\"localTime\":\"%s\""
           ",\"schedule\":{\"enable\":%s,\"on\":\"%s\",\"off\":\"%s\",\"active\":%s,\"wantOn\":%s}",
           timeSynced ? "true" : "false", localTime, radarScheduleEnable ? "true" : "false", onHm,
           offHm, active ? "true" : "false", wantOn ? "true" : "false");
}

static std::string radarIgnoreJson() {
  std::string s = ",\"ignore\":{\"enable\":";
  s += radarIgnoreEnable ? "true" : "false";
  const int f0 = radarIgnoreCount > 0 ? radarIgnoreSec[0].from : 0;
  const int t0 = radarIgnoreCount > 0 ? radarIgnoreSec[0].to : -60;
  char tmp[48];
  snprintf(tmp, sizeof(tmp), ",\"from\":%d,\"to\":%d,\"count\":%d,\"sectors\":[", f0, t0,
           radarIgnoreCount);
  s += tmp;
  for (int i = 0; i < radarIgnoreCount; i++) {
    snprintf(tmp, sizeof(tmp), "%s{\"from\":%d,\"to\":%d}", i ? "," : "", radarIgnoreSec[i].from,
             radarIgnoreSec[i].to);
    s += tmp;
  }
  s += "]}";
  return s;
}

static void radarAppendPowerAndTimer(std::string &body) {
  if (body.empty() || body.back() != '}') return;
  body.pop_back();
  body += ",\"power\":";
  body += flagRadarPwr ? "true" : "false";
  char sbuf[200];
  radarScheduleJsonInto(sbuf, sizeof(sbuf));
  body += sbuf;
  body += radarIgnoreJson();
  body += '}';
}

static void radarScheduleTick() {
  if (!radarScheduleEnable || !timeSynced || flagPeriphOff) return;
  const int64_t nowUs = esp_timer_get_time();
  if (schedLastCheckUs != 0 && (nowUs - schedLastCheckUs) < 1000000LL) return;
  schedLastCheckUs = nowUs;
  radarScheduleApply();
}

/** Q4 P-MOS：拉低 IO0_1 = 开雷达 3V3；开电即自动查询 */
static bool setRadarPower(bool on) {
  if (!actuatorLock()) return false;
  if (!xl.present()) {
    // 无扩展芯片无法开关雷达供电
    if (!on) {
      flagRadarPwr = false;
      radar_on_power(false);
    }
    actuatorUnlock();
    return !on;
  }
  const bool ok = true;  // v5: no MOSFET; soft power flag only
  if (ok) {
    flagRadarPwr = on;
    if (on) flagPeriphOff = false;
  }
  actuatorUnlock();
  if (ok) radar_on_power(on);
  return ok;
}

static bool servoAngle(uint8_t id, int angle) {
  if (id >= SERVO_COUNT) return false;
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  uint16_t us =
      SERVO_US_MIN + (uint16_t)((uint32_t)(SERVO_US_MAX - SERVO_US_MIN) * angle / 180);
  const bool ok = pcaServo.setPulseUs(SERVO_CH[id], us);
  if (ok) {
    servoAngleDeg[id] = angle;
    cfgSnapSrv[id] = angle;
  }
  return ok;
}

/** 风扇强度：LED_1 × LED_ALL（两者共同决定有效占空比）。 */
static int fanIntensityPct() {
  int a = spotDutyPct[0];
  int b = spotDutyPct[2];
  if (a < 0) a = 0;
  if (b < 0) b = 0;
  if (a > 100) a = 100;
  if (b > 100) b = 100;
  return (a * b) / 100;
}

static int oledLastFanPct = -1;
static char oledLastIp[16] = {0};

static void oledShowHome(bool force = false) {
  const int fan = fanIntensityPct();
  if (!force && fan == oledLastFanPct && strncmp(oledLastIp, ipStr, sizeof(oledLastIp)) == 0) return;
  if (!oled.present() || !oledMutex) return;
  if (xSemaphoreTake(oledMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  oled.showHome(ipStr[0] ? ipStr : nullptr, fan);
  oledLastFanPct = fan;
  strncpy(oledLastIp, ipStr, sizeof(oledLastIp) - 1);
  oledLastIp[sizeof(oledLastIp) - 1] = 0;
  xSemaphoreGive(oledMutex);
}

/** 上次有效风扇强度（关之前记住；语音/手动开时恢复）。 */
static int fanSavedLed1 = 100;
static int fanSavedLedAll = 100;

static void fanRememberIfOn() {
  if (spotDutyPct[0] > 0 && spotDutyPct[2] > 0) {
    fanSavedLed1 = spotDutyPct[0];
    fanSavedLedAll = spotDutyPct[2];
  }
}

static bool setSpotDuty(uint8_t id, int dutyPct) {
  if (id >= SPOT_COUNT) return false;
  if (dutyPct < 0) dutyPct = 0;
  if (dutyPct > 100) dutyPct = 100;
  uint16_t d = (uint16_t)((dutyPct * 4095L) / 100);
  const bool ok = pcaMotor.setDuty(SPOT_CH[id], d);
  if (ok) {
    spotDutyPct[id] = dutyPct;
    cfgSnapLed[id] = dutyPct;
    if (id == 0 || id == 2) {
      fanRememberIfOn();
      oledShowHome(false);
    }
  }
  return ok;
}

/** 雷达 → 仅控 LED_1（风扇）；开时恢复上次 LED_1×LED_ALL，关时只关 LED_1。 */
static bool fanAutoEnable = false;  // Web「控风扇」，默认关
static bool fanAutoOn = false;
static bool fanGestureEnable = false;  // Web「手势切档」，可与控风扇同时开
static int fanGearPct = 100;           // 手势档位 50/75/100；控风扇开时按此强度
static bool fanSuppressAutoOn = false; // 历史：切到关时抑制；手势循环已不含关
static const int64_t FAN_SAMPLE_US = 2000000;  // 每 2s 取样
static const uint8_t FAN_CONFIRM_ON = 1;       // 有人：1 次即开
static const uint8_t FAN_CONFIRM_OFF = 3;      // 无人：连续 3 次才关
static int64_t fanLastSampleUs = 0;
static uint8_t fanConfirmCount = 0;
static bool fanSamplePresent = false;
static char fanReason[64] = "未启用";
static char fanPhase[24] = "disabled";  // disabled/idle/arming/on/holdoff
static char fanLastAction[96] = "—";

static void fanSetPhase(const char *phase) { snprintf(fanPhase, sizeof(fanPhase), "%s", phase); }

/** 任意改风量后同步手势档位，避免语音/滑条与手势/自动恢复脱节。 */
static void syncFanGearFromIntensity(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  fanGearPct = pct;
}

/** 网页/API 改 LED_1 或 LED_ALL 后：同步档位，并与控风扇状态对齐（不关联动）。 */
static void onFanOutputChangedFromUi() {
  const int inten = fanIntensityPct();
  syncFanGearFromIntensity(inten);
  if (inten <= 0) {
    fanAutoOn = false;
    if (fanAutoEnable) fanSuppressAutoOn = true;
  } else {
    fanSuppressAutoOn = false;
    if (fanAutoEnable) fanAutoOn = true;
  }
}

/** 关掉雷达联动（语音/手动 power 时避免抢控）。手势切档保持，仅同步档位由调用方负责。 */
static void fanDisableAuto(const char *why) {
  fanAutoEnable = false;
  fanAutoOn = false;
  fanConfirmCount = 0;
  fanLastSampleUs = 0;
  fanSuppressAutoOn = false;
  fanSetPhase("disabled");
  snprintf(fanReason, sizeof(fanReason), "%s", why ? why : "未启用");
}

/**
 * 手动/语音开关风扇：开=恢复上次 LED_1×LED_ALL；关=先记住再只关 LED_1。
 * 同时关闭雷达控风扇。
 */
static bool applyManualFan(bool on, const char *src) {
  if (!pcaMotor.present()) return false;
  fanDisableAuto(src && strstr(src, "语音") ? "语音接管" : "手动接管");
  if (on) {
    if (!flagPwm && !setPwmEnable(true)) return false;
    if (!actuatorLock()) return false;
    int a = fanSavedLedAll > 0 ? fanSavedLedAll : 100;
    int b = fanSavedLed1 > 0 ? fanSavedLed1 : 100;
    bool ok = setSpotDuty(2, a);
    if (ok) ok = setSpotDuty(0, b);
    actuatorUnlock();
    if (ok) {
      syncFanGearFromIntensity(fanIntensityPct());
      snprintf(fanLastAction, sizeof(fanLastAction), "开风扇 %d%%（%s）", fanIntensityPct(),
               src ? src : "?");
      ESP_LOGI(TAG, "fan: MANUAL ON intensity=%d src=%s", fanIntensityPct(), src ? src : "?");
      cfgSave();
    }
    return ok;
  }
  if (!actuatorLock()) return false;
  fanRememberIfOn();
  const bool ok = setSpotDuty(0, 0);
  actuatorUnlock();
  if (ok) {
    syncFanGearFromIntensity(0);
    snprintf(fanLastAction, sizeof(fanLastAction), "关风扇（%s，已记强度 %d/%d）", src ? src : "?",
             fanSavedLed1, fanSavedLedAll);
    ESP_LOGI(TAG, "fan: MANUAL OFF saved=%d/%d src=%s", fanSavedLed1, fanSavedLedAll,
             src ? src : "?");
    cfgSave();
  }
  return ok;
}

static int fanSavedIntensityPct() {
  int a = fanSavedLed1;
  int b = fanSavedLedAll;
  if (a < 0) a = 0;
  if (b < 0) b = 0;
  if (a > 100) a = 100;
  if (b > 100) b = 100;
  return (a * b) / 100;
}

static bool fanIsOn() { return spotDutyPct[0] > 0 && spotDutyPct[2] > 0; }

/** 语音设绝对风量：LED_ALL=100、LED_1=pct（有效强度≈pct）。低于 20 抬到 20；0 则关。 */
static bool applyManualFanLevel(int pct, const char *src) {
  if (!pcaMotor.present()) return false;
  if (pct <= 0) return applyManualFan(false, src);
  if (pct < 20) pct = 20;
  if (pct > 100) pct = 100;
  fanDisableAuto(src && strstr(src, "语音") ? "语音接管" : "手动接管");
  if (!flagPwm && !setPwmEnable(true)) return false;
  if (!actuatorLock()) return false;
  bool ok = setSpotDuty(2, 100);
  if (ok) ok = setSpotDuty(0, pct);
  actuatorUnlock();
  if (ok) {
    syncFanGearFromIntensity(fanIntensityPct());
    snprintf(fanLastAction, sizeof(fanLastAction), "风量 %d%%（%s）", fanIntensityPct(),
             src ? src : "?");
    ESP_LOGI(TAG, "fan: LEVEL %d%% src=%s", fanIntensityPct(), src ? src : "?");
    cfgSave();
  }
  return ok;
}

/** ±步长；关着说「大一点」则先按记忆/50% 再加大；关着说「小一点」忽略。 */
static bool applyManualFanDelta(int delta, const char *src) {
  int cur = 0;
  if (fanIsOn()) {
    cur = fanIntensityPct();
  } else {
    if (delta <= 0) return false;
    cur = fanSavedIntensityPct();
    if (cur < 20) cur = 50;
  }
  int next = cur + delta;
  if (next < 20) next = 20;
  if (next > 100) next = 100;
  return applyManualFanLevel(next, src);
}

static void voiceAckBeep() {
  if (!i2sReady) return;
  const bool was = flagAmp;
  if (!was) setAmp(true);
  if (!was) vTaskDelay(pdMS_TO_TICKS(5));
  board_i2s_beep(60);
  if (!was) setAmp(false);
}

static int clampRadarAng(int a) {
  if (a < -60) return -60;
  if (a > 60) return 60;
  return a;
}

static bool radarIgnoreAdd(int from, int to) {
  if (radarIgnoreCount >= RADAR_IGN_MAX) return false;
  radarIgnoreSec[radarIgnoreCount].from = clampRadarAng(from);
  radarIgnoreSec[radarIgnoreCount].to = clampRadarAng(to);
  radarIgnoreCount++;
  return true;
}

static bool radarIgnoreDel(int idx) {
  if (idx < 0 || idx >= radarIgnoreCount) return false;
  for (int i = idx; i < radarIgnoreCount - 1; i++) radarIgnoreSec[i] = radarIgnoreSec[i + 1];
  radarIgnoreCount--;
  return true;
}

static bool radarIgnoreSet(int idx, int from, int to) {
  if (idx < 0 || idx >= radarIgnoreCount) return false;
  radarIgnoreSec[idx].from = clampRadarAng(from);
  radarIgnoreSec[idx].to = clampRadarAng(to);
  return true;
}

static bool radarSectorHit(const RadarIgnSec &s, int16_t deg) {
  int lo = s.from, hi = s.to;
  if (lo > hi) {
    const int t = lo;
    lo = hi;
    hi = t;
  }
  return deg >= lo && deg <= hi;
}

static bool radarAngleIgnored(int16_t deg) {
  if (!radarIgnoreEnable || radarIgnoreCount <= 0) return false;
  for (int i = 0; i < radarIgnoreCount; i++) {
    if (radarSectorHit(radarIgnoreSec[i], deg)) return true;
  }
  return false;
}

/** 可用扇区内最近目标。忽略开关关时任意有效目标。 */
static bool radarPickUsable(const RadarSnapshot &rs, uint16_t *range_mm, int16_t *angle_deg) {
  uint16_t best = 0;
  int16_t bestA = 0;
  bool found = false;
  auto consider = [&](bool valid, uint16_t r, int16_t a) {
    if (!valid) return;
    if (radarAngleIgnored(a)) return;
    if (!found || (r > 0 && (best == 0 || r < best))) {
      found = true;
      best = r;
      bestA = a;
    }
  };
  consider(rs.primary_valid || rs.is_detected != 0, rs.range_mm, rs.angle_deg);
  if (rs.multi_valid) {
    for (uint8_t i = 0; i < rs.obj_num && i < RADAR_OBJ_MAX; i++)
      consider(rs.objs[i].valid, rs.objs[i].range_mm, rs.objs[i].angle_deg);
  }
  if (!found) return false;
  if (range_mm) *range_mm = best;
  if (angle_deg) *angle_deg = bestA;
  return true;
}

/** 明确有人：主目标 / is_detected / 明显运动。不含单独呼吸、微动、OUT。 */
static bool classifyFanPresent(const RadarSnapshot &rs, char *reason, size_t n) {
  if (!flagRadarPwr) {
    snprintf(reason, n, "雷达未供电");
    return false;
  }
  uint16_t range = 0;
  int16_t ang = 0;
  const bool usable = radarPickUsable(rs, &range, &ang);
  if (radarIgnoreEnable && radarIgnoreCount > 0 && !usable) {
    snprintf(reason, n, "无人（忽略扇区）");
    return false;
  }
  if (rs.gesture[0] && strstr(rs.gesture, "扫") != nullptr) {
    snprintf(reason, n, "手势:%.20s", rs.gesture);
    return true;
  }
  if (rs.det_result & 0x07) {
    const char *src = rs.det_text[0] ? rs.det_text : rs.gesture;
    snprintf(reason, n, "运动:%.20s", src);
    return true;
  }
  if (usable) {
    if (range > 0)
      snprintf(reason, n, "人 %u.%um", (unsigned)(range / 1000),
               (unsigned)((range % 1000) / 100));
    else
      snprintf(reason, n, "检测到人");
    return true;
  }
  snprintf(reason, n, "无人");
  return false;
}

static bool applyRadarFan(bool on) {
  if (!pcaMotor.present()) return false;
  if (on) {
    // 手势切档开启时：按档位恢复；档位 0 则保持关（控风扇仍记「有人会话」由调用方处理）
    if (fanGestureEnable && fanGearPct <= 0) return true;
    if (!flagPwm && !setPwmEnable(true)) return false;
    if (!actuatorLock()) return false;
    int a, b;
    if (fanGestureEnable) {
      a = 100;
      b = fanGearPct;
      if (b < 1) b = 1;
      if (b > 100) b = 100;
    } else {
      a = fanSavedLedAll > 0 ? fanSavedLedAll : 100;
      b = fanSavedLed1 > 0 ? fanSavedLed1 : 100;
    }
    bool ok = setSpotDuty(2, a);
    if (ok) ok = setSpotDuty(0, b);
    actuatorUnlock();
    if (ok) cfgMarkDirty();
    return ok;
  }
  if (!actuatorLock()) return false;
  fanRememberIfOn();
  const bool ok = setSpotDuty(0, 0);
  actuatorUnlock();
  if (ok) cfgMarkDirty();
  return ok;
}

static void updateRadarFan(const RadarSnapshot &rs) {
  if (!fanAutoEnable) {
    if (fanAutoOn) {
      if (applyRadarFan(false)) {
        fanAutoOn = false;
        snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：联动已关闭");
        ESP_LOGI(TAG, "fan: OFF (auto disabled)");
      }
    }
    fanConfirmCount = 0;
    fanSetPhase("disabled");
    snprintf(fanReason, sizeof(fanReason), "未启用");
    return;
  }

  if (!flagRadarPwr) {
    if (fanAutoOn) {
      if (applyRadarFan(false)) {
        fanAutoOn = false;
        snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：雷达未供电");
        ESP_LOGI(TAG, "fan: OFF (radar power off)");
      }
    }
    fanConfirmCount = 0;
    fanSetPhase("idle");
    snprintf(fanReason, sizeof(fanReason), "雷达未供电");
    return;
  }

  const int64_t now = esp_timer_get_time();
  if (fanLastSampleUs != 0 && (now - fanLastSampleUs) < FAN_SAMPLE_US) {
    if (fanAutoOn)
      fanSetPhase("on");
    else if (fanConfirmCount > 0 && fanSamplePresent)
      fanSetPhase("arming");
    else if (fanConfirmCount > 0 && !fanSamplePresent)
      fanSetPhase("holdoff");
    else
      fanSetPhase("idle");
    return;
  }
  fanLastSampleUs = now;

  char reason[64];
  const bool present = classifyFanPresent(rs, reason, sizeof(reason));
  snprintf(fanReason, sizeof(fanReason), "%s", reason);

  if (fanConfirmCount == 0 || present != fanSamplePresent) {
    fanSamplePresent = present;
    fanConfirmCount = 1;
  } else {
    fanConfirmCount = (uint8_t)((fanConfirmCount < 250) ? fanConfirmCount + 1 : 250);
  }

  const uint8_t need = fanSamplePresent ? FAN_CONFIRM_ON : FAN_CONFIRM_OFF;
  if (fanConfirmCount < need) {
    fanSetPhase(fanSamplePresent ? (fanAutoOn ? "on" : "arming")
                                 : (fanAutoOn ? "holdoff" : "idle"));
    ESP_LOGI(TAG, "fan: sample %s %u/%u (%s)", fanSamplePresent ? "present" : "absent",
             (unsigned)fanConfirmCount, (unsigned)need, reason);
    return;
  }

  // 无人后允许再次自动开；手势档位=0 时有人也不自动开
  if (!fanSamplePresent) fanSuppressAutoOn = false;

  if (fanSamplePresent && !fanAutoOn) {
    if (fanSuppressAutoOn || (fanGestureEnable && fanGearPct <= 0)) {
      fanSetPhase("idle");
      if (fanGestureEnable && fanGearPct <= 0)
        snprintf(fanReason, sizeof(fanReason), "有人·档位关");
      return;
    }
    if (applyRadarFan(true)) {
      fanAutoOn = true;
      fanSetPhase("on");
      snprintf(fanLastAction, sizeof(fanLastAction), "开风扇 %d%%：%s", fanIntensityPct(), reason);
      ESP_LOGI(TAG, "fan: ON intensity=%d reason=%s", fanIntensityPct(), reason);
    }
  } else if (!fanSamplePresent && fanAutoOn) {
    if (applyRadarFan(false)) {
      fanAutoOn = false;
      fanSetPhase("idle");
      snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：%s", reason);
      ESP_LOGI(TAG, "fan: OFF reason=%s", reason);
    }
  } else {
    // 会话仍为「开」但输出被 PWM/滑条等清掉时，有人则按档位补开
    if (fanAutoOn && fanSamplePresent && !fanIsOn() &&
        !(fanSuppressAutoOn || (fanGestureEnable && fanGearPct <= 0))) {
      if (applyRadarFan(true)) {
        snprintf(fanLastAction, sizeof(fanLastAction), "开风扇 %d%%：恢复输出", fanIntensityPct());
        ESP_LOGI(TAG, "fan: RECOVER intensity=%d", fanIntensityPct());
      }
    }
    fanSetPhase(fanAutoOn ? "on" : "idle");
  }
}

/** 近距手掌停留切档：50% → 75% → 100% 循环。可与「控风扇」同时开。默认关。 */
static const uint16_t GEST_NEAR_ENTER_MM = 200;  // 近距，降低路过误触
static const uint16_t GEST_NEAR_EXIT_MM = 350;
static const int64_t GEST_HOLD_US = 2000000;
static bool gestInNear = false;
static bool gestFired = false;  // 已切档，等手离开再武装
static int64_t gestNearSinceUs = 0;
static int64_t gestHoldElapsedMs = 0;
static uint16_t gestLastRangeMm = 0;
static char gestPhase[24] = "disabled";  // disabled/no_power/idle/holding/wait_leave

static void gestResetTracking() {
  gestInNear = false;
  gestFired = false;
  gestNearSinceUs = 0;
  gestHoldElapsedMs = 0;
  gestLastRangeMm = 0;
}

static uint16_t nearestRangeMm(const RadarSnapshot &rs) {
  uint16_t best = 0;
  if (radarPickUsable(rs, &best, nullptr)) return best;
  return 0;
}

/** 档位索引：0=50% / 1=75% / 2=100%；低于约 25% 返回 -1（下一档从 50% 起）。 */
static int fanGearIndex() {
  if (fanGearPct >= 88) return 2;
  if (fanGearPct >= 62) return 1;
  if (fanGearPct >= 25) return 0;
  return -1;
}

/** 供 JSON / UI：实际输出档位（关着时也反映 sticky 手势档）。 */
static int fanLevelIndex() {
  if (fanGestureEnable) {
    const int i = fanGearIndex();
    return i < 0 ? 0 : i;
  }
  if (!fanIsOn()) return 0;
  const int cur = fanIntensityPct();
  if (cur >= 88) return 2;
  if (cur >= 62) return 1;
  return 0;
}

/** 手势设档：不关闭「控风扇」；与自动开/关分工。 */
static bool applyGestureFanLevel(int pct) {
  if (!pcaMotor.present()) return false;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  fanGearPct = pct;

  if (pct <= 0) {
    if (!actuatorLock()) return false;
    fanRememberIfOn();
    const bool ok = setSpotDuty(0, 0);
    actuatorUnlock();
    if (ok) {
      fanAutoOn = false;
      if (fanAutoEnable) fanSuppressAutoOn = true;
      cfgMarkDirty();
    }
    return ok;
  }

  fanSuppressAutoOn = false;
  if (!flagPwm && !setPwmEnable(true)) return false;
  if (!actuatorLock()) return false;
  bool ok = setSpotDuty(2, 100);
  if (ok) ok = setSpotDuty(0, pct);
  actuatorUnlock();
  if (ok) {
    if (fanAutoEnable) fanAutoOn = true;
    cfgMarkDirty();
  }
  return ok;
}

static void updateFanGesture(const RadarSnapshot &rs) {
  if (!fanGestureEnable) {
    gestResetTracking();
    snprintf(gestPhase, sizeof(gestPhase), "disabled");
    return;
  }
  if (!flagRadarPwr) {
    gestResetTracking();
    snprintf(gestPhase, sizeof(gestPhase), "no_power");
    return;
  }

  const int64_t now = esp_timer_get_time();
  const uint16_t range = nearestRangeMm(rs);
  gestLastRangeMm = range;

  bool near = false;
  if (range > 0) {
    if (gestInNear)
      near = range <= GEST_NEAR_EXIT_MM;
    else
      near = range <= GEST_NEAR_ENTER_MM;
  }

  if (!near) {
    gestInNear = false;
    gestNearSinceUs = 0;
    gestFired = false;
    gestHoldElapsedMs = 0;
    snprintf(gestPhase, sizeof(gestPhase), "idle");
    return;
  }

  gestInNear = true;
  if (gestFired) {
    snprintf(gestPhase, sizeof(gestPhase), "wait_leave");
    gestHoldElapsedMs = GEST_HOLD_US / 1000;
    return;
  }

  if (gestNearSinceUs == 0) gestNearSinceUs = now;
  const int64_t held = now - gestNearSinceUs;
  gestHoldElapsedMs = held / 1000;
  if (held < GEST_HOLD_US) {
    snprintf(gestPhase, sizeof(gestPhase), "holding");
    return;
  }

  static const int LEVELS[] = {50, 75, 100};
  const int idx = fanGearIndex();
  const int next = (idx < 0) ? LEVELS[0] : LEVELS[(idx + 1) % 3];
  if (applyGestureFanLevel(next)) {
    gestFired = true;
    snprintf(gestPhase, sizeof(gestPhase), "wait_leave");
    snprintf(fanLastAction, sizeof(fanLastAction), "手势切档 → %d%%（近距 %umm 停留2s）", next,
             (unsigned)range);
    ESP_LOGI(TAG, "fan: GESTURE gear=%d%% range=%umm auto=%d", next, (unsigned)range,
             fanAutoEnable ? 1 : 0);
  } else {
    snprintf(gestPhase, sizeof(gestPhase), "holding");
    ESP_LOGW(TAG, "fan: GESTURE apply failed next=%d", next);
  }
}

static bool emergencyStop() {
  if (!flagPeriphOff) {
    savedPwm = flagPwm;
    savedAmp = flagAmp;
    savedRadarPwr = flagRadarPwr;
    savedFanAuto = fanAutoEnable;
    savedFanGesture = fanGestureEnable;
  }
  recStop();
  if (!actuatorLock()) return false;
  bool oeOk = true, ampOk = true, radarOk = true;
  if (xl.present()) {
    oeOk = xl.setPin(XL_OE, true);
    ampOk = xl.setPin(XL_AMP_SD, false);
    radarOk = true;  // v5: external VCC, soft flag
  }
  const bool pwmOk = pcaAllOffOrAbsent();
  if (oeOk) flagPwm = false;
  if (ampOk) flagAmp = false;
  if (radarOk) {
    flagRadarPwr = false;
    radar_on_power(false);
  }
  fanAutoEnable = false;
  fanAutoOn = false;
  fanConfirmCount = 0;
  fanLastSampleUs = 0;
  fanSetPhase("disabled");
  fanGestureEnable = false;
  fanSuppressAutoOn = false;
  gestResetTracking();
  snprintf(gestPhase, sizeof(gestPhase), "disabled");
  snprintf(fanReason, sizeof(fanReason), "关闭所有外设");
  snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：关闭所有外设");
  spotDutyPct[0] = spotDutyPct[1] = spotDutyPct[2] = 0;
  flagPeriphOff = true;
  actuatorUnlock();
  radar_set_enabled(false);
  oledShowHome(true);
  return oeOk && ampOk && radarOk && pwmOk;
}

/** 关闭「关闭所有外设」：按关之前的快照恢复 PWM / 功放 / 雷达 / 风扇联动 / LED·舵机 */
static bool releasePeripherals() {
  flagPeriphOff = false;
  bool ok = true;
  if (savedPwm && !setPwmEnable(true)) ok = false;
  if (savedPwm && flagPwm && !cfgRestoreOutputs()) ok = false;
  if (savedAmp && !setAmp(true)) ok = false;
  if (savedRadarPwr && !setRadarPower(true)) ok = false;
  if (savedFanAuto) {
    fanAutoEnable = true;
    fanConfirmCount = 0;
    fanLastSampleUs = 0;
    fanSetPhase(flagRadarPwr ? "idle" : "disabled");
    snprintf(fanReason, sizeof(fanReason), "已恢复外设");
    snprintf(fanLastAction, sizeof(fanLastAction), "恢复风扇联动");
  } else {
    snprintf(fanReason, sizeof(fanReason), "已恢复外设");
    snprintf(fanLastAction, sizeof(fanLastAction), "允许单独开启");
  }
  if (savedFanGesture) {
    fanGestureEnable = true;
    gestResetTracking();
    snprintf(gestPhase, sizeof(gestPhase), flagRadarPwr ? "idle" : "no_power");
  }
  if (fanIsOn()) fanAutoOn = fanAutoEnable;
  oledShowHome(true);
  return ok;
}

static void shutdownTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(500));
  ESP_LOGW(TAG, "shutdown: deep sleep");
  emergencyStop();
  radar_stop();
  if (oled.present() && oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    oled.clear();
    oled.show();
    xSemaphoreGive(oledMutex);
  }
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_deep_sleep_start();
}

// ---- handlers ----
static esp_err_t handleOptions(httpd_req_t *req) {
  addCors(req);
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t handleRoot(httpd_req_t *req) {
  addCors(req);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleRadarPage(httpd_req_t *req) {
  addCors(req);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, RADAR_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleRadarGet(httpd_req_t *req) {
  char buf[1536];
  radar_json_summary(buf, sizeof(buf));
  std::string body = buf;
  radarAppendPowerAndTimer(body);
  return sendJson(req, 200, body);
}

static esp_err_t handleRadarLive(httpd_req_t *req) {
  char buf[3072];
  radar_json_live(buf, sizeof(buf));
  std::string body = buf;
  if (!body.empty() && body.back() == '}') {
    body.pop_back();
    body += radarIgnoreJson();
    body += '}';
  }
  return sendJson(req, 200, body);
}

static esp_err_t handleLogs(httpd_req_t *req) {
  const std::string q = queryStr(req);
  char value[32];
  uint64_t after = 0;
  size_t limit = 32;
  if (queryGet(q, "after", value, sizeof(value))) after = strtoull(value, nullptr, 10);
  if (queryGet(q, "limit", value, sizeof(value))) {
    const long parsed = strtol(value, nullptr, 10);
    if (parsed > 0) limit = static_cast<size_t>(parsed);
  }
  return sendJson(req, 200, device_log_json(after, limit));
}

static esp_err_t handleRadarPost(httpd_req_t *req) {
  auto a = loadArgs(req);
  bool changed = false;
  if (argsHasKey(a, "scheduleEnable")) {
    radarScheduleEnable = argBool(a, "scheduleEnable", false);
    changed = true;
  }
  if (argsHasKey(a, "scheduleOn")) {
    int m = 0;
    if (!parseHm(argStr(a, "scheduleOn", "").c_str(), m))
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"scheduleOn use HH:MM or empty\"}");
    radarSchedOnMin = m;
    changed = true;
  }
  if (argsHasKey(a, "scheduleOff")) {
    int m = 0;
    if (!parseHm(argStr(a, "scheduleOff", "").c_str(), m))
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"scheduleOff use HH:MM or empty\"}");
    radarSchedOffMin = m;
    changed = true;
  }
  if (argsHasKey(a, "power")) {
    const bool on = argBool(a, "power", true);
    if (!setRadarPower(on))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"radar power write failed\"}");
    cfgSave();
    changed = true;
  }
  if (argsHasKey(a, "ignoreEnable") || argsHasKey(a, "ignoreNeg60")) {
    radarIgnoreEnable = argsHasKey(a, "ignoreEnable") ? argBool(a, "ignoreEnable", false)
                                                      : argBool(a, "ignoreNeg60", false);
    cfgSave();
    changed = true;
  }
  if (argsHasKey(a, "ignoreClear") && argBool(a, "ignoreClear", false)) {
    radarIgnoreCount = 0;
    cfgSave();
    changed = true;
  }
  if (argsHasKey(a, "ignoreAdd") && argBool(a, "ignoreAdd", false)) {
    const int from = argsHasKey(a, "ignoreFrom") ? argInt(a, "ignoreFrom", 0) : 0;
    const int to = argsHasKey(a, "ignoreTo") ? argInt(a, "ignoreTo", -60) : -60;
    if (!radarIgnoreAdd(from, to))
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"ignore sectors full (max 8)\"}");
    cfgSave();
    changed = true;
  } else if (argsHasKey(a, "ignoreDel")) {
    if (!radarIgnoreDel(argInt(a, "ignoreDel", -1)))
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"ignoreDel index out of range\"}");
    cfgSave();
    changed = true;
  } else if (argsHasKey(a, "ignoreId")) {
    const int idx = argInt(a, "ignoreId", -1);
    if (idx < 0 || idx >= radarIgnoreCount)
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"ignoreId out of range\"}");
    const int from = argsHasKey(a, "ignoreFrom") ? argInt(a, "ignoreFrom", radarIgnoreSec[idx].from)
                                                 : radarIgnoreSec[idx].from;
    const int to = argsHasKey(a, "ignoreTo") ? argInt(a, "ignoreTo", radarIgnoreSec[idx].to)
                                             : radarIgnoreSec[idx].to;
    radarIgnoreSet(idx, from, to);
    cfgSave();
    changed = true;
  } else if (argsHasKey(a, "ignoreFrom") || argsHasKey(a, "ignoreTo")) {
    const int from = argsHasKey(a, "ignoreFrom")
                         ? argInt(a, "ignoreFrom", 0)
                         : (radarIgnoreCount > 0 ? radarIgnoreSec[0].from : 0);
    const int to = argsHasKey(a, "ignoreTo")
                       ? argInt(a, "ignoreTo", -60)
                       : (radarIgnoreCount > 0 ? radarIgnoreSec[0].to : -60);
    if (radarIgnoreCount <= 0) {
      if (!radarIgnoreAdd(from, to))
        return sendJson(req, 400, "{\"ok\":false,\"error\":\"ignore sectors full (max 8)\"}");
    } else {
      radarIgnoreSet(0, from, to);
    }
    cfgSave();
    changed = true;
  }
  if (changed) {
    if (argsHasKey(a, "scheduleEnable") || argsHasKey(a, "scheduleOn") ||
        argsHasKey(a, "scheduleOff")) {
      cfgSave();
      radarScheduleApply();
    }
    char buf[1536];
    radar_json_summary(buf, sizeof(buf));
    std::string body = buf;
    radarAppendPowerAndTimer(body);
    return sendJson(req, 200, body);
  }
  // 旧客户端若仍传 on=：忽略（供电开即查询）
  if (argsHasKey(a, "on")) {
    char buf[1536];
    radar_json_summary(buf, sizeof(buf));
    std::string body = buf;
    if (!body.empty() && body.back() == '}') {
      body.pop_back();
      body += ",\"power\":";
      body += flagRadarPwr ? "true" : "false";
      body += ",\"note\":\"acquire removed; use power only\"";
      body += '}';
    }
    return sendJson(req, 200, body);
  }
  const std::string cmd = argStr(a, "cmd", "");
  bool commandOk = false;
  if (cmd == "version") commandOk = radar_cmd_get_version();
  else if (cmd == "poll") commandOk = radar_cmd_get_det();
  else return sendJson(req, 400,
                       "{\"ok\":false,\"error\":\"use power, scheduleEnable/On/Off, ignoreEnable/Add/Del/Id/From/To, or cmd=version|poll\"}");
  if (!commandOk) {
    if (cmd == "poll" && !flagRadarPwr)
      return sendJson(req, 409, "{\"ok\":false,\"error\":\"radar power is off\"}");
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"radar UART write failed\"}");
  }
  char buf[1536];
  radar_json_summary(buf, sizeof(buf));
  return sendJson(req, 200, buf);
}

static esp_err_t handleStby(httpd_req_t *req) {
  auto a = loadArgs(req);
  bool on = argBool(a, "on", true);
  if (!setStby(on)) {
    const char *err = !xl.present()
                          ? "XL9555 missing"
                          : (pcaMotor.present() ? "STBY/U23 write failed" : "STBY write failed");
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\",\"xl9555\":%s,\"pcaMotor\":%s}", err,
             xl.present() ? "true" : "false", pcaMotor.present() ? "true" : "false");
    return sendJson(req, 500, b);
  }
  char b[64];
  snprintf(b, sizeof(b), "{\"ok\":true,\"motorStby\":%s}", on ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleMotor(httpd_req_t *req) {
  auto a = loadArgs(req);
  int id = argInt(a, "id", -1);
  int dir = argInt(a, "dir", 0);
  int duty = argInt(a, "duty", 40);
  if (id < 0 || id > 3) return sendJson(req, 400, "{\"ok\":false,\"error\":\"id 0..3\"}");
  if (duty < 0) duty = 0;
  if (duty > 100) duty = 100;
  if (dir != 0 && duty != 0 && (!flagPwm || !flagStby))
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM and STBY first\"}");
  if (!motorDrive((uint8_t)id, dir, duty))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"motor write failed\"}");
  char b[96];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"id\":%d,\"dir\":%d,\"duty\":%d,\"failsafeMs\":%u}",
           id, dir, duty, (unsigned)(MOTOR_FAILSAFE_US / 1000));
  return sendJson(req, 200, b);
}

static esp_err_t handleMotorStopAll(httpd_req_t *req) {
  (void)loadArgs(req);
  if (!motorStopAll()) return sendJson(req, 500, "{\"ok\":false,\"error\":\"motor stop failed\"}");
  return sendJson(req, 200, "{\"ok\":true}");
}

static esp_err_t handleEncoders(httpd_req_t *req) {
  uint8_t p0 = 0;
  xl.readPort(0, p0);
  portENTER_CRITICAL(&encMux);
  int32_t e1 = enc1, e2 = enc2, e3 = enc3, e4 = enc4;
  portEXIT_CRITICAL(&encMux);
  char b[160];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"enc1\":%ld,\"enc2\":%ld,\"enc3\":%ld,\"enc4\":%ld,\"xlPort0\":%u}",
           (long)e1, (long)e2, (long)e3, (long)e4, p0);
  return sendJson(req, 200, b);
}

static esp_err_t handleEncReset(httpd_req_t *req) {
  (void)loadArgs(req);
  portENTER_CRITICAL(&encMux);
  enc1 = 0;
  enc2 = 0;
  enc3 = 0;
  enc4 = 0;
  portEXIT_CRITICAL(&encMux);
  return sendJson(req, 200, "{\"ok\":true}");
}

static esp_err_t handleCamera(httpd_req_t *req) {
  auto a = loadArgs(req);
  bool pwdnH = true, rstH = true;
  const bool haveLvl = cameraCtrlLevels(xl, pwdnH, rstH);
  if (req->method == HTTP_GET) {
    char b[360];
    snprintf(b, sizeof(b),
             "{\"ok\":true,\"camera\":%s,\"res\":\"%s\","
             "\"resOptions\":[\"qqvga\",\"qvga\",\"vga\",\"svga\",\"hd\",\"sxga\"],"
             "\"pwdn_high\":%s,\"rst_high\":%s,\"levels_ok\":%s,"
             "\"capture\":\"/api/camera/capture\",\"stream\":\"/stream\"}",
             cameraOk() ? "true" : "false", cameraFramesizeName(),
             haveLvl ? (pwdnH ? "true" : "false") : "null",
             haveLvl ? (rstH ? "true" : "false") : "null",
             haveLvl ? "true" : "false");
    return sendJson(req, 200, b);
  }

  // 仅改分辨率（摄像头已开时立即生效；未开则记偏好）
  if (argsHasKey(a, "res") && !argsHasKey(a, "on") && !argsHasKey(a, "hold")) {
    const std::string res = argStr(a, "res", "qvga");
    if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return sendJson(req, 503, "{\"ok\":false,\"error\":\"camera busy\"}");
    const bool ok = cameraSetFramesizeName(res.c_str());
    xSemaphoreGive(cameraMutex);
    if (!ok) return sendJson(req, 400, "{\"ok\":false,\"error\":\"bad or unsupported res\"}");
    char b[120];
    snprintf(b, sizeof(b), "{\"ok\":true,\"res\":\"%s\",\"camera\":%s}", cameraFramesizeName(),
             cameraOk() ? "true" : "false");
    return sendJson(req, 200, b);
  }

  bool on = argBool(a, "on", true);
  const bool holdOnly = argBool(a, "hold", false);
  if (argsHasKey(a, "res")) {
    const std::string res = argStr(a, "res", "qvga");
    if (!cameraSetFramesizeName(res.c_str()))
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"bad or unsupported res\"}");
  }
  if (!on && streamSlot && uxSemaphoreGetCount(streamSlot) == 0)
    return sendJson(req, 409, "{\"ok\":false,\"error\":\"stop the active stream before powering camera off\"}");
  if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"camera busy\"}");
  if (on) {
    bool ok = holdOnly ? cameraHoldPower(xl) : cameraBegin(xl);
    cameraCtrlLevels(xl, pwdnH, rstH);
    uint8_t in = 0, out = 0, cfg = 0;
    const bool dump = xl.dumpPort0(in, out, cfg);
    xSemaphoreGive(cameraMutex);
    char b[420];
    if (!ok) {
      snprintf(b, sizeof(b),
               "{\"ok\":false,\"error\":\"%s\",\"pwdn_high\":%s,\"rst_high\":%s,"
               "\"p0_in\":%u,\"p0_out\":%u,\"p0_cfg\":%u,\"dump_ok\":%s,"
               "\"hint\":\"Do NOT use ohm mode. Measure VOLTAGE U6.17 to GND; expect ~0V while held. "
               "11.6kΩ is R22 and always looks similar.\"}",
               holdOnly ? "camera hold power failed (XL cannot pull PWDN low?)" : "camera init failed",
               pwdnH ? "true" : "false", rstH ? "true" : "false", (unsigned)in, (unsigned)out,
               (unsigned)cfg, dump ? "true" : "false");
      return sendJson(req, 500, b);
    }
    snprintf(b, sizeof(b),
             "{\"ok\":true,\"camera\":%s,\"hold\":%s,\"res\":\"%s\",\"pwdn_high\":%s,\"rst_high\":%s,"
             "\"p0_in\":%u,\"p0_out\":%u,\"p0_cfg\":%u}",
             cameraOk() ? "true" : "false", holdOnly ? "true" : "false", cameraFramesizeName(),
             pwdnH ? "true" : "false", rstH ? "true" : "false", (unsigned)in, (unsigned)out,
             (unsigned)cfg);
    return sendJson(req, 200, b);
  }
  cameraEnd(xl);
  cameraCtrlLevels(xl, pwdnH, rstH);
  xSemaphoreGive(cameraMutex);
  char b[160];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"camera\":false,\"pwdn_high\":%s,\"rst_high\":%s}",
           pwdnH ? "true" : "false", rstH ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleCameraCapture(httpd_req_t *req) {
  auto a = loadArgs(req);
  if (argsHasKey(a, "res")) {
    const std::string res = argStr(a, "res", "qvga");
    if (!cameraSetFramesizeName(res.c_str()))
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"bad or unsupported res\"}");
  }
  if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"camera busy\"}");
  if (!cameraOk() && !cameraBegin(xl)) {
    xSemaphoreGive(cameraMutex);
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"camera not ready\"}");
  }
  // 开着时再套一次，保证本次抓拍用所选分辨率
  if (argsHasKey(a, "res")) cameraSetFramesizeName(argStr(a, "res", "qvga").c_str());
  uint8_t *buf = nullptr;
  size_t len = 0;
  if (!cameraCaptureJpeg(buf, len) || !buf || len == 0) {
    xSemaphoreGive(cameraMutex);
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"capture failed\"}");
  }
  addCors(req);
  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t err = httpd_resp_send(req, (const char *)buf, len);
  cameraReleaseFrame();
  xSemaphoreGive(cameraMutex);
  return err;
}

static esp_err_t streamAsync(httpd_req_t *req) {
  if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"camera busy\"}");
  const bool cameraReady = cameraOk() || cameraBegin(xl);
  xSemaphoreGive(cameraMutex);
  if (!cameraReady) return sendJson(req, 500, "{\"ok\":false,\"error\":\"camera not ready\"}");

  addCors(req);
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Connection", "close");

  int64_t t0 = esp_timer_get_time();
  while ((esp_timer_get_time() - t0) < 120000000LL) {
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
    if (!cameraOk() && !cameraBegin(xl)) {
      xSemaphoreGive(cameraMutex);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (!cameraCaptureJpeg(buf, len)) {
      xSemaphoreGive(cameraMutex);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                        (unsigned)len);
    if (httpd_resp_send_chunk(req, hdr, hlen) != ESP_OK ||
        httpd_resp_send_chunk(req, (const char *)buf, len) != ESP_OK ||
        httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
      cameraReleaseFrame();
      xSemaphoreGive(cameraMutex);
      break;
    }
    cameraReleaseFrame();
    xSemaphoreGive(cameraMutex);
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

static void streamTask(void *arg) {
  httpd_req_t *req = static_cast<httpd_req_t *>(arg);
  streamAsync(req);
  httpd_req_async_handler_complete(req);
  xSemaphoreGive(streamSlot);
  vTaskDelete(nullptr);
}

static esp_err_t handleStream(httpd_req_t *req) {
  if (!streamSlot || xSemaphoreTake(streamSlot, 0) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"stream already active\"}");
  httpd_req_t *copy = nullptr;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    xSemaphoreGive(streamSlot);
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"stream async setup failed\"}");
  }
  if (xTaskCreate(streamTask, "camera_stream", 6144, copy, 4, nullptr) != pdPASS) {
    sendJson(copy, 500, "{\"ok\":false,\"error\":\"stream task start failed\"}");
    httpd_req_async_handler_complete(copy);
    xSemaphoreGive(streamSlot);
  }
  return ESP_OK;
}

static uint16_t parseColor(const std::string &s, uint16_t defVal) {
  if (s.empty()) return defVal;
  const char *p = s.c_str();
  if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) p += 2;
  char *end = nullptr;
  unsigned long v = strtoul(p, &end, 16);
  if (end == p) return defVal;
  return (uint16_t)v;
}

static esp_err_t handleLcd(httpd_req_t *req) {
  auto a = loadArgs(req);
  std::string cmd = argStr(a, "cmd", "status");
  if (cmd == "init" || (cmd == "status" && !lcdOk && argBool(a, "on", false))) {
    lcdOk = lcd.begin(xl);
    touchOk = touch.begin(xl);
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd init failed\"}");
    return sendJson(req, 200, "{\"ok\":true,\"lcd\":true,\"w\":320,\"h\":480}");
  }
  if (cmd == "status") {
    char b[64];
    snprintf(b, sizeof(b), "{\"ok\":true,\"lcd\":%s}", lcdOk ? "true" : "false");
    return sendJson(req, 200, b);
  }
  if (!lcdOk) {
    lcdOk = lcd.begin(xl);
    touchOk = touch.begin(xl);
  }
  if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd not ready\"}");
  if (cmd == "on") {
    lcd.backlight(true);
    lcdOk = lcd.present();
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd backlight write failed\"}");
    return sendJson(req, 200, "{\"ok\":true,\"backlight\":true}");
  }
  if (cmd == "off") {
    lcd.backlight(false);
    lcdOk = lcd.present();
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd backlight write failed\"}");
    return sendJson(req, 200, "{\"ok\":true,\"backlight\":false}");
  }
  if (cmd == "fill" || cmd == "color") {
    uint16_t c = parseColor(argStr(a, "color", "001F"), 0x001F);
    lcd.fillScreen(c);
    lcdOk = lcd.present();
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd fill failed\"}");
    char b[64];
    snprintf(b, sizeof(b), "{\"ok\":true,\"color\":%u}", c);
    return sendJson(req, 200, b);
  }
  if (cmd == "rect") {
    int x = argInt(a, "x", 0);
    int y = argInt(a, "y", 0);
    int w = argInt(a, "w", 40);
    int h = argInt(a, "h", 40);
    uint16_t c = parseColor(argStr(a, "color", "FFFF"), 0xFFFF);
    lcd.fillRect((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, c);
    lcdOk = lcd.present();
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd rect failed\"}");
    return sendJson(req, 200, "{\"ok\":true}");
  }
  if (cmd == "text") {
    std::string text = argStr(a, "text", "EDA Robot");
    if (text.size() > 240) text.resize(240);
    int x = argInt(a, "x", 8);
    int y = argInt(a, "y", 8);
    int scale = argInt(a, "scale", 2);
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    uint16_t fg = parseColor(argStr(a, "color", "FFFF"), 0xFFFF);
    uint16_t bg = parseColor(argStr(a, "bg", "0000"), 0x0000);
    if (argBool(a, "clear", false)) lcd.fillScreen(bg);
    lcd.drawText((int16_t)x, (int16_t)y, text.c_str(), fg, bg, (uint8_t)scale);
    lcd.backlight(true);
    lcdOk = lcd.present();
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd text failed\"}");
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":true,\"x\":%d,\"y\":%d,\"scale\":%d,\"len\":%u}", x, y, scale,
             (unsigned)text.size());
    return sendJson(req, 200, b);
  }
  if (cmd == "demo") {
    lcd.fillScreen(0x0000);
    lcd.fillRect(0, 0, (int16_t)lcd.width(), 48, 0x001F);
    lcd.drawText(8, 12, "EDA-RobotPro", 0xFFFF, 0x001F, 2);
    char line[48];
    snprintf(line, sizeof(line), "FW %s", FW_VERSION);
    lcd.drawText(8, 64, line, 0x07FF, 0x0000, 2);
    snprintf(line, sizeof(line), "IP %s", ipStr[0] ? ipStr : "no-ip");
    lcd.drawText(8, 96, line, 0x07E0, 0x0000, 2);
    snprintf(line, sizeof(line), "LCD %ux%u", lcd.width(), lcd.height());
    lcd.drawText(8, 128, line, 0xFFE0, 0x0000, 2);
    lcd.drawText(8, 176, "Web Debug -> LCD text", 0xFFFF, 0x0000, 2);
    lcd.drawText(8, 208, "ASCII only (5x7)", 0xC618, 0x0000, 2);
    lcd.backlight(true);
    lcdOk = lcd.present();
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd demo failed\"}");
    return sendJson(req, 200, "{\"ok\":true,\"demo\":true}");
  }
  if (cmd == "rotate") {
    int r = argInt(a, "r", 0);
    lcd.setRotation((uint8_t)r);
    lcdOk = lcd.present();
    if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd rotation failed\"}");
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":true,\"rotation\":%d,\"w\":%u,\"h\":%u}", r, lcd.width(),
             lcd.height());
    return sendJson(req, 200, b);
  }
  return sendJson(req, 400,
                  "{\"ok\":false,\"error\":\"cmd=init|on|off|fill|rect|text|demo|rotate\"}");
}

static esp_err_t handleTouch(httpd_req_t *req) {
  if ((!touchOk || !touch.present()) && xl.present()) touchOk = touch.begin(xl);
  if (!touchOk || !touch.present())
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"touch not ready\"}");
  uint16_t x = 0, y = 0, z = 0;
  bool pressed = touch.touched();
  bool ok = touch.read(x, y, z);
  char b[128];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"irq\":%s,\"valid\":%s,\"x\":%u,\"y\":%u,\"z\":%u}",
           pressed ? "true" : "false", ok ? "true" : "false", x, y, z);
  return sendJson(req, 200, b);
}

static esp_err_t handleApiIndex(httpd_req_t *req) {
  std::string body = "{";
  body += "\"ok\":true,\"fw\":\"";
  body += FW_VERSION;
  body += "\",\"framework\":\"esp-idf\",";
  body += "\"board\":\"AI通用机器人_v5 / V1.0.0\",";
  body += "\"endpoints\":[";
  body += "{\"path\":\"/api/status\"},{\"path\":\"/api/estop\",\"note\":\"POST {on:true|false} 关闭/恢复所有外设\"},";
  body += "{\"path\":\"/api/shutdown\",\"note\":\"deep sleep; wake by power cycle or reset\"},";
  body += "{\"path\":\"/api/pwm\"},";
  body += "{\"path\":\"/api/amp\",\"note\":\"on bool; volume 0..100 digital gain\"},";
  body += "{\"path\":\"/api/servo\",\"note\":\"id 0..4 = T3..T7 (U16 LED11..15)\"},";
  body += "{\"path\":\"/api/servos\",\"note\":\"angles[5] or a0..a4\"},";
  body += "{\"path\":\"/api/led\",\"note\":\"id 0=LED_1 1=LED_2 2=LED_ALL; need LED_ALL for 1/2\"},";
  body += "{\"path\":\"/api/fan\",\"note\":\"auto / gesture / power; gesture: near palm hold 2s cycles 0→50→100\"},";
  body += "{\"path\":\"/api/voice\",\"note\":\"GET status; POST {on:true|false}; all UI settings persist in NVS\"},";
  body += "{\"path\":\"/api/i2c\",\"note\":\"?full=1 for bus scan\"},";
  body += "{\"path\":\"/api/mic\",\"note\":\"RMS sample\"},";
  body += "{\"path\":\"/api/rec\",\"note\":\"POST on=1|0 record; GET status; GET /api/rec/wav\"},";
  body += "{\"path\":\"/api/play\",\"note\":\"POST play last recording\"},";
  body += "{\"path\":\"/api/play/upload\",\"note\":\"POST WAV PCM16 16kHz or raw PCM16LE\"},";
  body += "{\"path\":\"/api/beep\"},{\"path\":\"/api/oled\"},";
  body += "{\"path\":\"/api/ota\",\"methods\":[\"GET\",\"POST\"]},";
  body += "{\"path\":\"/api/logs\"},";
  body += "{\"path\":\"/api/radar\",\"note\":\"power; scheduleOn/Off HH:MM + scheduleEnable; ignoreEnable + ignoreAdd/Del/Id/From/To\"},";
  body += "{\"path\":\"/api/radar/live\"},{\"path\":\"/radar\"}";
  body += "]}";
  return sendJson(req, 200, body);
}

static void jsonEscLite(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 2 < n; i++) {
    const char c = in[i];
    if (c == '"' || c == '\\') {
      out[j++] = '\\';
      out[j++] = c;
    } else if ((uint8_t)c < 0x20) {
      out[j++] = ' ';
    } else {
      out[j++] = c;
    }
  }
  out[j < n ? j : n - 1] = 0;
}

static void fanJsonInto(char *buf, size_t buflen) {
  char r[72], a[112], gp[28];
  jsonEscLite(fanReason, r, sizeof(r));
  jsonEscLite(fanLastAction, a, sizeof(a));
  jsonEscLite(gestPhase, gp, sizeof(gp));
  snprintf(buf, buflen,
           "{\"auto\":%s,\"on\":%s,\"phase\":\"%s\",\"reason\":\"%s\",\"lastAction\":\"%s\","
           "\"progress\":%u,\"need\":%u,\"offNeed\":%u,\"sampleMs\":2000,\"confirm\":%u,"
           "\"samplePresent\":%s,\"led1\":%d,\"ledAll\":%d,\"intensity\":%d,"
           "\"savedLed1\":%d,\"savedLedAll\":%d,\"savedIntensity\":%d,"
           "\"gesture\":%s,\"gestPhase\":\"%s\",\"gestProgressMs\":%lld,\"gestNeedMs\":2000,"
           "\"gestNearMm\":%u,\"gestExitMm\":%u,\"gestRangeMm\":%u,\"gestLevel\":%d,\"gear\":%d}",
           fanAutoEnable ? "true" : "false", fanAutoOn ? "true" : "false", fanPhase, r, a,
           (unsigned)fanConfirmCount, (unsigned)FAN_CONFIRM_ON, (unsigned)FAN_CONFIRM_OFF,
           (unsigned)FAN_CONFIRM_OFF, fanSamplePresent ? "true" : "false", spotDutyPct[0],
           spotDutyPct[2], fanIntensityPct(), fanSavedLed1, fanSavedLedAll,
           (fanSavedLed1 * fanSavedLedAll) / 100, fanGestureEnable ? "true" : "false", gp,
           (long long)gestHoldElapsedMs, (unsigned)GEST_NEAR_ENTER_MM, (unsigned)GEST_NEAR_EXIT_MM,
           (unsigned)gestLastRangeMm, fanLevelIndex(), fanGearPct);
}

static esp_err_t handleFanGet(httpd_req_t *req) {
  char fan[760];
  fanJsonInto(fan, sizeof(fan));
  char buf[800];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"fan\":%s}", fan);
  return sendJson(req, 200, buf);
}

static esp_err_t handleFanPost(httpd_req_t *req) {
  auto a = loadArgs(req);
  const bool hasPower = argsHasKey(a, "power");
  const bool hasGesture = argsHasKey(a, "gesture");
  const bool hasAuto =
      argsHasKey(a, "auto") || (!hasPower && !hasGesture && argsHasKey(a, "on"));
  if (!hasPower && !hasAuto && !hasGesture)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"need auto, gesture or power bool\"}");

  if (hasPower) {
    const bool on = argBool(a, "power", false);
    if (!applyManualFan(on, "API"))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"fan power failed\"}");
  }
  if (hasAuto) {
    const bool en = argsHasKey(a, "auto") ? argBool(a, "auto", false) : argBool(a, "on", false);
    fanAutoEnable = en;
    if (en) {
      flagPeriphOff = false;
      fanSuppressAutoOn = false;
      fanConfirmCount = 0;
      fanLastSampleUs = 0;
      fanSetPhase(flagRadarPwr ? "idle" : "disabled");
      // 不强制改输出：若手势档>0 且已在转，保持；由下一轮取样决定
      if (fanIsOn()) fanAutoOn = true;
    } else {
      // 关闭联动：若手势仍开且档位>0，保留当前输出（手势接管强度）；否则关掉自动开的风扇
      if (fanAutoOn && !(fanGestureEnable && fanGearPct > 0)) {
        if (applyRadarFan(false)) {
          snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：用户关闭联动");
          ESP_LOGI(TAG, "fan: OFF (user disabled auto)");
        }
      }
      fanAutoOn = false;
      fanConfirmCount = 0;
      fanLastSampleUs = 0;
      fanSuppressAutoOn = false;
      fanSetPhase("disabled");
      snprintf(fanReason, sizeof(fanReason), "未启用");
    }
    ESP_LOGI(TAG, "fan: auto=%d", en ? 1 : 0);
  }
  if (hasGesture) {
    const bool en = argBool(a, "gesture", false);
    fanGestureEnable = en;
    if (en) {
      flagPeriphOff = false;
      if (fanIsOn()) syncFanGearFromIntensity(fanIntensityPct());
      fanSuppressAutoOn = false;
    } else {
      // 关手势：清抑制，让控风扇可按记忆强度恢复；档位值保留供再开
      fanSuppressAutoOn = false;
    }
    gestResetTracking();
    snprintf(gestPhase, sizeof(gestPhase), en ? (flagRadarPwr ? "idle" : "no_power") : "disabled");
    ESP_LOGI(TAG, "fan: gesture=%d gear=%d", en ? 1 : 0, fanGearPct);
  }
  cfgSave();
  char fan[760];
  fanJsonInto(fan, sizeof(fan));
  char buf[800];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"fan\":%s}", fan);
  return sendJson(req, 200, buf);
}

static bool voiceEnabledNvs() { return flagVoice; }

static void voiceSetEnabledNvs(bool on) {
  flagVoice = on;
  cfgSave();
}

/** 写入全部可持久化设置（急停/播音临时开功放不调用；急停中不覆盖硬件键为全关）。 */
static void cfgSave() {
  if (cfgMutex && xSemaphoreTake(cfgMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
  nvs_handle_t h;
  if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
    if (cfgMutex) xSemaphoreGive(cfgMutex);
    return;
  }
  auto clampU8 = [](int x) -> uint8_t {
    if (x < 0) return 0;
    if (x > 255) return 255;
    return (uint8_t)x;
  };
  const bool pwm = flagPeriphOff ? savedPwm : flagPwm;
  const bool amp = flagPeriphOff ? savedAmp : flagAmp;
  const bool radar = flagPeriphOff ? savedRadarPwr : flagRadarPwr;
  const bool fauto = flagPeriphOff ? savedFanAuto : fanAutoEnable;
  const bool fgest = flagPeriphOff ? savedFanGesture : fanGestureEnable;
  nvs_set_u8(h, "voice", flagVoice ? 1 : 0);
  nvs_set_u8(h, "vol", board_i2s_get_volume());
  nvs_set_u8(h, "pwm", pwm ? 1 : 0);
  nvs_set_u8(h, "amp", amp ? 1 : 0);
  nvs_set_u8(h, "radar", radar ? 1 : 0);
  nvs_set_u8(h, "fauto", fauto ? 1 : 0);
  nvs_set_u8(h, "fgest", fgest ? 1 : 0);
  nvs_set_u8(h, "fgear", clampU8(fanGearPct));
  nvs_set_u8(h, "fs1", clampU8(fanSavedLed1));
  nvs_set_u8(h, "fsa", clampU8(fanSavedLedAll));
  // 始终写 snap：急停清零的是 spotDutyPct，不是意图镜像
  nvs_set_u8(h, "led0", clampU8(cfgSnapLed[0]));
  nvs_set_u8(h, "led1", clampU8(cfgSnapLed[1]));
  nvs_set_u8(h, "led2", clampU8(cfgSnapLed[2]));
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    char key[8]; snprintf(key, sizeof(key), "srv%u", (unsigned)i);
    nvs_set_u8(h, key, clampU8(cfgSnapSrv[i]));
  }
  nvs_set_u8(h, "rsch_en", radarScheduleEnable ? 1 : 0);
  nvs_set_u16(h, "rsch_on", radarSchedOnMin < 0 ? 65535 : (uint16_t)radarSchedOnMin);
  nvs_set_u16(h, "rsch_off", radarSchedOffMin < 0 ? 65535 : (uint16_t)radarSchedOffMin);
  nvs_set_u8(h, "rign_en", radarIgnoreEnable ? 1 : 0);
  nvs_set_u8(h, "rign_n", (uint8_t)radarIgnoreCount);
  {
    int8_t packed[RADAR_IGN_MAX * 2] = {0};
    for (int i = 0; i < radarIgnoreCount; i++) {
      packed[i * 2] = (int8_t)clampRadarAng(radarIgnoreSec[i].from);
      packed[i * 2 + 1] = (int8_t)clampRadarAng(radarIgnoreSec[i].to);
    }
    if (radarIgnoreCount > 0)
      nvs_set_blob(h, "rigns", packed, (size_t)radarIgnoreCount * 2);
    const int a0 = radarIgnoreCount > 0 ? radarIgnoreSec[0].from : 0;
    const int b0 = radarIgnoreCount > 0 ? radarIgnoreSec[0].to : -60;
    nvs_set_i8(h, "rign_a", (int8_t)clampRadarAng(a0));
    nvs_set_i8(h, "rign_b", (int8_t)clampRadarAng(b0));
  }
  nvs_commit(h);
  nvs_close(h);
  cfgDirty = false;
  if (cfgMutex) xSemaphoreGive(cfgMutex);
}

static void cfgMarkDirty() { cfgDirty = true; }

static void cfgFlushIfDirty() {
  if (cfgDirty) cfgSave();
}

static void cfgLoad() {
  nvs_handle_t h;
  if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
  uint8_t v = 0;
  if (nvs_get_u8(h, "voice", &v) == ESP_OK) flagVoice = v != 0;
  if (nvs_get_u8(h, "vol", &v) == ESP_OK) board_i2s_set_volume(v > 100 ? 100 : v);
  if (nvs_get_u8(h, "pwm", &v) == ESP_OK) cfgWantPwm = v != 0;
  if (nvs_get_u8(h, "amp", &v) == ESP_OK) cfgWantAmp = v != 0;
  if (nvs_get_u8(h, "radar", &v) == ESP_OK) cfgWantRadar = v != 0;
  if (nvs_get_u8(h, "fauto", &v) == ESP_OK) fanAutoEnable = v != 0;
  if (nvs_get_u8(h, "fgest", &v) == ESP_OK) fanGestureEnable = v != 0;
  if (nvs_get_u8(h, "fgear", &v) == ESP_OK) fanGearPct = v > 100 ? 100 : (int)v;
  if (nvs_get_u8(h, "fs1", &v) == ESP_OK) fanSavedLed1 = v > 100 ? 100 : (int)v;
  if (nvs_get_u8(h, "fsa", &v) == ESP_OK) fanSavedLedAll = v > 100 ? 100 : (int)v;
  if (nvs_get_u8(h, "led0", &v) == ESP_OK) cfgSnapLed[0] = v > 100 ? 100 : (int)v;
  if (nvs_get_u8(h, "led1", &v) == ESP_OK) cfgSnapLed[1] = v > 100 ? 100 : (int)v;
  if (nvs_get_u8(h, "led2", &v) == ESP_OK) cfgSnapLed[2] = v > 100 ? 100 : (int)v;
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    char key[8]; snprintf(key, sizeof(key), "srv%u", (unsigned)i);
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
      cfgSnapSrv[i] = v > 180 ? 180 : (int)v;
      servoAngleDeg[i] = cfgSnapSrv[i];
    }
  }
  if (nvs_get_u8(h, "rsch_en", &v) == ESP_OK) radarScheduleEnable = v != 0;
  uint16_t u16 = 65535;
  if (nvs_get_u16(h, "rsch_on", &u16) == ESP_OK)
    radarSchedOnMin = u16 >= 1440 ? -1 : (int)u16;
  u16 = 65535;
  if (nvs_get_u16(h, "rsch_off", &u16) == ESP_OK)
    radarSchedOffMin = u16 >= 1440 ? -1 : (int)u16;
  if (nvs_get_u8(h, "rign_en", &v) == ESP_OK)
    radarIgnoreEnable = v != 0;
  else if (nvs_get_u8(h, "rign60", &v) == ESP_OK)
    radarIgnoreEnable = v != 0;
  {
    int8_t packed[RADAR_IGN_MAX * 2] = {0};
    size_t blobLen = sizeof(packed);
    uint8_t n = 0;
    const bool hasN = nvs_get_u8(h, "rign_n", &n) == ESP_OK;
    const bool hasBlob = nvs_get_blob(h, "rigns", packed, &blobLen) == ESP_OK && blobLen >= 2;
    if (hasN) {
      radarIgnoreCount = n > RADAR_IGN_MAX ? RADAR_IGN_MAX : (int)n;
      if (hasBlob) {
        const int fromBlob = (int)(blobLen / 2);
        if (radarIgnoreCount > fromBlob) radarIgnoreCount = fromBlob;
        for (int i = 0; i < radarIgnoreCount; i++) {
          radarIgnoreSec[i].from = clampRadarAng(packed[i * 2]);
          radarIgnoreSec[i].to = clampRadarAng(packed[i * 2 + 1]);
        }
      } else if (radarIgnoreCount > 0) {
        int8_t i8 = 0;
        radarIgnoreCount = 1;
        if (nvs_get_i8(h, "rign_a", &i8) == ESP_OK) radarIgnoreSec[0].from = clampRadarAng(i8);
        if (nvs_get_i8(h, "rign_b", &i8) == ESP_OK) radarIgnoreSec[0].to = clampRadarAng(i8);
      }
    } else {
      int8_t i8 = 0;
      radarIgnoreCount = 1;
      if (nvs_get_i8(h, "rign_a", &i8) == ESP_OK) radarIgnoreSec[0].from = clampRadarAng(i8);
      if (nvs_get_i8(h, "rign_b", &i8) == ESP_OK) radarIgnoreSec[0].to = clampRadarAng(i8);
    }
  }
  nvs_close(h);
  cfgLoaded = true;
  {
    const int a0 = radarIgnoreCount > 0 ? radarIgnoreSec[0].from : 0;
    const int b0 = radarIgnoreCount > 0 ? radarIgnoreSec[0].to : -60;
    ESP_LOGI(TAG,
             "cfg NVS: voice=%d vol=%u pwm=%d amp=%d radar=%d fauto=%d fgest=%d gear=%d "
             "ign=%d n=%d(%d~%d) led=[%d,%d,%d] srv=[%d,%d]",
             flagVoice ? 1 : 0, (unsigned)board_i2s_get_volume(), cfgWantPwm ? 1 : 0,
             cfgWantAmp ? 1 : 0, cfgWantRadar ? 1 : 0, fanAutoEnable ? 1 : 0,
             fanGestureEnable ? 1 : 0, fanGearPct, radarIgnoreEnable ? 1 : 0, radarIgnoreCount, a0,
             b0, cfgSnapLed[0], cfgSnapLed[1], cfgSnapLed[2], cfgSnapSrv[0], cfgSnapSrv[1]);
  }
}

/** 按 snap 写回 LED/舵机（调用方已保证 PWM/OE 已开）。 */
static bool cfgRestoreOutputs() {
  if (!pcaMotor.present()) return false;
  bool ok = true;
  if (!actuatorLock()) return false;
  for (uint8_t i = 0; i < SPOT_COUNT; i++) {
    if (!setSpotDuty(i, cfgSnapLed[i])) ok = false;
  }
  for (uint8_t i = 0; i < SERVO_COUNT; i++) {
    if (!servoAngle(i, cfgSnapSrv[i])) ok = false;
  }
  actuatorUnlock();
  return ok;
}

/** I2C 器件就绪后恢复上次外设（急停态不落盘，故重启仍按用户上次意图）。 */
static void cfgApplyBoot() {
  if (!cfgLoaded) return;

  if (fanAutoEnable) {
    fanSuppressAutoOn = false;
    fanConfirmCount = 0;
    fanLastSampleUs = 0;
    fanSetPhase(cfgWantRadar ? "idle" : "disabled");
    snprintf(fanReason, sizeof(fanReason), "上电恢复联动");
  }
  if (fanGestureEnable) {
    gestResetTracking();
    snprintf(gestPhase, sizeof(gestPhase), cfgWantRadar ? "idle" : "no_power");
  }

  const bool anyLed = cfgSnapLed[0] > 0 || cfgSnapLed[1] > 0 || cfgSnapLed[2] > 0;
  const bool needPwm = cfgWantPwm || anyLed;
  if (needPwm && (pcaServo.present() || pcaMotor.present())) {
    if (!setPwmEnable(true)) {
      ESP_LOGW(TAG, "cfg restore: PWM enable failed");
    } else if (!cfgRestoreOutputs()) {
      ESP_LOGW(TAG, "cfg restore: LED/servo write failed");
    }
  } else if (cfgWantPwm && !pcaServo.present() && !pcaMotor.present()) {
    ESP_LOGW(TAG, "cfg restore: PWM wanted but PCA absent");
  }

  if (cfgWantAmp) {
    if (!setAmp(true)) ESP_LOGW(TAG, "cfg restore: amp failed");
  }
  if (cfgWantRadar) {
    if (!setRadarPower(true)) ESP_LOGW(TAG, "cfg restore: radar power failed");
  }

  if (fanIsOn()) fanAutoOn = fanAutoEnable;
  oledShowHome(true);
  ESP_LOGI(TAG, "cfg restore done pwm=%d amp=%d radar=%d leds=%d/%d/%d", flagPwm ? 1 : 0,
           flagAmp ? 1 : 0, flagRadarPwr ? 1 : 0, spotDutyPct[0], spotDutyPct[1],
           spotDutyPct[2]);
}

static void voiceJsonInto(char *buf, size_t buflen) {
  char inner[160];
  voice_sr_status(inner, sizeof(inner));
  const char *p = inner;
  if (*p == '{') p++;
  size_t n = strlen(p);
  if (n && p[n - 1] == '}') n--;
  snprintf(buf, buflen, "{\"enabled\":%s,\"modelBytes\":%u,%.*s}",
           voiceEnabledNvs() ? "true" : "false", (unsigned)voice_sr_model_bytes(), (int)n, p);
}

static esp_err_t handleStatus(httpd_req_t *req) {
  wifi_ap_record_t ap = {};
  int rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

  const bool psramOk =
#if CONFIG_SPIRAM
      esp_psram_is_initialized();
  const size_t psramBytes = psramOk ? esp_psram_get_size() : 0;
#else
      false;
  const size_t psramBytes = 0;
#endif

  char fan[760];
  fanJsonInto(fan, sizeof(fan));
  char voice[280];
  voiceJsonInto(voice, sizeof(voice));
  char localTime[16];
  localTimeStr(localTime, sizeof(localTime));
  char buf[2200];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"fw\":\"%s\",\"board\":\"v5\",\"ip\":\"%s\",\"rssi\":%d,"
           "\"timeSynced\":%s,\"localTime\":\"%s\","
           "\"psram\":%s,\"psramBytes\":%u,"
           "\"xl9555\":%s,\"oled\":%s,\"pcaServo\":%s,\"pcaMotor\":%s,\"i2s\":%s,"
           "\"lcd\":%s,\"touch\":%s,\"stby\":%s,\"camera\":%s,\"camRes\":\"%s\",\"streaming\":%s,"
           "\"pwmEnable\":%s,\"ampEnable\":%s,\"volume\":%u,\"radarPower\":%s,\"peripheralsOff\":%s,\"otaBusy\":%s,"
           "\"leds\":[%d,%d,%d],\"fan\":%s,\"voice\":%s,\"i2c\":%s}",
           FW_VERSION, ipStr, rssi, timeSynced ? "true" : "false", localTime,
           psramOk ? "true" : "false", (unsigned)psramBytes,
           xl.present() ? "true" : "false", oled.present() ? "true" : "false",
           pcaServo.present() ? "true" : "false", pcaMotor.present() ? "true" : "false", i2sReady ? "true" : "false",
           lcdOk ? "true" : "false", touchOk ? "true" : "false", flagStby ? "true" : "false",
           cameraOk() ? "true" : "false", cameraFramesizeName(),
           (streamSlot && uxSemaphoreGetCount(streamSlot) == 0) ? "true" : "false",
           flagPwm ? "true" : "false", flagAmp ? "true" : "false",
           (unsigned)board_i2s_get_volume(),
           flagRadarPwr ? "true" : "false", flagPeriphOff ? "true" : "false",
           otaBusy ? "true" : "false", spotDutyPct[0],
           spotDutyPct[1], spotDutyPct[2], fan, voice, i2cKnownJson().c_str());
  return sendJson(req, 200, buf);
}

static esp_err_t handleEstop(httpd_req_t *req) {
  auto a = loadArgs(req);
  const bool on = argBool(a, "on", true);
  if (on) {
    if (!emergencyStop())
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"peripherals off hardware write failed\"}");
    return sendJson(req, 200, "{\"ok\":true,\"peripheralsOff\":true,\"estop\":true}");
  }
  if (!releasePeripherals())
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"peripherals restore failed\"}");
  return sendJson(req, 200, "{\"ok\":true,\"peripheralsOff\":false,\"estop\":false}");
}

static esp_err_t handleShutdown(httpd_req_t *req) {
  (void)loadArgs(req);
  if (otaBusy) return sendJson(req, 409, "{\"ok\":false,\"error\":\"OTA in progress\"}");
  if (shutdownPending)
    return sendJson(req, 409, "{\"ok\":false,\"error\":\"shutdown already in progress\"}");
  if (!emergencyStop())
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"shutdown safety stop failed\"}");

  shutdownPending = true;
  if (xTaskCreate(shutdownTask, "shutdown", 3072, nullptr, 8, nullptr) != pdPASS) {
    shutdownPending = false;
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"shutdown task start failed\"}");
  }
  return sendJson(req, 200,
                  "{\"ok\":true,\"shutdown\":true,\"mode\":\"deep_sleep\","
                  "\"wake\":\"power_cycle_or_reset\"}");
}

static esp_err_t handlePwm(httpd_req_t *req) {
  auto a = loadArgs(req);
  bool on = argBool(a, "on", true);
  if (!setPwmEnable(on))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"xl9555 OE write failed\"}");
  cfgSave();
  char b[64];
  snprintf(b, sizeof(b), "{\"ok\":true,\"pwmEnable\":%s}", on ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleAmp(httpd_req_t *req) {
  auto a = loadArgs(req);
  const bool hasVol = argsHasKey(a, "volume");
  const bool hasOn = argsHasKey(a, "on");
  if (hasVol) {
    int vol = argInt(a, "volume", 100);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    board_i2s_set_volume((uint8_t)vol);
  }
  // 仅改音量时不碰功放开关；无 volume 时保持旧行为（默认开）
  if (hasOn || !hasVol) {
    const bool on = argBool(a, "on", true);
    if (!setAmp(on))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"xl9555 AMP write failed\"}");
  }
  cfgSave();
  char b[96];
  snprintf(b, sizeof(b), "{\"ok\":true,\"ampEnable\":%s,\"volume\":%u}",
           flagAmp ? "true" : "false", (unsigned)board_i2s_get_volume());
  return sendJson(req, 200, b);
}

static esp_err_t handleServo(httpd_req_t *req) {
  auto a = loadArgs(req);
  int id = argInt(a, "id", -1);
  int angle = argInt(a, "angle", 90);
  if (id < 0 || id >= (int)SERVO_COUNT)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"id 0..4 (T3..T7)\"}");
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  if (!flagPwm) return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM first with POST /api/pwm\"}");
  if (!servoAngle((uint8_t)id, angle))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"pca9685 servo write failed\"}");
  cfgSave();
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"angle\":%d}", id, angle);
  return sendJson(req, 200, b);
}

static esp_err_t handleServos(httpd_req_t *req) {
  auto a = loadArgs(req);
  if (!flagPwm) return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM first with POST /api/pwm\"}");
  int angles[SERVO_COUNT];
  bool provided[SERVO_COUNT];
  for (int i = 0; i < (int)SERVO_COUNT; i++) {
    angles[i] = 90;
    provided[i] = false;
  }
  size_t arr = a.body.find("\"angles\"");
  if (arr != std::string::npos) {
    size_t lb = a.body.find('[', arr);
    size_t rb = a.body.find(']', lb);
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
      std::string inner = a.body.substr(lb + 1, rb - lb - 1);
      size_t start = 0;
      for (int i = 0; i < (int)SERVO_COUNT; i++) {
        size_t comma = inner.find(',', start);
        std::string tok =
            (comma == std::string::npos) ? inner.substr(start) : inner.substr(start, comma - start);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
        if (!tok.empty()) {
          angles[i] = atoi(tok.c_str());
          provided[i] = true;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
  }
  for (int i = 0; i < (int)SERVO_COUNT; i++) {
    char key[4] = {'a', (char)('0' + i), 0, 0};
    char v[16];
    if (queryGet(a.q, key, v, sizeof(v))) {
      angles[i] = atoi(v);
      provided[i] = true;
    }
  }
  for (bool valueProvided : provided) {
    if (!valueProvided)
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"all 5 servo angles required (T3..T7)\"}");
  }
  for (int i = 0; i < (int)SERVO_COUNT; i++) {
    if (angles[i] < 0) angles[i] = 0;
    if (angles[i] > 180) angles[i] = 180;
    if (!servoAngle((uint8_t)i, angles[i])) {
      char b[80];
      snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"servo write failed\",\"id\":%d}", i);
      return sendJson(req, 500, b);
    }
  }
  cfgSave();
  char out[96];
  snprintf(out, sizeof(out), "{\"ok\":true,\"angles\":[%d,%d,%d,%d,%d]}", angles[0], angles[1],
           angles[2], angles[3], angles[4]);
  return sendJson(req, 200, out);
}

static esp_err_t handleLed(httpd_req_t *req) {
  auto a = loadArgs(req);
  int id = argInt(a, "id", -1);
  int duty = argInt(a, "duty", 100);
  if (id < 0 || id >= (int)SPOT_COUNT)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"id 0=LED_1 1=LED_2 2=LED_ALL\"}");
  if (duty < 0) duty = 0;
  if (duty > 100) duty = 100;
  if (!flagPwm) {
    if (duty == 0) {
      char b[96];
      snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"duty\":0,\"pwmEnable\":false}", id);
      return sendJson(req, 200, b);
    }
    if (!setPwmEnable(true))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"auto enable PWM (OE#) failed\"}");
  }
  if (!actuatorLock())
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"actuator lock failed\"}");
  const bool ledOk = setSpotDuty((uint8_t)id, duty);
  actuatorUnlock();
  if (!ledOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"led write failed\"}");
  // LED_1 / LED_ALL 与风扇强度相关：同步档位，避免与控风扇/手势抢控
  if (id == 0 || id == 2) onFanOutputChangedFromUi();
  cfgSave();
  char b[96];
  snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"duty\":%d,\"pwmEnable\":true}", id, duty);
  return sendJson(req, 200, b);
}

static void onVoiceSrCmd(int cmd_id);

static esp_err_t handleVoiceGet(httpd_req_t *req) {
  char voice[280];
  voiceJsonInto(voice, sizeof(voice));
  char buf[320];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"voice\":%s}", voice);
  return sendJson(req, 200, buf);
}

static esp_err_t handleVoicePost(httpd_req_t *req) {
  const ReqArgs a = loadArgs(req);
  if (!argsHasKey(a, "on")) return sendJson(req, 400, "{\"ok\":false,\"error\":\"need on\"}");
  const bool on = argBool(a, "on", false);
  voiceSetEnabledNvs(on);

  if (!on) {
    voice_sr_pause();
    ESP_LOGI(TAG, "voice disabled (NVS); AFE stays paused until reboot if already started");
    return handleVoiceGet(req);
  }

  if (!i2sReady) return sendJson(req, 500, "{\"ok\":false,\"error\":\"i2s not ready\"}");
#if CONFIG_SPIRAM
  if (!esp_psram_is_initialized())
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"PSRAM unavailable; voice needs SPIRAM\"}");
  if (!voice_sr_ok() && !voice_sr_start(onVoiceSrCmd))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"voice_sr_start failed\"}");
  voice_sr_resume();
  return handleVoiceGet(req);
#else
  return sendJson(req, 503, "{\"ok\":false,\"error\":\"PSRAM disabled in firmware\"}");
#endif
}

static void onVoiceSrCmd(int cmd_id) {
  if (cmd_id == 0) {
    // 唤醒：随机播 assets 语音；失败则双音；播放时暂停识别防回灌
    if (!i2sReady) return;
    voice_sr_pause();
    int16_t *pcm = nullptr;
    size_t n = 0;
    const char *name = nullptr;
    if (wake_reply_decode_random(&pcm, &n, &name) && pcm && n > 0) {
      ESP_LOGI(TAG, "wake reply play %s (%u samples)", name ? name : "?", (unsigned)n);
      playPcmWithAmp(pcm, n);
      free(pcm);
    } else {
      const bool was = flagAmp;
      if (!was) setAmp(true);
      if (!was) vTaskDelay(pdMS_TO_TICKS(8));
      board_i2s_wake_ack();
      if (!was) setAmp(false);
    }
    voice_sr_resume();
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_ON) {
    if (applyManualFan(true, "语音")) voiceAckBeep();
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_OFF) {
    if (applyManualFan(false, "语音")) voiceAckBeep();
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_UP) {
    if (applyManualFanDelta(20, "语音")) voiceAckBeep();
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_DOWN) {
    if (applyManualFanDelta(-20, "语音")) voiceAckBeep();
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_MAX) {
    if (applyManualFanLevel(100, "语音")) voiceAckBeep();
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_MIN) {
    if (applyManualFanLevel(20, "语音")) voiceAckBeep();
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_MID) {
    if (applyManualFanLevel(50, "语音")) voiceAckBeep();
  }
}

static esp_err_t handleMic(httpd_req_t *req) {
  int32_t rms = 0, peak = 0;
  if (!board_i2s_mic_rms(rms, peak))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"i2s mic read failed\"}");
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"rms\":%ld,\"peak\":%ld}", (long)rms, (long)peak);
  return sendJson(req, 200, b);
}

static esp_err_t handleRecGet(httpd_req_t *req) {
  const uint32_t ms =
      recSamples ? (uint32_t)((recSamples * 1000u) / (uint32_t)BOARD_I2S_RATE) : 0;
  char b[160];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"recording\":%s,\"ready\":%s,\"samples\":%u,\"ms\":%u,\"rate\":%d,"
           "\"maxMs\":%u,\"playBusy\":%s}",
           recActive ? "true" : "false", (!recActive && recSamples > 0) ? "true" : "false",
           (unsigned)recSamples, (unsigned)ms, BOARD_I2S_RATE,
           (unsigned)((REC_MAX_SAMPLES * 1000u) / (uint32_t)BOARD_I2S_RATE),
           playBusy ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleRecPost(httpd_req_t *req) {
  auto a = loadArgs(req);
  const bool on = argBool(a, "on", true);
  if (on) {
    if (playBusy) return sendJson(req, 409, "{\"ok\":false,\"error\":\"playing\"}");
    if (recActive) return sendJson(req, 200, "{\"ok\":true,\"recording\":true}");
    if (!recStart())
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"rec start failed (i2s/psram/busy)\"}");
    return sendJson(req, 200, "{\"ok\":true,\"recording\":true}");
  }
  recStop();
  const uint32_t ms =
      recSamples ? (uint32_t)((recSamples * 1000u) / (uint32_t)BOARD_I2S_RATE) : 0;
  char b[128];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"recording\":false,\"ready\":%s,\"samples\":%u,\"ms\":%u}",
           recSamples > 0 ? "true" : "false", (unsigned)recSamples, (unsigned)ms);
  return sendJson(req, 200, b);
}

static esp_err_t handleRecWav(httpd_req_t *req) {
  if (recActive) return sendJson(req, 409, "{\"ok\":false,\"error\":\"still recording\"}");
  if (!recBuf || recSamples == 0)
    return sendJson(req, 404, "{\"ok\":false,\"error\":\"no recording\"}");
  const uint32_t dataBytes = (uint32_t)(recSamples * sizeof(int16_t));
  uint8_t hdr[44];
  writeWavHeader(hdr, dataBytes, (uint32_t)BOARD_I2S_RATE);
  addCors(req);
  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"rec.wav\"");
  if (httpd_resp_send_chunk(req, (const char *)hdr, sizeof(hdr)) != ESP_OK) return ESP_FAIL;
  const uint8_t *p = (const uint8_t *)recBuf;
  size_t left = dataBytes;
  while (left) {
    const size_t n = left > 4096 ? 4096 : left;
    if (httpd_resp_send_chunk(req, (const char *)p, n) != ESP_OK) return ESP_FAIL;
    p += n;
    left -= n;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t handlePlayRec(httpd_req_t *req) {
  if (recActive) return sendJson(req, 409, "{\"ok\":false,\"error\":\"recording\"}");
  if (!recBuf || recSamples == 0)
    return sendJson(req, 404, "{\"ok\":false,\"error\":\"no recording\"}");
  if (playBusy) return sendJson(req, 409, "{\"ok\":false,\"error\":\"playing\"}");
  const bool ok = playPcmWithAmp(recBuf, recSamples);
  if (!ok) return sendJson(req, 500, "{\"ok\":false,\"error\":\"play failed\"}");
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"samples\":%u,\"volume\":%u}", (unsigned)recSamples,
           (unsigned)board_i2s_get_volume());
  return sendJson(req, 200, b);
}

static esp_err_t handlePlayUpload(httpd_req_t *req) {
  if (req->method == HTTP_OPTIONS) return handleOptions(req);
  if (recActive) return sendJson(req, 409, "{\"ok\":false,\"error\":\"recording\"}");
  if (playBusy) return sendJson(req, 409, "{\"ok\":false,\"error\":\"playing\"}");
  if (req->content_len <= 0)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"Content-Length required\"}");
  if ((size_t)req->content_len > PLAY_UPLOAD_MAX)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"audio too large (max 512KB)\"}");

  uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)req->content_len,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc((size_t)req->content_len);
  if (!buf) return sendJson(req, 500, "{\"ok\":false,\"error\":\"oom\"}");

  int got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, (char *)buf + got, req->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {
      free(buf);
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"recv aborted\"}");
    }
    got += n;
  }

  int16_t *pcm = nullptr;
  size_t nSamples = 0;
  bool owned = false;
  bool ok = false;
  const char *err = nullptr;

  if (got >= 12 && !memcmp(buf, "RIFF", 4)) {
    if (!parseWavPcm16(buf, (size_t)got, &pcm, &nSamples, &owned)) {
      err = "need WAV PCM16 @16kHz mono/stereo";
    }
  } else {
    // 原始 PCM16LE mono @16kHz
    if ((got & 1) != 0) {
      err = "odd PCM length";
    } else {
      pcm = (int16_t *)buf;
      nSamples = (size_t)got / 2;
    }
  }

  if (!err && pcm && nSamples) {
    ok = playPcmWithAmp(pcm, nSamples);
    if (!ok) err = "play failed";
  } else if (!err) {
    err = "empty audio";
  }

  if (owned && pcm) free(pcm);
  free(buf);

  if (!ok) {
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", err ? err : "play failed");
    return sendJson(req, 400, b);
  }
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"samples\":%u,\"volume\":%u}", (unsigned)nSamples,
           (unsigned)board_i2s_get_volume());
  return sendJson(req, 200, b);
}

static esp_err_t handleBeep(httpd_req_t *req) {
  auto a = loadArgs(req);
  int ms = argInt(a, "ms", 250);
  if (ms < 50) ms = 50;
  if (ms > 2000) ms = 2000;
  if (argsHasKey(a, "volume")) {
    int vol = argInt(a, "volume", 100);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    board_i2s_set_volume((uint8_t)vol);
    cfgSave();
  }
  const bool wasOn = flagAmp;
  if (!wasOn && !setAmp(true))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"amp enable failed\"}");
  if (!wasOn) vTaskDelay(pdMS_TO_TICKS(5));
  const bool beepOk = board_i2s_beep((uint16_t)ms);
  const bool restoreOk = wasOn || setAmp(false);
  if (!beepOk || !restoreOk)
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"beep or amp restore failed\"}");
  char b[64];
  snprintf(b, sizeof(b), "{\"ok\":true,\"ms\":%d,\"volume\":%u}", ms,
           (unsigned)board_i2s_get_volume());
  return sendJson(req, 200, b);
}

static esp_err_t handleI2c(httpd_req_t *req) {
  const std::string q = queryStr(req);
  char v[16];
  const bool full = queryGet(q, "full", v, sizeof(v)) && (v[0] == '1' || !strcasecmp(v, "true"));
  char buf[512];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"full\":%s,\"addrs\":%s}", full ? "true" : "false",
           i2cScanJson(full).c_str());
  return sendJson(req, 200, buf);
}

static esp_err_t handleOled(httpd_req_t *req) {
  if (!oledMutex || xSemaphoreTake(oledMutex, pdMS_TO_TICKS(500)) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"oled busy\"}");
  auto a = loadArgs(req);
  std::string cmd = argStr(a, "cmd", "text");
  if (cmd == "init" || cmd == "probe") {
    uint8_t addr = 0;
    uint32_t hz = 0;
    int failStep = -1;
    std::string diag;
    const bool ok = oledTryInit(addr, hz, failStep, diag);
    std::string body = "{\"ok\":";
    body += ok ? "true" : "false";
    body += ",\"oled\":";
    body += ok ? "true" : "false";
    body += ",\"addr\":";
    char num[16];
    snprintf(num, sizeof(num), "%u", (unsigned)addr);
    body += num;
    body += ",\"sclHz\":";
    snprintf(num, sizeof(num), "%u", (unsigned)hz);
    body += num;
    body += ",\"failStep\":";
    snprintf(num, sizeof(num), "%d", failStep);
    body += num;
    body += ",\"diag\":\"";
    body += diag;
    body += "\",\"i2c\":";
    body += i2cScanJson(false);
    body += ",\"chip\":\"SSD1315/SSD1306\",\"pins\":{\"sda\":12,\"scl\":13}}";
    xSemaphoreGive(oledMutex);
    return sendJson(req, ok ? 200 : 500, body);
  }
  if (!oled.present()) {
    xSemaphoreGive(oledMutex);
    return sendJson(req, 500,
                    "{\"ok\":false,\"error\":\"oled not ready\",\"hint\":\"POST /api/oled "
                    "{\\\"cmd\\\":\\\"init\\\"} after wiring fix\"}");
  }
  bool ok = false;
  if (cmd == "clear") {
    oled.clear();
    ok = oled.show();
  } else if (cmd == "fill") {
    oled.fill();
    ok = oled.show();
  } else {
    std::string text = argStr(a, "text", "EDA Robot");
    ok = oled.printfLines(text.c_str(), ipStr, FW_VERSION, "LAN API");
  }
  xSemaphoreGive(oledMutex);
  if (!ok) return sendJson(req, 500, "{\"ok\":false,\"error\":\"oled write failed\"}");
  return sendJson(req, 200, "{\"ok\":true}");
}

static esp_err_t handleOtaInfo(httpd_req_t *req) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *factory =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  const esp_partition_t *ota0 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_app_desc_t *desc = esp_app_get_description();
  char b[512];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"fw\":\"%s\",\"project\":\"%s\",\"idf\":\"%s\","
           "\"running\":\"%s\",\"runningOffset\":%u,\"runningSize\":%u,"
           "\"factory\":\"%s\",\"factorySize\":%u,\"ota0\":\"%s\",\"ota0Size\":%u,\"busy\":%s,"
           "\"hint\":\"主系统请点「进入救援升级」；在救援页上传 eda_robot.bin 大包\"}",
           FW_VERSION, desc ? desc->project_name : "?", desc ? desc->idf_ver : "?",
           running ? running->label : "?", running ? (unsigned)running->address : 0,
           running ? (unsigned)running->size : 0, factory ? factory->label : "?",
           factory ? (unsigned)factory->size : 0, ota0 ? ota0->label : "?",
           ota0 ? (unsigned)ota0->size : 0, otaBusy ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleRescue(httpd_req_t *req) {
  (void)loadArgs(req);
  const esp_partition_t *factory =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory)
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"no factory partition — serial flash new table first\"}");

  nvs_handle_t h;
  if (nvs_open("rescue", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "enter", 1);
    nvs_commit(h);
    nvs_close(h);
  }

  const esp_err_t err = esp_ota_set_boot_partition(factory);
  if (err != ESP_OK) {
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"set boot %s\"}", esp_err_to_name(err));
    return sendJson(req, 500, b);
  }

  sendJson(req, 200, "{\"ok\":true,\"reboot\":true,\"to\":\"factory\"}");
  ESP_LOGW(TAG, "reboot to factory rescue");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

static esp_err_t handleOta(httpd_req_t *req) {
  if (req->method == HTTP_OPTIONS) return handleOptions(req);
  if (req->method == HTTP_GET) return handleOtaInfo(req);

  // 单槽 ota_0 架构：主系统不能 OTA 自己，必须进救援
  return sendJson(req, 400,
                  "{\"ok\":false,\"error\":\"use rescue OTA\",\"hint\":\"POST /api/rescue then upload "
                  "eda_robot.bin on rescue page\"}");
}

static esp_err_t handleNotFound(httpd_req_t *req, httpd_err_code_t err) {
  (void)err;
  if (req->method == HTTP_OPTIONS) return handleOptions(req);
  return sendJson(req, 404, "{\"ok\":false,\"error\":\"not found\",\"hint\":\"GET /api\"}");
}

#define URI(path, method, handler) \
  { .uri = path, .method = method, .handler = handler, .user_ctx = nullptr }

static bool registerUri(httpd_handle_t s, const char *path, httpd_method_t method,
                        esp_err_t (*handler)(httpd_req_t *)) {
  httpd_uri_t u = URI(path, method, handler);
  const esp_err_t err = httpd_register_uri_handler(s, &u);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "register %s method=%d failed: %s", path, (int)method, esp_err_to_name(err));
    httpRegistrationOk = false;
    return false;
  }
  return true;
}

static void setupHttp() {
  httpRegistrationOk = true;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // FAN+v5 并集约 75 条 URI（含 OPTIONS）；留余量避免 HANDLERS_FULL 整站停服
  config.max_uri_handlers = 128;
  config.stack_size = 10240;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.recv_wait_timeout = 120;
  config.send_wait_timeout = 30;
  config.lru_purge_enable = true;

  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }

  registerUri(server, "/", HTTP_GET, handleRoot);
  registerUri(server, "/radar", HTTP_GET, handleRadarPage);
  registerUri(server, "/api", HTTP_GET, handleApiIndex);
  registerUri(server, "/api/", HTTP_GET, handleApiIndex);
  registerUri(server, "/api/status", HTTP_GET, handleStatus);
  registerUri(server, "/api/status", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/radar", HTTP_GET, handleRadarGet);
  registerUri(server, "/api/radar", HTTP_POST, handleRadarPost);
  registerUri(server, "/api/radar", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/radar/live", HTTP_GET, handleRadarLive);
  registerUri(server, "/api/radar/live", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/logs", HTTP_GET, handleLogs);
  registerUri(server, "/api/logs", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/fan", HTTP_GET, handleFanGet);
  registerUri(server, "/api/fan", HTTP_POST, handleFanPost);
  registerUri(server, "/api/fan", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/voice", HTTP_GET, handleVoiceGet);
  registerUri(server, "/api/voice", HTTP_POST, handleVoicePost);
  registerUri(server, "/api/voice", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/i2c", HTTP_GET, handleI2c);
  registerUri(server, "/api/i2c", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/mic", HTTP_GET, handleMic);
  registerUri(server, "/api/mic", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/rec", HTTP_GET, handleRecGet);
  registerUri(server, "/api/rec", HTTP_POST, handleRecPost);
  registerUri(server, "/api/rec", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/rec/wav", HTTP_GET, handleRecWav);
  registerUri(server, "/api/rec/wav", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/play", HTTP_POST, handlePlayRec);
  registerUri(server, "/api/play", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/play/upload", HTTP_POST, handlePlayUpload);
  registerUri(server, "/api/play/upload", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/ota", HTTP_GET, handleOta);
  registerUri(server, "/api/ota", HTTP_POST, handleOta);
  registerUri(server, "/api/ota", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/rescue", HTTP_POST, handleRescue);
  registerUri(server, "/api/rescue", HTTP_OPTIONS, handleOptions);

  registerUri(server, "/api/stby", HTTP_POST, handleStby);
  registerUri(server, "/api/stby", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/motor", HTTP_POST, handleMotor);
  registerUri(server, "/api/motor", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/motor/stop_all", HTTP_POST, handleMotorStopAll);
  registerUri(server, "/api/motor/stop_all", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/encoders", HTTP_GET, handleEncoders);
  registerUri(server, "/api/encoders", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/encoders/reset", HTTP_POST, handleEncReset);
  registerUri(server, "/api/encoders/reset", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/camera", HTTP_GET, handleCamera);
  registerUri(server, "/api/camera", HTTP_POST, handleCamera);
  registerUri(server, "/api/camera", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/camera/capture", HTTP_GET, handleCameraCapture);
  registerUri(server, "/api/camera/capture", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/stream", HTTP_GET, handleStream);
  registerUri(server, "/api/lcd", HTTP_POST, handleLcd);
  registerUri(server, "/api/lcd", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/touch", HTTP_GET, handleTouch);
  registerUri(server, "/api/touch", HTTP_OPTIONS, handleOptions);


  const char *mutating[] = {"/api/estop", "/api/shutdown", "/api/pwm", "/api/amp",
                            "/api/servo", "/api/servos", "/api/led", "/api/beep", "/api/oled"};
  esp_err_t (*fns[])(httpd_req_t *) = {handleEstop, handleShutdown, handlePwm, handleAmp,
                                       handleServo, handleServos,   handleLed, handleBeep,
                                       handleOled};
  static_assert(sizeof(mutating) / sizeof(mutating[0]) == sizeof(fns) / sizeof(fns[0]));
  for (size_t i = 0; i < sizeof(mutating) / sizeof(mutating[0]); i++) {
    registerUri(server, mutating[i], HTTP_POST, fns[i]);
    registerUri(server, mutating[i], HTTP_OPTIONS, handleOptions);
  }

  const esp_err_t err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handleNotFound);
  if (err != ESP_OK) {
    httpRegistrationOk = false;
    ESP_LOGE(TAG, "register 404 handler failed: %s", esp_err_to_name(err));
  }
  if (!httpRegistrationOk) {
    ESP_LOGE(TAG, "HTTP registration incomplete; stopping server");
    httpd_stop(server);
    server = nullptr;
    return;
  }
  ESP_LOGI(TAG, "HTTP :80 ready");
}

// ---- WiFi ----
static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const bool hadIp = ipStr[0] != 0;
    wifiOk = false;
    timeSynced = false;
    ipStr[0] = 0;
    if (hadIp) oledShowHome(true);
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&event->ip_info.ip));
    wifiOk = true;
    ESP_LOGI(TAG, "Got IP: %s", ipStr);
    timeSyncOnGotIp();
    oledShowHome(true);
  }
}

static void wifi_init() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));

  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "WiFi connecting to '%s' ...", WIFI_SSID);
}

static void background_task(void *) {
  int64_t lastCfgFlushUs = 0;
  while (true) {
    // 空板：无 XL 不做 I2C；雷达未供电时 poll 立即返回
    if (xl.present()) {
      uint8_t p0 = 0;
      if (xl.readPort(0, p0)) radar_set_gpio_out((p0 >> XL_RADAR_OUT) & 1);
    }
    if (flagRadarPwr) radar_poll();
    RadarSnapshot rs;
    radar_get_snapshot(rs);
    updateEnc34();
    if (fanGestureEnable && pcaMotor.present()) updateFanGesture(rs);
    if (fanAutoEnable && pcaMotor.present()) updateRadarFan(rs);
    radarScheduleTick();
    if (oled.present()) oledShowHome(false);
    const int64_t now = esp_timer_get_time();
    if (cfgDirty && (now - lastCfgFlushUs) >= 2000000) {
      lastCfgFlushUs = now;
      cfgFlushIfDirty();
    }
    vTaskDelay(pdMS_TO_TICKS((xl.present() || flagRadarPwr) ? 20 : 500));
  }
}

extern "C" void app_main(void) {
  device_log_init();
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  esp_ota_mark_app_valid_cancel_rollback();

  ESP_LOGI(TAG, "=== EDA Robot LAN API (ESP-IDF) ===");
  ESP_LOGI(TAG, "FW %s  board AI通用机器人_v5", FW_VERSION);
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run) ESP_LOGI(TAG, "running partition %s @0x%x", run->label, (unsigned)run->address);

  actuatorMutex = xSemaphoreCreateRecursiveMutex();
  cameraMutex = xSemaphoreCreateMutex();
  streamSlot = xSemaphoreCreateCounting(1, 1);
  oledMutex = xSemaphoreCreateMutex();
  audioMutex = xSemaphoreCreateMutex();
  cfgMutex = xSemaphoreCreateMutex();
  if (!actuatorMutex || !cameraMutex || !streamSlot || !oledMutex || !cfgMutex) {
    ESP_LOGE(TAG, "failed to create synchronization primitives");
    return;
  }

  radar_init();
  encoders_init();
  const bool radarBootUart = radar_start();
  ESP_LOGI(TAG, "radar UART=%d (power still off until /api/radar power=1)", radarBootUart);
  board_i2c_init();

  // 空板原则：probe 失败则绝不 begin（避免缺件时长时间 I2C 事务）
  bool okXl = board_i2c_probe(ADDR_XL9555) && xl.begin(ADDR_XL9555);
  bool okOled = false;
  if (board_i2c_probe(ADDR_OLED))
    okOled = oled.begin(ADDR_OLED, 100000);
  else if (board_i2c_probe(0x3D))
    okOled = oled.begin(0x3D, 100000);
  bool okPcaS = board_i2c_probe(ADDR_PCA_SERVO) && pcaServo.begin(ADDR_PCA_SERVO, 50.0f);
  bool okPcaM = board_i2c_probe(ADDR_PCA_MOTOR) && pcaMotor.begin(ADDR_PCA_MOTOR, 1000.0f);
  bool okPca = okPcaS || okPcaM;

  if (okXl) {
    lcdOk = lcd.begin(xl);
    touchOk = touch.begin(xl);
    if (lcdOk) {
      lcd.fillScreen(0x0000);
      lcd.fillRect(0, 0, 320, 48, 0x001F);
      lcd.drawText(8, 12, "EDA-RobotPro", 0xFFFF, 0x001F, 2);
      lcd.drawText(8, 64, "boot OK", 0x07E0, 0x0000, 2);
      lcdOk = lcd.present();
    }
    cameraPower(xl, false);
  }

  ESP_LOGI(TAG, "XL9555=%d OLED=%d PCA_S=%d PCA_M=%d LCD=%d TOUCH=%d CJK=%u", okXl, okOled,
           okPcaS, okPcaM, lcdOk, touchOk, (unsigned)font_cjk_count());
  if (okOled && oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    oled.printfLines("EDA Robot", "汉字字库就绪", "等待 WiFi...", FW_VERSION);
    xSemaphoreGive(oledMutex);
  }
  flagPwm = flagAmp = flagRadarPwr = flagPeriphOff = false;
  (void)okPca;
  cfgLoad();

  i2sReady = board_i2s_init();
  ESP_LOGI(TAG, "I2S=%d", i2sReady);
  // 语音默认关（NVS cfg/voice）；开了才 init AFE，避免供电/PSRAM 不稳时启动即崩
  ESP_LOGI(TAG, "voice model embed=%u bytes, nvs_enabled=%d", (unsigned)voice_sr_model_bytes(),
           voiceEnabledNvs() ? 1 : 0);
  if (voiceEnabledNvs() && i2sReady) {
#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
      const bool vok = voice_sr_start(onVoiceSrCmd);
      ESP_LOGI(TAG, "voice_sr=%d (pseudo-wake=你好爱妃)", vok ? 1 : 0);
    } else {
      ESP_LOGW(TAG, "voice enabled in NVS but PSRAM missing — skipped");
    }
#else
    ESP_LOGW(TAG, "voice enabled in NVS but CONFIG_SPIRAM off — skipped");
#endif
  } else {
    ESP_LOGI(TAG, "voice_sr idle (enable via POST /api/voice {\"on\":true})");
  }

  cfgApplyBoot();

  wifi_init();

  for (int i = 0; i < 80 && !wifiOk; i++) vTaskDelay(pdMS_TO_TICKS(250));
  if (!wifiOk && okOled && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    oled.printfLines("WiFi FAIL", WIFI_SSID, "检查热点", FW_VERSION);
    xSemaphoreGive(oledMutex);
    oledLastFanPct = fanIntensityPct();
    oledLastIp[0] = 0;
  } else if (wifiOk) {
    oledShowHome(true);
  }

  if (xTaskCreate(background_task, "bg", 4096, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "background task start failed");
    emergencyStop();
    return;
  }
  setupHttp();
}
