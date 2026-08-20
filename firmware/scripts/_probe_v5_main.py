# -*- coding: utf-8 -*-
"""Patch FAN main.cpp for v5 hybrid board."""
from pathlib import Path
import re
import subprocess

root = Path(r"c:\Users\Administrator\Desktop\eda\eda-rebot")
main_path = root / "firmware/main/main.cpp"
src = main_path.read_text(encoding="utf-8")

v5 = subprocess.check_output(
    ["git", "show", "56479b0:firmware/main/main.cpp"], cwd=root
).decode("utf-8", errors="replace")

handlers = re.findall(r"^static esp_err_t (\w+)\(", v5, re.M)
print("v5 handlers:", handlers)

# list symbols for encoders
for name in [
    "enc1Count",
    "enc2Count",
    "enc3Count",
    "enc4Count",
    "pcaServo",
    "pcaMotor",
    "ST7796",
    "XPT2046",
    "camera_set",
    "flagStby",
]:
    print(name, "YES" if name in v5 else "no")
