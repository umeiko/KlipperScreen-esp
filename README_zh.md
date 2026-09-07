# Klipper Remote ESP32 Displays（中文文档）

[English README](README.md)

<p align="center">
  <img src="docs/screenshots/main_photo.jpg" alt="CYD 2432S028R 实机运行效果" width="720">
</p>

基于 ESP32 CYD 系列开发板的 Klipper 远程触屏显示器（对标 KlipperScreen），通过 WiFi 连接 Moonraker。多后端架构：所有界面/业务代码共享，每个后端（ESP32 各板型、desktop SDL2、未来的 Pico/STM32…）地位平等。

## 界面实拍

完整界面截图见文档站：**[界面展示 / Screenshots](https://umeiko.github.io/KlipperScreen-esp/zh/screenshots/)**

- 架构设计：[docs/architecture.md](docs/architecture.md)
- Klipper/Moonraker API 参考：[docs/klipper-moonraker-api.md](docs/klipper-moonraker-api.md)
- LVGL v9 API 防错笔记：[docs/lvgl-v9-api-notes.md](docs/lvgl-v9-api-notes.md)

## 功能

- 多打印机：最多 6 个 Moonraker 槽位，3×2 切换页一键换机，秒级重连
- 实时状态：标题栏喷嘴/热床温度、状态卡按状态整卡变色（空闲绿/异常红/断连黄）
- 打印任务：G-code 历史列表、二级菜单打印/删除、进度环 + 已用/剩余时间、暂停/恢复/取消
- 控制：轴点动/归零、挤出/回抽（冷挤出保护）、温度预设（PLA/PETG/ABS/冷却）、急停/下位机重启（带确认）
- 链路健壮：WS 自动重连、应用层心跳 RTT 显示、僵尸连接检测、Klipper 报错 toast（如限位未触发）
- 体验细节：「Umeko」开机动画、5 种语言（EN/简中/繁中/FR/IT，切换时渐暗到黑再重启）、背光滑杆、自动息屏（15秒~1小时/永不）触摸唤醒、标题栏时钟（从 Moonraker 上位机对时，纯内网）
- 触摸两点校准一次持久化到 flash；2432S028R 预置出厂校准参数

## 技术栈

ESP-IDF v5.5.5 · LVGL v9.3 · 多后端（ESP32 各 CYD 板型 / desktop SDL2：Windows+Linux / 未来 Pico SDK、STM32…）

## 刷机（免编译）

从 [Releases](../../releases) 或 CI artifacts 下载 `klipper-remote-esp32-*.zip`，解压后：

- **Windows**：`flash.bat COM6`（zip 内含 esptool.exe，无需装 Python）
- **macOS / Linux**：`./flash.sh /dev/ttyUSB0`（需 `pip install esptool`）

支持板型：**CYD 2432S028R**（2.8" 电阻屏）；**E32R35T**（ESP32-32E 3.5" 480×320 ST7796 电阻屏）；**JC8048W550**（Guition 5" 800×480 RGB 电容屏，ESP32-S3）。三款自 v0.2.0 起均为正式支持，排坑记录见 [docs/jc8048w550-rgb-display-guide.md](docs/jc8048w550-rgb-display-guide.md)。

首次启动自动格式化 LittleFS 分区并写入出厂触摸校准参数。

## 首次配置

1. 设置 → 无线网络：扫描 → 选 AP → 输密码，存入 `network.conf`
2. 设置 → Moonraker：先选打印机槽位（最多 6 台），填主机 IP + 端口（默认 7125）+ 可选 API Key，存入 `moonraker.conf`
3. 语言/背光/自动息屏等偏好存入 `klipperscreen.conf`

串口 CLI（115200 8N1）可调试：`help` / `wifi` / `mr` / `printer <1-6>` / `mrstart` / `gc` / `status` / `ps` / `ls` / `cd` / `cat` / `rm` …

## 目录

```
src/                      # 全部源码
  ui/                     #   界面（所有后端共享）：面板/主题/双语/图标/开机动画
  core/                   #   Moonraker 客户端（WS+JSON-RPC）/ 打印数据层 / 配置
  bsp/                    #   板级支持：功能→架构两级
    bsp.h                 #     BSP 接口契约
    esp32/                #     ESP32 家族各板（bsp_cyd_2432s028r.c 等）
    desktop/              #     桌面 SDL2 BSP
  ports/                  #   每个后端一个可构建工程（构建胶水 + 入口）
    esp32/                #     ESP-IDF 工程（idf.py 在此目录运行）
    desktop/              #     CMake 工程（SDL2，Win/Linux）
tools/                  # 构建脚本与便携工具链（非源码）
  release/              #   刷机脚本（flash.bat / flash.sh，CI 打包进 zip）
third_party/lvgl/       # LVGL v9.3（git clone，不入库）
docs/                   # 设计文档
.github/                # CI：ESP-IDF 构建 → 可刷机 zip（打 v* tag 自动发 Release）
```

新增后端 = `src/bsp/<架构>/` 加一个 BSP 实现 + `src/ports/<架构>/` 加一个工程壳；同一架构的新板型只需在 `src/bsp/<架构>/` 里加文件。

## 环境准备（Windows，全程不依赖 GitHub）

1. **ESP-IDF**：离线安装器安装 v5.5.5（本机位于 `C:\esp\v5.5.5\esp-idf`，工具链 `C:\Espressif\tools`）。
   注意：Git Bash 的 MSYS2 运行时会向子进程注入 `MSYSTEM`，导致 `idf.py` 静默空转——
   必须通过 `tools/idf.ps1` 包装器调用（内部用 EIM profile 激活并移除该变量）。
2. **便携 MSYS2**（desktop 后端的 MinGW 编译环境）：
   ```bash
   curl -L -o tools/dl/msys2-base.tar.xz https://mirrors.tuna.tsinghua.edu.cn/msys2/distrib/x86_64/msys2-base-x86_64-20260611.tar.xz
   mkdir -p tools/msys64 && tar -xf tools/dl/msys2-base.tar.xz -C tools/msys64 --strip-components=1
   tools/msys64/usr/bin/bash.exe -lc "echo ok"   # 首次运行完成初始化
   echo 'Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/msys/$arch'  > tools/msys64/etc/pacman.d/mirrorlist.msys
   echo 'Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/$repo' > tools/msys64/etc/pacman.d/mirrorlist.mingw
   tools/msys64/usr/bin/bash.exe -lc "pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2"
   ```
3. **LVGL**：`git clone --depth 1 -b release/v9.3 https://gitee.com/mirrors/lvgl.git third_party/lvgl`

## 构建

```bash
# desktop 后端（产物 src/ports/desktop/build/klipper_remote_desktop.exe）
bash tools/build-desktop.sh
./src/ports/desktop/build/klipper_remote_desktop.exe            # 交互窗口
./src/ports/desktop/build/klipper_remote_desktop.exe 3000 x.bmp # 3 秒后截图退出

# ESP32 后端（CYD 2432S028R，工程目录 src/ports/esp32）
cd src/ports/esp32
powershell -NoProfile -ExecutionPolicy Bypass -File ../../../tools/idf.ps1 build
powershell -NoProfile -ExecutionPolicy Bypass -File ../../../tools/idf.ps1 -p COMx flash monitor
```

## 中文字体（改了 UI 文案后必跑）

界面里所有字符串字面量的非 ASCII 字符会被自动提取，生成 CJK 子集字体：

```bash
python tools/fontgen/gen_fonts.py        # 默认用 C:/Windows/Fonts/simhei.ttf，可 --font 换
```

新增中文后不重跑就会出现方框（□）。生成后需重新编译对应后端。

## 图标（assets/icons.h）

界面图标来自 KlipperScreen 的 material-dark 主题（GPL-3.0），SVG 原件在 `src/ui/assets/svg/`，
经 resvg 渲染 + LVGLImage.py 转成 A8 alpha 图（体积小，运行时用 `theme_img()` 的 recolor 任意着色）：

```bash
# 首次：装依赖（Node 端 resvg + Python 端 pypng/lz4）
cd tools/icongen && npm install --registry=https://registry.npmmirror.com
python -m venv .venv && .venv/Scripts/python -m pip install pypng lz4
cd ../..
# 新增/替换 SVG 后重新生成：
node tools/icongen/gen_icons.mjs
```

## WiFi 配置

设置 → 无线网络：扫描 → 选 AP → 输密码 → 连接（转圈）→ toast 反馈。
三端实现共用 `src/bsp/bsp_wifi.h` 轮询接口：

- **ESP32**：`src/bsp/esp32/bsp_wifi_esp32.c`（esp_wifi 事件驱动）
- **Windows**：`src/bsp/desktop/bsp_wifi_windows.c`（netsh wlan，自动适配中英文系统输出）
- **Linux**：`src/bsp/desktop/bsp_wifi_linux.c`（nmcli，需 NetworkManager）
