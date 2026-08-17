#!/usr/bin/env python3
"""局域网调用 v6-1 板 REST API。用法: python robot_api.py 192.168.x.x"""

from __future__ import annotations

import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any


class RobotApi:
    def __init__(self, host: str, timeout: float = 5.0):
        if not host.startswith("http"):
            host = f"http://{host}"
        self.base = host.rstrip("/")
        self.timeout = timeout

    def _call(self, path: str, params: dict[str, Any] | None = None, method: str = "GET") -> dict:
        url = self.base + path
        data = None
        headers = {"Accept": "application/json"}
        if params:
            if method.upper() == "GET":
                url += "?" + urllib.parse.urlencode(params)
            else:
                data = json.dumps(params).encode("utf-8")
                headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method=method.upper())
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            try:
                return json.loads(body)
            except json.JSONDecodeError:
                return {"ok": False, "error": body, "http": e.code}

    def api(self) -> dict:
        return self._call("/api")

    def status(self) -> dict:
        return self._call("/api/status")

    def estop(self, on: bool = True) -> dict:
        """关闭所有外设 (on=True) 或按快照恢复 (on=False)。"""
        return self._call("/api/estop", {"on": on}, method="POST")

    def shutdown(self) -> dict:
        return self._call("/api/shutdown", method="POST")

    def pwm(self, on: bool = True) -> dict:
        return self._call("/api/pwm", {"on": on}, method="POST")

    def amp(self, on: bool | None = None, volume: int | None = None) -> dict:
        """功放开关 / 数字音量 0..100。仅传 volume 时不改开关。"""
        body: dict[str, Any] = {}
        if on is not None:
            body["on"] = on
        if volume is not None:
            body["volume"] = max(0, min(100, int(volume)))
        if not body:
            body["on"] = True
        return self._call("/api/amp", body, method="POST")

    def servo(self, servo_id: int, angle: int) -> dict:
        return self._call("/api/servo", {"id": servo_id, "angle": angle}, method="POST")

    def servos(self, angles: list[int]) -> dict:
        return self._call("/api/servos", {"angles": angles}, method="POST")

    def led(self, led_id: int, duty: int = 100) -> dict:
        """id 0=LED_1, 1=LED_2, 2=LED_ALL（点亮 1/2 时需同时开 LED_ALL）"""
        return self._call("/api/led", {"id": led_id, "duty": duty}, method="POST")

    def fan(self) -> dict:
        return self._call("/api/fan")

    def fan_auto(self, on: bool = True) -> dict:
        """雷达自动控 LED_1（风扇）；默认固件侧为关"""
        return self._call("/api/fan", {"auto": on}, method="POST")

    def fan_gesture(self, on: bool = True) -> dict:
        """近距手掌停留 2s 循环切档：关→50%→100%；可与 fan_auto 同时开"""
        return self._call("/api/fan", {"gesture": on}, method="POST")

    def fan_power(self, on: bool = True) -> dict:
        """手动开/关风扇（恢复上次强度；同时关闭雷达联动）"""
        return self._call("/api/fan", {"power": on}, method="POST")

    def voice(self) -> dict:
        """板端语音：伪唤醒「你好爱妃」→「开风扇」「关风扇」"""
        return self._call("/api/voice")

    def mic(self) -> dict:
        return self._call("/api/mic")

    def rec(self, on: bool | None = None) -> dict:
        """GET 状态；POST on=True 开始 / False 停止录音。"""
        if on is None:
            return self._call("/api/rec")
        return self._call("/api/rec", {"on": on}, method="POST")

    def play(self) -> dict:
        """播放最近一次录音（板载扬声器）。"""
        return self._call("/api/play", method="POST")

    def play_upload(self, wav_path: str, timeout: float = 30.0) -> dict:
        """上传 WAV（PCM16 @16kHz）或原始 PCM16LE 并播放。"""
        import pathlib

        data = pathlib.Path(wav_path).read_bytes()
        url = self.base + "/api/play/upload"
        req = urllib.request.Request(
            url,
            data=data,
            headers={"Content-Type": "audio/wav", "Content-Length": str(len(data))},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            try:
                return json.loads(body)
            except json.JSONDecodeError:
                return {"ok": False, "error": body, "http": e.code}

    def beep(self, ms: int = 250, volume: int | None = None) -> dict:
        body: dict[str, Any] = {"ms": ms}
        if volume is not None:
            body["volume"] = max(0, min(100, int(volume)))
        return self._call("/api/beep", body, method="POST")

    def oled(self, text: str = "", cmd: str = "text") -> dict:
        return self._call("/api/oled", {"cmd": cmd, "text": text}, method="POST")

    def radar(self, live: bool = False) -> dict:
        return self._call("/api/radar/live" if live else "/api/radar")

    def radar_power(self, on: bool = True) -> dict:
        return self._call("/api/radar", {"power": on}, method="POST")

    def radar_schedule(self, enable: bool | None = None, on_at: str | None = None,
                       off_at: str | None = None) -> dict:
        """Daily HH:MM schedule (requires SNTP). Empty on_at/off_at clears that side."""
        body: dict[str, Any] = {}
        if enable is not None:
            body["scheduleEnable"] = bool(enable)
        if on_at is not None:
            body["scheduleOn"] = on_at
        if off_at is not None:
            body["scheduleOff"] = off_at
        return self._call("/api/radar", body, method="POST")

    def radar_enable(self, on: bool = True) -> dict:
        """Deprecated: acquisition follows power. Maps to radar_power()."""
        return self.radar_power(on)

    def ota_info(self) -> dict:
        return self._call("/api/ota")

    def ota_flash(self, bin_path: str, timeout: float = 180.0) -> dict:
        import pathlib

        data = pathlib.Path(bin_path).read_bytes()
        url = self.base + "/api/ota"
        req = urllib.request.Request(
            url,
            data=data,
            headers={
                "Content-Type": "application/octet-stream",
                "Content-Length": str(len(data)),
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="replace")
            try:
                return json.loads(body)
            except json.JSONDecodeError:
                return {"ok": False, "error": body, "http": e.code}
        except urllib.error.URLError as e:
            return {"ok": True, "rebooting": True, "note": str(e.reason)}


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.3.215"
    bot = RobotApi(host)
    print("status:", bot.status())
    print("ota:", bot.ota_info())
    print("radar:", bot.radar())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
