#!/usr/bin/env bash
# 停止虚拟打印机（fake_klippy + moonraker）
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
HERE="$(pwd)"

stop_one() {
    local name="$1" pidfile="$HERE/runtime/$2"
    if [ -f "$pidfile" ]; then
        local pid; pid="$(cat "$pidfile")"
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            for i in $(seq 1 25); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.2
            done
            kill -9 "$pid" 2>/dev/null || true
            echo "$name 已停止 (pid $pid)"
        fi
        rm -f "$pidfile"
    else
        echo "$name 未运行"
    fi
}
stop_one moonraker moonraker.pid
stop_one fake_klippy fake_klippy.pid
