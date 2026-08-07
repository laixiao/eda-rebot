#include "font_cjk.h"
#include <string.h>

extern const uint8_t font_cjk_bin_start[] asm("_binary_font_cjk_bin_start");
extern const uint8_t font_cjk_bin_end[] asm("_binary_font_cjk_bin_end");

static const uint8_t *cjkBase() { return font_cjk_bin_start; }

static uint32_t cjkCount() {
  const uint8_t *p = cjkBase();
  if (!p || memcmp(p, "CJK1", 4) != 0) return 0;
  return (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
}

size_t font_cjk_count() { return cjkCount(); }

const uint8_t *font_cjk_lookup(uint32_t codepoint) {
  const uint32_t n = cjkCount();
  if (!n || codepoint > 0xFFFF) return nullptr;
  const uint8_t *base = cjkBase();
  const uint8_t *codes = base + 8;
  int lo = 0, hi = (int)n - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint16_t c = (uint16_t)codes[mid * 2] | ((uint16_t)codes[mid * 2 + 1] << 8);
    if (c == (uint16_t)codepoint) {
      return base + 8 + n * 2 + (size_t)mid * 32;
    }
    if (c < (uint16_t)codepoint) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}
