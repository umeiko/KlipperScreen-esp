#!/usr/bin/env bash
# host 端 bambu_status_stream 单元测试（独立于 desktop/ESP 构建）。
# Windows 走 tools/msys64 的 ucrt64 gcc；其它平台用系统 cc。
# --wrap malloc/calloc/realloc/free：断言解析器全程零堆调用。
set -e
cd "$(dirname "$0")/.."

SRC="tests/test_bambu_status_stream.c src/core/bambu_status_stream.c"
FLAGS="-std=c99 -Wall -Wextra -Werror -O2 -Isrc/core \
    -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,realloc -Wl,--wrap,free"

if [ -x tools/msys64/usr/bin/bash.exe ]; then
    tools/msys64/usr/bin/bash.exe -lc '
set -e
export PATH=/ucrt64/bin:$PATH
cd "'"$PWD"'"
mkdir -p build-tests
gcc '"$FLAGS"' '"$SRC"' -o build-tests/test_bambu_status_stream.exe
./build-tests/test_bambu_status_stream.exe
'
else
    mkdir -p build-tests
    cc $FLAGS $SRC -o build-tests/test_bambu_status_stream
    ./build-tests/test_bambu_status_stream
fi
