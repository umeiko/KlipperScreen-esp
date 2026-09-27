#!/bin/bash
# KlipperScreen-esp 启动器：按 BACKEND 拉起 X11(xinit) 或 Wayland(weston kiosk)。
# 由 KlipperScreen-esp.service 调用；模板变量在 install.sh 里替换。
set -u

SCRIPTPATH="$(dirname "$(realpath "$0")")"
KR_BIN="$SCRIPTPATH/bin/KlipperScreen-esp"

XDG_RUNTIME_DIR="/run/user/$(id -u)"
export XDG_RUNTIME_DIR
export KLIPPER_FULLSCREEN=1

# Klipper 生态上位机：日志落到 printer_data/logs，fluidd/mainsail 可直接下载。
# 无 printer_data 时保持 systemd journal 输出（unit 里 StandardOutput=journal）。
LOG_DIR="${HOME:-}/printer_data/logs"
if [ -n "${HOME:-}" ] && [ -d "$LOG_DIR" ]; then
    LOG_FILE="$LOG_DIR/KlipperScreen-esp.log"
    # 超 4MB 截断保留尾部 2MB，避免长期运行撑爆磁盘
    if [ -f "$LOG_FILE" ] && [ "$(stat -c %s "$LOG_FILE" 2>/dev/null || echo 0)" -gt 4194304 ]; then
        tail -c 2097152 "$LOG_FILE" > "$LOG_FILE.tmp" && mv "$LOG_FILE.tmp" "$LOG_FILE"
    fi
    exec >> "$LOG_FILE" 2>&1
    echo "=== KlipperScreen-esp starting $(date) ==="
fi

start_x11() {
    echo "KlipperScreen-esp on X11"
    export SDL_VIDEODRIVER=x11
    exec /usr/bin/xinit "$KR_BIN"
}

start_weston() {
    echo "KlipperScreen-esp on Wayland (weston kiosk)"

    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"

    export WAYLAND_DISPLAY=wayland-0
    export SDL_VIDEODRIVER=wayland

    rm -f "$XDG_RUNTIME_DIR/wayland-0"
    rm -f "$XDG_RUNTIME_DIR/wayland-0.lock"

    /usr/bin/weston \
        --backend=drm-backend.so \
        --socket="$WAYLAND_DISPLAY" \
        --shell=kiosk-shell.so \
        --idle-time=0 &

    WESTON_PID=$!

    cleanup() {
        kill "$WESTON_PID" 2>/dev/null
        wait "$WESTON_PID" 2>/dev/null
    }
    trap cleanup EXIT

    for _ in $(seq 1 50); do
        if [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
            echo "Weston ready"
            break
        fi
        if ! kill -0 "$WESTON_PID" 2>/dev/null; then
            echo "Weston exited unexpectedly"
            exit 1
        fi
        sleep 0.1
    done

    # 不能 exec app：exec 会替换掉脚本进程，cleanup trap 随之消失；systemd
    # 停服务时 weston 成为孤儿继续占着 DRM/logind 会话，下一次启动的 weston
    # 报 "no drm device found"。前台跑 app，trap 里连 weston 一起收。
    "$KR_BIN" &
    APP_PID=$!
    cleanup_all() {
        kill "$APP_PID" 2>/dev/null
        cleanup
    }
    trap cleanup_all EXIT TERM INT
    wait "$APP_PID" 2>/dev/null
}

if [[ "${BACKEND:-W}" =~ ^[xX]$ ]]; then
    if command -v xinit >/dev/null 2>&1; then
        start_x11
    else
        echo "xinit not found, exiting..."
        exit 1
    fi
else
    if command -v weston >/dev/null 2>&1; then
        start_weston
    else
        echo "weston not found, exiting..."
        exit 1
    fi
fi
