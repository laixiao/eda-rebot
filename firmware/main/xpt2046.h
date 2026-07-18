#pragma once

#include "xl9555.h"
#include <stdint.h>

class XPT2046 {
 public:
  bool begin(XL9555 &xl);
  bool touched();
  bool read(uint16_t &x, uint16_t &y, uint16_t &z);

 private:
  XL9555 *xl_ = nullptr;
  uint16_t readRaw(uint8_t cmd);
};
