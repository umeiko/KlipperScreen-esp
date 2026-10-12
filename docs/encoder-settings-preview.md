# 显示与编码器设置：开发预览与验证

Windows 交互已验收，v0.5.8 接入 ESP32。用户操作指南见
[显示与编码器设置](display-settings.md)；本文保留开发预览与复现方式。

## 为什么会出现转两格才动一下

这里需要匹配的是**每个机械卡位产生的正交计数**，不是编码器每圈的格数。
例如硬件每格产生 2 次计数，而固件按 4 次算一步，就需要转两格才能移动一项。
反过来，参数过小会一次跳过多个选项。接触不良、抖动或丢脉冲也会影响步进，
此设置不能代替这些硬件问题的排查。

## 交互

入口：设置 → 显示设置 → 编码器步进。

- 菜单只提供 **1、2、4** 三档，当前值直接显示数字，不显示“默认（4）”。
- 没有保存过偏好时使用板型的 `CONFIG_INPUT_ROTARY_COUNTS_PER_DETENT`，目前通常为 4；这只是内部初始值，不额外占菜单选项。
- 两格走一下：例如从 4 试调到 2；一格跳多项则增大数值。
- 选中后仅临时应用，弹窗给出 **20 秒**，默认焦点为“恢复”。
- 转动检查能否每格切换一项；明确按“确认”才保存到 `klipperscreen.conf` 的 `encoder_counts`。
- 点“恢复”、返回、超时或离开页面均恢复原值；超时后的迟到确认不会保存。
- 未确认时重启不会保留试用值。内部继续兼容旧配置值 0（沿用板型默认）及旧预览的非标准数值，但不在菜单里铺开。

Marlin 的 `ENCODER_PULSES_PER_STEP` 也是每步计数阈值，并不是 EC11 型号枚举。
[配置示例](https://github.com/MarlinFirmware/Marlin/blob/2.1.x/Marlin/Configuration.h) 使用 4，
[板型条件配置](https://github.com/MarlinFirmware/Marlin/blob/2.1.x/Marlin/src/inc/Conditionals_LCD.h) 中也有 1、2、5。
Marlin 还另有 `ENCODER_STEPS_PER_MENU_ITEM`，不应把它与本项目的每格正交计数混为一谈。
本项目面向常见 EC11 的用户界面只保留 1/2/4。

正式 ESP32 构建按 `BSP_HAS_ROTARY_ENCODER` 能力宏裁剪，来源为
`CONFIG_INPUT_ROTARY_ENCODER`，不按板名散落判断。无编码器构建和正式桌面端不展示。
共用 BSP API 同时接入 PCNT 与 C3 软件正交解码；调整参数时清除未完成的半格计数。

## Windows 预览

仅为模拟器显式开启 CMake 选项 `KR_ENCODER_SETTINGS_PREVIEW=ON`，默认 OFF。
产品桌面可执行文件不受此开关影响。现有 MSYS2 构建环境下：

```sh
cmake -S src/ports/desktop -B src/ports/desktop/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKR_ENCODER_SETTINGS_PREVIEW=ON
cmake --build src/ports/desktop/build --target klipper_remote_simulator
```

用 `--panel display` 启动模拟器可直接进入显示设置。使用独立的
`KLIPPER_CONFIG_DIR` 目录，以免改变现有模拟器/真实打印机配置。

- 鼠标滚轮模拟转动，中键模拟按下；也可以鼠标点击确认/恢复。
- 默认模拟每格 2 次计数：设置为 4 时两格一步，改为 2 后每格一步。
- 开发环境变量 `KLIPPER_ENCODER_HW_COUNTS=1..8` 可更换模拟硬件，默认 2。
- 方向键/回车是另一套导航输入，不代表物理编码器步进。
- 预览完成后用 `-DKR_ENCODER_SETTINGS_PREVIEW=OFF` 恢复普通模拟器构建。

针对性检查：预览版构建后运行 `powershell -File tools/test-encoder-settings.ps1`。
它复用实际 LVGL、显示设置、SDL 输入和配置读写，验证滚轮导航、确认、取消、
超时、迟到确认、离页、恢复原值、配置打开失败及非法值，并在独立临时目录输出截图。
设置 `KLIPPER_RES=160x128` 可检查小屏布局。它不证明实体编码器电气或实机兼容性。

## 屏幕色序预览

额外开启 `KR_DISPLAY_SETTINGS_PREVIEW=ON`，显示设置增加“屏幕色序”入口。
点击进入下一级页面，直接用三行单选列表选择默认 / RGB / BGR，不再使用下拉框；
当前项标记“当前”，转动只移动焦点，按下才选择。返回后入口摘要显示所选值。
普通构建恢复时也须把该开关设回 OFF；两个预览开关都只作用于模拟器目标。
子页面下方三色块的正确外观依次为红/绿/蓝，标注跟随界面语言：中文“红/绿/蓝”、
英文“red/green/blue”、繁体“紅/綠/藍”、法语“rouge/vert/bleu”、意大利语“rosso/verde/blu”。
本轮重新生成 10/12/14/16 号字库（含压缩变体），补齐新增繁体字；不改 JC8048 的 28/32 最小字库。
RGB 与 BGR 仅交换红蓝，绿色不变；
这与反色、RGB565 的高低字节顺序不是同一件事。

预览实时切换并保存 `display_color_order=0/1/2`，不用重启；选错可随时切回来，
不需要编码器设置那种超时确认。SDL 原生 RGB，所以默认与 RGB 相同，BGR 会让红蓝互换。
该选项按独立 BSP 显示能力开放，不依赖有无编码器；正式桌面端不开放。

ESP32 标准 SPI LCD 使用 `bsp_lcd_color_io`，基于 IDF 公开 IO 接口转发，
仅改每次 MADCTL 的色序位，不访问驱动私有字段、不拷贝像素；后续旋转/镜像仍保留所选色序。
DMA 与回调沿用原 transport，回调收到的 IO handle 也仍是原 transport。
立创实战派在自有 MADCTL helper 中处理同一个色序位。默认值保留各板原设置，
启动时在开机动画前恢复偏好；不改 JC8048 已验证的显示路径。

先完成一次 `esp32-st7789-320_240-ec11` 构建（生成 sdkconfig 头文件），再运行
`powershell -File tools/test-lcd-color-io.ps1`，用真实 IDF 头文件和假 transport 检查
两种板型默认、三种设置与全部 MADCTL 位组合、传输失败不污染状态、复位、像素和回调转发。
它证明软件协议逻辑，不代表全部实体面板已通过实测。
