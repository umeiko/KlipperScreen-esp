# KlipperScreen-esp

**KlipperScreen-esp** is a compact, cross-platform display and controller for 3D printers. It runs on inexpensive ESP32 boards and Windows/macOS desktops, provides full **Klipper** control through **Moonraker**, and adds read-only **Bambu Cloud** monitoring on Windows. Its shared LVGL UI supports touch, rotary encoder, mouse, keyboard, or mixed input; the EC11 reference target demonstrates a rotary-only device.

![On-device photo](screenshots/main_photo.jpg)

The separately named SDL2 **simulator** uses mock data for layout development, input experiments, and screenshot testing without a printer.

## Features

- **Main** — nozzle/bed/chamber temp cards, print progress, quick actions
- **G-code files** — thumbnails, metadata, history, print/delete
- **Control** — axis jog & homing, extrude/retract with cold-extrusion guard, temperature presets (PLA/PETG/ABS/cooldown), emergency stop & firmware restart with confirmation
- **Robust link** — WebSocket auto-reconnect, app-level heartbeat with RTT display, zombie-connection detection, Klipper error toasts (e.g. endstop not triggered)
- **Bambu status monitor** — Windows sign-in, verification code, bound-device selection, and cloud MQTT status sync are available. Cloud mode is read-only; LAN Developer Mode controls are reserved for a later backend.
- **Extras** — "Umeko" boot animation, 5 languages (EN / 简中 / 繁中 / FR / IT, fade-to-black reboot on switch), brightness slider, auto screen-off with touch wake, title-bar clock synced from the Moonraker host (no internet needed)
- **Input paths** — no-touch rotary boards need no touch layer; resistive touch uses a board-specific calibration path; capacitive touch normally reports screen coordinates directly. Touch and rotary can coexist.

## Supported boards

| Board | Display | Touch | MCU | Status |
|---|---|---|---|---|
| CYD 2432S028R | 2.8" 320×240 ILI9341 SPI | XPT2046 resistive | ESP32 | ✅ Stable |
| E32R35T (ESP32-32E 3.5") | 3.5" 480×320 ST7796 SPI | XPT2046 resistive (shared bus) | ESP32-32E | ✅ Stable |
| esp32s3-st7789-320_240-ec11 | 240×320 ST7789 SPI | None, rotary only | ESP32-S3 N16R8 | ✅ Official reference, contributor tested |
| JC8048W550 | 5" 800×480 ST7262 RGB parallel | GT911 capacitive | ESP32-S3 | ✅ Stable |

Full pinouts and hardware details: [Supported boards](boards.md).

## Quick start

1. Download the zip for your board from [Releases](https://github.com/umeiko/KlipperScreen-esp/releases), unzip, then `flash.bat COMx` (Windows) or `./flash.sh /dev/ttyUSB0`
2. Settings → WiFi: scan → pick AP → enter password
3. Settings → Printer Connection: pick a printer slot and choose Klipper or Bambu mode; for Klipper, enter the host IP + port (default 7125), while Bambu continues to its LAN or cloud settings

For self-compiling see the repo README; to run the firmware on your own board see the [porting guide](porting.md). Start its input section by choosing the no-touch or touch route, then choose resistive or capacitive touch where applicable.

## Documentation map

- [Supported boards](boards.md) — hardware info and pinouts of existing boards
- [Porting to your own board](porting.md) — the BSP contract and implementation notes
- [Contributing a new board](contributing-board.md) — what a board-support PR must change and verify
- [Screenshots](screenshots.md) — current English desktop simulator screens
- [Architecture](architecture.md) — shared UI, BSP, desktop controller, and simulator structure
- [Bambu integration notes](bambu-integration-architecture.md) — current read-only cloud boundary and planned LAN path

## License

MIT. Repository: [github.com/umeiko/KlipperScreen-esp](https://github.com/umeiko/KlipperScreen-esp)
