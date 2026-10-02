#!/usr/bin/env bash
# 拉取 Android 构建的外部依赖（均不入库，见 .gitignore）：
#   third_party/SDL2   —— SDL2 2.30.9 源码（与 CI 的 Linux 桌面静态构建同版本）
#   third_party/cjson  —— cJSON 1.7.18 源码（app/jni/CMakeLists.txt 单文件编译）
#   src/ports/android/app/src/main/java/org/libsdl/app/*.java
#     —— SDLActivity 等胶水层，随 SDL 版本走，构建前拷入 Android 工程
# LVGL 不在本脚本范围：与 desktop 端口共用 third_party/lvgl（见 build.yml 内联步骤）。
# 本地走代理时按惯例：https_proxy=http://127.0.0.1:7890 bash tools/fetch-android-deps.sh
set -e
cd "$(dirname "$0")/.."

SDL_VER=2.30.9
CJSON_VER=1.7.18

if [ ! -f third_party/SDL2/CMakeLists.txt ]; then
    echo "== fetch SDL2 $SDL_VER"
    curl -fL --retry 5 --retry-all-errors -o /tmp/sdl2.tar.gz \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
    tar xzf /tmp/sdl2.tar.gz -C /tmp
    mkdir -p third_party
    rm -rf third_party/SDL2
    mv "/tmp/SDL2-$SDL_VER" third_party/SDL2
fi

if [ ! -f third_party/cjson/cJSON.c ]; then
    echo "== fetch cJSON $CJSON_VER"
    curl -fL --retry 5 --retry-all-errors -o /tmp/cjson.tar.gz \
        "https://github.com/DaveGamble/cJSON/archive/refs/tags/v$CJSON_VER.tar.gz"
    tar xzf /tmp/cjson.tar.gz -C /tmp
    mkdir -p third_party
    rm -rf third_party/cjson
    mv "/tmp/cJSON-$CJSON_VER" third_party/cjson
fi

JAVA_DST=src/ports/android/app/src/main/java/org/libsdl/app
if [ ! -f "$JAVA_DST/SDLActivity.java" ]; then
    echo "== copy SDLActivity glue ($SDL_VER)"
    mkdir -p "$JAVA_DST"
    cp third_party/SDL2/android-project/app/src/main/java/org/libsdl/app/*.java "$JAVA_DST/"
fi

echo "Android deps ready: SDL2 $SDL_VER, cJSON $CJSON_VER"
