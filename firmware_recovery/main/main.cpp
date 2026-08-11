#include <stdio.h>
#include <string.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "board_config.h"
#include "board_i2c.h"
#include "ssd1306.h"

static const char *TAG = "rescue";
static const char *FW_VERSION = "R1.0.0";

static SSD1306 oled;
static char ipStr[16] = {0};
static bool wifiOk = false;
static httpd_handle_t server = nullptr;
static volatile bool otaBusy = false;

/** 1=用户主动进入；0=异常/开机回退到救援 */
static int rescueReason = 0;

static void oledSteps(const char *extra = nullptr) {
  if (!oled.present()) return;
  char l0[32], l1[32], l2[32], l3[32];
  if (rescueReason == 1)
    snprintf(l0, sizeof(l0), "救援升级");
  else
    snprintf(l0, sizeof(l0), "异常进救援");
  if (wifiOk && ipStr[0])
    snprintf(l1, sizeof(l1), "%s", ipStr);
  else
    snprintf(l1, sizeof(l1), "连WiFi中...");
  snprintf(l2, sizeof(l2), "1同WiFi 2开网页");
  if (extra && extra[0])
    snprintf(l3, sizeof(l3), "%s", extra);
  else
    snprintf(l3, sizeof(l3), "3上传大包固件");
  oled.printfLines(l0, l1, l2, l3);
}

static void addCors(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t sendJson(httpd_req_t *req, int code, const char *body) {
  addCors(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, code == 200 ? "200 OK" : "500 Internal Server Error");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static const char PAGE[] = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>救援升级</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 12px;background:#0d1117;color:#e6edf3}
h1{font-size:20px;margin:0 0 8px}
.box{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px;margin:12px 0}
.step{margin:6px 0;line-height:1.5}
button{background:#238636;color:#fff;border:0;padding:10px 16px;border-radius:6px;font-size:15px}
button:disabled{opacity:.5}
pre{white-space:pre-wrap;font-size:12px;color:#8b949e}
.bar{height:8px;background:#21262d;border-radius:4px;overflow:hidden;margin-top:8px}
.bar>i{display:block;height:100%;width:0;background:#3fb950}
</style></head><body>
<h1>救援模式 · 升级主系统</h1>
<div class="box">
  <div class="step">1. 手机/电脑连<strong>同一 WiFi</strong></div>
  <div class="step">2. 打开本页（当前就是）</div>
  <div class="step">3. 选择主固件 <code>eda_robot.bin</code>（大包，含语音模型）</div>
  <div class="step">4. 点上传，等待完成并自动重启</div>
</div>
<div class="box">
  <input id=f type=file accept=.bin />
  <p><button id=b onclick="go()">上传并烧录到主系统</button></p>
  <div class="bar"><i id=bar></i></div>
  <pre id=log>等待选择文件…</pre>
</div>
<script>
async function go(){
  const file=document.getElementById('f').files[0];
  const log=document.getElementById('log');
  const bar=document.getElementById('bar');
  const btn=document.getElementById('b');
  if(!file){alert('请先选择 .bin');return}
  btn.disabled=true; log.textContent='上传中…'; bar.style.width='0';
  try{
    await new Promise((resolve,reject)=>{
      const x=new XMLHttpRequest();
      x.open('POST','/api/ota');
      x.setRequestHeader('Content-Type','application/octet-stream');
      x.upload.onprogress=e=>{
        if(e.lengthComputable){
          const p=Math.round(e.loaded*100/e.total);
          bar.style.width=p+'%'; log.textContent='上传 '+p+'%';
        }
      };
      x.onload=()=>{
        let j={}; try{j=JSON.parse(x.responseText)}catch(e){}
        if(x.status>=200&&x.status<300&&j.ok!==false){
          log.textContent='成功，即将重启到主系统…'; resolve();
        } else reject(new Error((j&&j.error)||x.responseText||('HTTP '+x.status)));
      };
      x.onerror=()=>reject(new Error('网络错误'));
      x.send(file);
    });
  }catch(e){ log.textContent='失败: '+e.message; alert(e.message); btn.disabled=false; }
}
</script></body></html>)HTML";

static esp_err_t handleRoot(httpd_req_t *req) {
  addCors(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handleOta(httpd_req_t *req) {
  if (req->method == HTTP_OPTIONS) {
    addCors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
  }
  if (otaBusy) return sendJson(req, 500, "{\"ok\":false,\"error\":\"ota busy\"}");
  otaBusy = true;
  oledSteps("正在升级...");

  const esp_partition_t *update = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!update) {
    otaBusy = false;
    oledSteps("无ota_0分区");
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"no ota_0 partition\"}");
  }

  esp_ota_handle_t ota = 0;
  esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
  if (err != ESP_OK) {
    otaBusy = false;
    oledSteps("ota begin失败");
    char b[96];
    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"begin %s\"}", esp_err_to_name(err));
    return sendJson(req, 500, b);
  }

  std::string buf;
  buf.resize(4096);
  int remaining = req->content_len;
  while (remaining > 0) {
    const int want = remaining > (int)buf.size() ? (int)buf.size() : remaining;
    const int got = httpd_req_recv(req, buf.data(), want);
    if (got <= 0) {
      esp_ota_abort(ota);
      otaBusy = false;
      oledSteps("接收中断");
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"recv failed\"}");
    }
    err = esp_ota_write(ota, buf.data(), got);
    if (err != ESP_OK) {
      esp_ota_abort(ota);
      otaBusy = false;
      oledSteps("写入失败");
      return sendJson(req, 500, "{\"ok\":false,\"error\":\"write failed\"}");
    }
    remaining -= got;
  }

  err = esp_ota_end(ota);
  if (err != ESP_OK) {
    otaBusy = false;
    oledSteps("镜像校验失败");
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"bad image\"}");
  }
  err = esp_ota_set_boot_partition(update);
  if (err != ESP_OK) {
    otaBusy = false;
    oledSteps("设置启动失败");
    return sendJson(req, 500, "{\"ok\":false,\"error\":\"set boot failed\"}");
  }

  // 清掉「主动进救援」标记
  nvs_handle_t h;
  if (nvs_open("rescue", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "enter");
    nvs_commit(h);
    nvs_close(h);
  }

  addCors(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", HTTPD_RESP_USE_STRLEN);
  oledSteps("升级成功重启");
  vTaskDelay(pdMS_TO_TICKS(800));
  esp_restart();
  return ESP_OK;
}

static void setupHttp() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 120;
  config.max_uri_handlers = 8;
  // LWIP_MAX_SOCKETS 较小时，默认 max_open_sockets 会超限
  config.max_open_sockets = 4;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    oledSteps("网页启动失败");
    return;
  }
  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handleRoot, .user_ctx = nullptr};
  httpd_uri_t ota = {.uri = "/api/ota", .method = HTTP_POST, .handler = handleOta, .user_ctx = nullptr};
  httpd_uri_t otaOpt = {.uri = "/api/ota", .method = HTTP_OPTIONS, .handler = handleOta, .user_ctx = nullptr};
  httpd_register_uri_handler(server, &root);
  httpd_register_uri_handler(server, &ota);
  httpd_register_uri_handler(server, &otaOpt);
  ESP_LOGI(TAG, "rescue HTTP :80");
}

static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifiOk = false;
    ipStr[0] = 0;
    oledSteps();
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *event = (ip_event_got_ip_t *)data;
    snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&event->ip_info.ip));
    wifiOk = true;
    ESP_LOGI(TAG, "IP %s", ipStr);
    oledSteps();
  }
}

static void wifi_init() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
  wifi_config_t wifi_config = {};
  strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_start());
}

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  nvs_handle_t h;
  uint8_t enter = 0;
  if (nvs_open("rescue", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u8(h, "enter", &enter);
    nvs_close(h);
  }
  rescueReason = enter ? 1 : 0;

  ESP_LOGI(TAG, "=== RESCUE %s reason=%s ===", FW_VERSION, rescueReason ? "user" : "abnormal");

  board_i2c_init();
  oled.begin(0x3C, 100000);
  if (!oled.present()) oled.begin(0x3D, 100000);
  oledSteps("启动中...");

  wifi_init();
  for (int i = 0; i < 80 && !wifiOk; i++) vTaskDelay(pdMS_TO_TICKS(250));
  if (!wifiOk) oledSteps("WiFi失败检查热点");
  else oledSteps();

  setupHttp();
}
