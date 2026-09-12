# KlipperScreen-esp

**KlipperScreen-esp** 是一款紧凑的跨平台 3D 打印机显示与控制软件，可运行在低成本 ESP32 开发板以及 Windows/macOS 桌面端。它通过 **Moonraker** 完整控制 **Klipper** 打印机，并在 Windows 端提供只读的**拓竹云端状态监视**；共享 LVGL 界面支持触摸、旋转编码器、鼠标、键盘或混合输入，EC11 参考板则展示了完全不使用触摸屏的纯旋钮设备。

![实机照片](screenshots/main_photo.jpg)

单独命名的 SDL2 **模拟器**使用 mock 数据，无需连接打印机即可用于布局开发、输入实验和界面截图。

## 功能特性

- **主界面** — 喷嘴/热床/腔体温度卡片、打印进度、一键操作
- **G-code 文件** — 缩略图、元数据、历史记录，打印/删除
- **控制** — 轴点动与回零、冷挤出保护的挤进/回抽、温度预设（PLA/PETG/ABS/冷却）、带确认的紧急停止与固件重启
- **稳健连接** — WebSocket 自动重连、应用层心跳与 RTT 显示、僵尸连接检测、Klipper 错误 toast（如限位未触发）
- **拓竹状态监视** — Windows 已有登录、验证码、账号设备选择和云端 MQTT 状态同步；云端模式只读，局域网 Developer Mode 控制路径预留给后续后端。
- **杂项** — "Umeko" 开机动画、5 种语言（EN / 简中 / 繁中 / FR / IT，切换时淡黑重启）、亮度滑条、自动息屏触摸唤醒、标题栏时钟从 Moonraker 主机同步（无需联网）
- **输入路线** — 无触摸旋钮板无需触摸层；电阻屏走板型专属校准流程；电容屏通常直接提供屏幕坐标；触摸和旋钮可以共存

## 支持的板子

| 板型 | 屏幕 | 触摸 | 主控 | 状态 |
|---|---|---|---|---|
| CYD 2432S028R | 2.8" 320×240 ILI9341 SPI | XPT2046 电阻 | ESP32 | ✅ 稳定 |
| E32R35T（ESP32-32E 3.5"） | 3.5" 480×320 ST7796 SPI | XPT2046 电阻（共总线） | ESP32-32E | ✅ 稳定 |
| EC11 旋钮最小系统 | 240×320 ST7789 SPI | 无，纯旋钮 | ESP32-S3 N16R8 | ✅ 官方参考，贡献者实机验证 |
| JC8048W550 | 5" 800×480 ST7262 RGB 并口 | GT911 电容 | ESP32-S3 | ✅ 稳定 |

详细引脚与硬件参数见[支持的板子](boards.md)。

## 快速开始

1. 从 [Releases](https://github.com/umeiko/KlipperScreen-esp/releases) 下载对应板型的 zip，解压后 `flash.bat COMx`（Windows）或 `./flash.sh /dev/ttyUSB0`
2. 设置 → 无线网络：扫描 → 选 AP → 输密码
3. 设置 → 打印机连接设置：选打印机槽位和 Klipper/拓竹模式；Klipper 再填写主机 IP + 端口（默认 7125），拓竹继续进入局域网或云端设置

自行编译见仓库 README；把固件移植到自己的板子见[移植指南](porting.zh.md)。移植指南第 5 步先选择“无触摸”或“有触摸”，有触摸时再区分电阻屏和电容屏。

## 文档导航

- [支持的板子](boards.zh.md) — 现有板型的硬件信息与引脚定义
- [移植到自己的开发板](porting.zh.md) — BSP 接口契约与实现要点
- [贡献新板型（PR 指南）](contributing-board.zh.md) — 提交 PR 需要改哪些文件、验证什么
- [界面展示](screenshots.zh.md) — 当前桌面模拟器英文界面截图
- [架构说明](architecture.md) — 共享 UI、BSP、桌面控制端与模拟器结构
- [拓竹接入说明](bambu-integration-architecture.md) — 当前只读云端边界与后续局域网路线

## 许可证

MIT。仓库：[github.com/umeiko/KlipperScreen-esp](https://github.com/umeiko/KlipperScreen-esp)
