#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
字体字符集 = GB2312 一级/二级汉字全表（6763 字，覆盖常见简体）+ src/ui 源码字面量扫描
（补齐 ° ≥ … 等符号、繁体/法语/意语界面用词）。
调用 lv_font_conv 生成 font_cjk_14/16/28/32.c。

以后 UI 里新增中文/特殊字符后，只要重跑一次本脚本即可，不会再出现方框：
    python tools/fontgen/gen_fonts.py

默认字体源为 Windows 自带的 simhei.ttf，可用 --font 指定其它 ttf/otf。
"""

import argparse
import subprocess
import sys
from pathlib import Path

# Windows 控制台默认 GBK，避免打印中文路径/字符时乱码或报错
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

ROOT = Path(__file__).resolve().parents[2]
UI_DIR = ROOT / "src" / "ui"
CONV_JS = ROOT / "tools" / "fontgen" / "node_modules" / "lv_font_conv" / "lv_font_conv.js"
CHARSET_OUT = ROOT / "tmp" / "cjk_chars.txt"
DEFAULT_FONT = r"C:\Windows\Fonts\simhei.ttf"
# 西文兜底字体：simhei 只覆盖拼音用拉丁字母（é/è/à/ê/ù…），缺 ç/ô/É 等，
# Latin-1 补充区（0xA0-0xFF）整体由该字体补齐，避免法语/意语出现方框。
DEFAULT_LATIN_FONT = r"C:\Windows\Fonts\arial.ttf"
SIZES = (14, 16, 28, 32)   # 14/16: 320x240 基准；28/32: 800x480 双倍档


def iter_literals(path: Path):
    """简易 C 词法状态机：只产出字符串/字符字面量的原始内容，跳过注释。"""
    text = path.read_text(encoding="utf-8")
    i, n = 0, len(text)
    NORMAL, STRING, CHAR, LINE_CMT, BLOCK_CMT = range(5)
    state = NORMAL
    start = 0
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == NORMAL:
            if c == "/" and nxt == "/":
                state = LINE_CMT; i += 2; continue
            if c == "/" and nxt == "*":
                state = BLOCK_CMT; i += 2; continue
            if c == '"':
                state = STRING; start = i + 1; i += 1; continue
            if c == "'":
                state = CHAR; start = i + 1; i += 1; continue
            i += 1
        elif state in (STRING, CHAR):
            if c == "\\":
                i += 2; continue
            if (state == STRING and c == '"') or (state == CHAR and c == "'"):
                yield text[start:i], state == CHAR
                state = NORMAL
            i += 1
        elif state == LINE_CMT:
            if c == "\n":
                state = NORMAL
            i += 1
        else:  # BLOCK_CMT
            if c == "*" and nxt == "/":
                state = NORMAL; i += 2; continue
            i += 1


def decode_literal(raw: str) -> bytes:
    """把字面量内容按 C 转义规则解码成字节串（\\xNN 序列正好拼回 UTF-8）。"""
    out = bytearray()
    i, n = 0, len(raw)
    while i < n:
        c = raw[i]
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        i += 1
        if i >= n:
            break
        e = raw[i]
        i += 1
        simple = {"n": b"\n", "r": b"\r", "t": b"\t", "0": b"\0",
                  "\\": b"\\", '"': b'"', "'": b"'"}
        if e in simple:
            out += simple[e]
        elif e == "x":
            j = i
            while j < n and raw[j] in "0123456789abcdefABCDEF":
                j += 1
            if j > i:
                out.append(int(raw[i:j], 16) & 0xFF)
            i = j
        else:
            out += e.encode("utf-8")
    return bytes(out)


def gb2312_hanzi() -> str:
    """GB2312 汉字区（0xB0A1-0xF7FE）全表 6763 字：文件名/SSID 等动态内容不再出方框。"""
    chars = []
    for row in range(0xB0, 0xF8):
        # errors="ignore"：0xD7FA-0xD7FE 等个别码位未分配，直接跳过
        chars.append(bytes(b for c in range(0xA1, 0xFF) for b in (row, c))
                     .decode("gb2312", errors="ignore"))
    return "".join(chars)


def collect_chars() -> str:
    chars = set()
    for path in sorted(UI_DIR.rglob("*.[ch]")):
        for raw, is_char in iter_literals(path):
            text = decode_literal(raw).decode("utf-8", errors="ignore")
            for ch in text:
                if ord(ch) > 127:
                    chars.add(ch)
    return "".join(sorted(chars))


def gen_font(font: str, latin_font: str, size: int, symbols: str, compress: bool, suffix: str = "") -> Path:
    out = ROOT / "src" / "ui" / "assets" / f"font_cjk_{size}{suffix}.c"
    cmd = [
        "node", str(CONV_JS),
        "--font", font,
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--range", "0x20-0x7F",
        "--symbols", symbols,
        "--font", latin_font,
        "--range", "0xA0-0xFF",
        "-o", str(out),
    ]
    # 全部尺寸主版本不压缩：JC8048W550 滑动列表每帧重绘数百个字形，
    # LVGL builtin 字体路径无字形缓存，RLE 每字每帧现场解码开销巨大（实测滑动仅 9.6fps）。
    # 14/16 额外产出 _cmp 压缩变体给 CYD（4MB flash 分区放不下不压缩版；
    # CYD 屏小字少，解压开销不构成瓶颈）。未引用的变体被链接器 --gc-sections 裁掉。
    if not compress:
        cmd.append("--no-compress")
    print(f"[gen] size={size} compress={compress} -> {out.relative_to(ROOT)}")
    subprocess.run(cmd, check=True, cwd=ROOT)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 LVGL CJK 子集字体")
    ap.add_argument("--font", default=DEFAULT_FONT, help="ttf/otf 主字体源路径（CJK+ASCII）")
    ap.add_argument("--font-latin", default=DEFAULT_LATIN_FONT,
                    help="西文兜底字体源路径（Latin-1 补充区 0xA0-0xFF）")
    args = ap.parse_args()

    if not Path(args.font).is_file():
        print(f"字体源不存在: {args.font}", file=sys.stderr)
        return 1
    if not Path(args.font_latin).is_file():
        print(f"西文字体源不存在: {args.font_latin}", file=sys.stderr)
        return 1
    if not CONV_JS.is_file():
        print(f"lv_font_conv 未安装（缺 {CONV_JS}），请先在 tools/fontgen 下 npm install",
              file=sys.stderr)
        return 1

    lit = collect_chars()
    gb = gb2312_hanzi()
    symbols = "".join(sorted(set(lit) | set(gb)))
    CHARSET_OUT.parent.mkdir(exist_ok=True)
    CHARSET_OUT.write_text(symbols, encoding="utf-8")
    print(f"[scan] GB2312 全表 {len(gb)} + 源码扫描，合计 {len(symbols)} 个非 ASCII 字符"
          f" -> {CHARSET_OUT.relative_to(ROOT)}")

    for size in SIZES:
        # 主版本全尺寸不压缩（见 gen_font 注释）；14/16 另出 _cmp 压缩变体给 CYD
        gen_font(args.font, args.font_latin, size, symbols, compress=False)
        if size <= 16:
            gen_font(args.font, args.font_latin, size, symbols, compress=True, suffix="_cmp")

    # _min 最小子集变体（仅 UI 源码字面量，几百字）：JC8048W550 排障/降级用。
    # 全表 5.4MB 字形在 flash，渲染时走 XIP cache 读；表越大 cache 局部性越差，
    # 文本渲染的 flash 突发读取在 MSPI 上与 EDMA 扫描争抢（滑动抽动嫌疑之一）。
    # 代价：文件名/SSID 里表外汉字会显示方框。
    for size in (28, 32):
        gen_font(args.font, args.font_latin, size, lit, compress=False, suffix="_min")
    print("[done] 字体生成完成，重新编译固件/桌面端即可生效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
