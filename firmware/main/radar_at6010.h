#pragma once

#include <stdint.h>
#include <stddef.h>

// MS60 UART passive monitor plus experimental AIR Touch AT6010 HCI diagnostics.

static const int RADAR_TRAIL_MAX = 48;
static const int RADAR_OBJ_MAX = 3;

struct RadarTarget {
  uint16_t range_mm;
  int16_t angle_deg;
  int16_t velo;
  uint8_t rb_conf;
  uint8_t angle_conf;
  bool valid;
};

struct RadarTrailPoint {
  uint16_t range_mm;
  int16_t angle_deg;
  int64_t t_us;
};

struct RadarSnapshot {
  bool uart_on;
  bool link_ok;
  uint32_t baud;
  bool pins_swapped;
  bool uart_inverted;
  uint8_t uart_tx_pin;
  uint8_t uart_rx_pin;
  bool gpio_out;
  bool present;  // is_detected || gpio_out
  uint8_t is_detected;
  uint8_t det_result;
  uint16_t range_mm;
  int16_t angle_deg;
  int16_t velo;
  uint8_t rb_conf;
  uint8_t angle_conf;
  uint32_t frame_idx;
  uint8_t report_type;  // last 0x5A TYPE, 0xFF=none / poll
  bool primary_valid;
  int64_t last_primary_us;
  uint32_t declared_obj_num;
  uint8_t obj_num;
  bool multi_valid;
  bool truncated;
  int64_t last_multi_us;
  RadarTarget objs[RADAR_OBJ_MAX];
  uint8_t br_val;
  uint8_t hr_val;
  char gesture[24];  // UTF-8 short label
  char det_text[48];
  char version[48];
  uint32_t rx_frames;
  uint32_t rx_bytes;
  uint32_t crc_err;
  uint32_t malformed_frames;
  uint32_t unknown_frames;
  uint32_t discarded_bytes;
  uint32_t dropped_bytes;
  uint32_t frames_59;
  uint32_t frames_5a;
  uint32_t type_frames[8];
  uint32_t uart_buffered_bytes;
  char last_frame_hex[161];
  uint32_t probe_samples;
  uint32_t probe_low_samples;
  uint32_t probe_edges;
  int64_t last_frame_us;
  int64_t last_out_us;
  uint8_t trail_len;
  RadarTrailPoint trail[RADAR_TRAIL_MAX];
};

bool radar_init();
bool radar_start(uint32_t baud = 115200, bool swap_pins = false, bool invert_uart = false);
void radar_stop();
bool radar_running();
void radar_set_gpio_out(bool level);
void radar_poll();  // call from bg task
bool radar_probe_rx_edges(uint32_t duration_ms = 100);
bool radar_cmd_set_active_time(uint8_t seconds);
bool radar_cmd_sense(bool on);
bool radar_cmd_get_version();
bool radar_cmd_get_det();
bool radar_cmd_set_baud(uint32_t baud);
void radar_get_snapshot(RadarSnapshot &out);
size_t radar_json_summary(char *buf, size_t buflen);
size_t radar_json_live(char *buf, size_t buflen);
