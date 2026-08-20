# -*- coding: utf-8 -*-
import re
from pathlib import Path
src = Path("firmware/main/main.cpp").read_text(encoding="utf-8")
for n in ["setStby","motorDrive","motorStopAll","handleCamera","handleLcd","handleTouch","handleMotor","encoders_init","onEnc2","updateEnc34"]:
    print(n, bool(re.search(rf"^static[^\n]*\b{n}\s*\(", src, re.M)))
print("api_camera_reg", 'registerUri(server, "/api/camera"' in src)
print("api_motor_reg", 'registerUri(server, "/api/motor"' in src)
print("XL_RADAR_PWR", "XL_RADAR_PWR" in src)
print("updateEnc34_call", "updateEnc34();" in src)
print("streamSlot_boot", "streamSlot = xSemaphoreCreateCounting" in src)
print("pca_leftover", len(re.findall(r"\bpca\.", src)))
print("FW", "3.7.0-v5" in src)
