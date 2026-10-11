#!/usr/bin/env python3
"""Generate the compact Wio Terminal font chain.

The SAMD51 application flash is almost full, so Wio keeps LVGL's built-in
Montserrat faces for ASCII and icons. Latin-1/UI punctuation are small
extensions, while one 12px compressed CJK bitmap set is shared by all text
roles. The primary CJK set is the union of UI literals and the 512 most
frequent characters; ranks 513-1024 use a smaller 10px fallback set, while
ranks 1025-2048 use an 8px final fallback set. Mechanical/electronics/
3D-printing vocabulary is promoted to at least 10px, with the same number of
low-priority ordinary characters demoted to keep the flash budget stable.
"""

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CONV = ROOT / "tools" / "fontgen" / "node_modules" / "lv_font_conv" / "lv_font_conv.js"
LATO = ROOT / "tools" / "fontgen" / "fonts" / "Lato-Regular.ttf"
SIMHEI = Path(r"C:\Windows\Fonts\simhei.ttf")
MONTSERRAT = ROOT / "third_party" / "lvgl" / "scripts" / "built_in_font" / "Montserrat-Medium.ttf"
FONTAWESOME = ROOT / "third_party" / "lvgl" / "scripts" / "built_in_font" / "FontAwesome5-Solid+Brands+Regular.woff"
ASSETS = ROOT / "src" / "ui" / "assets"
COMMON_ZH = ROOT / "tools" / "fontgen" / "common_zh_512.txt"
COMMON_ZH_EXTRA = ROOT / "tools" / "fontgen" / "common_zh_512_extra.txt"
COMMON_ZH_TINY = ROOT / "tools" / "fontgen" / "common_zh_1024_extra.txt"
DOMAIN_ZH = ROOT / "tools" / "fontgen" / "domain_zh.txt"
SIZES = (12, 14, 16)
EXTRA_SYMBOLS = "…→↔≥"
MONTSERRAT_METRICS = {
    12: (15, 3),
    14: (16, 3),
    16: (18, 3),
}
MONTSERRAT_SIZES = (12, 14, 16, 20)
LV_SYMBOL_CODEPOINTS = (
    "61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,"
    "61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,"
    "61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,"
    "61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,"
    "61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,"
    "62018,62019,62020,62087,62099,62189,62212,62810,63426,63650"
)


def cjk_font_object(name: str, fallback: str, line_height: int, base_line: int) -> str:
    return f"""
const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -3,
    .underline_thickness = 1,
    .dsc = &font_dsc,
    .fallback = &{fallback},
    .user_data = NULL,
}};
"""


def _c_array(values: list[int], digits: int, per_line: int = 16) -> str:
    rows = []
    for start in range(0, len(values), per_line):
        row = values[start:start + per_line]
        rows.append("    " + ", ".join(f"0x{value:0{digits}X}" for value in row) + ",")
    return "\n".join(rows)


def _generated_array(text: str, name: str) -> list[int]:
    match = re.search(
        rf"static .*?\b{name}\[\]\s*=\s*\{{(.*?)\n\}};",
        text,
        flags=re.DOTALL,
    )
    if not match:
        raise RuntimeError(f"generated array not found: {name}")
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL)
    return [int(token, 0) for token in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]


def compact_tiny_font(path: Path) -> int:
    """Replace lv_font_conv's 8-byte/glyph descriptors with packed metadata."""
    generated = path.read_text(encoding="utf-8")
    bitmap = _generated_array(generated, "glyph_bitmap")
    unicode_offsets = _generated_array(generated, "unicode_list_0")
    range_match = re.search(r"\.range_start\s*=\s*(\d+)", generated)
    if not range_match:
        raise RuntimeError("generated cmap range_start not found")
    first_codepoint = int(range_match.group(1))
    codepoints = [first_codepoint + offset for offset in unicode_offsets]
    if codepoints != sorted(codepoints) or len(codepoints) != len(set(codepoints)):
        raise RuntimeError("tiny CJK cmap must be sorted and unique")

    descriptor_pattern = re.compile(
        r"\.bitmap_index\s*=\s*(\d+),\s*"
        r"\.adv_w\s*=\s*(\d+),\s*"
        r"\.box_w\s*=\s*(\d+),\s*"
        r"\.box_h\s*=\s*(\d+),\s*"
        r"\.ofs_x\s*=\s*(-?\d+),\s*"
        r"\.ofs_y\s*=\s*(-?\d+)"
    )
    descriptors = [tuple(map(int, values)) for values in descriptor_pattern.findall(generated)]
    if len(descriptors) != len(codepoints) + 1:
        raise RuntimeError("generated glyph descriptor/cmap count mismatch")
    descriptors = descriptors[1:]  # LVGL's reserved glyph id 0
    advances = {descriptor[1] for descriptor in descriptors}
    if len(advances) != 1:
        raise RuntimeError("tiny CJK glyphs must share one advance width")
    advance_px = (advances.pop() + (1 << 3)) >> 4

    bitmap_block_offsets = []
    packed_metrics = []
    expected_bitmap_index = 0
    for glyph_index, (bitmap_index, advance, box_w, box_h, ofs_x, ofs_y) in enumerate(descriptors):
        if not (0 <= bitmap_index <= 0xFFFF):
            raise RuntimeError("tiny CJK bitmap offset exceeds uint16_t")
        if bitmap_index != expected_bitmap_index:
            raise RuntimeError("tiny CJK bitmaps must be packed contiguously")
        if not (0 <= box_w <= 15 and 0 <= box_h <= 15):
            raise RuntimeError("tiny CJK box does not fit packed metrics")
        if not (-8 <= ofs_x <= 7 and -8 <= ofs_y <= 7):
            raise RuntimeError("tiny CJK offset does not fit packed metrics")
        bitmap_bytes = (box_w * box_h + 7) // 8
        if bitmap_index + bitmap_bytes > len(bitmap):
            raise RuntimeError("tiny CJK bitmap descriptor exceeds bitmap storage")
        if glyph_index % 16 == 0:
            bitmap_block_offsets.append(bitmap_index)
        expected_bitmap_index += bitmap_bytes
        packed_metrics.append(
            box_w | (box_h << 4) | ((ofs_x & 0xF) << 8) | ((ofs_y & 0xF) << 12)
        )
    if expected_bitmap_index != len(bitmap):
        raise RuntimeError("tiny CJK bitmap storage has trailing data")

    source = f'''/* Auto-generated by tools/fontgen/gen_wio_fonts.py. */
#include "lvgl.h"

LV_FONT_DECLARE(font_wio_12);
LV_FONT_DECLARE(font_wio_14);
LV_FONT_DECLARE(font_wio_16);

#define WIO_TINY_GLYPH_COUNT {len(codepoints)}u
#define WIO_TINY_BITMAP_BLOCK 16u

typedef struct {{
    const uint8_t * bitmap;
    const uint16_t * codepoints;
    const uint16_t * bitmap_block_offsets;
    const uint16_t * metrics;
    uint16_t count;
}} wio_tiny_font_dsc_t;

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
{_c_array(bitmap, 2)}
}};

static LV_ATTRIBUTE_LARGE_CONST const uint16_t glyph_codepoints[] = {{
{_c_array(codepoints, 4, 12)}
}};

static LV_ATTRIBUTE_LARGE_CONST const uint16_t glyph_bitmap_block_offsets[] = {{
{_c_array(bitmap_block_offsets, 4, 12)}
}};

/* width:4, height:4, signed x-offset:4, signed y-offset:4 */
static LV_ATTRIBUTE_LARGE_CONST const uint16_t glyph_metrics[] = {{
{_c_array(packed_metrics, 4, 12)}
}};

static const wio_tiny_font_dsc_t font_dsc = {{
    .bitmap = glyph_bitmap,
    .codepoints = glyph_codepoints,
    .bitmap_block_offsets = glyph_bitmap_block_offsets,
    .metrics = glyph_metrics,
    .count = WIO_TINY_GLYPH_COUNT,
}};

static int32_t tiny_find_glyph(const wio_tiny_font_dsc_t * dsc, uint32_t codepoint)
{{
    if(codepoint > 0xFFFFu) return -1;
    int32_t lo = 0;
    int32_t hi = (int32_t)dsc->count - 1;
    while(lo <= hi) {{
        int32_t mid = lo + ((hi - lo) >> 1);
        uint16_t candidate = dsc->codepoints[mid];
        if(candidate == codepoint) return mid;
        if(candidate < codepoint) lo = mid + 1;
        else hi = mid - 1;
    }}
    return -1;
}}

static bool tiny_get_glyph_dsc(const lv_font_t * font, lv_font_glyph_dsc_t * out,
                               uint32_t codepoint, uint32_t codepoint_next)
{{
    LV_UNUSED(codepoint_next);
    const wio_tiny_font_dsc_t * dsc = font->dsc;
    int32_t index = tiny_find_glyph(dsc, codepoint);
    if(index < 0) return false;

    uint16_t metrics = dsc->metrics[index];
    int8_t ofs_x = (int8_t)((metrics >> 8) & 0x0Fu);
    int8_t ofs_y = (int8_t)((metrics >> 12) & 0x0Fu);
    if(ofs_x & 0x08) ofs_x -= 16;
    if(ofs_y & 0x08) ofs_y -= 16;

    out->adv_w = {advance_px};
    out->box_w = metrics & 0x0Fu;
    out->box_h = (metrics >> 4) & 0x0Fu;
    out->ofs_x = ofs_x;
    out->ofs_y = ofs_y;
    out->stride = 0;
    out->format = LV_FONT_GLYPH_FORMAT_A1;
    out->is_placeholder = 0;
    out->gid.index = (uint32_t)index;
    return true;
}}

static const void * tiny_get_glyph_bitmap(lv_font_glyph_dsc_t * glyph, lv_draw_buf_t * draw_buf)
{{
    const wio_tiny_font_dsc_t * dsc = glyph->resolved_font->dsc;
    uint32_t index = glyph->gid.index;
    uint32_t first = index & ~(WIO_TINY_BITMAP_BLOCK - 1u);
    uint32_t bitmap_offset = dsc->bitmap_block_offsets[index / WIO_TINY_BITMAP_BLOCK];
    for(uint32_t i = first; i < index; i++) {{
        uint16_t metrics = dsc->metrics[i];
        uint32_t box_w = metrics & 0x0Fu;
        uint32_t box_h = (metrics >> 4) & 0x0Fu;
        bitmap_offset += (box_w * box_h + 7u) >> 3;
    }}
    const uint8_t * input = dsc->bitmap + bitmap_offset;
    if(glyph->req_raw_bitmap) return input;
    if(draw_buf == NULL) return NULL;

    uint8_t * output = draw_buf->data;
    uint32_t output_stride = lv_draw_buf_width_to_stride(glyph->box_w, LV_COLOR_FORMAT_A8);
    uint32_t bit = 0;
    for(uint32_t y = 0; y < glyph->box_h; y++) {{
        for(uint32_t x = 0; x < glyph->box_w; x++, bit++) {{
            output[y * output_stride + x] = (input[bit >> 3] & (0x80u >> (bit & 7u))) ? 0xFF : 0x00;
        }}
    }}
    lv_draw_buf_flush_cache(draw_buf, NULL);
    return draw_buf;
}}

#define WIO_TINY_FONT_OBJECT(name_, fallback_, height_, baseline_) \\
    const lv_font_t name_ = {{ \\
        .get_glyph_dsc = tiny_get_glyph_dsc, \\
        .get_glyph_bitmap = tiny_get_glyph_bitmap, \\
        .release_glyph = NULL, \\
        .line_height = (height_), \\
        .base_line = (baseline_), \\
        .subpx = LV_FONT_SUBPX_NONE, \\
        .kerning = LV_FONT_KERNING_NONE, \\
        .static_bitmap = 1, \\
        .underline_position = -3, \\
        .underline_thickness = 1, \\
        .dsc = &font_dsc, \\
        .fallback = &(fallback_), \\
        .user_data = NULL, \\
    }}

WIO_TINY_FONT_OBJECT(font_wio_cjk_tiny, font_wio_12, 10, 2);
WIO_TINY_FONT_OBJECT(font_wio_cjk_tiny_12, font_wio_12, 15, 3);
WIO_TINY_FONT_OBJECT(font_wio_cjk_tiny_14, font_wio_14, 16, 3);
WIO_TINY_FONT_OBJECT(font_wio_cjk_tiny_16, font_wio_16, 18, 3);
'''
    path.write_text(source, encoding="utf-8")
    return len(codepoints)


def generate_cjk() -> None:
    sys.path.insert(0, str(ROOT / "tools" / "fontgen"))
    from gen_fonts import collect_chars

    common = COMMON_ZH.read_text(encoding="utf-8").strip()
    if len(common) != 512 or len(set(common)) != 512:
        raise RuntimeError("common_zh_512.txt must contain exactly 512 unique characters")
    common_extra = COMMON_ZH_EXTRA.read_text(encoding="utf-8").strip()
    if len(common_extra) != 512 or len(set(common_extra)) != 512:
        raise RuntimeError("common_zh_512_extra.txt must contain exactly 512 unique characters")
    if set(common) & set(common_extra):
        raise RuntimeError("the two common Chinese character tiers must be disjoint")
    common_tiny = COMMON_ZH_TINY.read_text(encoding="utf-8").strip()
    if len(common_tiny) != 1024 or len(set(common_tiny)) != 1024:
        raise RuntimeError("common_zh_1024_extra.txt must contain exactly 1024 unique characters")
    if (set(common) | set(common_extra)) & set(common_tiny):
        raise RuntimeError("the three common Chinese character tiers must be disjoint")

    # Comments explain the list but are not themselves coverage requests.
    domain_text = "\n".join(
        line for line in DOMAIN_ZH.read_text(encoding="utf-8").splitlines()
        if not line.lstrip().startswith("#")
    )
    domain_chars = "".join(dict.fromkeys(
        ch for ch in domain_text if 0x3400 <= ord(ch) <= 0x9FFF
    ))

    ui_chars = collect_chars()
    cjk = "".join(sorted({ch for ch in ui_chars + common if ord(ch) >= 0x2E80}))
    # UI literals always stay in the clearer 12px tier.  Do not duplicate any
    # of them in the compact tiers even if they also occur in a frequency list.
    cjk_set = set(cjk)
    ranked_extra = [ch for ch in common_extra if ch not in cjk_set]
    domain_extra = [ch for ch in domain_chars if ch not in cjk_set]
    domain_extra_set = set(domain_extra)

    # Every mechanical/electronics/3D-printing character is at least 10px.
    # Keep the tier size stable by demoting the same number of lowest-priority
    # non-domain frequency characters to the 8px tier.
    target_extra_count = max(len(ranked_extra), len(domain_extra))
    ordinary_slots = target_extra_count - len(domain_extra)
    ordinary_kept = set(list(ch for ch in ranked_extra if ch not in domain_extra_set)[:ordinary_slots])
    cjk_extra = "".join(
        dict.fromkeys(
            [ch for ch in ranked_extra if ch in domain_extra_set or ch in ordinary_kept]
            + domain_extra
        )
    )
    if len(cjk_extra) != target_extra_count:
        raise RuntimeError("failed to keep the second CJK tier at its target size")
    demoted_common = "".join(ch for ch in ranked_extra if ch not in set(cjk_extra))
    covered = cjk_set | set(cjk_extra)
    cjk_tiny = "".join(dict.fromkeys(
        ch for ch in common_tiny + demoted_common + domain_chars if ch not in covered
    ))
    missing_domain = set(domain_chars) - covered
    if missing_domain:
        raise RuntimeError(
            f"domain characters fell below the 10px tier: {''.join(sorted(missing_domain))}"
        )
    missing_targets = set("套壳块孔洞") - (covered | set(cjk_tiny))
    if missing_targets:
        raise RuntimeError(f"required filename characters missing: {''.join(sorted(missing_targets))}")
    out = ASSETS / "font_wio_cjk.c"
    subprocess.run([
        "node", str(CONV),
        "--font", str(SIMHEI),
        "--size", "12",
        "--bpp", "1",
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--symbols", cjk,
        "-o", str(out),
    ], cwd=ROOT, check=True)

    text = out.read_text(encoding="utf-8")
    declarations = """LV_FONT_DECLARE(font_wio_12);
LV_FONT_DECLARE(font_wio_14);
LV_FONT_DECLARE(font_wio_16);
LV_FONT_DECLARE(font_wio_cjk_extra_12);
LV_FONT_DECLARE(font_wio_cjk_extra_14);
LV_FONT_DECLARE(font_wio_cjk_extra_16);

"""
    marker = "#ifndef FONT_WIO_CJK"
    if marker not in text:
        raise RuntimeError(f"font guard not found in {out}")
    text = text.replace(marker, declarations + marker, 1)

    variants = "".join([
        cjk_font_object("font_wio_cjk_12", "font_wio_cjk_extra_12", 15, 3),
        cjk_font_object("font_wio_cjk_14", "font_wio_cjk_extra_14", 16, 3),
        cjk_font_object("font_wio_cjk_16", "font_wio_cjk_extra_16", 18, 3),
    ])
    end_marker = "#endif /*#if FONT_WIO_CJK*/"
    if end_marker not in text:
        raise RuntimeError(f"font end guard not found in {out}")
    out.write_text(text.replace(end_marker, variants + "\n" + end_marker, 1), encoding="utf-8")
    print(f"[gen] {out.relative_to(ROOT)}: {len(cjk)} UI/common CJK glyphs, shared 12px/1bpp")

    extra_out = ASSETS / "font_wio_cjk_extra.c"
    subprocess.run([
        "node", str(CONV),
        "--font", str(SIMHEI),
        "--size", "10",
        "--bpp", "1",
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--symbols", cjk_extra,
        "-o", str(extra_out),
    ], cwd=ROOT, check=True)

    extra_text = extra_out.read_text(encoding="utf-8")
    extra_declarations = """LV_FONT_DECLARE(font_wio_cjk_tiny_12);
LV_FONT_DECLARE(font_wio_cjk_tiny_14);
LV_FONT_DECLARE(font_wio_cjk_tiny_16);

"""
    extra_marker = "#ifndef FONT_WIO_CJK_EXTRA"
    if extra_marker not in extra_text:
        raise RuntimeError(f"font guard not found in {extra_out}")
    extra_text = extra_text.replace(extra_marker, extra_declarations + extra_marker, 1)
    extra_variants = "".join([
        cjk_font_object("font_wio_cjk_extra_12", "font_wio_cjk_tiny_12", 15, 3),
        cjk_font_object("font_wio_cjk_extra_14", "font_wio_cjk_tiny_14", 16, 3),
        cjk_font_object("font_wio_cjk_extra_16", "font_wio_cjk_tiny_16", 18, 3),
    ])
    extra_end_marker = "#endif /*#if FONT_WIO_CJK_EXTRA*/"
    if extra_end_marker not in extra_text:
        raise RuntimeError(f"font end guard not found in {extra_out}")
    extra_out.write_text(
        extra_text.replace(extra_end_marker, extra_variants + "\n" + extra_end_marker, 1),
        encoding="utf-8",
    )
    promoted_count = len(set(domain_extra) - set(ranked_extra))
    print(
        f"[gen] {extra_out.relative_to(ROOT)}: {len(cjk_extra)} CJK glyphs at 10px/1bpp "
        f"({promoted_count} domain promoted, {len(demoted_common)} ordinary demoted)"
    )

    tiny_out = ASSETS / "font_wio_cjk_tiny.c"
    subprocess.run([
        "node", str(CONV),
        "--font", str(SIMHEI),
        "--size", "8",
        "--bpp", "1",
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--symbols", cjk_tiny,
        "-o", str(tiny_out),
    ], cwd=ROOT, check=True)

    compact_count = compact_tiny_font(tiny_out)
    if compact_count != len(cjk_tiny):
        raise RuntimeError("compact tiny font lost glyphs")
    print(
        f"[gen] {tiny_out.relative_to(ROOT)}: {len(cjk_tiny)} frequency/domain CJK glyphs, "
        "shared 8px/1bpp"
    )


def generate_montserrat() -> None:
    """Build the four LVGL base faces without their large kerning tables."""
    for source in (MONTSERRAT, FONTAWESOME):
        if not source.is_file():
            raise RuntimeError(f"LVGL built-in font source not found: {source}")
    for size in MONTSERRAT_SIZES:
        out = ASSETS / f"font_wio_montserrat_{size}.c"
        subprocess.run([
            "node", str(CONV),
            "--font", str(MONTSERRAT),
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "--range", "0x20-0x7F,0xB0,0x2022",
            "--font", str(FONTAWESOME),
            "--range", LV_SYMBOL_CODEPOINTS,
            "--no-compress",
            "--no-prefilter",
            "--no-kerning",
            "--lv-font-name", f"lv_font_montserrat_{size}",
            "-o", str(out),
        ], cwd=ROOT, check=True)
        # The exported symbol intentionally keeps LVGL's stock name, but its
        # generated compile guard must not reuse LV_FONT_MONTSERRAT_N: that
        # option is disabled so LVGL does not compile its kerning-heavy copy.
        text = out.read_text(encoding="utf-8")
        text = text.replace(
            f"LV_FONT_MONTSERRAT_{size}",
            f"FONT_WIO_MONTSERRAT_{size}",
        )
        out.write_text(text, encoding="utf-8")
        print(f"[gen] {out.relative_to(ROOT)}: Montserrat {size}px + LV symbols, no kerning")


def main() -> int:
    if not CONV.is_file():
        raise SystemExit(f"lv_font_conv not installed: {CONV}")
    if not SIMHEI.is_file():
        raise SystemExit(f"CJK font not found: {SIMHEI}")

    generate_montserrat()

    for size in SIZES:
        out = ASSETS / f"font_wio_{size}.c"
        subprocess.run([
            "node", str(CONV),
            "--font", str(LATO),
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "--range", "0xA0-0xFF",
            "--symbols", EXTRA_SYMBOLS,
            "--no-compress",
            # Lato's class kerning table is about 3 KiB per size.  At 12-16px
            # the visual difference is negligible, while dropping all three
            # tables leaves useful headroom in the 496 KiB application flash.
            "--no-kerning",
            "-o", str(out),
        ], cwd=ROOT, check=True)

        text = out.read_text(encoding="utf-8")
        needle = "    .fallback = NULL,"
        replacement = f"    .fallback = &lv_font_montserrat_{size},"
        if needle not in text:
            raise RuntimeError(f"fallback initializer not found in {out}")
        out.write_text(text.replace(needle, replacement, 1), encoding="utf-8")
        line_height, base_line = MONTSERRAT_METRICS[size]
        text = out.read_text(encoding="utf-8")
        text = text.replace(
            next(line for line in text.splitlines() if ".line_height =" in line),
            f"    .line_height = {line_height},          /* Match fallback font metrics */",
            1,
        ).replace(
            next(line for line in text.splitlines() if ".base_line =" in line),
            f"    .base_line = {base_line},             /* Match fallback font metrics */",
            1,
        )
        out.write_text(text, encoding="utf-8")
        print(f"[gen] {out.relative_to(ROOT)} -> Montserrat {size}px fallback")

    generate_cjk()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
