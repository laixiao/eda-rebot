#include "board_i2c.h"
#include "board_config.h"
#include "esp_log.h"

static const char *TAG = "i2c";
static i2c_master_bus_handle_t s_bus = nullptr;

bool board_i2c_init() {
  if (s_bus) return true;
  i2c_master_bus_config_t cfg = {};
  cfg.i2c_port = I2C_NUM_0;
  cfg.sda_io_num = (gpio_num_t)PIN_I2C_SDA;
  cfg.scl_io_num = (gpio_num_t)PIN_I2C_SCL;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = true;
  esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
    s_bus = nullptr;
    return false;
  }
  return true;
}

i2c_master_bus_handle_t board_i2c_bus() { return s_bus; }

bool board_i2c_add_device(uint8_t addr7, i2c_master_dev_handle_t *out, uint32_t scl_hz) {
  if (!s_bus || !out) return false;
  i2c_device_config_t dev = {};
  dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev.device_address = addr7;
  dev.scl_speed_hz = scl_hz;
  esp_err_t err = i2c_master_bus_add_device(s_bus, &dev, out);
  return err == ESP_OK;
}

bool board_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout_ms) {
  if (!dev) return false;
  return i2c_master_transmit(dev, data, len, timeout_ms) == ESP_OK;
}

bool board_i2c_write_read(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wlen, uint8_t *r,
                          size_t rlen, int timeout_ms) {
  if (!dev) return false;
  return i2c_master_transmit_receive(dev, w, wlen, r, rlen, timeout_ms) == ESP_OK;
}

bool board_i2c_probe(uint8_t addr7) {
  if (!s_bus) return false;
  return i2c_master_probe(s_bus, addr7, 50) == ESP_OK;
}
