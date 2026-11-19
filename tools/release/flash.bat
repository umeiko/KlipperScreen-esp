@echo off
rem Klipper Remote ESP32 Displays - Windows flash script
rem Usage: flash.bat [COMx|x]
rem Board parameters come from flash.cfg (CI 打包时按板型生成):
rem   CHIP=esp32|esp32s3   FLASH_SIZE=4MB|16MB   BL_OFFSET=0x1000|0x0   TOUCH_CAL=0|1
chcp 65001 >nul
setlocal
cd /d "%~dp0"

rem ---- 语言选择 / Language ----
set "LANG_ZH=0"
echo 请选择语言 / Choose language:
echo   1^) 中文
echo   2^) English
set /p "LANG_CHOICE=> "
if "%LANG_CHOICE%"=="1" set "LANG_ZH=1"

set "CHIP=esp32"
set "FLASH_SIZE=4MB"
set "BL_OFFSET=0x1000"
set "TOUCH_CAL=0"
if not exist flash.cfg goto cfg_done
for /f "tokens=1,2 delims== eol=#" %%a in (flash.cfg) do set "%%a=%%b"
:cfg_done

set "PORT=%~1"
if defined PORT goto normalize_port
:askport
if "%LANG_ZH%"=="1" goto askport_zh
set /p "PORT=Enter COM port (e.g. COM6 or 6): "
goto normalize_port
:askport_zh
set /p "PORT=请输入 COM 端口（例如 COM6 或 6）: "

:normalize_port
if not defined PORT goto askport
set "PORT=%PORT:"=%"
if /i "%PORT:~0,3%"=="COM" goto port_ready
set "PORT=COM%PORT%"
:port_ready

if exist esptool.exe goto flash_message
if "%LANG_ZH%"=="1" goto missing_tool_zh
echo esptool.exe not found next to this script! Please extract the full flash package first.
pause
exit /b 1
:missing_tool_zh
echo 脚本旁边未找到 esptool.exe！请解压完整刷机包后再运行。
pause
exit /b 1

:flash_message
if "%LANG_ZH%"=="1" goto flash_message_zh
echo Flashing to %PORT%  (chip=%CHIP%, flash=%FLASH_SIZE%)
goto flash_run
:flash_message_zh
echo 正在刷入 %PORT%（芯片=%CHIP%，flash=%FLASH_SIZE%）

:flash_run
rem Keep the board-specific mode/frequency embedded by ESP-IDF (CYD needs DIO/40MHz).
esptool.exe --chip %CHIP% -p "%PORT%" -b 460800 --before default-reset --after hard-reset ^
  write-flash --flash-mode keep --flash-size %FLASH_SIZE% --flash-freq keep ^
  %BL_OFFSET% bootloader.bin 0x8000 partition-table.bin 0x10000 klipper_remote_display.bin
if errorlevel 1 goto flash_failed

echo.
if "%LANG_ZH%"=="1" goto flash_done_zh
echo Done. Press RESET or replug USB. First boot takes about 3 seconds (boot animation).
goto after_flash
:flash_done_zh
echo 完成。请按 RESET 或重新插拔 USB。首次启动约 3 秒（开机动画）。
goto after_flash

:flash_failed
if "%LANG_ZH%"=="1" goto flash_failed_zh
echo Flash failed. Check that the port is not in use by a serial monitor or another app, then retry.
pause
exit /b 1
:flash_failed_zh
echo 刷写失败，请检查端口是否被串口监视器或其它软件占用后重试。
pause
exit /b 1

:after_flash
rem ---- 电阻触摸屏（TOUCH_CAL=1）：可选进入触摸两点校准 ----
if not "%TOUCH_CAL%"=="1" goto done
echo.
if "%LANG_ZH%"=="1" goto ask_cal_zh
set /p "CAL=This board has a resistive touch screen. Run touch calibration now? [y/N] "
goto check_cal
:ask_cal_zh
set /p "CAL=该板型为电阻触摸屏。需要现在进入触摸校准吗？[y/N] "

:check_cal
if /i not "%CAL%"=="y" goto done
if "%LANG_ZH%"=="1" goto wait_board_zh
echo Waiting for the board to reboot...
goto send_cal
:wait_board_zh
echo 等待设备重启...

:send_cal
timeout /t 3 /nobreak >nul
rem 经串口发送 caltouch：CLI 对 \r 和 \n 都识别为行结束
mode %PORT%: BAUD=115200 PARITY=n DATA=8 STOP=1 >nul 2>&1
echo caltouch>\\.\%PORT% 2>nul
if errorlevel 1 goto cal_failed
if "%LANG_ZH%"=="1" goto cal_done_zh
echo Calibration command sent. The board will reboot into touch calibration; tap the two crosses precisely.
goto done
:cal_done_zh
echo 已发送校准命令，设备将重启进入触摸校准界面，请依次精准点按两个校准点。
goto done

:cal_failed
if "%LANG_ZH%"=="1" goto cal_failed_zh
echo Serial write failed. You can later connect a serial terminal at 115200 and send: caltouch
goto done
:cal_failed_zh
echo 串口发送失败。可稍后手动用串口工具连接 115200 波特率，发送 caltouch 进入校准。

:done
pause
