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
| `main/` | 应用与驱动，FW **2.2.7**（含 Web OTA、雷达自动查询、急停、深度睡眠关机） |
| `clients/` | Python 调试客户端 |

浏览器打开板子 IP，或 `GET /api`。

调试页的“紧急停止”仅关闭执行器输出；“关机”会先急停、关闭外设和 Wi-Fi，再进入深度睡眠。板上没有 MCU 可控的主电源锁存，因此不会切断电池，需断电重上电或按 EN 复位恢复。

## Web 烧录 (OTA)

首次仍需串口刷入（会写入双 OTA 分区表）。之后可在调试页 **Web 烧录** 上传 `build/eda_robot.bin`，或：

```bash
curl -X POST --data-binary @build/eda_robot.bin \
  -H 'Content-Type: application/octet-stream' \
  http://<板子IP>/api/ota
```

`GET /api/ota` 查看当前/下一分区。成功后自动重启。

## MS60-1211S80M 雷达验证

临时接线及资料位于 `../docs/MS60-1211S80M/`。实机已验证雷达使用 115200 8N1、正常极性，雷达 TX→IO9、RX→IO10。固件启动后固定使用该配置，并每 200ms 自动发送一次只读 `0x30` 检测查询；`/radar` 页面无需配置，关闭浏览器也会持续采集。

```bash
python clients/robot_api.py 192.168.3.215
curl http://192.168.3.215/api/radar/live
```

`protocol=at6010_ci_0x30_validated` 表示 `0x59/0x30` 传输、校验以及单目标距离/角度已经过实机验证。TYPE=5 多目标仍待完整验收，`slot` 只是帧内序号，不是稳定目标 ID。诊断时可展开页面底部信息，检查 `rxBytes`、`frames59`、`frames5A`、`crcErr`、`malformedFrames`、`discardedBytes` 和 `droppedBytes`。
