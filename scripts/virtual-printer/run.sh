#!/usr/bin/env bash
# 启动虚拟打印机：fake_klippy（模拟下位机+klippy） + 真实 Moonraker。
# 日志在 runtime/printer_data/logs/，PID 在 runtime/*.pid。
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
HERE="$(pwd)"
DATA="$HERE/runtime/printer_data"
PY="$HERE/runtime/venv/bin/python"
[ -x "$PY" ] || { echo "未安装，请先运行 install.sh"; exit 1; }
[ -f "$DATA/config/moonraker.conf" ] || { echo "缺 runtime 配置，请先运行 install.sh"; exit 1; }

KLIPPY_ADDRESS="$(cat "$HERE/runtime/klippy_address")"

is_running() {  # pid_file
    [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null
}

start_klippy() {
    if is_running "$HERE/runtime/fake_klippy.pid"; then
        echo "fake_klippy 已在运行 (pid $(cat "$HERE/runtime/fake_klippy.pid"))"
        return
    fi
    nohup "$PY" "$HERE/fake_klippy.py" \
        --config "$DATA/config/printer.cfg" \
        --socket "$KLIPPY_ADDRESS" \
        --gcodes "$DATA/gcodes" \
        --log "$DATA/logs/klippy.log" \
        --klipper-repo "$HERE/repos/klipper" \
        ${FAKE_KLIPPY_BPS:+--bps "$FAKE_KLIPPY_BPS"} \
        >> "$DATA/logs/fake_klippy.stdout.log" 2>&1 &
    echo $! > "$HERE/runtime/fake_klippy.pid"
    echo "fake_klippy 已启动 (pid $!)"
}

wait_klippy() {
    local i
    for i in $(seq 1 50); do
        if [[ "$KLIPPY_ADDRESS" == tcp://* ]]; then
            local hp="${KLIPPY_ADDRESS#tcp://}"
            (exec 3<>"/dev/tcp/${hp%:*}/${hp##*:}") 2>/dev/null && {
                exec 3>&- 3<&-; return 0; }
        else
            [ -S "$KLIPPY_ADDRESS" ] && return 0
        fi
        sleep 0.2
    done
    echo "警告: klippy 接口未就绪: $KLIPPY_ADDRESS" >&2
    return 1
}

start_moonraker() {
    if is_running "$HERE/runtime/moonraker.pid"; then
        echo "moonraker 已在运行 (pid $(cat "$HERE/runtime/moonraker.pid"))"
        return
    fi
    nohup "$PY" "$HERE/repos/moonraker/moonraker/moonraker.py" \
        -d "$DATA" \
        -c "$DATA/config/moonraker.conf" \
        -l "$DATA/logs/moonraker.log" \
        >> "$DATA/logs/moonraker.stdout.log" 2>&1 &
    echo $! > "$HERE/runtime/moonraker.pid"
    echo "moonraker 已启动 (pid $!)"
}

wait_moonraker() {
    local i
    for i in $(seq 1 75); do
        if curl -s -m 2 -o /dev/null "http://127.0.0.1:7125/server/info"; then
            return 0
        fi
        sleep 0.2
    done
    echo "警告: moonraker 7125 端口未就绪" >&2
    return 1
}

start_klippy
wait_klippy || true
start_moonraker
wait_moonraker && {
    # 探测局域网 IP（连一个外部地址取本地出口 IP，不真的发流量）
    LAN_IP="$("$PY" -c 'import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.connect(("192.168.1.1", 80)); print(s.getsockname()[0])
except OSError:
    print("<本机IP>")
finally:
    s.close()' 2>/dev/null || echo "<本机IP>")"
    echo ""
    echo "虚拟打印机就绪:"
    echo "  Moonraker API:  http://$LAN_IP:7125   (本机: http://127.0.0.1:7125)"
    echo "  ESP32/局域网设备填上面第一个地址；Windows 首次可能弹防火墙，放行 python 即可"
    echo "  klippy 传输:    $KLIPPY_ADDRESS  (仅本机 Moonraker 用，无需对局域网开放)"
    echo "  日志:           $DATA/logs/"
    echo "  gcode 目录:     $DATA/gcodes/"
}
