# EDA Robot Firmware (ESP-IDF)

ESP32-S3-WROOM-1-N16R8 局域网调试固件，原生 **ESP-IDF v5.5** 工程。

## 构建

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
| `main/` | 应用与驱动，FW **2.0.0** |
| `clients/` | Python 调试客户端 |

浏览器打开板子 IP，或 `GET /api`。
