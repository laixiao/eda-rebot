#include "board_spi.h"
#include "board_config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "spi";
static spi_device_handle_t s_dev = nullptr;

bool board_spi_init() {
  if (s_dev) return true;

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_LCD_MOSI;
  bus.miso_io_num = PIN_LCD_MISO;
  bus.sclk_io_num = PIN_LCD_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;

  esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
    return false;
  }

  // CS 由 XL9555 控制，这里用软件 CS（SPI_DEVICE_NO_CS）
  spi_device_interface_config_t dev = {};
  dev.mode = 0;
  dev.clock_speed_hz = 40 * 1000 * 1000;
  dev.spics_io_num = -1;
  dev.queue_size = 4;
  dev.flags = SPI_DEVICE_NO_DUMMY;

  err = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
    s_dev = nullptr;
    return false;
  }
  return true;
}

spi_device_handle_t board_spi_device() { return s_dev; }

static bool xfer(const uint8_t *tx, uint8_t *rx, size_t len, uint32_t hz) {
  if (!s_dev || !len) return false;
  spi_transaction_t t = {};
  t.length = len * 8;
  t.tx_buffer = tx;
  t.rx_buffer = rx;
  // 运行时改时钟：通过临时重配较麻烦，固定用设备时钟；高速填充时依赖默认
  (void)hz;
  return spi_device_polling_transmit(s_dev, &t) == ESP_OK;
}

bool board_spi_tx(const uint8_t *data, size_t len, uint32_t hz) {
  return xfer(data, nullptr, len, hz);
}

bool board_spi_txrx(const uint8_t *tx, uint8_t *rx, size_t len, uint32_t hz) {
  return xfer(tx, rx, len, hz);
}

bool board_spi_write16(uint16_t v, uint32_t hz) {
  uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
  return board_spi_tx(b, 2, hz);
}
