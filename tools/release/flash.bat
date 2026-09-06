@echo off
rem Klipper Remote ESP32 Displays - Windows flash script
rem Usage: flash.bat [COMx]
rem Board parameters come from flash.cfg (CI 打包时按板型生成):
rem   CHIP=esp32|esp32s3   FLASH_SIZE=4MB|16MB   BL_OFFSET=0x1000|0x0
setlocal
cd /d "%~dp0"

set CHIP=esp32
set FLASH_SIZE=4MB
set BL_OFFSET=0x1000
if exist flash.cfg (
  for /f "tokens=1,2 delims== eol=#" %%a in (flash.cfg) do set "%%a=%%b"
)

set PORT=%~1
if "%PORT%"=="" set /p PORT=Enter COM port (e.g. COM6):

if not exist esptool.exe (
    echo esptool.exe not found next to this script!
    pause
    exit /b 1
)

echo Flashing to %PORT%  (chip=%CHIP%, flash=%FLASH_SIZE%)
esptool.exe --chip %CHIP% -b 460800 --before default-reset --after hard-reset ^
  write-flash --flash-mode dio --flash-size %FLASH_SIZE% --flash-freq 80m ^
  %BL_OFFSET% bootloader.bin 0x8000 partition-table.bin 0x10000 klipper_remote_display.bin

echo.
echo Done. Press RESET or replug USB. First boot takes ~3s (boot animation).
pause
