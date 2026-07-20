#pragma once

#include <stdint.h>
#include <stddef.h>

// AT6010 HCI / MS60-1211S80M UART 解析与状态（协议默认 921600 8N1）

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
  uint8_t obj_num;
  RadarTarget objs[RADAR_OBJ_MAX];
  uint8_t br_val;
  uint8_t hr_val;
  char gesture[24];  // UTF-8 short label
  char det_text[48];
  char version[48];
  uint32_t rx_frames;
  uint32_t rx_bytes;
  uint32_t crc_err;
  int64_t last_frame_us;
  int64_t last_out_us;
  uint8_t trail_len;
  RadarTrailPoint trail[RADAR_TRAIL_MAX];
};

bool radar_init();
bool radar_start(uint32_t baud = 921600);
void radar_stop();
bool radar_running();
void radar_set_gpio_out(bool level);
void radar_poll();  // call from bg task
bool radar_cmd_sense(bool on);
bool radar_cmd_get_version();
bool radar_cmd_get_det();
bool radar_cmd_set_baud(uint32_t baud);
void radar_get_snapshot(RadarSnapshot &out);
size_t radar_json_summary(char *buf, size_t buflen);
size_t radar_json_live(char *buf, size_t buflen);
