#include "pca9685.h"
#include "board_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const uint8_t MODE1 = 0x00;
static const uint8_t MODE2 = 0x01;
static const uint8_t PRESCALE = 0xFE;
static const uint8_t LED0_ON_L = 0x06;
static const uint8_t MODE1_SLEEP = 0x10;
static const uint8_t MODE1_AI = 0x20;
static const uint8_t MODE1_RESTART = 0x80;

bool PCA9685::write8(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  const bool ok = board_i2c_write(dev_, buf, 2);
  ok_ = ok;
  return ok;
}

bool PCA9685::read8(uint8_t reg, uint8_t &val) {
  const bool ok = board_i2c_write_read(dev_, &reg, 1, &val, 1);
  ok_ = ok;
  return ok;
}

bool PCA9685::begin(uint8_t addr, float freqHz) {
  ok_ = false;
  if (!board_i2c_add_device(addr, &dev_)) {
    return false;
  }
  uint8_t mode = 0;
  ok_ = read8(MODE1, mode);
  if (!ok_) return false;
  if (!write8(MODE2, 0x04)) return false;
  if (!write8(MODE1, MODE1_AI)) return false;
  vTaskDelay(pdMS_TO_TICKS(1));
  if (!allOff()) return false;
  ok_ = setPWMFreq(freqHz);
  return ok_;
}

bool PCA9685::setPWMFreq(float freqHz) {
  if (freqHz < 24.0f) freqHz = 24.0f;
  if (freqHz > 1526.0f) freqHz = 1526.0f;
  freq_ = freqHz;
  float prescale_f = 25000000.0f / (4096.0f * freqHz) - 1.0f;
  uint8_t prescale = (uint8_t)(prescale_f + 0.5f);

  uint8_t oldmode = 0;
  if (!read8(MODE1, oldmode)) return false;
  uint8_t sleepmode = (oldmode & ~MODE1_RESTART) | MODE1_SLEEP;
  if (!write8(MODE1, sleepmode)) return false;
  if (!write8(PRESCALE, prescale)) return false;
  if (!write8(MODE1, oldmode)) return false;
  vTaskDelay(pdMS_TO_TICKS(1));
  return write8(MODE1, oldmode | MODE1_RESTART | MODE1_AI);
}

bool PCA9685::setPWM(uint8_t channel, uint16_t on, uint16_t off) {
  if (channel > 15) return false;
  on &= 0x1FFF;
  off &= 0x1FFF;
  uint8_t buf[5] = {
      (uint8_t)(LED0_ON_L + 4 * channel),
      (uint8_t)(on & 0xFF),
      (uint8_t)(on >> 8),
      (uint8_t)(off & 0xFF),
      (uint8_t)(off >> 8),
  };
  const bool ok = board_i2c_write(dev_, buf, 5);
  ok_ = ok;
  return ok;
}

bool PCA9685::setDuty(uint8_t channel, uint16_t duty12) {
  if (duty12 == 0) return setPWM(channel, 0, 0x1000);
  if (duty12 >= 4095) return setPWM(channel, 0x1000, 0);
  return setPWM(channel, 0, duty12);
}

bool PCA9685::setPulseUs(uint8_t channel, uint16_t us) {
  float period_us = 1000000.0f / freq_;
  uint16_t ticks = (uint16_t)((us * 4096.0f) / period_us + 0.5f);
  if (ticks > 4095) ticks = 4095;
  return setDuty(channel, ticks);
}

bool PCA9685::allOff() {
  for (uint8_t ch = 0; ch < 16; ch++) {
    if (!setPWM(ch, 0, 0x1000)) return false;
  }
  return true;
}
