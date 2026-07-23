#include "xl9555.h"
#include "board_config.h"
#include "board_i2c.h"

bool XL9555::writeReg(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return board_i2c_write(dev_, buf, 2);
}

bool XL9555::readReg(uint8_t reg, uint8_t &val) {
  return board_i2c_write_read(dev_, &reg, 1, &val, 1);
}

bool XL9555::writeConfigUnlocked(uint8_t port, uint8_t config) {
  if (port > 1) return false;
  if (!writeReg(0x06 + port, config)) return false;
  cfg_[port] = config;
  return true;
}

bool XL9555::writePortUnlocked(uint8_t port, uint8_t value) {
  if (port > 1) return false;
  if (!writeReg(0x02 + port, value)) return false;
  out_[port] = value;
  return true;
}

bool XL9555::begin(uint8_t addr) {
  addr_ = addr;
  ok_ = false;
  if (!mutex_) mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return false;
  if (!board_i2c_add_device(addr, &dev_)) {
    return false;
  }
  uint8_t dummy = 0;
  ok_ = readReg(0x00, dummy);
  if (!ok_) return false;
  ok_ = applySafeDefaults();
  return ok_;
}

bool XL9555::applySafeDefaults() {
  // Port0: ENC3/4=in；PWDN/RST=in(开漏释放)；STBY/OE=out
  // Port1: T_IRQ/IO1_7=in；其余 LCD/触摸/功放控制=out
  if (!writeConfig(0, 0x9F)) return false;
  if (!writeConfig(1, 0xA0)) return false;
  out_[0] = (1u << XL_OE); // OE 高=禁止 PWM；STBY 低；PWDN/RST 由上拉保持 2.8V
  out_[1] = (1u << (XL_T_CS - 8)) | (1u << (XL_LCD_RST - 8)) | (1u << (XL_LCD_CS - 8));
  if (!writePort(0, out_[0])) return false;
  if (!writePort(1, out_[1])) return false;
  return true;
}

bool XL9555::writePort(uint8_t port, uint8_t value) {
  if (port > 1 || !mutex_) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = writePortUnlocked(port, value);
  ok_ = ok;
  xSemaphoreGive(mutex_);
  return ok;
}

bool XL9555::readPort(uint8_t port, uint8_t &value) {
  if (port > 1 || !mutex_) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = readReg(0x00 + port, value);
  ok_ = ok;
  xSemaphoreGive(mutex_);
  return ok;
}

bool XL9555::writeConfig(uint8_t port, uint8_t config) {
  if (port > 1 || !mutex_) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = writeConfigUnlocked(port, config);
  ok_ = ok;
  xSemaphoreGive(mutex_);
  return ok;
}

bool XL9555::setPin(uint8_t pin, bool level) {
  if (pin > 15 || !mutex_) return false;
  uint8_t port = pin / 8;
  uint8_t bit = pin % 8;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (level) out_[port] |= (1u << bit);
  else out_[port] &= ~(1u << bit);
  // 推挽脚：确保为输出
  cfg_[port] &= ~(1u << bit);
  bool ok = writeConfigUnlocked(port, cfg_[port]);
  if (ok) ok = writePortUnlocked(port, out_[port]);
  ok_ = ok;
  xSemaphoreGive(mutex_);
  return ok;
}

bool XL9555::driveLow(uint8_t pin) {
  if (pin > 15 || !mutex_) return false;
  uint8_t port = pin / 8;
  uint8_t bit = pin % 8;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  out_[port] &= ~(1u << bit);
  cfg_[port] &= ~(1u << bit); // output
  bool ok = writePortUnlocked(port, out_[port]);
  if (ok) ok = writeConfigUnlocked(port, cfg_[port]);
  ok_ = ok;
  xSemaphoreGive(mutex_);
  return ok;
}

bool XL9555::releasePin(uint8_t pin) {
  if (pin > 15 || !mutex_) return false;
  uint8_t port = pin / 8;
  uint8_t bit = pin % 8;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  cfg_[port] |= (1u << bit); // input / Hi-Z
  const bool ok = writeConfigUnlocked(port, cfg_[port]);
  ok_ = ok;
  xSemaphoreGive(mutex_);
  return ok;
}

bool XL9555::getPin(uint8_t pin, bool &level) {
  if (pin > 15) return false;
  uint8_t port = pin / 8;
  uint8_t bit = pin % 8;
  uint8_t v = 0;
  if (!readPort(port, v)) return false;
  level = (v >> bit) & 1;
  return true;
}
