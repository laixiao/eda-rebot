#!/usr/bin/env python3
"""局域网调用机器人头部板 REST API 示例。用法: python robot_api.py 192.168.x.x"""

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
        return self._call("/api/estop")

    def pwm(self, on: bool = True) -> dict:
        return self._call("/api/pwm", {"on": on})

    def stby(self, on: bool = True) -> dict:
        return self._call("/api/stby", {"on": on})

    def amp(self, on: bool = True) -> dict:
        return self._call("/api/amp", {"on": on})

    def servo(self, servo_id: int, angle: int) -> dict:
        return self._call("/api/servo", {"id": servo_id, "angle": angle})

    def servos(self, angles: list[int]) -> dict:
        return self._call("/api/servos", {"angles": angles}, method="POST")

    def motor(self, motor_id: int, direction: int, duty: int = 40) -> dict:
        return self._call("/api/motor", {"id": motor_id, "dir": direction, "duty": duty})

    def motor_stop_all(self) -> dict:
        return self._call("/api/motor/stop_all")

    def encoders(self) -> dict:
        return self._call("/api/encoders")

    def mic(self) -> dict:
        return self._call("/api/mic")

    def beep(self, ms: int = 250) -> dict:
        return self._call("/api/beep", {"ms": ms})

    def oled(self, text: str = "", cmd: str = "text") -> dict:
        return self._call("/api/oled", {"cmd": cmd, "text": text})

    def led(self, led_id: int, duty: int = 100) -> dict:
        return self._call("/api/led", {"id": led_id, "duty": duty})

    def camera(self) -> dict:
        return self._call("/api/camera")


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
    bot = RobotApi(host)
    print("status:", bot.status())
    print("catalog:", bot.api())
    # 安全演示：不开电机/舵机，只读与 OLED
    print("oled:", bot.oled("LAN OK"))
    print("encoders:", bot.encoders())
    print("camera:", bot.camera())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
