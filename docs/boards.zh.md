# 支持的板子

| 板型 | 构建目标 | 屏幕 | 触摸 | 主控 / Flash | 状态 |
|---|---|---|---|---|---|
| [CYD 2432S028R](#cyd-2432s028r) | `cyd_2432s028r` | 2.8" 240×320 ILI9341 SPI | XPT2046 电阻 | ESP32 / 4MB | ✅ 稳定 |
| [CYD 2432S028R-PLUS](#cyd-2432s028r-plus) | `cyd_2432s028r_plus` | 2.8" 240×320 ST7789 SPI | XPT2046 电阻 | ESP32-WROOM-32E / 4MB | 🆕 新机型，CYD 引脚 |
| [E32R35T](#e32r35t) | `e32r35t` | 3.5" 320×480 ST7796U SPI | XPT2046 电阻 | ESP32-32E / 4MB | ✅ 稳定 |
| [esp32s3-st7789-320_240-ec11](#esp32s3-st7789-320_240-ec11) | `esp32s3-st7789-320_240-ec11` | 240×320 ST7789 SPI | 无，纯旋钮 | ESP32-S3 N16R8 / 16MB | ✅ 官方参考，贡献者实机验证 |
| [esp32-st7735s-128_160-ec11](#esp32-st7735s-128_160-ec11) | `esp32-st7735s-128_160-ec11` | 1.8" 128×160 ST7735S SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32-st7789-320_240-ec11](#esp32-st7789-320_240-ec11) | `esp32-st7789-320_240-ec11` | 240×320 ST7789 SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32-ILI9341-320_240-ec11](#esp32-ili9341-320_240-ec11) | `esp32-ILI9341-320_240-ec11` | 240×320 ILI9341 SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32-ST7796-320_240-ec11](#esp32-st7796-320_240-ec11) | `esp32-ST7796-320_240-ec11` | 240×320 ST7796 SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32s3-st7796-480_320-xpt2046-ec11](#esp32s3-st7796-480_320-xpt2046-ec11) | `esp32s3-st7796-480_320-xpt2046-ec11` | 480×320 ST7796S SPI | XPT2046 电阻（共总线）+ EC11 | ESP32-S3 N16R8 / 16MB | 🆕 新机型 |
| [esp32s3-ILI9488-480_320-xpt2046-ec11](#esp32s3-ili9488-480_320-xpt2046-ec11) | `esp32s3-ILI9488-480_320-xpt2046-ec11` | 3.5" 480×320 ILI9488 SPI | XPT2046 电阻（共总线）+ EC11 | ESP32-S3 N16R8 / 16MB | 🆕 新机型，MKS PI-TS35 |
| [esp32s3-ILI9341-320_240-xpt2046-ec11](#esp32s3-ili9341-320_240-xpt2046-ec11) | `esp32s3-ILI9341-320_240-xpt2046-ec11` | 320×240 ILI9341 SPI | XPT2046 电阻（共总线）+ EC11 | ESP32-S3 N16R8 / 16MB | 🆕 新机型 |
| [JC8048W550](#jc8048w550) | `jc8048w550` | 5" 800×480 ST7262 RGB 并口 | GT911 电容 | ESP32-S3 / 16MB | ✅ 稳定 |
| [SenseCAP Indicator](#sensecap-indicator) | `esp32s3-sensecap-indicator` | 4" 480×480 ST7701S RGB 并口 | FT5x06 电容 | ESP32-S3 N8R8 / 8MB | 🆕 新机型 |
| [立创实战派 ESP32-S3](#立创实战派-esp32-s3) | `esp32s3-JLC-SZP` | 2.0" 240×320 ST7789 SPI | FT6336 电容 | ESP32-S3 N16R8 / 16MB | ✅ 已实机验证 |
| [esp32s3-retro-go](#esp32s3-retro-go) | `esp32s3-retro-go` | 3.2" 240×320 ST7789 SPI | 无，GPIO 按键 | ESP32-S3 / 16MB | 🆕 新机型 |
| [esp32c3-st7789-320_240-ec11](#esp32c3-st7789-320_240-ec11) | `esp32c3-st7789-320_240-ec11` | 240×320 ST7789 SPI | 无，纯旋钮 | ESP32-C3 / 4MB | 🆕 新机型，合宙 CORE/Super Mini 通用 |

刷机包命名：`ESP-IDFv5.5-<board>.zip`（资产名不带版本号，下面的直链永远指向最新正式版）。遇到问题请到 [Issues](https://github.com/umeiko/KlipperScreen-esp/issues) 反馈。

| 板型 | 刷机包直链（最新正式版） |
|---|---|
| CYD 2432S028R | [ESP-IDFv5.5-cyd_2432s028r.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-cyd_2432s028r.zip) |
| CYD 2432S028R-PLUS | [ESP-IDFv5.5-cyd_2432s028r_plus.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-cyd_2432s028r_plus.zip) |
| E32R35T | [ESP-IDFv5.5-e32r35t.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-e32r35t.zip) |
| esp32s3-st7789-320_240-ec11 | [ESP-IDFv5.5-esp32s3-st7789-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-st7789-320_240-ec11.zip) |
| esp32-st7735s-128_160-ec11 | [ESP-IDFv5.5-esp32-st7735s-128_160-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-st7735s-128_160-ec11.zip) |
| esp32-st7789-320_240-ec11 | [ESP-IDFv5.5-esp32-st7789-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-st7789-320_240-ec11.zip) |
| esp32-ILI9341-320_240-ec11 | [ESP-IDFv5.5-esp32-ILI9341-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-ILI9341-320_240-ec11.zip) |
| esp32-ST7796-320_240-ec11 | [ESP-IDFv5.5-esp32-ST7796-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32-ST7796-320_240-ec11.zip) |
| esp32s3-st7796-480_320-xpt2046-ec11 | [ESP-IDFv5.5-esp32s3-st7796-480_320-xpt2046-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-st7796-480_320-xpt2046-ec11.zip) |
| esp32s3-ILI9488-480_320-xpt2046-ec11 | [ESP-IDFv5.5-esp32s3-ILI9488-480_320-xpt2046-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-ILI9488-480_320-xpt2046-ec11.zip) |
| esp32s3-ILI9341-320_240-xpt2046-ec11 | [ESP-IDFv5.5-esp32s3-ILI9341-320_240-xpt2046-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-ILI9341-320_240-xpt2046-ec11.zip) |
| JC8048W550 | [ESP-IDFv5.5-jc8048w550.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-jc8048w550.zip) |
| SenseCAP Indicator | [ESP-IDFv5.5-esp32s3-sensecap-indicator.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-sensecap-indicator.zip) |
| 立创实战派 ESP32-S3 | [ESP-IDFv5.5-esp32s3-JLC-SZP.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-JLC-SZP.zip) |
| esp32s3-retro-go | [ESP-IDFv5.5-esp32s3-retro-go.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-retro-go.zip) |
| esp32c3-st7789-320_240-ec11 | [ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip) |
| Windows 桌面模拟器 | [desktop-win-x86_64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-win-x86_64.zip) |
| Linux 上位机（x86_64） | [desktop-linux-x86_64.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-x86_64.tar.gz) |
| Linux 上位机（arm64） | [desktop-linux-arm64.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-arm64.tar.gz) |

---

## CYD 2432S028R

![CYD 2432S028R](screenshots/boards/cyd_2432s028r.jpg)

*"Cheap Yellow Display"（黄色 PCB 的 2.8" 开发板），本项目的参考板型。* 图：[Random Nerd Tutorials](https://randomnerdtutorials.com/cheap-yellow-display-esp32-2432s028r/)

逻辑分辨率 **320×240 横屏**。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ILI9341，SPI2 @ 40MHz，DMA 双缓冲（2 × 40 行）
- 触摸：XPT2046 电阻屏，**独立 SPI3 总线**（实测与 LCD 共享总线时 MISO 无应答）；出厂触摸校准已内置
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

![CYD 2432S028R-PLUS](screenshots/boards/cyd_2432s028r_plus.png)

*CYD 的 ST7789 变种（ESP32-WROOM-32E 模组）。同一板族、同一引脚——只有显示驱动 IC 和复位线不同。*

逻辑分辨率 **320×240 横屏**。显示与触摸接线与上面的 CYD 2432S028R **完全相同**（LCD 走 SPI2：SCLK 14 / MOSI 13 / MISO 12 / CS 15 / DC 2 / BL 21；XPT2046 触摸走独立 SPI3：SCLK 25 / MOSI 32 / MISO 39 / CS 33 / IRQ 36；BOOT 键 GPIO0 息屏/唤醒）。差异均由固件处理：

- 显示：**ST7789**（替代 ILI9341），按厂商文档要求 BGR 色序初始化；SPI2 @ 40MHz，DMA 双缓冲
- **无 LCD 复位脚**（`TFT_RST = -1`）——靠初始化序列内的软件复位
- 触摸控制器、校准流程与出厂默认值与 CYD 共用（随时可用 `caltouch` 重校）

**可选 EC11 旋转编码器**——本板编码器走 **CN3 排针：A=GPIO23（MOSI）/ B=GPIO19（MISO）/ SW=GPIO18（SCK）**，公共端接 GND。三脚均有内部上拉，裸编码器直插即可，无需外接电阻。注意这三个脚与板载 SD 卡槽共用，不能同时插 SD 卡使用。

个别单元画面颠倒时，在 **设置 → 显示 → 180° 旋转** 切换；颜色反色时在同一页切换反色选项——不用改接线也不用重新编译。

## E32R35T

![E32R35T](screenshots/boards/e32r35t.png)

*ESP32-32E 3.5" 显示模组（[lcdwiki 资料页](https://www.lcdwiki.com/zh/3.5inch_ESP32-32E_Display)，触摸版 SKU：E32R35T）。* 图：lcdwiki

逻辑分辨率 **480×320 横屏**。

- 主控：ESP32-WROOM-32E（双核 240MHz），4MB QIO Flash
- 显示：ST7796U，SPI2 @ 40MHz；**与触摸屏共用 SPI 总线**（厂商设计）；无独立 RST（与 ESP32 EN 共用，驱动走软件复位）
- 触摸：XPT2046 电阻屏，共用 SPI2；出厂触摸校准已内置（提取自真机，免校准；个体差异可用串口 CLI `caltouch` 重校）
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

## esp32s3-st7789-320_240-ec11

![esp32s3-st7789-320_240-ec11 Fritzing 参考接线](screenshots/boards/ec11_knob_minimal_breadboard.zh.png)

这是一个可以直接用杜邦线搭起来的官方参考机型：**ESP32-S3-DevKitC-1 N16R8 + 8 针 240×320 ST7789 SPI 屏 + KY-040/EC11 旋钮模块**。它沿用贡献者在 [PR #6](https://github.com/umeiko/KlipperScreen-esp/pull/6) 和 [Issue #5](https://github.com/umeiko/KlipperScreen-esp/issues/5) 中实机验证的屏幕与旋钮引脚，只保留最小系统需要的两件外设。逻辑分辨率为 **320×240 横屏**。

可下载并修改 [Fritzing 源文件（.fzz）](hardware/ec11_knob_minimal.fzz)，也可以查看 [Fritzing 导出的 SVG](hardware/ec11_knob_minimal_breadboard.svg)。图中的 ST7789 是引脚顺序相同的通用 8 针模块；不同卖家的 PCB 外形、颜色和丝印位置可能不同。

| 模块引脚 | 接到 ESP32-S3 | 用途 |
|---|---|---|
| ST7789 GND | GND | 地 |
| ST7789 VCC | 3V3 | 屏幕供电 |
| ST7789 SCL / SCK | GPIO21 | SPI 时钟 |
| ST7789 SDA / MOSI | GPIO47 | SPI 数据输出 |
| ST7789 CS | GPIO41 | 片选 |
| ST7789 DC / RS | GPIO40 | 数据/命令选择 |
| ST7789 RST / RES | GPIO45 | 屏幕复位 |
| ST7789 BL / LED / BLK | GPIO42 | 背光，高电平点亮 |
| EC11 CLK / A | GPIO13 | 编码器 A 相 |
| EC11 DT / B | GPIO14 | 编码器 B 相 |
| EC11 SW / KEY | GPIO46 | 旋钮按下 |
| EC11 + / VCC | 3V3 | 模块供电 |
| EC11 GND | GND | 地 |
| 息屏按钮（外挂） | GPIO39 ── 按键 ── GND | 一键息屏/唤醒，内部上拉、低电平有效 |

开发板本身从 USB-C 供电。很多 SPI 屏把时钟和数据写成 `SCL`、`SDA`，这里仍然是 **SPI SCLK、MOSI**，不要接到 I2C 引脚。EC11 的旋转与按下都能导航，屏幕自动熄灭后再次操作旋钮即可唤醒；本机型没有触摸层，也不会进入触摸校准。

**息屏/唤醒按钮。** 在 GPIO39 与 GND 之间接一个轻触按键即可（固件开启内部上拉、下拉关闭：松开为高电平 1，按下接地为低电平 0，低电平有效）。按一下息屏，再按一下唤醒。开发板板载的 BOOT 键（GPIO0）功能相同——两个按钮同时生效，任意一个都能切换息屏/唤醒。

## esp32-st7735s-128_160-ec11

![1.8 寸 ST7735S 模组](screenshots/boards/ec11_knob_esp32_st7735s.png)

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

![ST7789 240×320 模组与 EC11](screenshots/boards/esp32_st7789_320_240_ec11.png)

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

![ILI9341 240×320 模组与 EC11 旋钮](screenshots/boards/esp32_ili9341_320_240_ec11.jpg)

*240×320 ILI9341 SPI 屏搭配 EC11 编码器旋钮（图为纵维立方 Kobra 2 Neo 的原厂显示屏组件）。排针一般印 GND / VCC / SCL / SDA / RES / DC / CS / BLK——这里的 `SCL`/`SDA` 是 SPI 的 SCLK 和 MOSI，不是 I2C。*

与 esp32-st7789-320_240-ec11 **同款主控（ESP32）、完全相同引脚**（全部对齐 CYD 2432S028R）的纯旋钮中屏机型：240×320 ILI9341 SPI 屏 + EC11 编码器，无触摸。逻辑分辨率 **320×240 横屏**——标准布局档，与 CYD 一致。相对 ST7789 机型只是换了一块屏，所有接线原位不动。参考项目：[kobra2neo-klipper](https://github.com/cheadrian/kobra2neo-klipper)。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ILI9341，走 esp_lcd 官方驱动（面板 BGR，默认 INVOFF 即正常颜色，无需强制反色；面板参数沿用 CYD 实测值）；SPI2 @ 40MHz，DMA 双缓冲（2×320×40）
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

*240×320 ST7796 SPI 屏搭配 EC11 编码器旋钮——esp32-ILI9341-320_240-ec11 的 ST7796 姊妹机型（暂无本机型实物图）。排针一般印 GND / VCC / SCL / SDA / RES / DC / CS / BLK——这里的 `SCL`/`SDA` 是 SPI 的 SCLK 和 MOSI，不是 I2C。*

与 esp32-ILI9341-320_240-ec11 **同款主控（ESP32）、完全相同引脚**（全部对齐 CYD 2432S028R）的纯旋钮中屏机型：240×320 ST7796 SPI 屏 + EC11 编码器，无触摸。逻辑分辨率 **320×240 横屏**——标准布局档，与 CYD 一致。相对 ILI9341 机型只是换了显示控制器，所有接线原位不动。

- 主控：ESP32（双核 240MHz，520KB SRAM），4MB QIO Flash
- 显示：ST7796，走 esp_lcd 官方驱动（面板 BGR，默认 INVOFF 即正常颜色；驱动、颜色格式与反色语义复用 E32R35T / esp32s3-st7796-480_320-xpt2046-ec11 的已验证实现）；SPI2 @ 40MHz，DMA 双缓冲（2×320×40）
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

不同卖家的 ST7796 模组在 320×240 窗口下有差异：画面镜像/颠倒或边缘出现彩边/偏移时，改 `src/bsp/esp32/bsp_esp32_st7796_ec11.c` 顶部的 `LCD_MIRROR_X/Y` 与 `LCD_GAP_X/Y` 重新编译即可（初始值沿用 480×320 ST7796 模组的横屏语义，需实机确认）。EC11 的旋转与按下都能导航，屏幕自动熄灭后再次操作旋钮即可唤醒。

## esp32s3-st7796-480_320-xpt2046-ec11

![MKS TS35 V2.0](screenshots/boards/esp32s3_st7796_ec11.png)

*本机型的典型板子——MKS TS35 V2.0：480×320 ST7796S 屏 + XPT2046 电阻触摸，板载 EC11 旋钮。*

与 esp32s3-st7789-320_240-ec11 **同款 ESP32-S3-DevKitC-1 N16R8 底座**的 480×320 电阻屏机型：ST7796S SPI 屏与 XPT2046 触摸**共用一条 SPI 总线**，EC11 旋钮沿用不变的参考引脚。逻辑分辨率 **480×320 横屏**（与 E32R35T 同一布局档）。出厂触摸校准已内置（提取自 MKS TS35 V2.0 真机两点校准结果）；个体差异可用串口 CLI `caltouch` 强制重校，校准结果存 `touch.json`，之后开机直接加载。

| 模块引脚 | 接到 ESP32-S3 | 用途 |
|---|---|---|
| SCK | GPIO21 | SPI 时钟——屏 + 触摸共用 |
| MOSI（SDA / DIN） | GPIO47 | SPI 数据出——屏数据 + 触摸 DIN 共用 |
| MISO（DOUT） | GPIO2 | SPI 数据入——XPT2046 坐标回读 |
| TFT_CS | GPIO41 | 屏幕片选 |
| TFT_DC（RS / A0） | GPIO40 | 数据/命令选择 |
| TOUCH_CS | GPIO1 | 触摸芯片片选 |
| RST / RES | GPIO45 | 屏幕复位；可省——接 3.3V 常高或共用 MCU 复位（驱动内另有软件复位） |
| BL / LED / BLK | GPIO42 | 背光，高电平点亮（LEDC PWM） |
| TOUCH_INT（T_IRQ） | — | 悬空不接——驱动轮询不依赖中断，触摸唤醒/点击/滑动全部照常 |
| EC11 CLK / A | GPIO13 | 编码器 A 相 |
| EC11 DT / B | GPIO14 | 编码器 B 相 |
| EC11 SW / KEY | GPIO46 | 旋钮按下 |
| EC11 + / VCC | 3V3 | 模块供电 |
| EC11 GND / C | GND | A/B/SW 公共端接地 |
| 息屏按钮（外挂） | GPIO39 ── 按键 ── GND | 一键息屏/唤醒，内部上拉、低电平有效；板载 BOOT 键（GPIO0）功能相同 |

开发板从 USB-C 供电。触摸与旋钮可同时使用——触摸走指针手势，旋钮走焦点导航。显示镜像/旋转沿用 E32R35T 同款面板的实测默认值；个别单元画面颠倒时，在 **设置 → 显示 → 180° 旋转** 里切换即可，不用改接线。

这套配置正好可以直接驱动 **Makerbase MKS TS35 V2.0**——接线参考下图：

![MKS TS35 V2.0 接线参考](screenshots/boards/mks_ts35_v2_0_wiring.jpg)

## esp32s3-ILI9488-480_320-xpt2046-ec11

![MKS PI-TS35 V1.0](screenshots/boards/mks_pi_ts35.png)

*本机型的典型板子——MKS PI-TS35 V1.0：3.5" 480×320 ILI9488 SPI 屏 + XPT2046 电阻触摸（MKS PI / SKIPR Klipper 上位机板的配屏）。任何 ILI9488 + XPT2046 的 SPI 模组都同样适用。*

[esp32s3-st7796-480_320-xpt2046-ec11](#esp32s3-st7796-480_320-xpt2046-ec11) 的 ILI9488 孪生机型：**同款 ESP32-S3-DevKitC-1 N16R8 底座、同款接线**——屏与 XPT2046 共用一条 SPI 总线，EC11 旋钮沿用不变的参考引脚。逻辑分辨率 **480×320 横屏**。本面板无出厂触摸数据，**首次开机自动进入两点触摸校准**；结果存 `touch.json`，之后开机直接加载，个体差异可用串口 CLI `caltouch` 强制重校。

| 模块引脚 | 接到 ESP32-S3 | 用途 |
|---|---|---|
| SCK | GPIO21 | SPI 时钟——屏 + 触摸共用 |
| MOSI（SDA / DIN） | GPIO47 | SPI 数据出——屏数据 + 触摸 DIN 共用 |
| MISO（DOUT） | GPIO2 | SPI 数据入——XPT2046 坐标回读 |
| TFT_CS | GPIO41 | 屏幕片选 |
| TFT_DC（RS / A0） | GPIO40 | 数据/命令选择 |
| TOUCH_CS | GPIO1 | 触摸芯片片选 |
| RST / RES | GPIO45 | 屏幕复位；可省——接 3.3V 常高或共用 MCU 复位（驱动内另有软件复位） |
| BL / LED / BLK | GPIO42 | 背光，高电平点亮（LEDC PWM） |
| TOUCH_INT（T_IRQ） | — | 悬空不接——驱动轮询不依赖中断，触摸唤醒/点击/滑动全部照常 |
| EC11 CLK / A | GPIO13 | 编码器 A 相 |
| EC11 DT / B | GPIO14 | 编码器 B 相 |
| EC11 SW / KEY | GPIO46 | 旋钮按下 |
| EC11 + / VCC | 3V3 | 模块供电 |
| EC11 GND / C | GND | A/B/SW 公共端接地 |
| 息屏按钮（外挂） | GPIO39 ── 按键 ── GND | 一键息屏/唤醒，内部上拉、低电平有效；板载 BOOT 键（GPIO0）功能相同 |

固件已处理的 ILI9488 特性：

- ILI9488 走 4 线 SPI 时只收 **18-bit RGB666 像素**（COLMOD=0x66）。面板驱动（[atanisoft/esp_lcd_ili9488](https://components.espressif.com/components/atanisoft/esp_lcd_ili9488)）内部从 RGB565 转换，像素流量为 3 字节/像素（同时钟下比 ST7796 多约 50%）
- 个别个体画面呈底片或方向不对时，用**设置 → 显示 → 反色 / 180° 旋转 / 水平镜像**修正即可，不用重刷固件
- PI-TS35 原生插 MKS PI 上位机板的 2×20 排母；接到 DevKit 时按 [MKS-TFT-Hardware](https://github.com/makerbase-mks/MKS-TFT-Hardware) 原理图把排母上的 SCK/MOSI/MISO/CS/DC/RST/BL/T_CS 网络对应到上面的引脚表

## esp32s3-ILI9341-320_240-xpt2046-ec11

**ESP32-S3-DevKitC-1 N16R8 底座**的 320×240 电阻触摸机型：ILI9341 SPI 屏与 XPT2046 触摸**共用一条 SPI 总线**，外挂 EC11 旋钮。逻辑分辨率 **320×240 横屏**。无出厂触摸数据，**首次开机自动进入两点触摸校准**；结果存 `touch.json`，之后开机直接加载，个体差异可用串口 CLI `caltouch` 强制重校。

| 模块引脚 | 接到 ESP32-S3 | 用途 |
|---|---|---|
| SCK | GPIO21 | SPI 时钟——屏 + 触摸共用 |
| MOSI（SDA / DIN） | GPIO47 | SPI 数据出——屏数据 + 触摸 DIN 共用 |
| MISO（DOUT） | GPIO2 | SPI 数据入——XPT2046 坐标回读 |
| TFT_CS | GPIO41 | 屏幕片选 |
| TFT_DC（RS / A0） | GPIO40 | 数据/命令选择 |
| TOUCH_CS | GPIO1 | 触摸芯片片选 |
| RST / RES | GPIO45 | 屏幕复位；可省——接 3.3V 常高或共用 MCU 复位（驱动内另有软件复位） |
| BL / LED / BLK | GPIO42 | 背光，高电平点亮（LEDC PWM） |
| TOUCH_INT（T_IRQ） | — | 悬空不接——驱动轮询不依赖中断，触摸唤醒/点击/滑动全部照常 |
| EC11 CLK / A | GPIO13 | 编码器 A 相 |
| EC11 DT / B | GPIO14 | 编码器 B 相 |
| EC11 SW / KEY | GPIO46 | 旋钮按下 |
| EC11 + / VCC | 3V3 | 模块供电 |
| EC11 GND / C | GND | A/B/SW 公共端接地 |
| 息屏按钮（外挂） | GPIO39 ── 按键 ── GND | 一键息屏/唤醒，内部上拉、低电平有效；板载 BOOT 键（GPIO0）功能相同 |

开发板从 USB-C 供电。ILI9341 沿用 CYD 上实测的面板参数（BGR 色彩顺序、不开反色）；个别个体画面呈底片或方向不对时，用**设置 → 显示 → 反色 / 180° 旋转 / 水平镜像**修正即可，不用重刷固件。

## JC8048W550

![JC8048W550](screenshots/boards/jc8048w550.png)

*Guition 5" 电容屏模组（ESP32-S3）。* 图：[openHASP 硬件页](https://www.openhasp.com/0.7.0/hardware/guition/jc8048w550/)

逻辑分辨率 **800×480**。RGB 并口屏的撕裂/抽动排障全过程见[开发笔记](jc8048w550-rgb-display-guide.md)。

- 主控：ESP32-S3，16MB Flash + PSRAM（双帧缓冲 2×768KB 放在 PSRAM）
- 显示：ST7262 RGB 并口（RGB565），PCLK **必须 16MHz**；自研 rgb44 驱动（IDF 4.4 传输模型 + vsync 换页）
- 触摸：GT911 电容屏，I2C0，轮询无 INT，无需校准
- 背光：GPIO2，高电平点亮，80–100% 区间有硬件曲线补偿

| 功能 | GPIO |
|---|---|
| LCD DE / VSYNC / HSYNC / PCLK | 40 / 41 / 39 / 42 |
| LCD B0..B4 | 8, 3, 46, 9, 1 |
| LCD G0..G5 | 5, 6, 7, 15, 16, 4 |
| LCD R0..R4 | 45, 48, 47, 21, 14 |
| LCD 背光 | 2 |
| 触摸 SDA / SCL / RST | 19 / 20 / 38 |
| BOOT 按键（息屏/唤醒） | 0 |

## SenseCAP Indicator

*Seeed SenseCAP Indicator（D1/D1S/D1L/D1Pro 显示部分硬件相同）：4" 480×480 方形电容屏，ESP32-S3 + RP2040 双主控设计。硬件资料：[Seeed Wiki](https://wiki.seeedstudio.com/cn/SenseCAP_Indicator_ESP32_4_inch_Touch_Screen/)。*

![SenseCAP Indicator 运行 KlipperScreen-esp](screenshots/boards/sensecap_indicator.gif)

逻辑分辨率 **480×480**。

- 主控：ESP32-S3-WROOM-1-N8R8，8MB QIO Flash + 8MB Octal PSRAM @ 80MHz
- 显示：ST7701S RGB 并口（RGB565），PCLK 12MHz（约 42fps）；与 JC8048W550 同款自研 **rgb44** 驱动（IDF 4.4 传输模型 + vsync 换页，LVGL DIRECT 双帧缓冲 2×450KB 放 PSRAM）。面板初始化走位 bang 3 线 9-bit SPI——SCK/MOSI 是真 GPIO，CS/RST 挂在 TCA9535 扩展器上——初始化序列照抄官方 Seeed SDK
- IO 扩展器：TCA9535，I2C0（先探 0x20，后期批次回退 0x39）；还挂着 RP2040 的复位线（拉高释放——RP2040 跑出厂固件，与本项目无关）
- 触摸：FT5x06 电容屏，与扩展器共用 I2C0，TP_RST 也在扩展器上（建驱动前先手动复位），无需校准
- 背光：GPIO45，LEDC PWM，高电平点亮（strapping 脚，官方 SDK 同样用法）
- 息屏/唤醒：侧键（GPIO38，低电平有效）
- 烧录请用 **"USB-SERIAL"（CH340）那个 Type-C 口**——ESP32-S3 侧，控制台 UART0 @ 115200。另一个口是 RP2040 的原生 USB，**不要**用它

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD HSYNC / VSYNC / DE / PCLK | 16 / 17 / 18 / 21 | RGB 并口 |
| LCD B0..B4 | 15, 14, 13, 12, 11 | |
| LCD G0..G5 | 10, 9, 8, 7, 6, 5 | |
| LCD R0..R4 | 4, 3, 2, 1, 0 | |
| LCD 背光 | 45 | LEDC PWM，高电平点亮 |
| 初始化 SPI SCK / MOSI | 41 / 48 | 位 bang 3 线 9-bit |
| LCD CS / LCD RST | TCA9535 P4 / P5 | IO 扩展器，I2C 0x20（部分批次 0x39） |
| TP RST / RP2040 RST | TCA9535 P7 / P8 | |
| 触摸 + 扩展器 SDA / SCL | 39 / 40 | I2C0 @ 100kHz |
| 侧键（息屏/唤醒） | 38 | 低电平有效，内部上拉 |

## 立创实战派 ESP32-S3

*立创"实战派" ESP32-S3 开发板，板载 2.0" 电容触摸屏。*

![立创实战派 ESP32-S3](screenshots/boards/esp32s3_jlc_szp.jpg)

逻辑分辨率 **320×240 横屏**。

- 主控：ESP32-S3-WROOM-1-N16R8，16MB QIO Flash + 8MB Octal PSRAM @ 80MHz
- 显示：ST7789（原生 240×320），SPI3 @ 80MHz **mode 3**，DMA 双缓冲（2 × 40 行）；BGR，横屏 MADCTL=0x68（MX|MV|BGR），**必须 INVON**（INVOFF 全屏反色；`bsp_disp_set_invert` 开关语义已取反）
- **LCD CS 不是 GPIO**：在 PCA9557（I2C 0x19）P0 上，且面板要求**每笔 SPI 交易都有 CS 下降沿**（CS 常低/常高均全黑）。esp_lcd 无法经 I2C 扩展器逐笔翻 CS，故本板 BSP 不用 esp_lcd 面板驱动，直接 SPI master + 手动控 CS/DC（复刻实测可亮的 [Arduino 参考工程](https://github.com/umeiko/jlc-shizhanpai-esp32s3-arduino-lvgl) 及其 TFT_eSPI fork 的 CS 挂钩方案）；无 RST 脚，初始化必须先 SWRESET(0x01)+150ms
- PCA9557 其余引脚按 Arduino 工程实测状态：P1 保持输入，P2=0（"摄像头电源"开启，疑似与 TFT 逻辑供电共用，P2=1 时背光亮但整屏黑）
- 触摸：FT6336 电容屏，与 PCA9557 同一条 I2C0 总线，轮询无 INT/RST，无需校准
- 背光：GPIO42，LEDC PWM 10bit/5kHz，**低电平点亮**（`output_invert` 方式驱动）
- 息屏/唤醒：板载用户键（GPIO0）

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD MOSI / SCLK / DC | 40 / 41 / 39 | SPI3 @ mode 3，无 MISO |
| LCD CS | PCA9557 P0（I2C 0x19） | 逐笔交易翻转（空闲高）；P1 保持输入，P2=0 摄像头/TFT 电源 |
| LCD RST | — | 未连接，初始化必须 SWRESET |
| LCD 背光 | 42 | LEDC PWM，低电平点亮 |
| 触摸 / PCA9557 SDA / SCL | 1 / 2 | I2C0 @ 100kHz |
| 用户键（息屏/唤醒） | 0 | 低电平有效，内部上拉 |

## esp32s3-retro-go

*Chaeng 的 retro-go ESP32-S3 掌机主板（T320-S3）：3.2" IPS 屏 + 全套掌机按键，无触摸。引脚依据上游固件 [retro-go_chaeng](https://github.com/Chaeng3/retro-go_chaeng)（`components/retro-go/targets/t320-s3`）核对；硬件开源页：[oshwhub.com/chaeng/project_jofcnupz](https://oshwhub.com/chaeng/project_jofcnupz)。*

![retro-go ESP32-S3 掌机](screenshots/boards/esp32s3_retro_go.jpg)

逻辑分辨率 **320×240 横屏**。

- 主控：ESP32-S3（双核 240MHz），16MB QIO Flash + 8MB Octal PSRAM @ 80MHz
- 显示：T320B7-C12-16 3.2" IPS（ST7789，原生 240×320），SPI2 @ 40MHz，DMA 双缓冲（2 × 40 行）；RGB 颜色序，横屏 MADCTL（MV|MY），**必须 INVON**（`bsp_disp_set_invert` 开关语义已取反）
- 输入：无触摸层——导航全部走板载 GPIO 按键，经 6 键语义层（上/下/左/右/确定/返回）驱动。确定键并联 **A、START、SELECT** 三键（任一个都是确认），返回键为 **B**；全部内部上拉、低电平有效。**KEY_MENU（GPIO18）、KEY_OPTION（GPIO8）、KEY_BOOT（GPIO0）本固件保留未映射**——GPIO0 有意不再兼任息屏按钮，避免输入职责混淆
- 背光：GPIO39，LEDC PWM 8bit/5kHz，高电平点亮
- 板上的 SD 卡槽、I2S 喇叭、麦克风、电池电量 ADC、WS2812 状态灯**本固件均未使用**

| 功能 | GPIO | 本固件是否使用 |
|---|---|---|
| TFT MOSI / CLK | 12 / 48 | 是——SPI2 @ 40MHz |
| TFT CS / DC / RST | 14 / 47 / 3 | 是 |
| TFT 背光 | 39 | 是——高电平点亮 |
| UP / DOWN / LEFT / RIGHT | 7 / 20 / 19 / 6 | 是——焦点导航 |
| A / START / SELECT | 15 / 17 / 16 | 是——三键同为确定 |
| B | 5 | 是——返回 |
| MENU / OPTION / BOOT | 18 / 8 / 0 | 否——保留未映射 |
| SD CMD(MOSI) / CLK / DATA(MISO) / CD(CS) | 11 / 13 / 9 / 10 | 否（SDSPI，SPI3） |
| 喇叭 DOUT / BCLK / LRCK | 40 / 41 / 42 | 否（I2S） |
| MIC WS / SCK / DIN | 1 / 2 / 21 | 否 |
| 电池电压 ADC | 4 | 否（ADC1_CH3） |
| STATUS_LED（WS2812） | 38 | 否 |

方向键移动焦点，确定激活聚焦控件，返回关闭弹层或退回上一面板——与桌面端键盘行为一致。屏幕超时熄灭后，第一次按键只负责唤醒。

## esp32c3-st7789-320_240-ec11

![ESP32-C3、ST7789 屏幕与 EC11 编码器实机](screenshots/boards/esp32c3_st7789_320_240_ec11.jpg)

为袖珍 **ESP32-C3** 板做的纯旋钮机型：240×320 ST7789 SPI 屏 + EC11 编码器，无触摸。逻辑分辨率 **320×240 横屏**——标准布局档。一个固件镜像同时覆盖两块目标板（接线完全相同）：

- **合宙 CORE ESP32-C3**——选 **USB 直连版（非 CH340）**；烧录和串口 CLI 都直接走 Type-C 口（USB-Serial-JTAG），Windows 8 以上免驱
- **ESP32-C3 Super Mini**——排针只引出 GPIO0–10/20/21，本方案每根线都落在 GPIO0–10 内

ESP32-C3 与其它目标芯片有三点差异，固件已全部处理：**单核**（LVGL 任务不绑核运行）、**无 PCNT 外设**（EC11 改用 2ms 定时轮询软件正交解码——不用 GPIO 中断，避免悬浮/噪声输入形成中断风暴）、合宙板 flash 为**两线 DIO 模式**（固件按 `FLASHMODE_DIO` 构建，Super Mini 同样兼容）。注意：C3 只有 400KB SRAM 且无 PSRAM 可选，堆空间比 ESP32 板紧张——Klipper/Moonraker 是主要使用场景。

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

## Linux 上位机

![红米4 上的 Ubuntu 运行 KlipperScreen-esp](screenshots/boards/linux_redmi4.jpg)

*不用 ESP32——直接在打印机的 Linux 上位机上跑同一套 UI（图：退役红米4 手机，aarch64 Ubuntu 24.04，weston kiosk 后端）。手机/平板刷 Linux chroot/proot 也一样能用。*

桌面构建面向 Debian/Ubuntu 系 Linux 上位机（glibc ≥ 2.35，x86_64 与 arm64），是 KlipperScreen 的轻量替代——预编译二进制静态链接 SDL2/cJSON，上位机不需要编译任何东西。

### 一行安装

```bash
curl -fsSL https://raw.githubusercontent.com/umeiko/KlipperScreen-esp/main/scripts/linux/install.sh | bash
```

安装脚本自动识别架构、从最新 release 下载匹配的预编译包，然后询问安装形态：

- **独占显示服务（默认）**——写入 `KlipperScreen-esp.service` systemd 单元，开机自启全屏显示，图形后端可选 Wayland（weston kiosk shell）或 X11（裸 xinit），所需软件包（`weston` 或 `xinit`）自动安装。检测到 `KlipperScreen.service` 时会询问是否停用它，避免两个程序抢屏幕
- **桌面 App**——只装二进制 + 应用菜单里的启动快捷方式

文件装在 `~/.local/share/KlipperScreen-esp/`，配置存放在 `~/.config/KlipperScreen-esp/`。

### 手动安装

从 [Releases](https://github.com/umeiko/KlipperScreen-esp/releases/latest) 下载 `desktop-linux-x86_64.tar.gz` 或 `desktop-linux-arm64.tar.gz`，解压后运行 `./install.sh`。脚本化部署可用非交互环境变量：

```bash
SERVICE=n ./install.sh                  # 只装桌面 App
KR_BACKEND=x11 ./install.sh             # 服务模式，强制 X11
KR_BACKEND=wayland KR_START=0 ./install.sh
```

卸载用包内附带的 `./uninstall.sh`。

### 说明

- 720p 及以上分辨率自动切换到加大字号/图标档；高分辨率 Linux 上位机跳过开机动画，启动更快。
- 触摸走内核 evdev/libinput，weston 和 xinit 后端都会透传。
- Klipper 上位机（存在 `~/printer_data`）下，配置放在 `~/printer_data/config/KlipperScreen-esp/`，fluidd/mainsail 可直接查看编辑；服务日志写到 `~/printer_data/logs/KlipperScreen-esp.log`，网页端可直接下载。每次应用写配置会同步刷新 `.bak` 检查点——主文件被手改损坏时自动还原到最近一次已知良好的内容，不会崩。首次开机会把打印机槽 1 预填为本机 Moonraker（`127.0.0.1`，名称沿用登录用户名）。
- 想从源码构建见 [从源码构建](building.md)。
