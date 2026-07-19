#include "ssd1306.h"
#include "board_i2c.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

bool SSD1306::cmd(uint8_t c) {
  uint8_t buf[2] = {0x00, c};
  const bool ok = board_i2c_write(dev_, buf, 2);
  if (!ok) ok_ = false;
  return ok;
}

bool SSD1306::data(const uint8_t *d, size_t n) {
  uint8_t chunk[17];
  chunk[0] = 0x40;
  while (n) {
    size_t m = n > 16 ? 16 : n;
    memcpy(chunk + 1, d, m);
    if (!board_i2c_write(dev_, chunk, m + 1)) {
      ok_ = false;
      return false;
    }
    d += m;
    n -= m;
  }
  ok_ = true;
  return true;
}

bool SSD1306::begin(uint8_t addr) {
  ok_ = false;
  if (!board_i2c_add_device(addr, &dev_)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  ok_ = true;
  if (!cmd(0xAE) || !cmd(0xD5) || !cmd(0x80) || !cmd(0xA8) || !cmd(0x3F) ||
      !cmd(0xD3) || !cmd(0x00) || !cmd(0x40) || !cmd(0x8D) || !cmd(0x14) ||
      !cmd(0x20) || !cmd(0x00) || !cmd(0xA1) || !cmd(0xC8) || !cmd(0xDA) ||
      !cmd(0x12) || !cmd(0x81) || !cmd(0xCF) || !cmd(0xD9) || !cmd(0xF1) ||
      !cmd(0xDB) || !cmd(0x40) || !cmd(0xA4) || !cmd(0xA6)) return false;
  clear();
  if (!show() || !cmd(0xAF)) return false;
  return ok_;
}

void SSD1306::clear() { memset(buf_, 0, sizeof(buf_)); }
void SSD1306::fill() { memset(buf_, 0xFF, sizeof(buf_)); }

void SSD1306::putChar(uint8_t col, uint8_t page, char c) {
  if (page > 7 || col > 122) return;
  if (c < 32 || c > 127) c = '?';
  const uint8_t *glyph = FONT5X7 + (c - 32) * 5;
  size_t base = (size_t)page * 128 + col;
  for (uint8_t i = 0; i < 5; i++) buf_[base + i] = glyph[i];
  buf_[base + 5] = 0;
}

void SSD1306::drawText(uint8_t col, uint8_t page, const char *text) {
  while (*text && col < 122) {
    putChar(col, page, *text++);
    col += 6;
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
