#pragma once

#include "driver/i2c_master.h"
#include <stdint.h>

class PCA9685 {
 public:
  bool begin(uint8_t addr, float freqHz = 50.0f);
  bool present() const { return ok_; }
  bool setPWMFreq(float freqHz);
  bool setPWM(uint8_t channel, uint16_t on, uint16_t off);
  bool setDuty(uint8_t channel, uint16_t duty12);
  bool setPulseUs(uint8_t channel, uint16_t us);
  bool allOff();

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  bool ok_ = false;
  float freq_ = 50.0f;

  bool write8(uint8_t reg, uint8_t val);
  bool read8(uint8_t reg, uint8_t &val);
};
