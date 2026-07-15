# AI 机器人头部板（EasyEDA）

## 工程入口

| 项目 | 当前值 |
|---|---|
| EasyEDA 工程 | `AI智能机器狗_lx_改版_v2` |
| Board / 原理图 / 图页 | `V1.0_6` / `EDA-RobotPro_6` / `P1` |
| 原理图图页 UUID | `1d774ca900623155` |
| 主控 | `U1 ESP32-S3-WROOM-1-N16R8`（16MB Flash + 8MB Octal PSRAM） |
| 最新实时网表基线 | 2026-07-14：80 个物理器件、61 条网络、无重复位号、无单端命名网络 |
| DRC | 原理图 3 个 warning；**PCB（PCB2_8）尚未布线**，且内层 Inner1 残留旧网名 `3.3V` 平面分区，布线前必须改为 `3V3` |

详细且持续更新的电路事实见 `.cursor/rules/board-facts.mdc`。审查时以 EasyEDA 实时网表为最终依据，不把本文件当作网表替代品。

## 必守约定

- **原理图只读**：不通过 EasyEDA API 改图，只读取、查询和审查；改线由用户手动操作。
- **U1 IO35/36/37 禁用**：N16R8 内部 Octal PSRAM 占用，必须保持 NC，不得分配给任何外设。
- 用户已接受：实际接入的 **5V 总负载保持低于 5A**，不再以“所有接口同时满载”作为阻断项。
- 用户已接受：仅 ENC1/ENC2 用于可靠计数；ENC3/ENC4 继续通过 XL9555，只作低速/状态用途。
- 2026-07-14 已删除：摄像头、探照灯、SPI 显示/触摸模组；`CAM_*` / 探照灯 / `LCD_CS` 等专属网络已消失。

## 电源架构

`U9/VIN -> Q5 AO4407C 高边开关 -> VBAT -> D1 SS56 -> 8V -> U11 TMI3255 -> 5V -> U5 LD1117S33 -> 3V3`

- 电源网已统一为单一 `3V3`（无旧名 `3.3V`）。
- **主要电气风险仍是 U5**：LD1117 从 5V 降到 3V3，供应 ESP32、OLED、编码器和逻辑器件，存在电流及 SOT-223 散热裕量风险。下单前应改为 >=1.5A 的 Buck，或用实测峰值电流和热测试证明可用。

## 总线与地址

| 总线 | U1 引脚 | 设备/地址 |
|---|---|---|
| I2C | IO12=SDA、IO13=SCL；R11/R12 4.7k 上拉 | XL9555 U13=`0x20`、OLED U10=`0x3C`、PCA9685 U16=`0x40`、U23=`0x41` |
| UART0 | RXD0/TXD0 | T2 下载排针；本板无 USB 下载接口 |

## 主要模块分配

| 模块 | 已核实连接 |
|---|---|
| I2S 麦克风 U2 | SCK=IO16、WS=IO17、SD=IO18、L/R 经 R2 接地 |
| I2S 功放 U3 | LRC=IO38、BCLK=IO39、DIN=IO40、SD_MODE=XL9555 IO1_6（R10 10k 上拉） |
| 编码器 1/2 | IO9/IO10、IO21/IO47，供固件 PCNT 使用 |
| 编码器 3/4 | XL9555 IO0_0~IO0_3 |
| 舵机 T3-T7 | U16 PCA9685 LED11-15，5V 供电 |
| 电机 U19/U22 | TB6612，方向/PWM 来自 U23 LED8-15；STBY=XL9555 IO0_5，R24 10k 下拉 |
| PWM 总失能 | U16/U23 OE# 共接 XL9555 IO0_6，R23 10k 上拉，默认禁止输出 |

## U1 Strapping 启动脚

上电电平影响 Boot、JTAG 和 VDD_SPI 配置，以下电阻不可随意删除（显示已删，下拉仍保留）。

| GPIO | 网络名（历史） | 当前保障 |
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
4. 优先复核：`3V3`、U23 A0、IO0_6/OE#、IO0_5/STBY、U1 IO35/36/37、四个 strapping 电阻、M1-M5/SCREW 生产属性。
5. 原理图只给文字修改建议，不执行任何写操作。

## 规则与资料

| 文件 | 内容 |
|---|---|
| `.cursor/rules/board-facts.mdc` | 最新完整电路事实、接口分配和已接受风险 |
| `.cursor/rules/schematic-readonly.mdc` | 原理图禁止写操作 |
| `.cursor/rules/easyeda-schematic-read.mdc` | 实时读取与审查流程 |
| `.cursor/rules/esp32-s3-constraints.mdc` | U1 PSRAM/引脚约束 |
| `docs/esp32 s3.jpg` | 拆机摄像头来源开发板原理图（历史） |
| `docs/SPI显示模块转接板.md` | ST7796 + XPT2046 转接板引脚（已删除模组，历史） |
| `docs/12GA-N20编码器.md` | 电机、编码器线色与参数 |
| `docs/LD2402单模块/` | HLK-LD2402 雷达资料 |
