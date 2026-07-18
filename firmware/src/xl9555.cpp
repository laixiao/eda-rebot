#include "xl9555.h"
#include "board_config.h"

bool XL9555::writeReg(uint8_t reg, uint8_t val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  wire_->write(val);
  return wire_->endTransmission() == 0;
}

bool XL9555::readReg(uint8_t reg, uint8_t &val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;
  if (wire_->requestFrom(addr_, (uint8_t)1) != 1) return false;
  val = wire_->read();
  return true;
}

bool XL9555::begin(TwoWire &wire, uint8_t addr) {
  wire_ = &wire;
  addr_ = addr;
  uint8_t dummy = 0;
  ok_ = readReg(0x00, dummy);
  if (!ok_) return false;
  return applySafeDefaults();
}

bool XL9555::applySafeDefaults() {
  // Port0: ENC 输入；CAM_PWDN/STBY/OE 输出；IO0_7 输入 → 0b10001111
  // Port1: LCD/CS 等输出；T_IRQ 输入；Amp 输出；IO1_7 输入 → 0b10100000
  if (!writeConfig(0, 0x8F)) return false;
  if (!writeConfig(1, 0xA0)) return false;
  // OE#=1 禁 PWM，STBY=0，CAM_PWDN=1 掉电摄像头
  out_[0] = (1u << XL_OE) | (1u << XL_CAM_PWDN);
  // T_CS/LCD_CS/LCD_RST=1，背光关，Amp=0
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
