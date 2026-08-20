# -*- coding: utf-8 -*-
"""Finish v5 hybrid: missing helpers, URI routes, boot hooks."""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(r"c:\Users\Administrator\Desktop\eda\eda-rebot")
MAIN = ROOT / "firmware/main/main.cpp"


def extract_func(text: str, name: str) -> str:
    m = re.search(rf"^static[^\n]*\b{re.escape(name)}\s*\(", text, re.M)
    if not m:
        raise SystemExit(f"missing {name}")
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


def has_def(src: str, name: str) -> bool:
    return bool(re.search(rf"^static[^\n]*\b{re.escape(name)}\s*\(", src, re.M))


def main() -> None:
    src = MAIN.read_text(encoding="utf-8")
    v5 = subprocess.check_output(
        ["git", "show", "56479b0:firmware/main/main.cpp"], cwd=ROOT
    ).decode("utf-8", errors="replace")

    # Inject missing helpers before setPwmEnable
    for fname in ["setStby", "motorStop", "motorStopAll", "motorDrive", "encoders_init"]:
        if has_def(src, fname):
            print("ok", fname)
            continue
        body = extract_func(v5, fname)
        if "static bool setPwmEnable" not in src:
            raise SystemExit("no setPwmEnable anchor")
        src = src.replace("static bool setPwmEnable", body + "\n\nstatic bool setPwmEnable", 1)
        print("injected", fname)

    # Ensure XPT2046 touch global
    if "static XPT2046 touch;" not in src:
        src = src.replace("static ST7796 lcd;", "static ST7796 lcd;\nstatic XPT2046 touch;")

    # Register v5 URIs in setupHttp
    if 'registerUri(server, "/api/camera"' not in src:
        block = """
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
"""
        src = src.replace(
            'registerUri(server, "/api/rescue", HTTP_OPTIONS, handleOptions);',
            'registerUri(server, "/api/rescue", HTTP_OPTIONS, handleOptions);\n' + block,
            1,
        )
        print("registered v5 routes")

    # Boot: camera mutex / enc2 / spi lcd — only once
    if "streamSlot = xSemaphoreCreateCounting" not in src:
        boot = """
  if (!cameraMutex) cameraMutex = xSemaphoreCreateMutex();
  if (!streamSlot) streamSlot = xSemaphoreCreateCounting(1, 1);
  gpio_config_t enc_io = {};
  enc_io.pin_bit_mask = (1ULL << PIN_ENC2_A) | (1ULL << PIN_ENC2_B);
  enc_io.mode = GPIO_MODE_INPUT;
  enc_io.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&enc_io);
  gpio_set_intr_type((gpio_num_t)PIN_ENC2_A, GPIO_INTR_ANYEDGE);
  gpio_install_isr_service(0);
  gpio_isr_handler_add((gpio_num_t)PIN_ENC2_A, onEnc2, nullptr);
  if (board_spi_init()) {
    lcdOk = lcd.begin();
    touchOk = touch.begin();
  }
"""
        # Prefer after oledMutex create in app_main
        if "oledMutex = xSemaphoreCreateMutex();" in src:
            src = src.replace(
                "oledMutex = xSemaphoreCreateMutex();",
                "oledMutex = xSemaphoreCreateMutex();" + boot,
                1,
            )
            print("boot hooks after oledMutex")
        elif "actuatorMutex = xSemaphoreCreateRecursiveMutex();" in src:
            src = src.replace(
                "actuatorMutex = xSemaphoreCreateRecursiveMutex();",
                "actuatorMutex = xSemaphoreCreateRecursiveMutex();" + boot,
                1,
            )
            print("boot hooks after actuatorMutex")
        else:
            print("WARN: no boot anchor")

    # background: updateEnc34
    if "updateEnc34();" not in src and has_def(src, "updateEnc34"):
        if "if (fanGestureEnable && pcaMotor.present()) updateFanGesture(rs);" in src:
            src = src.replace(
                "if (fanGestureEnable && pcaMotor.present()) updateFanGesture(rs);",
                "updateEnc34();\n    if (fanGestureEnable && pcaMotor.present()) updateFanGesture(rs);",
                1,
            )
            print("bg updateEnc34")

    # Soft radar: any remaining XL_RADAR_PWR
    if "XL_RADAR_PWR" in src:
        src = src.replace("xl.setPin(XL_RADAR_PWR, !on)", "true /* v5 soft radar power */")
        src = src.replace("xl.setPin(XL_RADAR_PWR, true)", "true /* v5 soft radar power */")
        src = src.replace("xl.setPin(XL_RADAR_PWR, false)", "true /* v5 soft radar power */")
        if "XL_RADAR_PWR" in src:
            raise SystemExit("XL_RADAR_PWR still present")

    # Fix status snprintf if we doubled pca fields incorrectly
    # Look for consecutive pcaServo/pcaMotor true/false with mismatched format — leave for compile

    # Ensure cfgSnapSrv NVS loop exists for srv2..4
    if 'snprintf(key, sizeof(key), "srv%u"' not in src and 'nvs_set_u8(h, "srv0"' in src:
        src = re.sub(
            r'nvs_set_u8\(h, "srv0", clampU8\(cfgSnapSrv\[0\]\)\);\s*'
            r'nvs_set_u8\(h, "srv1", clampU8\(cfgSnapSrv\[1\]\)\);',
            'for (uint8_t si = 0; si < SERVO_COUNT; si++) {\n'
            '    char skey[8]; snprintf(skey, sizeof(skey), "srv%u", (unsigned)si);\n'
            "    nvs_set_u8(h, skey, clampU8(cfgSnapSrv[si]));\n"
            "  }",
            src,
            count=1,
        )
        print("nvs save srv loop")

    if re.search(r"\bpca\.", src):
        for i, line in enumerate(src.splitlines(), 1):
            if re.search(r"\bpca\.", line):
                print("leftover", i, line)
        raise SystemExit("pca. leftovers")

    MAIN.write_text(src, encoding="utf-8")
    print("OK", MAIN.stat().st_size)


if __name__ == "__main__":
    main()
