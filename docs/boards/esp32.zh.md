# ESP32 板型

基于经典 ESP32（双核 240MHz，4MB Flash，无 PSRAM）的板型。每节给出刷机包、接线与引脚表。

## CYD 2432S028R

**刷机包**: [ESP-IDFv5.5-cyd_2432s028r.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-cyd_2432s028r.zip)

![CYD 2432S028R](../screenshots/boards/cyd_2432s028r.jpg)

*"Cheap Yellow Display"（黄色 PCB 的 2.8" 开发板），本项目的参考板型。* 图：[Random Nerd Tutorials](https://randomnerdtutorials.com/cheap-yellow-display-esp32-2432s028r/)

逻辑分辨率 **320×240 横屏**。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ILI9341，SPI2 @ 40MHz，DMA 双缓冲（2 × 40 行）
- 触摸：XPT2046 电阻屏，**独立 SPI3 总线**（与 LCD 不同总线）；出厂触摸校准已内置
- 背光：GPIO21，LEDC PWM 8bit/5kHz，高电平点亮

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD SCLK / MOSI / MISO | 14 / 13 / 12 | SPI2 |
| LCD CS / DC / RST | 15 / 2 / 4 | |
| LCD 背光 | 21 | LEDC PWM |
| 触摸 SCLK / MOSI / MISO | 25 / 32 / 39 | SPI3，与 LCD 不同总线 |
| 触摸 CS / IRQ | 33 / 36 | |
| BOOT 按键 | 0 | 息屏/唤醒 |

**可选 EC11 旋转编码器**

CYD 固件默认已启用旋转编码器支持（PCNT 硬件正交解码）。把裸 EC11 接到扩展 IO 口即可，编码器与触摸并存——旋转移动焦点，按下确认。

| EC11 引脚 | GPIO | 备注 |
|---|---|---|
| A | 35 | **需外接 ~10kΩ 上拉电阻到 3V3**——GPIO35 为输入专用脚，无内部上拉 |
| B | 22 | 内部上拉 |
| SW（按下） | 27 | 内部上拉，低电平有效 |
| C / GND | GND | A/B/SW 的公共端接 GND |

自带 A/B 上拉的编码器模块可直接接线。

## CYD 2432S028R-PLUS

**刷机包**: [ESP-IDFv5.5-cyd_2432s028r_plus.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-cyd_2432s028r_plus.zip)

![CYD 2432S028R-PLUS](../screenshots/boards/cyd_2432s028r_plus.png)

*CYD 的 ST7789 变种（ESP32-WROOM-32E 模组）。同一板族、同一引脚——只有显示驱动 IC 和复位线不同。*

逻辑分辨率 **320×240 横屏**。显示与触摸接线与上面的 CYD 2432S028R **完全相同**（LCD 走 SPI2：SCLK 14 / MOSI 13 / MISO 12 / CS 15 / DC 2 / BL 21；XPT2046 触摸走独立 SPI3：SCLK 25 / MOSI 32 / MISO 39 / CS 33 / IRQ 36；BOOT 键 GPIO0 息屏/唤醒）。差异均由固件处理：

- 显示：**ST7789**（替代 ILI9341），按厂商文档要求 BGR 色序初始化；SPI2 @ 40MHz，DMA 双缓冲
- **无 LCD 复位脚**（`TFT_RST = -1`）——靠初始化序列内的软件复位
- 触摸控制器、校准流程与出厂默认值与 CYD 共用（随时可用 `caltouch` 重校）

**可选 EC11 旋转编码器**——本板编码器走 **CN3 排针：A=GPIO23（MOSI）/ B=GPIO19（MISO）/ SW=GPIO18（SCK）**，公共端接 GND。三脚均有内部上拉，裸编码器直插即可，无需外接电阻。注意这三个脚与板载 SD 卡槽共用，不能同时插 SD 卡使用。

个别单元画面颠倒时，在 **设置 → 显示 → 180° 旋转** 切换；颜色反色时在同一页切换反色选项——不用改接线也不用重新编译。

## E32R35T

**刷机包**: [ESP-IDFv5.5-e32r35t.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-e32r35t.zip)

![E32R35T](../screenshots/boards/e32r35t.png)

*ESP32-32E 3.5" 显示模组（[lcdwiki 资料页](https://www.lcdwiki.com/zh/3.5inch_ESP32-32E_Display)，触摸版 SKU：E32R35T）。* 图：lcdwiki

逻辑分辨率 **480×320 横屏**。

- 主控：ESP32-WROOM-32E（双核 240MHz），4MB QIO Flash
- 显示：ST7796U，SPI2 @ 40MHz；**与触摸屏共用 SPI 总线**（厂商设计）；无独立 RST（与 ESP32 EN 共用，驱动走软件复位）
- 触摸：XPT2046 电阻屏，共用 SPI2；出厂触摸校准已内置（个体差异可用串口 CLI `caltouch` 重校）
- 背光：GPIO27，高电平点亮

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD+触摸 SCLK / MOSI / MISO | 14 / 13 / 12 | SPI2，两设备共用 |
| LCD CS / DC | 15 / 2 | |
| LCD RST | — | 与 EN 共用 |
| LCD 背光 | 27 | LEDC PWM |
| 触摸 CS / IRQ | 33 / 36 | XPT2046 |
| RGB 三色灯 R / G / B | 22 / 16 / 17 | 共阳极，低电平点亮（固件未使用） |
| MicroSD CS / MOSI / SCLK / MISO | 5 / 23 / 18 / 19 | 独立 SPI 组（固件未使用） |
| 音频使能 / DAC 输出 | 4 / 26 | 喇叭接口（固件未使用） |
| 电池电压 ADC | 34 | 输入 |
| BOOT 按键 | 0 | 息屏/唤醒 |

## esp32-st7735s-128_160-ec11

**刷机包**: [ESP-IDFv5.5-esp32-st7735s-128_160-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-st7735s-128_160-ec11.zip)

![1.8 寸 ST7735S 模组](../screenshots/boards/ec11_knob_esp32_st7735s.png)

*常见的 1.8" 128×160 ST7735S SPI 模组。排针从上到下：GND / VCC / SCL / SDA / RES / DC / CS / BLK——注意这里的 `SCL`/`SDA` 是 SPI 的 SCLK 和 MOSI，不是 I2C。*

与 CYD 2432S028R **同款主控（ESP32）** 的纯旋钮最小系统：1.8" 128×160 ST7735S SPI 屏 + EC11 编码器，无触摸。所有 IO 分配都与 CYD 的板载 LCD 排针及其外挂 EC11 接法一一对应，CYD 底板（或任意 ESP32 开发板按同样接线）可直接使用。逻辑分辨率 **160×128 横屏**。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ST7735S，走 ST7789 兼容的 esp_lcd 驱动并开启反色（ST7735S 必须 INVON）；SPI2 @ 40MHz，DMA 双缓冲
- 输入：仅 EC11（PCNT 硬件正交解码）；无触摸层，不会进入触摸校准
- 背光：GPIO21，LEDC PWM 8bit/5kHz，高电平点亮
- 息屏/唤醒：板载 BOOT 键（GPIO0）

| 模块引脚 | ESP32 引脚 | 用途 |
|---|---|---|
| ST7735S VCC | 3V3 | 屏幕供电 |
| ST7735S GND | GND | 地 |
| ST7735S SCL / SCK | GPIO14 | SPI 时钟 |
| ST7735S SDA / MOSI | GPIO13 | SPI 数据输出 |
| ST7735S CS | GPIO15 | 片选 |
| ST7735S DC / RS | GPIO2 | 数据/命令选择 |
| ST7735S RST / RES | GPIO4 | 屏幕复位 |
| ST7735S BL / LED / BLK | GPIO21 | 背光，高电平点亮 |
| EC11 CLK / A | GPIO35 | **需外接 ~10kΩ 上拉到 3V3**——GPIO35 只能输入且无内部上拉 |
| EC11 DT / B | GPIO22 | 内部上拉 |
| EC11 SW / KEY | GPIO27 | 内部上拉，低电平有效 |
| EC11 C / GND | GND | A/B/SW 公共端接地 |

不同卖家的 ST7735S 模组有差异：画面镜像或边缘出现彩边/偏移时，改 `src/bsp/esp32/bsp_ec11_knob_esp32.c` 顶部的 `LCD_MIRROR_X/Y` 与 `LCD_GAP_X/Y` 重新编译即可。EC11 的旋转与按下都能导航，屏幕自动熄灭后再次操作旋钮即可唤醒。

## esp32-st7789-320_240-ec11

**刷机包**: [ESP-IDFv5.5-esp32-st7789-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-st7789-320_240-ec11.zip)

![ST7789 240×320 模组与 EC11](../screenshots/boards/esp32_st7789_320_240_ec11.png)

*常见的 240×320 ST7789 SPI 模组搭配 EC11 编码器。排针一般印 GND / VCC / SCL / SDA / RES / DC / CS / BLK——这里的 `SCL`/`SDA` 是 SPI 的 SCLK 和 MOSI，不是 I2C。*

与 esp32-st7735s-128_160-ec11 **同款主控（ESP32）、完全相同引脚**（全部对齐 CYD 2432S028R）的纯旋钮中屏机型：240×320 ST7789 SPI 屏 + EC11 编码器，无触摸。逻辑分辨率 **320×240 横屏**——标准布局档，与 CYD 一致。相对 ST7735S 机型只是换了一块屏，所有接线原位不动。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ST7789，走 esp_lcd 官方驱动（默认 INVOFF 即正常颜色，无需强制反色）；SPI2 @ 40MHz，DMA 双缓冲（2×320×40）
- 输入：仅 EC11（PCNT 硬件正交解码）；无触摸层，不会进入触摸校准
- 背光：GPIO21，LEDC PWM 8bit/5kHz，高电平点亮
- 息屏/唤醒：板载 BOOT 键（GPIO0）

| 模块引脚 | ESP32 引脚 | 用途 |
|---|---|---|
| ST7789 VCC | 3V3 | 屏幕供电 |
| ST7789 GND | GND | 地 |
| ST7789 SCL / SCK | GPIO14 | SPI 时钟 |
| ST7789 SDA / MOSI | GPIO13 | SPI 数据输出 |
| ST7789 CS | GPIO15 | 片选 |
| ST7789 DC / RS | GPIO2 | 数据/命令选择 |
| ST7789 RST / RES | GPIO4 | 屏幕复位 |
| ST7789 BL / LED / BLK | GPIO21 | 背光，高电平点亮 |
| EC11 CLK / A | GPIO35 | **需外接 ~10kΩ 上拉到 3V3**——GPIO35 只能输入且无内部上拉 |
| EC11 DT / B | GPIO22 | 内部上拉 |
| EC11 SW / KEY | GPIO27 | 内部上拉，低电平有效 |
| EC11 C / GND | GND | A/B/SW 公共端接地 |

不同卖家的 ST7789 模组有差异：画面镜像或边缘出现彩边/偏移时，改 `src/bsp/esp32/bsp_ec11_knob_esp32_st7789.c` 顶部的 `LCD_MIRROR_X/Y` 与 `LCD_GAP_X/Y` 重新编译即可。EC11 的旋转与按下都能导航，屏幕自动熄灭后再次操作旋钮即可唤醒。

## esp32-ILI9341-320_240-ec11

**刷机包**: [ESP-IDFv5.5-esp32-ILI9341-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-ILI9341-320_240-ec11.zip)

![ILI9341 240×320 模组与 EC11 旋钮](../screenshots/boards/esp32_ili9341_320_240_ec11.jpg)

*240×320 ILI9341 SPI 屏搭配 EC11 编码器旋钮（图为纵维立方 Kobra 2 Neo 的原厂显示屏组件）。排针一般印 GND / VCC / SCL / SDA / RES / DC / CS / BLK——这里的 `SCL`/`SDA` 是 SPI 的 SCLK 和 MOSI，不是 I2C。*

与 esp32-st7789-320_240-ec11 **同款主控（ESP32）、完全相同引脚**（全部对齐 CYD 2432S028R）的纯旋钮中屏机型：240×320 ILI9341 SPI 屏 + EC11 编码器，无触摸。逻辑分辨率 **320×240 横屏**——标准布局档，与 CYD 一致。相对 ST7789 机型只是换了一块屏，所有接线原位不动。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ILI9341，走 esp_lcd 官方驱动（面板 BGR，默认 INVOFF 即正常颜色）；SPI2 @ 40MHz，DMA 双缓冲（2×320×40）
- 输入：仅 EC11（PCNT 硬件正交解码）；无触摸层，不会进入触摸校准
- 背光：GPIO21，LEDC PWM 8bit/5kHz，高电平点亮
- 息屏/唤醒：板载 BOOT 键（GPIO0）

| 模块引脚 | ESP32 引脚 | 用途 |
|---|---|---|
| ILI9341 VCC | 3V3 | 屏幕供电 |
| ILI9341 GND | GND | 地 |
| ILI9341 SCL / SCK | GPIO14 | SPI 时钟 |
| ILI9341 SDA / MOSI | GPIO13 | SPI 数据输出 |
| ILI9341 CS | GPIO15 | 片选 |
| ILI9341 DC / RS | GPIO2 | 数据/命令选择 |
| ILI9341 RST / RES | GPIO4 | 屏幕复位 |
| ILI9341 BL / LED / BLK | GPIO21 | 背光，高电平点亮 |
| EC11 CLK / A | GPIO35 | **需外接 ~10kΩ 上拉到 3V3**——GPIO35 只能输入且无内部上拉 |
| EC11 DT / B | GPIO22 | 内部上拉 |
| EC11 SW / KEY | GPIO27 | 内部上拉，低电平有效 |
| EC11 C / GND | GND | A/B/SW 公共端接地 |

不同卖家的 ILI9341 模组有差异：画面镜像或边缘出现彩边/偏移时，改 `src/bsp/esp32/bsp_esp32_ili9341_ec11.c` 顶部的 `LCD_MIRROR_X/Y` 与 `LCD_GAP_X/Y` 重新编译即可。EC11 的旋转与按下都能导航，屏幕自动熄灭后再次操作旋钮即可唤醒。

## esp32-ST7796-320_240-ec11

**刷机包**: [ESP-IDFv5.5-esp32-ST7796-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-ST7796-320_240-ec11.zip)

*240×320 ST7796 SPI 屏搭配 EC11 编码器旋钮——esp32-ILI9341-320_240-ec11 的 ST7796 姊妹机型（暂无本机型实物图）。排针一般印 GND / VCC / SCL / SDA / RES / DC / CS / BLK——这里的 `SCL`/`SDA` 是 SPI 的 SCLK 和 MOSI，不是 I2C。*

与 esp32-ILI9341-320_240-ec11 **同款主控（ESP32）、完全相同引脚**（全部对齐 CYD 2432S028R）的纯旋钮中屏机型：240×320 ST7796 SPI 屏 + EC11 编码器，无触摸。逻辑分辨率 **320×240 横屏**——标准布局档，与 CYD 一致。相对 ILI9341 机型只是换了显示控制器，所有接线原位不动。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ST7796，走 esp_lcd 官方驱动（面板 BGR，默认 INVOFF 即正常颜色）；SPI2 @ 40MHz，DMA 双缓冲（2×320×40）
- 输入：仅 EC11（PCNT 硬件正交解码）；无触摸层，不会进入触摸校准
- 背光：GPIO21，LEDC PWM 8bit/5kHz，高电平点亮
- 息屏/唤醒：板载 BOOT 键（GPIO0）

| 模块引脚 | ESP32 引脚 | 用途 |
|---|---|---|
| ST7796 VCC | 3V3 | 屏幕供电 |
| ST7796 GND | GND | 地 |
| ST7796 SCL / SCK | GPIO14 | SPI 时钟 |
| ST7796 SDA / MOSI | GPIO13 | SPI 数据输出 |
| ST7796 CS | GPIO15 | 片选 |
| ST7796 DC / RS | GPIO2 | 数据/命令选择 |
| ST7796 RST / RES | GPIO4 | 屏幕复位 |
| ST7796 BL / LED / BLK | GPIO21 | 背光，高电平点亮 |
| EC11 CLK / A | GPIO35 | **需外接 ~10kΩ 上拉到 3V3**——GPIO35 只能输入且无内部上拉 |
| EC11 DT / B | GPIO22 | 内部上拉 |
| EC11 SW / KEY | GPIO27 | 内部上拉，低电平有效 |
| EC11 C / GND | GND | A/B/SW 公共端接地 |

不同卖家的 ST7796 模组在 320×240 窗口下有差异：画面镜像/颠倒或边缘出现彩边/偏移时，改 `src/bsp/esp32/bsp_esp32_st7796_ec11.c` 顶部的 `LCD_MIRROR_X/Y` 与 `LCD_GAP_X/Y` 重新编译即可。EC11 的旋转与按下都能导航，屏幕自动熄灭后再次操作旋钮即可唤醒。
