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
static uint8_t s_volume = 100;
static int s_mic_users = 0;
static bool s_amp_on = false;

void board_i2s_set_volume(uint8_t pct) {
  if (pct > 100) pct = 100;
  s_volume = pct;
}

uint8_t board_i2s_get_volume() { return s_volume; }

static inline int16_t apply_volume(int16_t s) {
  if (s_volume >= 100) return s;
  if (s_volume == 0) return 0;
  return (int16_t)(((int32_t)s * (int32_t)s_volume) / 100);
}

static void cleanup_channels() {
  if (s_mic_rx) {
    if (s_mic_users > 0) i2s_channel_disable(s_mic_rx);
    i2s_del_channel(s_mic_rx);
    s_mic_rx = nullptr;
  }
  if (s_amp_tx) {
    if (s_amp_on) i2s_channel_disable(s_amp_tx);
    i2s_del_channel(s_amp_tx);
    s_amp_tx = nullptr;
  }
  s_mic_users = 0;
  s_amp_on = false;
  s_ok = false;
}

bool board_i2s_ready() { return s_ok; }

bool board_i2s_mic_acquire() {
  if (!s_mic_rx) return false;
  if (s_mic_users == 0) {
    if (i2s_channel_enable(s_mic_rx) != ESP_OK) {
      ESP_LOGE(TAG, "mic enable failed");
      return false;
    }
  }
  s_mic_users++;
  return true;
}

void board_i2s_mic_release() {
  if (!s_mic_rx || s_mic_users <= 0) return;
  s_mic_users--;
  if (s_mic_users == 0) {
    i2s_channel_disable(s_mic_rx);
  }
}

static bool amp_ensure(bool on) {
  if (!s_amp_tx) return false;
  if (on == s_amp_on) return true;
  if (on) {
    if (i2s_channel_enable(s_amp_tx) != ESP_OK) return false;
    s_amp_on = true;
  } else {
    i2s_channel_disable(s_amp_tx);
    s_amp_on = false;
  }
  return true;
}

bool board_i2s_init() {
  if (s_ok) return true;

  i2s_chan_config_t mic_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  mic_chan.auto_clear = true;
  mic_chan.dma_desc_num = 4;
  mic_chan.dma_frame_num = 240;
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
  // 只 init，不 enable：常开 RX DMA 会与 WiFi 争用导致 Interrupt WDT（访问网页即重启）
  if (i2s_channel_init_std_mode(s_mic_rx, &mic_std) != ESP_OK) {
    ESP_LOGE(TAG, "mic init failed");
    cleanup_channels();
    return false;
  }

  i2s_chan_config_t amp_chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  amp_chan.auto_clear = true;
  amp_chan.dma_desc_num = 4;
  amp_chan.dma_frame_num = 240;
  if (i2s_new_channel(&amp_chan, &s_amp_tx, nullptr) != ESP_OK) {
    ESP_LOGE(TAG, "amp channel failed");
    cleanup_channels();
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
  if (i2s_channel_init_std_mode(s_amp_tx, &amp_std) != ESP_OK) {
    ESP_LOGE(TAG, "amp init failed");
    cleanup_channels();
    return false;
  }

  s_ok = true;
  ESP_LOGI(TAG, "I2S ready (mic/amp DMA idle until use)");
  return true;
}

bool board_i2s_mic_rms(int32_t &rms, int32_t &peak) {
  if (!s_mic_rx) return false;
  if (!board_i2s_mic_acquire()) return false;
  int32_t samples[256];
  size_t n_bytes = 0;
  const esp_err_t err = i2s_channel_read(s_mic_rx, samples, sizeof(samples), &n_bytes, 100);
  board_i2s_mic_release();
  if (err != ESP_OK) return false;
  size_t n = n_bytes / sizeof(int32_t);
  if (n == 0) return false;
  int64_t acc = 0;
  peak = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = samples[i] >> 14;
    if (v < 0) v = -v;
    acc += (int64_t)v * v;
    if (v > peak) peak = v;
  }
  rms = (int32_t)sqrt((double)acc / (double)n);
  return true;
}

bool board_i2s_mic_read_pcm16(int16_t *out, size_t max_samples, size_t *got) {
  if (!s_mic_rx || !out || max_samples == 0) return false;
  if (got) *got = 0;
  const bool held = s_mic_users > 0;
  if (!held && !board_i2s_mic_acquire()) return false;

  int32_t raw[256];
  size_t filled = 0;
  while (filled < max_samples) {
    const size_t want = (max_samples - filled) > 256 ? 256 : (max_samples - filled);
    size_t n_bytes = 0;
    if (i2s_channel_read(s_mic_rx, raw, want * sizeof(int32_t), &n_bytes, 100) != ESP_OK)
      break;
    const size_t n = n_bytes / sizeof(int32_t);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      int32_t v = raw[i] >> 14;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      out[filled++] = (int16_t)v;
    }
  }
  if (!held) board_i2s_mic_release();
  if (got) *got = filled;
  return filled > 0;
}

bool board_i2s_play_pcm16(const int16_t *mono, size_t n_samples) {
  if (!s_amp_tx || !mono || n_samples == 0) return false;
  if (!amp_ensure(true)) return false;
  int16_t frames[256 * 2];
  size_t done = 0;
  bool ok = true;
  while (done < n_samples) {
    const size_t count = (n_samples - done) > 256 ? 256 : (n_samples - done);
    for (size_t i = 0; i < count; i++) {
      const int16_t s = apply_volume(mono[done + i]);
      frames[i * 2] = s;
      frames[i * 2 + 1] = s;
    }
    size_t written = 0;
    const size_t bytes = count * 2 * sizeof(int16_t);
    if (i2s_channel_write(s_amp_tx, frames, bytes, &written, 250) != ESP_OK || written != bytes) {
      ok = false;
      break;
    }
    done += count;
  }
  if (ok) {
    memset(frames, 0, sizeof(frames));
    size_t written = 0;
    ok = i2s_channel_write(s_amp_tx, frames, sizeof(frames), &written, 250) == ESP_OK;
  }
  amp_ensure(false);
  return ok;
}

bool board_i2s_beep(uint16_t ms) {
  if (!s_amp_tx) return false;
  if (!amp_ensure(true)) return false;
  const int rate = BOARD_I2S_RATE;
  const int freq = 1000;
  const size_t n = (size_t)rate * ms / 1000;
  int16_t frames[256 * 2];
  size_t done = 0;
  bool ok = true;
  while (done < n) {
    const size_t count = (n - done) > 256 ? 256 : (n - done);
    for (size_t i = 0; i < count; i++) {
      float t = (float)(done + i) / rate;
      int16_t s = apply_volume((int16_t)(8000.0f * sinf(2.0f * (float)M_PI * freq * t)));
      frames[i * 2] = s;
      frames[i * 2 + 1] = s;
    }
    size_t written = 0;
    const size_t bytes = count * 2 * sizeof(int16_t);
    if (i2s_channel_write(s_amp_tx, frames, bytes, &written, 250) != ESP_OK || written != bytes) {
      ok = false;
      break;
    }
    done += count;
  }
  if (ok) {
    memset(frames, 0, sizeof(frames));
    size_t written = 0;
    ok = i2s_channel_write(s_amp_tx, frames, sizeof(frames), &written, 250) == ESP_OK;
  }
  amp_ensure(false);
  return ok;
}
