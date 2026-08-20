#include "st7796.h"
#include "board_config.h"
#include "board_spi.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const uint32_t SPI_HZ = 40000000;

void ST7796::cs(bool level) { if (!xl_ || !xl_->setPin(XL_LCD_CS, level)) io_ok_ = false; }
void ST7796::dc(bool level) { if (!xl_ || !xl_->setPin(XL_LCD_DC, level)) io_ok_ = false; }
void ST7796::rst(bool level) { if (!xl_ || !xl_->setPin(XL_LCD_RST, level)) io_ok_ = false; }

void ST7796::writeCmd(uint8_t cmd) {
  dc(false);
  cs(false);
  if (!board_spi_tx(&cmd, 1, SPI_HZ)) io_ok_ = false;
  cs(true);
}

void ST7796::writeData(uint8_t data) {
  dc(true);
  cs(false);
  if (!board_spi_tx(&data, 1, SPI_HZ)) io_ok_ = false;
  cs(true);
}

void ST7796::writeData16(uint16_t data) {
  dc(true);
  cs(false);
  if (!board_spi_write16(data, SPI_HZ)) io_ok_ = false;
  cs(true);
}

void ST7796::hardReset() {
  cs(true);
  dc(true);
  rst(false);
  vTaskDelay(pdMS_TO_TICKS(20));
  rst(true);
  vTaskDelay(pdMS_TO_TICKS(120));
}

void ST7796::backlight(bool on) {
  if (!xl_ || !xl_->setPin(XL_LCD_LED, on)) {
    io_ok_ = false;
    ok_ = false;
  }
}

void ST7796::setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  writeCmd(0x2A);
  writeData(x0 >> 8);
  writeData(x0 & 0xFF);
  writeData(x1 >> 8);
  writeData(x1 & 0xFF);
  writeCmd(0x2B);
  writeData(y0 >> 8);
  writeData(y0 & 0xFF);
  writeData(y1 >> 8);
  writeData(y1 & 0xFF);
  writeCmd(0x2C);
}

bool ST7796::begin(XL9555 &xl) {
  xl_ = &xl;
  ok_ = false;
  io_ok_ = xl.present();
  if (!io_ok_ || !board_spi_init()) return false;
  hardReset();

  writeCmd(0x01);
  vTaskDelay(pdMS_TO_TICKS(150));
  writeCmd(0x11);
  vTaskDelay(pdMS_TO_TICKS(120));

  writeCmd(0xF0); writeData(0xC3);
  writeCmd(0xF0); writeData(0x96);
  writeCmd(0x36); writeData(0x48);
  writeCmd(0x3A); writeData(0x55);
  writeCmd(0xB4); writeData(0x01);
  writeCmd(0xB7); writeData(0xC6);
  writeCmd(0xC0); writeData(0x80); writeData(0x45);
  writeCmd(0xC1); writeData(0x13);
  writeCmd(0xC2); writeData(0xA7);
  writeCmd(0xC5); writeData(0x0A);
  writeCmd(0xE8);
  writeData(0x40); writeData(0x8A); writeData(0x00); writeData(0x00);
  writeData(0x29); writeData(0x19); writeData(0xA5); writeData(0x33);

  writeCmd(0xE0);
  const uint8_t p0[] = {0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47,
                        0x17, 0x13, 0x13, 0x2B, 0x31};
  for (uint8_t v : p0) writeData(v);
  writeCmd(0xE1);
  const uint8_t p1[] = {0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x27, 0x2B, 0x33, 0x47,
                        0x38, 0x15, 0x16, 0x2C, 0x32};
  for (uint8_t v : p1) writeData(v);

  writeCmd(0xF0); writeData(0x3C);
  writeCmd(0xF0); writeData(0x69);
  writeCmd(0x29);
  vTaskDelay(pdMS_TO_TICKS(20));

  w_ = LCD_WIDTH;
  h_ = LCD_HEIGHT;
  rot_ = 0;
  ok_ = true;
  backlight(true);
  fillScreen(0x0000);
  ok_ = io_ok_;
  return ok_;
}

void ST7796::setRotation(uint8_t r) {
  rot_ = r & 3;
  uint8_t madctl = 0x08;
  switch (rot_) {
    case 0: madctl |= 0x40; w_ = LCD_WIDTH; h_ = LCD_HEIGHT; break;
    case 1: madctl |= 0x20; w_ = LCD_HEIGHT; h_ = LCD_WIDTH; break;
    case 2: madctl |= 0x80; w_ = LCD_WIDTH; h_ = LCD_HEIGHT; break;
    case 3: madctl |= 0xE0; w_ = LCD_HEIGHT; h_ = LCD_WIDTH; break;
  }
  writeCmd(0x36);
  writeData(madctl);
  if (!io_ok_) ok_ = false;
}

void ST7796::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (!ok_ || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > w_) w = w_ - x;
  if (y + h > h_) h = h_ - y;
  if (w <= 0 || h <= 0) return;

  setWindow((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
  dc(true);
  cs(false);
  const uint32_t n = (uint32_t)w * (uint32_t)h;
  uint8_t pixels[512];
  for (size_t i = 0; i < sizeof(pixels); i += 2) {
    pixels[i] = (uint8_t)(color >> 8);
    pixels[i + 1] = (uint8_t)color;
  }
  uint32_t left = n;
  while (left) {
    const uint32_t chunk = left > 256 ? 256 : left;
    if (!board_spi_tx(pixels, chunk * 2, SPI_HZ)) {
      io_ok_ = false;
      break;
    }
    left -= chunk;
  }
  cs(true);
  if (!io_ok_) ok_ = false;
}

void ST7796::fillScreen(uint16_t color) { fillRect(0, 0, w_, h_, color); }

void ST7796::drawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
  if (!ok_ || scale == 0) return;
  if (scale > 6) scale = 6;
  if (c < 32 || c > 126) c = '?';
  const uint8_t *glyph = FONT5X7 + (c - 32) * 5;
  const int16_t cw = (int16_t)(6 * scale);
  const int16_t ch = (int16_t)(8 * scale);
  if (x >= w_ || y >= h_ || x + cw <= 0 || y + ch <= 0) return;
  if (x < 0 || y < 0 || x + cw > w_ || y + ch > h_) {
    // clipped path: slow but correct
    for (uint8_t col = 0; col < 6; col++) {
      const uint8_t bits = (col < 5) ? glyph[col] : 0;
      for (uint8_t row = 0; row < 8; row++) {
        const bool on = (row < 7) && (bits & (1u << row));
        fillRect(x + (int16_t)col * scale, y + (int16_t)row * scale, scale, scale, on ? fg : bg);
      }
    }
    return;
  }

  setWindow((uint16_t)x, (uint16_t)y, (uint16_t)(x + cw - 1), (uint16_t)(y + ch - 1));
  dc(true);
  cs(false);
  for (uint8_t row = 0; row < 8; row++) {
    for (uint8_t sy = 0; sy < scale; sy++) {
      for (uint8_t col = 0; col < 6; col++) {
        const uint8_t bits = (col < 5) ? glyph[col] : 0;
        const bool on = (row < 7) && (bits & (1u << row));
        const uint16_t color = on ? fg : bg;
        for (uint8_t sx = 0; sx < scale; sx++) {
          if (!board_spi_write16(color, SPI_HZ)) {
            io_ok_ = false;
            cs(true);
            ok_ = false;
            return;
          }
        }
      }
    }
  }
  cs(true);
  if (!io_ok_) ok_ = false;
}

void ST7796::drawText(int16_t x, int16_t y, const char *text, uint16_t fg, uint16_t bg,
                      uint8_t scale) {
  if (!ok_ || !text || scale == 0) return;
  if (scale > 6) scale = 6;
  const int16_t x0 = x;
  const int16_t stepX = (int16_t)(6 * scale);
  const int16_t stepY = (int16_t)(8 * scale);
  while (*text) {
    if (*text == '\n') {
      x = x0;
      y += stepY;
      ++text;
      continue;
    }
    if (x + stepX > w_) {
      x = x0;
      y += stepY;
    }
    if (y >= h_) break;
    drawChar(x, y, *text++, fg, bg, scale);
    x += stepX;
  }
}
