# Windows / macOS client

The same UI that runs on the ESP32 boards is also available as a desktop app: full Klipper/Moonraker control plus Bambu Cloud monitoring, driven by mouse and keyboard. It doubles as the development vehicle — all UI work is debugged on the desktop build first.

**Packages**: [desktop-win-x86_64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-win-x86_64.zip) / [desktop-macos-arm64.zip](https://github.com/umeiko/KlipperScreen-esp/releases/latest/download/desktop-macos-arm64.zip)

## Quick start

1. Download and extract the zip for your platform.
2. Run **`KlipperScreen-esp`** (`KlipperScreen-esp.exe` on Windows). Do **not** run `klipper_remote_simulator` — that separately named binary is a layout-development simulator feeding mock data (fake file list, no thumbnails, no real Moonraker connection).
3. Settings → Printer Connection: pick a slot, choose Klipper (host IP + port, default 7125) or Bambu mode.

On Windows the configuration is stored in `%APPDATA%\KlipperRemote\moonraker.conf`.

## Notes

- **Keyboard navigation** mirrors the physical boards: arrows move the focus, Enter confirms, Esc goes back. In text inputs the arrows + Enter type on the on-screen keyboard, and **F1 submits the form**.
- **Display rotation** 0/90/180/270° is available under Settings → Display (software rotation; a restart of the app applies it).
- **macOS**: the binaries are ad-hoc signed. If Gatekeeper refuses to open them, run `xattr -dr com.apple.quarantine` on the extracted folder, or allow the app in System Settings → Privacy & Security.
- Building from source (including the mock-data simulator): see [Building from source](../building.md).
