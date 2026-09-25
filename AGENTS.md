# AGENTS.md

## 项目概况

Klipper 远程显示屏：ESP32 固件（ESP-IDF 5.5.5）+ Windows 桌面端（MinGW，调试用同一套 UI 代码）。LVGL 9.3，MIT。仓库：`umeiko/KlipperScreen-esp`。

## 外部编码代理与 Token 控制

- 委派的目标若是节省 Codex token，小型、边界清楚的改动默认由当前 Agent 直接完成；不要使用 `Kimi ACP + K3-256k + Thinking Max + 全量流式监控`。一次桌面键盘适配实测让 Kimi 使用约 167k token，同时消耗约 5% Codex 周限额，成本高于直接实现。
- 用户明确要求使用 Kimi 时，优先调用 `python tools/kimi-delegate.py -p "..."`；它在本地消费 ACP 流，只输出最终答复。默认 `--approval ask`：遇到工具审批或 AskUserQuestion 时只显示一条 `[kimi-request]` 摘要，通过同一进程 stdin 回复选项编号后原 turn 继续；自动策略必须显式传 `--approval reject|allow-once|allow-always`。主 Agent 只读取最终报告、`git diff` 和少量验证结果。不要直接把 `kimi acp` 接到会话终端；确需保留原始协议时才显式传 `--log <path>`，该日志可能很大。
- 委派启动时会立即输出 `run-id`，并在 `tmp/kimi-delegate-state/` 原子维护限长状态快照。监工默认只等待原委派进程的最终输出或 `[kimi-request]`，一次等待 45～60 秒；不要同时查询进程树、包管理器日志或 ACP 状态。若原进程句柄丢失，使用 `python tools/kimi-delegate.py wait --run-id <id> --timeout 55 --quiet-timeout`：它只在完成、失败、取消或需要审批时输出，无变化超时不产生上下文。`status --run-id <id> --max-chars 800 --since <cursor>` 仅用于两轮以上静默、怀疑卡死时的一次性诊断，不作常规轮询。快照不含 thought 或 raw tool output。
- 推荐默认档位为 `python tools/kimi-delegate.py --thinking low --mode auto --approval ask -p "..."`；低档不足再升 `high`，只有用户明确要求或任务确实需要时才用 `max`。平时不要开 `--verbose` 或 `--log`；长提示词用 `-f <prompt-file>`，需要追问或审批后续接时复用 `--session-id`。提示词只给必要上下文，并要求最终答复简短列出改动文件、验证结果和遗留问题。
- 主 Agent 验收外部代理时只看最终报告、`git diff --stat`、目标文件 diff、`git diff --check` 和一个针对性 build/smoke；不要把 ACP 的思考流、完整工具输出或无关大 diff 重新灌入上下文。这样才可能真正节省 Codex token；ACP 只是传输协议，本身不保证省 token。
- 外部命令仍在运行但没有新输出是正常状态，不等于卡死。只有达到委派提示词里的超时/停止条件、代理主动报错，或连续两次低噪声等待后仍需判断是否人工介入，才查看一份限长状态；底层进程和安装日志只在状态明确显示失败且最终报告不足以定位时检查。
- 状态快照属于非关键旁路：Windows 上目标 JSON 被扫描器或读取进程短暂占用时，中间件会重试并跳过本次快照，绝不能因此终止 ACP turn。中间件若异常退出，优先从快照取 `sessionId` 用 `--session-id` 接续，不重新执行已经完成的工作。
- `kimi acp` 与 `tools/kimi-delegate.py` 默认使用当前 `KIMI_CODE_HOME`（未设置时为 `~/.kimi-code`）中的 Kimi Code 登录凭证。切换订阅账号要先 `kimi logout` 再 `kimi login`，切换后建议重启 ACP 进程；若要同时隔离多个账号，为每个进程设置不同的 `KIMI_CODE_HOME` 并分别登录，委派工具会继承该环境变量。
- `Thinking Max` 只用于确有复杂推理需求的任务；常规实现使用低/高档思考。提示词必须写清修改边界、最小验证和停止条件，happy path 已证明后立即收尾，不继续追逐截图时机、动画中间态等非阻塞现象。
- 委派前后分别检查 `git status --short --branch`；要求外部代理自行清理调试打印、自动 demo、截图和临时文件。验收只做与风险相称的 `git diff --check`、目标构建或一个 smoke check，不因委派自动扩大测试范围。

## 构建/烧录

- ESP32：`bash tools/build-esp32.sh <board> [flash COMx]`，board ∈ `cyd_2432s028r` / `cyd_2432s028r_plus`（CYD 的 ST7789 变种，WROOM-32E，引脚同 CYD、无 RST 脚，与 CYD 共用 BSP 文件的 `#if` 分支）/ `e32r35t` / `esp32s3-st7789-320_240-ec11`（原 ec11_knob_minimal，S3+ST7789）/ `esp32-st7735s-128_160-ec11`（原 ec11_knob_esp32，ESP32+ST7735S）/ `esp32-st7789-320_240-ec11`（ESP32+ST7789 320x240，引脚同 st7735s 板）/ `esp32-ILI9341-320_240-ec11`（ESP32+ILI9341 320x240，引脚同 st7789 板）/ `esp32-ST7796-320_240-ec11`（ESP32+ST7796 320x240，引脚同 ILI9341 板）/ `esp32s3-st7796-480_320-xpt2046-ec11`（S3+ST7796S+XPT2046 共总线+EC11）/ `esp32s3-ILI9488-480_320-xpt2046-ec11`（S3+ILI9488+XPT2046 共总线+EC11，MKS PI-TS35，引脚同 st7796 板；18-bit SPI，atanisoft/esp_lcd_ili9488，flush 必须等 DMA 完成保内部共享转换缓冲；无出厂触摸校准，首启两点校准）/ `esp32s3-ILI9341-320_240-xpt2046-ec11`（S3+ILI9341 320x240+XPT2046 共总线+EC11，引脚同 st7796 板；面板参数沿用 CYD 实测：BGR/mirror(true,true)/INVOFF，就地字节交换；无出厂触摸校准，首启两点校准）/ `jc8048w550` / `esp32s3-sensecap-indicator`（Seeed SenseCAP Indicator，S3+4" 480x480 ST7701S RGB 并口+FT5x06 电容触摸，GX 批次触摸地址 0x48；与 JC8048 共用 rgb44 DIRECT 双缓冲，PCLK 12MHz；TCA9535 扩展器管 CS/RST/TP_RST/RP2040_RST；烧录走 CH340 的 "USB-SERIAL" 口，另一个 Type-C 是 RP2040 的别碰）/ `esp32s3-JLC-SZP` / `esp32s3-retro-go`（Chaeng retro-go S3 掌机，ST7789+GPIO 按键）/ `esp32c3-st7789-320_240-ec11`（C3 单核无 PCNT：编码器走 2ms 定时轮询软件正交解码，flash DIO，USB-Serial-JTAG 控制台；合宙 CORE USB 版与 Super Mini 通用）/ `all`。烧录前必须先断开串口占用（`mcp__serial-mcp__close_port`），烧后重连（115200）。
- 桌面端：`bash tools/build-desktop.sh`。
- **sdkconfig 大坑**：改 `sdkconfig.defaults.<board>` 对已生成的 `sdkconfig.<board>` 不生效——要改必须两个文件都改（sdkconfig 里翻 canonical 行，注意 `# CONFIG_XXX is not set` 会覆盖 defaults）。
- IDF 源码在 `C:/esp/v5.5.5/esp-idf`。GitHub 走代理 `curl --proxy http://127.0.0.1:7890`。
- 系统有 pio（`C:\Users\m9291\.platformio\penv\Scripts\pio.exe`）；`tmp/pio_music` 是厂商 demo 的 PIO 对照工程，增量编译+上传约 25 秒，做显示实验比 IDF 全量快得多。

## 发版约定

- 版本号维护在 `src/core/version.h`（`KR_VERSION`，设置页和 Moonraker identify 都用它）；发版 = 改它 + 打同名 `vX.Y.Z` tag 推送。
- CI 固件按 esp32 / esp32s3 两个 shard 在同一 IDF 容器内顺序合并构建（共享按 shard 分开的 ccache，单板失败不中断其余板型），package 统一为单 Job 聚合产物。
- CI  release 资产名**不带版本号**：固件 `ESP-IDFv5.5-<board>.zip`、桌面 `desktop-win-x86_64.zip` / `desktop-macos-arm64.zip`、Linux 上位机 `desktop-linux-x86_64.tar.gz` / `desktop-linux-arm64.tar.gz`（ubuntu-22.04 / ubuntu-22.04-arm runner 静态编译 SDL2+cJSON，glibc≥2.35；tarball 内含 bin/KlipperScreen-esp + scripts/linux 的 install/uninstall/systemd/启动脚本）（`ESP-IDFv5.5` 是构建框架版本，不表示目标芯片都是 ESP32）；文档站下载直链走 `releases/latest/download/...`；tag 含 `wip` 标为预发布。旧 `klipper-remote-*` 遗留资产由 release job 在新资产上传成功后自动按 id 清理。
- **命名约定**：产品二进制与 systemd 服务统一叫 `KlipperScreen-esp`（`KlipperScreen-esp.exe` / `bin/KlipperScreen-esp` / `KlipperScreen-esp.service`），`klipper-remote` 一名已停用；Moonraker identify 的 client_name 同步为 `KlipperScreen-esp[-平台]`。开发模拟器仍叫 `klipper_remote_simulator`。
- CI 会强推移动标签 `latest` 到最新正式版提交。
- `src/ui/CMakeLists.txt` 是 GLOB 收集源文件：新增面板/字体文件后若链接报 undefined，先 touch 它触发 CMake 重配（不能加 CONFIGURE_DEPENDS，IDF script 模式会报错）。

## UI 约定

- 小屏（160x128，`ui_scale() < 1.0f`）专属待遇：标题栏用 ≤2 字短标题——面板注册时在 `panel_def_t` 里填 `.title_s`（NULL 则用 `.title`），新增词条要同步补 `src/ui/lang.c` 五语言 dict；子面板标题栏不显示温度（panel_mgr.c show() 里按 ui_scale 判断）；SVG 图标统一用 0.45x 预生成变体（tools/icongen 生成 `_sm` 图标，`ui_layout.c` 的 `icon_sm()` 按映射表替换，新图标要同步进 `icon_sm_map`；`panel_printers.c` 槽位 logo 有自己的 scale 需单独乘 0.45）。
- **面板不常驻**：非主面板离开时屏幕+导航组即销毁（panel_mgr.c `destroy_left_panel()`，CYD 无 PSRAM 扛不住 17 个面板全缓存，曾是 OOM 卡死根因）。面板每次进入都重跑 `create()`，静态对象指针不得假设跨访问存活；标题长/与打印控制无关的面板在 `panel_def_t` 置 `.hide_temps = 1`。
- **切语言**：ESP32 保存后渐暗重启重建 UI；桌面端免重启——`panel_mgr_reload()`（异步调用，先切临时空屏再销毁全部面板树重建）后回语言页。
- **大字档（desktop only）**：`ui_scale() >= 3.0`（720p+，如红米4 5寸 293dpi）走 huge 档——字体 40/48（`font_cjk_40/48.c`，gen_fonts.py `DESKTOP_ONLY_SIZES`，文件体带 `#ifndef ESP_PLATFORM` 守卫，ESP32 GLOB 编进工程也是空文件）、图标经 `icon_lg_map` 映射到 2x 变体（`_64`/`_112`/`_48`，ESP32 不编译该表，不引用不链接）。
- **方向键导航白名单**（分派在 `ui_buttons.c`，实现在 `ui_nav.c`，ESP32 实体键与桌面键盘共用）：未标记组保持原生——上下=LVGL 线性 NEXT/PREV、左右原样送达控件、回车确认、Esc 返回。面板在 `create()` 里对默认组标记：`ui_nav_group_set_list()`（纯列表页：左=返回、右=进入/确定）或 `ui_nav_group_set_spatial()`（网格布局：四方向按屏幕坐标几何就近聚焦；禁用/隐藏项始终跳过，两轮扫描——严格正交邻居找不到时放宽到该方向最近可选项，防灰色项困死焦点）。当前 spatial：主界面/温度/机器模式/切换打印机/挤出/打印状态/拓竹设置/数字键盘（keypad.c）；list：设置/语言/显示/WiFi/文件/文件详情/Moonraker/拓竹连接。屏幕键盘（lv_keyboard 焦点）与展开的下拉框自动四键原样送达，无需标记；组编辑态（`lv_group_get_editing`，如温度调值、IP 段）左右自动原样。桌面端文本输入会话中方向键+回车归导航（回车=按虚拟键盘高亮键=输入字符），**F1=提交表单**；确认框（confirm.c）上下左右都切换按钮。

## 串口 CLI（JC8048 / esp32 端）

`help|scan|wifi|wifioff|wifion|mr|mrstart|status|ps|printer|gc|ls|cd|pwd|cat|rm|mem|ht|caltouch`，实现在 `src/ports/esp32/entry/debug_cli.c`（`mem` 查堆水位、`ht` heap trace 抓未释放块、`caltouch` 写标记文件重启后强制进入触摸两点校准（仅电阻屏机型：CYD/E32R35T/st7796-ec11，其余机型直接拒绝；门控用 BSP 层能力宏 `BSP_HAS_TOUCH_CAL`，板型能力统一登记在 `src/bsp/bsp_caps.h`，上层不得直接判 `CONFIG_BOARD_*`））。

## 息屏/唤醒按钮（ESP32 端）

- BSP 接口：`bsp_screen_off()` / `bsp_screen_wake()` / `bsp_screen_is_off()`（`src/bsp/bsp.h`），与自动超时息屏共享同一 `screen_off` 状态；desktop 端为空操作。
- 通用驱动 `src/bsp/esp32/bsp_sleep_button.c`：多 GPIO 轮询消抖（10ms 轮询 / 30ms 消抖，最多 8 个），任意按钮按下即在息屏/唤醒间切换。各板在 `bsp_init` 里用 `bsp_sleep_button_init()` 注册自己的按钮表。
- 现有按钮：CYD / CYD-PLUS / E32R35T / JC8048 / esp32-st7735s-128_160-ec11 / esp32-st7789-320_240-ec11 = 板载 BOOT 键（GPIO0，低电平有效）；esp32s3-st7789-320_240-ec11（S3）/ esp32s3-st7796-480_320-xpt2046-ec11（S3）/ esp32s3-ILI9488-480_320-xpt2046-ec11（S3）/ esp32s3-ILI9341-320_240-xpt2046-ec11（S3）= BOOT（GPIO0）+ 外挂息屏按钮（GPIO39──按键──GND，内部上拉、低电平有效）；esp32c3-st7789-320_240-ec11（C3）= 板载 BOOT 键（GPIO9，低电平有效）。

## GPIO 导航按键（ESP32 端，仅 esp32s3-retro-go）

- 通用后端 `src/bsp/esp32/bsp_gpio_buttons.[ch]`：6 键语义（上/下/左/右/确定/返回）喂 `ui_buttons_send()`；逐 GPIO 独立配置内部上拉/下拉/浮空与高/低电平有效；同一语义键可挂多 GPIO（确定键 1..3 个，任一按下即按下、全部抬起才算抬起，按下计数）。10ms 轮询 + 30ms 消抖，必须在持 LVGL 锁的 lvgl_task 里 `bsp_gpio_buttons_poll()`；事件经 handler 函数指针由 entry（app_main.c）接到语义层（避免 bsp→ui 组件反向依赖；两边枚举同序，静态断言钉死）。息屏时第一次按键只唤醒（bsp_screen_activity 吞键）。
- 板型能力宏 `BSP_HAS_BUTTONS`（`bsp_caps.h`）目前仅 esp32s3-retro-go = 1；键表在各板 `bsp_init` 里 `bsp_gpio_buttons_bind()` 注册，后端不含任何板型引脚。retro-go 键表：UP=7 / DOWN=20 / LEFT=19 / RIGHT=6，OK=A(15)/START(17)/SELECT(16) 并联，BACK=B(5)；MENU(18)/OPTION(8)/BOOT(0) 保留未映射，GPIO0 不注册息屏按钮。

---

## 立创实战派（esp32s3-JLC-SZP）显示驱动——排障记录（**已解决**）

现象：背光亮但整屏全黑，串口一切正常（`BSP ready`、无 abort）。根因叠加了三层：

1. **教程文档（`.reff/szp.md`）的参数是错的**：spi_mode=2（下降沿采样，ST7789 要上升沿）、CS 拉低保持。实测可亮的基准是用户 Arduino 工程（`.reff/jlc-shizhanpai-esp32s3-arduino-lvgl` + 其 fork 的 TFT_eSPI，`.reff/tft_espi_umeiko`）。
2. **面板要求每笔 SPI 交易都有 CS 下降沿**（CS 在 PCA9557 P0 上，常低/常高均全黑）。esp_lcd 面板驱动无法经 I2C 扩展器逐笔翻 CS → 本板 BSP **不用 esp_lcd 面板驱动**，直接 SPI master + 手动控 CS/DC（`src/bsp/esp32/bsp_esp32s3_jlc_szp.c`），ST7789 初始化序列照抄 TFT_eSPI `ST7789_Init.h`（含 SWRESET+150ms，本板无 RST 脚）。
3. **PCA9557 其余脚位必须对齐 Arduino 实测状态**：config=0xFA（P1 保持输入）、空闲 output=0xFB（P2=0"摄像头电源"开，疑似与 TFT 逻辑供电共用，P2=1 整屏黑）。

最终可用配置：SPI3 mode 3 @ 80MHz、BGR、横屏 MADCTL=0x68（180°=0xC8）、**必须 INVON**（INVOFF 全屏反色；`bsp_disp_set_invert` 语义取反，同 esp32-st7735s-128_160-ec11）、像素高字节先发。诊断手法：I2C 扫描 + PCA9557 寄存器回读 + 触摸初始化前整屏推红区分 LCD 链路与外设。

---

## JC8048W550 画面 X 方向抽动/撕裂——排障记录（**已解决**）

现象：画面持续 X 方向抖动/抽动，滑动时狂闪。CYD 2432S028R（SPI 屏）无此问题，仅 RGB 并口屏的 JC8048 有。
**完整排坑指南见 `docs/jc8048w550-rgb-display-guide.md`**（机制链、测量方法、LVGL 内部机制、最终方案全部细节）。

### 已确立的事实（用户实测，可信）

- **厂商 Arduino_GFX demo（`tmp/pio_music`，PCLK 16MHz）：画面稳定**。硬件/时序无恙。
- **IDF 5.5.5 官方 rgbtest 例程（`tmp/rgbtest`）：稳定**。
- PCLK 必须 16MHz；12MHz 下面板不同步退自检变色（黑红白绿蓝循环）。
- **GFX 的 `Arduino_ESP32RGBPanel` 底层就是 `esp_lcd_new_rgb_panel`**（见 `tmp/pio_music/lib/Arduino_GFX-master/src/databus/Arduino_ESP32RGBPanel.cpp`）——与我们是同一个 IDF 驱动。GFX 稳定的秘诀不在驱动而在用法：**拿到 fb 指针后直接 CPU 写 fb，从不再调 draw_bitmap、从不 msync、从不等 vsync**。注意：GFX 不显式 msync 也能显示正确，是靠每次 flush 近百 KB 的 memcpy 制造 cache 驱逐压力被动回写脏行——**不是**因为"EDMA 经 cache 读 PSRAM 与 CPU 天然一致"（此说法已被证伪，见下方最终方案第 5 条）。
- IDF 驱动 `esp_lcd_panel_rgb.c` 的 `rgb_panel_draw_bitmap`：非 direct 路径每次 flush 都 `esp_cache_msync` **整帧 768KB**（686 行附近）；direct 路径（699 行）只刷脏行。整帧 msync 曾高度怀疑是"每秒抽风"（对上 1Hz 时钟刷新）的元凶。

### 已排除项（逐项实测均仍抽）

1. WiFi 干扰——CLI `wifioff` 彻底关驱动后仍抽
2. XIP（代码/常量放 PSRAM）——开也抽、关也抽（两个方向都测过），已决定不开（与 Arduino 对齐）
3. bounce buffer 10/20 行——两次都出随机细线+狂抽，已弃用（`bounce_buffer_size_px = 0`）
4. LVGL 全屏 PSRAM 缓冲 + PARTIAL——更差，已弃用
5. LVGL DIRECT 模式直渲 fb（`esp_lcd_rgb_panel_get_frame_buffer` 拿 fb 给 LVGL）——**更闪**（边扫边画撕裂可见），已弃用
6. smartdisplay 对齐配置（PLL160M / sram_trans_align=4 / psram_trans_align=64 / idle_low=0）——无改善
7. **GFX 同款 flush 路径：内部 SRAM 60 行缓冲 + flush 逐行 memcpy 直写 fb，完全不调 draw_bitmap/msync**（当前 `bsp_jc8048w550.c` 的状态）——**仍抽**。至此写屏路径与 GFX 完全等价，差异必在别处。
8. 应用代码——`tmp/lvtest` 空 IDF 工程（我们的 sdkconfig + LVGL 9.3 + 同款 flush）跑 lv_demo_widgets 也抽 → 问题在系统层
9. CPU 频率——lvtest 降 160MHz 仍抽；cache/PSRAM/flash 配置与 Arduino 工程 diff 后确认一致

### 根因与最终方案（lvtest 已实测：滑动抽动/卷轴错位消除，静态 39fps 纹丝不动）

排障后期用 ISR 测量实锤了完整机制链：

1. **节拍无抖动**：vsync 间隔 25632~25640µs（<10µs），39.0fps 恒定——不是时序问题。
2. **`dma_late`（vsync 到了 DMA 还没发完上帧=欠载帧）只在用户滑动时飙高**（56/46/89 每 5s）→ LVGL 大面积 memcpy 写 fb 挤爆 MSPI 总线 → EDMA 一帧拉不完 → FIFO 见底 → 单帧脏 → 肉眼"抽动"。佐证：widgets demo 里整屏滑动的 Profile/Shop 页抽，只动小图表的 Analytics 页不抽。
3. **IDF 5.5.5 驱动的两种模式都放大了欠载的后果**：stream 模式 `auto_next_frame=true` LCD 永不停 + DMA 环形链 → 欠载后错位**永久保持**（卷轴错位）；`RESTART_IN_VSYNC` 模式 LCD 不停就重启 DMA，靠跳 FIFO_PRESERVE 像素猜 ISR 延迟，猜错就单帧错位（周期性抽动）。
4. **最终方案 = 自研 rgb44 驱动（`tmp/lvtest/main/rgb44.c`，~300 行）+ LVGL DIRECT 双缓冲**：
   - 传输模型照搬 IDF 4.4（厂商 GFX demo 稳的原因）：`auto_next_frame=false` LCD 每帧扫完自动停 + 一次性 DMA 链 + VSYNC_END ISR 里全量重启（gdma_reset→lcd stop→fifo reset→gdma_start→1us→lcd start）。LCD 停着所以重启零竞态，**欠载帧下一帧必然自愈**。
   - 双 fb（PSRAM 各 768KB）+ vsync 换页（ISR 里只换 `gdma_start` 链头，零拷贝）；LVGL `LV_DISPLAY_RENDER_MODE_DIRECT` 直渲两块 fb，零 memcpy → 总线争抢源头消失。
   - **flush_cb 必须阻塞等换页在 vsync 真正生效再 `flush_ready`**（rgb44 提供 swap 信号量，仅在实际换页的 vsync 给出）：否则 LVGL 缓冲轮转与物理扫描脱钩，会往正在扫描的 fb 里渲染/同步拷贝 → 小元素花屏。
   - **换页前必须 `esp_cache_msync(fb, 整帧, DIR_C2M)` 回写**：S3 的 GDMA 读 PSRAM 不过 cache，CPU 渲染的脏行不回写 EDMA 就一直扫旧数据。小面积更新脏行少、长期驻留 cache → 屏幕显示"没变/微微花"；大面积渲染靠驱逐压力被动回写所以看着正常。这是排障最后一块拼图，用户实测确认修复。

### DIRECT 双缓冲的 LVGL 侧要点（`managed_components/lvgl__lvgl/src/core/lv_refr.c`）

- LVGL 9 自己做双缓冲同步：`refr_sync_areas()` 每帧把上帧渲过的区域从"在屏缓冲"拷到"离屏缓冲"，两块 fb 内容保持一致。
- flush_cb 的 `px_map` = `layer->draw_buf->data`（buf_act 基址，无偏移）。
- **一段刷新有多个脏区时会调多次 flush_cb，且 DIRECT 模式下 `buf_act` 只在最后一次 flush（`flushing_last`）后才交换**（`draw_buf_flush` 1376 行）。中途换页 = 后续脏区渲进正在扫描的 fb。

### 关键源码位置

- 本板 BSP：`src/bsp/esp32/bsp_jc8048w550.c`（rgb44 DIRECT 双缓冲、flush_cb 三条铁律）
- 自研驱动：`src/bsp/esp32/rgb44.c` / `rgb44.h`（4.4 传输模型 + 双 fb vsync 换页 + swap 信号量；实验原型在 `tmp/lvtest/main/`）
- IDF RGB 驱动：`C:/esp/v5.5.5/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c`
- 厂商 demo：`tmp/pio_music/src/lvgl_music_gt911_5.0.ino`（LVGL 缓冲 2×800×480/8 像素内部 RAM）

### 第二轮排障备忘（全表字库引入的滑动抽动，已解决）

换 GB2312 全表字库后滑动抽动复发：根因是 5.4MB 字形表在 flash 走 XIP cache 读，
表大 cache 局部性差，文本渲染的 flash 突发在 MSPI 上与 EDMA 扫描争抢 → 帧中
EDMA 饥饿（所有计数器都看不见）。修复：JC8048 链 `_min` 最小子集字体
（`ui_layout.c` 的 `UI_FONT_MIN` 开关），抽动消失。全程细节见 docs 指南第 12 节。
JC8048 当前配置：DIRECT 双缓冲 + -O2 + refr15 + cache line 32B + UI_FONT_MIN=1。

### 排障原则（用户明确要求）

- 不要拉 Arduino core 作依赖；GFX 已经证明就是 esp_lcd，魔改库无意义。
- 每次实验只改一个变量；用 `tmp/pio_music`（25 秒迭代）做驱动级假设验证。
