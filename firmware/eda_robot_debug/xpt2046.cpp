#include "xpt2046.h"
#include "board_config.h"

static const uint32_t TOUCH_HZ = 2000000;

bool XPT2046::begin(SPIClass &spi, XL9555 &xl) {
  spi_ = &spi;
  xl_ = &xl;
  xl_->setPin(XL_T_CS, true);
  return true;
}

uint16_t XPT2046::readRaw(uint8_t cmd) {
  xl_->setPin(XL_T_CS, false);
  spi_->beginTransaction(SPISettings(TOUCH_HZ, MSBFIRST, SPI_MODE0));
  spi_->transfer(cmd);
  uint16_t v = (uint16_t)spi_->transfer16(0) >> 3;
  spi_->endTransaction();
  xl_->setPin(XL_T_CS, true);
  return v;
}

bool XPT2046::touched() {
  bool irq = true;
  if (!xl_->getPin(XL_T_IRQ, irq)) return false;
  return !irq;  // 按下时 T_IRQ 拉低
}

bool XPT2046::read(uint16_t &x, uint16_t &y, uint16_t &z) {
  // 0xD0=X, 0x90=Y, 0xB0=Z1
  uint32_t xs = 0, ys = 0;
  for (int i = 0; i < 4; i++) {
    xs += readRaw(0xD0);
    ys += readRaw(0x90);
  }
  x = (uint16_t)(xs / 4);
  y = (uint16_t)(ys / 4);
  z = readRaw(0xB0);
  return z > 80;
}
