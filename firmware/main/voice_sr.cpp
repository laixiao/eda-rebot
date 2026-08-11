#include "voice_sr.h"
#include "board_i2s.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "voice_sr";

/** MultiNet 灵敏度：偏易唤醒；靠冷却抑制风扇误触，不靠高门槛卡真人 */
static constexpr float kMnDetThreshold = 0.28f;      // 模型侧（越低越灵敏）
static constexpr float kWakeMinProb = 0.25f;         // 伪唤醒软门槛
static constexpr float kCmdMinProb = 0.22f;          // 开/关/调风量
static constexpr int64_t kWakeCooldownUs = 2500000;  // 播报后/开风扇后约 2.5s 不接受伪唤醒
static constexpr int kMnCmdTimeoutMs = 10000;        // 唤醒后命令窗（含播报后继续听）

static voice_sr_cmd_cb_t s_cb = nullptr;
static volatile bool s_ok = false;
static volatile bool s_running = false;
static volatile bool s_paused = false;
static volatile bool s_feeding = false;
static volatile int s_wakeup = 0;
static bool s_mic_held = false;
static int64_t s_wake_cooldown_until_us = 0;

static const esp_afe_sr_iface_t *s_afe = nullptr;
static esp_afe_sr_data_t *s_afe_data = nullptr;
static srmodel_list_t *s_models = nullptr;

static char s_last[96] = "未启动";

static void setLast(const char *msg) {
  if (!msg) return;
  snprintf(s_last, sizeof(s_last), "%s", msg);
}

static void armWakeCooldown(int64_t extra_us) {
  const int64_t until = esp_timer_get_time() + extra_us;
  if (until > s_wake_cooldown_until_us) s_wake_cooldown_until_us = until;
}

static bool addMn(int id, const char *py) {
  if (esp_mn_commands_add(id, py) != ESP_OK) {
    ESP_LOGE(TAG, "mn add fail id=%d '%s'", id, py);
    return false;
  }
  return true;
}

static bool loadCommands(esp_mn_iface_t *mn, model_iface_data_t *md) {
  esp_mn_commands_clear();
  // 伪唤醒额外别名，降低必须一字不差的压力
  if (!addMn(VOICE_SR_CMD_WAKE, "ni hao ai fei")) return false;
  if (!addMn(VOICE_SR_CMD_WAKE, "ai fei ai fei")) return false;

  // 开 / 关
  if (!addMn(VOICE_SR_CMD_FAN_ON, "kai feng shan")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_ON, "da kai feng shan")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_OFF, "guan feng shan")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_OFF, "guan bi feng shan")) return false;

  // 大一点（冗余）
  if (!addMn(VOICE_SR_CMD_FAN_UP, "da yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "da yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "feng da yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "feng da yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "feng su da yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "feng su da yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "feng liang da yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "feng liang da yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "jia da feng su")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "feng su jia da")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_UP, "jia kuai")) return false;

  // 小一点（冗余）
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "xiao yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "xiao yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "feng xiao yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "feng xiao yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "feng su xiao yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "feng su xiao yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "feng liang xiao yi dian")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "feng liang xiao yi xie")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "jian xiao feng su")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "feng su jian xiao")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_DOWN, "jian man")) return false;

  // 最大 / 最小 / 中等
  if (!addMn(VOICE_SR_CMD_FAN_MAX, "zui da feng")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MAX, "feng su zui da")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MAX, "zui da feng su")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MAX, "feng liang zui da")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MIN, "zui xiao feng")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MIN, "feng su zui xiao")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MIN, "zui xiao feng su")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MIN, "feng liang zui xiao")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MID, "zhong deng feng")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MID, "zhong deng feng su")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MID, "yi ban feng")) return false;
  if (!addMn(VOICE_SR_CMD_FAN_MID, "feng su zhong deng")) return false;

  esp_mn_error_t *err = esp_mn_commands_update();
  if (err) {
    ESP_LOGE(TAG, "mn commands update failed");
    return false;
  }
  (void)mn;
  (void)md;
  return true;
}

static void feedTask(void *) {
  const int chunk = s_afe->get_feed_chunksize(s_afe_data);
  const int nch = s_afe->get_feed_channel_num(s_afe_data);
  int16_t *buf = (int16_t *)heap_caps_malloc((size_t)chunk * (size_t)nch * sizeof(int16_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) {
    ESP_LOGE(TAG, "feed buf OOM");
    vTaskDelete(nullptr);
    return;
  }

  while (s_running) {
    if (s_paused) {
      s_feeding = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    s_feeding = true;
    size_t got = 0;
    // AFE 单麦：一次喂 chunk 个样点
    if (!board_i2s_mic_read_pcm16(buf, (size_t)chunk, &got) || got < (size_t)chunk) {
      // 读不满则补零，避免 AFE 饿死
      if (got < (size_t)chunk) memset(buf + got, 0, ((size_t)chunk - got) * sizeof(int16_t));
    }
    if (nch > 1) {
      // 本板单麦；若 AFE 要多通道则复制到其余通道（少见）
      for (int c = 1; c < nch; c++) {
        memcpy(buf + c * chunk, buf, (size_t)chunk * sizeof(int16_t));
      }
    }
    s_afe->feed(s_afe_data, buf);
  }
  s_feeding = false;
  free(buf);
  vTaskDelete(nullptr);
}

static void detectTask(void *) {
  char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  if (!mn_name) {
    setLast("无中文 MultiNet");
    ESP_LOGE(TAG, "no Chinese MultiNet model");
    vTaskDelete(nullptr);
    return;
  }
  ESP_LOGI(TAG, "multinet=%s", mn_name);
  esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
  if (!multinet) {
    setLast("MultiNet 句柄失败");
    vTaskDelete(nullptr);
    return;
  }
  // 命令窗加长：唤醒播报约 3s，之后仍有时间说开/关/调风
  model_iface_data_t *model_data = multinet->create(mn_name, kMnCmdTimeoutMs);
  if (!model_data) {
    setLast("MultiNet create 失败");
    vTaskDelete(nullptr);
    return;
  }
  if (!loadCommands(multinet, model_data)) {
    setLast("命令词装载失败");
    multinet->destroy(model_data);
    vTaskDelete(nullptr);
    return;
  }
  if (multinet->set_det_threshold) {
    multinet->set_det_threshold(model_data, kMnDetThreshold);
    ESP_LOGI(TAG, "mn det_threshold=%.2f wake_min_prob=%.2f", kMnDetThreshold, kWakeMinProb);
  }
  multinet->print_active_speech_commands(model_data);

  const int afe_chunk = s_afe->get_fetch_chunksize(s_afe_data);
  const int mu_chunk = multinet->get_samp_chunksize(model_data);
  if (afe_chunk != mu_chunk) {
    ESP_LOGW(TAG, "chunk mismatch afe=%d mn=%d", afe_chunk, mu_chunk);
  }

  setLast("待命：说「你好爱妃」");
  ESP_LOGI(TAG, "voice ready — pseudo-wake: 你好爱妃; cmds: 开/关风扇");

  while (s_running) {
    if (s_paused) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
    if (!res || res->ret_value == ESP_FAIL) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (!res->data) continue;

    // 无 WakeNet：始终跑 MultiNet；伪唤醒词与命令词共用检测
    esp_mn_state_t st = multinet->detect(model_data, res->data);
    if (st == ESP_MN_STATE_DETECTING) continue;

    if (st == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t *mn = multinet->get_results(model_data);
      if (!mn || mn->num <= 0) continue;
      const int cmd = mn->command_id[0];
      const float prob = mn->prob[0];
      ESP_LOGI(TAG, "cmd=%d str=%s prob=%.2f wake=%d", cmd, mn->string, prob, s_wakeup);

      if (cmd == VOICE_SR_CMD_WAKE) {
        // 已在命令窗：忽略再次伪唤醒（风扇噪声常见）
        if (s_wakeup == 1) {
          ESP_LOGI(TAG, "ignore wake (already listening cmds)");
          multinet->clean(model_data);
          continue;
        }
        const int64_t now = esp_timer_get_time();
        if (now < s_wake_cooldown_until_us) {
          ESP_LOGI(TAG, "ignore wake (cooldown %.1fs left, prob=%.2f)",
                   (s_wake_cooldown_until_us - now) / 1e6f, prob);
          multinet->clean(model_data);
          continue;
        }
        if (prob < kWakeMinProb) {
          ESP_LOGI(TAG, "reject wake low prob=%.2f < %.2f", prob, kWakeMinProb);
          multinet->clean(model_data);
          continue;
        }

        ESP_LOGI(TAG, "PSEUDO_WAKE 你好爱妃 prob=%.2f str=%s", prob, mn->string);
        setLast("已唤醒：开/关/大一点/小一点/最大/最小/中等");
        multinet->clean(model_data);
        s_wakeup = 1;
        if (s_cb) s_cb(0);  // 播放回复（内部 pause）；返回后再冷却
        armWakeCooldown(kWakeCooldownUs);
        multinet->clean(model_data);
        continue;
      }

      if (s_wakeup != 1) {
        // 未唤醒时忽略开/关风扇，避免误控
        ESP_LOGI(TAG, "ignore cmd=%d (need wake)", cmd);
        continue;
      }

      if (prob < kCmdMinProb) {
        ESP_LOGI(TAG, "reject cmd=%d low prob=%.2f", cmd, prob);
        continue;
      }

      if (cmd == VOICE_SR_CMD_FAN_ON) {
        if (strstr(mn->string, "da kai"))
          setLast("识别：打开风扇");
        else
          setLast("识别：开风扇");
      } else if (cmd == VOICE_SR_CMD_FAN_OFF) {
        if (strstr(mn->string, "guan bi"))
          setLast("识别：关闭风扇");
        else
          setLast("识别：关风扇");
      } else if (cmd == VOICE_SR_CMD_FAN_UP) {
        setLast("识别：风量大一点");
      } else if (cmd == VOICE_SR_CMD_FAN_DOWN) {
        setLast("识别：风量小一点");
      } else if (cmd == VOICE_SR_CMD_FAN_MAX) {
        setLast("识别：最大风");
      } else if (cmd == VOICE_SR_CMD_FAN_MIN) {
        setLast("识别：最小风");
      } else if (cmd == VOICE_SR_CMD_FAN_MID) {
        setLast("识别：中等风");
      } else {
        snprintf(s_last, sizeof(s_last), "识别：cmd=%d", cmd);
      }
      if (s_cb && cmd > 0) s_cb(cmd);
      // 开风扇/加大后噪声大，拉长伪唤醒冷却
      if (cmd == VOICE_SR_CMD_FAN_ON || cmd == VOICE_SR_CMD_FAN_UP || cmd == VOICE_SR_CMD_FAN_MAX ||
          cmd == VOICE_SR_CMD_FAN_MID)
        armWakeCooldown(kWakeCooldownUs);
      continue;
    }

    if (st == ESP_MN_STATE_TIMEOUT) {
      if (s_wakeup == 1) {
        ESP_LOGI(TAG, "mn timeout → wait pseudo-wake");
        setLast("待命：说「你好爱妃」");
        s_wakeup = 0;
        // 刚退出命令窗时风扇可能仍在转，短暂不接受伪唤醒
        armWakeCooldown(1500000);
      }
      multinet->clean(model_data);
    }
  }

  multinet->destroy(model_data);
  vTaskDelete(nullptr);
}

bool voice_sr_ok() { return s_ok; }

size_t voice_sr_model_bytes() {
  extern const uint8_t srmodels_bin_start[] asm("_binary_srmodels_bin_start");
  extern const uint8_t srmodels_bin_end[] asm("_binary_srmodels_bin_end");
  return (size_t)(+srmodels_bin_end - +srmodels_bin_start);
}

void voice_sr_pause() {
  s_paused = true;
  for (int i = 0; i < 50 && s_feeding; i++) vTaskDelay(pdMS_TO_TICKS(10));
  if (s_mic_held) {
    board_i2s_mic_release();
    s_mic_held = false;
  }
}

void voice_sr_resume() {
  if (s_ok && s_running && !s_mic_held) {
    if (board_i2s_mic_acquire()) s_mic_held = true;
  }
  s_paused = false;
}

void voice_sr_status(char *buf, size_t buflen) {
  if (!buf || buflen == 0) return;
  snprintf(buf, buflen,
           "{\"ok\":%s,\"paused\":%s,\"listening\":%s,\"last\":\"%s\"}",
           s_ok ? "true" : "false", s_paused ? "true" : "false",
           (s_wakeup == 1) ? "true" : "false", s_last);
}

bool voice_sr_start(voice_sr_cmd_cb_t cb) {
  if (s_ok) return true;
  if (!board_i2s_ready()) {
    setLast("I2S 未就绪");
    return false;
  }
  s_cb = cb;

  // 模型打进 app（srmodels.bin embed），随 14MB 大包 OTA，无需独立 model 分区
  extern const uint8_t srmodels_bin_start[] asm("_binary_srmodels_bin_start");
  extern const uint8_t srmodels_bin_end[] asm("_binary_srmodels_bin_end");
  const size_t srmodels_sz = (size_t)(+srmodels_bin_end - +srmodels_bin_start);
  if (srmodels_sz < 64) {
    setLast("内嵌模型为空");
    ESP_LOGE(TAG, "embedded srmodels.bin empty");
    return false;
  }
  s_models = srmodel_load(srmodels_bin_start);
  if (!s_models) {
    setLast("内嵌模型加载失败");
    ESP_LOGE(TAG, "srmodel_load(embedded) failed");
    return false;
  }
  ESP_LOGI(TAG, "srmodels embedded size=%u", (unsigned)srmodels_sz);

  afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (!afe_cfg) {
    setLast("AFE 配置失败");
    return false;
  }
  // 单麦板：关闭 SE；伪唤醒不跑 WakeNet，省算力、避免「你好小智」误导
  afe_cfg->aec_init = false;
  afe_cfg->se_init = false;
  afe_cfg->wakenet_init = false;

  s_afe = esp_afe_handle_from_config(afe_cfg);
  s_afe_data = s_afe ? s_afe->create_from_config(afe_cfg) : nullptr;
  afe_config_free(afe_cfg);
  if (!s_afe || !s_afe_data) {
    setLast("AFE 创建失败");
    ESP_LOGE(TAG, "AFE create failed");
    return false;
  }

  s_running = true;
  s_paused = false;
  if (!board_i2s_mic_acquire()) {
    s_running = false;
    setLast("麦克风占用失败");
    return false;
  }
  s_mic_held = true;
  if (xTaskCreatePinnedToCore(feedTask, "sr_feed", 8 * 1024, nullptr, 5, nullptr, 0) != pdPASS ||
      xTaskCreatePinnedToCore(detectTask, "sr_det", 8 * 1024, nullptr, 5, nullptr, 1) != pdPASS) {
    s_running = false;
    if (s_mic_held) {
      board_i2s_mic_release();
      s_mic_held = false;
    }
    setLast("任务创建失败");
    return false;
  }

  s_ok = true;
  setLast("待命：说「你好爱妃」");
  return true;
}
