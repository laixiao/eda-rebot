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
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_psram.h"
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

static const char *TAG = "eda_robot";
static const char *FW_VERSION = "3.1.0";
static volatile bool otaBusy = false;
static volatile bool shutdownPending = false;

static XL9555 xl;
static PCA9685 pca;
static SSD1306 oled;

static bool flagPwm = false;
static bool flagAmp = false;
static bool flagRadarPwr = false;
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
  const bool ok = xl.setPin(XL_AMP_SD, on);
  if (ok) flagAmp = on;
  actuatorUnlock();
  return ok;
}

/** Q4 P-MOS：拉低 IO0_1 = 开雷达 3V3 */
static bool setRadarPower(bool on) {
  if (!actuatorLock()) return false;
  const bool ok = xl.setPin(XL_RADAR_PWR, !on);
  if (ok) flagRadarPwr = on;
  actuatorUnlock();
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

static bool setSpotDuty(uint8_t id, int dutyPct) {
  if (id >= SPOT_COUNT) return false;
  if (dutyPct < 0) dutyPct = 0;
  if (dutyPct > 100) dutyPct = 100;
  uint16_t d = (uint16_t)((dutyPct * 4095L) / 100);
  return pca.setDuty(SPOT_CH[id], d);
}

static bool emergencyStop() {
  if (!actuatorLock()) return false;
  const bool oeOk = xl.setPin(XL_OE, true);
  const bool ampOk = xl.setPin(XL_AMP_SD, false);
  const bool radarOk = xl.setPin(XL_RADAR_PWR, true);
  const bool pwmOk = pcaAllOffOrAbsent();
  if (oeOk) flagPwm = false;
  if (ampOk) flagAmp = false;
  if (radarOk) flagRadarPwr = false;
  actuatorUnlock();
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
  }
  if (argsHasKey(a, "on")) {
    radar_set_enabled(argBool(a, "on", true));
  }
  if (argsHasKey(a, "power") || argsHasKey(a, "on")) {
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
  const std::string cmd = argStr(a, "cmd", "");
  bool commandOk = false;
  if (cmd == "version") commandOk = radar_cmd_get_version();
  else if (cmd == "poll") commandOk = radar_cmd_get_det();
  else return sendJson(req, 400, "{\"ok\":false,\"error\":\"use power/on/cmd=version|poll\"}");
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
  body += "\"board\":\"AI通用机器人_v6-1 / V1.0.0\",";
  body += "\"endpoints\":[";
  body += "{\"path\":\"/api/status\"},{\"path\":\"/api/estop\"},";
  body += "{\"path\":\"/api/shutdown\",\"note\":\"deep sleep; wake by power cycle or reset\"},";
  body += "{\"path\":\"/api/pwm\"},{\"path\":\"/api/amp\"},";
  body += "{\"path\":\"/api/servo\",\"note\":\"id 0..1 = T3/T4\"},";
  body += "{\"path\":\"/api/servos\"},";
  body += "{\"path\":\"/api/led\",\"note\":\"id 0=LED_1 1=LED_2 2=LED_ALL; need LED_ALL for 1/2\"},";
  body += "{\"path\":\"/api/i2c\",\"note\":\"?full=1 for bus scan\"},";
  body += "{\"path\":\"/api/mic\"},{\"path\":\"/api/beep\"},{\"path\":\"/api/oled\"},";
  body += "{\"path\":\"/api/ota\",\"methods\":[\"GET\",\"POST\"]},";
  body += "{\"path\":\"/api/logs\"},";
  body += "{\"path\":\"/api/radar\",\"note\":\"power + acquire on/off\"},";
  body += "{\"path\":\"/api/radar/live\"},{\"path\":\"/radar\"}";
  body += "]}";
  return sendJson(req, 200, body);
}

static esp_err_t handleStatus(httpd_req_t *req) {
  wifi_ap_record_t ap = {};
  int rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

  const bool psramOk = esp_psram_is_initialized();
  const size_t psramBytes = psramOk ? esp_psram_get_size() : 0;
  char buf[640];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"fw\":\"%s\",\"board\":\"v6-1\",\"ip\":\"%s\",\"rssi\":%d,"
           "\"psram\":%s,\"psramBytes\":%u,"
           "\"xl9555\":%s,\"oled\":%s,\"pca9685\":%s,\"i2s\":%s,"
           "\"pwmEnable\":%s,\"ampEnable\":%s,\"radarPower\":%s,\"otaBusy\":%s,\"i2c\":%s}",
           FW_VERSION, ipStr, rssi, psramOk ? "true" : "false", (unsigned)psramBytes,
           xl.present() ? "true" : "false", oled.present() ? "true" : "false",
           pca.present() ? "true" : "false", i2sReady ? "true" : "false",
           flagPwm ? "true" : "false", flagAmp ? "true" : "false",
           flagRadarPwr ? "true" : "false", otaBusy ? "true" : "false", i2cScanJson().c_str());
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
  config.max_uri_handlers = 48;
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
  registerUri(server, "/api/i2c", HTTP_GET, handleI2c);
  registerUri(server, "/api/i2c", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/mic", HTTP_GET, handleMic);
  registerUri(server, "/api/mic", HTTP_OPTIONS, handleOptions);
  registerUri(server, "/api/ota", HTTP_GET, handleOta);
  registerUri(server, "/api/ota", HTTP_POST, handleOta);
  registerUri(server, "/api/ota", HTTP_OPTIONS, handleOptions);

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
    wifiOk = false;
    ipStr[0] = 0;
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&event->ip_info.ip));
    wifiOk = true;
    ESP_LOGI(TAG, "Got IP: %s", ipStr);
    if (oled.present() && oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      oled.printfLines("WiFi OK", ipStr, "打开浏览器", FW_VERSION);
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

static void background_task(void *) {
  while (true) {
    uint8_t p0 = 0;
    if (xl.readPort(0, p0)) radar_set_gpio_out((p0 >> XL_RADAR_OUT) & 1);
    radar_poll();
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

  esp_ota_mark_app_valid_cancel_rollback();

  ESP_LOGI(TAG, "=== EDA Robot LAN API (ESP-IDF) ===");
  ESP_LOGI(TAG, "FW %s  board AI通用机器人_v6-1", FW_VERSION);
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run) ESP_LOGI(TAG, "running partition %s @0x%x", run->label, (unsigned)run->address);

  actuatorMutex = xSemaphoreCreateRecursiveMutex();
  oledMutex = xSemaphoreCreateMutex();
  if (!actuatorMutex || !oledMutex) {
    ESP_LOGE(TAG, "failed to create synchronization primitives");
    return;
  }

  radar_init();
  const bool radarBootUart = radar_start();
  ESP_LOGI(TAG, "radar UART=%d (power still off until /api/radar power=1)", radarBootUart);
  board_i2c_init();

  bool okXl = xl.begin(ADDR_XL9555);
  bool okOled = oled.begin(ADDR_OLED, 100000);
  if (!okOled) okOled = oled.begin(0x3D, 100000);
  bool okPca = pca.begin(ADDR_PCA9685, 50.0f);

  ESP_LOGI(TAG, "XL9555=%d OLED=%d PCA9685=%d CJK=%u", okXl, okOled, okPca,
           (unsigned)font_cjk_count());
  if (okOled && oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    oled.printfLines("EDA Robot", "汉字字库就绪", "等待 WiFi...", FW_VERSION);
    xSemaphoreGive(oledMutex);
  }
  flagPwm = flagAmp = flagRadarPwr = false;

  i2sReady = board_i2s_init();
  ESP_LOGI(TAG, "I2S=%d", i2sReady);

  wifi_init();

  for (int i = 0; i < 80 && !wifiOk; i++) vTaskDelay(pdMS_TO_TICKS(250));
  if (!wifiOk && okOled && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    oled.printfLines("WiFi FAIL", WIFI_SSID, "检查热点", FW_VERSION);
    xSemaphoreGive(oledMutex);
  }

  if (xTaskCreate(background_task, "bg", 4096, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "background task start failed");
    emergencyStop();
    return;
  }
  setupHttp();
}
