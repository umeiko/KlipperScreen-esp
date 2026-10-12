#!/bin/bash
# ESP32 固件构建/烧录（多板型）。
# 用法:
#   tools/build-esp32.sh <board> build          构建
#   tools/build-esp32.sh <board> flash <port>   构建+烧录（如 COM6 / /dev/ttyUSB0）
#   tools/build-esp32.sh <board> menuconfig     打开 menuconfig（Board selection 里可改板型）
#   board: cyd_2432s028r | cyd_2432s028r_plus | e32r35t | esp32s3-st7789-320_240-ec11 | esp32-st7735s-128_160-ec11 | esp32-st7789-320_240-ec11 | esp32-ILI9341-320_240-ec11 | esp32-ST7796-320_240-ec11 | esp32s3-st7796-480_320-xpt2046-ec11 | esp32s3-ILI9488-480_320-xpt2046-ec11 | esp32s3-ILI9341-320_240-xpt2046-ec11 | esp32c3-st7789-320_240-ec11 | jc8048w550 | esp32s3-sensecap-indicator | esp32s3-JLC-SZP | esp32s3-retro-go | all
#
# 每板型独立的构建目录与 sdkconfig（芯片目标不同，不能混用）：
#   cyd_2432s028r → ESP32   → build/             sdkconfig（仓库已有完整文件）
#   cyd_2432s028r_plus → ESP32（ST7789 版 CYD）→ build-cyd-plus/ sdkconfig.cyd_2432s028r_plus
#   e32r35t       → ESP32   → build-e32r35t/     sdkconfig.e32r35t（首次构建由 defaults 生成）
#   esp32s3-st7789-320_240-ec11 → ESP32-S3 + ST7789 + EC11 → build-ec11-knob-minimal/ sdkconfig.esp32s3-st7789-320_240-ec11
#   esp32-st7735s-128_160-ec11  → ESP32 + ST7735S + EC11（引脚对齐 CYD）→ build-ec11-knob-esp32/ sdkconfig.esp32-st7735s-128_160-ec11
#   esp32-st7789-320_240-ec11   → ESP32 + ST7789 320x240 + EC11（引脚对齐 CYD）→ build-ec11-knob-esp32-st7789/ sdkconfig.esp32-st7789-320_240-ec11
#   esp32-ILI9341-320_240-ec11  → ESP32 + ILI9341 320x240 + EC11（引脚对齐 CYD）→ build-esp32-ili9341-ec11/ sdkconfig.esp32-ILI9341-320_240-ec11
#   esp32-ST7796-320_240-ec11   → ESP32 + ST7796 320x240 + EC11（引脚对齐 CYD，同 ILI9341 板）→ build-esp32-st7796-ec11/ sdkconfig.esp32-ST7796-320_240-ec11
#   esp32s3-st7796-480_320-xpt2046-ec11 → ESP32-S3 + ST7796S + XPT2046 + EC11 → build-esp32s3-st7796-ec11/ sdkconfig.esp32s3-st7796-480_320-xpt2046-ec11
#   esp32s3-ILI9488-480_320-xpt2046-ec11 → ESP32-S3 + ILI9488 + XPT2046 + EC11（MKS PI-TS35，引脚同 st7796 板）→ build-esp32s3-ili9488-ec11/ sdkconfig.esp32s3-ILI9488-480_320-xpt2046-ec11
#   esp32s3-ILI9341-320_240-xpt2046-ec11 → ESP32-S3 + ILI9341 + XPT2046 + EC11（引脚同 st7796 板）→ build-esp32s3-ili9341-ec11/ sdkconfig.esp32s3-ILI9341-320_240-xpt2046-ec11
#   esp32c3-st7789-320_240-ec11 → ESP32-C3 + ST7789 320x240 + EC11（合宙 CORE USB 直连版/Super Mini）→ build-esp32c3-st7789-ec11/ sdkconfig.esp32c3-st7789-320_240-ec11
#   jc8048w550    → ESP32-S3 → build-jc8048w550/ sdkconfig.jc8048w550（首次构建由 defaults 生成）
#   esp32s3-sensecap-indicator → Seeed SenseCAP Indicator（ESP32-S3 + 4" 480x480 ST7701S RGB + FT5x06）→ build-sensecap-indicator/ sdkconfig.esp32s3-sensecap-indicator
#   esp32s3-JLC-SZP → 立创实战派 ESP32-S3 → build-esp32s3-jlc-szp/ sdkconfig.esp32s3-JLC-SZP（首次构建由 defaults 生成）
#   esp32s3-retro-go → Chaeng retro-go ESP32-S3 掌机 → build-esp32s3-retro-go/ sdkconfig.esp32s3-retro-go（首次构建由 defaults 生成）
set -e
cd "$(dirname "$0")/../src/ports/esp32"
IDF_PS1="../../../tools/idf.ps1"

board_conf() {
    case "$1" in
        cyd_2432s028r)
            TARGET=esp32;   BDIR=build;            SDKCFG=sdkconfig
            DEFS="sdkconfig.defaults;sdkconfig.defaults.cyd_2432s028r" ;;
        cyd_2432s028r_plus)
            TARGET=esp32;   BDIR=build-cyd-plus;   SDKCFG=sdkconfig.cyd_2432s028r_plus
            DEFS="sdkconfig.defaults;sdkconfig.defaults.cyd_2432s028r_plus" ;;
        e32r35t)
            TARGET=esp32;   BDIR=build-e32r35t;    SDKCFG=sdkconfig.e32r35t
            DEFS="sdkconfig.defaults;sdkconfig.defaults.e32r35t" ;;
        esp32s3-st7789-320_240-ec11)
            TARGET=esp32s3; BDIR=build-ec11-knob-minimal; SDKCFG=sdkconfig.esp32s3-st7789-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-st7789-320_240-ec11" ;;
        esp32-st7735s-128_160-ec11)
            TARGET=esp32;   BDIR=build-ec11-knob-esp32;   SDKCFG=sdkconfig.esp32-st7735s-128_160-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-st7735s-128_160-ec11" ;;
        esp32-st7789-320_240-ec11)
            TARGET=esp32;   BDIR=build-ec11-knob-esp32-st7789; SDKCFG=sdkconfig.esp32-st7789-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-st7789-320_240-ec11" ;;
        esp32-ILI9341-320_240-ec11)
            TARGET=esp32;   BDIR=build-esp32-ili9341-ec11; SDKCFG=sdkconfig.esp32-ILI9341-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-ILI9341-320_240-ec11" ;;
        esp32-ST7796-320_240-ec11)
            TARGET=esp32;   BDIR=build-esp32-st7796-ec11; SDKCFG=sdkconfig.esp32-ST7796-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32-ST7796-320_240-ec11" ;;
        esp32s3-st7796-480_320-xpt2046-ec11)
            TARGET=esp32s3; BDIR=build-esp32s3-st7796-ec11; SDKCFG=sdkconfig.esp32s3-st7796-480_320-xpt2046-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-st7796-480_320-xpt2046-ec11" ;;
        esp32s3-ILI9488-480_320-xpt2046-ec11)
            TARGET=esp32s3; BDIR=build-esp32s3-ili9488-ec11; SDKCFG=sdkconfig.esp32s3-ILI9488-480_320-xpt2046-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-ILI9488-480_320-xpt2046-ec11" ;;
        esp32s3-ILI9341-320_240-xpt2046-ec11)
            TARGET=esp32s3; BDIR=build-esp32s3-ili9341-ec11; SDKCFG=sdkconfig.esp32s3-ILI9341-320_240-xpt2046-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-ILI9341-320_240-xpt2046-ec11" ;;
        esp32c3-st7789-320_240-ec11)
            TARGET=esp32c3; BDIR=build-esp32c3-st7789-ec11; SDKCFG=sdkconfig.esp32c3-st7789-320_240-ec11
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32c3-st7789-320_240-ec11" ;;
        jc8048w550)
            TARGET=esp32s3; BDIR=build-jc8048w550; SDKCFG=sdkconfig.jc8048w550
            DEFS="sdkconfig.defaults;sdkconfig.defaults.jc8048w550" ;;
        esp32s3-sensecap-indicator)
            TARGET=esp32s3; BDIR=build-sensecap-indicator; SDKCFG=sdkconfig.esp32s3-sensecap-indicator
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-sensecap-indicator" ;;
        esp32s3-JLC-SZP)
            TARGET=esp32s3; BDIR=build-esp32s3-jlc-szp; SDKCFG=sdkconfig.esp32s3-JLC-SZP
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-JLC-SZP" ;;
        esp32s3-retro-go)
            TARGET=esp32s3; BDIR=build-esp32s3-retro-go; SDKCFG=sdkconfig.esp32s3-retro-go
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-retro-go" ;;
        *) echo "unknown board: $1 (cyd_2432s028r | cyd_2432s028r_plus | e32r35t | esp32s3-st7789-320_240-ec11 | esp32-st7735s-128_160-ec11 | esp32-st7789-320_240-ec11 | esp32-ILI9341-320_240-ec11 | esp32-ST7796-320_240-ec11 | esp32s3-st7796-480_320-xpt2046-ec11 | esp32s3-ILI9488-480_320-xpt2046-ec11 | esp32s3-ILI9341-320_240-xpt2046-ec11 | esp32c3-st7789-320_240-ec11 | jc8048w550 | esp32s3-sensecap-indicator | esp32s3-JLC-SZP | esp32s3-retro-go | all)" >&2; exit 1 ;;
    esac
}

idf() { powershell -NoProfile -ExecutionPolicy Bypass -File "$IDF_PS1" "$@"; }

# 电阻触摸屏板型（与 src/bsp/bsp_caps.h 的 BSP_HAS_TOUCH_CAL 登记保持一致）
has_touch_cal() {
    case "$1" in
        cyd_2432s028r|cyd_2432s028r_plus|e32r35t|\
        esp32s3-st7796-480_320-xpt2046-ec11|esp32s3-ILI9488-480_320-xpt2046-ec11|\
        esp32s3-ILI9341-320_240-xpt2046-ec11) return 0 ;;
        *) return 1 ;;
    esac
}

# 烧录后引导触摸校准：经串口发送 CLI 命令 caltouch（写标记文件并重启进入两点校准 UI）
offer_touch_cal() {
    local port="$1"
    echo
    read -r -p "该板型为电阻触摸屏。需要现在进入触摸校准吗？[y/N] " ans
    case "$ans" in y|Y|yes|YES) ;; *) return 0 ;; esac
    local py
    py=$(ls -d "$(cygpath "$USERPROFILE")"/.espressif/python_env/*/Scripts/python.exe 2>/dev/null | head -1)
    if [ -z "$py" ]; then
        echo "未找到 IDF python 环境，请稍后手动用串口发送 caltouch 命令进入校准。"
        return 0
    fi
    echo "等待设备重启..."
    sleep 3
    "$py" - "$port" <<'EOF'
import sys, time
try:
    import serial
except ImportError:
    print("pyserial 不可用，请手动用串口发送 caltouch 命令进入校准")
    sys.exit(0)
port = sys.argv[1]
try:
    s = serial.Serial(port, 115200, timeout=2)
except Exception as e:
    print(f"串口 {port} 打开失败: {e}")
    print("请手动用串口工具连接 115200 波特率，发送 caltouch 进入校准")
    sys.exit(0)
time.sleep(0.3)
s.write(b"caltouch\n")
s.flush()
time.sleep(1.0)
try:
    resp = s.read(4096).decode(errors="ignore").strip()
    if resp:
        print(resp)
except Exception:
    pass
s.close()
print("已发送 caltouch，设备将重启并进入触摸校准界面，请依次点按屏幕上的校准点。")
EOF
}

build_one() {
    board_conf "$1"
    if [ ! -f "$SDKCFG" ]; then
        echo "== $1: set-target $TARGET (生成 $SDKCFG)"
        idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -DSDKCONFIG_DEFAULTS="$DEFS" set-target "$TARGET"
    fi
    echo "== $1: build ($BDIR)"
    idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -DSDKCONFIG_DEFAULTS="$DEFS" build
}

BOARD="${1:?board}"; ACT="${2:-build}"
if [ "$BOARD" = all ]; then
    [ "$ACT" = build ] || { echo "all 只支持 build" >&2; exit 1; }
    build_one cyd_2432s028r
    build_one cyd_2432s028r_plus
    build_one e32r35t
    build_one esp32s3-st7789-320_240-ec11
    build_one esp32-st7735s-128_160-ec11
    build_one esp32-st7789-320_240-ec11
    build_one esp32-ILI9341-320_240-ec11
    build_one esp32-ST7796-320_240-ec11
    build_one esp32s3-st7796-480_320-xpt2046-ec11
    build_one esp32s3-ILI9488-480_320-xpt2046-ec11
    build_one esp32s3-ILI9341-320_240-xpt2046-ec11
    build_one esp32c3-st7789-320_240-ec11
    build_one jc8048w550
    build_one esp32s3-sensecap-indicator
    build_one esp32s3-JLC-SZP
    build_one esp32s3-retro-go
    exit 0
fi

board_conf "$BOARD"
case "$ACT" in
    build)      build_one "$BOARD" ;;
    menuconfig) idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -DSDKCONFIG_DEFAULTS="$DEFS" menuconfig ;;
    flash)      build_one "$BOARD"; idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -p "${3:?port}" flash
                has_touch_cal "$BOARD" && offer_touch_cal "$3" ;;
    *) echo "unknown action: $ACT" >&2; exit 1 ;;
esac
