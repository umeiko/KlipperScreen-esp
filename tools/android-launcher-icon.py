#!/usr/bin/env python3
"""生成 Android 启动器图标（mipmap-*/ic_launcher.png）。

图案来源与开机画面同源：src/ui/assets/boot_logo_path.h 的「Umeko」
轮廓描边点表 + 逐行填充游程（坐标系 280x240，logo 本体约 x15..264 y91..147）。
配色取主题常量：底色 THEME_COL_BG #12151C，填充 THEME_COL_ACCENT #2D9CDB，
描边浅蓝。纯标准库实现（无 PIL 依赖），4x 超采样抗锯齿。
"""
import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOGO_H = ROOT / "src/ui/assets/boot_logo_path.h"
RES_DIR = ROOT / "src/ports/android/app/src/main/res"

# (mipmap 目录名, 图标边长 px)
SIZES = [("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)]

BG = (0x12, 0x15, 0x1C)
FILL = (0x2D, 0x9C, 0xDB)
STROKE = (0xBF, 0xE3, 0xF7)


def read_array(text, name):
    m = re.search(name + r"\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        raise SystemExit(f"array {name} not found in {LOGO_H}")
    return [int(v) for v in m.group(1).split(",")]


def write_png(path, w, h, rgba):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(
        b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h)
    )
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def render(size, path_x, path_y, fill_y, fill_x0, fill_x1):
    ss = 4                      # 超采样倍率
    s = size * ss
    px = bytearray(s * s * 4)

    # 圆角方底
    radius = s * 0.18
    for y in range(s):
        for x in range(s):
            dx = max(radius - 0.5 - x, x - (s - radius - 0.5), 0)
            dy = max(radius - 0.5 - y, y - (s - radius - 0.5), 0)
            if dx * dx + dy * dy <= radius * radius:
                i = (y * s + x) * 4
                px[i:i + 4] = bytes((*BG, 255))

    # logo 几何：本体宽 250（x15..264）、高 57（y91..147），缩放到图标 80% 宽并居中
    logo_w, logo_h = 264 - 15, 147 - 91
    scale = (s * 0.80) / logo_w
    off_x = (s - logo_w * scale) / 2 - 15 * scale
    off_y = (s - logo_h * scale) / 2 - 91 * scale

    def dot(cx, cy, r, rgb):
        x0, x1 = int(cx - r), int(cx + r) + 1
        y0, y1 = int(cy - r), int(cy + r) + 1
        for yy in range(max(0, y0), min(s, y1)):
            for xx in range(max(0, x0), min(s, x1)):
                if (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r:
                    i = (yy * s + xx) * 4
                    px[i:i + 4] = bytes((*rgb, 255))

    # 先描边（浅色，略粗），再填充（主题色压上）→ 描边外沿留出浅色细边。
    # 填充游程按缩放后的纵向条带铺满（只画 1px 行会在 scale>1 时留出缝隙）。
    for lx, ly in zip(path_x, path_y):
        dot(lx * scale + off_x, ly * scale + off_y, max(scale * 1.6, ss * 0.6), STROKE)
    for ly, lx0, lx1 in zip(fill_y, fill_x0, fill_x1):
        y = ly * scale + off_y
        x0, x1 = int(lx0 * scale + off_x), int(lx1 * scale + off_x) + 1
        for yy in range(max(0, int(y)), min(s, int(y + scale) + 1)):
            for xx in range(max(0, x0), min(s, x1)):
                i = (yy * s + xx) * 4
                px[i:i + 4] = bytes((*FILL, 255))

    # 4x 盒式降采样
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            acc = [0, 0, 0, 0]
            for dy in range(ss):
                for dx in range(ss):
                    i = ((y * ss + dy) * s + (x * ss + dx)) * 4
                    for c in range(4):
                        acc[c] += px[i + c]
            o = (y * size + x) * 4
            out[o:o + 4] = bytes(a // (ss * ss) for a in acc)
    return out


def main():
    text = LOGO_H.read_text(encoding="utf-8")
    path_x = read_array(text, "logo_path_x")
    path_y = read_array(text, "logo_path_y")
    fill_y = read_array(text, "logo_fill_y")
    fill_x0 = read_array(text, "logo_fill_x0")
    fill_x1 = read_array(text, "logo_fill_x1")
    for dpi, size in SIZES:
        out = RES_DIR / f"mipmap-{dpi}" / "ic_launcher.png"
        write_png(out, size, size, render(size, path_x, path_y, fill_y, fill_x0, fill_x1))
        print(f"wrote {out.relative_to(ROOT)} ({size}x{size})")


if __name__ == "__main__":
    main()
