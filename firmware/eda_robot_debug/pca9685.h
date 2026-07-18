#pragma once
#include <Arduino.h>
#include <Wire.h>

class PCA9685 {
 public:
  bool begin(TwoWire &wire, uint8_t addr, float freqHz = 50.0f);
  bool present() const { return ok_; }
  bool setPWMFreq(float freqHz);
  bool setPWM(uint8_t channel, uint16_t on, uint16_t off);
  bool setDuty(uint8_t channel, uint16_t duty12);  // 0..4095
  bool setPulseUs(uint8_t channel, uint16_t us);
  bool allOff();

 private:
  TwoWire *wire_ = nullptr;
  uint8_t addr_ = 0;
  bool ok_ = false;
  float freq_ = 50.0f;

  bool write8(uint8_t reg, uint8_t val);
  bool read8(uint8_t reg, uint8_t &val);
};
