# KlipperScreen-esp

A touchscreen remote display for **Klipper** 3D printers, talking to **Moonraker** over WiFi — running on cheap ESP32 boards. Think of it as a pocket-sized, wireless KlipperScreen.

![On-device photo](screenshots/main_photo.jpg)

The same UI code also compiles as a **desktop simulator** (SDL2, Windows/Linux), so every panel can be developed and screenshot-tested without flashing hardware.

## Features

- **Main** — nozzle/bed/chamber temp cards, print progress, quick actions
- **G-code files** — thumbnails, metadata, history, print/delete
- **Control** — axis jog & homing, extrude/retract with cold-extrusion guard, temperature presets (PLA/PETG/ABS/cooldown), emergency stop & firmware restart with confirmation
- **Robust link** — WebSocket auto-reconnect, app-level heartbeat with RTT display, zombie-connection detection, Klipper error toasts (e.g. endstop not triggered)
- **Extras** — "Umeko" boot animation, 5 languages (EN / 简中 / 繁中 / FR / IT, fade-to-black reboot on switch), brightness slider, auto screen-off with touch wake, title-bar clock synced from the Moonraker host (no internet needed)
- **One-time touch calibration** persisted to flash; factory calibration pre-installed for the 2432S028R

## Supported boards

| Board | Display | Touch | MCU | Status |
|---|---|---|---|---|
| CYD 2432S028R | 2.8" 320×240 ILI9341 SPI | XPT2046 resistive | ESP32 | ✅ Stable |
| E32R35T (ESP32-32E 3.5") | 3.5" 480×320 ST7796 SPI | XPT2046 resistive (shared bus) | ESP32-32E | ✅ Stable |
| JC8048W550 | 5" 800×480 ST7262 RGB parallel | GT911 capacitive | ESP32-S3 | ✅ Stable |

Full pinouts and hardware details: [Supported boards](boards.md).

## Quick start

1. Download the zip for your board from [Releases](https://github.com/umeiko/KlipperScreen-esp/releases), unzip, then `flash.bat COMx` (Windows) or `./flash.sh /dev/ttyUSB0`
2. Settings → WiFi: scan → pick AP → enter password
3. Settings → Moonraker: pick a printer slot (up to 6), enter host IP + port (default 7125)

For self-compiling see the repo README; to run the firmware on your own board see the [porting guide](porting.md).

## Documentation map

- [Supported boards](boards.md) — hardware info and pinouts of existing boards
- [Porting to your own board](porting.md) — the BSP contract and implementation notes
- [Contributing a new board](contributing-board.md) — what a board-support PR must change and verify

## License

GPLv3. Repository: [github.com/umeiko/KlipperScreen-esp](https://github.com/umeiko/KlipperScreen-esp)
