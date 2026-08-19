# ESP32-S3 USB-JTAG 烧录

## 引脚

| GPIO | USB 信号 |
|------|---------|
| GPIO19 | D- |
| GPIO20 | D+ |

接到 USB-C 座的 D-/D+ 即可，无需外部 USB 转串口芯片。

## 烧录命令

```powershell
python -m esptool --chip esp32s3 -p COM7 -b 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0      build\bootloader\bootloader.bin `
  0x8000   build\partition_table\partition-table.bin `
  0xf000   build\ota_data_initial.bin `
  0x20000  build\your_app.bin
```

COM 口按实际修改（Windows 设备管理器查看）。

## 优点

- 自动进下载模式，无需手动按 BOOT+EN
- 无需 CH340 / CP2102 等外部芯片
- 同时支持 JTAG 调试（OpenOCD）

## 注意

- 仅 **ESP32-S3 / ESP32-S2** 支持，原版 ESP32 不支持
- GPIO19/20 占用后不能再做普通 GPIO
- D-/D+ 建议加 ESD 保护器件（可选）
