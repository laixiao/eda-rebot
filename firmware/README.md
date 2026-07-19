# EDA Robot Firmware (ESP-IDF)

ESP32-S3-WROOM-1-N16R8 局域网调试固件，原生 **ESP-IDF v5.5** 工程。

## 构建

**烧录/OTA 前必须有 `main/wifi_config.h`**（真实 SSID/密码）。缺文件时编译直接失败，不会再用 example 的 `your-ssid`。

```bash
cp main/wifi_config.example.h main/wifi_config.h   # 仅首次
# 编辑 WIFI_SSID / WIFI_PASS
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build
# 烧录前确认：strings build/eda_robot.bin | grep your-ssid 应无输出
idf.py -p /dev/ttyUSB0 flash monitor
```

## 目录

| 路径 | 说明 |
|---|---|
| `main/` | 应用与驱动，FW **2.1.0**（含 Web OTA） |
| `clients/` | Python 调试客户端 |

浏览器打开板子 IP，或 `GET /api`。

## Web 烧录 (OTA)

首次仍需串口刷入（会写入双 OTA 分区表）。之后可在调试页 **Web 烧录** 上传 `build/eda_robot.bin`，或：

```bash
curl -X POST --data-binary @build/eda_robot.bin \
  -H 'Content-Type: application/octet-stream' \
  http://<板子IP>/api/ota
```

`GET /api/ota` 查看当前/下一分区。成功后自动重启。
