# ESP32-C3 boards

Boards built on the ESP32-C3 (single-core RISC-V 160MHz, 400KB SRAM, no PSRAM; flash in two-wire DIO mode).

## esp32c3-st7789-320_240-ec11

**Flash package**: [ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/ESP-IDFv5.5-esp32c3-st7789-320_240-ec11.zip)

![ESP32-C3 with ST7789 display and EC11 encoder](../screenshots/boards/esp32c3_st7789_320_240_ec11.jpg)

A rotary-only build for the tiny **ESP32-C3** boards: a 240×320 ST7789 SPI display plus an EC11 encoder, no touch. Logical resolution **320×240 landscape** — the standard layout class. One firmware image covers both target boards (they share the same wiring):

- **LuatOS CORE ESP32-C3** — use the **USB-direct version (non-CH340)**; flashing and the serial CLI both go straight through the Type-C port (USB-Serial-JTAG), no driver needed on Windows 8+
- **ESP32-C3 Super Mini** — only GPIO0–10/20/21 are pinned out, and every wire of this build lands inside GPIO0–10

ESP32-C3 differs from the other targets in three ways, all handled by the firmware: it is **single-core** (the LVGL task runs unpinned), it has **no PCNT peripheral** (the EC11 uses a 2 ms timer-polling software quadrature decoder instead), and the LuatOS board wires its flash in **two-wire DIO mode** (the firmware is built with `FLASHMODE_DIO`, which also works on the Super Mini). Heads-up: the C3 has 400KB SRAM and no PSRAM option, so free heap is tighter than on the ESP32 boards — Klipper/Moonraker is the primary use case.

- MCU: ESP32-C3 (single-core RISC-V 160MHz, 400KB SRAM), 4MB flash @ 80MHz **DIO**
- Display: ST7789 via the esp_lcd driver (normal colour with the default INVOFF); SPI2 @ 40MHz, DMA double buffering (2×320×40)
- Input: EC11 only (timer-polling software quadrature); no touch layer, never enters touch calibration
- Backlight: GPIO4, LEDC PWM 8bit/5kHz, active high
- Screen off / wake: on-board BOOT key (GPIO9)

| Module pin | ESP32-C3 pin | Purpose |
|---|---|---|
| ST7789 VCC | 3V3 | Display power |
| ST7789 GND | GND | Ground |
| ST7789 SCL / SCK | GPIO2 | SPI clock |
| ST7789 SDA / MOSI | GPIO3 | SPI data out |
| ST7789 CS | GPIO7 | Chip select |
| ST7789 DC / RS | GPIO6 | Data/command select |
| ST7789 RST / RES | GPIO5 | Display reset |
| ST7789 BL / LED / BLK | GPIO4 | Backlight, active high |
| EC11 CLK / A | GPIO0 | Internal pull-up |
| EC11 DT / B | GPIO1 | Internal pull-up |
| EC11 SW / KEY | GPIO10 | Internal pull-up, active-low |
| EC11 C / GND | GND | Common contact of A/B/SW to GND |

Deliberately avoided pins: GPIO8 (Super Mini on-board LED), GPIO12/13 (LuatOS on-board LEDs, and flash is DIO so QIO would not boot), GPIO18/19 (USB), GPIO20/21 (UART0). ST7789 modules vary between sellers: if the picture is mirrored or shows a coloured offset band at an edge, adjust `LCD_MIRROR_X/Y` and `LCD_GAP_X/Y` at the top of `src/bsp/esp32/bsp_esp32c3_st7789_ec11.c` and rebuild. Rotation and press provide all navigation, and either action wakes the display after its timeout.
