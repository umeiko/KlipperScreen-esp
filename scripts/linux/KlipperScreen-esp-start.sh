#!/bin/bash
# KlipperScreen-esp 启动器：按 BACKEND 拉起 X11(xinit) 或 Wayland(weston kiosk)。
# 由 KlipperScreen-esp.service 调用；模板变量在 install.sh 里替换。
set -u

SCRIPTPATH="$(dirname "$(realpath "$0")")"
KR_BIN="$SCRIPTPATH/bin/KlipperScreen-esp"

XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR
export KLIPPER_FULLSCREEN=1

# Klipper 生态上位机：日志落到 printer_data/logs，fluidd/mainsail 可直接下载。
# 无 printer_data 时保持 systemd journal 输出（unit 里 StandardOutput=journal）。
LOG_DIR="${HOME:-}/printer_data/logs"
if [ -n "${HOME:-}" ] && [ -d "$LOG_DIR" ]; then
    LOG_FILE="$LOG_DIR/KlipperScreen-esp.log"
    # 按自然日轮转：旧日志改名 .1 只留一天（崩溃排查刚好够用），再开新文件
    if [ -f "$LOG_FILE" ] && [ "$(date -r "$LOG_FILE" +%F 2>/dev/null)" != "$(date +%F)" ]; then
        mv "$LOG_FILE" "$LOG_FILE.1"
    fi
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

    # 独立 socket：不删除桌面会话/其他服务的 wayland-0 与 lock。
    export WAYLAND_DISPLAY=klipperscreen-esp
    export SDL_VIDEODRIVER=wayland

    WESTON_PID=
    APP_PID=
    cleanup_all() {
        # 只清理本次启动的子进程。先 TERM，最多等 5s，再 KILL，不能因
        # 损坏的显示栈永久卡在 wait 里（不可中断的内核 D 状态仍需内核恢复）。
        trap '' TERM INT HUP
        local pid running
        local -a children=()
        for pid in "$APP_PID" "$WESTON_PID"; do
            [ -n "$pid" ] && children+=("$pid")
        done
        if [ "${#children[@]}" -gt 0 ]; then
            kill -TERM "${children[@]}" 2>/dev/null || true
            for _ in {1..50}; do
                running=0
                for pid in "${children[@]}"; do
                    kill -0 "$pid" 2>/dev/null && running=1
                done
                [ "$running" = 0 ] && break
                sleep 0.1
            done
            kill -KILL "${children[@]}" 2>/dev/null || true
            # 不阻塞等待内核 D 状态；systemd 的 KillMode=control-group
            # 会继续收尾。正常退出/被 KILL 的子进程由 bash 自动回收。
        fi
    }
    trap cleanup_all EXIT
    trap 'exit 143' TERM
    trap 'exit 130' INT
    trap 'exit 129' HUP

    weston \
        --backend=drm-backend.so \
        --socket="$WAYLAND_DISPLAY" \
        --shell=kiosk-shell.so \
        --idle-time=0 &

    WESTON_PID=$!

    local ready=0
    for _ in {1..50}; do
        if ! kill -0 "$WESTON_PID" 2>/dev/null; then
            echo "Weston exited before becoming ready"
            return 1
        fi
        if [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
            echo "Weston ready"
            ready=1
            break
        fi
        sleep 0.1
    done
    if [ "$ready" = 0 ]; then
        echo "Weston readiness timed out after 5s; not starting the app"
        return 1
    fi

    # 两个子进程共同监督：仅 wait APP_PID 会漏掉 Weston 的 SIGABRT，
    # SDL kiosk 又会忽略 close，导致应用活着但永远黑屏、systemd 不重启。
    "$KR_BIN" &
    APP_PID=$!
    local result=0
    wait -n "$WESTON_PID" "$APP_PID" || result=$?
    echo "Wayland session child exited (status=$result); restarting the whole display session"
    # 即使子进程正常退出也视为 kiosk 会话结束，由 systemd Restart=always
    # 在清理完另一端后重建；不在同一脚本内重复启动/积累僵尸会话。
    [ "$result" = 0 ] && result=1
    return "$result"
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
