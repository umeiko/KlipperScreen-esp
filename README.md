# KlipperScreen-esp

[![build](https://github.com/umeiko/Klipper-Remote-esp32-Displays/actions/workflows/build.yml/badge.svg)](https://github.com/umeiko/Klipper-Remote-esp32-Displays/actions/workflows/build.yml)

[中文文档](README_zh.md)

<p align="center">
  <img src="docs/screenshots/main_photo.jpg" alt="Klipper Remote on a CYD 2432S028R" width="720">
</p>

A touchscreen remote display for **Klipper** 3D printers, talking to **Moonraker** over WiFi — running on cheap **ESP32 CYD (2432S028R)** boards. Think of it as a pocket-sized, wireless KlipperScreen.

The same UI code also compiles as a **desktop simulator** (SDL2, Windows/Linux), so every panel can be developed and screenshot-tested without flashing hardware.

## Screenshots

See the full interface gallery in the docs: **[界面展示 / Screenshots](https://umeiko.github.io/KlipperScreen-esp/screenshots/)**

## Features

- **Multi-printer** — up to 6 Moonraker slots with a 3×2 switcher page; one tap changes the active printer and reconnects instantly
- **Live printer status** — nozzle/bed temperatures in the title bar, state card (idle / printing / paused / complete / error), per-state full-card color coding
- **Print jobs** — browse G-code history, print or delete from a detail view, live progress ring with elapsed/remaining time, pause / resume / cancel
- **Control** — axis jog & homing, extrude/retract with cold-extrusion guard, temperature presets (PLA/PETG/ABS/cooldown), emergency stop & firmware restart with confirmation
- **Robust link** — WebSocket auto-reconnect, app-level heartbeat with RTT display, zombie-connection detection, Klipper error toasts (e.g. endstop not triggered)
- **Extras** — "Umeko" boot animation, 5 languages (EN / 简中 / 繁中 / FR / IT, fade-to-black reboot on switch), brightness slider, auto screen-off with touch wake, title-bar clock synced from the Moonraker host (no internet needed)
- **One-time touch calibration** persisted to flash; factory calibration pre-installed for the 2432S028R

## Hardware

- **ESP32-2432S028R** ("Cheap Yellow Display"): 320×240 ILI9341 TFT + XPT2046 resistive touch, WiFi
- **ESP32-32E E32R35T** (3.5"): 480×320 ST7796 TFT + XPT2046 resistive touch (shared SPI bus) — stable since v0.2.0
- **JC8048W550** (Guition 5"): 800×480 ST7262 RGB TFT + GT911 capacitive touch, ESP32-S3 — stable since v0.2.0 (the tearing hunt is documented in [docs/jc8048w550-rgb-display-guide.md](docs/jc8048w550-rgb-display-guide.md))
- Same LAN as the Klipper host (Moonraker reachable at `host:7125`)

## Flash (release zip)

Download `klipper-remote-esp32-*.zip` from [Releases](../../releases) (or CI artifacts), unzip, then:

- **Windows**: `flash.bat COM6`
- **macOS / Linux**: `./flash.sh /dev/ttyUSB0` (needs `pip install esptool`)

The zip contains `bootloader.bin`, `partition-table.bin`, the app binary, `esptool.exe` (Windows standalone) and the flash scripts. First boot auto-formats the LittleFS partition and writes factory touch calibration.

## First-time setup

1. **Settings → WiFi**: scan, pick AP, enter password — saved to `network.conf`
2. **Settings → Moonraker**: pick a printer slot (up to 6), host IP + port (default 7125), optional API key — saved to `moonraker.conf`
3. Preferences (language / brightness / screen-off) live in `klipperscreen.conf`

All config lives in LittleFS on the device. A serial CLI (`115200 8N1`) is available for debugging: `help`, `wifi`, `mr`, `printer <1-6>`, `mrstart`, `gc`, `status`, `ps`, `ls`, `cd`, `cat`, `rm` …

## Build from source

Toolchain: **ESP-IDF v5.5.5** · **LVGL v9.3** · SDL2 (desktop).

```bash
# Desktop simulator (Windows via bundled MSYS2, or Linux with system SDL2)
bash tools/build-desktop.sh
./src/ports/desktop/build/klipper_remote_desktop.exe              # interactive window
./src/ports/desktop/build/klipper_remote_desktop.exe 3000 x.bmp   # screenshot after 3s

# ESP32 firmware (project dir: src/ports/esp32)
cd src/ports/esp32
powershell -NoProfile -ExecutionPolicy Bypass -File ../../../tools/idf.ps1 build   # Windows wrapper
powershell -NoProfile -ExecutionPolicy Bypass -File ../../../tools/idf.ps1 -p COMx flash monitor
```

On Windows, call `idf.py` through `tools/idf.ps1` — Git Bash injects `MSYSTEM` into child processes and makes `idf.py` silently no-op. See [README_zh.md](README_zh.md) for the full Chinese toolchain guide (offline installers, mirrors).

## Project layout

```
src/
  ui/          # panels, theme, i18n, icons, boot animation — shared by all ports
  core/        # moonraker client (WS+JSON-RPC), printer model, settings
  bsp/         # board support: bsp.h contract + per-arch implementations
  ports/       # one buildable project per port (esp32 / desktop)
docs/          # architecture & API notes
tools/         # build scripts, font/icon generators, release packaging
.github/       # CI: ESP-IDF build → flashable zip (+ release on v* tags)
```

Adding a new board of the same architecture = one file in `src/bsp/<arch>/`. A whole new port = BSP implementation + a thin project shell in `src/ports/<arch>/`. Details: [docs/architecture.md](docs/architecture.md).

## Regenerating assets

- **CJK fonts** (required after changing any UI string): `python tools/fontgen/gen_fonts.py` — extracts non-ASCII literals from `src/ui` and rebuilds the subset fonts; otherwise you'll get □ boxes.
- **Icons**: drop an SVG into `src/ui/assets/svg/`, add a line in `tools/icongen/gen_icons.mjs`, run `node tools/icongen/gen_icons.mjs`.

## Credits

- UI iconography adapted from [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen) material-dark theme (GPL-3.0)

- LVGL, ESP-IDF, esptool by their respective authors

## License

GPL-3.0.
