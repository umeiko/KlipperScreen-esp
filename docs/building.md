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
