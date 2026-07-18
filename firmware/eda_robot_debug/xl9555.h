#pragma once
#include <Arduino.h>
#include <Wire.h>

// XL9555 / PCA9555 兼容寄存器
class XL9555 {
 public:
  bool begin(TwoWire &wire, uint8_t addr);
  bool present() const { return ok_; }

  bool writePort(uint8_t port, uint8_t value);
  bool readPort(uint8_t port, uint8_t &value);
  bool writeConfig(uint8_t port, uint8_t config);
  bool setPin(uint8_t pin, bool level);  // pin 0-15
  bool getPin(uint8_t pin, bool &level);

  // 安全默认：STBY=0, OE#=1(禁PWM), Amp=0(关)
  bool applySafeDefaults();

 private:
  TwoWire *wire_ = nullptr;
  uint8_t addr_ = 0;
  bool ok_ = false;
  uint8_t out_[2] = {0x40, 0x00};  // OE high

  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t &val);
};
