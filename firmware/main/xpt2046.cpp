#include "xpt2046.h"
#include "board_config.h"
#include "board_spi.h"

static const uint32_t TOUCH_HZ = 2000000;

bool XPT2046::begin(XL9555 &xl) {
  xl_ = &xl;
  ready_ = xl_->present() && xl_->setPin(XL_T_CS, true) && board_spi_init();
  return ready_;
}

uint16_t XPT2046::readRaw(uint8_t cmd) {
  if (!ready_ || !xl_->setPin(XL_T_CS, false)) return 0;
  uint8_t tx[3] = {cmd, 0, 0};
  alignas(4) uint8_t rx[4] = {0, 0, 0, 0};
  const bool ok = board_spi_txrx(tx, rx, 3, TOUCH_HZ);
  ready_ = xl_->setPin(XL_T_CS, true) && ok;
  if (!ready_) return 0;
  uint16_t v = ((uint16_t)rx[1] << 8) | rx[2];
  return v >> 3;
}

bool XPT2046::touched() {
  if (!ready_ || !xl_) return false;
  bool irq = true;
  if (!xl_->getPin(XL_T_IRQ, irq)) {
    ready_ = false;
    return false;
  }
  return !irq;
}

bool XPT2046::read(uint16_t &x, uint16_t &y, uint16_t &z) {
  if (!ready_ || !xl_) return false;
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
