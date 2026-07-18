#include "st7796.h"
#include "board_config.h"

static const uint32_t SPI_HZ = 40000000;

void ST7796::cs(bool level) { xl_->setPin(XL_LCD_CS, level); }
void ST7796::dc(bool level) { xl_->setPin(XL_LCD_DC, level); }
void ST7796::rst(bool level) { xl_->setPin(XL_LCD_RST, level); }

void ST7796::writeCmd(uint8_t cmd) {
  dc(false);
  cs(false);
  spi_->beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  spi_->transfer(cmd);
  spi_->endTransaction();
  cs(true);
}

void ST7796::writeData(uint8_t data) {
  dc(true);
  cs(false);
  spi_->beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  spi_->transfer(data);
  spi_->endTransaction();
  cs(true);
}

void ST7796::writeData16(uint16_t data) {
  dc(true);
  cs(false);
  spi_->beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  spi_->transfer16(data);
  spi_->endTransaction();
  cs(true);
}

void ST7796::hardReset() {
  cs(true);
  dc(true);
  rst(false);
  delay(20);
  rst(true);
  delay(120);
}

void ST7796::backlight(bool on) { xl_->setPin(XL_LCD_LED, on); }

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

bool ST7796::begin(SPIClass &spi, XL9555 &xl) {
  spi_ = &spi;
  xl_ = &xl;
  hardReset();

  writeCmd(0x01);  // SWRESET
  delay(150);
  writeCmd(0x11);  // SLPOUT
  delay(120);

  writeCmd(0xF0);
  writeData(0xC3);
  writeCmd(0xF0);
  writeData(0x96);

  writeCmd(0x36);
  writeData(0x48);  // MADCTL：BGR + MX（竖屏常用）
  writeCmd(0x3A);
  writeData(0x55);  // 16-bit

  writeCmd(0xB4);
  writeData(0x01);
  writeCmd(0xB7);
  writeData(0xC6);

  writeCmd(0xC0);
  writeData(0x80);
  writeData(0x45);
  writeCmd(0xC1);
  writeData(0x13);
  writeCmd(0xC2);
  writeData(0xA7);
  writeCmd(0xC5);
  writeData(0x0A);

  writeCmd(0xE8);
  writeData(0x40);
  writeData(0x8A);
  writeData(0x00);
  writeData(0x00);
  writeData(0x29);
  writeData(0x19);
  writeData(0xA5);
  writeData(0x33);

  writeCmd(0xE0);
  const uint8_t p0[] = {0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47,
                        0x17, 0x13, 0x13, 0x2B, 0x31};
  for (uint8_t v : p0) writeData(v);
  writeCmd(0xE1);
  const uint8_t p1[] = {0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x27, 0x2B, 0x33, 0x47,
                        0x38, 0x15, 0x16, 0x2C, 0x32};
  for (uint8_t v : p1) writeData(v);

  writeCmd(0xF0);
  writeData(0x3C);
  writeCmd(0xF0);
  writeData(0x69);

  writeCmd(0x29);  // DISPON
  delay(20);

  w_ = LCD_WIDTH;
  h_ = LCD_HEIGHT;
  rot_ = 0;
  backlight(true);
  fillScreen(0x0000);
  ok_ = true;
  return true;
}

void ST7796::setRotation(uint8_t r) {
  rot_ = r & 3;
  uint8_t madctl = 0x08;  // BGR
  switch (rot_) {
    case 0:
      madctl |= 0x40;  // MX
      w_ = LCD_WIDTH;
      h_ = LCD_HEIGHT;
      break;
    case 1:
      madctl |= 0x20;  // MV
      w_ = LCD_HEIGHT;
      h_ = LCD_WIDTH;
      break;
    case 2:
      madctl |= 0x80;  // MY
      w_ = LCD_WIDTH;
      h_ = LCD_HEIGHT;
      break;
    case 3:
      madctl |= 0xE0;  // MX|MY|MV
      w_ = LCD_HEIGHT;
      h_ = LCD_WIDTH;
      break;
  }
  writeCmd(0x36);
  writeData(madctl);
}

void ST7796::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (!ok_ || w <= 0 || h <= 0) return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > w_) w = w_ - x;
  if (y + h > h_) h = h_ - y;
  if (w <= 0 || h <= 0) return;

  setWindow((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
  dc(true);
  cs(false);
  spi_->beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  const uint32_t n = (uint32_t)w * (uint32_t)h;
  for (uint32_t i = 0; i < n; i++) spi_->transfer16(color);
  spi_->endTransaction();
  cs(true);
}

void ST7796::fillScreen(uint16_t color) { fillRect(0, 0, w_, h_, color); }
