#!/usr/bin/env bash
# 查看虚拟打印机状态
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
HERE="$(pwd)"

for name in fake_klippy moonraker; do
    pidfile="$HERE/runtime/$name.pid"
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name: 运行中 (pid $(cat "$pidfile"))"
    else
        echo "$name: 未运行"
    fi
done
if curl -s -m 2 "http://127.0.0.1:7125/server/info" 2>/dev/null; then
    echo ""
else
    echo "moonraker API 不可达"
fi
