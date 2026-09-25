#!/bin/bash
# KlipperScreen-esp 卸载脚本
set -u

INSTALL_DIR="${HOME}/.local/share/KlipperScreen-esp"
SERVICE_NAME="KlipperScreen-esp.service"

if systemctl list-unit-files "$SERVICE_NAME" 2>/dev/null | grep -q KlipperScreen-esp; then
    sudo systemctl disable --now "$SERVICE_NAME"
    sudo rm -f "/etc/systemd/system/$SERVICE_NAME"
    sudo systemctl daemon-reload
fi

rm -rf "$INSTALL_DIR"
rm -f "$HOME/.local/share/applications/KlipperScreen-esp.desktop"

echo "KlipperScreen-esp removed. Config kept at ~/.config/KlipperScreen-esp (delete manually if unwanted)."
