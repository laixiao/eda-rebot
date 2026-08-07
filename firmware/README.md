# EDA Robot Firmware (ESP-IDF) — v6-1

ESP32-S3-WROOM-1-N16R8 局域网调试固件，对应原理图 **AI通用机器人_v6-1**。

## 构建

**烧录/OTA 前必须有 `main/wifi_config.h`**（真实 SSID/密码）。

```bash
cp main/wifi_config.example.h main/wifi_config.h   # 仅首次
# 编辑 WIFI_SSID / WIFI_PASS
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## 本板能力（FW 3.0.0）

| 模块 | 说明 | API |
|---|---|---|
| I2C | XL9555 `0x20`、OLED `0x3C`、PCA9685 `0x40` | `/api/status` |
| 舵机 T3/T4 | U16 LED11/12；先 `/api/pwm?on=1` | `/api/servo` |
| 探照灯 | U16 LED1/2/0 → MOSFET；LED_ALL 为公共地 | `/api/led` |
| 雷达 MS60 | UART IO9/10；供电 XL IO0_1；OUT→IO0_0 | `/api/radar` `power`/`on` |
| 麦/功放 | I2S；功放 SD→XL IO1_6 | `/api/mic` `/api/amp` `/api/beep` |
| OLED | | `/api/oled` |
| OTA | 双分区 | `/api/ota` |

未焊接的 I2C 设备会在状态里显示失败，其余功能仍可测。

已移除（v5）：摄像头、SPI 屏、电机、编码器、第二路 PCA9685。

## 雷达

1. `POST /api/radar {"power":true}` — 打开 Q4 供电  
2. `POST /api/radar {"on":true}` — 开始采集  
3. 浏览器 `/radar` 或 `GET /api/radar/live`

## Web 烧录

```bash
curl -X POST --data-binary @build/eda_robot.bin \
  -H 'Content-Type: application/octet-stream' \
  http://<板子IP>/api/ota
```
