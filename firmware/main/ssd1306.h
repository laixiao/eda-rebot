#pragma once

#include "driver/i2c_master.h"
#include <stdint.h>
#include <stddef.h>

class SSD1306 {
 public:
  bool begin(uint8_t addr);
  bool present() const { return ok_; }
  void clear();
  void fill();
  void drawText(uint8_t col, uint8_t page, const char *text);
  bool show();
  bool printfLines(const char *l0, const char *l1 = "", const char *l2 = "",
                   const char *l3 = "");

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  bool ok_ = false;
  uint8_t buf_[1024];

  bool cmd(uint8_t c);
  bool data(const uint8_t *d, size_t n);
  void putChar(uint8_t col, uint8_t page, char c);
};
