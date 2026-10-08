# ESP32-C3 板型

基于 ESP32-C3（单核 RISC-V 160MHz，400KB SRAM，无 PSRAM；flash 走两线 DIO 模式）的板型。

## esp32c3-st7789-320_240-ec11

**刷机包**: [ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip)

![ESP32-C3、ST7789 屏幕与 EC11 编码器实机](../screenshots/boards/esp32c3_st7789_320_240_ec11.jpg)

为袖珍 **ESP32-C3** 板做的纯旋钮机型：240×320 ST7789 SPI 屏 + EC11 编码器，无触摸。逻辑分辨率 **320×240 横屏**——标准布局档。一个固件镜像同时覆盖两块目标板（接线完全相同）：

- **合宙 CORE ESP32-C3**——选 **USB 直连版（非 CH340）**；烧录和串口 CLI 都直接走 Type-C 口（USB-Serial-JTAG），Windows 8 以上免驱
- **ESP32-C3 Super Mini**——排针只引出 GPIO0–10/20/21，本方案每根线都落在 GPIO0–10 内

ESP32-C3 与其它目标芯片有三点差异，固件已全部处理：**单核**（LVGL 任务不绑核运行）、**无 PCNT 外设**（EC11 改用 2ms 定时轮询软件正交解码）、合宙板 flash 为**两线 DIO 模式**（固件按 `FLASHMODE_DIO` 构建，Super Mini 同样兼容）。注意：C3 只有 400KB SRAM 且无 PSRAM 可选，堆空间比 ESP32 板紧张——Klipper/Moonraker 是主要使用场景。

- 主控：ESP32-C3（单核 RISC-V 160MHz，400KB SRAM），4MB flash @ 80MHz **DIO**
- 显示：ST7789 走 esp_lcd 官方驱动（默认 INVOFF 即正常颜色）；SPI2 @ 40MHz，DMA 双缓冲（2×320×40）
- 输入：仅 EC11（定时轮询软件正交解码）；无触摸层，永不进入触摸校准
- 背光：GPIO4，LEDC PWM 8bit/5kHz，高电平点亮
- 息屏/唤醒：板载 BOOT 键（GPIO9）

| 模块引脚 | ESP32-C3 引脚 | 用途 |
|---|---|---|
| ST7789 VCC | 3V3 | 屏幕供电 |
| ST7789 GND | GND | 地 |
| ST7789 SCL / SCK | GPIO2 | SPI 时钟 |
| ST7789 SDA / MOSI | GPIO3 | SPI 数据出 |
| ST7789 CS | GPIO7 | 片选 |
| ST7789 DC / RS | GPIO6 | 数据/命令选择 |
| ST7789 RST / RES | GPIO5 | 屏幕复位 |
| ST7789 BL / LED / BLK | GPIO4 | 背光，高电平点亮 |
| EC11 CLK / A | GPIO0 | 内部上拉 |
| EC11 DT / B | GPIO1 | 内部上拉 |
| EC11 SW / KEY | GPIO10 | 内部上拉，低电平有效 |
| EC11 C / GND | GND | A/B/SW 公共端接地 |

刻意避开的引脚：GPIO8（Super Mini 板载 LED）、GPIO12/13（合宙板载 LED，且 flash 为 DIO 模式用 QIO 无法启动）、GPIO18/19（USB）、GPIO20/21（UART0）。不同卖家的 ST7789 模组有差异：若画面镜像或边缘出现彩色偏移带，调整 `src/bsp/esp32/bsp_esp32c3_st7789_ec11.c` 顶部的 `LCD_MIRROR_X/Y` 与 `LCD_GAP_X/Y` 后重新编译。旋转与按下承担全部导航，息屏后任一动作均可唤醒。
