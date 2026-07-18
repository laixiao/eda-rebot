#include "xpt2046.h"
#include "board_config.h"
#include "board_spi.h"

static const uint32_t TOUCH_HZ = 2000000;

bool XPT2046::begin(XL9555 &xl) {
  xl_ = &xl;
  xl_->setPin(XL_T_CS, true);
  return board_spi_init();
}

uint16_t XPT2046::readRaw(uint8_t cmd) {
  xl_->setPin(XL_T_CS, false);
  uint8_t tx[3] = {cmd, 0, 0};
  uint8_t rx[3] = {0, 0, 0};
  board_spi_txrx(tx, rx, 3, TOUCH_HZ);
  xl_->setPin(XL_T_CS, true);
  uint16_t v = ((uint16_t)rx[1] << 8) | rx[2];
  return v >> 3;
}

bool XPT2046::touched() {
  bool irq = true;
  if (!xl_->getPin(XL_T_IRQ, irq)) return false;
  return !irq;
}

bool XPT2046::read(uint16_t &x, uint16_t &y, uint16_t &z) {
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
