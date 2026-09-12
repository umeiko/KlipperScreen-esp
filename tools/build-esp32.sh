#!/bin/bash
# ESP32 固件构建/烧录（多板型）。
# 用法:
#   tools/build-esp32.sh <board> build          构建
#   tools/build-esp32.sh <board> flash <port>   构建+烧录（如 COM6 / /dev/ttyUSB0）
#   tools/build-esp32.sh <board> menuconfig     打开 menuconfig（Board selection 里可改板型）
#   board: cyd_2432s028r | e32r35t | ec11_knob_minimal | ec11_knob_esp32 | jc8048w550 | esp32s3-JLC-SZP | all
#
# 每板型独立的构建目录与 sdkconfig（芯片目标不同，不能混用）：
#   cyd_2432s028r → ESP32   → build/             sdkconfig（仓库已有完整文件）
#   e32r35t       → ESP32   → build-e32r35t/     sdkconfig.e32r35t（首次构建由 defaults 生成）
#   ec11_knob_minimal → EC11 旋钮最小系统 (ESP32-S3) → build-ec11-knob-minimal/ sdkconfig.ec11_knob_minimal
#   ec11_knob_esp32   → EC11 旋钮最小系统 (ESP32, 引脚对齐 CYD) → build-ec11-knob-esp32/ sdkconfig.ec11_knob_esp32
#   jc8048w550    → ESP32-S3 → build-jc8048w550/ sdkconfig.jc8048w550（首次构建由 defaults 生成）
#   esp32s3-JLC-SZP → 立创实战派 ESP32-S3 → build-esp32s3-jlc-szp/ sdkconfig.esp32s3-JLC-SZP（首次构建由 defaults 生成）
set -e
cd "$(dirname "$0")/../src/ports/esp32"
IDF_PS1="../../../tools/idf.ps1"

board_conf() {
    case "$1" in
        cyd_2432s028r)
            TARGET=esp32;   BDIR=build;            SDKCFG=sdkconfig
            DEFS="sdkconfig.defaults;sdkconfig.defaults.cyd_2432s028r" ;;
        e32r35t)
            TARGET=esp32;   BDIR=build-e32r35t;    SDKCFG=sdkconfig.e32r35t
            DEFS="sdkconfig.defaults;sdkconfig.defaults.e32r35t" ;;
        ec11_knob_minimal)
            TARGET=esp32s3; BDIR=build-ec11-knob-minimal; SDKCFG=sdkconfig.ec11_knob_minimal
            DEFS="sdkconfig.defaults;sdkconfig.defaults.ec11_knob_minimal" ;;
        ec11_knob_esp32)
            TARGET=esp32;   BDIR=build-ec11-knob-esp32;   SDKCFG=sdkconfig.ec11_knob_esp32
            DEFS="sdkconfig.defaults;sdkconfig.defaults.ec11_knob_esp32" ;;
        jc8048w550)
            TARGET=esp32s3; BDIR=build-jc8048w550; SDKCFG=sdkconfig.jc8048w550
            DEFS="sdkconfig.defaults;sdkconfig.defaults.jc8048w550" ;;
        esp32s3-JLC-SZP)
            TARGET=esp32s3; BDIR=build-esp32s3-jlc-szp; SDKCFG=sdkconfig.esp32s3-JLC-SZP
            DEFS="sdkconfig.defaults;sdkconfig.defaults.esp32s3-JLC-SZP" ;;
        *) echo "unknown board: $1 (cyd_2432s028r | e32r35t | ec11_knob_minimal | ec11_knob_esp32 | jc8048w550 | esp32s3-JLC-SZP | all)" >&2; exit 1 ;;
    esac
}

idf() { powershell -NoProfile -ExecutionPolicy Bypass -File "$IDF_PS1" "$@"; }

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
    build_one e32r35t
    build_one ec11_knob_minimal
    build_one ec11_knob_esp32
    build_one jc8048w550
    build_one esp32s3-JLC-SZP
    exit 0
fi

board_conf "$BOARD"
case "$ACT" in
    build)      build_one "$BOARD" ;;
    menuconfig) idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -DSDKCONFIG_DEFAULTS="$DEFS" menuconfig ;;
    flash)      build_one "$BOARD"; idf -B "$BDIR" -DSDKCONFIG="$SDKCFG" -p "${3:?port}" flash ;;
    *) echo "unknown action: $ACT" >&2; exit 1 ;;
esac
