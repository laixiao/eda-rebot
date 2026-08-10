#include "voice_sr.h"
#include "board_i2s.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "voice_sr";

static voice_sr_cmd_cb_t s_cb = nullptr;
static volatile bool s_ok = false;
static volatile bool s_running = false;
static volatile bool s_paused = false;
static volatile bool s_feeding = false;
static volatile int s_wakeup = 0;

static const esp_afe_sr_iface_t *s_afe = nullptr;
static esp_afe_sr_data_t *s_afe_data = nullptr;
static srmodel_list_t *s_models = nullptr;

static char s_last[96] = "未启动";

static void setLast(const char *msg) {
  if (!msg) return;
  snprintf(s_last, sizeof(s_last), "%s", msg);
}

static bool loadCommands(esp_mn_iface_t *mn, model_iface_data_t *md) {
  esp_mn_commands_clear();
  // MultiNet6 中文：拼音空格分隔
  if (esp_mn_commands_add(VOICE_SR_CMD_FAN_ON, "kai feng shan") != ESP_OK) return false;
  if (esp_mn_commands_add(VOICE_SR_CMD_FAN_ON, "da kai feng shan") != ESP_OK) return false;
  if (esp_mn_commands_add(VOICE_SR_CMD_FAN_OFF, "guan feng shan") != ESP_OK) return false;
  if (esp_mn_commands_add(VOICE_SR_CMD_FAN_OFF, "guan bi feng shan") != ESP_OK) return false;
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
  model_iface_data_t *model_data = multinet->create(mn_name, 6000);
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
  multinet->print_active_speech_commands(model_data);

  const int afe_chunk = s_afe->get_fetch_chunksize(s_afe_data);
  const int mu_chunk = multinet->get_samp_chunksize(model_data);
  if (afe_chunk != mu_chunk) {
    ESP_LOGW(TAG, "chunk mismatch afe=%d mn=%d", afe_chunk, mu_chunk);
  }

  setLast("待命：说「你好小智」");
  ESP_LOGI(TAG, "voice ready — wake: 你好小智; cmds: 开/关风扇");

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

    if (res->wakeup_state == WAKENET_DETECTED) {
      ESP_LOGI(TAG, "WAKEWORD");
      setLast("已唤醒，请说开/关风扇");
      multinet->clean(model_data);
      s_wakeup = 1;
      if (s_cb) s_cb(0);  // 0 = 唤醒提示（主程序可 beep）
    } else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
      s_wakeup = 1;
      multinet->clean(model_data);
    }

    if (s_wakeup != 1) continue;

    esp_mn_state_t st = multinet->detect(model_data, res->data);
    if (st == ESP_MN_STATE_DETECTING) continue;

    if (st == ESP_MN_STATE_DETECTED) {
      esp_mn_results_t *mn = multinet->get_results(model_data);
      if (mn && mn->num > 0) {
        const int cmd = mn->command_id[0];
        ESP_LOGI(TAG, "cmd=%d str=%s prob=%.2f", cmd, mn->string, mn->prob[0]);
        if (cmd == VOICE_SR_CMD_FAN_ON)
          setLast("识别：开风扇");
        else if (cmd == VOICE_SR_CMD_FAN_OFF)
          setLast("识别：关风扇");
        else
          snprintf(s_last, sizeof(s_last), "识别：cmd=%d", cmd);
        if (s_cb && cmd > 0) s_cb(cmd);
      }
      // 单次命令后继续听，直到超时；保持唤醒以便连说
      continue;
    }

    if (st == ESP_MN_STATE_TIMEOUT) {
      ESP_LOGI(TAG, "mn timeout → wait wake");
      setLast("待命：说「你好小智」");
      s_afe->enable_wakenet(s_afe_data);
      s_wakeup = 0;
    }
  }

  multinet->destroy(model_data);
  vTaskDelete(nullptr);
}

bool voice_sr_ok() { return s_ok; }

void voice_sr_pause() {
  s_paused = true;
  for (int i = 0; i < 50 && s_feeding; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

void voice_sr_resume() { s_paused = false; }

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

  s_models = esp_srmodel_init("model");
  if (!s_models) {
    setLast("模型分区加载失败");
    ESP_LOGE(TAG, "esp_srmodel_init(model) failed — flash model partition?");
    return false;
  }

  afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (!afe_cfg) {
    setLast("AFE 配置失败");
    return false;
  }
  // 单麦板：关闭 SE，省算力
  afe_cfg->aec_init = false;
  afe_cfg->se_init = false;

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
  if (xTaskCreatePinnedToCore(feedTask, "sr_feed", 8 * 1024, nullptr, 5, nullptr, 0) != pdPASS ||
      xTaskCreatePinnedToCore(detectTask, "sr_det", 8 * 1024, nullptr, 5, nullptr, 1) != pdPASS) {
    s_running = false;
    setLast("任务创建失败");
    return false;
  }

  s_ok = true;
  setLast("待命：说「你好小智」");
  return true;
}
