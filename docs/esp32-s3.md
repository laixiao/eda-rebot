# ESP32-S3-WROOM-1（U1）

本板主控模组。参考图：`esp32 s3.jpg`（原理图）、`esp32 s3实物.png`（开发板实物）。

## 模组

| 项目 | 参数 |
|------|------|
| 型号 | **ESP32-S3-WROOM-1-N16R8** |
| Flash | 16 MB |
| PSRAM | 8 MB（Octal，内部占用 IO35/36/37） |
| 无线 | Wi‑Fi + BLE |

## 禁用引脚

**IO35、IO36、IO37** 被内部 PSRAM 占用，**不可**作外设 GPIO。

## 开发板接口（参考实物）

- 双 USB‑C：UART 下载 / USB OTG
- FPC：摄像头或屏接口
- 按键：RST（EN）、BOOT（IO0）
- 排针：两侧各 20 Pin，引出 3V3 / 5V / GND 及 GPIO

## 常用启动脚（Strapping）

| GPIO | 作用 | 建议 |
|------|------|------|
| IO0 | Boot 模式 | 上电勿误拉低 |
| IO3 | JTAG 选择 | 按需 |
| IO45 | VDD_SPI 电压 | 按需 |
| IO46 | ROM 日志 | 按需 |
