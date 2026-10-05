#!/usr/bin/env bash
# 虚拟打印机安装脚本：拉取官方主线 klipper/moonraker、打 patch、建运行时。
# 幂等；重复运行只补齐缺失部分。
#   --update        拉取仓库最新主线并重新应用 patch
#   --force-config  用模板覆盖 runtime 里的 printer.cfg / moonraker.conf
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
HERE="$(pwd)"
REPO_ROOT="$(cd ../.. && pwd)"

log() { printf '\033[1;32m[install]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[install]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[install]\033[0m %s\n' "$*" >&2; exit 1; }

UPDATE=0; FORCE_CFG=0
for a in "$@"; do
    case "$a" in
        --update) UPDATE=1 ;;
        --force-config) FORCE_CFG=1 ;;
        *) die "未知参数: $a" ;;
    esac
done

# ---------------- 平台与 python ----------------
UNAME="$(uname -s)"
IS_WINDOWS=0
case "$UNAME" in
    Linux*) ;;
    MSYS*|MINGW*|CYGWIN*) IS_WINDOWS=1 ;;
    *) die "不支持的平台: $UNAME（支持 Linux 与 MSYS2）" ;;
esac

if [ "$IS_WINDOWS" = 1 ]; then
    # 优先使用仓库内 tools/msys64 的 python（cygwin 语义：AF_UNIX/fcntl/信号齐全）
    CAND="$REPO_ROOT/tools/msys64/usr/bin/python3.exe"
    if [ ! -x "$CAND" ]; then
        log "tools/msys64 无 python，尝试 pacman 安装..."
        PACMAN="$REPO_ROOT/tools/msys64/usr/bin/pacman.exe"
        [ -x "$PACMAN" ] || die "找不到 MSYS2（tools/msys64）。请先安装 MSYS2 到 tools/msys64，或用 Linux 主机运行。"
        "$PACMAN" -S --noconfirm --needed python python-pip
    fi
    SYS_PY="/usr/bin/python3"   # 当前已在 MSYS2 bash 内
    command -v python3 >/dev/null || die "MSYS2 内找不到 python3"
    KLIPPY_ADDRESS="tcp://127.0.0.1:7126"
    # libnacl（Moonraker 依赖）按 "libsodium.so" 名搜索，需把 mingw64 的 DLL 链到 usr/bin
    if [ ! -e /usr/bin/libsodium.so ]; then
        if [ ! -f /mingw64/bin/libsodium-26.dll ]; then
            log "安装 mingw-w64-x86_64-libsodium（libnacl 需要）..."
            /usr/bin/pacman -S --noconfirm --needed mingw-w64-x86_64-libsodium
        fi
        cp -f /mingw64/bin/libsodium-26.dll /usr/bin/libsodium.so
        log "已拷贝 libsodium-26.dll -> /usr/bin/libsodium.so"
    fi
else
    command -v python3 >/dev/null || die "需要 python3"
    SYS_PY="$(command -v python3)"
    KLIPPY_ADDRESS="$HERE/runtime/printer_data/comms/klippy.sock"
fi
log "平台: $UNAME  python: $("$SYS_PY" --version 2>&1)"
log "klippy 地址: $KLIPPY_ADDRESS"

# ---------------- 网络/代理 ----------------
proxy_ok() {
    curl -s -m 5 -o /dev/null -x "$1" https://github.com 2>/dev/null
}
if ! curl -s -m 8 -o /dev/null https://github.com; then
    if proxy_ok http://127.0.0.1:7890; then
        export http_proxy=http://127.0.0.1:7890 https_proxy=http://127.0.0.1:7890
        log "直连 github 失败，已启用代理 127.0.0.1:7890"
    else
        warn "github 直连与 127.0.0.1:7890 代理均不可用，克隆/拉取可能失败"
    fi
fi

# ---------------- 克隆仓库 ----------------
clone_or_update() {
    local url="$1" dir="$2" branch="${3:-}"
    if [ -d "$dir/.git" ]; then
        if [ "$UPDATE" = 1 ]; then
            log "更新 $dir ..."
            git -C "$dir" fetch --depth 1 origin ${branch:+"$branch"} || true
            git -C "$dir" reset --hard origin/HEAD 2>/dev/null || git -C "$dir" pull --ff-only || true
        else
            log "$dir 已存在，跳过（--update 可更新）"
        fi
    else
        log "克隆 $url ..."
        git clone --depth 1 ${branch:+-b "$branch"} "$url" "$dir"
    fi
}
mkdir -p repos
clone_or_update https://github.com/Arksine/moonraker.git repos/moonraker
clone_or_update https://github.com/Klipper3d/klipper.git repos/klipper

# ---------------- 应用 patch ----------------
apply_patch() {
    local repo="$1" patch="$2"
    local name; name="$(basename "$patch")"
    local patch_abs; patch_abs="$(cd "$(dirname "$patch")" && pwd)/$name"
    if git -C "$repo" apply --check "$patch_abs" 2>/dev/null; then
        git -C "$repo" apply "$patch_abs"
        log "已应用 $name"
    elif git -C "$repo" apply --reverse --check "$patch_abs" 2>/dev/null; then
        log "$name 已在位，跳过"
    else
        die "patch $name 无法应用（上游已变化？请人工检查 $repo）"
    fi
}
apply_patch repos/moonraker patches/0001-klippy-tcp-transport.patch
if [ "$IS_WINDOWS" = 1 ]; then
    apply_patch repos/moonraker patches/0003-file-manager-inotify-optional.patch
fi
# file_system_observer: none 下启动时扫描已有 gcode 的元数据/缩略图（全平台）
apply_patch repos/moonraker patches/0004-none-observer-metadata-scan.patch

# PIL 不可用才打 metadata patch
if "$SYS_PY" -c "import PIL" 2>/dev/null; then
    PIL_OK=1
else
    PIL_OK=0
    # 看 venv 建好后是否有
fi

# ---------------- python 依赖 ----------------
VENV="$HERE/runtime/venv"
if [ ! -x "$VENV/bin/python" ]; then
    log "创建 venv: $VENV"
    "$SYS_PY" -m venv "$VENV"
fi
PY="$VENV/bin/python"
"$PY" -m pip install --quiet --upgrade pip
DEPS="tornado streaming-form-data distro inotify-simple libnacl jinja2 dbus-fast importlib_metadata"
if "$PY" -c "import PIL" 2>/dev/null; then
    PIL_OK=1
fi
log "安装 python 依赖: $DEPS"
"$PY" -m pip install --quiet $DEPS

if [ "$PIL_OK" = 0 ]; then
    apply_patch repos/moonraker patches/0002-metadata-pil-optional.patch
    warn "Pillow 不可用：已打 0002 patch（缩略图提取保留 PNG 直存，跳过格式转换/32x32 缩略）"
fi

# ---------------- 运行时目录与配置 ----------------
DATA="$HERE/runtime/printer_data"
mkdir -p "$DATA"/{config,gcodes,logs,comms,database}

render_conf() {
    local tpl="$1" out="$2"
    sed -e "s|@KLIPPY_ADDRESS@|$KLIPPY_ADDRESS|g" \
        -e "s|@GCODES_DIR@|$DATA/gcodes|g" \
        -e "s|@DATA_PATH@|$DATA|g" \
        "$tpl" > "$out"
}
if [ ! -f "$DATA/config/moonraker.conf" ] || [ "$FORCE_CFG" = 1 ]; then
    render_conf config/moonraker.conf "$DATA/config/moonraker.conf"
    log "已写入 moonraker.conf"
fi
if [ ! -f "$DATA/config/printer.cfg" ] || [ "$FORCE_CFG" = 1 ]; then
    render_conf config/printer.cfg "$DATA/config/printer.cfg"
    log "已写入 printer.cfg"
fi
echo "$KLIPPY_ADDRESS" > "$HERE/runtime/klippy_address"

# ---------------- 示例 gcode ----------------
if [ -z "$(ls -A "$DATA/gcodes" 2>/dev/null)" ] || [ "$FORCE_CFG" = 1 ]; then
    log "生成示例 gcode..."
    "$SYS_PY" tools/make_sample_gcode.py "$DATA/gcodes"
fi

log "完成。启动: scripts/virtual-printer/run.sh"
