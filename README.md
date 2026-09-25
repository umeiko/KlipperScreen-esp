# KlipperScreen-esp

[![build](https://github.com/umeiko/KlipperScreen-esp/actions/workflows/build.yml/badge.svg)](https://github.com/umeiko/KlipperScreen-esp/actions/workflows/build.yml)

[中文 README](README_zh.md)

> **📖 Documentation: https://umeiko.github.io/KlipperScreen-esp/**


<p align="center">
  <img src="docs/screenshots/main_photo.png" alt="KlipperScreen-esp running on a CYD 2432S028R" width="720">
</p>

**KlipperScreen-esp** is a compact, cross-platform display and controller for 3D printers. It runs on inexpensive ESP32 dev boards as well as Windows/macOS desktops, fully controls **Klipper** printers through **Moonraker**, and provides **Bambu cloud status monitoring**.

## Screenshots

Full interface gallery on the docs site: **[Screenshots](https://umeiko.github.io/KlipperScreen-esp/screenshots/)**

## Features

- Wireless control with **zero intrusion** on the Klipper host — no plugins, no performance impact
- Manage and switch between **Bambu** and **Klipper** printers on one device
- Multi-printer on one screen: up to 6 printer slots, each bound to Klipper or Bambu, one-tap switch with instant reconnect
- Control: axis jog & homing, extrude/retract, temperature presets (PLA/PETG/ABS/cooldown), emergency stop & host restart
- Print status monitoring, pause, and temperature control — for both Klipper and Bambu printers

## Tech stack

ESP-IDF v5.5.5 · LVGL v9.3 · Multi-backend (ESP32, Windows, Linux, macOS)

## Flash 

Download the flash package `*.zip` for your board from [Releases](../../releases), unzip, then:

- **Windows**: double-click the `*.bat` script and follow the prompts.
- **macOS / Linux**: `pip install esptool`, then `./flash.sh /dev/ttyUSB0`

Supported boards: the common yellow CYD ESP32 dev boards and ESP32-S3 boards. Prebuilt firmware per board: **[Supported boards](https://umeiko.github.io/KlipperScreen-esp/boards/)**

## Linux host (prebuilt, KlipperScreen-style deployment)

One-line install (downloads the latest release for your architecture, asks how you want it installed):

```bash
curl -fsSL https://raw.githubusercontent.com/umeiko/KlipperScreen-esp/main/scripts/linux/install.sh | bash
```

Or download `desktop-linux-x86_64.tar.gz` / `desktop-linux-arm64.tar.gz` from [Releases](../../releases), extract, then run `./install.sh`:

- Choose between a **dedicated display service** (systemd autostart, fullscreen via Wayland-weston or X11) or a regular **desktop app**.
- If `KlipperScreen.service` is detected, the installer offers to disable it to avoid contention for the screen.
- The binary statically links SDL2/cJSON (X11/Wayland libraries are loaded at runtime) and only needs glibc ≥ 2.35 (Debian 12 / Ubuntu 22.04+, aarch64 or x86_64).
- Config lives in `~/.config/KlipperScreen-esp/`; uninstall with `uninstall.sh`.


## First-time setup

1. Settings → WiFi: scan → pick an AP → enter the password. WiFi credentials are saved and auto-connect on next boot.
2. Settings → Printer Connection: pick a printer slot and its machine mode (Bambu or Klipper). For Klipper, enter host IP + port (default 7125); for Bambu, sign in with your phone or email. The configuration is saved and auto-connects on next boot.
3. Language / backlight / auto screen-off preferences are all saved automatically.

Bambu mode lives under Settings → Printer Connection → Machine Mode → Bambu.

Serial CLI commands (115200 8N1): `help` / `wifi` / `mr` / `printer <1-6>` / `mrstart` / `gc` / `status` / `ps` / `ls` / `cd` / `cat` / `rm` …

## Building from source

Toolchain: **ESP-IDF v5.5.5** · **LVGL v9.3** · SDL2 (desktop).

```bash
bash tools/build-desktop.sh                       # desktop (KlipperScreen-esp controller + simulator)
bash tools/build-esp32.sh <board> build           # ESP32 firmware (16 boards)
bash tools/build-esp32.sh <board> flash COMx      # build and flash
```

Linux hosts can also build from source: `sudo apt install cmake libsdl2-dev libcjson-dev`, then in `src/ports/desktop` run `cmake -S . -B build && cmake --build build`.

Portable MSYS2 toolchain setup, LVGL checkout, multi-board build details, and regenerating CJK font subsets and icons: **[Building from source](https://umeiko.github.io/KlipperScreen-esp/building/)**

## Credits

- [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen) — UI design inspiration
- LVGL, ESP-IDF, esptool by their respective authors

## License

MIT
