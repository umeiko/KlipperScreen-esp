# Klipper Remote ESP32 Displays（中文文档）

[English README](README.md)

> **📖 文档站：https://umeiko.github.io/KlipperScreen-esp/zh/**
> 支持的板子与引脚、零基础移植教程、贡献指南都在文档站里。

<p align="center">
  <img src="docs/screenshots/main_photo.jpg" alt="CYD 2432S028R 实机运行效果" width="720">
</p>

**Klipper Remote** 是一款紧凑的跨平台 3D 打印机显示与控制软件，可运行在低成本 ESP32 开发板以及 Windows/macOS 桌面端。它通过 **Moonraker** 完整控制 **Klipper** 打印机，并在 Windows 端提供只读的**拓竹云端状态监视**；共享 LVGL 界面支持触摸、旋转编码器、鼠标、键盘或混合输入，EC11 参考板则展示了完全不使用触摸屏的纯旋钮设备。

单独命名的 SDL2 模拟器使用 mock 数据，无需连接打印机即可用于布局开发、输入实验和界面截图。

## 界面实拍

完整界面截图见文档站：**[界面展示 / Screenshots](https://umeiko.github.io/KlipperScreen-esp/zh/screenshots/)**

- 架构设计：[docs/architecture.md](docs/architecture.md)
- Klipper/Moonraker API 参考：[docs/klipper-moonraker-api.md](docs/klipper-moonraker-api.md)
- LVGL v9 API 防错笔记：[docs/lvgl-v9-api-notes.md](docs/lvgl-v9-api-notes.md)

## 功能

- 多打印机：最多 6 个打印机槽位，可分别对应 Klipper 或拓竹，3×2 切换页一键换机，秒级重连
- 实时状态：标题栏喷嘴/热床温度、状态卡按状态整卡变色（空闲绿/异常红/断连黄）
- 打印任务：G-code 历史列表、二级菜单打印/删除、进度环 + 已用/剩余时间、暂停/恢复/取消
- 控制：轴点动/归零、挤出/回抽（冷挤出保护）、温度预设（PLA/PETG/ABS/冷却）、急停/下位机重启（带确认）
- 链路健壮：WS 自动重连、应用层心跳 RTT 显示、僵尸连接检测、Klipper 报错 toast（如限位未触发）
- 拓竹云监视：Windows 控制端已有登录、验证码、账号设备选择和云端 MQTT 实时状态同步；云端模式只读，局域网 Developer Mode 界面已预留，控制后端仍在开发中
- 体验细节：「Umeko」开机动画、5 种语言（EN/简中/繁中/FR/IT，切换时渐暗到黑再重启）、背光滑杆、自动息屏（15秒~1小时/永不）触摸/旋钮唤醒、标题栏时钟（从 Moonraker 上位机对时，纯内网）
- 电阻触摸使用两点校准并持久化到 flash；电容触摸直接使用屏幕坐标，纯旋钮板无需触摸层

## 技术栈

ESP-IDF v5.5.5 · LVGL v9.3 · 多后端（ESP32 四个正式板型 / desktop SDL2：Windows+Linux）

## 刷机（免编译）

从 [Releases](../../releases) 或 CI artifacts 下载 `klipper-remote-esp32-*.zip`，解压后：

- **Windows**：`flash.bat COM6`（zip 内含 esptool.exe，无需装 Python）
- **macOS / Linux**：`./flash.sh /dev/ttyUSB0`（需 `pip install esptool`）

支持板型：**CYD 2432S028R**（2.8" 电阻屏）；**E32R35T**（ESP32-32E 3.5" 480×320 ST7796 电阻屏）；**EC11 旋钮最小系统**（ESP32-S3 + 240×320 ST7789 + EC11，无触摸，构建目标 `ec11_knob_minimal`）；**JC8048W550**（Guition 5" 800×480 RGB 电容屏，ESP32-S3）。面包板接线与 Fritzing 源文件见[支持的板子](docs/boards.zh.md#ec11-旋钮最小系统)，RGB 屏排坑记录见 [docs/jc8048w550-rgb-display-guide.md](docs/jc8048w550-rgb-display-guide.md)。

首次启动会自动格式化 LittleFS。电阻触摸板有出厂参数时直接加载，否则进入校准；电容触摸和纯旋钮板不会运行校准流程。

## Windows 控制端

Windows 发行包包含两个用途明确的程序：

- `klipper_remote_desktop.exe` 是真实控制端。在“设置 → 打印机连接设置”配置 Klipper 主机后，它通过系统 WinHTTP WebSocket 接收实时状态，并发送与 ESP32 固件相同的控制指令。
- `klipper_remote_desktop.exe` 还提供当前 Windows 拓竹云流程：登录、验证码、账号设备选择和只读 MQTT 状态监视。
- `klipper_remote_simulator.exe` 使用本地模拟打印机数据，供界面布局预研和截图回归；启动时不会自动连接真实打印机或拓竹云。

真实控制端的配置保存在 `%APPDATA%\KlipperRemote`，模拟器仍把便携配置留在运行目录，二者不会混用打印机状态。

在“设置 → 打印机连接设置 → 主机”上按下旋钮会打开四段式 IPv4 编辑器：旋转修改当前 0–255 数值，按下进入下一段，快速旋转最高加速到每格 10。触摸点击仍打开完整键盘，因此可以继续输入域名。

## 首次配置

1. 设置 → 无线网络：扫描 → 选 AP → 输密码，存入 `network.conf`
2. 设置 → 打印机连接设置：先选打印机槽位（最多 6 台）和机器模式；Klipper 再填主机 IP + 端口（默认 7125）+ 可选 API Key，拓竹继续进入局域网或云端设置，配置存入 `moonraker.conf`
3. 语言/背光/自动息屏等偏好存入 `klipperscreen.conf`

拓竹模式从“设置 → 打印机连接设置 → 机器模式 → 拓竹”进入。Windows 产品端登录并选定设备后可使用“云端监视”查看状态；“局域网控制”是 Developer Mode 预留路径，控制后端尚未完成。

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
   tools/msys64/usr/bin/bash.exe -lc "pacman -Sy --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cjson"
   ```
3. **LVGL**：`git clone --depth 1 -b release/v9.3 https://gitee.com/mirrors/lvgl.git third_party/lvgl`

## 构建

```bash
# 桌面端（Windows：真实控制端 + 开发模拟器；Linux/macOS：桌面目标）
bash tools/build-desktop.sh
./src/ports/desktop/build/klipper_remote_desktop.exe              # 真实 Moonraker 控制端
./src/ports/desktop/build/klipper_remote_simulator.exe            # 布局模拟器
./src/ports/desktop/build/klipper_remote_simulator.exe 3000 x.bmp # 3 秒后截图退出

# ESP32 后端（CYD 2432S028R，工程目录 src/ports/esp32）
cd src/ports/esp32
powershell -NoProfile -ExecutionPolicy Bypass -File ../../../tools/idf.ps1 build
powershell -NoProfile -ExecutionPolicy Bypass -File ../../../tools/idf.ps1 -p COMx flash monitor
```

桌面真实控制端默认把配置放在 `%APPDATA%\KlipperRemote`，模拟器从自己的工作目录读写配置。开发和截图时可设置 `KLIPPER_CONFIG_DIR` 指向独立目录，并在其中写入 `language=en` 生成英文界面。

桌面窗口中，鼠标左键仍模拟触摸；滚轮正反转模拟旋钮旋转，中键模拟按下旋钮，
可以直接测试触摸与旋钮并存的交互。

## 中文字体（改了 UI 文案后必跑）

界面里所有字符串字面量的非 ASCII 字符会被自动提取，生成 CJK 子集字体：

```bash
python tools/fontgen/gen_fonts.py        # 默认用 C:/Windows/Fonts/simhei.ttf，可 --font 换
```

新增中文后不重跑就会出现方框（□）。生成后需重新编译对应后端。

## 图标（assets/icons.h）

界面图标的 SVG 原件在 `src/ui/assets/svg/`，
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

## 致谢

- [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen) — 界面设计灵感来源
- LVGL、ESP-IDF、esptool 等由各自作者维护

## 许可证

MIT。
