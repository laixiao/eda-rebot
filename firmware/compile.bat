@echo off
REM Compile for ESP32-S3-WROOM-1-N16R8 (Octal PSRAM)
set CLI=%~dp0tools\arduino-cli.exe
if not exist "%CLI%" (
  echo Missing tools\arduino-cli.exe
  exit /b 1
)
set FQBN=esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=default,PSRAM=opi,CDCOnBoot=default,UploadMode=default,USBMode=hwcdc
"%CLI%" compile --fqbn %FQBN% "%~dp0eda_robot_debug"
if errorlevel 1 exit /b 1
echo.
echo Compile OK. Upload via UART0 (T2) example:
echo   %CLI% upload -p COMx --fqbn %FQBN% "%~dp0eda_robot_debug"
echo Then open serial 115200, wait for IP, browse http://^<IP^>/
