# Linux host

Run KlipperScreen-esp directly on the Linux host of your 3D printer (the same machine that runs Klipper/Moonraker) — a lightweight, dependency-free alternative to KlipperScreen. A retired phone or tablet running a Linux chroot/proot also works.

![KlipperScreen-esp running on a Redmi 4 (aarch64 Ubuntu) via the weston kiosk backend](screenshots/boards/linux_redmi4.jpg)

*A retired Redmi 4 (aarch64, Ubuntu 24.04) as the Klipper host display, fullscreen through the systemd service + weston kiosk shell.*

## One-line install

```bash
curl -fsSL https://raw.githubusercontent.com/umeiko/KlipperScreen-esp/main/scripts/linux/install.sh | bash
```

The installer automatically:

1. Detects your architecture (`x86_64` / `arm64`) and downloads the matching prebuilt package from the latest release — no compilation on the host
2. Asks whether you want a **dedicated display service** or a regular **desktop app**:
   - **Service (default)**: a `KlipperScreen-esp.service` systemd unit starts the UI fullscreen on boot, through a Wayland (weston kiosk shell) or X11 (bare xinit) backend of your choice; the required packages (`weston` or `xinit`) are installed automatically. If `KlipperScreen.service` is detected, the installer offers to disable it so the two don't fight over the screen
   - **Desktop app**: just installs the binary and a launcher shortcut in your applications menu
3. Installs into `~/.local/share/KlipperScreen-esp/`; configuration lives in `~/.config/KlipperScreen-esp/`

Requirements: Debian/Ubuntu-family with `apt`, glibc ≥ 2.35 (Debian 12 / Ubuntu 22.04 and newer). The binary statically links SDL2 and cJSON; X11/Wayland system libraries are loaded at runtime only.

## Manual install

Download `desktop-linux-x86_64.tar.gz` or `desktop-linux-arm64.tar.gz` from [Releases](https://github.com/umeiko/KlipperScreen-esp/releases/latest), extract, and run `./install.sh`. Non-interactive usage for scripted deployments:

```bash
SERVICE=n ./install.sh                  # desktop app only
KR_BACKEND=x11 ./install.sh             # service mode, force X11
KR_BACKEND=wayland KR_START=0 ./install.sh
```

Uninstall with the bundled `./uninstall.sh`.

## Notes

- At 720p and above the UI switches to the large-font/icon tier automatically, and the boot animation is skipped on high-resolution Linux hosts for a faster start.
- Touch input works through the kernel's evdev/libinput stack — both weston and xinit backends forward it transparently.
- Building from source instead: see [Building from source](building.md).
