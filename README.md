# KlipperScreen-esp

<p align="center">
  <img src="docs/assets/branding/klipperscreen-esp-logo-v1.png" alt="KlipperScreen-esp logo" width="520">
</p>

[![build](https://github.com/umeiko/KlipperScreen-esp/actions/workflows/build.yml/badge.svg)](https://github.com/umeiko/KlipperScreen-esp/actions/workflows/build.yml)

[中文 README](README_zh.md)

> **📖 Documentation: https://umeiko.github.io/KlipperScreen-esp/**


<p align="center">
  <img src="docs/screenshots/main_photo.jpg" alt="KlipperScreen-esp running on multiple devices" width="720">
</p>

**KlipperScreen-esp** is a compact, cross-platform display and controller for 3D printers. It runs on inexpensive ESP32 boards, Seeed Wio Terminal, and desktop/mobile systems, fully controls **Klipper** printers through **Moonraker**, and provides **Bambu cloud status monitoring**.

## Screenshots

Full interface gallery on the docs site: **[Screenshots](https://umeiko.github.io/KlipperScreen-esp/screenshots/)**

## Features

- Wireless control with **zero intrusion** on the Klipper host — no plugins, no performance impact
- Manage and switch between **Bambu** and **Klipper** printers on one device
- Multi-printer on one screen: up to 6 printer slots, each bound to Klipper or Bambu, one-tap switch with instant reconnect
- Control: axis jog & homing, extrude/retract, temperature presets (PLA/PETG/ABS/cooldown), emergency stop & host restart
- Print status monitoring, pause, and temperature control — for both Klipper and Bambu printers

## Tech stack

ESP-IDF v5.5.5 · PlatformIO/ATSAMD51 · LVGL v9.3 · Multi-backend (ESP32, Wio Terminal, Windows, Linux, macOS, Android)

## Flash 

Download the flash package `*.zip` for your board from [Releases](../../releases), unzip, then:

- **Windows**: double-click the `*.bat` script and follow the prompts.
- **macOS / Linux**: `pip install esptool`, then `./flash.sh /dev/ttyUSB0`

For **Seeed Wio Terminal**, download `KlipperScreen-esp-Wio-Terminal.uf2`, enter the UF2 bootloader by sliding the power/reset switch twice quickly, then copy the file to the `Arduino` drive.

Supported boards include the common yellow CYD ESP32 boards, ESP32-S3/C3 boards, and Seeed Wio Terminal. Prebuilt firmware per board: **[Supported boards](https://umeiko.github.io/KlipperScreen-esp/boards/)**

## Deploying to a Linux host

Install with this one-liner:

```bash
curl -fsSL https://raw.githubusercontent.com/umeiko/KlipperScreen-esp/main/scripts/linux/install.sh | bash
```

Or download `desktop-linux-x86_64.tar.gz` / `desktop-linux-arm64.tar.gz` from [Releases](../../releases), extract, then run `./install.sh`:

- Choose between a **dedicated display service** (systemd autostart, fullscreen via Wayland-weston or X11) or a regular **desktop app**.
- If `KlipperScreen.service` is detected, the installer offers to disable it.
- On Klipper hosts, config lands in `~/printer_data/config/KlipperScreen-esp/` (editable from fluidd/mainsail) and logs in `~/printer_data/logs/`.
- On desktop environments, config lives in `~/.config/KlipperScreen-esp/`; uninstall with `uninstall.sh`.


## First-time setup

1. Settings → WiFi: scan → pick an AP → enter the password. WiFi credentials are saved and auto-connect on next boot.
2. Settings → Printer Connection: pick a printer slot and its machine mode (Bambu or Klipper). For Klipper, enter host IP + port (default 7125); for Bambu, sign in with your phone or email. The configuration is saved and auto-connects on next boot.
3. Language / backlight / auto screen-off preferences are all saved automatically.

Bambu mode lives under Settings → Printer Connection → Machine Mode → Bambu.

If taps land off-target on a resistive-touch board, run the two-point calibration described in the docs: [Touch calibration](https://umeiko.github.io/KlipperScreen-esp/#touch-calibration-resistive-touch-boards).

Serial CLI commands (115200 8N1): `help` / `wifi` / `mr` / `printer <1-6>` / `mrstart` / `gc` / `status` / `ps` / `ls` / `cd` / `cat` / `rm` …

## Building from source

Toolchain: **ESP-IDF v5.5.5** · **LVGL v9.3** · SDL2 (desktop).

```bash
bash tools/build-desktop.sh                       # desktop (KlipperScreen-esp controller + simulator)
bash tools/build-esp32.sh <board> build           # ESP32 firmware (16 boards)
bash tools/build-esp32.sh <board> flash COMx      # build and flash
platformio run -d src/ports/wio_terminal -e wio-terminal  # Wio Terminal
```

Linux hosts can also build from source: `sudo apt install cmake libsdl2-dev libcjson-dev`, then in `src/ports/desktop` run `cmake -S . -B build && cmake --build build`.

Portable MSYS2 toolchain setup, LVGL checkout, multi-board build details, and regenerating CJK font subsets and icons: **[Building from source](https://umeiko.github.io/KlipperScreen-esp/building/)**

## Credits

- [KlipperScreen](https://github.com/KlipperScreen/KlipperScreen) — UI design inspiration
- LVGL, ESP-IDF, esptool by their respective authors

## License

MIT
