#include "board_i2s.h"
#include "board_config.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "i2s";
static i2s_chan_handle_t s_mic_rx = nullptr;
static i2s_chan_handle_t s_amp_tx = nullptr;
static bool s_ok = false;

bool board_i2s_ready() { return s_ok; }

bool board_i2s_init() {
  if (s_ok) return true;

  i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  mic_chan.auto_clear = true;
  if (i2s_new_channel(&mic_chan, nullptr, &s_mic_rx) != ESP_OK) {
    ESP_LOGE(TAG, "mic channel failed");
    return false;
  }

  i2s_std_config_t mic_std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)PIN_I2S_MIC_SCK,
              .ws = (gpio_num_t)PIN_I2S_MIC_WS,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t)PIN_I2S_MIC_SD,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  mic_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  if (i2s_channel_init_std_mode(s_mic_rx, &mic_std) != ESP_OK ||
      i2s_channel_enable(s_mic_rx) != ESP_OK) {
    ESP_LOGE(TAG, "mic init failed");
    return false;
  }

  i2s_chan_config_t amp_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  amp_chan.auto_clear = true;
  if (i2s_new_channel(&amp_chan, &s_amp_tx, nullptr) != ESP_OK) {
    ESP_LOGE(TAG, "amp channel failed");
    return false;
  }

  i2s_std_config_t amp_std = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)PIN_I2S_AMP_BCLK,
              .ws = (gpio_num_t)PIN_I2S_AMP_LRC,
              .dout = (gpio_num_t)PIN_I2S_AMP_DIN,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  if (i2s_channel_init_std_mode(s_amp_tx, &amp_std) != ESP_OK ||
      i2s_channel_enable(s_amp_tx) != ESP_OK) {
    ESP_LOGE(TAG, "amp init failed");
    return false;
  }

  s_ok = true;
  return true;
}

bool board_i2s_mic_rms(int32_t &rms, int32_t &peak) {
  if (!s_mic_rx) return false;
  int32_t samples[256];
  size_t n_bytes = 0;
  if (i2s_channel_read(s_mic_rx, samples, sizeof(samples), &n_bytes, 100) != ESP_OK) return false;
  size_t n = n_bytes / sizeof(int32_t);
  if (n == 0) return false;
  int64_t acc = 0;
  peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = samples[i] >> 14;
    if (v < 0) v = -v;
    acc += v;
    if (v > peak) peak = v;
  }
  rms = (int32_t)(acc / (int64_t)n);
  return true;
}

bool board_i2s_beep(uint16_t ms) {
  if (!s_amp_tx) return false;
  const int rate = 16000;
  const int freq = 1000;
  const size_t n = (size_t)rate * ms / 1000;
  int16_t frame[2];
  for (size_t i = 0; i < n; i++) {
    float t = (float)i / rate;
    int16_t s = (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * freq * t));
    frame[0] = s;
    frame[1] = s;
    size_t written = 0;
    if (i2s_channel_write(s_amp_tx, frame, sizeof(frame), &written, 100) != ESP_OK) return false;
  }
  frame[0] = frame[1] = 0;
  for (int i = 0; i < 200; i++) {
    size_t written = 0;
    i2s_channel_write(s_amp_tx, frame, sizeof(frame), &written, 50);
  }
  return true;
}
