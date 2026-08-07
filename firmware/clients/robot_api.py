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

    def estop(self) -> dict:
        return self._call("/api/estop", method="POST")

    def shutdown(self) -> dict:
        return self._call("/api/shutdown", method="POST")

    def pwm(self, on: bool = True) -> dict:
        return self._call("/api/pwm", {"on": on}, method="POST")

    def amp(self, on: bool = True) -> dict:
        return self._call("/api/amp", {"on": on}, method="POST")

    def servo(self, servo_id: int, angle: int) -> dict:
        return self._call("/api/servo", {"id": servo_id, "angle": angle}, method="POST")

    def servos(self, angles: list[int]) -> dict:
        return self._call("/api/servos", {"angles": angles}, method="POST")

    def led(self, led_id: int, duty: int = 100) -> dict:
        """id 0=LED_1, 1=LED_2, 2=LED_ALL（点亮 1/2 时需同时开 LED_ALL）"""
        return self._call("/api/led", {"id": led_id, "duty": duty}, method="POST")

    def mic(self) -> dict:
        return self._call("/api/mic")

    def beep(self, ms: int = 250) -> dict:
        return self._call("/api/beep", {"ms": ms}, method="POST")

    def oled(self, text: str = "", cmd: str = "text") -> dict:
        return self._call("/api/oled", {"cmd": cmd, "text": text}, method="POST")

    def radar(self, live: bool = False) -> dict:
        return self._call("/api/radar/live" if live else "/api/radar")

    def radar_power(self, on: bool = True) -> dict:
        return self._call("/api/radar", {"power": on}, method="POST")

    def radar_enable(self, on: bool = True) -> dict:
        return self._call("/api/radar", {"on": on}, method="POST")

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
