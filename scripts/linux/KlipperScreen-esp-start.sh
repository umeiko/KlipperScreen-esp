#!/bin/bash
# KlipperScreen-esp 启动器：按 BACKEND 拉起 X11(xinit) 或 Wayland(weston kiosk)。
# 由 KlipperScreen-esp.service 调用；模板变量在 install.sh 里替换。
set -u

SCRIPTPATH="$(dirname "$(realpath "$0")")"
KR_BIN="$SCRIPTPATH/bin/KlipperScreen-esp"

XDG_RUNTIME_DIR="/run/user/$(id -u)"
export XDG_RUNTIME_DIR
export KLIPPER_FULLSCREEN=1

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

    exec "$KR_BIN"
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
