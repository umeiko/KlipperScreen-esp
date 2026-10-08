# Windows / macOS 客户端

与 ESP32 板型同一套 UI 的桌面应用：完整的 Klipper/Moonraker 控制 + 拓竹云监视，鼠标键盘操作。它同时是开发载体——所有 UI 工作都先在桌面端调试。

**软件包**：[desktop-win-x86_64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-win-x86_64.zip) / [desktop-macos-arm64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-macos-arm64.zip)

## 快速开始

1. 下载对应平台的 zip 并解压。
2. 运行 **`KlipperScreen-esp`**（Windows 下为 `KlipperScreen-esp.exe`）。**不要**运行 `klipper_remote_simulator`——那是布局开发用的模拟器，喂的是假数据（假文件列表、无缩略图、不连真实 Moonraker）。
3. 设置 → 打印机连接：选一个槽位，选 Klipper（主机 IP + 端口，默认 7125）或拓竹模式。

Windows 下配置存放在 `%APPDATA%\KlipperRemote\moonraker.conf`。

## 说明

- **键盘导航**与实体板一致：方向键移动焦点，回车确认，Esc 返回。文本输入状态下方向键 + 回车在屏幕键盘上输入字符，**F1 提交表单**。
- **显示旋转** 0/90/180/270° 在 设置 → 显示 中调整（软件旋转，重启应用后生效）。
- **macOS**：二进制为 ad-hoc 签名。若 Gatekeeper 拒绝打开，对解压后的目录执行 `xattr -dr com.apple.quarantine`，或在 系统设置 → 隐私与安全性 中放行。
- 从源码构建（含假数据模拟器）见 [从源码构建](../building.md)。
