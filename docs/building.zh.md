# 从源码构建

本文面向开发者：搭建工具链、构建桌面端与各板型固件、重新生成字体与图标资源。只刷机不需要看这里——release 刷机包见[支持的板子](boards.md)。

## 编译依赖

1. 安装 **ESP-IDF** v5.5.5（ESP32 固件）。
2. 安装 **便携 MSYS2**（desktop 后端的 MinGW 编译环境）：
   ```bash
   curl -L -o tools/dl/msys2-base.tar.xz https://mirrors.tuna.tsinghua.edu.cn/msys2/distrib/x86_64/msys2-base-x86_64-20260611.tar.xz
   mkdir -p tools/msys64 && tar -xf tools/dl/msys2-base.tar.xz -C tools/msys64 --strip-components=1
   tools/msys64/usr/bin/bash.exe -lc "echo ok"   # 首次运行完成初始化
   echo 'Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/msys/$arch'  > tools/msys64/etc/pacman.d/mirrorlist.msys
   echo 'Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/$repo' > tools/msys64/etc/pacman.d/mirrorlist.mingw
   tools/msys64/usr/bin/bash.exe -lc "pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cjson"
   ```
3. 克隆 **LVGL** 源码：`git clone --depth 1 -b release/v9.3 https://gitee.com/mirrors/lvgl.git third_party/lvgl`

## 构建命令

```bash
# 桌面端（Windows：真实控制端 + 开发模拟器；Linux/macOS：桌面目标）
bash tools/build-desktop.sh
./src/ports/desktop/build/KlipperScreen-esp.exe              # 真实 Moonraker 控制端
./src/ports/desktop/build/klipper_remote_simulator.exe            # 布局模拟器
./src/ports/desktop/build/klipper_remote_simulator.exe 3000 x.bmp # 运行 3 秒后截图（开发用）

# ESP32 固件（多板型，脚本封装 idf.py）
bash tools/build-esp32.sh <board> build          # 只构建
bash tools/build-esp32.sh <board> flash COMx     # 构建并烧录
bash tools/build-esp32.sh all build              # 全部板型
```

board 取值：`cyd_2432s028r` / `cyd_2432s028r_plus` / `e32r35t` / `esp32s3-st7789-320_240-ec11` / `esp32-st7735s-128_160-ec11` / `esp32-st7789-320_240-ec11` / `esp32-ILI9341-320_240-ec11` / `esp32-ST7796-320_240-ec11` / `esp32s3-st7796-480_320-xpt2046-ec11` / `esp32s3-ILI9488-480_320-xpt2046-ec11` / `esp32s3-ILI9341-320_240-xpt2046-ec11` / `jc8048w550` / `esp32s3-JLC-SZP` / `esp32s3-retro-go` / `esp32c3-st7789-320_240-ec11`。每板型有独立的构建目录与 sdkconfig（芯片目标不同，不能混用）。

在 Windows 上直接调 `idf.py` 要走 `tools/idf.ps1` 包装——Git Bash 会把 `MSYSTEM` 注入子进程，导致 `idf.py` 静默空转。

## 字体生成与替换

界面里所有字符串字面量的非 ASCII 字符会被自动提取，生成 CJK 子集字体。**改了任何 UI 字符串后必须重新生成**，否则新字显示为 □：

```bash
# 默认用 C:/Windows/Fonts/simhei.ttf，可 --font 换
python tools/fontgen/gen_fonts.py
```

## 图标生成与替换（assets/icons.h）

界面图标的 SVG 原件在 `src/ui/assets/svg/`。新增/替换 SVG 后重新生成：

```bash
# 首次：装依赖（Node 端 resvg + Python 端 pypng/lz4）
cd tools/icongen && npm install --registry=https://registry.npmmirror.com
python -m venv .venv && .venv/Scripts/python -m pip install pypng lz4
cd ../..
# 新增/替换 SVG 后重新生成：
node tools/icongen/gen_icons.mjs
```
