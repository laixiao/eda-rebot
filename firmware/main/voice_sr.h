#pragma once

#include <stddef.h>
#include <stdint.h>

/** 命令 ID（与 MultiNet esp_mn_commands_add 一致） */
enum VoiceSrCmd : int {
  VOICE_SR_CMD_FAN_ON = 1,
  VOICE_SR_CMD_FAN_OFF = 2,
  /** MultiNet 伪唤醒：「你好爱妃」（无 WakeNet） */
  VOICE_SR_CMD_WAKE = 3,
  VOICE_SR_CMD_FAN_UP = 4,    // 大一点 / 风速大一些 …
  VOICE_SR_CMD_FAN_DOWN = 5,  // 小一点 / 风速小一些 …
  VOICE_SR_CMD_FAN_MAX = 6,   // 最大风
  VOICE_SR_CMD_FAN_MIN = 7,   // 最小风
  VOICE_SR_CMD_FAN_MID = 8,   // 中等风 / 一半风
};

typedef void (*voice_sr_cmd_cb_t)(int cmd_id);

/** 启动板端唤醒+命令词；失败返回 false（缺模型/麦克风等不阻断整机）。 */
bool voice_sr_start(voice_sr_cmd_cb_t cb);

bool voice_sr_ok();

/** 内嵌 srmodels.bin 字节数（保证链接进大包，即使未 start）。 */
size_t voice_sr_model_bytes();

/** 录音/独占麦克风前暂停；停止后 resume。 */
void voice_sr_pause();
void voice_sr_resume();

/** 最近一次识别摘要（供 /api/status）。JSON 片段，无 enabled 字段。 */
void voice_sr_status(char *buf, size_t buflen);
