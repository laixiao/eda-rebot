#include "radar_at6010.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"

static const char *TAG = "radar";
static const uart_port_t RADAR_UART = UART_NUM_1;
static const int RADAR_RX_BUF = 2048;

static SemaphoreHandle_t sMtx = nullptr;
static bool sUartOn = false;
static uint32_t sBaud = 115200;
static bool sPinsSwapped = false;
static bool sUartInverted = false;
static bool sGpioOut = false;

static uint8_t sRx[1024];
static size_t sRxLen = 0;

static RadarSnapshot sState = {};
static int16_t sPrevAngle = 0;
static bool sHaveAngle = false;
static int64_t sSwipeUntil = 0;
static char sSwipeLabel[16] = {0};

static uint16_t ciChecksum(const uint8_t *data, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += data[i];
  return (uint16_t)sum;
}

static void rememberFrame(const uint8_t *data, size_t n) {
  static const char HEX[] = "0123456789ABCDEF";
  const size_t maxBytes = (sizeof(sState.last_frame_hex) - 1) / 2;
  if (n > maxBytes) n = maxBytes;
  for (size_t i = 0; i < n; i++) {
    sState.last_frame_hex[i * 2] = HEX[data[i] >> 4];
    sState.last_frame_hex[i * 2 + 1] = HEX[data[i] & 0x0F];
  }
  sState.last_frame_hex[n * 2] = 0;
}

static void clearPrimary() {
  sState.primary_valid = false;
  sState.is_detected = 0;
  sState.range_mm = 0;
  sState.angle_deg = 0;
  sState.velo = 0;
  sState.rb_conf = 0;
  sState.angle_conf = 0;
}

static void clearMulti() {
  sState.declared_obj_num = 0;
  sState.obj_num = 0;
  sState.multi_valid = false;
  sState.truncated = false;
  for (int i = 0; i < RADAR_OBJ_MAX; i++) sState.objs[i] = {};
}

static bool sendCi(uint8_t cmd, const uint8_t *params, uint8_t plen) {
  if (!sUartOn) return false;
  uint8_t frame[64];
  if ((size_t)plen + 5 > sizeof(frame)) return false;
  frame[0] = 0x58;
  frame[1] = cmd;
  frame[2] = plen;
  if (plen && params) memcpy(frame + 3, params, plen);
  const uint16_t sum = ciChecksum(frame, 3 + plen);
  frame[3 + plen] = (uint8_t)(sum & 0xFF);
  frame[4 + plen] = (uint8_t)(sum >> 8);
  const int n = 5 + plen;
  const int w = uart_write_bytes(RADAR_UART, (const char *)frame, n);
  return w == n;
}

static void pushTrail(uint16_t range_mm, int16_t angle_deg) {
  if (sState.trail_len < RADAR_TRAIL_MAX) {
    sState.trail[sState.trail_len++] = {range_mm, angle_deg, esp_timer_get_time()};
  } else {
    memmove(sState.trail, sState.trail + 1, sizeof(RadarTrailPoint) * (RADAR_TRAIL_MAX - 1));
    sState.trail[RADAR_TRAIL_MAX - 1] = {range_mm, angle_deg, esp_timer_get_time()};
  }
}

static void formatDetText(uint8_t det, char *out, size_t n) {
  out[0] = 0;
  auto append = [&](const char *s) {
    if (out[0]) strncat(out, "+", n - strlen(out) - 1);
    strncat(out, s, n - strlen(out) - 1);
  };
  if (det & 0x01) append("靠近");
  if (det & 0x02) append("远离");
  if (det & 0x04) append("运动");
  if (det & 0x08) append("微动");
  if (det & 0x10) append("呼吸");
  if (!out[0]) strncpy(out, "无", n - 1);
}

static void updateGesture(uint8_t det, int16_t angle_deg, bool detected) {
  const int64_t now = esp_timer_get_time();
  if (sSwipeUntil > now && sSwipeLabel[0]) {
    strncpy(sState.gesture, sSwipeLabel, sizeof(sState.gesture) - 1);
    return;
  }
  if (detected && sHaveAngle) {
    const int d = (int)angle_deg - (int)sPrevAngle;
    if (d <= -20) {
      strncpy(sSwipeLabel, "扫左", sizeof(sSwipeLabel) - 1);
      sSwipeUntil = now + 800000;
      strncpy(sState.gesture, sSwipeLabel, sizeof(sState.gesture) - 1);
      sPrevAngle = angle_deg;
      return;
    }
    if (d >= 20) {
      strncpy(sSwipeLabel, "扫右", sizeof(sSwipeLabel) - 1);
      sSwipeUntil = now + 800000;
      strncpy(sState.gesture, sSwipeLabel, sizeof(sState.gesture) - 1);
      sPrevAngle = angle_deg;
      return;
    }
  }
  if (det & 0x01) strncpy(sState.gesture, "靠近", sizeof(sState.gesture) - 1);
  else if (det & 0x02) strncpy(sState.gesture, "远离", sizeof(sState.gesture) - 1);
  else if (det & 0x04) strncpy(sState.gesture, "挥动/运动", sizeof(sState.gesture) - 1);
  else if (det & 0x08) strncpy(sState.gesture, "微动", sizeof(sState.gesture) - 1);
  else if (det & 0x10) strncpy(sState.gesture, "呼吸存在", sizeof(sState.gesture) - 1);
  else if (sGpioOut || detected) strncpy(sState.gesture, "存在", sizeof(sState.gesture) - 1);
  else strncpy(sState.gesture, "无人", sizeof(sState.gesture) - 1);

  if (detected) {
    sPrevAngle = angle_deg;
    sHaveAngle = true;
  }
}

static void applyDetInfo(const uint8_t *p, size_t n, uint8_t type) {
  if (n < 8) {
    sState.malformed_frames++;
    return;
  }
  sState.report_type = type;
  sState.is_detected = p[0];
  sState.det_result = p[1];
  sState.range_mm = (uint16_t)(p[2] | (p[3] << 8));
  sState.angle_deg = (int16_t)(p[4] | (p[5] << 8));
  sState.velo = (int16_t)(p[6] | (p[7] << 8));
  sState.rb_conf = 0;
  sState.angle_conf = 0;
  if (n >= 20) {
    sState.rb_conf = p[14];
    sState.angle_conf = p[15];
    sState.frame_idx = (uint32_t)(p[16] | (p[17] << 8) | (p[18] << 16) | (p[19] << 24));
  }
  sState.primary_valid = sState.is_detected != 0;
  sState.last_primary_us = esp_timer_get_time();
  formatDetText(sState.det_result, sState.det_text, sizeof(sState.det_text));
  updateGesture(sState.det_result, sState.angle_deg, sState.primary_valid);
  if (sState.primary_valid) pushTrail(sState.range_mm, sState.angle_deg);
  else clearPrimary();
  sState.link_ok = true;
  sState.last_frame_us = esp_timer_get_time();
  sState.rx_frames++;
}

static void applyRegion(const uint8_t *p, size_t n) {
  if (n < 4) {
    sState.malformed_frames++;
    return;
  }
  const uint32_t declared = (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
  if (declared > (SIZE_MAX - 4) / 4 || n < 4 + (size_t)declared * 4) {
    sState.malformed_frames++;
    return;
  }

  RadarTarget parsed[RADAR_OBJ_MAX] = {};
  const uint32_t reported = declared > RADAR_OBJ_MAX ? RADAR_OBJ_MAX : declared;
  size_t off = 4;
  for (uint32_t i = 0; i < reported; i++) {
    const uint16_t r = (uint16_t)(p[off] | (p[off + 1] << 8));
    const int16_t a = (int16_t)(p[off + 2] | (p[off + 3] << 8));
    parsed[i] = {r, a, 0, 0, 0, true};
    off += 4;
  }

  sState.report_type = 5;
  sState.declared_obj_num = declared;
  sState.obj_num = (uint8_t)reported;
  sState.multi_valid = reported > 0;
  sState.truncated = declared > RADAR_OBJ_MAX;
  sState.last_multi_us = esp_timer_get_time();
  memcpy(sState.objs, parsed, sizeof(parsed));
  if (reported > 0) {
    pushTrail(parsed[0].range_mm, parsed[0].angle_deg);
    updateGesture(sState.det_result, parsed[0].angle_deg, true);
  } else if (!sState.primary_valid && !sGpioOut) {
    strncpy(sState.gesture, "无人", sizeof(sState.gesture) - 1);
  }
  formatDetText(sState.det_result, sState.det_text, sizeof(sState.det_text));
  sState.link_ok = true;
  sState.last_frame_us = esp_timer_get_time();
  sState.rx_frames++;
}

static void applyBhr(const uint8_t *p, size_t n) {
  if (n < 6) {
    sState.malformed_frames++;
    return;
  }
  sState.report_type = 4;
  sState.det_result = p[0];
  sState.br_val = p[1];
  sState.hr_val = p[2];
  sState.range_mm = (uint16_t)(p[4] | (p[5] << 8));
  sState.is_detected = (p[0] != 0) ? 1 : 0;
  formatDetText(sState.det_result, sState.det_text, sizeof(sState.det_text));
  updateGesture(sState.det_result, sState.angle_deg, sState.is_detected != 0);
  sState.link_ok = true;
  sState.last_frame_us = esp_timer_get_time();
  sState.rx_frames++;
}

static void handleAutoPayload(uint8_t type, const uint8_t *p, size_t n) {
  switch (type) {
    case 0:  // full
    case 3:  // motion/presence
      applyDetInfo(p, n, type);
      break;
    case 4:
      applyBhr(p, n);
      break;
    case 5:
      applyRegion(p, n);
      break;
    default:
      sState.report_type = type;
      sState.unknown_frames++;
      sState.link_ok = true;
      sState.last_frame_us = esp_timer_get_time();
      sState.rx_frames++;
      break;
  }
}

static void handleCiReply(uint8_t cmd, const uint8_t *p, uint8_t plen) {
  sState.link_ok = true;
  if (cmd == 0xFE && plen >= 7) {
    snprintf(sState.version, sizeof(sState.version), "SDK %u.%u.%u / cust %u.%u / hw %u.%u",
             p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
    sState.link_ok = true;
  } else if (cmd == 0x30 && plen >= 8) {
    applyDetInfo(p, plen, 0xFF);
  } else if (cmd == 0xD0 && plen >= 1) {
    sState.link_ok = true;
  }
  sState.last_frame_us = esp_timer_get_time();
  sState.rx_frames++;
}

static void consumeRx() {
  while (sRxLen > 0) {
    size_t i = 0;
    while (i < sRxLen && sRx[i] != 0x5A && sRx[i] != 0x59) i++;
    if (i > 0) {
      sState.discarded_bytes += (uint32_t)i;
      memmove(sRx, sRx + i, sRxLen - i);
      sRxLen -= i;
    }
    if (sRxLen < 2) return;

    if (sRx[0] == 0x5A) {
      if (sRxLen < 3) return;
      const uint8_t len = sRx[1];
      const size_t total = (size_t)2 + len + 1;  // HEAD LEN PAYLOAD CHECK
      if (len == 0 || total > sizeof(sRx)) {
        sState.malformed_frames++;
        sState.discarded_bytes++;
        memmove(sRx, sRx + 1, sRxLen - 1);
        sRxLen--;
        continue;
      }
      if (sRxLen < total) return;
      uint8_t sum = 0;
      for (size_t k = 0; k < 2 + len; k++) sum += sRx[k];
      if (sum != sRx[2 + len]) {
        sState.crc_err++;
        sState.discarded_bytes++;
        memmove(sRx, sRx + 1, sRxLen - 1);
        sRxLen--;
        continue;
      }
      const uint8_t type = sRx[2];
      rememberFrame(sRx, total);
      sState.frames_5a++;
      if (type < 8) sState.type_frames[type]++;
      handleAutoPayload(type, sRx + 3, len > 0 ? (size_t)len - 1 : 0);
      memmove(sRx, sRx + total, sRxLen - total);
      sRxLen -= total;
      continue;
    }

    // 0x59 CI reply: HEAD CMD LEN PARAMS SUM_LO SUM_HI
    if (sRxLen < 5) return;
    const uint8_t cmd = sRx[1];
    const uint8_t plen = sRx[2];
    const size_t total = (size_t)5 + plen;
    if (total > sizeof(sRx)) {
      sState.malformed_frames++;
      sState.discarded_bytes++;
      memmove(sRx, sRx + 1, sRxLen - 1);
      sRxLen--;
      continue;
    }
    if (sRxLen < total) return;
    const uint16_t expect = (uint16_t)(sRx[3 + plen] | (sRx[4 + plen] << 8));
    const uint16_t got = ciChecksum(sRx, 3 + plen);
    if (expect != got) {
      sState.crc_err++;
      sState.discarded_bytes++;
      memmove(sRx, sRx + 1, sRxLen - 1);
      sRxLen--;
      continue;
    }
    rememberFrame(sRx, total);
    sState.frames_59++;
    handleCiReply(cmd, sRx + 3, plen);
    memmove(sRx, sRx + total, sRxLen - total);
    sRxLen -= total;
  }
}

bool radar_init() {
  if (!sMtx) sMtx = xSemaphoreCreateMutex();
  memset(&sState, 0, sizeof(sState));
  strncpy(sState.gesture, "—", sizeof(sState.gesture) - 1);
  strncpy(sState.det_text, "—", sizeof(sState.det_text) - 1);
  strncpy(sState.version, "未读取", sizeof(sState.version) - 1);
  sState.baud = 115200;
  sState.uart_tx_pin = PIN_RADAR_UART_TX;
  sState.uart_rx_pin = PIN_RADAR_UART_RX;
  sState.report_type = 0xFF;
  return sMtx != nullptr;
}

bool radar_start(uint32_t baud, bool swap_pins, bool invert_uart) {
  if (!sMtx && !radar_init()) {
    ESP_LOGE(TAG, "mutex init failed");
    return false;
  }
  if (baud == 0) baud = 115200;
  if (sUartOn) {
    if (baud == sBaud && swap_pins == sPinsSwapped && invert_uart == sUartInverted) return true;
    // API 诊断切换只改变 ESP32 本地 UART，不向雷达写永久波特率。
    radar_stop();
  }

  const int txPin = swap_pins ? PIN_RADAR_UART_RX : PIN_RADAR_UART_TX;
  const int rxPin = swap_pins ? PIN_RADAR_UART_TX : PIN_RADAR_UART_RX;

  uart_config_t cfg = {};
  cfg.baud_rate = (int)baud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  esp_err_t err = uart_driver_install(RADAR_UART, RADAR_RX_BUF, 512, 0, nullptr, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
    return false;
  }
  err = uart_param_config(RADAR_UART, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
    uart_driver_delete(RADAR_UART);
    return false;
  }
  const uint32_t inverseMask =
      invert_uart ? (UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV) : UART_SIGNAL_INV_DISABLE;
  err = uart_set_line_inverse(RADAR_UART, inverseMask);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_line_inverse: %s", esp_err_to_name(err));
    uart_driver_delete(RADAR_UART);
    return false;
  }
  err = uart_set_pin(RADAR_UART, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
    uart_driver_delete(RADAR_UART);
    return false;
  }
  uart_flush(RADAR_UART);

  xSemaphoreTake(sMtx, portMAX_DELAY);
  sUartOn = true;
  sBaud = baud;
  sPinsSwapped = swap_pins;
  sUartInverted = invert_uart;
  sState.uart_on = true;
  sState.baud = baud;
  sState.pins_swapped = swap_pins;
  sState.uart_inverted = invert_uart;
  sState.uart_tx_pin = (uint8_t)txPin;
  sState.uart_rx_pin = (uint8_t)rxPin;
  sState.link_ok = false;
  sState.rx_frames = 0;
  sState.rx_bytes = 0;
  sState.crc_err = 0;
  sState.malformed_frames = 0;
  sState.unknown_frames = 0;
  sState.discarded_bytes = 0;
  sState.dropped_bytes = 0;
  sState.frames_59 = 0;
  sState.frames_5a = 0;
  memset(sState.type_frames, 0, sizeof(sState.type_frames));
  sState.uart_buffered_bytes = 0;
  sState.last_frame_hex[0] = 0;
  sState.probe_samples = 0;
  sState.probe_low_samples = 0;
  sState.probe_edges = 0;
  sState.last_frame_us = 0;
  sState.report_type = 0xFF;
  strncpy(sState.version, "未读取", sizeof(sState.version) - 1);
  sRxLen = 0;
  xSemaphoreGive(sMtx);

  ESP_LOGI(TAG, "UART1 on TX=%d RX=%d baud=%u swap=%d invert=%d", txPin, rxPin,
           (unsigned)baud, (int)swap_pins, (int)invert_uart);
  // MS60 vendor UART protocol is not yet available. Listen passively at boot;
  // AIR Touch HCI commands remain available only through explicit diagnostics.
  return true;
}

void radar_stop() {
  if (!sUartOn) return;
  xSemaphoreTake(sMtx, portMAX_DELAY);
  sUartOn = false;
  sState.uart_on = false;
  sState.link_ok = false;
  xSemaphoreGive(sMtx);
  uart_driver_delete(RADAR_UART);
  // restore ENC1 as GPIO inputs (ISR re-added by caller if needed)
  gpio_config_t io = {};
  io.intr_type = GPIO_INTR_DISABLE;
  io.mode = GPIO_MODE_INPUT;
  io.pin_bit_mask = (1ULL << PIN_RADAR_UART_RX) | (1ULL << PIN_RADAR_UART_TX);
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&io);
}

bool radar_running() { return sUartOn; }

void radar_set_gpio_out(bool level) {
  if (!sMtx) return;
  xSemaphoreTake(sMtx, portMAX_DELAY);
  sGpioOut = level;
  sState.gpio_out = level;
  sState.last_out_us = esp_timer_get_time();
  if (!sState.primary_valid && !sState.multi_valid)
    updateGesture(sState.det_result, sState.angle_deg, level);
  sState.present = sState.primary_valid || sState.multi_valid || sGpioOut;
  xSemaphoreGive(sMtx);
}

void radar_poll() {
  if (!sUartOn || !sMtx) return;
  uint8_t tmp[512];
  const int64_t deadline = esp_timer_get_time() + 3000;
  uint32_t totalRead = 0;

  xSemaphoreTake(sMtx, portMAX_DELAY);
  while (totalRead < 4096 && esp_timer_get_time() < deadline) {
    size_t buffered = 0;
    if (uart_get_buffered_data_len(RADAR_UART, &buffered) == ESP_OK)
      sState.uart_buffered_bytes = (uint32_t)buffered;
    if (buffered == 0) break;
    const size_t want = buffered < sizeof(tmp) ? buffered : sizeof(tmp);
    const int n = uart_read_bytes(RADAR_UART, tmp, want, 0);
    if (n <= 0) break;
    totalRead += (uint32_t)n;
    sState.rx_bytes += (uint32_t)n;
    for (int i = 0; i < n; i++) {
      if (sRxLen == sizeof(sRx)) {
        memmove(sRx, sRx + 1, sizeof(sRx) - 1);
        sRxLen--;
        sState.dropped_bytes++;
      }
      sRx[sRxLen++] = tmp[i];
    }
    consumeRx();
  }

  const int64_t now = esp_timer_get_time();
  if (sState.last_primary_us && now - sState.last_primary_us > 1500000) clearPrimary();
  if (sState.last_multi_us && now - sState.last_multi_us > 1500000) clearMulti();
  if (!sState.primary_valid && !sState.multi_valid && !sGpioOut &&
      !(sSwipeUntil > now))
    strncpy(sState.gesture, "无人", sizeof(sState.gesture) - 1);
  sState.present = sState.primary_valid || sState.multi_valid || sGpioOut;
  xSemaphoreGive(sMtx);
}

bool radar_probe_rx_edges(uint32_t duration_ms) {
  if (!sUartOn || !sMtx) return false;
  if (duration_ms < 10) duration_ms = 10;
  if (duration_ms > 250) duration_ms = 250;

  RadarSnapshot snap;
  radar_get_snapshot(snap);
  const gpio_num_t rxPin = (gpio_num_t)snap.uart_rx_pin;
  uart_flush_input(RADAR_UART);
  if (!radar_cmd_get_version()) return false;

  uint32_t samples = 0;
  uint32_t lowSamples = 0;
  uint32_t edges = 0;
  int prev = gpio_get_level(rxPin);
  const int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
  while (esp_timer_get_time() < deadline) {
    const int level = gpio_get_level(rxPin);
    samples++;
    if (!level) lowSamples++;
    if (level != prev) {
      edges++;
      prev = level;
    }
  }

  xSemaphoreTake(sMtx, portMAX_DELAY);
  sState.probe_samples = samples;
  sState.probe_low_samples = lowSamples;
  sState.probe_edges = edges;
  xSemaphoreGive(sMtx);
  return true;
}

bool radar_cmd_set_active_time(uint8_t seconds) { return sendCi(0x90, &seconds, 1); }

bool radar_cmd_sense(bool on) {
  uint8_t p = on ? 1 : 0;
  return sendCi(0xD1, &p, 1);
}

bool radar_cmd_get_version() { return sendCi(0xFE, nullptr, 0); }

bool radar_cmd_get_det() { return sendCi(0x30, nullptr, 0); }

bool radar_cmd_set_baud(uint32_t baud) {
  uint8_t p[4] = {(uint8_t)(baud & 0xFF), (uint8_t)((baud >> 8) & 0xFF),
                  (uint8_t)((baud >> 16) & 0xFF), (uint8_t)((baud >> 24) & 0xFF)};
  if (!sendCi(0x19, p, 4)) return false;
  vTaskDelay(pdMS_TO_TICKS(50));
  uart_set_baudrate(RADAR_UART, baud);
  xSemaphoreTake(sMtx, portMAX_DELAY);
  sBaud = baud;
  sState.baud = baud;
  xSemaphoreGive(sMtx);
  return true;
}

void radar_get_snapshot(RadarSnapshot &out) {
  if (!sMtx) {
    memset(&out, 0, sizeof(out));
    return;
  }
  xSemaphoreTake(sMtx, portMAX_DELAY);
  out = sState;
  out.present = sState.primary_valid || sState.multi_valid || sGpioOut;
  out.gpio_out = sGpioOut;
  out.uart_on = sUartOn;
  out.pins_swapped = sPinsSwapped;
  out.uart_inverted = sUartInverted;
  xSemaphoreGive(sMtx);
}

static void jsonEscape(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 2 < n; i++) {
    if (in[i] == '"' || in[i] == '\\') {
      out[j++] = '\\';
      out[j++] = in[i];
    } else if ((uint8_t)in[i] < 0x20) {
      out[j++] = ' ';
    } else {
      out[j++] = in[i];
    }
  }
  out[j] = 0;
}

size_t radar_json_summary(char *buf, size_t buflen) {
  RadarSnapshot s;
  radar_get_snapshot(s);
  char g[32], d[64], v[64];
  jsonEscape(s.gesture, g, sizeof(g));
  jsonEscape(s.det_text, d, sizeof(d));
  jsonEscape(s.version, v, sizeof(v));
  const int txLevel = gpio_get_level((gpio_num_t)s.uart_tx_pin);
  const int rxLevel = gpio_get_level((gpio_num_t)s.uart_rx_pin);
  return (size_t)snprintf(
      buf, buflen,
      "{\"ok\":true,\"protocol\":\"at6010_hci_unconfirmed\",\"idStable\":false,"
      "\"uart\":%s,\"link\":%s,\"baud\":%u,\"swap\":%s,\"invert\":%s,"
      "\"tx\":%u,\"rx\":%u,\"txLevel\":%d,\"rxLevel\":%d,\"gpioOut\":%s,\"present\":%s,"
      "\"primaryValid\":%s,\"range_mm\":%u,\"angle_deg\":%d,\"det\":\"%s\",\"gesture\":\"%s\","
      "\"multiValid\":%s,\"declaredObjNum\":%u,\"objNum\":%u,\"truncated\":%s,"
      "\"version\":\"%s\",\"rxFrames\":%u,\"rxBytes\":%u,\"crcErr\":%u,"
      "\"malformedFrames\":%u,\"unknownFrames\":%u,\"discardedBytes\":%u,\"droppedBytes\":%u,"
      "\"frames59\":%u,\"frames5A\":%u,\"uartBufferedBytes\":%u,\"lastFrameHex\":\"%s\","
      "\"probeSamples\":%u,\"probeLowSamples\":%u,\"probeEdges\":%u}",
      s.uart_on ? "true" : "false", s.link_ok ? "true" : "false", (unsigned)s.baud,
      s.pins_swapped ? "true" : "false", s.uart_inverted ? "true" : "false",
      (unsigned)s.uart_tx_pin, (unsigned)s.uart_rx_pin, txLevel, rxLevel,
      s.gpio_out ? "true" : "false", s.present ? "true" : "false",
      s.primary_valid ? "true" : "false", (unsigned)s.range_mm, (int)s.angle_deg, d, g,
      s.multi_valid ? "true" : "false", (unsigned)s.declared_obj_num, (unsigned)s.obj_num,
      s.truncated ? "true" : "false", v, (unsigned)s.rx_frames, (unsigned)s.rx_bytes,
      (unsigned)s.crc_err, (unsigned)s.malformed_frames, (unsigned)s.unknown_frames,
      (unsigned)s.discarded_bytes, (unsigned)s.dropped_bytes, (unsigned)s.frames_59,
      (unsigned)s.frames_5a, (unsigned)s.uart_buffered_bytes, s.last_frame_hex,
      (unsigned)s.probe_samples, (unsigned)s.probe_low_samples, (unsigned)s.probe_edges);
}

size_t radar_json_live(char *buf, size_t buflen) {
  RadarSnapshot s;
  radar_get_snapshot(s);
  char g[32], d[64], v[64];
  jsonEscape(s.gesture, g, sizeof(g));
  jsonEscape(s.det_text, d, sizeof(d));
  jsonEscape(s.version, v, sizeof(v));
  const int txLevel = gpio_get_level((gpio_num_t)s.uart_tx_pin);
  const int rxLevel = gpio_get_level((gpio_num_t)s.uart_rx_pin);

  size_t n = 0;
  int w = snprintf(
      buf, buflen,
      "{\"ok\":true,\"protocol\":\"at6010_hci_unconfirmed\",\"idStable\":false,"
      "\"uart\":%s,\"link\":%s,\"baud\":%u,\"gpioOut\":%s,\"present\":%s,"
      "\"primaryValid\":%s,\"isDetected\":%u,\"detResult\":%u,\"det\":\"%s\",\"gesture\":\"%s\","
      "\"range_mm\":%u,\"angle_deg\":%d,\"velo\":%d,\"rbConf\":%u,\"angleConf\":%u,"
      "\"frameIdx\":%u,\"reportType\":%u,\"br\":%u,\"hr\":%u,"
      "\"multiValid\":%s,\"declaredObjNum\":%u,\"objNum\":%u,\"truncated\":%s,"
      "\"version\":\"%s\",\"rxFrames\":%u,\"rxBytes\":%u,\"crcErr\":%u,"
      "\"malformedFrames\":%u,\"unknownFrames\":%u,\"discardedBytes\":%u,\"droppedBytes\":%u,"
      "\"frames59\":%u,\"frames5A\":%u,\"uartBufferedBytes\":%u,\"lastFrameHex\":\"%s\","
      "\"lastFrameUs\":%lld,\"lastPrimaryUs\":%lld,\"lastMultiUs\":%lld,"
      "\"probeSamples\":%u,\"probeLowSamples\":%u,\"probeEdges\":%u,"
      "\"pins\":{\"tx\":%u,\"rx\":%u,\"swap\":%s,\"invert\":%s,"
      "\"txLevel\":%d,\"rxLevel\":%d,\"out\":\"ENC3_A\"},"
      "\"objs\":[",
      s.uart_on ? "true" : "false", s.link_ok ? "true" : "false", (unsigned)s.baud,
      s.gpio_out ? "true" : "false", s.present ? "true" : "false",
      s.primary_valid ? "true" : "false", (unsigned)s.is_detected,
      (unsigned)s.det_result, d, g, (unsigned)s.range_mm, (int)s.angle_deg, (int)s.velo,
      (unsigned)s.rb_conf, (unsigned)s.angle_conf, (unsigned)s.frame_idx,
      (unsigned)s.report_type, (unsigned)s.br_val, (unsigned)s.hr_val,
      s.multi_valid ? "true" : "false", (unsigned)s.declared_obj_num, (unsigned)s.obj_num,
      s.truncated ? "true" : "false", v, (unsigned)s.rx_frames, (unsigned)s.rx_bytes,
      (unsigned)s.crc_err, (unsigned)s.malformed_frames, (unsigned)s.unknown_frames,
      (unsigned)s.discarded_bytes, (unsigned)s.dropped_bytes, (unsigned)s.frames_59,
      (unsigned)s.frames_5a, (unsigned)s.uart_buffered_bytes, s.last_frame_hex,
      (long long)s.last_frame_us, (long long)s.last_primary_us, (long long)s.last_multi_us,
      (unsigned)s.probe_samples, (unsigned)s.probe_low_samples, (unsigned)s.probe_edges,
      (unsigned)s.uart_tx_pin, (unsigned)s.uart_rx_pin,
      s.pins_swapped ? "true" : "false", s.uart_inverted ? "true" : "false", txLevel,
      rxLevel);
  if (w < 0) return 0;
  n = (size_t)w;
  for (uint8_t i = 0; i < s.obj_num && i < RADAR_OBJ_MAX && n + 80 < buflen; i++) {
    if (!s.objs[i].valid) continue;
    w = snprintf(buf + n, buflen - n, "%s{\"slot\":%u,\"range_mm\":%u,\"angle_deg\":%d,\"velo\":%d}",
                 (i ? "," : ""), (unsigned)i, (unsigned)s.objs[i].range_mm, (int)s.objs[i].angle_deg,
                 (int)s.objs[i].velo);
    if (w < 0) break;
    n += (size_t)w;
  }
  if (n + 16 >= buflen) return n;
  n += (size_t)snprintf(buf + n, buflen - n, "],\"trail\":[");
  for (uint8_t i = 0; i < s.trail_len && n + 64 < buflen; i++) {
    w = snprintf(buf + n, buflen - n, "%s{\"r\":%u,\"a\":%d}", (i ? "," : ""),
                 (unsigned)s.trail[i].range_mm, (int)s.trail[i].angle_deg);
    if (w < 0) break;
    n += (size_t)w;
  }
  if (n + 2 < buflen) {
    buf[n++] = ']';
    buf[n++] = '}';
    buf[n] = 0;
  }
  return n;
}
