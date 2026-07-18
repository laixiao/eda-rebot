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

bool XL9555::begin(uint8_t addr) {
  addr_ = addr;
  if (!board_i2c_add_device(addr, &dev_)) {
    ok_ = false;
    return false;
  }
  uint8_t dummy = 0;
  ok_ = readReg(0x00, dummy);
  if (!ok_) return false;
  return applySafeDefaults();
}

bool XL9555::applySafeDefaults() {
  if (!writeConfig(0, 0x8F)) return false;
  if (!writeConfig(1, 0xA0)) return false;
  out_[0] = (1u << XL_OE) | (1u << XL_CAM_PWDN);
  out_[1] = (1u << (XL_T_CS - 8)) | (1u << (XL_LCD_RST - 8)) | (1u << (XL_LCD_CS - 8));
  if (!writePort(0, out_[0])) return false;
  if (!writePort(1, out_[1])) return false;
  return true;
}

bool XL9555::writePort(uint8_t port, uint8_t value) {
  if (port > 1) return false;
  out_[port] = value;
  return writeReg(0x02 + port, value);
}

bool XL9555::readPort(uint8_t port, uint8_t &value) {
  if (port > 1) return false;
  return readReg(0x00 + port, value);
}

bool XL9555::writeConfig(uint8_t port, uint8_t config) {
  if (port > 1) return false;
  return writeReg(0x06 + port, config);
}

bool XL9555::setPin(uint8_t pin, bool level) {
  if (pin > 15) return false;
  uint8_t port = pin / 8;
  uint8_t bit = pin % 8;
  if (level) out_[port] |= (1u << bit);
  else out_[port] &= ~(1u << bit);
  return writePort(port, out_[port]);
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
