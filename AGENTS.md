# AI 机器人头部板（EasyEDA）

## 工程入口

| 项目 | 当前值 |
|---|---|
| EasyEDA 工程 | `AI通用机器狗_v4` |
| Board / 原理图 / 图页 | `V1.0.0` / `eda-robot` / `P1` |
| 原理图图页 UUID | `1d774ca900623155` |
| 主控 | `U1 ESP32-S3-WROOM-1-N16R8`（16MB Flash + 8MB Octal PSRAM） |
| 最新实时网表基线 | 2026-07-18：约 102 位号、90 网络 |
| 固件 | `firmware/` ESP-IDF FW **2.1.0**（`idf.py build`；支持 Web OTA） |
| DRC | 原理图 DRC API 空；**PCB（V1.0.0）尚未布线**，内层若残留 `3.3V` 平面须改为 `3V3` |

详细且持续更新的电路事实见 `.cursor/rules/board-facts.mdc`。审查时以 EasyEDA 实时网表为最终依据。

## 必守约定

- **原理图只读**：不通过 EasyEDA API 改图，只读取、查询和审查；改线由用户手动操作。
- **U1 IO35/36/37 禁用**：N16R8 内部 Octal PSRAM 占用，必须保持 NC。
- 用户已接受：实际接入的 **5V 总负载保持低于 5A**。
- 用户已接受：仅 ENC1/ENC2 用于可靠计数；ENC3/ENC4 经 XL9555，只作低速/状态用途。

## 电源架构

`U9/VIN -> Q5 AO4407C 高边开关 -> VBAT -> D1 SS56 -> 8V -> U11 TMI3255 -> 5V -> U5 LD1117S33 -> 3V3`

- 电源网已统一为单一 `3V3`。
- **主要电气风险仍是 U5**：LD1117 从 5V 降到 3V3；下单前应改为 >=1.5A Buck，或用实测证明可用。

## 总线与地址

| 总线 | U1 引脚 | 设备/地址 |
|---|---|---|
| I2C | IO12=SDA、IO13=SCL；R11/R12 4.7k 上拉 | XL9555 U13=`0x20`、OLED U10=`0x3C`、PCA9685 U16=`0x40`、U23=`0x41` |
| UART0 | RXD0/TXD0 | T2 下载排针；本板无 USB 下载接口 |
| 摄像头 SCCB | IO4/IO5 | U6 OV2640（独立于主 I2C） |
| SPI 显示 | IO45/46/3 | U4 ST7796+XPT2046；片选等经 XL9555 |

## 主要模块分配

| 模块 | 已核实连接 | 固件 API |
|---|---|---|
| I2S 麦克风 U2 | SCK=IO16、WS=IO17、SD=IO18 | `/api/mic` |
| I2S 功放 U3 | LRC=IO38、BCLK=IO39、DIN=IO40、SD→XL IO1_6 | `/api/amp` `/api/beep` |
| 编码器 1/2 | IO9/10、IO21/47 | `/api/encoders` |
| 编码器 3/4 | XL9555 IO0_0~3 | `/api/encoders` |
| 舵机 T3-T7 | U16 LED11-15 | `/api/servo`（先 `/api/pwm?on=1`） |
| 电机 U19/U22 | U23 LED14/15…LED9/8；STBY=IO0_5 | `/api/motor`（先 pwm+stby） |
| 探照灯 | U23 LED0/1/2 | `/api/led` |
| OLED U10 | I2C 0x3C | `/api/oled` |
| 摄像头 U6 | 见 board-facts；PWDN=XL IO0_4 | `/api/camera` `/stream` |
| SPI 屏+触摸 U4 | 见 board-facts | `/api/lcd` `/api/touch` |
| Web 烧录 OTA | 双分区 `ota_0`/`ota_1` | `/api/ota` 上传 `eda_robot.bin` |

## U1 Strapping 启动脚

| GPIO | 网络名 | 当前保障 |
|---|---|---|
| IO0 | BOOT / T1 | R1 10k 上拉 |
| IO3 | LCD_MISO | R9 10k 下拉 |
| IO45 | LCD_SCK | R7 10k 下拉，保持 VDD_SPI=3.3V |
| IO46 | LCD_MOSI | R20 10k 下拉 |

## 快速复查流程

用户说“EDA 启动”或要求重新审查时：

1. 使用 `easyeda-api-skill` 连接 Bridge，确认当前工程和图页 UUID。
2. 打开图页 `1d774ca900623155`，等待约 1 秒后读取最新网表、BOM、源码和 DRC。
3. 重新构建器件与网络关系，不沿用旧统计数字。
4. 优先复核：`3V3`、U23 A0、IO0_6/OE#、IO0_5/STBY、U1 IO35/36/37、四个 strapping 电阻、摄像头/LCD 引脚、M1-M5/SCREW 生产属性。
5. 原理图只给文字修改建议，不执行任何写操作。

## 规则与资料

| 文件 | 内容 |
|---|---|
| `.cursor/rules/board-facts.mdc` | 最新完整电路事实、接口分配 |
| `.cursor/rules/schematic-readonly.mdc` | 原理图禁止写操作 |
| `.cursor/rules/easyeda-schematic-read.mdc` | 实时读取与审查流程 |
| `.cursor/rules/esp32-s3-constraints.mdc` | U1 PSRAM/引脚约束 |
| `docs/SPI显示模块转接板.md` | ST7796 + XPT2046 转接板引脚 |
| `docs/12GA-N20编码器.md` | 电机、编码器线色与参数 |
| `firmware/` | ESP-IDF 局域网调试固件（FW 2.0.0）与 Python 客户端 |
