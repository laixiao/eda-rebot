#pragma once

#include "driver/spi_master.h"
#include <stdint.h>
#include <stddef.h>

bool board_spi_init();
spi_device_handle_t board_spi_device();

bool board_spi_tx(const uint8_t *data, size_t len, uint32_t hz);
bool board_spi_txrx(const uint8_t *tx, uint8_t *rx, size_t len, uint32_t hz);
bool board_spi_write16(uint16_t v, uint32_t hz);
