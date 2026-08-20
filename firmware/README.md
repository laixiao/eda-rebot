# EDA Robot Firmware (ESP-IDF) — v5 + FAN

ESP32-S3-WROOM-1-N16R8 局域网调试固件，对应原理图 **AI通用机器人_v5**（固件含 FAN 语音/风扇能力）。

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

若缺少 `main/font_cjk.bin`（约 3800 字 OLED 字库），在 `firmware/` 下执行：

```bash
python scripts/pack_font_cjk.py
```

然后重新 `idf.py build`。
## 本板能力（FW 3.4.2）

| 模块 | 说明 | API |
|---|---|---|
| I2C | XL9555 `0x20`、OLED `0x3C`、PCA9685 `0x40` | `/api/status` |
| 舵机 T3/T4 | U16 LED11/12；先 `/api/pwm?on=1` | `/api/servo` |
| 探照灯 | U16 LED1/2/0 → MOSFET；LED_ALL 为公共地 | `/api/led` |
| 雷达 MS60 | UART IO9/10 飞线；OUT→XL IO0_0；power 软开关 | `/api/radar` |
| 录音/扬声器 | I2S；功放 SD→XL IO1_6；数字音量 0..100 | `/api/rec` `/api/play` `/api/play/upload` `/api/amp` `/api/beep` |
| OLED | | `/api/oled` |
| OTA | 双分区 | `/api/ota` |

未焊接的 I2C 设备会在状态里显示失败，其余功能仍可测。

本树保留 v5 外设（摄像头/SPI 屏/电机/编码器/U23），并含 FAN 语音与风扇联动。

## 雷达

1. `POST /api/radar {"power":true}` — v5 为软开关（飞线常供电，无 Q4）；开后自动 UART 查询
2. **每日时刻**（WiFi 获 IP 后 SNTP 校时，北京时间 CST-8）：
   - `POST /api/radar {"scheduleEnable":true,"scheduleOn":"08:00","scheduleOff":"22:00"}`
   - 空串 `scheduleOn`/`scheduleOff` 清除该侧；未校时前不执行
   - 网页开关与时刻改完即生效（无保存按钮）
3. `GET /api/radar` 含 `timeSynced`、`localTime`、`schedule`、`ignore`；`GET /api/status` 含校时状态
4. **忽略扇区**（默认关；默认可有一段 0°～-60°；最多 8 段，NVS 持久化）：
   - 总开关：`POST /api/radar {"ignoreEnable":true}`
   - 添加：`POST /api/radar {"ignoreAdd":true,"ignoreFrom":0,"ignoreTo":-60}`
   - 删除：`POST /api/radar {"ignoreDel":0}`（按下标）
   - 改某一段：`POST /api/radar {"ignoreId":0,"ignoreFrom":10,"ignoreTo":20}`
   - 清空：`POST /api/radar {"ignoreClear":true}`
   - 旧写法 `ignoreFrom`/`ignoreTo` 仍改第 0 段
   - 开后这些扇区目标不参与控风扇 / 手势切档；画布仍显示并标红

## Web 烧录

```bash
curl -X POST --data-binary @build/eda_robot.bin \
  -H 'Content-Type: application/octet-stream' \
  http://<板子IP>/api/ota
```
