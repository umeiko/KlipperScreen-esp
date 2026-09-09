#!/usr/bin/env bash
# 构建 desktop 后端，输出 src/ports/desktop/build/
# Windows: 真实控制端 + 开发模拟器；Linux: 开发模拟器。
# 依赖: tools/msys64（便携 MSYS2，ucrt64 gcc/cmake/ninja/SDL2/cJSON）
set -e
MSYS_BASH=tools/msys64/usr/bin/bash.exe

$MSYS_BASH -lc '
set -e
export PATH=/ucrt64/bin:$PATH
cd "'"$PWD"'/src/ports/desktop"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp -f /ucrt64/bin/SDL2.dll build/ 2>/dev/null || true
echo BUILD_OK
ls -la build/klipper_remote_desktop.exe build/klipper_remote_simulator.exe 2>/dev/null || ls -la build/klipper_remote_desktop
'
