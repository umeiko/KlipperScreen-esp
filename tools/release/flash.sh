#!/usr/bin/env bash
# Klipper Remote ESP32 Displays - macOS / Linux flash script
# Usage: ./flash.sh [serial-port]   (auto-detects /dev/ttyUSB* or /dev/cu.usbserial-* if omitted)
# Requires esptool: pip install esptool
# Board parameters come from flash.cfg (CI 打包时按板型生成):
#   CHIP=esp32|esp32s3   FLASH_SIZE=4MB|16MB   BL_OFFSET=0x1000|0x0   TOUCH_CAL=0|1
set -e
cd "$(dirname "$0")"

# ---- 语言选择 / Language（非交互环境默认 English） ----
LANG_CHOICE=en
if [ -t 0 ]; then
  echo "请选择语言 / Choose language:"
  echo "  1) 中文"
  echo "  2) English"
  read -r -p "> " c
  [ "$c" = "1" ] && LANG_CHOICE=zh
fi
say() { if [ "$LANG_CHOICE" = zh ]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }
ask() { if [ "$LANG_CHOICE" = zh ]; then read -r -p "$1" REPLY; else read -r -p "$2" REPLY; fi; }

CHIP=esp32
FLASH_SIZE=4MB
BL_OFFSET=0x1000
TOUCH_CAL=0
if [ -f flash.cfg ]; then
  set -a; . ./flash.cfg; set +a
fi

PORT="${1:-}"
if [ -z "$PORT" ]; then
  if [ "$(uname)" = "Darwin" ]; then
    PORT=$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial-* /dev/cu.SLAB_USBtoUART 2>/dev/null | head -1)
  else
    PORT=$(ls /dev/ttyUSB* 2>/dev/null | head -1)
  fi
fi
if [ -z "$PORT" ]; then
  say "未找到串口。用法：./flash.sh <端口>，例如 ./flash.sh /dev/ttyUSB0" \
      "No serial port found. Usage: ./flash.sh <port>   e.g. ./flash.sh /dev/ttyUSB0"
  exit 1
fi

if command -v esptool >/dev/null 2>&1; then
  ESP="esptool"
elif command -v esptool.py >/dev/null 2>&1; then
  ESP="esptool.py"
else
  ESP="python3 -m esptool"
fi

say "正在刷入 $PORT（芯片=$CHIP，flash=$FLASH_SIZE）" \
    "Flashing to $PORT  (chip=$CHIP, flash=$FLASH_SIZE)"
$ESP --chip "$CHIP" -b 460800 --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size "$FLASH_SIZE" --flash-freq 80m \
  "$BL_OFFSET" bootloader.bin 0x8000 partition-table.bin 0x10000 klipper_remote_display.bin

say "完成。请按 RESET 或重新插拔 USB。首次启动约 3 秒（开机动画）。" \
    "Done. Press RESET or replug USB. First boot takes ~3s (boot animation)."

# ---- 电阻触摸屏（TOUCH_CAL=1）：可选进入触摸两点校准 ----
if [ "$TOUCH_CAL" = "1" ] && [ -t 0 ]; then
  ask "该板型为电阻触摸屏。需要现在进入触摸校准吗？[y/N] " \
      "This board has a resistive touch screen. Run touch calibration now? [y/N] "
  case "$REPLY" in
    y|Y|yes|YES|是)
      say "等待设备重启..." "Waiting for the board to reboot..."
      sleep 3
      python3 - "$PORT" <<'PYEOF'
import sys, time
port = sys.argv[1]
try:
    import serial
except ImportError:
    print("pyserial not found; connect a serial terminal at 115200 and send: caltouch")
    sys.exit(0)
try:
    s = serial.Serial(port, 115200, timeout=2)
except Exception as e:
    print(f"cannot open {port}: {e}")
    print("connect a serial terminal at 115200 and send: caltouch")
    sys.exit(0)
time.sleep(0.3)
s.write(b"caltouch\n")
s.flush()
time.sleep(1.0)
s.close()
print("caltouch sent")
PYEOF
      say "已发送校准命令，设备将重启进入触摸校准界面，请依次精准点按两个校准点。" \
          "Calibration command sent. The board will reboot into touch calibration - tap the two crosses precisely."
      ;;
  esac
fi
