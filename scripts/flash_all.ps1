# 串口全量烧录：分区表 + 救援(factory@0x20000) + 主系统(ota_0@0x120000)
# 用法: .\scripts\flash_all.ps1 -Port COM3
param([string]$Port = "COM3")

$ErrorActionPreference = "Stop"
& 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$MainBuild = Join-Path $Root "firmware\build"
$RescueBuild = Join-Path $Root "firmware_recovery\build"

Write-Host "=== build rescue ==="
Set-Location (Join-Path $Root "firmware_recovery")
idf.py -B $RescueBuild build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "=== build main ==="
Set-Location (Join-Path $Root "firmware")
idf.py -B $MainBuild build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "=== flash all -> $Port ==="
python -m esptool --chip esp32s3 -p $Port -b 115200 --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 (Join-Path $MainBuild "bootloader\bootloader.bin") `
  0x8000 (Join-Path $MainBuild "partition_table\partition-table.bin") `
  0xf000 (Join-Path $MainBuild "ota_data_initial.bin") `
  0x20000 (Join-Path $RescueBuild "eda_rescue.bin") `
  0x120000 (Join-Path $MainBuild "eda_robot.bin")

Write-Host @"

完成。若开机进了救援页：按屏幕步骤打开 IP，上传 eda_robot.bin 一次即可回主系统。
日常升级：主页点「进入救援升级」→ 救援页上传大包。
"@
