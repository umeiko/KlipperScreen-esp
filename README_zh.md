# KlipperScreen-esp（中文文档）

[English README](README.md)

> **📖 文档站：https://umeiko.github.io/KlipperScreen-esp/zh/**


<p align="center">
  <img src="docs/screenshots/main_photo.png" alt="CYD 2432S028R 实机运行效果" width="720">
</p>

**KlipperScreen-esp** 是一款紧凑的跨平台 3D 打印机显示与控制软件，可运行在低成本 ESP32 开发板以及 Windows/macOS 桌面端。它通过 **Moonraker** 完整控制 **Klipper** 打印机，并提供 **拓竹云端状态监视** 


## 界面实拍

完整界面截图见文档站：**[Screenshots](https://umeiko.github.io/KlipperScreen-esp/zh/screenshots/)**



## 功能
- 无线与控制，对Klipper上位机**零侵入**，不影响上位机性能
- 同时支持 **拓竹** 与 **Klipper** 3D打印机的管理与切换
- 一屏多打印机管理：最多 6 个打印机槽位，可分别对应 Klipper 或拓竹，一键换机，秒级重连
- 控制：轴点动/归零、挤出/回抽、温度预设（PLA/PETG/ABS/冷却）、急停/下位机重启
- 打印状态监控，暂停，温度控制。同时支持Klipper与拓竹打印机



## 技术栈

ESP-IDF v5.5.5 · LVGL v9.3 · 多后端（ESP32, Windows, Linux, MacOS）

## 刷机（免编译）

从 [Releases](../../releases) 下载对应版型的刷机包 `*.zip`，解压后：

- **Windows**：双击 `*.bat`脚本，根据提示刷机即可。
- **macOS / Linux**：在终端内`pip install esptool`后， `./flash.sh /dev/ttyUSB0`

支持板型：最常见的各种CYD黄色esp32开发板，以及esp32s3开发板。预编译固件详见 **[支持的板子](https://umeiko.github.io/KlipperScreen-esp/zh/boards/)**

## Linux 上位机（免编译，对标 KlipperScreen 部署）

一行安装（自动下载匹配架构的最新 release，交互式选择安装形态）：

```bash
curl -fsSL https://raw.githubusercontent.com/umeiko/KlipperScreen-esp/main/scripts/linux/install.sh | bash
```

也可以从 [Releases](../../releases) 下载 `desktop-linux-x86_64.tar.gz` 或 `desktop-linux-arm64.tar.gz`，解压后运行 `./install.sh`：

- 安装时可选**独占显示服务**（systemd 开机自启全屏，Wayland-weston 或 X11 后端）或普通**桌面 App**。
- 检测到 `KlipperScreen.service` 时会询问是否停用，避免抢屏。
- 二进制静态链接 SDL2/cJSON（X11/Wayland 库运行时加载），仅需 glibc ≥ 2.35（Debian 12 / Ubuntu 22.04 及以上，aarch64 与 x86_64）。
- 配置存 `~/.config/KlipperScreen-esp/`；卸载运行 `uninstall.sh`。


## 首次配置

1. 设置 → 无线网络：扫描 → 选 AP → 输密码，WIFI配置会保存，下次自动连接。
2. 设置 → 打印机连接设置：先选打印机槽位和机器模式（拓竹 或 Klipper）；Klipper 填主机 IP + 端口（默认 7125），拓竹则点击登录通过手机或邮箱登录，配置后会保存，下次自动连接。
3. 语言/背光/自动息屏等偏好设置皆为自动保存。

拓竹模式从“设置 → 打印机连接设置 → 机器模式 → 拓竹”进入。

串口 CLI 可用命令（115200 8N1）：`help` / `wifi` / `mr` / `printer <1-6>` / `mrstart` / `gc` / `status` / `ps` / `ls` / `cd` / `cat` / `rm` …


## 从源码构建

工具链：**ESP-IDF v5.5.5** · **LVGL v9.3** · SDL2（桌面端）。

```bash
bash tools/build-desktop.sh                       # 桌面端（控制端 KlipperScreen-esp + 模拟器）
bash tools/build-esp32.sh <board> build           # ESP32 固件（16 种板型）
bash tools/build-esp32.sh <board> flash COMx      # 构建并烧录
```

Linux 上位机也可从源码构建：`sudo apt install cmake libsdl2-dev libcjson-dev` 后进 `src/ports/desktop` 执行 `cmake -S . -B build && cmake --build build`。

便携 MSYS2 工具链安装、LVGL 克隆、多板型构建细节、中文字体子集与图标的重新生成，见文档站 **[从源码构建](https://umeiko.github.io/KlipperScreen-esp/zh/building/)**。


## 致谢

- [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen) — 界面设计灵感来源
- LVGL、ESP-IDF、esptool 等由各自作者维护

## 许可证

MIT
