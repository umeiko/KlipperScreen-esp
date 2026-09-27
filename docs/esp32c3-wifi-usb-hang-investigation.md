# 单核 ESP32-C3 开发记录

更新日期：2026-09-17
适用板型：`esp32c3-st7789-320_240-ec11`（合宙 CORE ESP32-C3 USB 直连版 / ESP32-C3 Super Mini）

## 最终结论

开机 Logo 之后画面卡死的根因不是 PCNT、编码器、Wi-Fi 堆内存或
`esp_wifi_start()` 本身，而是 **LCD 控制命令与 LVGL 首帧 SPI DMA 并发**。

原启动顺序是：

```text
app_main 持有 LVGL 锁
  -> 播放 Logo
  -> 创建 UI
  -> 释放 LVGL 锁
  -> 下发反色 / 180°旋转 / 水平镜像命令
  -> 启动 Wi-Fi
```

释放锁的瞬间，高优先级 `lvgl` 任务立即开始首帧刷新。C3 的屏幕使用
40 行 partial buffer，所以首帧被拆成多次 SPI DMA。与此同时，`app_main`
在同一 LCD/SPI 队列中插入 MADCTL/INV 控制命令，最终使后续颜色传输的
完成信号与等待方失配。典型现象是：

- 顶部约 40 像素（恰好一个 `DRAW_BUF_LINES`）已经渲染；
- 剩余区域仍保留开机 Logo；
- `lvgl` 任务永久等待 `lcd_done`；
- CPU 并非全局死机，因此不一定触发看门狗复位。

最终修复：在 `src/ports/esp32/entry/app_main.c` 中，把反色、旋转、镜像
设置移到 `bsp_lvgl_unlock()` **之前**，保证所有面板命令在 LVGL 首帧
flush 开始前串行完成。实机验证后主页可完整刷出并正常操作。

## 为什么一度误判为 Wi-Fi / USB 问题

原始日志总是停在：

```text
I (...) wifi:mode : sta (...)
I (...) wifi:enable tsf
```

随后 C3 原生 USB Serial/JTAG 也会出现 COM 写入超时或 JTAG 不可用。这与
Espressif 的历史问题高度相似：

- <https://github.com/espressif/esp-idf/issues/8046>
- <https://github.com/espressif/arduino-esp32/issues/6264>
- <https://github.com/espressif/arduino-esp32/pull/6287>

但本项目使用的 ESP-IDF 5.5.5 已在 PHY 启动路径中编译并调用
`phy_bbpll_en_usb(true)`，且实测芯片为 ESP32-C3 revision v0.4，不属于早期
revision 限制。

排障时在 `esp_wifi_start()` 前后直接向 LCD 推送色条：

- 启动前色条出现；
- 调用返回后绿条出现；
- 因当时 MADCTL 设为 BGR，预期的红条实际显示为蓝条。

“蓝条后迅速变绿条”直接证明 `esp_wifi_start()` 正常返回，USB 日志停止
不能当成 CPU 停止的证据。真正的突破点是用户观察到“只刷出顶部
几十像素”，这与 40 行 LVGL partial buffer 完全对应。

## 启动顺序约束

单核 C3 上应把启动阶段分为两类：

1. 持有 LVGL 锁时完成所有 UI 构造和 LCD 面板配置。
2. 释放 LVGL 锁后再启动 Wi-Fi/PHY 等可能阻塞、占用大堆或使 USB 短暂
   失效的外设。

目前结构为：

```text
bsp_lvgl_lock
  -> bsp_input_init
  -> boot_anim_play
  -> ui_app_create
  -> brightness / screen timeout
  -> invert / rotate / mirror
bsp_lvgl_unlock
  -> bsp_wifi_init
  -> bsp_wifi_connect
  -> debug_cli_start
```

`titlebar_init()` 在 ESP32 端不再隐式启动 Wi-Fi；Wi-Fi 由 ESP32 entry 在 UI 就绪
后显式初始化。这不是本次 LCD 死锁的直接修复，但是单核板上必要的
职责分离：网络初始化不应隐藏在 UI 组件创建函数中。

## ST7789 色序

该批面板在 IDF ST7789 驱动中需要：

```c
.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB
```

`LCD_RGB_ELEMENT_ORDER_BGR` 会使红蓝对调。这是 MADCTL 的 RGB/BGR 位，与
RGB565 像素的高低字节交换是两个独立问题：

- MADCTL RGB/BGR：决定红色与蓝色分量的解释顺序；
- `flush_cb()` 内的 16 位字节交换：适配 CPU 小端 RGB565 与 SPI 高字节先发。

不要为了修红蓝反转去删掉字节交换。

## ESP32-C3 无 PCNT 时的 EC11

C3 没有 PCNT 外设，本项目用 `esp_timer` 轮询 A/B 相，再通过四位状态
转移表累加 `-1/0/+1`。

初始实现使用 10 ms 周期（100 Hz）。这不是优先级不够：`esp_timer`
回调本来就由 IDF 的高优先级 timer task 派发。真正的问题是采样率太低，
快速转动时 A/B 在相邻采样间可以跨过多个状态，已丢失的边沿无法由算法
恢复。

当前调整为 2 ms（500 Hz）：

```c
#define QDEC_POLL_US 2000
```

这个频率对手动 EC11 快速旋转留有足够余量，每次回调只读两个 GPIO 并查
16 项转移表，对 160 MHz C3 的开销可忽略。机械抖动引起的相反转移会在
转移表内相互抵消，不需要通过降低采样率消抖。

若 2 ms 实测仍丢步，优先检查：

1. A/B 相是否有可靠上拉；
2. `counts_per_detent=4` 是否匹配实际编码器；
3. 触点波形是否有过长振铃或外接 RC 延迟；
4. 再考虑 1 ms 轮询，而不是先改成 GPIO 双边沿中断。

GPIO 中断方案虽然不丢边沿，但机械抖动、悬浮或接线噪声可能在 C3 单核上
形成中断风暴；对人手旋钮，2 ms 轮询是更稳健的折中。

## 其他 C3 配置要点

- Flash 必须使用 DIO；合宙 CORE 的这批接线用 QIO 无法正常启动。
- 本机控制台使用 USB-Serial-JTAG（GPIO18/19）。USB 日志不可作为唯一存活
  证据；排查 PHY/Wi-Fi 时应同时使用屏幕标记或 UART0（GPIO20/21）。
- C3 是单核，任务“不绑核”不代表能与双核板一样隔离；高优先级 Wi-Fi、
  `esp_timer`、LVGL 仍在同一 CPU 上调度。
- 面板命令和像素 flush 共用 SPI 设备时，“命令调用很短”不等于可以无锁并发。
- C3 无 PSRAM，当前 LVGL 双 partial buffer 为 `2 × 320 × 40 × 2 = 51.2 KB`。
- Bambu Cloud Monitor 接入后，旧的 `-Og` canonical 配置会超出 app 分区约
  18 KiB。不能直接移动 LittleFS 扩大 app（升级会让已有配置失效），因此 C3
  单独改用 `-Os`；v0.5.9 全量构建的 app 为 `0x2fced0`，在 `0x320000`
  分区中还剩 `0x23130`（约 140 KiB / 4%）。后续新增大模块仍须先核对体积。
- `sdkconfig.defaults.<board>` 不会覆盖已生成的 canonical `sdkconfig.<board>`；
  修改 Kconfig 时必须同步检查两者。

## 排障方法复盘

本次有效的最短路径：

1. 核对最后一条日志的真实语义，不把“串口停了”等同于“CPU 停了”。
2. 在可疑阻塞调用前后用不依赖 USB 的屏幕色条标记。
3. 严格一次只改一个变量。将 Wi-Fi 移出 LVGL 锁后仍卡，排除其为直接根因。
4. 记录屏幕已经刷出的几何范围；“约 40 像素”比“卡在 Logo”更有信息量。
5. 用已知缓冲高度将现象映射到具体 flush，再检查该 flush 的共享资源和时序。
6. 修正 LCD 命令与首帧 DMA 的串行关系后，实机验证“主页完整并可操作”。

## 构建与烧录

```bash
bash tools/build-esp32.sh esp32c3-st7789-320_240-ec11
bash tools/build-esp32.sh esp32c3-st7789-320_240-ec11 flash COM68
```

若 Windows 上脚本因 CRLF 无法被 WSL bash 执行，可在
`src/ports/esp32` 目录直接调用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "../../../tools/idf.ps1" `
  -B "build-esp32c3-st7789-ec11" `
  -DSDKCONFIG="sdkconfig.esp32c3-st7789-320_240-ec11" `
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c3-st7789-320_240-ec11" `
  -p "COM68" flash
```

该实物板通常需要手动拉入下载模式，烧录后再手动按一次 RESET 进入应用。
