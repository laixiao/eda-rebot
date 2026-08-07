#pragma once

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>

class XL9555 {
 public:
  bool begin(uint8_t addr);
  bool present() const { return ok_; }

  bool writePort(uint8_t port, uint8_t value);
  bool readPort(uint8_t port, uint8_t &value);
  bool writeConfig(uint8_t port, uint8_t config);
  bool setPin(uint8_t pin, bool level);
  bool getPin(uint8_t pin, bool &level);
  bool applySafeDefaults();

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  uint8_t addr_ = 0;
  bool ok_ = false;
  uint8_t out_[2] = {0x42, 0x00}; // OE 高 + 雷达电源脚高（关）
  uint8_t cfg_[2] = {0xBD, 0xBF};
  SemaphoreHandle_t mutex_ = nullptr;

  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t &val);
  bool writeConfigUnlocked(uint8_t port, uint8_t config);
  bool writePortUnlocked(uint8_t port, uint8_t value);
};
