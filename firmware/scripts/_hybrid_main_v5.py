# -*- coding: utf-8 -*-
"""Hybridize FAN main.cpp onto v5 board_config (dual PCA, soft radar power, v5 APIs)."""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(r"c:\Users\Administrator\Desktop\eda\eda-rebot")
MAIN = ROOT / "firmware/main/main.cpp"


def extract_func(text: str, name: str) -> str:
    m = re.search(rf"^static[^\n]*\b{re.escape(name)}\s*\(", text, re.M)
    if not m:
        raise SystemExit(f"missing function {name}")
    i = text.find("{", m.end())
    depth = 0
    j = i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start() : j + 1]
        j += 1
    raise SystemExit(f"unclosed {name}")


def extract_between(text: str, start_pat: str, end_pat: str) -> str:
    m0 = re.search(start_pat, text, re.M)
    m1 = re.search(end_pat, text, re.M)
    if not m0 or not m1 or m1.start() <= m0.start():
        raise SystemExit(f"bad range {start_pat} .. {end_pat}")
    return text[m0.start() : m1.start()]


def main() -> None:
    src = MAIN.read_text(encoding="utf-8")
    v5 = subprocess.check_output(
        ["git", "show", "56479b0:firmware/main/main.cpp"], cwd=ROOT
    ).decode("utf-8", errors="replace")

    # --- includes ---
    if '#include "camera_board.h"' not in src:
        src = src.replace(
            '#include "board_i2c.h"\n',
            '#include "board_i2c.h"\n#include "board_spi.h"\n#include "camera_board.h"\n#include "st7796.h"\n#include "xpt2046.h"\n',
        )

    # --- version / board name ---
    src = src.replace('static const char *FW_VERSION = "3.6.26";', 'static const char *FW_VERSION = "3.7.0-v5";')
    src = src.replace("AI通用机器人_v6-1", "AI通用机器人_v5")
    src = src.replace("board AI通用机器人_v6-1", "board AI通用机器人_v5")

    # --- globals: dual PCA + v5 periph ---
    src = src.replace("static PCA9685 pca;", "static PCA9685 pcaServo;\nstatic PCA9685 pcaMotor;")
    if "static ST7796 lcd;" not in src:
        src = src.replace(
            "static SSD1306 oled;",
            "static SSD1306 oled;\nstatic ST7796 lcd;\nstatic XPT2046 touch;",
        )
    src = src.replace("static int servoAngleDeg[2] = {90, 90};", "static int servoAngleDeg[5] = {90, 90, 90, 90, 90};")
    src = src.replace("static int cfgSnapSrv[2] = {90, 90};", "static int cfgSnapSrv[5] = {90, 90, 90, 90, 90};")

    # insert encoder / motor / lcd flags after flagAmp if missing
    if "static volatile int32_t enc1" not in src:
        insert = """
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
"""
        src = src.replace("static bool flagAmp = false;", "static bool flagAmp = false;" + insert)

    # --- dual PCA / spot on motor / servo on servo ---
    src = src.replace("add(ADDR_PCA9685, pca.present());",
                      "add(ADDR_PCA_SERVO, pcaServo.present());\n  add(ADDR_PCA_MOTOR, pcaMotor.present());")
    src = src.replace(
        "static const uint8_t kAddrs[] = {ADDR_XL9555, ADDR_OLED, 0x3D, ADDR_PCA9685};",
        "static const uint8_t kAddrs[] = {ADDR_XL9555, ADDR_OLED, 0x3D, ADDR_PCA_SERVO, ADDR_PCA_MOTOR};",
    )
    src = src.replace(
        "static bool pcaAllOffOrAbsent() { return !pca.present() || pca.allOff(); }",
        "static bool pcaAllOffOrAbsent() {\n"
        "  const bool sOk = !pcaServo.present() || pcaServo.allOff();\n"
        "  const bool mOk = !pcaMotor.present() || pcaMotor.allOff();\n"
        "  return sOk && mOk;\n"
        "}",
    )

    # setPulseUs servos
    src = src.replace("pca.setPulseUs(SERVO_CH[id], us)", "pcaServo.setPulseUs(SERVO_CH[id], us)")
    # spots
    src = src.replace("pca.setDuty(SPOT_CH[id], d)", "pcaMotor.setDuty(SPOT_CH[id], d)")

    # remaining pca.present / begin
    src = src.replace("pca.present()", "pcaMotor.present()")  # fan/spot paths default motor
    # fix servo restore loops that should use pcaServo — after blanket replace, fix servoAngle path:
    # servoAngle already uses pcaServo.setPulseUs. cfgRestore that calls servoAngle is fine.
    # Boot begin:
    src = src.replace(
        "bool okPca = board_i2c_probe(ADDR_PCA9685) && pca.begin(ADDR_PCA9685, 50.0f);",
        "bool okPcaS = board_i2c_probe(ADDR_PCA_SERVO) && pcaServo.begin(ADDR_PCA_SERVO, 50.0f);\n"
        "  bool okPcaM = board_i2c_probe(ADDR_PCA_MOTOR) && pcaMotor.begin(ADDR_PCA_MOTOR, 1000.0f);\n"
        "  bool okPca = okPcaS || okPcaM;",
    )

    # status JSON may still say pcaMotor for both — also report servo
    src = src.replace(
        'pcaMotor.present() ? "true" : "false", i2sReady ? "true" : "false",',
        'pcaServo.present() ? "true" : "false", pcaMotor.present() ? "true" : "false", i2sReady ? "true" : "false",',
    )
    # if format string has single pca field, widen carefully later

    # --- radar power soft ---
    # replace setPin XL_RADAR_PWR blocks
    src = re.sub(
        r"const bool ok = xl\.setPin\(XL_RADAR_PWR, !on\);",
        "const bool ok = true;  // v5: no MOSFET; soft power flag only",
        src,
    )
    src = re.sub(
        r"radarOk = xl\.setPin\(XL_RADAR_PWR, true\);",
        "radarOk = true;  // v5: external VCC, soft flag",
        src,
    )
    if "XL_RADAR_PWR" in src:
        raise SystemExit("XL_RADAR_PWR still present after soft patch")

    # --- NVS srv0..srv4 ---
    if 'nvs_set_u8(h, "srv2"' not in src:
        src = src.replace(
            'nvs_set_u8(h, "srv0", clampU8(cfgSnapSrv[0]));\n  nvs_set_u8(h, "srv1", clampU8(cfgSnapSrv[1]));',
            'for (uint8_t i = 0; i < SERVO_COUNT; i++) {\n'
            '    char key[8]; snprintf(key, sizeof(key), "srv%u", (unsigned)i);\n'
            '    nvs_set_u8(h, key, clampU8(cfgSnapSrv[i]));\n'
            "  }",
        )
    # load srv loop — replace srv0/srv1 pair with loop if still pairwise
    if 'nvs_get_u8(h, "srv0"' in src and "SERVO_COUNT" not in src[src.find('nvs_get_u8(h, "srv0"') : src.find('nvs_get_u8(h, "srv0"') + 400]:
        old_load = extract_between(
            src,
            r'if \(nvs_get_u8\(h, "srv0"',
            r'if \(nvs_get_u8\(h, "rsch_en"',
        )
        # fragile — do simple replace of the two blocks
        src = re.sub(
            r'if \(nvs_get_u8\(h, "srv0", &v\) == ESP_OK\) \{\s*'
            r"cfgSnapSrv\[0\] = v > 180 \? 180 : \(int\)v;\s*"
            r"servoAngleDeg\[0\] = cfgSnapSrv\[0\];\s*"
            r"\}\s*"
            r'if \(nvs_get_u8\(h, "srv1", &v\) == ESP_OK\) \{\s*'
            r"cfgSnapSrv\[1\] = v > 180 \? 180 : \(int\)v;\s*"
            r"servoAngleDeg\[1\] = cfgSnapSrv\[1\];\s*"
            r"\}",
            'for (uint8_t i = 0; i < SERVO_COUNT; i++) {\n'
            '    char key[8]; snprintf(key, sizeof(key), "srv%u", (unsigned)i);\n'
            "    if (nvs_get_u8(h, key, &v) == ESP_OK) {\n"
            "      cfgSnapSrv[i] = v > 180 ? 180 : (int)v;\n"
            "      servoAngleDeg[i] = cfgSnapSrv[i];\n"
            "    }\n"
            "  }",
            src,
            count=1,
            flags=re.S,
        )

    # --- inject encoder helpers from v5 after oledMutex / before addCors if not present ---
    if "static void IRAM_ATTR onEnc1" not in src:
        enc_block = extract_between(v5, r"^// ---- encoders ----", r"^// ---- HTTP helpers ----")
        # place before addCors
        src = src.replace("static void addCors(httpd_req_t *req) {", enc_block + "\nstatic void addCors(httpd_req_t *req) {")

    # --- inject motor/stby/camera helpers & handlers from v5 ---
    # Pull setStby/setMotor style helpers if named
    for fname in [
        "handleStby",
        "handleMotor",
        "handleMotorStopAll",
        "handleEncoders",
        "handleEncReset",
        "handleCamera",
        "handleCameraCapture",
        "streamAsync",
        "handleStream",
        "handleLcd",
        "handleTouch",
    ]:
        if re.search(rf"\b{fname}\s*\(", src):
            print("keep existing", fname)
            continue
        try:
            body = extract_func(v5, fname)
        except SystemExit as e:
            print("skip", e)
            continue
        # rewrite pcaMotor/pcaServo already correct in v5 extract
        # Insert before handleApiIndex or before start_webserver
        anchor = "static esp_err_t handleApiIndex"
        if anchor not in src:
            anchor = "static esp_err_t handleStatus"
        src = src.replace(anchor, body + "\n\n" + anchor)
        print("injected", fname)

    # Also need setMotor / motor failsafe helpers used by handlers — search v5
    for fname in ["setStby", "motorStop", "motorStopAll", "motorDrive", "encoders_init"]:
        if re.search(rf"\b{fname}\s*\(", src):
            print("keep helper", fname)
            continue
        try:
            body = extract_func(v5, fname)
            src = src.replace("static bool setPwmEnable", body + "\n\nstatic bool setPwmEnable")
            print("injected helper", fname)
        except SystemExit:
            print("helper missing extract", fname)

    # --- URI registration: add routes before server start ---
    uri_snip = """
  static httpd_uri_t uri_stby = {.uri="/api/stby", .method=HTTP_POST, .handler=handleStby, .user_ctx=nullptr};
  static httpd_uri_t uri_motor = {.uri="/api/motor", .method=HTTP_POST, .handler=handleMotor, .user_ctx=nullptr};
  static httpd_uri_t uri_motor_stop = {.uri="/api/motor/stop_all", .method=HTTP_POST, .handler=handleMotorStopAll, .user_ctx=nullptr};
  static httpd_uri_t uri_enc = {.uri="/api/encoders", .method=HTTP_GET, .handler=handleEncoders, .user_ctx=nullptr};
  static httpd_uri_t uri_enc_rst = {.uri="/api/encoders/reset", .method=HTTP_POST, .handler=handleEncReset, .user_ctx=nullptr};
  static httpd_uri_t uri_cam = {.uri="/api/camera", .method=HTTP_GET, .handler=handleCamera, .user_ctx=nullptr};
  static httpd_uri_t uri_cam_post = {.uri="/api/camera", .method=HTTP_POST, .handler=handleCamera, .user_ctx=nullptr};
  static httpd_uri_t uri_cam_cap = {.uri="/api/camera/capture", .method=HTTP_GET, .handler=handleCameraCapture, .user_ctx=nullptr};
  static httpd_uri_t uri_stream = {.uri="/stream", .method=HTTP_GET, .handler=handleStream, .user_ctx=nullptr};
  static httpd_uri_t uri_lcd = {.uri="/api/lcd", .method=HTTP_POST, .handler=handleLcd, .user_ctx=nullptr};
  static httpd_uri_t uri_touch = {.uri="/api/touch", .method=HTTP_GET, .handler=handleTouch, .user_ctx=nullptr};
"""
    if "/api/camera" not in src or "uri_cam" not in src:
        # find httpd_register_uri_handler batch — insert registrations near other registers
        m = re.search(r"httpd_register_uri_handler\(server, &uri_led\);", src)
        if not m:
            m = re.search(r"httpd_register_uri_handler\(server, &uri_pwm\);", src)
        if m:
            regs = """
  httpd_register_uri_handler(server, &uri_stby);
  httpd_register_uri_handler(server, &uri_motor);
  httpd_register_uri_handler(server, &uri_motor_stop);
  httpd_register_uri_handler(server, &uri_enc);
  httpd_register_uri_handler(server, &uri_enc_rst);
  httpd_register_uri_handler(server, &uri_cam);
  httpd_register_uri_handler(server, &uri_cam_post);
  httpd_register_uri_handler(server, &uri_cam_cap);
  httpd_register_uri_handler(server, &uri_stream);
  httpd_register_uri_handler(server, &uri_lcd);
  httpd_register_uri_handler(server, &uri_touch);
"""
            # insert uri defs before first httpd_register
            first_reg = src.find("httpd_register_uri_handler")
            # find a good spot: after uri structs — insert defs just before first register
            src = src[:first_reg] + uri_snip + "\n" + src[first_reg:]
            # re-find and add register calls after uri_led or pwm
            src = src.replace(
                "httpd_register_uri_handler(server, &uri_pwm);",
                "httpd_register_uri_handler(server, &uri_pwm);" + regs,
                1,
            )
            print("registered v5 uris")
        else:
            print("WARN: could not find uri register anchor")

    # --- boot: create camera mutex / stream slot / encoders / lcd ---
    if "cameraMutex = xSemaphoreCreateMutex" not in src:
        boot = """
  cameraMutex = xSemaphoreCreateMutex();
  streamSlot = xSemaphoreCreateCounting(1, 1);
  // ENC2 only (ENC1 owned by radar UART)
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << PIN_ENC2_A) | (1ULL << PIN_ENC2_B);
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
  gpio_set_intr_type((gpio_num_t)PIN_ENC2_A, GPIO_INTR_ANYEDGE);
  gpio_install_isr_service(0);
  gpio_isr_handler_add((gpio_num_t)PIN_ENC2_A, onEnc2, nullptr);
  if (board_spi_init()) {
    lcdOk = lcd.begin();
    touchOk = touch.begin();
    if (lcdOk) lcd.fillScreen(0x0000);
  }
"""
        # insert after actuatorMutex create if present
        if "actuatorMutex =" in src:
            src = src.replace(
                "actuatorMutex = xSemaphoreCreateRecursiveMutex();",
                "actuatorMutex = xSemaphoreCreateRecursiveMutex();" + boot,
                1,
            )
        else:
            src = src.replace(
                "oledMutex = xSemaphoreCreateMutex();",
                "oledMutex = xSemaphoreCreateMutex();" + boot,
                1,
            )

    # call updateEnc34 in background loop if present
    if "updateEnc34();" not in src and "updateEnc34" in src:
        src = src.replace(
            "if (fanGestureEnable && pcaMotor.present()) updateFanGesture(rs);",
            "updateEnc34();\n    if (fanGestureEnable && pcaMotor.present()) updateFanGesture(rs);",
        )

    # Fix servo-related pcaMotor.present that should be pcaServo for restoreOutputs servo path:
    # cfgRestoreOutputs: `if (!pcaMotor.present()) return false` before servos — should allow either
    src = src.replace(
        "if (!pcaMotor.present()) return false;\n  for (uint8_t i = 0; i < SERVO_COUNT; i++)",
        "if (!pcaServo.present() && !pcaMotor.present()) return false;\n  for (uint8_t i = 0; i < SERVO_COUNT; i++)",
    )

    # needPwm uses either pca
    src = src.replace(
        "if (needPwm && pcaMotor.present())",
        "if (needPwm && (pcaServo.present() || pcaMotor.present()))",
    )
    src = src.replace(
        "} else if (cfgWantPwm && !pcaMotor.present())",
        "} else if (cfgWantPwm && !pcaServo.present() && !pcaMotor.present())",
    )

    if "XL_RADAR_PWR" in src:
        raise SystemExit("XL_RADAR_PWR still in source")
    if re.search(r"\bpca\.", src):
        for i, line in enumerate(src.splitlines(), 1):
            if re.search(r"\bpca\.", line):
                print("leftover pca.", i, line)
        raise SystemExit("leftover pca. references")

    MAIN.write_text(src, encoding="utf-8")
    print("wrote", MAIN, "bytes", MAIN.stat().st_size)


if __name__ == "__main__":
    main()
