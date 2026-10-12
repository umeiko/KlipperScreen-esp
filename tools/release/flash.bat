@echo off
rem Klipper Remote ESP32 Displays - Windows flash script
rem Usage: flash.bat [COMx]
rem Board parameters come from flash.cfg (CI 打包时按板型生成):
rem   CHIP=esp32|esp32s3   FLASH_SIZE=4MB|16MB   BL_OFFSET=0x1000|0x0   TOUCH_CAL=0|1
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ---- 语言选择 / Language ----
set LANG_ZH=0
echo 请选择语言 / Choose language:
echo   1^) 中文
echo   2^) English
set /p LANG_CHOICE="> "
if "%LANG_CHOICE%"=="1" set LANG_ZH=1

set CHIP=esp32
set FLASH_SIZE=4MB
set BL_OFFSET=0x1000
set TOUCH_CAL=0
if exist flash.cfg (
  for /f "tokens=1,2 delims== eol=#" %%a in (flash.cfg) do set "%%a=%%b"
)

set PORT=%~1
if "%PORT%"=="" goto askport
goto haveport
:askport
if "%LANG_ZH%"=="1" (set /p PORT=请输入 COM 端口（例如 COM6）: ) else (set /p PORT="Enter COM port (e.g. COM6): ")
:haveport

if not exist esptool.exe (
  if "%LANG_ZH%"=="1" (echo 脚本旁边未找到 esptool.exe！请解压完整刷机包后再运行。) else (echo esptool.exe not found next to this script! Please extract the full flash package first.)
  pause
  exit /b 1
)

if "%LANG_ZH%"=="1" (echo 正在刷入 %PORT%（芯片=%CHIP%，flash=%FLASH_SIZE%）) else (echo Flashing to %PORT%  (chip=%CHIP%, flash=%FLASH_SIZE%))
esptool.exe --chip %CHIP% -b 460800 --before default-reset --after hard-reset ^
  write-flash --flash-mode dio --flash-size %FLASH_SIZE% --flash-freq 80m ^
  %BL_OFFSET% bootloader.bin 0x8000 partition-table.bin 0x10000 klipper_remote_display.bin
if errorlevel 1 (
  if "%LANG_ZH%"=="1" (echo 刷写失败，请检查端口是否被占用（串口监视器/其它软件）后重试。) else (echo Flash failed. Check that the port is not in use (serial monitor/other apps) and retry.)
  pause
  exit /b 1
)

echo.
if "%LANG_ZH%"=="1" (echo 完成。请按 RESET 或重新插拔 USB。首次启动约 3 秒（开机动画）。) else (echo Done. Press RESET or replug USB. First boot takes ~3s (boot animation).)

rem ---- 电阻触摸屏（TOUCH_CAL=1）：可选进入触摸两点校准 ----
if not "%TOUCH_CAL%"=="1" goto done
echo.
if "%LANG_ZH%"=="1" (set /p CAL=该板型为电阻触摸屏。需要现在进入触摸校准吗？[y/N] ) else (set /p CAL="This board has a resistive touch screen. Run touch calibration now? [y/N] ")
if /i not "%CAL%"=="y" goto done
if "%LANG_ZH%"=="1" (echo 等待设备重启...) else (echo Waiting for the board to reboot...)
timeout /t 3 /nobreak >nul
rem 经串口发送 caltouch：CLI 对 \r 和 \n 都识别为行结束
mode %PORT%: BAUD=115200 PARITY=n DATA=8 STOP=1 >nul 2>&1
echo caltouch> \\.\%PORT% 2>nul
if errorlevel 1 goto calfail
if "%LANG_ZH%"=="1" (echo 已发送校准命令，设备将重启进入触摸校准界面，请依次精准点按两个校准点。) else (echo Calibration command sent. The board will reboot into touch calibration - tap the two crosses precisely.)
goto done
:calfail
if "%LANG_ZH%"=="1" (echo 串口发送失败。可稍后手动用串口工具连接 115200 波特率，发送 caltouch 进入校准。) else (echo Serial write failed. You can later connect a serial terminal at 115200 and send: caltouch)

:done
pause
