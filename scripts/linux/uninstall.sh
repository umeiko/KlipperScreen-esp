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

# 硬件联动遗留（install.sh 服务模式安装）
sudo rm -f /etc/udev/rules.d/99-KlipperScreen-esp-backlight.rules
sudo udevadm control --reload 2>/dev/null || true
sudo rm -f /etc/systemd/logind.conf.d/90-KlipperScreen-esp.conf
sudo systemctl restart systemd-logind 2>/dev/null || true

echo "KlipperScreen-esp removed. Config kept at ~/.config/KlipperScreen-esp (delete manually if unwanted)."
