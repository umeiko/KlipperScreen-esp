#!/bin/bash
# KlipperScreen-esp Linux 安装脚本
# 一行安装（自动下载匹配架构的最新 release）：
#   curl -fsSL https://raw.githubusercontent.com/umeiko/KlipperScreen-esp/main/scripts/linux/install.sh | bash
# 或从 release tar.gz 解压后运行：./install.sh
# 非交互用法（CI/脚本）：
#   SERVICE=n ./install.sh                 # 只装桌面 App
#   KR_BACKEND=x11 ./install.sh            # 服务模式强制 X11
#   KR_BACKEND=wayland KR_START=0 ./install.sh
set -u

SCRIPTPATH=$(dirname -- "$(readlink -f -- "$0")")
INSTALL_DIR="${HOME}/.local/share/KlipperScreen-esp"
SERVICE_NAME="KlipperScreen-esp.service"
RELEASE_BASE="https://github.com/umeiko/KlipperScreen-esp/releases/latest/download"

Red='\033[0;31m'; Green='\033[0;32m'; Cyan='\033[0;36m'; Normal='\033[0m'
echo_text() { printf "${Normal}%s${Cyan}\n" "$1"; }
echo_error() { printf "${Red}%s${Normal}\n" "$1"; }
echo_ok() { printf "${Green}%s${Normal}\n" "$1"; }

if [ "$EUID" -eq 0 ]; then
    echo_error "Please do not run this script as root"
    exit 1
fi
# 无 tty（ssh/脚本调用）时 sudo 读不到密码：设了 SUDO_ASKPASS 就自动走 -A
if [ ! -t 0 ] && [ -n "${SUDO_ASKPASS:-}" ]; then
    sudo() { command sudo -A "$@"; }
fi
if ! command -v apt >/dev/null 2>&1; then
    echo_error "This installer targets Debian/Ubuntu (apt not found)"
    exit 1
fi

# curl|bash 管道安装时 stdin 是脚本本身：交互问答改从 /dev/tty 读。
# 注意不能用 -r/-w 判断：无控制终端的会话里 /dev/tty 存在但打不开，
# 必须真的打开一次才算数。完全无头时绝不能 read——stdin 上还连着
# 脚本本身，read 会把后续脚本内容吃掉，bash 解析错位报语法错误。
INTERACTIVE=0
TTY=/dev/stdin
if [ -t 0 ]; then
    INTERACTIVE=1
elif (exec 3<>/dev/tty) 2>/dev/null; then
    INTERACTIVE=1
    TTY=/dev/tty
fi

# ---------- 0. 独立运行（curl 管道）时自动下载 release 包 ----------
PKG_TMP=""
if [ ! -f "$SCRIPTPATH/bin/KlipperScreen-esp" ]; then
    case "$(uname -m)" in
        x86_64|amd64)  PKG_ARCH=x86_64 ;;
        aarch64|arm64) PKG_ARCH=arm64 ;;
        *) echo_error "Unsupported architecture: $(uname -m)"; exit 1 ;;
    esac
    PKG_TMP=$(mktemp -d)
    trap 'rm -rf "$PKG_TMP"' EXIT
    PKG_URL="$RELEASE_BASE/desktop-linux-$PKG_ARCH.tar.gz"
    echo_text "Downloading $PKG_URL"
    if command -v curl >/dev/null 2>&1; then
        curl -fSL "$PKG_URL" -o "$PKG_TMP/pkg.tar.gz" \
            || { echo_error "Download failed"; exit 1; }
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$PKG_TMP/pkg.tar.gz" "$PKG_URL" \
            || { echo_error "Download failed"; exit 1; }
    else
        echo_error "Neither curl nor wget found"; exit 1
    fi
    tar -xzf "$PKG_TMP/pkg.tar.gz" -C "$PKG_TMP" \
        || { echo_error "Failed to extract package"; exit 1; }
    SCRIPTPATH="$PKG_TMP"
fi

# ---------- 1. 安装形态 ----------
if [ -z "${SERVICE:-}" ]; then
    echo_text ""
    echo_text "Install as a dedicated display service?"
    echo_text "It will start fullscreen on boot (replaces the console on tty7)."
    echo_text "Say no to install as a regular desktop app instead."
    echo_text ""
    echo "Press enter for default (Yes)"
    # 重定向失败时 read 不会执行，必须显式置空，否则 set -u 下引用即死
    if [ "$INTERACTIVE" = 1 ]; then
        read -r -e -p "[Y/n] " SERVICE < "$TTY" || SERVICE=
    else
        SERVICE=
    fi
fi

if [[ "$SERVICE" =~ ^[nN]$ ]]; then
    SERVICE=n
else
    SERVICE=y
fi

# ---------- 2. 图形后端（仅服务模式） ----------
BACKEND="${KR_BACKEND:-}"
if [ "$SERVICE" = y ] && [ -z "$BACKEND" ]; then
    echo_text ""
    echo_text "Choose graphical backend"
    echo_text "Wayland (weston kiosk) is recommended; X11 uses a bare xinit session."
    echo_text ""
    echo "Press enter for default (Wayland)"
    if [ "$INTERACTIVE" = 1 ]; then
        read -r -e -p "Backend Wayland or X11? [W/x] " BACKEND < "$TTY" || BACKEND=
    else
        BACKEND=
    fi
fi
if [[ "$BACKEND" =~ ^[xX]$ ]]; then
    BACKEND=X
else
    BACKEND=W
fi

# ---------- 3. 依赖 ----------
install_deps() {
    echo_text "Installing dependencies..."
    sudo apt update
    if [ "$SERVICE" = n ]; then
        return
    fi
    if [ "$BACKEND" = X ]; then
        sudo apt install -y xinit xserver-xorg-input-libinput xserver-xorg-input-evdev \
            || { echo_error "Failed to install X11 dependencies"; exit 1; }
        sudo tee /etc/X11/Xwrapper.config > /dev/null <<EOF
allowed_users=anybody
needs_root_rights=yes
EOF
    else
        sudo apt install -y weston \
            || { echo_error "Failed to install weston"; exit 1; }
    fi
    # weston/xinit 以普通用户访问显示与输入设备
    sudo adduser "$USER" tty >/dev/null 2>&1 || true
    sudo adduser "$USER" video >/dev/null 2>&1 || true
    sudo adduser "$USER" input >/dev/null 2>&1 || true
    sudo adduser "$USER" render >/dev/null 2>&1 || true
}

# ---------- 4. 既有 KlipperScreen 冲突 ----------
handle_klipperscreen() {
    [ "$SERVICE" = y ] || return
    if systemctl list-unit-files KlipperScreen.service 2>/dev/null | grep -q KlipperScreen; then
        echo_text ""
        echo_text "Detected KlipperScreen.service (it owns the screen via weston/tty7)."
        if [ -z "${KR_REPLACE_KS:-}" ]; then
            echo "Press enter for default (Yes)"
            if [ "$INTERACTIVE" = 1 ]; then
                read -r -e -p "Disable and stop it? [Y/n] " KR_REPLACE_KS < "$TTY" || KR_REPLACE_KS=
            else
                KR_REPLACE_KS=
            fi
        fi
        if [[ ! "$KR_REPLACE_KS" =~ ^[nN]$ ]]; then
            sudo systemctl disable --now KlipperScreen.service \
                && echo_ok "KlipperScreen disabled" \
                || echo_error "Failed to disable KlipperScreen (continuing)"
        fi
    fi
}

# ---------- 5. 安装文件 ----------
install_files() {
    mkdir -p "$INSTALL_DIR/bin"
    cp "$SCRIPTPATH/bin/KlipperScreen-esp" "$INSTALL_DIR/bin/KlipperScreen-esp"
    chmod +x "$INSTALL_DIR/bin/KlipperScreen-esp"
    cp "$SCRIPTPATH/KlipperScreen-esp-start.sh" "$INSTALL_DIR/KlipperScreen-esp-start.sh"
    chmod +x "$INSTALL_DIR/KlipperScreen-esp-start.sh"
    mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/KlipperScreen-esp"
}

# ---------- 6. systemd 服务 ----------
install_service() {
    [ "$SERVICE" = y ] || return
    echo_text "Installing $SERVICE_NAME"

    UNIT=$(cat "$SCRIPTPATH/KlipperScreen-esp.service")
    UNIT=${UNIT//KR_USER/$USER}
    UNIT=${UNIT//KR_DIR/$INSTALL_DIR}
    UNIT=${UNIT//KR_BACKEND/$BACKEND}

    echo "$UNIT" | sudo tee "/etc/systemd/system/$SERVICE_NAME" > /dev/null
    sudo systemctl unmask "$SERVICE_NAME" 2>/dev/null || true
    sudo systemctl daemon-reload
    sudo systemctl enable "$SERVICE_NAME"
    sudo systemctl set-default multi-user.target

    # 无虚拟终端的环境（部分容器/手机 chroot）禁用 VT 相关配置
    if [ ! -e /sys/class/tty/tty7 ] || [ ! -c /dev/tty7 ]; then
        echo_text "No usable virtual terminal detected, disabling VT options"
        sudo sed -i \
            -e '/^ConditionPathExists=/s/^/#/' \
            -e '/^ExecStartPost=/s/^/#/' \
            -e '/^UtmpIdentifier=/s/^/#/' \
            -e '/^UtmpMode=/s/^/#/' \
            -e '/^TTYPath=/s/^/#/' \
            -e '/^TTYReset=/s/^/#/' \
            -e '/^TTYVHangup=/s/^/#/' \
            -e '/^TTYVTDisallocate=/s/^/#/' \
            "/etc/systemd/system/$SERVICE_NAME"
        sudo systemctl daemon-reload
    fi
}

# ---------- 7. 桌面 App 入口 ----------
install_desktop_file() {
    mkdir -p "$HOME/.local/share/applications"
    cat > "$HOME/.local/share/applications/KlipperScreen-esp.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Klipper Remote
Comment=Remote display for Klipper/Moonraker 3D printers
Exec=$INSTALL_DIR/bin/KlipperScreen-esp
Terminal=false
Categories=Utility;
EOF
}

install_deps
handle_klipperscreen
install_files
install_service
install_desktop_file

if [ "$SERVICE" = y ] && [ "${KR_START:-1}" != 0 ]; then
    echo_text "Starting service..."
    sudo systemctl restart "$SERVICE_NAME"
    echo_ok "KlipperScreen-esp installed and started (backend: $BACKEND)"
else
    echo_ok "KlipperScreen-esp installed"
    echo_text "Run: $INSTALL_DIR/bin/KlipperScreen-esp"
fi
