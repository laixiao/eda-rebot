#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "xl9555.h"

// ST7796 320x480，CS/DC/RST/LED 经 XL9555
class ST7796 {
 public:
  bool begin(SPIClass &spi, XL9555 &xl);
  bool present() const { return ok_; }
  void backlight(bool on);
  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void setRotation(uint8_t r);
  uint16_t width() const { return w_; }
  uint16_t height() const { return h_; }

 private:
  SPIClass *spi_ = nullptr;
  XL9555 *xl_ = nullptr;
  bool ok_ = false;
  uint16_t w_ = 320;
  uint16_t h_ = 480;
  uint8_t rot_ = 0;

  void cs(bool level);
  void dc(bool level);
  void rst(bool level);
  void writeCmd(uint8_t cmd);
  void writeData(uint8_t data);
  void writeData16(uint16_t data);
  void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
  void hardReset();
};
