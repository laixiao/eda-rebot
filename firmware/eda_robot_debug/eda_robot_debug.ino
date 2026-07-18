#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <SPI.h>

#include "board_config.h"
#include "xl9555.h"
#include "pca9685.h"
#include "ssd1306.h"
#include "camera_board.h"
#include "st7796.h"
#include "xpt2046.h"
#include "web_ui.h"

static const char *FW_VERSION = "1.2.0";

static WebServer server(80);
static XL9555 xl;
static PCA9685 pcaServo;
static PCA9685 pcaMotor;
static SSD1306 oled;
static ST7796 lcd;
static XPT2046 touch;
static SPIClass lcdSpi(HSPI);

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

// ---- encoders ENC1/ENC2 (GPIO ISR) ----
static void IRAM_ATTR onEnc1() {
  const bool a = digitalRead(PIN_ENC1_A);
  const bool b = digitalRead(PIN_ENC1_B);
  enc1 += (a == b) ? 1 : -1;
}

static void IRAM_ATTR onEnc2() {
  const bool a = digitalRead(PIN_ENC2_A);
  const bool b = digitalRead(PIN_ENC2_B);
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
static void addCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Max-Age", "600");
}

static void sendJson(int code, const String &body) {
  addCors();
  server.send(code, "application/json", body);
}

static void handleOptions() {
  addCors();
  server.send(204);
}

static String reqBody() { return server.arg("plain"); }

static int bodyInt(const String &body, const char *key, int defVal) {
  String k = String("\"") + key + "\"";
  int p = body.indexOf(k);
  if (p < 0) return defVal;
  p = body.indexOf(':', p);
  if (p < 0) return defVal;
  p++;
  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  return body.substring(p).toInt();
}

static bool bodyBool(const String &body, const char *key, bool defVal) {
  String k = String("\"") + key + "\"";
  int p = body.indexOf(k);
  if (p < 0) return defVal;
  p = body.indexOf(':', p);
  if (p < 0) return defVal;
  String rest = body.substring(p + 1);
  if (rest.indexOf("true") >= 0 &&
      (rest.indexOf("false") < 0 || rest.indexOf("true") < rest.indexOf("false")))
    return true;
  if (rest.indexOf("false") >= 0) return false;
  return bodyInt(body, key, defVal ? 1 : 0) != 0;
}

static String bodyStr(const String &body, const char *key) {
  String k = String("\"") + key + "\"";
  int p = body.indexOf(k);
  if (p < 0) return "";
  p = body.indexOf(':', p);
  if (p < 0) return "";
  p = body.indexOf('"', p);
  if (p < 0) return "";
  int q = body.indexOf('"', p + 1);
  if (q < 0) return "";
  return body.substring(p + 1, q);
}

static int argInt(const char *key, int defVal) {
  if (server.hasArg(key)) return server.arg(key).toInt();
  return bodyInt(reqBody(), key, defVal);
}

static bool argBool(const char *key, bool defVal) {
  if (server.hasArg(key)) {
    String v = server.arg(key);
    v.toLowerCase();
    if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return v.toInt() != 0;
  }
  return bodyBool(reqBody(), key, defVal);
}

static String argStr(const char *key, const char *defVal = "") {
  if (server.hasArg(key)) return server.arg(key);
  String s = bodyStr(reqBody(), key);
  return s.length() ? s : String(defVal);
}

static String i2cScanJson() {
  String s = "[";
  bool first = true;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (!first) s += ',';
      first = false;
      s += String(a);
    }
  }
  s += ']';
  return s;
}

// ---- actuator helpers ----
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

// ---- I2S mic / amp ----
#include <ESP_I2S.h>
#include <math.h>

static I2SClass i2sMic;
static I2SClass i2sAmp;
static bool i2sMicOk = false;
static bool i2sAmpOk = false;

static bool initI2S() {
  i2sMic.setPins(PIN_I2S_MIC_SCK, PIN_I2S_MIC_WS, -1, PIN_I2S_MIC_SD);
  if (!i2sMic.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO,
                    I2S_STD_SLOT_LEFT)) {
    return false;
  }
  i2sMicOk = true;

  i2sAmp.setPins(PIN_I2S_AMP_BCLK, PIN_I2S_AMP_LRC, PIN_I2S_AMP_DIN);
  if (!i2sAmp.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    return false;
  }
  i2sAmpOk = true;
  return true;
}

static bool readMicRms(int32_t &rms, int32_t &peak) {
  if (!i2sMicOk) return false;
  int32_t samples[256];
  size_t n = i2sMic.readBytes((char *)samples, sizeof(samples));
  n /= sizeof(int32_t);
  if (n == 0) return false;
  int64_t acc = 0;
  peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = samples[i] >> 14;
    if (v < 0) v = -v;
    acc += v;
    if (v > peak) peak = v;
  }
  rms = (int32_t)(acc / (int64_t)n);
  return true;
}

static bool playBeep(uint16_t ms = 300) {
  if (!i2sAmpOk) return false;
  if (!flagAmp) setAmp(true);
  const int rate = 16000;
  const int freq = 1000;
  const size_t n = (size_t)rate * ms / 1000;
  int16_t frame[2];
  for (size_t i = 0; i < n; i++) {
    float t = (float)i / rate;
    int16_t s = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * freq * t));
    frame[0] = s;
    frame[1] = s;
    if (i2sAmp.write((const uint8_t *)frame, sizeof(frame)) != sizeof(frame)) return false;
  }
  frame[0] = frame[1] = 0;
  for (int i = 0; i < 200; i++) {
    i2sAmp.write((const uint8_t *)frame, sizeof(frame));
  }
  return true;
}

// ---- HTTP handlers ----
static void handleRoot() {
  addCors();
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleApiIndex() {
  String body = "{";
  body += "\"ok\":true,\"fw\":\"";
  body += FW_VERSION;
  body += "\",";
  body += "\"board\":\"AI通用机器狗_v4 / V1.0.0\",";
  body += "\"endpoints\":[";
  body += "{\"path\":\"/api/status\"},";
  body += "{\"path\":\"/api/estop\"},";
  body += "{\"path\":\"/api/pwm\",\"params\":{\"on\":\"bool\"}},";
  body += "{\"path\":\"/api/stby\",\"params\":{\"on\":\"bool\"}},";
  body += "{\"path\":\"/api/amp\",\"params\":{\"on\":\"bool\"}},";
  body += "{\"path\":\"/api/servo\",\"params\":{\"id\":\"0..4\",\"angle\":\"0..180\"}},";
  body += "{\"path\":\"/api/servos\",\"params\":{\"a0..a4\":\"angle\"}},";
  body += "{\"path\":\"/api/motor\",\"params\":{\"id\":\"0..3\",\"dir\":\"-1|0|1\",\"duty\":\"0..100\"}},";
  body += "{\"path\":\"/api/motor/stop_all\"},";
  body += "{\"path\":\"/api/led\",\"params\":{\"id\":\"0..2\",\"duty\":\"0..100\"}},";
  body += "{\"path\":\"/api/encoders\"},";
  body += "{\"path\":\"/api/encoders/reset\"},";
  body += "{\"path\":\"/api/mic\"},";
  body += "{\"path\":\"/api/beep\",\"params\":{\"ms\":\"int\"}},";
  body += "{\"path\":\"/api/oled\",\"params\":{\"cmd\":\"text|clear|fill\",\"text\":\"string\"}},";
  body += "{\"path\":\"/api/camera\",\"params\":{\"on\":\"bool\"}},";
  body += "{\"path\":\"/api/camera/capture\"},";
  body += "{\"path\":\"/stream\"},";
  body += "{\"path\":\"/api/lcd\",\"params\":{\"cmd\":\"init|on|off|fill|color\",\"color\":\"RGB565 hex\"}},";
  body += "{\"path\":\"/api/touch\"}";
  body += "]}";
  sendJson(200, body);
}

static void handleStatus() {
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  String body = "{";
  body += "\"ok\":true,\"fw\":\"" + String(FW_VERSION) + "\",";
  body += "\"ip\":\"" + ip + "\",\"rssi\":" + String(WiFi.RSSI()) + ",";
  body += "\"xl9555\":" + String(xl.present() ? "true" : "false") + ",";
  body += "\"oled\":" + String(oled.present() ? "true" : "false") + ",";
  body += "\"pcaServo\":" + String(pcaServo.present() ? "true" : "false") + ",";
  body += "\"pcaMotor\":" + String(pcaMotor.present() ? "true" : "false") + ",";
  body += "\"lcd\":" + String(lcdOk ? "true" : "false") + ",";
  body += "\"camera\":" + String(cameraOk() ? "true" : "false") + ",";
  body += "\"i2s\":" + String(i2sReady ? "true" : "false") + ",";
  body += "\"pwmEnable\":" + String(flagPwm ? "true" : "false") + ",";
  body += "\"motorStby\":" + String(flagStby ? "true" : "false") + ",";
  body += "\"ampEnable\":" + String(flagAmp ? "true" : "false") + ",";
  body += "\"i2c\":" + i2cScanJson();
  body += "}";
  sendJson(200, body);
}

static void handleEstop() {
  emergencyStop();
  sendJson(200, "{\"ok\":true,\"estop\":true}");
}

static void handlePwm() {
  bool on = argBool("on", true);
  if (!setPwmEnable(on)) {
    sendJson(500, "{\"ok\":false,\"error\":\"xl9555 OE write failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"pwmEnable\":") + (on ? "true" : "false") + "}");
}

static void handleStby() {
  bool on = argBool("on", true);
  if (!setStby(on)) {
    sendJson(500, "{\"ok\":false,\"error\":\"xl9555 STBY write failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"motorStby\":") + (on ? "true" : "false") + "}");
}

static void handleAmp() {
  bool on = argBool("on", true);
  if (!setAmp(on)) {
    sendJson(500, "{\"ok\":false,\"error\":\"xl9555 AMP write failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"ampEnable\":") + (on ? "true" : "false") + "}");
}

static void handleServo() {
  int id = argInt("id", -1);
  int angle = argInt("angle", 90);
  if (id < 0 || id > 4) {
    sendJson(400, "{\"ok\":false,\"error\":\"id 0..4 (T3-T7)\"}");
    return;
  }
  if (!flagPwm) {
    sendJson(400, "{\"ok\":false,\"error\":\"enable PWM first: /api/pwm?on=1\"}");
    return;
  }
  if (!servoAngle((uint8_t)id, angle)) {
    sendJson(500, "{\"ok\":false,\"error\":\"pca9685 servo write failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"id\":") + id + ",\"angle\":" + angle + "}");
}

static void handleServos() {
  if (!flagPwm) {
    sendJson(400, "{\"ok\":false,\"error\":\"enable PWM first: /api/pwm?on=1\"}");
    return;
  }
  int angles[5];
  bool any = false;
  String body = reqBody();
  int arr = body.indexOf("\"angles\"");
  if (arr >= 0) {
    int lb = body.indexOf('[', arr);
    int rb = body.indexOf(']', lb);
    if (lb >= 0 && rb > lb) {
      String inner = body.substring(lb + 1, rb);
      int start = 0;
      for (int i = 0; i < 5; i++) {
        angles[i] = 90;
        int comma = inner.indexOf(',', start);
        String tok = (comma < 0) ? inner.substring(start) : inner.substring(start, comma);
        tok.trim();
        if (tok.length()) {
          angles[i] = tok.toInt();
          any = true;
        }
        if (comma < 0) break;
        start = comma + 1;
      }
    }
  }
  for (int i = 0; i < 5; i++) {
    char key[4] = {'a', (char)('0' + i), 0, 0};
    if (server.hasArg(key)) {
      angles[i] = server.arg(key).toInt();
      any = true;
    } else if (!any) {
      angles[i] = 90;
    }
  }
  if (!any) {
    for (int i = 0; i < 5; i++) angles[i] = 90;
  }
  for (int i = 0; i < 5; i++) {
    if (!servoAngle((uint8_t)i, angles[i])) {
      sendJson(500, "{\"ok\":false,\"error\":\"servo write failed\",\"id\":" + String(i) + "}");
      return;
    }
  }
  String out = "{\"ok\":true,\"angles\":[";
  for (int i = 0; i < 5; i++) {
    if (i) out += ',';
    out += String(angles[i]);
  }
  out += "]}";
  sendJson(200, out);
}

static void handleMotor() {
  int id = argInt("id", -1);
  int dir = argInt("dir", 0);
  int duty = argInt("duty", 40);
  if (id < 0 || id > 3) {
    sendJson(400, "{\"ok\":false,\"error\":\"id 0..3\"}");
    return;
  }
  if (!flagPwm || !flagStby) {
    sendJson(400, "{\"ok\":false,\"error\":\"enable PWM and STBY first\"}");
    return;
  }
  if (!motorDrive((uint8_t)id, dir, duty)) {
    sendJson(500, "{\"ok\":false,\"error\":\"motor write failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"id\":") + id + ",\"dir\":" + dir + ",\"duty\":" + duty +
                     "}");
}

static void handleMotorStopAll() {
  for (uint8_t i = 0; i < 4; i++) motorStop(i);
  sendJson(200, "{\"ok\":true}");
}

static void handleLed() {
  int id = argInt("id", -1);
  int duty = argInt("duty", 100);
  if (id < 0 || id > 2) {
    sendJson(400, "{\"ok\":false,\"error\":\"id 0=LED_1 1=LED_2 2=LED_ALL\"}");
    return;
  }
  if (!flagPwm) {
    sendJson(400, "{\"ok\":false,\"error\":\"enable PWM first: /api/pwm?on=1\"}");
    return;
  }
  if (duty < 0) duty = 0;
  if (duty > 100) duty = 100;
  uint16_t d = (uint16_t)((duty * 4095L) / 100);
  if (!pcaMotor.setDuty(SPOT_CH[id], d)) {
    sendJson(500, "{\"ok\":false,\"error\":\"led write failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"id\":") + id + ",\"duty\":" + duty + "}");
}

static void handleEncoders() {
  updateEnc34();
  uint8_t p0 = 0;
  xl.readPort(0, p0);
  noInterrupts();
  int32_t e1 = enc1, e2 = enc2;
  interrupts();
  String body = "{";
  body += "\"ok\":true,";
  body += "\"enc1\":" + String(e1) + ",\"enc2\":" + String(e2) + ",";
  body += "\"enc3\":" + String(enc3) + ",\"enc4\":" + String(enc4) + ",";
  body += "\"xlPort0\":" + String(p0);
  body += "}";
  sendJson(200, body);
}

static void handleEncReset() {
  noInterrupts();
  enc1 = enc2 = 0;
  interrupts();
  enc3 = enc4 = 0;
  sendJson(200, "{\"ok\":true}");
}

static void handleMic() {
  int32_t rms = 0, peak = 0;
  if (!readMicRms(rms, peak)) {
    sendJson(500, "{\"ok\":false,\"error\":\"i2s mic read failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"rms\":") + rms + ",\"peak\":" + peak + "}");
}

static void handleBeep() {
  int ms = argInt("ms", 250);
  if (ms < 50) ms = 50;
  if (ms > 2000) ms = 2000;
  if (!playBeep((uint16_t)ms)) {
    sendJson(500, "{\"ok\":false,\"error\":\"beep failed\"}");
    return;
  }
  sendJson(200, String("{\"ok\":true,\"ms\":") + ms + "}");
}

static void handleOled() {
  String cmd = argStr("cmd", "text");
  if (cmd == "clear") {
    oled.clear();
    oled.show();
  } else if (cmd == "fill") {
    oled.fill();
    oled.show();
  } else {
    String text = argStr("text", "EDA Robot");
    char line[32];
    snprintf(line, sizeof(line), "%s", text.c_str());
    oled.printfLines(line, WiFi.localIP().toString().c_str(), FW_VERSION, "LAN API");
  }
  sendJson(200, "{\"ok\":true}");
}

static void handleCamera() {
  // GET: 状态；POST/?on=1 开关摄像头
  if (server.method() == HTTP_GET && !server.hasArg("on") && reqBody().length() == 0) {
    sendJson(200, String("{\"ok\":true,\"camera\":") + (cameraOk() ? "true" : "false") +
                       ",\"capture\":\"/api/camera/capture\",\"stream\":\"/stream\"}");
    return;
  }
  bool on = argBool("on", true);
  if (on) {
    if (!cameraBegin(xl)) {
      sendJson(500, "{\"ok\":false,\"error\":\"camera init failed (check FPC / PSRAM)\"}");
      return;
    }
    sendJson(200, "{\"ok\":true,\"camera\":true}");
  } else {
    cameraEnd(xl);
    sendJson(200, "{\"ok\":true,\"camera\":false}");
  }
}

static void handleCameraCapture() {
  if (!cameraOk() && !cameraBegin(xl)) {
    sendJson(500, "{\"ok\":false,\"error\":\"camera not ready\"}");
    return;
  }
  uint8_t *buf = nullptr;
  size_t len = 0;
  if (!cameraCaptureJpeg(buf, len) || !buf || len == 0) {
    sendJson(500, "{\"ok\":false,\"error\":\"capture failed\"}");
    return;
  }
  addCors();
  server.setContentLength(len);
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  client.write(buf, len);
  cameraReleaseFrame();
}

static void handleStream() {
  if (!cameraOk() && !cameraBegin(xl)) {
    sendJson(500, "{\"ok\":false,\"error\":\"camera not ready\"}");
    return;
  }
  WiFiClient client = server.client();
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: multipart/x-mixed-replace; boundary=frame"));
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Connection: close"));
  client.println();

  uint32_t t0 = millis();
  while (client.connected() && millis() - t0 < 120000) {
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (!cameraCaptureJpeg(buf, len)) {
      delay(10);
      continue;
    }
    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                  (unsigned)len);
    client.write(buf, len);
    client.print("\r\n");
    cameraReleaseFrame();
    delay(30);
    if (!client.connected()) break;
  }
}

static uint16_t parseColor(const String &s, uint16_t defVal) {
  if (!s.length()) return defVal;
  String t = s;
  if (t.startsWith("0x") || t.startsWith("0X")) t = t.substring(2);
  char *end = nullptr;
  unsigned long v = strtoul(t.c_str(), &end, 16);
  if (end == t.c_str()) return defVal;
  return (uint16_t)v;
}

static void handleLcd() {
  String cmd = argStr("cmd", "status");
  if (cmd == "init" || (cmd == "status" && !lcdOk && argBool("on", false))) {
    lcdOk = lcd.begin(lcdSpi, xl);
    touch.begin(lcdSpi, xl);
    if (!lcdOk) {
      sendJson(500, "{\"ok\":false,\"error\":\"lcd init failed\"}");
      return;
    }
    sendJson(200, "{\"ok\":true,\"lcd\":true,\"w\":320,\"h\":480}");
    return;
  }
  if (cmd == "status") {
    sendJson(200, String("{\"ok\":true,\"lcd\":") + (lcdOk ? "true" : "false") + "}");
    return;
  }
  if (!lcdOk) {
    lcdOk = lcd.begin(lcdSpi, xl);
    touch.begin(lcdSpi, xl);
  }
  if (!lcdOk) {
    sendJson(500, "{\"ok\":false,\"error\":\"lcd not ready\"}");
    return;
  }
  if (cmd == "on") {
    lcd.backlight(true);
    sendJson(200, "{\"ok\":true,\"backlight\":true}");
  } else if (cmd == "off") {
    lcd.backlight(false);
    sendJson(200, "{\"ok\":true,\"backlight\":false}");
  } else if (cmd == "fill" || cmd == "color") {
    uint16_t c = parseColor(argStr("color", "001F"), 0x001F);
    lcd.fillScreen(c);
    sendJson(200, String("{\"ok\":true,\"color\":") + c + "}");
  } else if (cmd == "rotate") {
    int r = argInt("r", 0);
    lcd.setRotation((uint8_t)r);
    sendJson(200, String("{\"ok\":true,\"rotation\":") + r + ",\"w\":" + lcd.width() +
                       ",\"h\":" + lcd.height() + "}");
  } else {
    sendJson(400, "{\"ok\":false,\"error\":\"cmd=init|on|off|fill|rotate\"}");
  }
}

static void handleTouch() {
  uint16_t x = 0, y = 0, z = 0;
  bool pressed = touch.touched();
  bool ok = touch.read(x, y, z);
  String body = "{";
  body += "\"ok\":true,";
  body += "\"irq\":" + String(pressed ? "true" : "false") + ",";
  body += "\"valid\":" + String(ok ? "true" : "false") + ",";
  body += "\"x\":" + String(x) + ",\"y\":" + String(y) + ",\"z\":" + String(z);
  body += "}";
  sendJson(200, body);
}

static void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    handleOptions();
    return;
  }
  sendJson(404, "{\"ok\":false,\"error\":\"not found\",\"hint\":\"GET /api\"}");
}

static void onBoth(const char *uri, void (*fn)()) {
  server.on(uri, HTTP_GET, fn);
  server.on(uri, HTTP_POST, fn);
  server.on(uri, HTTP_OPTIONS, handleOptions);
}

static void setupHttp() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleApiIndex);
  server.on("/api/", HTTP_GET, handleApiIndex);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/encoders", HTTP_GET, handleEncoders);
  server.on("/api/encoders", HTTP_OPTIONS, handleOptions);
  server.on("/api/mic", HTTP_GET, handleMic);
  server.on("/api/mic", HTTP_OPTIONS, handleOptions);
  server.on("/api/camera", HTTP_GET, handleCamera);
  server.on("/api/camera", HTTP_POST, handleCamera);
  server.on("/api/camera", HTTP_OPTIONS, handleOptions);
  server.on("/api/camera/capture", HTTP_GET, handleCameraCapture);
  server.on("/api/camera/capture", HTTP_OPTIONS, handleOptions);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/api/touch", HTTP_GET, handleTouch);
  server.on("/api/touch", HTTP_OPTIONS, handleOptions);

  onBoth("/api/estop", handleEstop);
  onBoth("/api/pwm", handlePwm);
  onBoth("/api/stby", handleStby);
  onBoth("/api/amp", handleAmp);
  onBoth("/api/servo", handleServo);
  onBoth("/api/servos", handleServos);
  onBoth("/api/motor", handleMotor);
  onBoth("/api/motor/stop_all", handleMotorStopAll);
  onBoth("/api/led", handleLed);
  onBoth("/api/encoders/reset", handleEncReset);
  onBoth("/api/beep", handleBeep);
  onBoth("/api/oled", handleOled);
  onBoth("/api/lcd", handleLcd);

  server.onNotFound(handleNotFound);
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== EDA Robot LAN API ==="));
  Serial.printf("FW %s  board AI通用机器狗_v4\n", FW_VERSION);

  pinMode(PIN_ENC1_A, INPUT_PULLUP);
  pinMode(PIN_ENC1_B, INPUT_PULLUP);
  pinMode(PIN_ENC2_A, INPUT_PULLUP);
  pinMode(PIN_ENC2_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC1_A), onEnc1, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC2_A), onEnc2, CHANGE);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  bool okXl = xl.begin(Wire, ADDR_XL9555);
  bool okOled = oled.begin(Wire, ADDR_OLED);
  bool okS = pcaServo.begin(Wire, ADDR_PCA_SERVO, 50.0f);
  bool okM = pcaMotor.begin(Wire, ADDR_PCA_MOTOR, 1000.0f);

  // LCD/触摸 SPI（CS 等在 XL9555）
  lcdSpi.begin(PIN_LCD_SCK, PIN_LCD_MISO, PIN_LCD_MOSI, -1);
  if (okXl) {
    lcdOk = lcd.begin(lcdSpi, xl);
    touch.begin(lcdSpi, xl);
    if (lcdOk) {
      lcd.fillScreen(0x0000);
      lcd.fillRect(0, 0, 320, 40, 0x001F);
    }
  }

  Serial.printf("XL9555=%d OLED=%d PCA16=%d PCA23=%d LCD=%d\n", okXl, okOled, okS, okM, lcdOk);
  flagPwm = false;
  flagStby = false;
  flagAmp = false;

  // 摄像头默认不上电；需要时 /api/camera?on=1
  if (okXl) cameraPower(xl, false);

  i2sReady = initI2S();
  Serial.printf("I2S=%d\n", i2sReady);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi connecting to '%s' ...\n", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP "));
    Serial.println(WiFi.localIP());
    if (okOled) {
      oled.printfLines("WiFi OK", WiFi.localIP().toString().c_str(), "open browser",
                       FW_VERSION);
    }
  } else {
    Serial.println(F("WiFi FAILED — retrying in loop"));
    if (okOled) oled.printfLines("WiFi FAIL", WIFI_SSID, "check AP", FW_VERSION);
  }

  setupHttp();
  Serial.println(F("HTTP :80 ready  GET /api"));
}

void loop() {
  server.handleClient();
  updateEnc34();

  static uint32_t lastWifi = 0;
  if (millis() - lastWifi > 3000) {
    lastWifi = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }
}
