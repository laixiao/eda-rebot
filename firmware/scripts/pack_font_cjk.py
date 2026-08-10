#!/usr/bin/env python3
"""Pack GB2312 level-1 (+ UI extras) into firmware/main/font_cjk.bin (CJK1 format).

Format (see font_cjk.cpp):
  magic "CJK1" | u32le count | count * u16le codepoint (sorted)
  | count * 32-byte glyphs (SSD1306 column-major 16x16)
"""

from __future__ import annotations

import gzip
import io
import struct
import sys
import urllib.request
from pathlib import Path

UNIFONT_URL = (
    "https://unifoundry.com/pub/unifont/unifont-15.1.05/"
    "font-builds/unifont-15.1.05.hex.gz"
)

# Extra UI strings used by firmware OLED / web notes
EXTRA_CHARS = "你好机器人汉字字库就绪等待打开浏览器检查热点已关机深度睡眠急停风扇雷达采集供电"


def gb2312_level1() -> list[int]:
    cps: list[int] = []
    for qu in range(16, 56):  # 一级汉字区
        for wei in range(1, 95):
            try:
                ch = bytes((0xA0 + qu, 0xA0 + wei)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            cp = ord(ch)
            if cp >= 0x80:
                cps.append(cp)
    return cps


def row_major_to_ssd1306(rows: list[int]) -> bytes:
    """rows[y] = 16-bit row, MSB = left. Out: 16 cols * (top8, bot8)."""
    out = bytearray(32)
    for x in range(16):
        top = 0
        bot = 0
        mask = 0x8000 >> x
        for y in range(8):
            if rows[y] & mask:
                top |= 1 << y
            if rows[y + 8] & mask:
                bot |= 1 << y
        out[x * 2] = top
        out[x * 2 + 1] = bot
    return bytes(out)


def parse_unifont_hex(text: str) -> dict[int, bytes]:
    glyphs: dict[int, bytes] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or ":" not in line:
            continue
        code_s, hex_s = line.split(":", 1)
        try:
            cp = int(code_s, 16)
        except ValueError:
            continue
        hex_s = hex_s.strip()
        # 8x16 (32 hex) or 16x16 (64 hex)
        if len(hex_s) == 32:
            # double-width pad: each row nibble-pair -> 16-bit with left half
            rows = []
            for i in range(0, 32, 2):
                b = int(hex_s[i : i + 2], 16)
                rows.append(b << 8)
            glyphs[cp] = row_major_to_ssd1306(rows)
        elif len(hex_s) == 64:
            rows = [int(hex_s[i : i + 4], 16) for i in range(0, 64, 4)]
            glyphs[cp] = row_major_to_ssd1306(rows)
    return glyphs


def load_unifont(cache: Path) -> dict[int, bytes]:
    if cache.exists() and cache.stat().st_size > 100_000:
        raw = cache.read_bytes()
        if cache.suffix == ".gz" or raw[:2] == b"\x1f\x8b":
            text = gzip.decompress(raw).decode("ascii", errors="ignore")
        else:
            text = raw.decode("ascii", errors="ignore")
        return parse_unifont_hex(text)

    print(f"Downloading {UNIFONT_URL} ...", file=sys.stderr)
    with urllib.request.urlopen(UNIFONT_URL, timeout=120) as resp:
        data = resp.read()
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(data)
    text = gzip.decompress(data).decode("ascii", errors="ignore")
    return parse_unifont_hex(text)


def pack(codepoints: list[int], table: dict[int, bytes]) -> bytes:
    pairs: list[tuple[int, bytes]] = []
    missing = 0
    blank = bytes(32)
    for cp in sorted(set(codepoints)):
        g = table.get(cp)
        if g is None:
            missing += 1
            g = blank
        pairs.append((cp, g))
    n = len(pairs)
    buf = io.BytesIO()
    buf.write(b"CJK1")
    buf.write(struct.pack("<I", n))
    for cp, _ in pairs:
        buf.write(struct.pack("<H", cp))
    for _, g in pairs:
        buf.write(g)
    print(f"packed {n} glyphs, missing={missing}, size={buf.tell()} bytes", file=sys.stderr)
    return buf.getvalue()


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    out = root / "main" / "font_cjk.bin"
    cache = root / "scripts" / ".cache" / "unifont-15.1.05.hex.gz"

    cps = gb2312_level1()
    for ch in EXTRA_CHARS:
        cps.append(ord(ch))
    # common CJK punctuation
    for cp in range(0x3000, 0x303F):
        cps.append(cp)
    for cp in (0xFF0C, 0xFF01, 0xFF1F, 0xFF1A, 0xFF1B, 0x3001, 0x3002):
        cps.append(cp)

    table = load_unifont(cache)
    data = pack(cps, table)
    out.write_bytes(data)
    print(f"wrote {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
