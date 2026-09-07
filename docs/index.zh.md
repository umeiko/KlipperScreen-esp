# KlipperScreen-esp

Klipper 3D 打印机的**触摸屏远程显示器**：运行在廉价的 ESP32 开发板上，通过 WiFi 与 **Moonraker** 通信。可以把它理解为一个口袋大小、无线的 KlipperScreen。

![实机照片](screenshots/main_photo.jpg)

同一套 UI 代码还能编译成 **桌面模拟器**（SDL2，Windows/Linux），所有面板都可以在不刷机的情况下开发和截图验证。

## 功能特性

- **主界面** — 喷嘴/热床/腔体温度卡片、打印进度、一键操作
- **G-code 文件** — 缩略图、元数据、历史记录，打印/删除
- **控制** — 轴点动与回零、冷挤出保护的挤进/回抽、温度预设（PLA/PETG/ABS/冷却）、带确认的紧急停止与固件重启
- **稳健连接** — WebSocket 自动重连、应用层心跳与 RTT 显示、僵尸连接检测、Klipper 错误 toast（如限位未触发）
- **杂项** — "Umeko" 开机动画、5 种语言（EN / 简中 / 繁中 / FR / IT，切换时淡黑重启）、亮度滑条、自动息屏触摸唤醒、标题栏时钟从 Moonraker 主机同步（无需联网）
- **一次性触摸校准**持久化到 flash；2432S028R 出厂校准已内置

## 支持的板子

| 板型 | 屏幕 | 触摸 | 主控 | 状态 |
|---|---|---|---|---|
| CYD 2432S028R | 2.8" 320×240 ILI9341 SPI | XPT2046 电阻 | ESP32 | ✅ 稳定 |
| E32R35T（ESP32-32E 3.5"） | 3.5" 480×320 ST7796 SPI | XPT2046 电阻（共总线） | ESP32-32E | ✅ 稳定 |
| JC8048W550 | 5" 800×480 ST7262 RGB 并口 | GT911 电容 | ESP32-S3 | ✅ 稳定 |

详细引脚与硬件参数见[支持的板子](boards.md)。

## 快速开始

1. 从 [Releases](https://github.com/umeiko/KlipperScreen-esp/releases) 下载对应板型的 zip，解压后 `flash.bat COMx`（Windows）或 `./flash.sh /dev/ttyUSB0`
2. 设置 → 无线网络：扫描 → 选 AP → 输密码
3. 设置 → Moonraker：选打印机槽位（最多 6 台），填主机 IP + 端口（默认 7125）

自行编译见仓库 README；把固件移植到自己的板子见[移植指南](porting.md)。

## 文档导航

- [支持的板子](boards.md) — 现有板型的硬件信息与引脚定义
- [移植到自己的开发板](porting.md) — BSP 接口契约与实现要点
- [贡献新板型（PR 指南）](contributing-board.md) — 提交 PR 需要改哪些文件、验证什么

## 许可证

GPLv3。仓库：[github.com/umeiko/KlipperScreen-esp](https://github.com/umeiko/KlipperScreen-esp)
