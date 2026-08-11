#include "wake_reply.h"
#include "board_i2s.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "wake_reply";

struct WakeReplyAsset {
  const uint8_t *start;
  const uint8_t *end;
  const char *name;
};

#include "wake_reply_assets.inc"

size_t wake_reply_count() {
  return sizeof(kWakeReplyAssets) / sizeof(kWakeReplyAssets[0]);
}

static int16_t *alloc_pcm(size_t n) {
  int16_t *p = (int16_t *)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (int16_t *)malloc(n * sizeof(int16_t));
  return p;
}

/** 线性重采样到 BOARD_I2S_RATE；src 为 mono PCM16。 */
static int16_t *resample_mono(const int16_t *src, size_t src_n, uint32_t src_rate, size_t *out_n) {
  *out_n = 0;
  if (!src || src_n == 0 || src_rate == 0) return nullptr;
  if (src_rate == (uint32_t)BOARD_I2S_RATE) {
    int16_t *cp = alloc_pcm(src_n);
    if (!cp) return nullptr;
    memcpy(cp, src, src_n * sizeof(int16_t));
    *out_n = src_n;
    return cp;
  }
  const size_t dst_n =
      (size_t)(((uint64_t)src_n * (uint64_t)BOARD_I2S_RATE + (uint64_t)src_rate / 2) / (uint64_t)src_rate);
  if (dst_n == 0) return nullptr;
  int16_t *dst = alloc_pcm(dst_n);
  if (!dst) return nullptr;
  for (size_t i = 0; i < dst_n; i++) {
    const double pos = (double)i * (double)src_rate / (double)BOARD_I2S_RATE;
    size_t i0 = (size_t)pos;
    if (i0 >= src_n) i0 = src_n - 1;
    size_t i1 = i0 + 1;
    if (i1 >= src_n) i1 = src_n - 1;
    const double frac = pos - (double)i0;
    const double s = (1.0 - frac) * (double)src[i0] + frac * (double)src[i1];
    if (s > 32767.0) dst[i] = 32767;
    else if (s < -32768.0) dst[i] = -32768;
    else dst[i] = (int16_t)s;
  }
  *out_n = dst_n;
  return dst;
}

static bool decode_wav_to_16k(const uint8_t *buf, size_t len, int16_t **pcm, size_t *n_samples) {
  *pcm = nullptr;
  *n_samples = 0;
  if (!buf || len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return false;

  size_t pos = 12;
  uint16_t audioFmt = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t *data = nullptr;
  uint32_t dataLen = 0;

  while (pos + 8 <= len) {
    const char *id = (const char *)(buf + pos);
    uint32_t sz = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
                 ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
    pos += 8;
    if (sz > len - pos) break;
    if (!memcmp(id, "fmt ", 4) && sz >= 16) {
      audioFmt = (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
      channels = (uint16_t)buf[pos + 2] | ((uint16_t)buf[pos + 3] << 8);
      rate = (uint32_t)buf[pos + 4] | ((uint32_t)buf[pos + 5] << 8) |
             ((uint32_t)buf[pos + 6] << 16) | ((uint32_t)buf[pos + 7] << 24);
      bits = (uint16_t)buf[pos + 14] | ((uint16_t)buf[pos + 15] << 8);
    } else if (!memcmp(id, "data", 4)) {
      data = buf + pos;
      dataLen = sz;
      break;
    }
    pos += (size_t)((sz + 1) & ~1u);
  }

  if (!data || audioFmt != 1 || bits != 16) return false;
  if (channels != 1 && channels != 2) return false;

  const size_t frameBytes = (size_t)channels * 2;
  if (frameBytes == 0 || dataLen < frameBytes) return false;
  const size_t frames = dataLen / frameBytes;
  const int16_t *src = (const int16_t *)data;

  int16_t *mono = nullptr;
  bool mono_owned = false;
  if (channels == 1) {
    mono = (int16_t *)src;  // points into embed; resample will copy
  } else {
    mono = alloc_pcm(frames);
    if (!mono) return false;
    mono_owned = true;
    for (size_t i = 0; i < frames; i++) {
      const int32_t L = src[i * 2];
      const int32_t R = src[i * 2 + 1];
      mono[i] = (int16_t)((L + R) / 2);
    }
  }

  int16_t *out = resample_mono(mono, frames, rate, n_samples);
  if (mono_owned) free(mono);
  if (!out) return false;
  *pcm = out;
  return true;
}

bool wake_reply_decode_random(int16_t **pcm, size_t *n_samples, const char **name) {
  if (pcm) *pcm = nullptr;
  if (n_samples) *n_samples = 0;
  if (name) *name = nullptr;

  const size_t n = wake_reply_count();
  if (n == 0 || !pcm || !n_samples) return false;

  const size_t idx = (size_t)(esp_random() % (uint32_t)n);
  const WakeReplyAsset &a = kWakeReplyAssets[idx];
  const size_t len = (size_t)(a.end - a.start);
  if (len < 44) {
    ESP_LOGE(TAG, "asset %s too small", a.name);
    return false;
  }
  if (!decode_wav_to_16k(a.start, len, pcm, n_samples)) {
    ESP_LOGE(TAG, "decode fail %s", a.name);
    return false;
  }
  if (name) *name = a.name;
  ESP_LOGI(TAG, "pick %s -> %u samples @%dHz", a.name, (unsigned)*n_samples, BOARD_I2S_RATE);
  return true;
}
