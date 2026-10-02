# Building from source

For developers: toolchain setup, desktop and per-board firmware builds, and regenerating font/icon assets. If you only want to flash a release package, see [Supported boards](boards.md) instead.

## Prerequisites

1. Install **ESP-IDF** v5.5.5 (ESP32 firmware).
2. Install the **portable MSYS2** toolchain (MinGW environment for the desktop port):
   ```bash
   curl -L -o tools/dl/msys2-base.tar.xz https://mirrors.tuna.tsinghua.edu.cn/msys2/distrib/x86_64/msys2-base-x86_64-20260611.tar.xz
   mkdir -p tools/msys64 && tar -xf tools/dl/msys2-base.tar.xz -C tools/msys64 --strip-components=1
   tools/msys64/usr/bin/bash.exe -lc "echo ok"   # first run finishes MSYS2 init
   echo 'Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/msys/$arch'  > tools/msys64/etc/pacman.d/mirrorlist.msys
   echo 'Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/$repo' > tools/msys64/etc/pacman.d/mirrorlist.mingw
   tools/msys64/usr/bin/bash.exe -lc "pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cjson"
   ```
3. Clone **LVGL**: `git clone --depth 1 -b release/v9.3 https://gitee.com/mirrors/lvgl.git third_party/lvgl`
4. **Bambu POSIX backend dependencies** (only for the Linux desktop port and Android; the Windows desktop uses winhttp/OpenSSL and ESP32 uses IDF's mbedTLS):
   ```bash
   git clone --depth 1 --recurse-submodules --shallow-submodules -b v3.6.5 \
     https://github.com/Mbed-TLS/mbedtls.git third_party/mbedtls
   curl -L -o third_party/cacert.pem https://curl.se/ca/cacert.pem
   ```
   Android builds get these automatically via `tools/fetch-android-deps.sh`.

## Build commands

```bash
# Desktop (Windows: real controller + dev simulator; Linux/macOS: desktop targets)
bash tools/build-desktop.sh
./src/ports/desktop/build/KlipperScreen-esp.exe              # real Moonraker controller
./src/ports/desktop/build/klipper_remote_simulator.exe            # layout simulator
./src/ports/desktop/build/klipper_remote_simulator.exe 3000 x.bmp # screenshot after 3s (dev)

# ESP32 firmware (multi-board, script wraps idf.py)
bash tools/build-esp32.sh <board> build          # build only
bash tools/build-esp32.sh <board> flash COMx     # build and flash
bash tools/build-esp32.sh all build              # all boards
```

Board names: `cyd_2432s028r` / `cyd_2432s028r_plus` / `e32r35t` / `esp32s3-st7789-320_240-ec11` / `esp32-st7735s-128_160-ec11` / `esp32-st7789-320_240-ec11` / `esp32-ILI9341-320_240-ec11` / `esp32-ST7796-320_240-ec11` / `esp32s3-st7796-480_320-xpt2046-ec11` / `esp32s3-ILI9488-480_320-xpt2046-ec11` / `esp32s3-ILI9341-320_240-xpt2046-ec11` / `jc8048w550` / `esp32s3-JLC-SZP` / `esp32s3-retro-go` / `esp32c3-st7789-320_240-ec11`. Each board has its own build directory and sdkconfig (chip targets differ — never mix them).

On Windows, always call `idf.py` through the `tools/idf.ps1` wrapper — Git Bash injects `MSYSTEM` into child processes and makes `idf.py` silently no-op.

## Android (APK)

The Android port (`src/ports/android`) reuses the entire desktop UI/core codebase: an SDL2 Android project builds `libmain.so` via CMake (same entry point as `src/ports/desktop/main.c`), and Gradle packages the APK. Day-to-day packaging is done by the CI `android` job (artifact `android-apk-<ref>`). To build locally:

1. Install **JDK 17 + Android SDK** (platform android-34, build-tools 34.0.0, any NDK, CMake 3.22.1 — installing Android Studio covers all of these).
2. Fetch the native dependencies (SDL2 2.30.9 sources + cJSON + the SDLActivity glue; none of them are committed):
   ```bash
   bash tools/fetch-android-deps.sh
   ```
3. Build (`third_party/lvgl` is shared with the desktop port and must be fetched as well):
   ```bash
   cd src/ports/android
   bash gradlew assembleRelease          # add -PndkVersion=<ver> if your NDK differs from AGP's default
   # output: app/build/outputs/apk/release/app-release.apk
   ```

Notes: orientation fully follows the system (`fullSensor` — four-way when system auto-rotate is on, locked to the current orientation when off); rotating the device rebuilds the UI at the new resolution, and the in-app rotation setting is hidden on Android. Settings live in the app's private storage; release builds are currently signed with the debug key (sideloadable — switch to a proper key before shipping on the release page); WiFi is managed by the OS so the settings page has no WiFi entry (same as macOS); the brightness, screen-off and rotation rows are hidden on Android; local gcode thumbnails and self-update are Linux-host-only capabilities and are not offered; language and theme switching apply dynamically without restarting the app.

## Regenerating fonts

All non-ASCII characters in UI string literals are extracted automatically to build the CJK subset fonts. **Regenerate after changing any UI string**, otherwise new characters render as □:

```bash
# defaults to C:/Windows/Fonts/simhei.ttf, override with --font
python tools/fontgen/gen_fonts.py
```

## Regenerating icons (assets/icons.h)

Icon SVG sources live in `src/ui/assets/svg/`. After adding or replacing an SVG:

```bash
# first time: install dependencies (resvg on the Node side, pypng/lz4 on the Python side)
cd tools/icongen && npm install --registry=https://registry.npmmirror.com
python -m venv .venv && .venv/Scripts/python -m pip install pypng lz4
cd ../..
# regenerate after adding/replacing SVGs:
node tools/icongen/gen_icons.mjs
```
