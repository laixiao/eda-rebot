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
static bool s_mic_held = false;

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
  // MultiNet 伪唤醒（拼音空格分隔）；无 WakeNet
  if (esp_mn_commands_add(VOICE_SR_CMD_WAKE, "ni hao ai fei") != ESP_OK) return false;
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
  // 6000ms：唤醒后命令窗；未唤醒时 TIMEOUT 仅重置状态，继续听伪唤醒词
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
      ESP_LOGI(TAG, "cmd=%d str=%s prob=%.2f wake=%d", cmd, mn->string, mn->prob[0],
               s_wakeup);

      if (cmd == VOICE_SR_CMD_WAKE) {
        ESP_LOGI(TAG, "PSEUDO_WAKE 你好爱妃");
        setLast("已唤醒，请说开/关风扇");
        multinet->clean(model_data);
        s_wakeup = 1;
        if (s_cb) s_cb(0);  // 0 = 唤醒提示音
        continue;
      }

      if (s_wakeup != 1) {
        // 未唤醒时忽略开/关风扇，避免误控
        ESP_LOGI(TAG, "ignore cmd=%d (need wake)", cmd);
        continue;
      }

      if (cmd == VOICE_SR_CMD_FAN_ON)
        setLast("识别：开风扇");
      else if (cmd == VOICE_SR_CMD_FAN_OFF)
        setLast("识别：关风扇");
      else
        snprintf(s_last, sizeof(s_last), "识别：cmd=%d", cmd);
      if (s_cb && cmd > 0) s_cb(cmd);
      continue;
    }

    if (st == ESP_MN_STATE_TIMEOUT) {
      if (s_wakeup == 1) {
        ESP_LOGI(TAG, "mn timeout → wait pseudo-wake");
        setLast("待命：说「你好爱妃」");
        s_wakeup = 0;
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
