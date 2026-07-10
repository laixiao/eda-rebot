# AI 机器人头部板（EasyEDA）

## 必守约定

- **原理图只读**：不改图，只查/审查；改线由用户手动操作
- **U1 IO35/36/37 禁用**：ESP32-S3-WROOM-1-N16R8 内部 PSRAM 占用，勿再分配外设

## U1 Strapping 启动脚（非禁用，谨慎使用）

上电电平影响 Boot / JTAG / 供电配置，不能像普通 IO 随意接。

| GPIO | 作用 | 本板现状（v2） |
|------|------|----------|
| IO0 | Boot 模式 | R1 10k 上拉，接 T1 下载排针 |
| IO3 | JTAG 源选择 | 复用 LCD_MISO，R9 10k 下拉 |
| IO45 | VDD_SPI 电压 strap | 复用 LCD_SCK，R7 10k 下拉（保持 3.3V） |
| IO46 | ROM 日志 strap | 复用 LCD_MOSI，R20 10k 下拉（下载模式有效） |

> R7/R9/R20 三个下拉是 strap 安全的前提，改版时**不可删除**。

## 规则目录

| 文件 | 内容 |
|------|------|
| `schematic-readonly.mdc` | 原理图禁止写操作 |
| `easyeda-schematic-read.mdc` | 原理图读取与审查流程 |
| `esp32-s3-constraints.mdc` | U1 引脚约束（含 PSRAM） |
| `project-knowledge-updates.mdc` | 新约定如何写入规则 |

## 启动 EDA

用户说「EDA 启动」→ 按 `easyeda-api-skill` 连 Bridge，只读查原理图/网表。
