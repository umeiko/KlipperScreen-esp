# Supported boards

Firmware boards are grouped by chip family, desktop/mobile clients by platform. Wiring, pinout tables and per-board notes live on the sub-pages:

- **[ESP32 boards](boards/esp32.md)** — CYD 2432S028R / 2432S028R-PLUS, E32R35T, and the rotary-only ST7735S / ST7789 / ILI9341 / ST7796 builds
- **[ESP32-S3 boards](boards/esp32s3.md)** — the EC11 reference build, MKS TS35 / PI-TS35 twins, JC8048W550, SenseCAP Indicator, JLC SZP, retro-go
- **[ESP32-C3 boards](boards/esp32c3.md)** — LuatOS CORE / Super Mini rotary build
- **[Linux client](boards/linux.md)** — run the same UI directly on the printer's Linux host
- **[Windows / macOS client](boards/desktop.md)** — desktop app
- **[Android client](boards/android.md)** — phone / tablet app

## Board overview

| Board | Build target | Display | Touch | MCU / Flash | Status |
|---|---|---|---|---|---|
| [CYD 2432S028R](boards/esp32.md#cyd-2432s028r) | `cyd_2432s028r` | 2.8" 240×320 ILI9341 SPI | XPT2046 resistive | ESP32 / 4MB | ✅ Stable |
| [CYD 2432S028R-PLUS](boards/esp32.md#cyd-2432s028r-plus) | `cyd_2432s028r_plus` | 2.8" 240×320 ST7789 SPI | XPT2046 resistive | ESP32-WROOM-32E / 4MB | 🆕 New, CYD pinout |
| [E32R35T](boards/esp32.md#e32r35t) | `e32r35t` | 3.5" 320×480 ST7796U SPI | XPT2046 resistive | ESP32-32E / 4MB | ✅ Stable |
| [esp32s3-st7789-320_240-ec11](boards/esp32s3.md#esp32s3-st7789-320_240-ec11) | `esp32s3-st7789-320_240-ec11` | 240×320 ST7789 SPI | None, rotary only | ESP32-S3 N16R8 / 16MB | ✅ Official reference, contributor tested |
| [esp32-st7735s-128_160-ec11](boards/esp32.md#esp32-st7735s-128_160-ec11) | `esp32-st7735s-128_160-ec11` | 1.8" 128×160 ST7735S SPI | None, rotary only | ESP32 / 4MB | 🆕 New, CYD-compatible pinout |
| [esp32-st7789-320_240-ec11](boards/esp32.md#esp32-st7789-320_240-ec11) | `esp32-st7789-320_240-ec11` | 240×320 ST7789 SPI | None, rotary only | ESP32 / 4MB | 🆕 New, CYD-compatible pinout |
| [esp32-ILI9341-320_240-ec11](boards/esp32.md#esp32-ili9341-320_240-ec11) | `esp32-ILI9341-320_240-ec11` | 240×320 ILI9341 SPI | None, rotary only | ESP32 / 4MB | 🆕 New, CYD-compatible pinout |
| [esp32-ST7796-320_240-ec11](boards/esp32.md#esp32-st7796-320_240-ec11) | `esp32-ST7796-320_240-ec11` | 240×320 ST7796 SPI | None, rotary only | ESP32 / 4MB | 🆕 New, CYD-compatible pinout |
| [esp32s3-st7796-480_320-xpt2046-ec11](boards/esp32s3.md#esp32s3-st7796-480_320-xpt2046-ec11) | `esp32s3-st7796-480_320-xpt2046-ec11` | 480×320 ST7796S SPI | XPT2046 resistive (shared bus) + EC11 | ESP32-S3 N16R8 / 16MB | 🆕 New |
| [esp32s3-ILI9488-480_320-xpt2046-ec11](boards/esp32s3.md#esp32s3-ili9488-480_320-xpt2046-ec11) | `esp32s3-ILI9488-480_320-xpt2046-ec11` | 3.5" 480×320 ILI9488 SPI | XPT2046 resistive (shared bus) + EC11 | ESP32-S3 N16R8 / 16MB | 🆕 New, MKS PI-TS35 |
| [esp32s3-ILI9341-320_240-xpt2046-ec11](boards/esp32s3.md#esp32s3-ili9341-320_240-xpt2046-ec11) | `esp32s3-ILI9341-320_240-xpt2046-ec11` | 320×240 ILI9341 SPI | XPT2046 resistive (shared bus) + EC11 | ESP32-S3 N16R8 / 16MB | 🆕 New |
| [JC8048W550](boards/esp32s3.md#jc8048w550) | `jc8048w550` | 5" 800×480 ST7262 RGB parallel | GT911 capacitive | ESP32-S3 / 16MB | ✅ Stable |
| [SenseCAP Indicator](boards/esp32s3.md#sensecap-indicator) | `esp32s3-sensecap-indicator` | 4" 480×480 ST7701S RGB parallel | FT5x06 capacitive | ESP32-S3 N8R8 / 8MB | 🆕 New |
| [JLC SZP ESP32-S3](boards/esp32s3.md#jlc-szp-esp32-s3) | `esp32s3-JLC-SZP` | 2.0" 240×320 ST7789 SPI | FT6336 capacitive | ESP32-S3 N16R8 / 16MB | ✅ Verified |
| [esp32s3-retro-go](boards/esp32s3.md#esp32s3-retro-go) | `esp32s3-retro-go` | 3.2" 240×320 ST7789 SPI | None, GPIO buttons | ESP32-S3 / 16MB | 🆕 New |
| [esp32c3-st7789-320_240-ec11](boards/esp32c3.md#esp32c3-st7789-320_240-ec11) | `esp32c3-st7789-320_240-ec11` | 240×320 ST7789 SPI | None, rotary only | ESP32-C3 / 4MB | 🆕 New, LuatOS CORE & Super Mini |

## Download (latest stable)

Flash packages are named `ESP-IDFv5.5-<board>.zip` (asset names carry no version, so the links below always point to the latest stable release). Please report problems in [Issues](https://github.com/umeiko/KlipperScreen-esp/issues).

| Board | Flash package (latest stable) |
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
| JLC SZP ESP32-S3 | [ESP-IDFv5.5-esp32s3-JLC-SZP.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-JLC-SZP.zip) |
| esp32s3-retro-go | [ESP-IDFv5.5-esp32s3-retro-go.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32s3-retro-go.zip) |
| esp32c3-st7789-320_240-ec11 | [ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip) |
| Windows desktop simulator | [desktop-win-x86_64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-win-x86_64.zip) |
| macOS desktop | [desktop-macos-arm64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-macos-arm64.zip) |
| Linux host (x86_64) | [desktop-linux-x86_64.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-x86_64.tar.gz) |
| Linux host (arm64) | [desktop-linux-arm64.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-arm64.tar.gz) |
| Linux host (armhf) | [desktop-linux-armhf.tar.gz](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-linux-armhf.tar.gz) |
| Android phone/tablet | [KlipperScreen-esp-android.apk](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/KlipperScreen-esp-android.apk) |
