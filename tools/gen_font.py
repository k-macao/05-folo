#!/usr/bin/env python3
"""Generate an LVGL 9 text font (4 bpp, PLAIN bitmap) from a TrueType/OpenType face.

Why this exists
---------------
``lv_font_conv`` (the usual LVGL font generator) produced corrupt bitmaps for us
under current Node.js, so this script rasterises with Pillow/FreeType and emits
LVGL's ``lv_font_fmt_txt`` structures directly.  It then **re-parses the C file it
just wrote**, resolves every code point through ``cmaps[]`` (including binary search
over ``SPARSE_TINY`` tables), decodes every glyph exactly the way
``lv_font_get_bitmap_fmt_txt()`` does and compares the result pixel by pixel with
the rasterisation.  A mismatch fails the run instead of shipping garbled text.

Output
------
``--out`` gets a C file plus a header declaring the font.  The code point
inventory is written next to it (``*.chars``) so a later change can be diffed and
so regenerating never silently drops a glyph another build still uses.

Usage
-----
    python3 tools/gen_font.py \
        --font /path/NotoSansMonoCJKsc-Regular.otf --size 12 --mono \
        --name crt_font_12 --out main/fonts/crt_font_12.c

The source font is never committed: record family, licence, download source and
the command above in the asset documentation instead (SIL OFL 1.1 for Noto).
"""

from __future__ import annotations

import argparse
import bisect
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageFont
except ImportError:  # pragma: no cover - host check imports this module without Pillow
    Image = None      # type: ignore[assignment]
    ImageFont = None  # type: ignore[assignment]


# --------------------------------------------------------------------------- #
# Character inventory
# --------------------------------------------------------------------------- #
ASCII = [chr(c) for c in range(0x20, 0x7F)]

# Punctuation / symbols that matter to a terminal-flavoured UI.  Everything here
# is present in Noto Sans CJK.
EXTRA = (
    "、。，．・：；？！…‥—―～·／＼｜（）〔〕【】《》〈〉「」『』“”‘’"
    "％＋－×÷＝±≤≥≈√∞°′″℃￥＄＃＠＆＊"
    "▲▼◀▶◁▷△▽●○■□◆◇★☆◎※→←↑↓↔"
    "①②③④⑤⑥⑦⑧⑨⑩⑪⑫⑬⑭⑮"
    "✓✔✗✘⚠↑↓"
)


# 这些码点在 intel_normalize() 里被替换成 ASCII 近似（中文字体常缺），
# 因此不需要出现在位图字体里。
FALLBACK_MAPPED = set("—°′✓")


def gb2312_charset() -> list[str]:
    """GB2312 symbols (rows A1-A9) plus level-1 hanzi (rows B0-D7, 3755 chars)."""
    out: list[str] = []
    for hi in range(0xA1, 0xFE):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if 0xA1 <= hi <= 0xA9 or 0xB0 <= hi <= 0xD7:
                # GB2312 rows A1-A9 also carry Latin-1 letters and pinyin marks;
                # the UI never uses them and they would break half/full-width layout.
                if ord(ch) < 0x100 or 0xFF00 <= ord(ch) <= 0xFFEF:
                    continue
                out.append(ch)
    return out


def build_inventory() -> list[int]:
    chars: set[str] = set(ASCII)
    chars.update(EXTRA)
    chars.update(gb2312_charset())
    chars.discard("\n")
    chars.discard("\r")
    return sorted({ord(c) for c in chars})


# --------------------------------------------------------------------------- #
# Rasterisation
# --------------------------------------------------------------------------- #
class Glyph:
    __slots__ = ("cp", "adv", "w", "h", "ofs_x", "ofs_y", "levels", "bitmap")

    def __init__(self, cp: int) -> None:
        self.cp = cp
        self.adv = 0
        self.w = 0
        self.h = 0
        self.ofs_x = 0
        self.ofs_y = 0
        self.levels: list[int] = []
        self.bitmap = b""


def pack_4bpp(g: Glyph) -> bytes:
    """Pack a glyph the way LVGL's PLAIN 4 bpp reader expects.

    Rows are a continuous nibble stream (no per-row padding); every glyph starts
    on a byte boundary and the high nibble is the left-most pixel.
    """
    out = bytearray()
    acc = 0
    half = False
    for v in g.levels:
        if half:
            out.append((acc << 4) | (v & 0xF))
            half = False
        else:
            acc = v & 0xF
            half = True
    if half:
        out.append(acc << 4)
    return bytes(out)


def rasterise(font: ImageFont.FreeTypeFont, cp: int, crop_top: int,
              line_height: int) -> Glyph:
    ch = chr(cp)
    g = Glyph(cp)
    g.adv = int(round(font.getlength(ch) * 16))
    if cp == 0x20:  # keep the space advance, emit an empty box
        return g

    x0, y0, x1, y1 = font.getbbox(ch)
    if x1 <= x0 or y1 <= y0:
        return g

    mask = font.getmask(ch, mode="L")
    # A mask can be wider than the bbox for overhanging glyphs; crop to the bbox.
    img = Image.new("L", (mask.size[0], mask.size[1]))
    img.putdata(list(mask))
    if img.size != (x1 - x0, y1 - y0):
        img = img.crop((0, 0, x1 - x0, y1 - y0))

    g.w, g.h = img.size
    g.ofs_x = x0
    g.ofs_y = y0 - crop_top
    if g.w > 255 or g.h > 255:
        raise SystemExit(f"glyph U+{cp:04X} too large ({g.w}x{g.h})")
    if not (-128 <= g.ofs_x <= 127 and -128 <= g.ofs_y <= 127):
        raise SystemExit(f"glyph U+{cp:04X} offset out of int8 range")

    levels = [min(15, (v * 15 + 127) // 255) for v in img.tobytes()]
    if g.ofs_y + g.h > line_height:
        raise SystemExit(
            f"glyph U+{cp:04X} overflows the {line_height}px line "
            f"(ofs_y {g.ofs_y} + h {g.h})")
    g.levels = levels
    g.bitmap = pack_4bpp(g)
    return g


# --------------------------------------------------------------------------- #
# C emission
# --------------------------------------------------------------------------- #
def c_array(name: str, ctype: str, values: list[int], per_line: int,
            fmt: str = "0x{:02X}") -> str:
    lines = []
    for i in range(0, len(values), per_line):
        chunk = ", ".join(fmt.format(v) for v in values[i:i + per_line])
        lines.append("    " + chunk + ",")
    return f"static const {ctype} {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def build_cmaps_section(code_points: list[int]) -> str:
    """Emit CHARACTER MAPPING arrays + cmaps[] for sorted code_points (glyph_id = 1..N)."""
    assert code_points == sorted(code_points), "code_points must be sorted"
    ascii_cps = list(range(0x20, 0x7F))
    assert code_points[:len(ascii_cps)] == ascii_cps, (
        "ASCII 0x20..0x7E must be complete and contiguous at glyph ids 1..95"
    )

    # Every code point after 0x7E (including 0x80..0xFF such as ±, ·, ×, ÷!)
    # MUST be mapped with its exact 1-based glyph_dsc index; skipping any range
    # shifts every subsequent glyph id in SPARSE_TINY lookup.
    rest: list[tuple[int, int]] = [
        (idx + 1, cp) for idx, cp in enumerate(code_points) if cp > 0x7E
    ]

    # Partition into SPARSE_TINY chunks whose span fits in uint16_t range_length (<= 0xFFFF).
    chunks: list[list[tuple[int, int]]] = []
    for gid, cp in rest:
        if not chunks or (cp - chunks[-1][0][1]) >= 0xFFFF:
            chunks.append([(gid, cp)])
        else:
            chunks[-1].append((gid, cp))

    src: list[str] = []
    src.append("/*-----------------")
    src.append(" *  CHARACTER MAPPING")
    src.append(" *----------------*/")
    src.append("/* ASCII: 0x20..0x7E, glyph ids 1..95 (FORMAT0_TINY). */")

    for ci, chunk in enumerate(chunks, start=1):
        base_cp = chunk[0][1]
        offsets = [cp - base_cp for _, cp in chunk]
        src.append(f"static const uint16_t unicode_list_{ci}[] = {{")
        for i in range(0, len(offsets), 10):
            src.append("    " + ", ".join("0x%04X" % v for v in offsets[i:i + 10]) + ",")
        src.append("};")
        src.append("")

    src.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    cmap_entries: list[str] = [
        "    {\n"
        "        .range_start = 0x20, .range_length = 95, .glyph_id_start = 1,\n"
        "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,\n"
        "        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n"
        "    }"
    ]
    for ci, chunk in enumerate(chunks, start=1):
        first_gid, base_cp = chunk[0]
        last_cp = chunk[-1][1]
        range_len = last_cp - base_cp + 1
        assert 1 <= range_len <= 0xFFFF
        cmap_entries.append(
            "    {\n"
            f"        .range_start = 0x{base_cp:02X}, .range_length = {range_len}, "
            f".glyph_id_start = {first_gid},\n"
            f"        .unicode_list = unicode_list_{ci}, .glyph_id_ofs_list = NULL,\n"
            f"        .list_length = {len(chunk)},\n"
            "        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
            "    }"
        )
    src.append(",\n".join(cmap_entries))
    src.append("};")
    return "\n".join(src)


def emit(font_name: str, glyphs: list[Glyph], line_height: int, base_line: int,
         size: int, source_note: str) -> tuple[str, str]:
    header = f"""/* Generated by tools/gen_font.py -- do not edit by hand.
 *
 * {source_note}
 * Size {size}px, 4 bpp, PLAIN bitmaps, line_height {line_height}, base_line {base_line}.
 */
#pragma once

#include "lvgl.h"

LV_FONT_DECLARE({font_name});
"""
    hdr_name = f"{font_name}.h"

    bitmap: list[int] = []
    dsc_rows: list[str] = []
    for g in glyphs:
        idx = len(bitmap)
        bitmap.extend(g.bitmap)
        dsc_rows.append(
            "    { .bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, "
            ".ofs_x = %d, .ofs_y = %d }," %
            (idx, g.adv, g.w, g.h, g.ofs_x, g.ofs_y))

    cps = [g.cp for g in glyphs]
    cmaps_section = build_cmaps_section(cps)
    cmap_count = cmaps_section.count(".range_start =")

    src = []
    src.append("/* Generated by tools/gen_font.py -- do not edit by hand. */")
    src.append(f"/* {source_note} */")
    src.append('#include "lvgl.h"')
    src.append(f'#include "{hdr_name}"')
    src.append("")
    src.append("/*-----------------")
    src.append(" *  BITMAP")
    src.append(" *----------------*/")
    src.append(c_array("glyph_bitmap", "uint8_t", bitmap, 16))
    src.append("/*-----------------")
    src.append(" *  GLYPH DESCRIPTORS")
    src.append(" *----------------*/")
    src.append("#if LV_FONT_FMT_TXT_LARGE\n#error \"this font uses the compact glyph descriptor\"\n#endif\n")
    src.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    src.append("    { .bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0,"
               " .ofs_x = 0, .ofs_y = 0 }, /* id 0 is unused */")
    src.extend(dsc_rows)
    src.append("};")
    src.append("")
    src.append(cmaps_section)
    src.append("")
    src.append("/*-----------------")
    src.append(" *  FONT DESCRIPTOR")
    src.append(" *----------------*/")
    src.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    src.append("    .glyph_bitmap = glyph_bitmap,")
    src.append("    .glyph_dsc = glyph_dsc,")
    src.append("    .cmaps = cmaps,")
    src.append("    .kern_dsc = NULL,")
    src.append("    .kern_scale = 0,")
    src.append(f"    .cmap_num = {cmap_count},")
    src.append("    .bpp = 4,")
    src.append("    .kern_classes = 0,")
    src.append("    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,")
    src.append("    .stride = 0,")
    src.append("};")
    src.append("")
    src.append(f"const lv_font_t {font_name} = {{")
    src.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    src.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    src.append(f"    .line_height = {line_height},")
    src.append(f"    .base_line = {base_line},")
    src.append("    .subpx = LV_FONT_SUBPX_NONE,")
    src.append("    .kerning = LV_FONT_KERNING_NONE,")
    src.append("    .underline_position = -2,")
    src.append("    .underline_thickness = 1,")
    src.append("    .dsc = &font_dsc,")
    src.append("    .fallback = NULL,")
    src.append("    .user_data = NULL,")
    src.append("};")
    src.append("")
    return header, "\n".join(src)


# --------------------------------------------------------------------------- #
# Self-verification: decode cmaps[] + glyph_dsc[] + glyph_bitmap[] like LVGL
# --------------------------------------------------------------------------- #
def parse_cmaps_from_c(c_text: str) -> list[dict[str, object]]:
    """Parse unicode_list_* and cmaps[] from generated C source."""
    ulists: dict[str, list[int]] = {}
    for m in re.finditer(r"uint16_t\s+(unicode_list_\d+)\[\]\s*=\s*\{(.*?)\};", c_text, re.S):
        vals = [int(tok, 16) if tok.startswith("0x") else int(tok)
                for tok in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(2))]
        ulists[m.group(1)] = vals

    cm_block = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", c_text, re.S)
    if not cm_block:
        raise SystemExit("verify: cmaps[] missing from C file")

    cmaps: list[dict[str, object]] = []
    for entry in re.finditer(
        r"\.range_start\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*"
        r"\.range_length\s*=\s*(\d+)\s*,\s*"
        r"\.glyph_id_start\s*=\s*(\d+)\s*,\s*"
        r"\.unicode_list\s*=\s*(\w+)\s*,\s*"
        r"\.glyph_id_ofs_list\s*=\s*(\w+)\s*,\s*"
        r"\.list_length\s*=\s*(\d+)\s*,\s*"
        r"\.type\s*=\s*(\w+)",
        cm_block.group(1),
    ):
        rs_s, rl_s, gis_s, ulist_name, ofs_name, ll_s, ctype = entry.groups()
        rs = int(rs_s, 16) if rs_s.startswith("0x") else int(rs_s)
        rl = int(rl_s)
        gis = int(gis_s)
        ll = int(ll_s)
        if not (0 < rl <= 0xFFFF):
            raise SystemExit(f"verify: cmap range_length {rl} overflows uint16_t")
        ulist = None if ulist_name == "NULL" else ulists.get(ulist_name)
        if ulist_name != "NULL" and (ulist is None or len(ulist) != ll):
            raise SystemExit(f"verify: {ulist_name} length mismatch")
        if ulist is not None and ulist != sorted(ulist):
            raise SystemExit(f"verify: {ulist_name} is not sorted for binary search")
        if ofs_name != "NULL":
            raise SystemExit(f"verify: unsupported glyph_id_ofs_list {ofs_name}")
        cmaps.append({
            "range_start": rs,
            "range_length": rl,
            "glyph_id_start": gis,
            "unicode_list": ulist,
            "type": ctype,
        })

    dsc_num_m = re.search(r"\.cmap_num\s*=\s*(\d+)", c_text)
    if not dsc_num_m or int(dsc_num_m.group(1)) != len(cmaps):
        raise SystemExit("verify: font_dsc.cmap_num does not match cmaps[] count")
    return cmaps


def lvgl_lookup_glyph_id(cmaps: list[dict[str, object]], cp: int) -> int:
    """Replicate lv_font_get_glyph_dsc_fmt_txt() code point -> glyph_id lookup."""
    for cm in cmaps:
        rs = int(cm["range_start"])   # type: ignore[arg-type]
        rl = int(cm["range_length"])  # type: ignore[arg-type]
        if cp < rs or cp >= rs + rl:
            continue
        rcp = cp - rs
        gis = int(cm["glyph_id_start"])  # type: ignore[arg-type]
        ctype = str(cm["type"])
        if ctype == "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY":
            return gis + rcp
        if ctype == "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY":
            ulist = cm["unicode_list"]
            assert isinstance(ulist, list)
            pos = bisect.bisect_left(ulist, rcp)
            if pos < len(ulist) and ulist[pos] == rcp:
                return gis + pos
            return 0
        raise SystemExit(f"verify: unsupported cmap type {ctype}")
    return 0


def verify_c_font(c_text: str, expected_cps: list[int]) -> None:
    """Verify that every code point in expected_cps resolves to its exact glyph_dsc row."""
    dsc = re.findall(r"\{ \.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
                     r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+) \}", c_text)
    if len(dsc) != len(expected_cps) + 1:
        raise SystemExit(
            f"verify: {len(dsc)} glyph_dsc entries for {len(expected_cps)} code points"
        )
    cmaps = parse_cmaps_from_c(c_text)
    for expected_gid, cp in enumerate(expected_cps, start=1):
        got_gid = lvgl_lookup_glyph_id(cmaps, cp)
        if got_gid != expected_gid:
            raise SystemExit(
                f"verify: U+{cp:04X} ({chr(cp)!r}) mapped to glyph_id {got_gid}, "
                f"expected {expected_gid}"
            )


def verify(c_text: str, glyphs: list[Glyph], line_height: int) -> None:
    """Re-parse the generated arrays and compare against the rasterisation."""
    def grab(name: str) -> list[int]:
        m = re.search(r"%s\[\]\s*=\s*\{(.*?)\};" % name, c_text, re.S)
        if not m:
            raise SystemExit(f"verify: array {name} missing from output")
        body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
        return [int(tok, 16) if tok.startswith("0x") else int(tok)
                for tok in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]

    verify_c_font(c_text, [g.cp for g in glyphs])

    bitmap = bytes(grab("glyph_bitmap"))
    dsc = re.findall(r"\{ \.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
                     r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+) \}", c_text)

    for i, g in enumerate(glyphs, start=1):
        bi, adv, bw, bh, bx, by = (int(v) for v in dsc[i])
        expect = (g.bitmap, g.adv, g.w, g.h, g.ofs_x, g.ofs_y)
        got = (bitmap[bi:bi + len(g.bitmap)], adv, bw, bh, bx, by)
        if got != expect:
            raise SystemExit(f"verify: glyph U+{g.cp:04X} descriptor/bitmap mismatch")

        # Decode nibbles like lv_font_get_bitmap_fmt_txt (bpp 4, PLAIN, stride 0).
        out = []
        for byte in bitmap[bi:bi + (len(g.levels) + 1) // 2]:
            out.append((byte >> 4) & 0xF)
            out.append(byte & 0xF)
        if out[:len(g.levels)] != g.levels:
            raise SystemExit(f"verify: glyph U+{g.cp:04X} pixels decode differently")
        if by + bh > line_height:
            raise SystemExit(f"verify: glyph U+{g.cp:04X} exceeds line height")

    print(f"verify: {len(glyphs)} glyphs, {len(bitmap)} bitmap bytes, cmaps 1:1 -- OK")


# --------------------------------------------------------------------------- #
def main() -> None:
    if Image is None or ImageFont is None:
        sys.exit("Pillow is required: python3 -m pip install pillow")

    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="source .otf/.ttf (not committed)")
    ap.add_argument("--size", type=int, default=12)
    ap.add_argument("--name", default="crt_font_12")
    ap.add_argument("--out", required=True)
    ap.add_argument("--line-height", type=int, default=0,
                    help="default: metrics ascent+descent, cropped to size+3")
    ap.add_argument("--chars", default="", help="extra characters to force in")
    ap.add_argument("--mono", action="store_true",
                    help="assert half/full width advances (6px/12px at 12px size); "
                         "layout code relies on this")
    args = ap.parse_args()

    face = ImageFont.truetype(args.font, args.size)
    ascent, descent = face.getmetrics()
    natural = ascent + descent
    line_height = args.line_height or min(natural, args.size + 3)
    crop_top = natural - line_height
    if crop_top < 0:
        raise SystemExit("line height larger than font metrics is not supported")
    base_line = descent

    cps = set(build_inventory())
    cps.update(ord(c) for c in args.chars)
    cps = sorted(cps)

    half = 96 if args.size == 12 else int(round(args.size / 2 * 16))
    full = 192 if args.size == 12 else int(round(args.size * 16))

    glyphs: list[Glyph] = []
    for cp in cps:
        try:
            glyphs.append(rasterise(face, cp, crop_top, line_height))
        except Exception as exc:  # noqa: BLE001 - report which code point failed
            raise SystemExit(f"rasterise U+{cp:04X} failed: {exc}") from exc

    if args.mono:
        # Layout math assumes every glyph advances 6px or 12px at 12px size.
        # Anything else (odd symbol in the source face) is dropped, not stretched.
        bad = [(g.cp, g.adv) for g in glyphs
               if g.adv not in (half, full) and g.cp != 0x20]
        if bad:
            print("drop non-monospace glyphs: " +
                  ", ".join(f"U+{cp:04X}" for cp, _ in bad[:20]) +
                  (f" (+{len(bad) - 20})" if len(bad) > 20 else ""), file=sys.stderr)
            bad_cps = {cp for cp, _ in bad}
            glyphs = [g for g in glyphs if g.cp not in bad_cps]
            if args.chars:
                forced = [ord(c) for c in args.chars if ord(c) in bad_cps]
                if forced:
                    raise SystemExit("forced characters are not monospace: " +
                                     ", ".join(f"U+{c:04X}" for c in forced))

    source_note = (f"Source: {Path(args.font).name} {args.size}px; rasterised by Pillow/FreeType; "
                   f"inventory {len(glyphs)} code points")
    header, c_text = emit(args.name, glyphs, line_height, base_line, args.size, source_note)
    verify(c_text, glyphs, line_height)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(c_text, encoding="utf-8")
    out.with_suffix(".h").write_text(header, encoding="utf-8")
    inventory = out.with_suffix(".chars")
    # Inventory = what is actually embedded, so a later diff is meaningful.
    inventory.write_text("".join(chr(g.cp) for g in glyphs), encoding="utf-8")

    bitmap_bytes = sum(len(g.bitmap) for g in glyphs)
    print(f"wrote {out} ({out.stat().st_size / 1024:.0f} KiB source, "
          f"{bitmap_bytes / 1024:.0f} KiB bitmap, {len(glyphs)} glyphs, "
          f"line_height {line_height}, base_line {base_line})")


if __name__ == "__main__":
    main()
