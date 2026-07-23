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
  // 开漏：输出低 / 改输入高阻（靠外部上拉到 2.8V）；禁止对摄像头脚输出高
  bool driveLow(uint8_t pin);
  bool releasePin(uint8_t pin);
  bool applySafeDefaults();

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  uint8_t addr_ = 0;
  bool ok_ = false;
  uint8_t out_[2] = {0x40, 0x00};
  uint8_t cfg_[2] = {0xFF, 0xFF}; // 1=input, 0=output
  SemaphoreHandle_t mutex_ = nullptr;

  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t &val);
  bool writeConfigUnlocked(uint8_t port, uint8_t config);
  bool writePortUnlocked(uint8_t port, uint8_t value);
};
