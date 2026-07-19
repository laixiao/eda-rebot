#!/usr/bin/env python3
"""局域网调用机器人头部板 REST API。用法: python robot_api.py 192.168.x.x"""

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
                ctype = resp.headers.get("Content-Type", "")
                raw = resp.read()
                if "image/" in ctype:
                    return {"ok": True, "content_type": ctype, "bytes": len(raw)}
                return json.loads(raw.decode("utf-8"))
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

    def pwm(self, on: bool = True) -> dict:
        return self._call("/api/pwm", {"on": on}, method="POST")

    def stby(self, on: bool = True) -> dict:
        return self._call("/api/stby", {"on": on}, method="POST")

    def amp(self, on: bool = True) -> dict:
        return self._call("/api/amp", {"on": on}, method="POST")

    def servo(self, servo_id: int, angle: int) -> dict:
        return self._call("/api/servo", {"id": servo_id, "angle": angle}, method="POST")

    def servos(self, angles: list[int]) -> dict:
        return self._call("/api/servos", {"angles": angles}, method="POST")

    def motor(self, motor_id: int, direction: int, duty: int = 40) -> dict:
        return self._call(
            "/api/motor", {"id": motor_id, "dir": direction, "duty": duty}, method="POST"
        )

    def motor_stop_all(self) -> dict:
        return self._call("/api/motor/stop_all", method="POST")

    def encoders(self) -> dict:
        return self._call("/api/encoders")

    def mic(self) -> dict:
        return self._call("/api/mic")

    def beep(self, ms: int = 250) -> dict:
        return self._call("/api/beep", {"ms": ms}, method="POST")

    def oled(self, text: str = "", cmd: str = "text") -> dict:
        return self._call("/api/oled", {"cmd": cmd, "text": text}, method="POST")

    def led(self, led_id: int, duty: int = 100) -> dict:
        return self._call("/api/led", {"id": led_id, "duty": duty}, method="POST")

    def camera(self, on: bool | None = None) -> dict:
        if on is None:
            return self._call("/api/camera")
        return self._call("/api/camera", {"on": on}, method="POST")

    def camera_capture(self) -> dict:
        return self._call("/api/camera/capture")

    def lcd(
        self,
        cmd: str = "status",
        color: str | None = None,
        text: str | None = None,
        x: int | None = None,
        y: int | None = None,
        scale: int | None = None,
        bg: str | None = None,
        clear: bool | None = None,
        w: int | None = None,
        h: int | None = None,
    ) -> dict:
        params: dict[str, Any] = {"cmd": cmd}
        if color is not None:
            params["color"] = color
        if text is not None:
            params["text"] = text
        if x is not None:
            params["x"] = x
        if y is not None:
            params["y"] = y
        if scale is not None:
            params["scale"] = scale
        if bg is not None:
            params["bg"] = bg
        if clear is not None:
            params["clear"] = clear
        if w is not None:
            params["w"] = w
        if h is not None:
            params["h"] = h
        return self._call("/api/lcd", params, method="POST")

    def touch(self) -> dict:
        return self._call("/api/touch")

    def ota_info(self) -> dict:
        return self._call("/api/ota")

    def ota_flash(self, bin_path: str, timeout: float = 180.0) -> dict:
        """POST raw firmware .bin to /api/ota (board reboots on success)."""
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
            # reboot drops TCP — often means success
            return {"ok": True, "rebooting": True, "note": str(e.reason)}


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
    bot = RobotApi(host)
    print("status:", bot.status())
    print("catalog:", bot.api())
    print("oled:", bot.oled("LAN OK"))
    print("encoders:", bot.encoders())
    print("camera:", bot.camera())
    print("lcd:", bot.lcd("status"))
    print("touch:", bot.touch())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
