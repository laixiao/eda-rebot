#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
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

#include "board_config.h"
#include "board_i2c.h"
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

static const char *TAG = "eda_robot";
static const char *FW_VERSION = "3.6.5";
static volatile bool otaBusy = false;
static volatile bool shutdownPending = false;

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
static PCA9685 pca;
static SSD1306 oled;

static bool flagPwm = false;
static bool flagAmp = false;
static bool flagRadarPwr = false;
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
  add(ADDR_PCA9685, pca.present());
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
    static const uint8_t kAddrs[] = {ADDR_XL9555, ADDR_OLED, 0x3D, ADDR_PCA9685};
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
static bool pcaAllOffOrAbsent() { return !pca.present() || pca.allOff(); }

static bool setPwmEnable(bool on) {
  if (!actuatorLock()) return false;
  // 无 XL9555 时无法控 OE#；关请求视为成功，开请求失败
  if (!xl.present()) {
    if (!on) flagPwm = false;
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
    if (ok) flagPwm = true;
  } else if (xl.present()) {
    flagPwm = false;
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
  if (ok) flagAmp = on;
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
  const bool ok = xl.setPin(XL_RADAR_PWR, !on);
  if (ok) flagRadarPwr = on;
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
  return pca.setPulseUs(SERVO_CH[id], us);
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
  const bool ok = pca.setDuty(SPOT_CH[id], d);
  if (ok) {
    spotDutyPct[id] = dutyPct;
    if (id == 0 || id == 2) {
      fanRememberIfOn();
      oledShowHome(false);
    }
  }
  return ok;
}

/** 雷达 → 仅控 LED_1（风扇）；开时顺带拉高 LED_ALL（公共地），关时只关 LED_1。 */
static bool fanAutoEnable = false;  // Web「控风扇」，默认关
static bool fanAutoOn = false;
static const int64_t FAN_SAMPLE_US = 2000000;  // 每 2s 取样
static const uint8_t FAN_CONFIRM = 3;          // 连续 3 次相同再切换
static int64_t fanLastSampleUs = 0;
static uint8_t fanConfirmCount = 0;
static bool fanSamplePresent = false;
static char fanReason[64] = "未启用";
static char fanPhase[24] = "disabled";  // disabled/idle/arming/on/holdoff
static char fanLastAction[96] = "—";

static void fanSetPhase(const char *phase) { snprintf(fanPhase, sizeof(fanPhase), "%s", phase); }

/** 关掉雷达联动（语音/手动控风扇时避免抢控）。 */
static void fanDisableAuto(const char *why) {
  fanAutoEnable = false;
  fanAutoOn = false;
  fanConfirmCount = 0;
  fanLastSampleUs = 0;
  fanSetPhase("disabled");
  snprintf(fanReason, sizeof(fanReason), "%s", why ? why : "未启用");
}

/**
 * 手动/语音开关风扇：开=恢复上次 LED_1×LED_ALL；关=先记住再只关 LED_1。
 * 同时关闭雷达控风扇。
 */
static bool applyManualFan(bool on, const char *src) {
  if (!pca.present()) return false;
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
      snprintf(fanLastAction, sizeof(fanLastAction), "开风扇 %d%%（%s）", fanIntensityPct(),
               src ? src : "?");
      ESP_LOGI(TAG, "fan: MANUAL ON intensity=%d src=%s", fanIntensityPct(), src ? src : "?");
    }
    return ok;
  }
  if (!actuatorLock()) return false;
  fanRememberIfOn();
  const bool ok = setSpotDuty(0, 0);
  actuatorUnlock();
  if (ok) {
    snprintf(fanLastAction, sizeof(fanLastAction), "关风扇（%s，已记强度 %d/%d）", src ? src : "?",
             fanSavedLed1, fanSavedLedAll);
    ESP_LOGI(TAG, "fan: MANUAL OFF saved=%d/%d src=%s", fanSavedLed1, fanSavedLedAll,
             src ? src : "?");
  }
  return ok;
}

/** 明确有人：主目标 / is_detected / 明显运动。不含单独呼吸、微动、OUT。 */
static bool classifyFanPresent(const RadarSnapshot &rs, char *reason, size_t n) {
  if (!flagRadarPwr) {
    snprintf(reason, n, "雷达未供电");
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
  if (rs.primary_valid || rs.is_detected) {
    if (rs.range_mm > 0)
      snprintf(reason, n, "人 %u.%um", (unsigned)(rs.range_mm / 1000),
               (unsigned)((rs.range_mm % 1000) / 100));
    else
      snprintf(reason, n, "检测到人");
    return true;
  }
  snprintf(reason, n, "无人");
  return false;
}

static bool applyRadarFan(bool on) {
  if (!pca.present()) return false;
  if (on) {
    if (!flagPwm && !setPwmEnable(true)) return false;
    if (!actuatorLock()) return false;
    bool ok = true;
    if (spotDutyPct[2] < 100) ok = setSpotDuty(2, 100);
    if (ok) ok = setSpotDuty(0, 100);
    actuatorUnlock();
    return ok;
  }
  if (!actuatorLock()) return false;
  fanRememberIfOn();
  const bool ok = setSpotDuty(0, 0);
  actuatorUnlock();
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

  if (fanConfirmCount < FAN_CONFIRM) {
    fanSetPhase(fanSamplePresent ? (fanAutoOn ? "on" : "arming")
                                 : (fanAutoOn ? "holdoff" : "idle"));
    ESP_LOGI(TAG, "fan: sample %s %u/%u (%s)", fanSamplePresent ? "present" : "absent",
             (unsigned)fanConfirmCount, (unsigned)FAN_CONFIRM, reason);
    return;
  }

  if (fanSamplePresent && !fanAutoOn) {
    if (applyRadarFan(true)) {
      fanAutoOn = true;
      fanSetPhase("on");
      snprintf(fanLastAction, sizeof(fanLastAction), "开 LED_1：%s", reason);
      ESP_LOGI(TAG, "fan: ON reason=%s", reason);
    }
  } else if (!fanSamplePresent && fanAutoOn) {
    if (applyRadarFan(false)) {
      fanAutoOn = false;
      fanSetPhase("idle");
      snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：%s", reason);
      ESP_LOGI(TAG, "fan: OFF reason=%s", reason);
    }
  } else {
    fanSetPhase(fanAutoOn ? "on" : "idle");
  }
}

static bool emergencyStop() {
  recStop();
  if (!actuatorLock()) return false;
  bool oeOk = true, ampOk = true, radarOk = true;
  if (xl.present()) {
    oeOk = xl.setPin(XL_OE, true);
    ampOk = xl.setPin(XL_AMP_SD, false);
    radarOk = xl.setPin(XL_RADAR_PWR, true);
  }
  const bool pwmOk = pcaAllOffOrAbsent();
  if (oeOk) flagPwm = false;
  if (ampOk) flagAmp = false;
  if (radarOk) {
    flagRadarPwr = false;
    radar_on_power(false);
  }
  fanAutoOn = false;
  fanConfirmCount = 0;
  fanLastSampleUs = 0;
  fanSetPhase(fanAutoEnable ? "idle" : "disabled");
  snprintf(fanReason, sizeof(fanReason), "急停");
  snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：急停");
  spotDutyPct[0] = spotDutyPct[1] = spotDutyPct[2] = 0;
  actuatorUnlock();
  oledShowHome(true);
  return oeOk && ampOk && radarOk && pwmOk;
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
  // append power flag without rewriting radar module
  std::string body = buf;
  if (!body.empty() && body.back() == '}') {
    body.pop_back();
    body += ",\"power\":";
    body += flagRadarPwr ? "true" : "false";
    body += '}';
  }
  return sendJson(req, 200, body);
}

static esp_err_t handleRadarLive(httpd_req_t *req) {
  char buf[3072];
  radar_json_live(buf, sizeof(buf));
  return sendJson(req, 200, buf);
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
  if (argsHasKey(a, "power")) {
    if (!setRadarPower(argBool(a, "power", true)))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"radar power write failed\"}");
    char buf[1536];
    radar_json_summary(buf, sizeof(buf));
    std::string body = buf;
    if (!body.empty() && body.back() == '}') {
      body.pop_back();
      body += ",\"power\":";
      body += flagRadarPwr ? "true" : "false";
      body += '}';
    }
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
  else return sendJson(req, 400, "{\"ok\":false,\"error\":\"use power or cmd=version|poll\"}");
  if (!commandOk) {
    if (cmd == "poll" && !flagRadarPwr)
      return sendJson(req, 409, "{\"ok\":false,\"error\":\"radar power is off\"}");
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"radar UART write failed\"}");
  }
  char buf[1536];
  radar_json_summary(buf, sizeof(buf));
  return sendJson(req, 200, buf);
}

static esp_err_t handleApiIndex(httpd_req_t *req) {
  std::string body = "{";
  body += "\"ok\":true,\"fw\":\"";
  body += FW_VERSION;
  body += "\",\"framework\":\"esp-idf\",";
  body += "\"board\":\"AI通用机器人_v6-1 / V1.0.0\",";
  body += "\"endpoints\":[";
  body += "{\"path\":\"/api/status\"},{\"path\":\"/api/estop\"},";
  body += "{\"path\":\"/api/shutdown\",\"note\":\"deep sleep; wake by power cycle or reset\"},";
  body += "{\"path\":\"/api/pwm\"},";
  body += "{\"path\":\"/api/amp\",\"note\":\"on bool; volume 0..100 digital gain\"},";
  body += "{\"path\":\"/api/servo\",\"note\":\"id 0..1 = T3/T4\"},";
  body += "{\"path\":\"/api/servos\"},";
  body += "{\"path\":\"/api/led\",\"note\":\"id 0=LED_1 1=LED_2 2=LED_ALL; need LED_ALL for 1/2\"},";
  body += "{\"path\":\"/api/fan\",\"note\":\"radar auto OR power=1|0 (last intensity; disables auto)\"},";
  body += "{\"path\":\"/api/voice\",\"note\":\"GET status; POST {on:true|false} 语音开关(NVS,默认关)\"},";
  body += "{\"path\":\"/api/i2c\",\"note\":\"?full=1 for bus scan\"},";
  body += "{\"path\":\"/api/mic\",\"note\":\"RMS sample\"},";
  body += "{\"path\":\"/api/rec\",\"note\":\"POST on=1|0 record; GET status; GET /api/rec/wav\"},";
  body += "{\"path\":\"/api/play\",\"note\":\"POST play last recording\"},";
  body += "{\"path\":\"/api/play/upload\",\"note\":\"POST WAV PCM16 16kHz or raw PCM16LE\"},";
  body += "{\"path\":\"/api/beep\"},{\"path\":\"/api/oled\"},";
  body += "{\"path\":\"/api/ota\",\"methods\":[\"GET\",\"POST\"]},";
  body += "{\"path\":\"/api/logs\"},";
  body += "{\"path\":\"/api/radar\",\"note\":\"power on/off (auto query when powered)\"},";
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
  char r[72], a[112];
  jsonEscLite(fanReason, r, sizeof(r));
  jsonEscLite(fanLastAction, a, sizeof(a));
  snprintf(buf, buflen,
           "{\"auto\":%s,\"on\":%s,\"phase\":\"%s\",\"reason\":\"%s\",\"lastAction\":\"%s\","
           "\"progress\":%u,\"need\":%u,\"offNeed\":%u,\"sampleMs\":2000,\"confirm\":%u,"
           "\"samplePresent\":%s,\"led1\":%d,\"ledAll\":%d,\"intensity\":%d,"
           "\"savedLed1\":%d,\"savedLedAll\":%d,\"savedIntensity\":%d}",
           fanAutoEnable ? "true" : "false", fanAutoOn ? "true" : "false", fanPhase, r, a,
           (unsigned)fanConfirmCount, (unsigned)FAN_CONFIRM, (unsigned)FAN_CONFIRM,
           (unsigned)FAN_CONFIRM, fanSamplePresent ? "true" : "false", spotDutyPct[0],
           spotDutyPct[2], fanIntensityPct(), fanSavedLed1, fanSavedLedAll,
           (fanSavedLed1 * fanSavedLedAll) / 100);
}

static esp_err_t handleFanGet(httpd_req_t *req) {
  char fan[560];
  fanJsonInto(fan, sizeof(fan));
  char buf[600];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"fan\":%s}", fan);
  return sendJson(req, 200, buf);
}

static esp_err_t handleFanPost(httpd_req_t *req) {
  auto a = loadArgs(req);
  const bool hasPower = argsHasKey(a, "power");
  const bool hasAuto = argsHasKey(a, "auto") || (!hasPower && argsHasKey(a, "on"));
  if (!hasPower && !hasAuto)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"need auto or power bool\"}");

  if (hasPower) {
    const bool on = argBool(a, "power", false);
    if (!applyManualFan(on, "API"))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"fan power failed\"}");
  }
  if (hasAuto) {
    const bool en = argsHasKey(a, "auto") ? argBool(a, "auto", false) : argBool(a, "on", false);
    fanAutoEnable = en;
    if (!en && fanAutoOn) {
      if (applyRadarFan(false)) {
        fanAutoOn = false;
        snprintf(fanLastAction, sizeof(fanLastAction), "关 LED_1：用户关闭联动");
        ESP_LOGI(TAG, "fan: OFF (user disabled auto)");
      }
    }
    fanConfirmCount = 0;
    fanLastSampleUs = 0;
    fanSetPhase(en ? "idle" : "disabled");
    if (!en) snprintf(fanReason, sizeof(fanReason), "未启用");
    ESP_LOGI(TAG, "fan: auto=%d", en ? 1 : 0);
  }
  char fan[560];
  fanJsonInto(fan, sizeof(fan));
  char buf[600];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"fan\":%s}", fan);
  return sendJson(req, 200, buf);
}

static bool voiceEnabledNvs() {
  nvs_handle_t h;
  uint8_t v = 0;
  if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return false;
  nvs_get_u8(h, "voice", &v);
  nvs_close(h);
  return v != 0;
}

static void voiceSetEnabledNvs(bool on) {
  nvs_handle_t h;
  if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "voice", on ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
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

  char fan[560];
  fanJsonInto(fan, sizeof(fan));
  char voice[280];
  voiceJsonInto(voice, sizeof(voice));
  char buf[1400];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"fw\":\"%s\",\"board\":\"v6-1\",\"ip\":\"%s\",\"rssi\":%d,"
           "\"psram\":%s,\"psramBytes\":%u,"
           "\"xl9555\":%s,\"oled\":%s,\"pca9685\":%s,\"i2s\":%s,"
           "\"pwmEnable\":%s,\"ampEnable\":%s,\"volume\":%u,\"radarPower\":%s,\"otaBusy\":%s,"
           "\"leds\":[%d,%d,%d],\"fan\":%s,\"voice\":%s,\"i2c\":%s}",
           FW_VERSION, ipStr, rssi, psramOk ? "true" : "false", (unsigned)psramBytes,
           xl.present() ? "true" : "false", oled.present() ? "true" : "false",
           pca.present() ? "true" : "false", i2sReady ? "true" : "false",
           flagPwm ? "true" : "false", flagAmp ? "true" : "false",
           (unsigned)board_i2s_get_volume(),
           flagRadarPwr ? "true" : "false", otaBusy ? "true" : "false", spotDutyPct[0],
           spotDutyPct[1], spotDutyPct[2], fan, voice, i2cKnownJson().c_str());
  return sendJson(req, 200, buf);
}

static esp_err_t handleEstop(httpd_req_t *req) {
  (void)loadArgs(req);
  if (!emergencyStop())
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"estop hardware write failed\"}");
  radar_set_enabled(false);
  return sendJson(req, 200, "{\"ok\":true,\"estop\":true}");
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
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"id 0..1 (T3/T4)\"}");
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  if (!flagPwm) return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM first with POST /api/pwm\"}");
  if (!servoAngle((uint8_t)id, angle))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"pca9685 servo write failed\"}");
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"angle\":%d}", id, angle);
  return sendJson(req, 200, b);
}

static esp_err_t handleServos(httpd_req_t *req) {
  auto a = loadArgs(req);
  if (!flagPwm) return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM first with POST /api/pwm\"}");
  int angles[2] = {90, 90};
  bool provided[2] = {false, false};
  size_t arr = a.body.find("\"angles\"");
  if (arr != std::string::npos) {
    size_t lb = a.body.find('[', arr);
    size_t rb = a.body.find(']', lb);
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
      std::string inner = a.body.substr(lb + 1, rb - lb - 1);
      size_t start = 0;
      for (int i = 0; i < 2; i++) {
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
  for (int i = 0; i < 2; i++) {
    char key[4] = {'a', (char)('0' + i), 0, 0};
    char v[16];
    if (queryGet(a.q, key, v, sizeof(v))) {
      angles[i] = atoi(v);
      provided[i] = true;
    }
  }
  for (bool valueProvided : provided) {
    if (!valueProvided)
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"both servo angles required\"}");
  }
  for (int i = 0; i < 2; i++) {
    if (angles[i] < 0) angles[i] = 0;
    if (angles[i] > 180) angles[i] = 180;
    if (!servoAngle((uint8_t)i, angles[i])) {
      char b[80];
      snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"servo write failed\",\"id\":%d}", i);
      return sendJson(req, 500, b);
    }
  }
  char out[64];
  snprintf(out, sizeof(out), "{\"ok\":true,\"angles\":[%d,%d]}", angles[0], angles[1]);
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
    // 唤醒提示音（短 beep）；失败忽略
    if (i2sReady) {
      const bool was = flagAmp;
      if (!was) setAmp(true);
      board_i2s_beep(80);
      if (!was) setAmp(false);
    }
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_ON) {
    applyManualFan(true, "语音");
    return;
  }
  if (cmd_id == VOICE_SR_CMD_FAN_OFF) {
    applyManualFan(false, "语音");
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
  config.max_uri_handlers = 64;
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
    ipStr[0] = 0;
    if (hadIp) oledShowHome(true);
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&event->ip_info.ip));
    wifiOk = true;
    ESP_LOGI(TAG, "Got IP: %s", ipStr);
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
  while (true) {
    // 空板：无 XL 不做 I2C；雷达未供电时 poll 立即返回
    if (xl.present()) {
      uint8_t p0 = 0;
      if (xl.readPort(0, p0)) radar_set_gpio_out((p0 >> XL_RADAR_OUT) & 1);
    }
    if (flagRadarPwr) radar_poll();
    RadarSnapshot rs;
    radar_get_snapshot(rs);
    if (fanAutoEnable && pca.present()) updateRadarFan(rs);
    if (oled.present()) oledShowHome(false);
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
  ESP_LOGI(TAG, "FW %s  board AI通用机器人_v6-1", FW_VERSION);
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run) ESP_LOGI(TAG, "running partition %s @0x%x", run->label, (unsigned)run->address);

  actuatorMutex = xSemaphoreCreateRecursiveMutex();
  oledMutex = xSemaphoreCreateMutex();
  audioMutex = xSemaphoreCreateMutex();
  if (!actuatorMutex || !oledMutex) {
    ESP_LOGE(TAG, "failed to create synchronization primitives");
    return;
  }

  radar_init();
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
  bool okPca = board_i2c_probe(ADDR_PCA9685) && pca.begin(ADDR_PCA9685, 50.0f);

  ESP_LOGI(TAG, "XL9555=%d OLED=%d PCA9685=%d CJK=%u (bare-board safe)", okXl, okOled, okPca,
           (unsigned)font_cjk_count());
  if (okOled && oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    oled.printfLines("EDA Robot", "汉字字库就绪", "等待 WiFi...", FW_VERSION);
    xSemaphoreGive(oledMutex);
  }
  flagPwm = flagAmp = flagRadarPwr = false;

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
