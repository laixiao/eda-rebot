#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <math.h>

#include "freertos/FreeRTOS.h"
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

static const char *TAG = "eda_robot";
static const char *FW_VERSION = "2.0.0";

static XL9555 xl;
static PCA9685 pcaServo;
static PCA9685 pcaMotor;
static SSD1306 oled;
static ST7796 lcd;
static XPT2046 touch;

static volatile int32_t enc1 = 0;
static volatile int32_t enc2 = 0;
static int32_t enc3 = 0;
static int32_t enc4 = 0;
static uint8_t prevXlA = 0;

static bool flagPwm = false;
static bool flagStby = false;
static bool flagAmp = false;
static bool i2sReady = false;
static bool lcdOk = false;
static bool wifiOk = false;
static char ipStr[16] = {0};

static httpd_handle_t server = nullptr;

// ---- encoders ----
static void IRAM_ATTR onEnc1(void *) {
  const int a = gpio_get_level((gpio_num_t)PIN_ENC1_A);
  const int b = gpio_get_level((gpio_num_t)PIN_ENC1_B);
  enc1 += (a == b) ? 1 : -1;
}

static void IRAM_ATTR onEnc2(void *) {
  const int a = gpio_get_level((gpio_num_t)PIN_ENC2_A);
  const int b = gpio_get_level((gpio_num_t)PIN_ENC2_B);
  enc2 += (a == b) ? 1 : -1;
}

static void updateEnc34() {
  uint8_t p0 = 0;
  if (!xl.readPort(0, p0)) return;
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
                             : code == 404 ? "404 Not Found"
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
  std::string s = "[";
  bool first = true;
  for (uint8_t addr = 1; addr < 127; addr++) {
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
static bool setPwmEnable(bool on) {
  if (!xl.setPin(XL_OE, !on)) return false;
  flagPwm = on;
  return true;
}

static bool setStby(bool on) {
  if (!xl.setPin(XL_STBY, on)) return false;
  flagStby = on;
  return true;
}

static bool setAmp(bool on) {
  if (!xl.setPin(XL_AMP_SD, on)) return false;
  flagAmp = on;
  return true;
}

static bool motorStop(uint8_t id) {
  if (id > 3) return false;
  return pcaMotor.setDuty(MOTOR_IN1[id], 0) && pcaMotor.setDuty(MOTOR_IN2[id], 0);
}

static bool motorDrive(uint8_t id, int dir, int dutyPct) {
  if (id > 3) return false;
  if (dutyPct < 0) dutyPct = 0;
  if (dutyPct > 100) dutyPct = 100;
  uint16_t duty = (uint16_t)((dutyPct * 4095L) / 100);
  if (dir == 0 || duty == 0) return motorStop(id);
  if (dir > 0) {
    return pcaMotor.setDuty(MOTOR_IN1[id], duty) && pcaMotor.setDuty(MOTOR_IN2[id], 0);
  }
  return pcaMotor.setDuty(MOTOR_IN1[id], 0) && pcaMotor.setDuty(MOTOR_IN2[id], duty);
}

static bool servoAngle(uint8_t id, int angle) {
  if (id > 4) return false;
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  uint16_t us =
      SERVO_US_MIN + (uint16_t)((uint32_t)(SERVO_US_MAX - SERVO_US_MIN) * angle / 180);
  return pcaServo.setPulseUs(SERVO_CH[id], us);
}

static void emergencyStop() {
  setStby(false);
  setPwmEnable(false);
  setAmp(false);
  pcaMotor.allOff();
  pcaServo.allOff();
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

static esp_err_t handleApiIndex(httpd_req_t *req) {
  std::string body = "{";
  body += "\"ok\":true,\"fw\":\"";
  body += FW_VERSION;
  body += "\",\"framework\":\"esp-idf\",";
  body += "\"board\":\"AI通用机器狗_v4 / V1.0.0\",";
  body += "\"endpoints\":[";
  body += "{\"path\":\"/api/status\"},{\"path\":\"/api/estop\"},";
  body += "{\"path\":\"/api/pwm\"},{\"path\":\"/api/stby\"},{\"path\":\"/api/amp\"},";
  body += "{\"path\":\"/api/servo\"},{\"path\":\"/api/servos\"},";
  body += "{\"path\":\"/api/motor\"},{\"path\":\"/api/motor/stop_all\"},";
  body += "{\"path\":\"/api/led\"},{\"path\":\"/api/encoders\"},";
  body += "{\"path\":\"/api/encoders/reset\"},{\"path\":\"/api/mic\"},";
  body += "{\"path\":\"/api/beep\"},{\"path\":\"/api/oled\"},";
  body += "{\"path\":\"/api/camera\"},{\"path\":\"/api/camera/capture\"},";
  body += "{\"path\":\"/stream\"},{\"path\":\"/api/lcd\"},{\"path\":\"/api/touch\"}";
  body += "]}";
  return sendJson(req, 200, body);
}

static esp_err_t handleStatus(httpd_req_t *req) {
  updateEnc34();
  wifi_ap_record_t ap = {};
  int rssi = 0;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

  char buf[768];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"fw\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
           "\"xl9555\":%s,\"oled\":%s,\"pcaServo\":%s,\"pcaMotor\":%s,"
           "\"lcd\":%s,\"camera\":%s,\"i2s\":%s,"
           "\"pwmEnable\":%s,\"motorStby\":%s,\"ampEnable\":%s,\"i2c\":%s}",
           FW_VERSION, ipStr, rssi, xl.present() ? "true" : "false",
           oled.present() ? "true" : "false", pcaServo.present() ? "true" : "false",
           pcaMotor.present() ? "true" : "false", lcdOk ? "true" : "false",
           cameraOk() ? "true" : "false", i2sReady ? "true" : "false",
           flagPwm ? "true" : "false", flagStby ? "true" : "false",
           flagAmp ? "true" : "false", i2cScanJson().c_str());
  return sendJson(req, 200, buf);
}

static esp_err_t handleEstop(httpd_req_t *req) {
  (void)loadArgs(req);
  emergencyStop();
  return sendJson(req, 200, "{\"ok\":true,\"estop\":true}");
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
  if (!flagPwm) return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM first: /api/pwm?on=1\"}");
  if (!servoAngle((uint8_t)id, angle))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"pca9685 servo write failed\"}");
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"angle\":%d}", id, angle);
  return sendJson(req, 200, b);
}

static esp_err_t handleServos(httpd_req_t *req) {
  auto a = loadArgs(req);
  if (!flagPwm) return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM first: /api/pwm?on=1\"}");
  int angles[5] = {90, 90, 90, 90, 90};
  bool any = false;
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
          any = true;
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
      any = true;
    }
  }
  (void)any;
  for (int i = 0; i < 5; i++) {
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
  if (!flagPwm || !flagStby)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM and STBY first\"}");
  if (!motorDrive((uint8_t)id, dir, duty))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"motor write failed\"}");
  char b[96];
  snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"dir\":%d,\"duty\":%d}", id, dir, duty);
  return sendJson(req, 200, b);
}

static esp_err_t handleMotorStopAll(httpd_req_t *req) {
  (void)loadArgs(req);
  for (uint8_t i = 0; i < 4; i++) motorStop(i);
  return sendJson(req, 200, "{\"ok\":true}");
}

static esp_err_t handleLed(httpd_req_t *req) {
  auto a = loadArgs(req);
  int id = argInt(a, "id", -1);
  int duty = argInt(a, "duty", 100);
  if (id < 0 || id > 2)
    return sendJson(req, 400, "{\"ok\":false,\"error\":\"id 0=LED_1 1=LED_2 2=LED_ALL\"}");
  if (!flagPwm) return sendJson(req, 400, "{\"ok\":false,\"error\":\"enable PWM first: /api/pwm?on=1\"}");
  if (duty < 0) duty = 0;
  if (duty > 100) duty = 100;
  uint16_t d = (uint16_t)((duty * 4095L) / 100);
  if (!pcaMotor.setDuty(SPOT_CH[id], d))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"led write failed\"}");
  char b[80];
  snprintf(b, sizeof(b), "{\"ok\":true,\"id\":%d,\"duty\":%d}", id, duty);
  return sendJson(req, 200, b);
}

static esp_err_t handleEncoders(httpd_req_t *req) {
  updateEnc34();
  uint8_t p0 = 0;
  xl.readPort(0, p0);
  int32_t e1 = enc1, e2 = enc2;
  char b[160];
  snprintf(b, sizeof(b),
           "{\"ok\":true,\"enc1\":%ld,\"enc2\":%ld,\"enc3\":%ld,\"enc4\":%ld,\"xlPort0\":%u}",
           (long)e1, (long)e2, (long)enc3, (long)enc4, p0);
  return sendJson(req, 200, b);
}

static esp_err_t handleEncReset(httpd_req_t *req) {
  (void)loadArgs(req);
  enc1 = 0;
  enc2 = 0;
  enc3 = 0;
  enc4 = 0;
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
  if (!flagAmp) setAmp(true);
  if (!board_i2s_beep((uint16_t)ms)) return sendJson(req, 500, "{\"ok\":false,\"error\":\"beep failed\"}");
  char b[48];
  snprintf(b, sizeof(b), "{\"ok\":true,\"ms\":%d}", ms);
  return sendJson(req, 200, b);
}

static esp_err_t handleOled(httpd_req_t *req) {
  auto a = loadArgs(req);
  std::string cmd = argStr(a, "cmd", "text");
  if (cmd == "clear") {
    oled.clear();
    oled.show();
  } else if (cmd == "fill") {
    oled.fill();
    oled.show();
  } else {
    std::string text = argStr(a, "text", "EDA Robot");
    oled.printfLines(text.c_str(), ipStr, FW_VERSION, "LAN API");
  }
  return sendJson(req, 200, "{\"ok\":true}");
}

static esp_err_t handleCamera(httpd_req_t *req) {
  auto a = loadArgs(req);
  bool hasOn = a.q.find("on=") != std::string::npos || a.body.find("\"on\"") != std::string::npos;
  if (req->method == HTTP_GET && !hasOn) {
    char b[160];
    snprintf(b, sizeof(b),
             "{\"ok\":true,\"camera\":%s,\"capture\":\"/api/camera/capture\",\"stream\":\"/stream\"}",
             cameraOk() ? "true" : "false");
    return sendJson(req, 200, b);
  }
  bool on = argBool(a, "on", true);
  if (on) {
    if (!cameraBegin(xl))
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"camera init failed (check FPC / PSRAM)\"}");
    return sendJson(req, 200, "{\"ok\":true,\"camera\":true}");
  }
  cameraEnd(xl);
  return sendJson(req, 200, "{\"ok\":true,\"camera\":false}");
}

static esp_err_t handleCameraCapture(httpd_req_t *req) {
  if (!cameraOk() && !cameraBegin(xl))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"camera not ready\"}");
  uint8_t *buf = nullptr;
  size_t len = 0;
  if (!cameraCaptureJpeg(buf, len) || !buf || len == 0)
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"capture failed\"}");
  addCors(req);
  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t err = httpd_resp_send(req, (const char *)buf, len);
  cameraReleaseFrame();
  return err;
}

static esp_err_t handleStream(httpd_req_t *req) {
  if (!cameraOk() && !cameraBegin(xl))
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"camera not ready\"}");

  addCors(req);
  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
  httpd_resp_set_hdr(req, "Connection", "close");

  int64_t t0 = esp_timer_get_time();
  while ((esp_timer_get_time() - t0) < 120000000LL) {
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (!cameraCaptureJpeg(buf, len)) {
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
      break;
    }
    cameraReleaseFrame();
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  httpd_resp_send_chunk(req, nullptr, 0);
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
    touch.begin(xl);
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
    touch.begin(xl);
  }
  if (!lcdOk) return sendJson(req, 500, "{\"ok\":false,\"error\":\"lcd not ready\"}");
  if (cmd == "on") {
    lcd.backlight(true);
    return sendJson(req, 200, "{\"ok\":true,\"backlight\":true}");
  }
  if (cmd == "off") {
    lcd.backlight(false);
    return sendJson(req, 200, "{\"ok\":true,\"backlight\":false}");
  }
  if (cmd == "fill" || cmd == "color") {
    uint16_t c = parseColor(argStr(a, "color", "001F"), 0x001F);
    lcd.fillScreen(c);
    char b[64];
    snprintf(b, sizeof(b), "{\"ok\":true,\"color\":%u}", c);
    return sendJson(req, 200, b);
  }
  if (cmd == "rotate") {
    int r = argInt(a, "r", 0);
    lcd.setRotation((uint8_t)r);
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":true,\"rotation\":%d,\"w\":%u,\"h\":%u}", r, lcd.width(),
             lcd.height());
    return sendJson(req, 200, b);
  }
  return sendJson(req, 400, "{\"ok\":false,\"error\":\"cmd=init|on|off|fill|rotate\"}");
}

static esp_err_t handleTouch(httpd_req_t *req) {
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

static void registerUri(httpd_handle_t s, const char *path, httpd_method_t method,
                        esp_err_t (*handler)(httpd_req_t *)) {
  httpd_uri_t u = URI(path, method, handler);
  httpd_register_uri_handler(s, &u);
}

static void setupHttp() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 48;
  config.stack_size = 8192;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }

  registerUri(server, "/", HTTP_GET, handleRoot);
  registerUri(server, "/api", HTTP_GET, handleApiIndex);
  registerUri(server, "/api/", HTTP_GET, handleApiIndex);
  registerUri(server, "/api/status", HTTP_GET, handleStatus);
  registerUri(server, "/api/status", HTTP_OPTIONS, handleOptions);
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

  const char *both[] = {"/api/estop",         "/api/pwm",  "/api/stby", "/api/amp",
                        "/api/servo",         "/api/servos", "/api/motor", "/api/motor/stop_all",
                        "/api/led",           "/api/encoders/reset", "/api/beep", "/api/oled",
                        "/api/lcd"};
  esp_err_t (*fns[])(httpd_req_t *) = {
      handleEstop, handlePwm, handleStby, handleAmp, handleServo, handleServos, handleMotor,
      handleMotorStopAll, handleLed, handleEncReset, handleBeep, handleOled, handleLcd};
  for (size_t i = 0; i < sizeof(both) / sizeof(both[0]); i++) {
    registerUri(server, both[i], HTTP_GET, fns[i]);
    registerUri(server, both[i], HTTP_POST, fns[i]);
    registerUri(server, both[i], HTTP_OPTIONS, handleOptions);
  }

  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handleNotFound);
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
    if (oled.present()) oled.printfLines("WiFi OK", ipStr, "open browser", FW_VERSION);
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
  io.pin_bit_mask = (1ULL << PIN_ENC1_A) | (1ULL << PIN_ENC1_B) | (1ULL << PIN_ENC2_A) |
                    (1ULL << PIN_ENC2_B);
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&io);
  gpio_install_isr_service(0);
  gpio_isr_handler_add((gpio_num_t)PIN_ENC1_A, onEnc1, nullptr);
  gpio_isr_handler_add((gpio_num_t)PIN_ENC2_A, onEnc2, nullptr);
}

static void background_task(void *) {
  while (true) {
    updateEnc34();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  ESP_LOGI(TAG, "=== EDA Robot LAN API (ESP-IDF) ===");
  ESP_LOGI(TAG, "FW %s  board AI通用机器狗_v4", FW_VERSION);

  encoders_init();
  board_i2c_init();

  bool okXl = xl.begin(ADDR_XL9555);
  bool okOled = oled.begin(ADDR_OLED);
  bool okS = pcaServo.begin(ADDR_PCA_SERVO, 50.0f);
  bool okM = pcaMotor.begin(ADDR_PCA_MOTOR, 1000.0f);

  if (okXl) {
    lcdOk = lcd.begin(xl);
    touch.begin(xl);
    if (lcdOk) {
      lcd.fillScreen(0x0000);
      lcd.fillRect(0, 0, 320, 40, 0x001F);
    }
  }

  ESP_LOGI(TAG, "XL9555=%d OLED=%d PCA16=%d PCA23=%d LCD=%d", okXl, okOled, okS, okM, lcdOk);
  flagPwm = flagStby = flagAmp = false;
  if (okXl) cameraPower(xl, false);

  i2sReady = board_i2s_init();
  ESP_LOGI(TAG, "I2S=%d", i2sReady);

  wifi_init();

  // wait up to 20s for IP
  for (int i = 0; i < 80 && !wifiOk; i++) vTaskDelay(pdMS_TO_TICKS(250));
  if (!wifiOk && okOled) oled.printfLines("WiFi FAIL", WIFI_SSID, "check AP", FW_VERSION);

  setupHttp();
  xTaskCreate(background_task, "bg", 4096, nullptr, 5, nullptr);
}
