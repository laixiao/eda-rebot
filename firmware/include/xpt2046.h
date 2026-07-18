#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "xl9555.h"

class XPT2046 {
 public:
  bool begin(SPIClass &spi, XL9555 &xl);
  bool touched();
  bool read(uint16_t &x, uint16_t &y, uint16_t &z);

 private:
  SPIClass *spi_ = nullptr;
  XL9555 *xl_ = nullptr;
  uint16_t readRaw(uint8_t cmd);
};
