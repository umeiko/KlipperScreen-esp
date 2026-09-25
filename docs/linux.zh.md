# Linux 上位机

在 3D 打印机的 Linux 上位机（跑 Klipper/Moonraker 的那台）上直接运行 KlipperScreen-esp——一个零依赖编译、对标 KlipperScreen 部署方式的轻量替代品。退役手机/平板刷 Linux chroot/proot 也一样能用。

![红米4（aarch64 Ubuntu）通过 weston kiosk 后端全屏运行 KlipperScreen-esp](screenshots/boards/linux_redmi4.jpg)

*退役红米4（aarch64，Ubuntu 24.04）当作 Klipper 上位机显示屏：systemd 服务 + weston kiosk shell 全屏运行。*

## 一行安装

```bash
curl -fsSL https://raw.githubusercontent.com/umeiko/KlipperScreen-esp/main/scripts/linux/install.sh | bash
```

安装脚本会自动：

1. 识别架构（`x86_64` / `arm64`），从最新 release 下载对应的预编译包——上位机不需要编译任何东西
2. 询问你要 **独占显示服务** 还是普通 **桌面 App**：
   - **服务（默认）**：写入 `KlipperScreen-esp.service` systemd 单元，开机自启全屏显示，图形后端可选 Wayland（weston kiosk shell）或 X11（裸 xinit），所需软件包（`weston` 或 `xinit`）自动安装。检测到 `KlipperScreen.service` 时会询问是否停用它，避免两个程序抢屏幕
   - **桌面 App**：只装二进制 + 应用菜单里的启动快捷方式
3. 安装到 `~/.local/share/KlipperScreen-esp/`；配置存放在 `~/.config/KlipperScreen-esp/`

要求：Debian/Ubuntu 系（带 `apt`），glibc ≥ 2.35（Debian 12 / Ubuntu 22.04 及以上）。二进制静态链接 SDL2 与 cJSON，X11/Wayland 系统库仅运行时加载。

## 手动安装

从 [Releases](https://github.com/umeiko/KlipperScreen-esp/releases/latest) 下载 `desktop-linux-x86_64.tar.gz` 或 `desktop-linux-arm64.tar.gz`，解压后运行 `./install.sh`。脚本化部署可用非交互环境变量：

```bash
SERVICE=n ./install.sh                  # 只装桌面 App
KR_BACKEND=x11 ./install.sh             # 服务模式，强制 X11
KR_BACKEND=wayland KR_START=0 ./install.sh
```

卸载用包内附带的 `./uninstall.sh`。

## 说明

- 720p 及以上分辨率自动切换到加大字号/图标档；高分辨率 Linux 上位机跳过开机动画，启动更快。
- 触摸走内核 evdev/libinput，weston 和 xinit 后端都会透传。
- 想从源码构建见 [从源码构建](building.md)。
