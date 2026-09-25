# Klipper Remote Display — 架构文档

> 项目：**KlipperScreen-esp**（Klipper-Remote-ESP32-Displays），MIT 协议。
> 状态：v0.5.0（2026-09）——本文描述当前代码的真实架构，只写已落地的内容。
> 相关文档：[从源码构建](building.md) · [支持的板子](boards.md) · [移植新板型](porting.md) · [贡献板型](contributing-board.md) · [Moonraker API 细节](klipper-moonraker-api.md) · [JC8048W550 RGB 屏排障指南](jc8048w550-rgb-display-guide.md)

---

## 1. 项目概述

在低成本 ESP32 触屏开发板上运行一个"远程 KlipperScreen"：显示打印状态、温度、进度，支持点动、预热、挤出、文件管理等操作。通信走 **Moonraker WebSocket JSON-RPC**（端口 7125），不直连 klippy；另支持拓竹（Bambu）云端账号登录与状态监视。

同一份 UI 与业务代码支撑三类可构建目标：

- **ESP32 固件**（ESP-IDF 5.5.5，七块板型，见 §4）；
- **Windows 桌面控制端**（SDL2 显示 + WinHTTP WebSocket，真实连接打印机）；
- **桌面 simulator**（SDL2 + 本地 mock 数据，用于布局开发与截图，Linux/macOS 也构建此目标）。

UI 库为 **LVGL 9.3**。界面参考 KlipperScreen 的交互设计，并加入了原作基本没有的过渡动画与数值补间。

---

## 2. 分层架构

```
┌─────────────────────────────────────────────────────────────┐
│                        UI 层 (src/ui/)                       │
│  panels/（17 个面板）· panel_mgr · titlebar · theme · lang    │
│  ui_nav（焦点导航）· ui_anim · widgets/ · ui_layout           │
├─────────────────────────────────────────────────────────────┤
│                       Core 层 (src/core/)                    │
│  printer.h 数据层契约                                          │
│  ├─ printer_model.c（真实实现，ESP32/Windows）                 │
│  └─ printer_mock.c（simulator 本地模拟）                       │
│  MoonrakerClient（连接/握手/订阅/重连）· klipper_api（RPC 封装）│
│  Bambu：bambu_cloud · bambu_monitor · bambu_status(_stream)   │
│  app_settings（key=value 配置，带缓存与并发保护）               │
├─────────────────────────────────────────────────────────────┤
│                        BSP 层 (src/bsp/)                     │
│  bsp.h 接口契约 · bsp_screen_power（亮度/息屏状态机）           │
│  bsp_wifi（非阻塞 WiFi 抽象）· bsp_conf（配置存储介质）          │
│  esp32/（七块板型 BSP + 旋钮/息屏键/自研 rgb44 驱动）            │
│  desktop/（SDL2 BSP + Win/Linux WiFi + 文件存储）              │
├─────────────────────────────────────────────────────────────┤
│                       Ports 层 (src/ports/)                  │
│  esp32/（ESP-IDF 工程壳：app_main、Kconfig、sdkconfig、分区表） │
│  desktop/（CMake 工程：main.c、两个构建目标、WinHTTP/OpenSSL）  │
└─────────────────────────────────────────────────────────────┘
```

**依赖方向单向向下**：UI → Core → BSP。Core 不反向 include UI——数据刷新靠 UI 注入的回调（`printer_set_refresh_hook(panel_mgr_tick)`），LVGL 锁由 BSP 提供（`bsp_lvgl_lock/unlock`）。跨后端差异（TLS、凭据存储、网络栈）收在 Ports 层，Core 只面对统一的小接口。

**同接口多实现按构建目标选源文件，不用 `#ifdef`**：例如 `printer_model.c` / `printer_mock.c`、`moonraker_client.c`（ESP32）/ `moonraker_client_winhttp.c`（Windows）/ `moonraker_client_posix.c`（macOS/Linux 桌面）/ `moonraker_client_stub.c`（simulator）都实现同一组符号，由各自构建系统挑一个参与链接。

---

## 3. 目录结构

```
├── src/
│   ├── ui/                     # 全部后端共享的 UI 代码
│   │   ├── panels/             #   17 个面板，每个一个 .c，导出 panel_def_t
│   │   ├── widgets/            #   通用控件（toast、keypad 等）
│   │   ├── assets/             #   图标/字体 C 数组（tools/ 脚本生成）
│   │   ├── panel_mgr.c         #   面板注册表 + 导航栈 + 转场
│   │   ├── theme.c / ui_layout.c  # 主题色/控件工厂；分辨率缩放与字体档位
│   │   ├── lang.c              #   五语言字典 + TR() 宏
│   │   ├── ui_nav.c            #   编码器/键盘焦点组管理
│   │   ├── titlebar.c          #   常驻标题栏（连接状态、温度、时钟）
│   │   ├── ui_anim.c           #   动画助手（统一时长/缓动约定）
│   │   └── boot_anim.c         #   开机动画（LVGL 场景外直推像素）
│   ├── core/                   # 数据层与协议客户端（见 §7、§8）
│   ├── bsp/                    # 板级支持：接口契约 + esp32/ + desktop/（见 §4）
│   └── ports/                  # 每个后端的工程壳与入口（见 §10）
│       ├── esp32/              #   ESP-IDF 工程：entry/app_main.c、Kconfig、
│       │                       #   每板型 sdkconfig* 与 build* 目录、bambu/ 端云登录
│       └── desktop/            #   CMake 工程：main.c、lv_conf.h、
│                               #   bambu_cloud_winhttp.c、bambu_monitor_openssl.c
├── third_party/lvgl/           # LVGL v9.3（git clone，不入库）
├── managed_components/         # ESP-IDF 组件依赖（lvgl、esp_lcd 系列、XPT2046 等）
├── tests/                      # 宿主机单元测试（bambu 流式解析器）
├── tools/                      # 构建脚本、fontgen/icongen 资源生成、便携工具链
└── docs/                       # 文档（mkdocs 站点源）
```

- 新增面板 = `src/ui/panels/` 加一个文件 + 在 `panel_mgr.c` 注册表登记。注意 `src/ui/CMakeLists.txt` 是 GLOB 收集源文件，新增文件后若链接报 undefined，touch 该 CMakeLists 触发重配（不能加 `CONFIGURE_DEPENDS`，IDF script 模式会报错）。
- `src/` 下各目录的 `CMakeLists.txt`（`idf_component_register`）只被 ESP-IDF 消费，对桌面 CMake 是惰性文件；桌面端有自己的源文件清单。

---

## 4. BSP 与多板型

### 4.1 接口契约（`src/bsp/bsp.h`）

每块板/后端实现同一组接口，要点：

- `bsp_init()`：显示 + 板载主输入 + LVGL 节拍任务；`bsp_input_init()`：可选附加输入（ESP32 按 Kconfig 建 EC11 旋钮，desktop 建鼠标滚轮）。
- `bsp_lvgl_lock/unlock()`：所有 LVGL API 调用必须持锁（桌面单线程后端仍提供同一接口）。
- `bsp_lcd_push()` / `bsp_delay_ms()`：LVGL 场景外直推 RGB565 像素（开机动画用）。
- 屏幕电源一组：`bsp_set_brightness()` / `bsp_set_screen_timeout()` / `bsp_screen_activity()` / `bsp_screen_off/wake/toggle/is_off()` / `bsp_fade_out()`，语义见 §5。
- 显示偏好：`bsp_disp_can_invert/set_invert`、`bsp_disp_can_rotate180/set_rotate180`——SPI 屏支持（panel 命令 + 触摸坐标翻转），JC8048W550（RGB 并口）与 desktop 不支持，`can_*` 让 UI 隐藏对应开关。
- `bsp_restart()`：重建全部 UI 的场景用（语言切换）；`bsp_time_sync_from_http_date()` 统一消费标准 HTTP Date，`bsp_time_sync_from_host()` 异步从 Moonraker 主机取得该响应头。

BSP 还有两个配套抽象：

- **WiFi**（`src/bsp/bsp_wifi.h`）：全非阻塞轮询模型——`bsp_wifi_scan_start/poll`、`bsp_wifi_connect`、`bsp_wifi_status/connected`。实现：`esp32/bsp_wifi_esp32.c`（esp_wifi 事件驱动，断线自动重连）、`desktop/bsp_wifi_windows.c`（netsh wlan，输出可能 UTF-8 也可能 GBK，逐行探测转码）、`desktop/bsp_wifi_linux.c`（nmcli）。
- **配置存储**（`src/bsp/bsp_conf.h`）：`esp32/bsp_conf_littlefs.c`（LittleFS `storage` 分区）与 `desktop/bsp_conf_file.c`（Windows 存 `%APPDATA%\KlipperRemote`，simulator 用工作目录，避免覆盖真实控制端配置）。

### 4.2 板型选择机制

- Kconfig `choice BOARD`（`src/bsp/Kconfig.projbuild`）定义 `CONFIG_BOARD_*`；`src/bsp/CMakeLists.txt` 注册全部 BSP 源文件，**文件内部用 `#if CONFIG_BOARD_*` 裁剪**（组件注册的第一遍扫描早于 Kconfig 加载，无法按宏选文件）。
- 每板型独立的 sdkconfig、defaults 文件、分区表和构建目录（芯片目标不同，不能混用）。构建入口 `tools/build-esp32.sh <board>`，细节见 [building.md](building.md)。**注意**：改 `sdkconfig.defaults.<board>` 对已生成的 `sdkconfig.<board>` 不生效，两个文件都要改。

### 4.3 十块板型

| board | 芯片 | 屏幕 | 输入 | 显示路径 |
|---|---|---|---|---|
| `cyd_2432s028r` | ESP32 | 2.8" 320×240 ILI9341 | XPT2046 电阻触摸 | esp_lcd SPI |
| `cyd_2432s028r_plus` | ESP32 | 2.8" 320×240 ST7789 | XPT2046 电阻触摸 | esp_lcd SPI |
| `e32r35t` | ESP32 | 3.5" 480×320 ST7796 | XPT2046 电阻触摸 | esp_lcd SPI |
| `esp32-st7735s-128_160-ec11` | ESP32 | 1.8" 160×128 ST7735S | EC11 旋钮（无触摸） | esp_lcd SPI |
| `esp32-st7789-320_240-ec11` | ESP32 | 320×240 ST7789 | EC11 旋钮（无触摸） | esp_lcd SPI |
| `esp32-ILI9341-320_240-ec11` | ESP32 | 320×240 ILI9341 | EC11 旋钮（无触摸） | esp_lcd SPI |
| `esp32-ST7796-320_240-ec11` | ESP32 | 320×240 ST7796 | EC11 旋钮（无触摸） | esp_lcd SPI |
| `esp32s3-st7789-320_240-ec11` | ESP32-S3 | 2" 320×240 ST7789 | EC11 旋钮（无触摸） | esp_lcd SPI |
| `esp32s3-st7796-480_320-xpt2046-ec11` | ESP32-S3 | 480×320 ST7796S | XPT2046 电阻触摸（共总线）+ EC11 | esp_lcd SPI |
| `esp32s3-ILI9488-480_320-xpt2046-ec11` | ESP32-S3 | 480×320 ILI9488（18-bit SPI） | XPT2046 电阻触摸（共总线）+ EC11 | esp_lcd SPI |
| `esp32s3-ILI9341-320_240-xpt2046-ec11` | ESP32-S3 | 320×240 ILI9341 | XPT2046 电阻触摸（共总线）+ EC11 | esp_lcd SPI |
| `jc8048w550` | ESP32-S3 | 5" 800×480 RGB 并口 | GT911 电容触摸 | 自研 rgb44（见下） |
| `esp32s3-sensecap-indicator` | ESP32-S3 | 4" 480×480 ST7701S RGB 并口 | FT5x06 电容触摸 | 自研 rgb44（同 JC8048） |
| `esp32s3-JLC-SZP` | ESP32-S3 | 2.0" 320×240 ST7789 | FT6336 电容触摸 | 手动 SPI（见下） |
| `esp32s3-retro-go` | ESP32-S3 | 3.2" 320×240 ST7789 | GPIO 按键（无触摸） | esp_lcd SPI |
| `esp32c3-st7789-320_240-ec11` | ESP32-C3 | 320×240 ST7789 | EC11 旋钮（无触摸，软件正交解码） | esp_lcd SPI |

三块特殊板型：

- **JC8048W550**：不用 IDF 5.5 的 `esp_lcd_rgb_panel`，用自研驱动 `src/bsp/esp32/rgb44.c`（IDF 4.4 传输模型：每帧扫完自停 + vsync 全量重启，欠载帧下一拍自愈）+ LVGL DIRECT 双缓冲（PSRAM 双 fb，vsync 换页，flush 前整帧 `esp_cache_msync` 回写）。完整机制链与测量过程见 [jc8048w550-rgb-display-guide.md](jc8048w550-rgb-display-guide.md)，这里不展开。
- **SenseCAP Indicator**：与 JC8048 共用同一套 rgb44 + LVGL DIRECT 双缓冲渲染路径（PCLK 12MHz，480×480 方形屏），差异在面板初始化（ST7701S 位 bang 3 线 9-bit SPI，CS/RST 挂 TCA9535 I²C 扩展器）和触摸（FT5x06，GX 批次地址 0x48）。详见 [boards.md](boards.md#sensecap-indicator)。
- **esp32s3-JLC-SZP（立创实战派）**：不用 esp_lcd 面板驱动——CS 在 PCA9557 I²C 扩展器上，面板要求每笔交易都有 CS 下降沿，BSP 直接 SPI master + 手动控 CS/DC，初始化序列照抄 TFT_eSPI。

新增板型的完整流程见 [porting.md](porting.md) 与 [contributing-board.md](contributing-board.md)。

---

## 5. 输入与屏幕电源

### 5.1 输入通道

```
触摸 / SDL 鼠标 ──────────────▶ LVGL pointer indev（不进焦点组）
EC11（PCNT）/ SDL 滚轮 ─▶ encoder indev ─▶ ui_nav 焦点组 ─▶ 当前面板
息屏按钮 GPIO ────────────▶ 轮询消抖 ─▶ bsp_screen_toggle()
```

- 板型 BSP 只创建板载 pointer（有触摸的板）；EC11 旋钮由共享驱动 `src/bsp/esp32/bsp_rotary_encoder.c` 按 Kconfig 创建（有 PCNT 的芯片用 PCNT 计数，ESP32-C3 无 PCNT 走 2ms 定时轮询软件正交解码——不用 GPIO 中断，避免悬浮/噪声输入形成中断风暴）。触摸、触摸+旋钮、纯旋钮都是完整配置。
- **XPT2046 电阻触摸需要校准**，参数存 LittleFS `touch.json`；GT911 / FT6336 电容触摸直接报屏幕坐标。
- `ui_nav`（`src/ui/ui_nav.c`）枚举 encoder/keypad indev 并绑定当前面板的 LVGL group；pointer 刻意不进组，两种交互方式可同时用。每个面板有独立 group，切换面板时激活对应 group；弹窗压入临时组，旋钮不会穿透遮罩。
- 可操作控件用 `theme_action_card()`（真实 `lv_button`）或 `theme_focusable()` 登记，不靠对象树猜可操作性。同一控件可按输入来源提供不同交互（如 Moonraker 主机行：触摸打开全键盘，旋钮打开 IPv4 四段编辑器）——差异留在控件语义层，不渗入 BSP。

### 5.2 屏幕电源（`src/bsp/bsp_screen_power.c`）

亮度、自动息屏超时、`screen_off` 状态、最后活动时间由这一处公共状态机统一保存；板型只注册 `backlight_apply(0..100)` 回调，负责把有效亮度映射到 PWM/GPIO/背光 IC。

- `bsp_screen_activity()` 是触摸与旋钮共用的活动打点契约：返回"本次是否刚唤醒"，输入驱动据此**吞掉唤醒当次点击**，防止唤醒同时误触控件。
- 息屏按钮（`src/bsp/esp32/bsp_sleep_button.c`）：多 GPIO 轮询消抖（10ms 轮询 / 30ms 消抖，最多 8 个），任意按钮按下即 toggle，与自动超时息屏共享同一 `screen_off` 状态。它不是 LVGL 输入设备，不参与焦点。现有板型多用板载 BOOT 键（GPIO0），S3 旋钮板另有外挂按钮。

---

## 6. 面板系统与 UI 框架

### 6.1 面板管理（`src/ui/panel_mgr.c`）

- 每个面板一个 `.c` 文件，导出 `panel_def_t`（`panel_mgr.h`）：`name`、`title`、`title_s`、`create/on_show/on_tick`、`hide_temps`。当前注册表共 **17 个面板**：main、job_status、temperature、move、extrude、files、file_detail、settings、language、display、wifi、moonraker、machine_mode、bambu_link、bambu_setup、printers、brightness。
- **导航栈**（深 8）：`panel_mgr_open/back/home` + 左滑/右滑转场；栈顶去重。
- **面板不常驻**：懒加载创建，只有主面板（栈底）和当前面板存活；离开的非主面板在转场结束后销毁屏幕对象树与导航组（`destroy_left_panel()`）——CYD 无 PSRAM，17 个面板全常驻曾 OOM 卡死。**推论：面板每次进入都重跑 `create()`，静态对象指针不得假设跨访问存活。**
- **打开即全量回放**：每次显示调 `on_show()` 刷新全量数据；**仅栈顶面板收 `on_tick()` 数据节拍**（约 1Hz，由 `panel_mgr_tick` 驱动），标题栏单独走 `titlebar_tick()`。
- 标题栏：`title_s` 是小屏（`ui_scale() < 1.0f`，如 160×128）专用短标题（≤2 字，NULL 则用 `title`）；小屏子面板标题栏不显示温度（位置让给标题）；标题长的面板置 `hide_temps = 1`。

### 6.2 主题与分辨率自适应（`theme.c` / `ui_layout.c`）

- `ui_scale() = 屏高 / 240`，控件尺寸按 `ui_px()` 等比缩放，间距走 `ui_gap()` 次线性缩放（大屏不至于太空旷）——一套布局适配 160×128 到 800×480。
- 字号档 ESP32 按板型预处理期定死（未用的全表字体直接被链接器丢掉）：160×128 用 10/12，320×240/480×320 用 14/16，800×480 用 28/32；desktop 走运行时档位（可切分辨率调试）。
- 图标为 A8 单色图，`theme_img()` 靠 `image_recolor` 上色；小屏用 tools/icongen 生成的 0.45x `_sm` 变体（`ui_layout.c` 的 `icon_sm()` 映射）。主题 dark/light 存 klipperscreen.conf。

### 6.3 动画（`src/ui/ui_anim.c`）

统一时长约定：`UI_ANIM_FAST 150` / `UI_ANIM_NORMAL 250` / `UI_ANIM_SLOW 700`，业务代码不散落魔法数字。面板转场、toast 滑入滑出、数值补间都走这里。两条用事故换来的硬约束：

- **ESP32 上禁止对控件做 `style_opa` / `transform_scale` 动画**：二者强制 LVGL 把控件渲进中间层缓冲（宽×高×2 字节），堆紧张时分配失败会让 draw 任务无限重试，占死 CPU 触发 IDLE 看门狗、UI 整体卡死。按压反馈用 `bg_opa`，弹窗直接显示。数值/位置类动画（arc value、label 文本、y 坐标）不受影响。
- **跨任务调 `lv_async_call` 必须持 `bsp_lvgl_lock()`**：本项目 `LV_USE_OS=NONE`，LVGL 内部无锁；`lv_async_call` 内部的 `lv_timer_create` 会改全局 timer 链表，与 LVGL 任务的 `lv_timer_handler` 并发会踩坏链表（崩溃表现为 async 定时器执行两次、`lv_free` 报堆错）。新增跨任务投递一律走 `moonraker_client.c` 的 `post_to_lvgl()`（内部加锁）或照它写。

---

## 7. Core 层

### 7.1 数据层契约（`src/core/printer.h`）

面板只通过 `printer_*` 访问器取数/下发操作，不感知数据来源。两个实现按构建目标二选一：

- `printer_model.c`——ESP32 与 Windows 真实实现，数据来自 Moonraker / Bambu monitor；
- `printer_mock.c`——simulator 本地模拟（截图/布局用，不会进入真实控制链路）。

`printer_capabilities()` 能力位（温度控制/点动/挤出/文件/开始/暂停/恢复/取消/急停/固件重启）让同一套 UI 承载三种后端：Klipper 全量、拓竹云端只读监视、拓竹 LAN（预留）。面板按能力开放入口。

### 7.2 MoonrakerClient（`src/core/moonraker_client.[ch]`）

三个平台实现同一接口：ESP32 用 esp_websocket_client，Windows 用 WinHTTP（`moonraker_client_winhttp.c`），macOS/Linux 桌面用手写 POSIX sockets（`moonraker_client_posix.c`），simulator 用 stub。协议细节（握手字段、订阅对象、通知语义）见 [klipper-moonraker-api.md](klipper-moonraker-api.md)，这里只给状态机骨架：

```
OFFLINE ──moonraker_start()──▶ CONNECTING ──WS open──▶ identify
   ▲                                                      │ server.info（等 klippy_connected，
   │ moonraker_stop()（幂等、非阻塞，                       │  未就绪则周期性重查）
   │ desired-disabled，socket 关闭在各自                     ▼
   │ worker/timer 上下文执行）                    objects.list → objects.subscribe
   │                                                      │ 订阅响应即全量快照
   └──────── on_close / 心跳超时 ◀──── READY ◀────────────┘
            （指数退避自动重连）
```

- 生命周期：`moonraker_start()` 读 moonraker.conf 启动（可重复调用，幂等）；`moonraker_stop()` 只表达 desired disabled 并唤醒 worker——阻塞关闭绝不在调用方（可能是 LVGL 任务）栈上执行；stop 后重连/心跳/RPC 全部抑制。**start/stop 互斥切换是 Klipper ↔ Bambu 后端切换的机制**（任一时刻只有一套网络连接）。
- 数据出口：订阅快照与 `notify_status_update` 增量在各自网络任务里解析成 status 字典文本，经加锁的 `lv_async_call`（`post_to_lvgl()`）投回 LVGL 任务，由 `printer_model_apply_status()` 合入模型——**模型读写全在 LVGL 上下文，不需要互斥锁，也没有独立 core_task / EventBus**；高频推送做在途去重。UI→网络方向：`printer_*` 写访问器 → `klipper_api_*` 拼 JSON-RPC → 直接发送（发送接口线程安全，无队列）。
- 对外 RPC 两种形式：`moonraker_send_rpc()`（fire-and-forget）与 `moonraker_rpc()`（带应答回调，应答经 LVGL 上下文投递）。

### 7.3 配置系统（`src/core/app_settings.c` + bsp_conf）

key=value 行格式（`#` 注释），两端通用，不引 JSON 库：

| 文件 | 内容 |
|---|---|
| `network.conf` | WiFi 凭据（ssid/pass） |
| `moonraker.conf` | 打印机槽位：最多 6 槽，每槽名称/machine_mode/host/port/api_key/Bambu 设备与连接方式，`active=N` 记当前槽；旧格式读取时自动迁移 |
| `klipperscreen.conf` | 本机偏好：语言、亮度、自动息屏、反色/旋转、主题 |
| `touch.json` | XPT2046 电阻触摸校准（仅电阻屏板型） |

两个性能/并发要点（都是实测踩坑）：

- `moonraker.conf` 有**全量 RAM 缓存**：温度/能力 getter 每秒被 UI 节拍多次调用，每次走 4KB malloc + 同步文件 IO 曾把温度页卡到整页掉帧。所有写路径负责失效缓存。
- 桌面 Moonraker worker、ESP32 esp_timer 回调与 LVGL 线程会**并发读写缓存**，一切访问在 `mr_cache_lock` 临界区内（Windows `CRITICAL_SECTION`，其余 `pthread_mutex`），缓存指针不得带出锁外。
- `machine_mode` 读取走 moonraker.conf 缓存（零文件 IO）；旧版存在 klipperscreen.conf 的全局值由一次性迁移逻辑补写回当前槽（`mr_legacy_checked` 保证每槽每次开机最多查一次、不反复写 flash）。

### 7.4 线程模型（ESP32 现状）

```
esp_websocket_client 任务 ──解析 JSON──▶ post_to_lvgl（持锁 lv_async_call）
                                                      ▼
                                              LVGL 任务（核 1）
                                              printer_model 合并 → panel_mgr_tick → 栈顶面板 on_tick
UI 触摸/旋钮 ──▶ klipper_api_* 拼 RPC ──▶ esp_websocket_client_send_text（直接发）
```

没有独立 net/core 双任务、没有 EventBus、没有跨线程队列——比早期设计稿简化了一整层，代价是 WS 回调里做 JSON 解析（目前帧率与内存都够用）。开机流程（`src/ports/esp32/entry/app_main.c`）：`bsp_init` → 附加输入 → 开机动画 → `ui_app_create()`（与桌面共享的同一份 UI）→ 应用偏好 → 按 `network.conf` 自动回连 WiFi；Moonraker 客户端由 printer_model 的周期轮询在 WiFi 就绪且已配置后拉起。串口调试 CLI（`help|scan|wifi|mr|mem|ht|...`）在 `entry/debug_cli.c`。

### 7.5 Windows 桌面端

真实控制端与 simulator 是**两个构建目标**（`KlipperScreen-esp` / `klipper_remote_simulator`），mock 状态不会进入实际控制链路。Windows 网络在独立 worker 线程收发与路由 JSON-RPC，状态更新同样经 `lv_async_call` 投回持锁的 LVGL 主线程。配置存 `%APPDATA%\KlipperRemote`。macOS/Linux 构建桌面目标（POSIX sockets 客户端）。

---

## 8. 拓竹（Bambu）后端

Core 层三个小接口，平台差异收在 Ports 层：

- **云登录**（`bambu_cloud.h`）：异步接口（区域选择、密码/邮箱验证码/短信验证码登录、设备列表、登出），UI 只面对 snapshot。ESP32 实现在 `src/ports/esp32/bambu/`（bambu_net actor 任务跑全部 HTTPS，NVS 存凭据；内部笔记见该目录 `DEVNOTES.md`），Windows 用 `bambu_cloud_winhttp.c`。
- **状态监视**（`bambu_monitor.h`）：云端 MQTT 只读监视。Windows 使用 `bambu_monitor_openssl.c`；ESP32 使用 `bambu_monitor_esp32.c`，由同一个 `bambu_net` actor 串行编排 HTTPS/MQTT，4KiB TLS MFL + ESP-MQTT 分片直喂零分配解析器，避免无 PSRAM 板累计完整大包。
- **状态模型与解析**：`bambu_status.c`（cJSON 版 merge 规则）与 `bambu_status_stream.c`（零分配流式解析器，ESP-MQTT 分片直喂，语义对齐前者；有宿主机单元测试 `tests/test_bambu_status_stream.c`）。

Bambu 连接方式按槽位保存（`bambu_link_t`）：`CLOUD_MONITOR` 云端只读监视（已实现）；`LAN` 局域网开发者模式是预留路径（UI 已留入口，控制后端未实现）。**与 Moonraker 生命周期互斥**：切到 Bambu 时 `moonraker_stop()`，切回时 `moonraker_start()`，任一时刻只有一套网络连接。

ESP32 标题栏时钟不依赖额外 NTP：Moonraker 模式读取本地主机响应的 `Date`，Bambu 模式复用现有云端 HTTPS 响应的 `Date`，两者都交给 BSP 的同一校时入口。

---

## 9. 多语言与资源生成

- **i18n**（`src/ui/lang.c`）：五语言字典——简体中文 / English / 繁體中文 / Français / Italiano。dict 以中文字面量为 key，源码里写中文、`TR()` 宏包一层查表。新增字符串要同步补五列译文；`title_s` 等新增词条同理。语言切换写 klipperscreen.conf 后 `bsp_fade_out()` + `bsp_restart()` 重启生效（字体与布局随语言重建）。
- **字体**：`tools/fontgen/gen_fonts.py` 提取全部 UI 字符串字面量的非 ASCII 字符，生成 CJK 子集字体（`src/ui/assets/`）。**改了任何 UI 字符串必须重新生成**，否则新字显示为 □。14/16 档另有 `_cmp` 压缩变体（CYD 4MB flash 用）；JC8048 用 `UI_FONT_MIN=1` 的最小子集 `_min` 字体——全表 5.4MB 字形走 XIP cache 读，文本渲染的 flash 突发在 MSPI 总线上与 RGB 屏 EDMA 扫描争抢，曾导致滑动抽动（排障细节见 JC8048 指南第 12 节），代价是文件名/SSID 里表外汉字显示方框。
- **图标**：`tools/icongen` 把 SVG 转 LVGL A8 C 数组，并生成小屏 0.45x `_sm` 变体。

生成命令与工具链细节见 [building.md](building.md)。

---

## 10. 构建与 CI

- ESP32：`bash tools/build-esp32.sh <board> [flash COMx]`，七板型或 `all`；桌面端：`bash tools/build-desktop.sh`。工具链准备与分板型细节见 [building.md](building.md)。
- 版本号维护在 `src/core/version.h`（`KR_VERSION`，设置页与 Moonraker identify 共用）；发版 = 改它 + 打同名 `vX.Y.Z` tag 推送。
- CI（`.github/workflows/build.yml`）：push main / tag `v*` / 手动触发。固件矩阵在 `espressif/idf:v5.5.5` 容器里全量构建九块板型（各自独立 sdkconfig 与构建目录），桌面端构建 Windows 与 macOS 目标。tag 触发 release：资产名不带版本号（固件 `ESP-IDFv5.5-<board>.zip`，桌面 `desktop-win-x86_64.zip` / `desktop-macos-arm64.zip`），文档站下载直链走 `releases/latest/download/...`；CI 会把移动标签 `latest` 强推到最新正式版提交；tag 含 `wip` 标为预发布。

---

## 附录：KlipperScreen 架构对照

| 本项目 | KlipperScreen | 说明 |
|---|---|---|
| MoonrakerClient | KlippyWebsocket + MoonrakerApi | 全 WebSocket JSON-RPC，握手四步 |
| printer_model / printer.h | printer.py Printer | 数据层契约 + 增量合并；事件用注入回调代替 GLib.idle_add |
| panel_mgr | screen.py panels/_cur_panels | 注册表 + 导航栈；不同之处：非主面板离开即销毁 |
| widgets / titlebar | ks_includes/widgets、base_panel | toast、keypad、常驻标题栏 |
| theme / ui_layout | styles/ + KlippyGtk | 基准尺寸派生布局（scale = 屏高/240） |
| ui_anim | （原作几乎没有） | 本项目增量 |
| bsp / ports | （无，直接 GTK/系统） | 多后端的关键抽象 |
