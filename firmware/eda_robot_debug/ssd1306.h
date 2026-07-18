#pragma once
#include <Arduino.h>
#include <Wire.h>

class SSD1306 {
 public:
  bool begin(TwoWire &wire, uint8_t addr);
  bool present() const { return ok_; }
  void clear();
  void fill();
  void drawText(uint8_t col, uint8_t page, const char *text);
  void show();
  void printfLines(const char *l0, const char *l1 = "", const char *l2 = "",
                    const char *l3 = "");

 private:
  TwoWire *wire_ = nullptr;
  uint8_t addr_ = 0;
  bool ok_ = false;
  uint8_t buf_[1024];

  bool cmd(uint8_t c);
  bool cmd2(uint8_t c0, uint8_t c1);
  bool data(const uint8_t *d, size_t n);
  void putChar(uint8_t col, uint8_t page, char c);
};
