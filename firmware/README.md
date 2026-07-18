# EDA Robot Firmware (ESP-IDF)

ESP32-S3-WROOM-1-N16R8 局域网调试固件，原生 **ESP-IDF v5.5** 工程。

## 构建

Wi-Fi 参数位于已忽略的 `main/wifi_config.h`；新环境可参考 `main/wifi_config.example.h`。

```bash
. $IDF_PATH/export.sh   # 或 source ~/.../esp-idf/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 目录

| 路径 | 说明 |
|---|---|
| `main/` | 应用与驱动，FW **2.1.0** |
| `clients/` | Python 调试客户端 |

浏览器打开板子 IP，或 `GET /api`。

状态查询使用 GET，执行器和显示控制使用 POST。电机命令超过 1.5 秒未刷新时会自动关闭 STBY。
