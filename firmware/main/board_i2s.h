#pragma once

#include <stdint.h>
#include <stddef.h>

bool board_i2s_init();
bool board_i2s_ready();
bool board_i2s_mic_rms(int32_t &rms, int32_t &peak);
bool board_i2s_beep(uint16_t ms);

/** 从麦克风读 mono PCM16（16 kHz），最多 max_samples；*got 为实际样点数。 */
bool board_i2s_mic_read_pcm16(int16_t *out, size_t max_samples, size_t *got);

/** 经功放播放 mono PCM16（16 kHz）；内部扩成立体声写入，并按当前音量缩放。 */
bool board_i2s_play_pcm16(const int16_t *mono, size_t n_samples);

/** 数字音量 0..100（默认 100）；影响 play / beep。 */
void board_i2s_set_volume(uint8_t pct);
uint8_t board_i2s_get_volume();

static constexpr int BOARD_I2S_RATE = 16000;
