# 支持的板子

固件板型按芯片分族，桌面/移动客户端按平台分。接线、引脚表与各板注意事项都在子页面：

- **[ESP32 板型](boards/esp32.md)** —— CYD 2432S028R / 2432S028R-PLUS、E32R35T，以及 ST7735S / ST7789 / ILI9341 / ST7796 纯旋钮机型
- **[ESP32-S3 板型](boards/esp32s3.md)** —— EC11 参考机型、MKS TS35 / PI-TS35 孪生机型、JC8048W550、SenseCAP Indicator、立创实战派、retro-go
- **[ESP32-C3 板型](boards/esp32c3.md)** —— 合宙 CORE / Super Mini 纯旋钮机型
- **[Linux 客户端](boards/linux.md)** —— 直接在打印机的 Linux 上位机上跑同一套 UI
- **[Windows / macOS 客户端](boards/desktop.md)** —— PC / Mac 桌面应用
- **[Android 客户端](boards/android.md)** —— 手机 / 平板应用

## 板型总览

| 板型 | 构建目标 | 屏幕 | 触摸 | 主控 / Flash | 状态 |
|---|---|---|---|---|---|
| [CYD 2432S028R](boards/esp32.md#cyd-2432s028r) | `cyd_2432s028r` | 2.8" 240×320 ILI9341 SPI | XPT2046 电阻 | ESP32 / 4MB | ✅ 稳定 |
| [CYD 2432S028R-PLUS](boards/esp32.md#cyd-2432s028r-plus) | `cyd_2432s028r_plus` | 2.8" 240×320 ST7789 SPI | XPT2046 电阻 | ESP32-WROOM-32E / 4MB | 🆕 新机型，CYD 引脚 |
| [E32R35T](boards/esp32.md#e32r35t) | `e32r35t` | 3.5" 320×480 ST7796U SPI | XPT2046 电阻 | ESP32-32E / 4MB | ✅ 稳定 |
| [esp32s3-st7789-320_240-ec11](boards/esp32s3.md#esp32s3-st7789-320_240-ec11) | `esp32s3-st7789-320_240-ec11` | 240×320 ST7789 SPI | 无，纯旋钮 | ESP32-S3 N16R8 / 16MB | ✅ 官方参考，贡献者实机验证 |
| [esp32-st7735s-128_160-ec11](boards/esp32.md#esp32-st7735s-128_160-ec11) | `esp32-st7735s-128_160-ec11` | 1.8" 128×160 ST7735S SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32-st7789-320_240-ec11](boards/esp32.md#esp32-st7789-320_240-ec11) | `esp32-st7789-320_240-ec11` | 240×320 ST7789 SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32-ILI9341-320_240-ec11](boards/esp32.md#esp32-ili9341-320_240-ec11) | `esp32-ILI9341-320_240-ec11` | 240×320 ILI9341 SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32-ST7796-320_240-ec11](boards/esp32.md#esp32-st7796-320_240-ec11) | `esp32-ST7796-320_240-ec11` | 240×320 ST7796 SPI | 无，纯旋钮 | ESP32 / 4MB | 🆕 新机型，引脚兼容 CYD |
| [esp32s3-st7796-480_320-xpt2046-ec11](boards/esp32s3.md#esp32s3-st7796-480_320-xpt2046-ec11) | `esp32s3-st7796-480_320-xpt2046-ec11` | 480×320 ST7796S SPI | XPT2046 电阻（共总线）+ EC11 | ESP32-S3 N16R8 / 16MB | 🆕 新机型 |
| [esp32s3-ILI9488-480_320-xpt2046-ec11](boards/esp32s3.md#esp32s3-ili9488-480_320-xpt2046-ec11) | `esp32s3-ILI9488-480_320-xpt2046-ec11` | 3.5" 480×320 ILI9488 SPI | XPT2046 电阻（共总线）+ EC11 | ESP32-S3 N16R8 / 16MB | 🆕 新机型，MKS PI-TS35 |
| [esp32s3-ILI9341-320_240-xpt2046-ec11](boards/esp32s3.md#esp32s3-ili9341-320_240-xpt2046-ec11) | `esp32s3-ILI9341-320_240-xpt2046-ec11` | 320×240 ILI9341 SPI | XPT2046 电阻（共总线）+ EC11 | ESP32-S3 N16R8 / 16MB | 🆕 新机型 |
| [JC8048W550](boards/esp32s3.md#jc8048w550) | `jc8048w550` | 5" 800×480 ST7262 RGB 并口 | GT911 电容 | ESP32-S3 / 16MB | ✅ 稳定 |
| [SenseCAP Indicator](boards/esp32s3.md#sensecap-indicator) | `esp32s3-sensecap-indicator` | 4" 480×480 ST7701S RGB 并口 | FT5x06 电容 | ESP32-S3 N8R8 / 8MB | 🆕 新机型 |
| [立创实战派（JLC SZP）ESP32-S3](boards/esp32s3.md#jlc-szp-esp32-s3) | `esp32s3-JLC-SZP` | 2.0" 240×320 ST7789 SPI | FT6336 电容 | ESP32-S3 N16R8 / 16MB | ✅ 已实机验证 |
| [esp32s3-retro-go](boards/esp32s3.md#esp32s3-retro-go) | `esp32s3-retro-go` | 3.2" 240×320 ST7789 SPI | 无，GPIO 按键 | ESP32-S3 / 16MB | 🆕 新机型 |
| [esp32c3-st7789-320_240-ec11](boards/esp32c3.md#esp32c3-st7789-320_240-ec11) | `esp32c3-st7789-320_240-ec11` | 240×320 ST7789 SPI | 无，纯旋钮 | ESP32-C3 / 4MB | 🆕 新机型，合宙 CORE/Super Mini 通用 |

## 下载（最新正式版）

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
| macOS 桌面端 | [desktop-macos-arm64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-macos-arm64.zip) |
| Linux 上位机（x86_64） | [desktop-linux-x86_64.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-x86_64.tar.gz) |
| Linux 上位机（arm64） | [desktop-linux-arm64.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-arm64.tar.gz) |
| Linux 上位机（armhf） | [desktop-linux-armhf.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-armhf.tar.gz) |
| Android 手机/平板 | [KlipperScreen-esp-android.apk](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/KlipperScreen-esp-android.apk) |
