@echo off
REM 串口全量：分区表 + 救援(factory) + 主系统(ota_0)
REM 用法: flash_all.bat COM3
set PORT=%1
if "%PORT%"=="" set PORT=COM3

call "C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1"

set ROOT=%~dp0..
set MAIN_BUILD=%ROOT%\firmware\build
set RESCUE_BUILD=%ROOT%\firmware_recovery\build

echo === build rescue ===
cd /d "%ROOT%\firmware_recovery"
idf.py -B "%RESCUE_BUILD%" build
if errorlevel 1 exit /b 1

echo === build main ===
cd /d "%ROOT%\firmware"
idf.py -B "%MAIN_BUILD%" build
if errorlevel 1 exit /b 1

echo === flash all to %PORT% ===
python -m esptool --chip esp32s3 -p %PORT% -b 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m ^
  0x0 "%MAIN_BUILD%\bootloader\bootloader.bin" ^
  0x8000 "%MAIN_BUILD%\partition_table\partition-table.bin" ^
  0xf000 "%MAIN_BUILD%\ota_data_initial.bin" ^
  0x20000 "%RESCUE_BUILD%\eda_rescue.bin" ^
  0x120000 "%MAIN_BUILD%\eda_robot.bin"

echo done. Device should boot into ota_0 main app.
