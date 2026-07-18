#pragma once

#include <stdint.h>
#include <stddef.h>

bool board_i2s_init();
bool board_i2s_ready();
bool board_i2s_mic_rms(int32_t &rms, int32_t &peak);
bool board_i2s_beep(uint16_t ms);
