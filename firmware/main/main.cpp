#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "board_config.h"
#include "board_i2c.h"
#include "board_spi.h"
#include "board_i2s.h"
#include "xl9555.h"
#include "pca9685.h"
#include "ssd1306.h"
#include "camera_board.h"
#include "st7796.h"
#include "xpt2046.h"
#include "web_ui.h"
#include "web_radar_ui.h"
#include "radar_at6010.h"
#include "device_log.h"
#include "esp_psram.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_system.h"

static const char *TAG = "eda_robot";
static const char *FW_VERSION = "2.2.8";
static const int64_t MOTOR_FAILSAFE_US = 1500000;
static volatile bool otaBusy = false;
static volatile bool shutdownPending = false;

static XL9555 xl;
static PCA9685 pcaServo;
static PCA9685 pcaMotor;
static SSD1306 oled;
static ST7796 lcd;
static XPT2046 touch;

static volatile int32_t enc1 = 0;
static volatile int32_t enc2 = 0;
static volatile int32_t enc3 = 0;
static volatile int32_t enc4 = 0;
static uint8_t prevXlA = 0;
static bool enc34Initialized = false;
static portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;

static bool flagPwm = false;
static bool flagStby = false;
static bool flagAmp = false;
static bool i2sReady = false;
static bool lcdOk = false;
static bool touchOk = false;
static bool wifiOk = false;
static char ipStr[16] = {0};
static volatile uint8_t motorActiveMask = 0;
static int64_t lastMotorCommandUs = 0;

static httpd_handle_t server = nullptr;
static SemaphoreHandle_t actuatorMutex = nullptr;
static SemaphoreHandle_t cameraMutex = nullptr;
static SemaphoreHandle_t oledMutex = nullptr;
static SemaphoreHandle_t streamSlot = nullptr;
static bool httpRegistrationOk = true;

static bool actuatorLock() {
  return actuatorMutex && xSemaphoreTakeRecursive(actuatorMutex, portMAX_DELAY) == pdTRUE;
}

static void actuatorUnlock() {
  if (actuatorMutex) xSemaphoreGiveRecursive(actuatorMutex);
}

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

static std::string i2cScanJson() {
  // 只扫本板已知地址，避免 1..126 全扫导致超时刷屏并卡死 HTTP
  static const uint8_t kAddrs[] = {ADDR_XL9555, ADDR_OLED, ADDR_PCA_SERVO, ADDR_PCA_MOTOR};
  std::string s = "[";
  bool first = true;
  for (uint8_t addr : kAddrs) {
    if (board_i2c_probe(addr)) {
      if (!first) s += ',';
      first = false;
      char b[8];
      snprintf(b, sizeof(b), "%u", addr);
      s += b;
    }
  }
  s += ']';
  return s;
}

// ---- actuators ----
// U16 (servo PCA) may be absent; do not block OE# / motor PCA (U23) when it is.
static bool pcaAllOffOrAbsent(PCA9685 &pca) { return !pca.present() || pca.allOff(); }

static bool setPwmEnable(bool on) {
  if (!actuatorLock()) return false;
  bool ok = true;
  if (on) {
    const bool motorsOff = pcaAllOffOrAbsent(pcaMotor);
    const bool servosOff = pcaAllOffOrAbsent(pcaServo);
    ok = motorsOff && servosOff;
    if (ok) ok = xl.setPin(XL_OE, false);
  } else {
    const bool motorsOff = pcaAllOffOrAbsent(pcaMotor);
    const bool servosOff = pcaAllOffOrAbsent(pcaServo);
    ok = xl.setPin(XL_OE, true) && motorsOff && servosOff;
    if (ok || motorsOff) motorActiveMask = 0;
  }
  if (on) {
    if (ok) flagPwm = true;
  } else if (xl.present()) {
    flagPwm = false;
  }
  actuatorUnlock();
  return ok;
}

static bool setStby(bool on) {
  if (!actuatorLock()) return false;
  bool ok = true;
  if (on) {
    ok = pcaMotor.allOff();
    if (ok) {
      motorActiveMask = 0;
      ok = xl.setPin(XL_STBY, true);
    }
  } else {
    const bool stbyOk = xl.setPin(XL_STBY, false);
    const bool pwmOk = pcaMotor.allOff();
    if (stbyOk || pwmOk) motorActiveMask = 0;
    ok = stbyOk && pwmOk;
  }
  if (on) {
    if (ok) flagStby = true;
  } else if (xl.present()) {
    flagStby = false;
  }
  actuatorUnlock();
  return ok;
}

static bool setAmp(bool on) {
  if (!actuatorLock()) return false;
  const bool ok = xl.setPin(XL_AMP_SD, on);
  if (ok) flagAmp = on;
  actuatorUnlock();
  return ok;
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

static bool servoAngle(uint8_t id, int angle) {
  if (id > 4) return false;
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  uint16_t us =
      SERVO_US_MIN + (uint16_t)((uint32_t)(SERVO_US_MAX - SERVO_US_MIN) * angle / 180);
  return pcaServo.setPulseUs(SERVO_CH[id], us);
}

static bool emergencyStop() {
  if (!actuatorLock()) return false;
  const bool stbyOk = xl.setPin(XL_STBY, false);
  const bool oeOk = xl.setPin(XL_OE, true);
  const bool ampOk = xl.setPin(XL_AMP_SD, false);
  const bool motorOk = pcaAllOffOrAbsent(pcaMotor);
  const bool servoOk = pcaAllOffOrAbsent(pcaServo);
  if (stbyOk) flagStby = false;
  if (oeOk) flagPwm = false;
  if (ampOk) flagAmp = false;
  if (stbyOk || motorOk) motorActiveMask = 0;
  actuatorUnlock();
  return stbyOk && oeOk && ampOk && motorOk && servoOk;
}

static void shutdownTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(500));
  ESP_LOGW(TAG, "shutdown: disabling peripherals and entering deep sleep");

  emergencyStop();
  radar_stop();

  if (cameraMutex && xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    cameraEnd(xl);
    xSemaphoreGive(cameraMutex);
  } else {
    cameraPower(xl, false);
  }

  if (oled.present() && oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    oled.clear();
    oled.show();
    xSemaphoreGive(oledMutex);
  }
  if (lcd.present()) lcd.backlight(false);

  const esp_err_t wifiErr = esp_wifi_stop();
  if (wifiErr != ESP_OK) ESP_LOGW(TAG, "shutdown: esp_wifi_stop failed: %s", esp_err_to_name(wifiErr));
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
  return sendJson(req, 200, buf);
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

static bool argsHasKey(const ReqArgs &a, const char *key) {
  char v[8];
  if (queryGet(a.q, key, v, sizeof(v))) return true;
  const std::string k = std::string("\"") + key + "\"";
  return a.body.find(k) != std::string::npos;
}

static esp_err_t handleRadarPost(httpd_req_t *req) {
  auto a = loadArgs(req);
  if (argsHasKey(a, "on")) {
    radar_set_enabled(argBool(a, "on", true));
    char buf[1536];
    radar_json_summary(buf, sizeof(buf));
    return sendJson(req, 200, buf);
  }
  const std::string cmd = argStr(a, "cmd", "");
  bool commandOk = false;
  if (cmd == "version") commandOk = radar_cmd_get_version();
  else if (cmd == "poll") commandOk = radar_cmd_get_det();
  else return sendJson(req, 400, "{\"ok\":false,\"error\":\"unsupported radar command\"}");
  if (!commandOk) {
    if (cmd == "poll" && !radar_enabled())
      return sendJson(req, 409, "{\"ok\":false,\"error\":\"radar acquisition is disabled\"}");
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
  body += "\"board\":\"AI通用机器狗_v4 / V1.0.0\",";
  body += "\"endpoints\":[";
  body += "{\"path\":\"/api/status\"},{\"path\":\"/api/estop\"},";
  body += "{\"path\":\"/api/shutdown\",\"note\":\"deep sleep; wake by power cycle or reset\"},";
  body += "{\"path\":\"/api/pwm\"},{\"path\":\"/api/stby\"},{\"path\":\"/api/amp\"},";
  body += "{\"path\":\"/api/servo\"},{\"path\":\"/api/servos\"},";
  body += "{\"path\":\"/api/motor\"},{\"path\":\"/api/motor/stop_all\"},";
  body += "{\"path\":\"/api/led\"},{\"path\":\"/api/encoders\"},";
  body += "{\"path\":\"/api/encoders/reset\"},{\"path\":\"/api/mic\"},";
  body += "{\"path\":\"/api/beep\"},{\"path\":\"/api/oled\"},";
  body += "{\"path\":\"/api/camera\"},{\"path\":\"/api/camera/capture\"},";
  body += "{\"path\":\"/stream\"},{\"path\":\"/api/lcd\"},{\"path\":\"/api/touch\"},";
  body += "{\"path\":\"/api/ota\",\"methods\":[\"GET\",\"POST\"],\"note\":\"upload .bin\"},";
  body += "{\"path\":\"/api/logs\",\"note\":\"incremental device logs\"},";
  body += "{\"path\":\"/api/radar\"},{\"path\":\"/api/radar/live\"},{\"path\":\"/radar\"}";
  body += "]}";
  return sendJson(req, 200, body);
}

static esp_err_t handleStatus(httpd_req_t *req) {
  wifi_ap_record_t ap = {};
  int rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

  const bool psramOk = esp_psram_is_initialized();
  const size_t psramBytes = psramOk ? esp_psram_get_size() : 0;
  const bool streaming = streamSlot && uxSemaphoreGetCount(streamSlot) == 0;
  const bool lcdReady = lcd.present();
  const bool touchReady = touch.present();
  char buf[896];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"fw\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
           "\"psram\":%s,\"psramBytes\":%u,"
           "\"xl9555\":%s,\"oled\":%s,\"pcaServo\":%s,\"pcaMotor\":%s,"
           "\"lcd\":%s,\"touch\":%s,\"camera\":%s,\"streaming\":%s,\"i2s\":%s,"
           "\"pwmEnable\":%s,\"motorStby\":%s,\"motorActiveMask\":%u,"
           "\"motorFailsafeMs\":%u,\"ampEnable\":%s,\"otaBusy\":%s,\"i2c\":%s}",
           FW_VERSION, ipStr, rssi, psramOk ? "true" : "false", (unsigned)psramBytes,
           xl.present() ? "true" : "false", oled.present() ? "true" : "false",
           pcaServo.present() ? "true" : "false", pcaMotor.present() ? "true" : "false",
           lcdReady ? "true" : "false", touchReady ? "true" : "false",
           cameraOk() ? "true" : "false", streaming ? "true" : "false",
           i2sReady ? "true" : "false", flagPwm ? "true" : "false",
           flagStby ? "true" : "false", (unsigned)motorActiveMask,
           (unsigned)(MOTOR_FAILSAFE_US / 1000), flagAmp ? "true" : "false",
           otaBusy ? "true" : "false", i2cScanJson().c_str());
  return sendJson(req, 200, buf);
}

static esp_err_t handleEstop(httpd_req_t *req) {
  (void)loadArgs(req);
  if (!emergencyStop())
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"estop hardware write failed\"}");
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
  if (!setPwmEnable(on)) return sendJson(req, 500, "{\"ok\":false,\"error\":\"xl9555 OE write failed\"}");
  char b[64];
  snprintf(b, sizeof(b), "{\"ok\":true,\"pwmEnable\":%s}", on ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleStby(httpd_req_t *req) {
  auto a = loadArgs(req);
  bool on = argBool(a, "on", true);
  if (!setStby(on)) return sendJson(req, 500, "{\"ok\":false,\"error\":\"xl9555 STBY write failed\"}");
  char b[64];
  snprintf(b, sizeof(b), "{\"ok\":true,\"motorStby\":%s}", on ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleAmp(httpd_req_t *req) {
  auto a = loadArgs(req);
  bool on = argBool(a, "on", true);
  if (!setAmp(on)) return sendJson(req, 500, "{\"ok\":false,\"error\":\"xl9555 AMP write failed\"}");
  char b[64];
  snprintf(b, sizeof(b), "{\"ok\":true,\"ampEnable\":%s}", on ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleServo(httpd_req_t *req) {
  auto a = loadArgs(req);
  int id = argInt(a, "id", -1);
  int angle = argInt(a, "angle", 90);
  if (id < 0 || id > 4) return sendJson(req, 400, "{\"ok\":false,\"error\":\"id 0..4 (T3-T7)\"}");
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
  int angles[5] = {90, 90, 90, 90, 90};
  bool provided[5] = {false, false, false, false, false};
  size_t arr = a.body.find("\"angles\"");
  if (arr != std::string::npos) {
    size_t lb = a.body.find('[', arr);
    size_t rb = a.body.find(']', lb);
    if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
      std::string inner = a.body.substr(lb + 1, rb - lb - 1);
      size_t start = 0;
      for (int i = 0; i < 5; i++) {
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
  for (int i = 0; i < 5; i++) {
    char key[4] = {'a', (char)('0' + i), 0, 0};
    char v[16];
    if (queryGet(a.q, key, v, sizeof(v))) {
      angles[i] = atoi(v);
      provided[i] = true;
    }
  }
  for (bool valueProvided : provided) {
    if (!valueProvided)
      return sendJson(req, 400, "{\"ok\":false,\"error\":\"all five servo angles are required\"}");
  }
  for (int i = 0; i < 5; i++) {
    if (angles[i] < 0) angles[i] = 0;
    if (angles[i] > 180) angles[i] = 180;
    if (!servoAngle((uint8_t)i, angles[i])) {
      char b[80];
      snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"servo write failed\",\"id\":%d}", i);
      return sendJson(req, 500, b);
    }
  }
  char out[96];
  snprintf(out, sizeof(out), "{\"ok\":true,\"angles\":[%d,%d,%d,%d,%d]}", angles[0], angles[1],
           angles[2], angles[3], angles[4]);
  return sendJson(req, 200, out);
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

static esp_err_t handleLed(httpd_req_t *req) {
  auto a = loadArgs(req);
  int id = argInt(a, "id", -1);
  int duty = argInt(a, "duty", 100);
  if (id < 0 || id > 2)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"id 0=LED_1 1=LED_2 2=LED_ALL\"}");
  if (duty < 0) duty = 0;
  if (duty > 100) duty = 100;
  // OE# 默认关闭；点亮时自动使能，避免 Web「全亮」静默失败
  if (!flagPwm) {
    if (duty == 0) {
      char b[96];
      snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"duty\":0,\"pwmEnable\":false}", id);
      return sendJson(req, 200, b);
    }
    if (!setPwmEnable(true))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"auto enable PWM (OE#) failed\"}");
  }
  uint16_t d = (uint16_t)((duty * 4095L) / 100);
  if (!actuatorLock())
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"actuator lock failed\"}");
  const bool ledOk = pcaMotor.setDuty(SPOT_CH[id], d);
  actuatorUnlock();
  if (!ledOk)
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"led write failed\"}");
  char b[96];
  snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"duty\":%d,\"pwmEnable\":true}", id, duty);
  return sendJson(req, 200, b);
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

static esp_err_t handleMic(httpd_req_t *req) {
  int32_t rms = 0, peak = 0;
  if (!board_i2s_mic_rms(rms, peak))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"i2s mic read failed\"}");
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"rms\":%ld,\"peak\":%ld}", (long)rms, (long)peak);
  return sendJson(req, 200, b);
}

static esp_err_t handleBeep(httpd_req_t *req) {
  auto a = loadArgs(req);
  int ms = argInt(a, "ms", 250);
  if (ms < 50) ms = 50;
  if (ms > 2000) ms = 2000;
  const bool wasOn = flagAmp;
  if (!wasOn && !setAmp(true))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"amp enable failed\"}");
  if (!wasOn) vTaskDelay(pdMS_TO_TICKS(5));
  const bool beepOk = board_i2s_beep((uint16_t)ms);
  const bool restoreOk = wasOn || setAmp(false);
  if (!beepOk || !restoreOk)
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"beep or amp restore failed\"}");
  char b[48];
  snprintf(b, sizeof(b), "{\"ok\":true,\"ms\":%d}", ms);
  return sendJson(req, 200, b);
}

static esp_err_t handleOled(httpd_req_t *req) {
  if (!oled.present()) return sendJson(req, 500, "{\"ok\":false,\"error\":\"oled not ready\"}");
  if (!oledMutex || xSemaphoreTake(oledMutex, pdMS_TO_TICKS(500)) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"oled busy\"}");
  auto a = loadArgs(req);
  std::string cmd = argStr(a, "cmd", "text");
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

static esp_err_t handleCamera(httpd_req_t *req) {
  auto a = loadArgs(req);
  if (req->method == HTTP_GET) {
    char b[160];
    snprintf(b, sizeof(b),
             "{\"ok\":true,\"camera\":%s,\"capture\":\"/api/camera/capture\",\"stream\":\"/stream\"}",
             cameraOk() ? "true" : "false");
    return sendJson(req, 200, b);
  }
  bool on = argBool(a, "on", true);
  if (!on && streamSlot && uxSemaphoreGetCount(streamSlot) == 0)
    return sendJson(req, 409, "{\"ok\":false,\"error\":\"stop the active stream before powering camera off\"}");
  if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"camera busy\"}");
  bool ok = true;
  if (on) {
    ok = cameraBegin(xl);
    xSemaphoreGive(cameraMutex);
    if (!ok)
      return sendJson(req, 500,
                      "{\"ok\":false,\"error\":\"camera init failed\",\"hint\":\"SCCB no ACK on IO4/IO5 — "
                      "check OV5640 FPC, CAM_1V2≈1.2V, CAM_PWDN (XL IO0_4)\"}");
    return sendJson(req, 200, "{\"ok\":true,\"camera\":true}");
  }
  cameraEnd(xl);
  xSemaphoreGive(cameraMutex);
  return sendJson(req, 200, "{\"ok\":true,\"camera\":false}");
}

static esp_err_t handleCameraCapture(httpd_req_t *req) {
  if (!cameraMutex || xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    return sendJson(req, 503, "{\"ok\":false,\"error\":\"camera busy\"}");
  if (!cameraOk() && !cameraBegin(xl)) {
    xSemaphoreGive(cameraMutex);
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"camera not ready\"}");
  }
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

static esp_err_t handleOtaInfo(httpd_req_t *req) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  const esp_app_desc_t *desc = esp_app_get_description();
  char b[384];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"fw\":\"%s\",\"project\":\"%s\",\"idf\":\"%s\","
           "\"running\":\"%s\",\"runningOffset\":%u,\"runningSize\":%u,"
           "\"next\":\"%s\",\"nextOffset\":%u,\"nextSize\":%u,\"busy\":%s,"
           "\"hint\":\"POST raw .bin to /api/ota (application/octet-stream)\"}",
           FW_VERSION, desc ? desc->project_name : "?", desc ? desc->idf_ver : "?",
           running ? running->label : "?", running ? (unsigned)running->address : 0,
           running ? (unsigned)running->size : 0, next ? next->label : "?",
           next ? (unsigned)next->address : 0, next ? (unsigned)next->size : 0,
           otaBusy ? "true" : "false");
  return sendJson(req, 200, b);
}

static esp_err_t handleOta(httpd_req_t *req) {
  if (req->method == HTTP_OPTIONS) return handleOptions(req);
  if (req->method == HTTP_GET) return handleOtaInfo(req);

  if (otaBusy) return sendJson(req, 400, "{\"ok\":false,\"error\":\"OTA already in progress\"}");
  if (req->content_len <= 0)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"Content-Length required; POST raw firmware .bin\"}");
  if (req->content_len < 1024)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"firmware too small\"}");

  const esp_partition_t *update = esp_ota_get_next_update_partition(nullptr);
  if (!update)
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"no OTA partition (need dual-OTA table)\"}");
  if ((size_t)req->content_len > update->size)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"firmware larger than OTA slot\"}");

  otaBusy = true;
  emergencyStop();
  if (cameraOk()) cameraPower(xl, false);

  ESP_LOGI(TAG, "OTA begin -> %s @0x%x size=%d", update->label, (unsigned)update->address,
           req->content_len);

  esp_ota_handle_t ota = 0;
  esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
  if (err != ESP_OK) {
    otaBusy = false;
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"esp_ota_begin %s\"}", esp_err_to_name(err));
    return sendJson(req, 500, b);
  }

  char buf[4096];
  int remaining = req->content_len;
  int written = 0;
  bool magicOk = false;
  while (remaining > 0) {
    int want = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
    int got = 0;
    while (got < want) {
      int n = httpd_req_recv(req, buf + got, want - got);
      if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
      if (n <= 0) {
        esp_ota_abort(ota);
        otaBusy = false;
        return sendJson(req, 500, "{\"ok\":false,\"error\":\"recv aborted\"}");
      }
      got += n;
    }

    if (!magicOk) {
      if ((uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
        esp_ota_abort(ota);
        otaBusy = false;
        return sendJson(req, 400,
                        "{\"ok\":false,\"error\":\"not ESP firmware (magic!=0xE9); use build/*.bin\"}");
      }
      magicOk = true;
    }

    err = esp_ota_write(ota, buf, got);
    if (err != ESP_OK) {
      esp_ota_abort(ota);
      otaBusy = false;
      char b[96];
      snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"esp_ota_write %s\"}", esp_err_to_name(err));
      return sendJson(req, 500, b);
    }
    remaining -= got;
    written += got;
    if ((written & 0x3FFFF) == 0) ESP_LOGI(TAG, "OTA %d / %d", written, req->content_len);
  }

  err = esp_ota_end(ota);
  if (err != ESP_OK) {
    otaBusy = false;
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"esp_ota_end %s (bad image?)\"}",
             esp_err_to_name(err));
    return sendJson(req, 500, b);
  }

  err = esp_ota_set_boot_partition(update);
  if (err != ESP_OK) {
    otaBusy = false;
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"set_boot %s\"}", esp_err_to_name(err));
    return sendJson(req, 500, b);
  }

  ESP_LOGI(TAG, "OTA ok %d bytes -> %s, reboot", written, update->label);
  char b[160];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"written\":%d,\"partition\":\"%s\",\"rebooting\":true}", written,
           update->label);
  sendJson(req, 200, b);
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
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
  config.max_uri_handlers = 56;
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
  registerUri(server, "/api/encoders", HTTP_GET, handleEncoders);
  registerUri(server, "/api/encoders", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/mic", HTTP_GET, handleMic);
  registerUri(server, "/api/mic", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/camera", HTTP_GET, handleCamera);
  registerUri(server, "/api/camera", HTTP_POST, handleCamera);
  registerUri(server, "/api/camera", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/camera/capture", HTTP_GET, handleCameraCapture);
  registerUri(server, "/api/camera/capture", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/stream", HTTP_GET, handleStream);
  registerUri(server, "/api/touch", HTTP_GET, handleTouch);
  registerUri(server, "/api/touch", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/ota", HTTP_GET, handleOta);
  registerUri(server, "/api/ota", HTTP_POST, handleOta);
  registerUri(server, "/api/ota", HTTP_OPTIONS, handleOptions);

  const char *mutating[] = {"/api/estop",         "/api/shutdown", "/api/pwm",  "/api/stby",
                            "/api/amp",           "/api/servo",    "/api/servos", "/api/motor",
                            "/api/motor/stop_all", "/api/led",      "/api/encoders/reset",
                            "/api/beep",          "/api/oled",     "/api/lcd"};
  esp_err_t (*fns[])(httpd_req_t *) = {
      handleEstop, handleShutdown, handlePwm, handleStby, handleAmp, handleServo, handleServos,
      handleMotor, handleMotorStopAll, handleLed, handleEncReset, handleBeep, handleOled, handleLcd};
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
    wifiOk = false;
    ipStr[0] = 0;
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&event->ip_info.ip));
    wifiOk = true;
    ESP_LOGI(TAG, "Got IP: %s", ipStr);
    if (oled.present() && oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      oled.printfLines("WiFi OK", ipStr, "open browser", FW_VERSION);
      xSemaphoreGive(oledMutex);
    }
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

static void background_task(void *) {
  while (true) {
    updateEnc34();
    radar_poll();
    bool motorExpired = false;
    if (actuatorLock()) {
      motorExpired = flagStby && motorActiveMask != 0 &&
                     esp_timer_get_time() - lastMotorCommandUs > MOTOR_FAILSAFE_US;
      actuatorUnlock();
    }
    if (motorExpired) {
      ESP_LOGW(TAG, "motor command timeout; entering standby");
      if (!setStby(false)) ESP_LOGE(TAG, "motor failsafe stop failed");
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

extern "C" void app_main(void) {
  device_log_init();
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  // Confirm current OTA image so rollback does not revert after a good boot
  esp_ota_mark_app_valid_cancel_rollback();

  ESP_LOGI(TAG, "=== EDA Robot LAN API (ESP-IDF) ===");
  ESP_LOGI(TAG, "FW %s  board AI通用机器狗_v4", FW_VERSION);
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run) ESP_LOGI(TAG, "running partition %s @0x%x", run->label, (unsigned)run->address);

  actuatorMutex = xSemaphoreCreateRecursiveMutex();
  cameraMutex = xSemaphoreCreateMutex();
  oledMutex = xSemaphoreCreateMutex();
  streamSlot = xSemaphoreCreateBinary();
  if (!actuatorMutex || !cameraMutex || !oledMutex || !streamSlot) {
    ESP_LOGE(TAG, "failed to create synchronization primitives");
    return;
  }
  xSemaphoreGive(streamSlot);

  radar_init();
  encoders_init();
  const bool radarBootUart = radar_start();
  ESP_LOGI(TAG, "radar early UART=%d", radarBootUart);
  board_i2c_init();

  bool okXl = xl.begin(ADDR_XL9555);
  bool okOled = oled.begin(ADDR_OLED);
  bool okS = pcaServo.begin(ADDR_PCA_SERVO, 50.0f);
  bool okM = pcaMotor.begin(ADDR_PCA_MOTOR, 1000.0f);

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
  }

  ESP_LOGI(TAG, "XL9555=%d OLED=%d PCA16=%d PCA23=%d LCD=%d TOUCH=%d", okXl, okOled, okS,
           okM, lcdOk, touchOk);
  flagPwm = flagStby = flagAmp = false;
  if (okXl) cameraPower(xl, false);

  i2sReady = board_i2s_init();
  ESP_LOGI(TAG, "I2S=%d", i2sReady);

  wifi_init();

  // wait up to 20s for IP
  for (int i = 0; i < 80 && !wifiOk; i++) vTaskDelay(pdMS_TO_TICKS(250));
  if (!wifiOk && okOled && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    oled.printfLines("WiFi FAIL", WIFI_SSID, "check AP", FW_VERSION);
    xSemaphoreGive(oledMutex);
  }

  if (xTaskCreate(background_task, "bg", 4096, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "background task start failed");
    emergencyStop();
    return;
  }
  setupHttp();
}
