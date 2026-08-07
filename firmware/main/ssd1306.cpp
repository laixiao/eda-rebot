#include "ssd1306.h"
#include "board_i2c.h"
#include "font5x7.h"
#include "font_cjk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

void SSD1306::releaseDev() {
  if (dev_) {
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
  }
}

bool SSD1306::cmd(uint8_t c) {
  uint8_t buf[2] = {0x00, c};
  const bool ok = board_i2c_write(dev_, buf, 2, 300);
  if (!ok) ok_ = false;
  return ok;
}

bool SSD1306::data(const uint8_t *d, size_t n) {
  uint8_t chunk[17];
  chunk[0] = 0x40;
  while (n) {
    size_t m = n > 16 ? 16 : n;
    memcpy(chunk + 1, d, m);
    if (!board_i2c_write(dev_, chunk, m + 1, 300)) {
      ok_ = false;
      return false;
    }
    d += m;
    n -= m;
  }
  ok_ = true;
  return true;
}

int SSD1306::initSequence(uint8_t comPins) {
  static const uint8_t seq[] = {
      0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00,
      0xA1, 0xC8, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
  };
  int step = 0;
  for (uint8_t b : seq) {
    if (!cmd(b)) return step;
    step++;
  }
  if (!cmd(0xDA)) return step;
  step++;
  if (!cmd(comPins)) return step;
  step++;
  clear();
  if (!show()) return step;
  step++;
  if (!cmd(0xAF)) return step;
  return -1;
}

int SSD1306::beginEx(uint8_t addr, uint32_t scl_hz) {
  ok_ = false;
  releaseDev();
  addr_ = addr;
  scl_hz_ = scl_hz;
  if (!board_i2c_add_device(addr, &dev_, scl_hz)) return 0;
  vTaskDelay(pdMS_TO_TICKS(120));
  ok_ = true;
  int r = initSequence(0x12);
  if (r >= 0) r = initSequence(0x02);
  if (r >= 0) {
    ok_ = false;
    return r;
  }
  return -1;
}

bool SSD1306::begin(uint8_t addr, uint32_t scl_hz) { return beginEx(addr, scl_hz) < 0; }

void SSD1306::clear() { memset(buf_, 0, sizeof(buf_)); }
void SSD1306::fill() { memset(buf_, 0xFF, sizeof(buf_)); }

void SSD1306::putAscii(uint8_t col, uint8_t page, char c) {
  if (page > 7 || col > 122) return;
  const uint8_t uc = (uint8_t)c;
  if (uc < 32 || uc > 127) c = '?';
  const uint8_t *glyph = FONT5X7 + ((uint8_t)c - 32) * 5;
  // Draw into top page; leave bottom of 16px cell empty for alignment with CJK
  size_t base = (size_t)page * 128 + col;
  for (uint8_t i = 0; i < 5; i++) buf_[base + i] = glyph[i];
  buf_[base + 5] = 0;
  if (page + 1 <= 7) {
    size_t base2 = (size_t)(page + 1) * 128 + col;
    for (uint8_t i = 0; i < 6; i++) buf_[base2 + i] = 0;
  }
}

void SSD1306::putCjk(uint8_t col, uint8_t page, const uint8_t glyph[32]) {
  if (page > 6 || col > 112) return;
  size_t top = (size_t)page * 128 + col;
  size_t bot = (size_t)(page + 1) * 128 + col;
  for (uint8_t x = 0; x < 16; x++) {
    buf_[top + x] = glyph[x * 2];
    buf_[bot + x] = glyph[x * 2 + 1];
  }
}

static const char *nextUtf8(const char *text, uint32_t &cp) {
  const uint8_t b0 = (uint8_t)*text;
  if (b0 < 0x80) {
    cp = b0;
    return text + 1;
  }
  if ((b0 & 0xE0) == 0xC0 && text[1]) {
    cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint8_t)text[1] & 0x3F);
    return text + 2;
  }
  if ((b0 & 0xF0) == 0xE0 && text[1] && text[2]) {
    cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)((uint8_t)text[1] & 0x3F) << 6) |
         ((uint8_t)text[2] & 0x3F);
    return text + 3;
  }
  if ((b0 & 0xF8) == 0xF0 && text[1] && text[2] && text[3]) {
    cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)((uint8_t)text[1] & 0x3F) << 12) |
         ((uint32_t)((uint8_t)text[2] & 0x3F) << 6) | ((uint8_t)text[3] & 0x3F);
    return text + 4;
  }
  cp = '?';
  return text + 1;
}

void SSD1306::drawText(uint8_t col, uint8_t page, const char *text) {
  while (*text && col < 128) {
    uint32_t cp = 0;
    text = nextUtf8(text, cp);
    if (cp >= 0x80) {
      const uint8_t *g = font_cjk_lookup(cp);
      if (!g || col + 16 > 128) {
        if (col + 6 > 128) break;
        putAscii(col, page, '?');
        col += 6;
      } else {
        putCjk(col, page, g);
        col += 16;
      }
    } else {
      if (col + 6 > 128) break;
      putAscii(col, page, (char)cp);
      col += 6;
    }
  }
}

bool SSD1306::printfLines(const char *l0, const char *l1, const char *l2, const char *l3) {
  clear();
  drawText(0, 0, l0);
  drawText(0, 2, l1);
  drawText(0, 4, l2);
  drawText(0, 6, l3);
  return show();
}

bool SSD1306::show() {
  if (!ok_) return false;
  return cmd(0x21) && cmd(0) && cmd(127) && cmd(0x22) && cmd(0) && cmd(7) &&
         data(buf_, sizeof(buf_));
}
