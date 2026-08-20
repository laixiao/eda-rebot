# -*- coding: utf-8 -*-
from pathlib import Path
import subprocess

ROOT = Path(r"c:\Users\Administrator\Desktop\eda\eda-rebot")

bf = subprocess.check_output(
    ["git", "show", "56479b0:.cursor/rules/board-facts.mdc"], cwd=ROOT
).decode("utf-8")
addon = """

## 固件并集（FAN-Circle → v5，FW ≥ 3.7.0-v5）

在 **v5 原理图/网表** 上保留摄像头、SPI 屏、电机、编码器、U23；并入 FAN 的语音/风扇/手势/雷达增强。

- 分区：`factory` 1MB 救援 + `ota_0` 14MB（语音模型内嵌）；首次改分区须串口 `scripts/flash_all.ps1`
- 雷达仍为 **飞线**：UART ENC1 IO9/10（RX=9 TX=10）；OUT→ENC3_A；**无** XL IO0_1 供电脚（`/api/radar` 的 `power` 为软开关）
- 探照灯/「风扇」PWM：U23 `SPOT_CH={0,1,2}`（LED_1/LED_2/LED_ALL）；控风扇打 LED_1×LED_ALL
- 舵机：U16 五路 id `0..4`；电机/STBY：U23 + XL IO0_5
- API 并集：FAN 的 `/api/fan` `/api/voice` `/api/rec` `/api/play` 雷达定时/忽略扇区 + v5 的 `/api/camera` `/stream` `/api/lcd` `/api/touch` `/api/motor` `/api/encoders` `/api/stby`
- 栈约定：bg 任务禁止同步播音，见 `firmware-bg-audio.mdc`
"""
(ROOT / ".cursor/rules/board-facts.mdc").write_text(bf.rstrip() + addon + "\n", encoding="utf-8")

ag = subprocess.check_output(["git", "show", "56479b0:AGENTS.md"], cwd=ROOT).decode("utf-8")
lines = []
for line in ag.splitlines():
    if line.startswith("| EasyEDA 工程 |"):
        lines.append("| EasyEDA 工程 | `AI通用机器人_v5` |")
    elif line.startswith("| Board / 原理图 / 图页 |"):
        lines.append("| Board / 原理图 / 图页 | `V1.0.0` / `EDA-Robot` / `P1` |")
    elif line.startswith("| 最新实时网表基线 |"):
        lines.append(
            "| 最新实时网表基线 | 2026-08-02：**105** 位号、**92** 网络（v5）；审查以 Bridge 实时网表为准 |"
        )
    elif line.startswith("| 固件 |"):
        lines.append(
            "| 固件 | `firmware/` ESP-IDF **FW 3.7.0-v5**（FAN 语音/风扇/雷达增强 + v5 全外设；factory+ota_0 大分区） |"
        )
    else:
        lines.append(line)
body = "\n".join(lines)
needle = "## U1 Strapping 启动脚"
note = (
    "**固件并集（相对纯 v5）**：另含 `/api/fan` `/api/voice` `/api/rec` `/api/play`、"
    "雷达定时/忽略扇区、救援 OTA；雷达 `power` 在 v5 为软开关（无 Q4）。\n\n"
)
if "固件并集（相对纯 v5）" not in body and needle in body:
    body = body.replace(needle, note + needle)
(ROOT / "AGENTS.md").write_text(body + "\n", encoding="utf-8")

# firmware README header tweak
readme = (ROOT / "firmware/README.md").read_text(encoding="utf-8")
readme = readme.replace("— v6-1", "— v5 + FAN")
readme = readme.replace("**AI通用机器人_v6-1**", "**AI通用机器人_v5**（固件含 FAN 语音/风扇能力）")
readme = readme.replace("已移除（v5）：摄像头、SPI 屏、电机、编码器、第二路 PCA9685。",
                        "本树保留 v5 外设（摄像头/SPI 屏/电机/编码器/U23），并含 FAN 语音与风扇联动。")
(ROOT / "firmware/README.md").write_text(readme, encoding="utf-8")

# firmware-build-ota / bg-audio: light board name if any
for rel in [".cursor/rules/firmware-build-ota.mdc", ".cursor/rules/firmware-bg-audio.mdc"]:
    p = ROOT / rel
    t = p.read_text(encoding="utf-8")
    # no board-specific rename required; leave as-is
    p.write_text(t, encoding="utf-8")

print("docs updated")
