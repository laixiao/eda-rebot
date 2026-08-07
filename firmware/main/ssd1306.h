#pragma once

#include "driver/i2c_master.h"
#include <stdint.h>
#include <stddef.h>

class SSD1306 {
 public:
  bool begin(uint8_t addr, uint32_t scl_hz = 100000);
  int beginEx(uint8_t addr, uint32_t scl_hz = 100000);
  bool present() const { return ok_; }
  uint8_t addr() const { return addr_; }
  uint32_t sclHz() const { return scl_hz_; }
  void clear();
  void fill();
  /** UTF-8 text; ASCII 6px, CJK 16x16. page is top page (uses page and page+1 for CJK). */
  void drawText(uint8_t col, uint8_t page, const char *text);
  bool show();
  bool printfLines(const char *l0, const char *l1 = "", const char *l2 = "",
                   const char *l3 = "");

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  bool ok_ = false;
  uint8_t addr_ = 0;
  uint32_t scl_hz_ = 100000;
  uint8_t buf_[1024];

  void releaseDev();
  bool cmd(uint8_t c);
  bool data(const uint8_t *d, size_t n);
  void putAscii(uint8_t col, uint8_t page, char c);
  void putCjk(uint8_t col, uint8_t page, const uint8_t glyph[32]);
  int initSequence(uint8_t comPins);
};
