#!/usr/bin/env bash
# Klipper Remote ESP32 Displays - macOS / Linux flash script
# Usage: ./flash.sh [serial-port]   (auto-detects /dev/ttyUSB* or /dev/cu.usbserial-* if omitted)
# Requires esptool: pip install esptool
# Board parameters come from flash.cfg (CI 打包时按板型生成):
#   CHIP=esp32|esp32s3   FLASH_SIZE=4MB|16MB   BL_OFFSET=0x1000|0x0
set -e
cd "$(dirname "$0")"

CHIP=esp32
FLASH_SIZE=4MB
BL_OFFSET=0x1000
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
  echo "No serial port found. Usage: ./flash.sh <port>   e.g. ./flash.sh /dev/ttyUSB0"
  exit 1
fi

if command -v esptool >/dev/null 2>&1; then
  ESP="esptool"
elif command -v esptool.py >/dev/null 2>&1; then
  ESP="esptool.py"
else
  ESP="python3 -m esptool"
fi

echo "Flashing to $PORT  (chip=$CHIP, flash=$FLASH_SIZE)"
$ESP --chip "$CHIP" -b 460800 --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size "$FLASH_SIZE" --flash-freq 80m \
  "$BL_OFFSET" bootloader.bin 0x8000 partition-table.bin 0x10000 klipper_remote_display.bin

echo "Done. Press RESET or replug USB. First boot takes ~3s (boot animation)."
