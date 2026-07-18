#pragma once

#include "driver/i2c_master.h"
#include <stdint.h>
#include <stddef.h>

bool board_i2c_init();
i2c_master_bus_handle_t board_i2c_bus();

bool board_i2c_add_device(uint8_t addr7, i2c_master_dev_handle_t *out, uint32_t scl_hz = 400000);
bool board_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout_ms = 100);
bool board_i2c_write_read(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wlen, uint8_t *r,
                          size_t rlen, int timeout_ms = 100);
bool board_i2c_probe(uint8_t addr7);
