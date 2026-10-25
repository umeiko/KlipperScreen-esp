"""Build the shared KlipperScreen UI in the small Wio Terminal Arduino port."""

import json
from pathlib import Path
Import("env")

port_dir = Path(env.subst("$PROJECT_DIR"))
src_root = port_dir.parent.parent
repo_root = src_root.parent
lvgl_dir = repo_root / "third_party" / "lvgl"
if not lvgl_dir.exists():
    lvgl_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"] / "lvgl"


def patch_rpc_client_timeout():
    """Bound Seeed eRPC waits; upstream waits forever if RTL8720DN misses a reply."""
    roots = [repo_root / "tmp" / "wio-rpcunified"]
    libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]

    if libdeps.exists():
        roots.extend(p.parent.parent.parent for p in libdeps.rglob("src/erpc/erpc_transport_arbitrator.cpp"))
        for link in libdeps.glob("*.pio-link"):
            try:
                spec = json.loads(link.read_text(encoding="utf-8"))["spec"]
                uri = spec.get("uri", "")
                if uri.startswith("symlink://"):
                    roots.append((port_dir / uri[len("symlink://"):]).resolve())
            except (OSError, KeyError, TypeError, ValueError):
                pass

    old = """    // Wait on the semaphore until we're signaled.
    info->m_sem.get(Semaphore::kWaitForever);

    removePendingClient(info);

    return kErpcStatus_Success;
"""
    new = """    // Seeed upstream waits forever here. A lost/corrupt RTL8720DN reply
    // would otherwise permanently block whichever application task made the RPC.
#ifdef KR_ERPC_CLIENT_TIMEOUT_US
    if (!info->m_sem.get(KR_ERPC_CLIENT_TIMEOUT_US))
    {
        removePendingClient(info);
        return kErpcStatus_Timeout;
    }
#else
    info->m_sem.get(Semaphore::kWaitForever);
#endif

    removePendingClient(info);

    return kErpcStatus_Success;
"""

    checked = set()
    for root in roots:
        target = (root / "src" / "erpc" / "erpc_transport_arbitrator.cpp").resolve()
        if target in checked or not target.exists():
            continue
        checked.add(target)
        text = target.read_text(encoding="utf-8")
        if "KR_ERPC_CLIENT_TIMEOUT_US" in text:
            return
        if old not in text:
            continue
        target.write_text(text.replace(old, new, 1), encoding="utf-8")
        return

    raise RuntimeError("unable to apply the pinned rpcUnified client-timeout patch")


def patch_rpc_client_type():
    """Keep the generated client's declaration identical to its definition for LTO."""
    roots = [repo_root / "tmp" / "wio-rpcunified"]
    libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    if libdeps.exists():
        roots.extend(p.parent.parent.parent for p in libdeps.rglob("src/erpc/rpc_wifi_api_client.cpp"))
        for link in libdeps.glob("*.pio-link"):
            try:
                spec = json.loads(link.read_text(encoding="utf-8"))["spec"]
                uri = spec.get("uri", "")
                if uri.startswith("symlink://"):
                    roots.append((port_dir / uri[len("symlink://"):]).resolve())
            except (OSError, KeyError, TypeError, ValueError):
                pass

    old_include = '#include "erpc_client_manager.h"'
    new_include = '#include "erpc_arbitrated_client_manager.h"'
    old_decl = "extern ClientManager *g_client;"
    new_decl = "extern ArbitratedClientManager *g_client;"
    checked = set()
    for root in roots:
        target = (root / "src" / "erpc" / "rpc_wifi_api_client.cpp").resolve()
        if target in checked or not target.exists():
            continue
        checked.add(target)
        text = target.read_text(encoding="utf-8")
        if new_decl in text:
            return
        if old_decl not in text:
            continue
        text = text.replace(old_include, new_include, 1)
        text = text.replace(old_decl, new_decl, 1)
        target.write_text(text, encoding="utf-8")
        return

    raise RuntimeError("unable to apply the pinned rpcUnified client-type patch")


patch_rpc_client_timeout()
patch_rpc_client_type()

env.Append(
    CPPPATH=[
        str(port_dir),
        str(src_root / "bsp"),
        str(src_root / "core"),
        str(src_root / "ui"),
        str(src_root / "ui" / "panels"),
        str(src_root / "ui" / "widgets"),
        str(lvgl_dir),
    ],
    LINKFLAGS=["-Wl,--gc-sections", "-flto"],
)


def build_dir(name, path, src_filter=None):
    env.BuildSources(
        str(Path(env.subst("$BUILD_DIR")) / "shared" / name),
        str(path),
        src_filter=src_filter,
    )


build_dir("ui", src_root / "ui", ["+<*.c>"])
build_dir("ui_panels", src_root / "ui" / "panels", ["+<*.c>"])
build_dir("ui_widgets", src_root / "ui" / "widgets", ["+<*.c>"])

# Keep the normal-size icons and Wio-specific compact Latin/CJK tiers used by
# the shared 320x240 layouts. Leave the generic full CJK, tiny-screen and
# desktop huge variants out of this flash-constrained build.
asset_filter = [
    "+<img_*.c>",
    "+<font_wio_*.c>",
    "-<*_sm.c>",
    "-<*_48.c>",
    "-<*_64.c>",
    "-<*_72.c>",
    "-<*_112.c>",
    "-<*_224.c>",
]
build_dir("ui_assets", src_root / "ui" / "assets", asset_filter)

core_files = [
    "app_settings.c",
    "bambu_cloud_stub.c",
    "bambu_monitor_stub.c",
]
build_dir("core", src_root / "core", [f"+<{name}>" for name in core_files])
build_dir(
    "bsp_common",
    src_root / "bsp",
    ["+<bsp_screen_power.c>", "+<bsp_display_color.c>"],
)
