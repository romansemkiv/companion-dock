#!/usr/bin/env python3
"""Rasterises the deck font into components/statusui/deck_font.c.

    python3 -I tools/gen_deck_font.py            # from the repo root; needs Pillow

Printable ASCII (0x20-0x7E) at each size in SIZES, 4-bit alpha per pixel, with per-glyph metrics
(advance, ink box, bearing) and per-size ascent, descent, cap height and line height. The output is
committed, so the firmware build needs no Python packages; run this again only to change the font or
the sizes. The font is Barlow Condensed Bold (SIL Open Font License 1.1, tools/fonts/OFL.txt).
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "tools", "fonts", "BarlowCondensed-Bold.ttf")
OUT = os.path.join(ROOT, "components", "statusui", "deck_font.c")
NAME = "Barlow Condensed Bold"
LICENCE = "SIL Open Font License 1.1 (tools/fonts/OFL.txt)"

# Sizes: the font's em in px. Every px from 14 (the smallest readable size on a 72 px key) to 22,
# where most long words land; then steps of 2-4 px, with each key size's cap (42 % of the key:
# 72 -> 30, 80 -> 33, 96 -> 40, 112 -> 47, 120 -> 50) present. 10 and 12 are fallbacks for keys
# too small for 14 px (no deck has one).
SIZES = [10, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 24, 26, 28, 30, 33, 36, 40, 44, 47, 50]
LINE_EM = 1.05  # baseline to baseline of the text lines of one word in its key
FIRST, LAST = 0x20, 0x7E


def glyph(font, px, ch):
    """(adv16, bx, by, bw, bh, nibbles) of ch: ink box relative to the pen on the baseline."""
    adv16 = int(round(font.getlength(ch) * 16))
    w = h = 3 * px
    ox, oy = px, 2 * px  # the pen on the baseline
    im = Image.new("L", (w, h), 0)
    ImageDraw.Draw(im).text((ox, oy), ch, font=font, fill=255, anchor="ls")
    q = im.point(lambda v: (v * 15 + 127) // 255)
    box = q.getbbox()
    if box is None:
        return adv16, 0, 0, 0, 0, []
    x0, y0, x1, y1 = box
    if x0 == 0 or y0 == 0 or x1 == w or y1 == h:
        sys.exit(f"glyph {ch!r} at {px} px touches the canvas edge")
    data = q.tobytes()
    nib = [data[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]
    return adv16, x0 - ox, y0 - oy, x1 - x0, y1 - y0, nib


def pack(nib):
    if len(nib) % 2:
        nib = nib + [0]
    return [(nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2)]


def c_bytes(vals, indent="    ", width=118):
    out, line = [], indent
    for v in vals:
        t = f"{v},"
        if len(line) + len(t) > width:
            out.append(line.rstrip())
            line = indent
        line += t
    if line.strip():
        out.append(line.rstrip())
    return "\n".join(out)


def main():
    sizes, bits_total, meta_total = [], 0, 0
    body = []
    for px in SIZES:
        font = ImageFont.truetype(FONT, px)
        bits, glyphs = [], []
        for code in range(FIRST, LAST + 1):
            adv16, bx, by, bw, bh, nib = glyph(font, px, chr(code))
            if not (-128 <= bx <= 127 and -128 <= by <= 127 and bw <= 255 and bh <= 255 and adv16 <= 0xFFFF):
                sys.exit(f"glyph {chr(code)!r} at {px} px out of range")
            glyphs.append((len(bits), adv16, bx, by, bw, bh))
            bits += pack(nib)
        if len(bits) > 0xFFFF:
            sys.exit(f"{px} px: {len(bits)} bytes of alpha, more than a 16-bit offset reaches")
        asc = max(-g[3] for g in glyphs if g[5])
        desc = max(g[3] + g[5] for g in glyphs if g[5])
        cap = -glyphs[ord("H") - FIRST][3]
        line = int(round(px * LINE_EM))
        sizes.append((px, asc, desc, cap, line))
        bits_total += len(bits)
        meta_total += 8 * len(glyphs)
        body.append(f"static const uint8_t bits_{px}[{len(bits)}] = {{\n{c_bytes(bits)}\n}};\n")
        rows = []
        for code, (off, adv16, bx, by, bw, bh) in zip(range(FIRST, LAST + 1), glyphs):
            ch = chr(code)
            cmt = {"\\": "backslash", "*": "star", "/": "slash"}.get(ch, ch)
            rows.append(f"    {{{off}, {adv16}, {bx}, {by}, {bw}, {bh}}}, /* {cmt} */")
        body.append(f"static const sui_glyph_t glyphs_{px}[SUI_FONT_GLYPHS] = {{\n" + "\n".join(rows) + "\n};\n")
    table = ",\n".join(f"    {{{px}, {asc}, {desc}, {cap}, {line}, glyphs_{px}, bits_{px}}}"
                       for px, asc, desc, cap, line in sizes)
    total = bits_total + meta_total + 16 * len(sizes)
    head = (
        "/* Output of tools/gen_deck_font.py - do not edit; run it again to change the font or the sizes.\n"
        f" * Font: {NAME} (tools/fonts/BarlowCondensed-Bold.ttf), {LICENCE}.\n"
        " * Copyright 2017 The Barlow Project Authors (https://github.com/jpt/barlow).\n"
        f" * Printable ASCII at {len(SIZES)} sizes ({', '.join(map(str, SIZES))} px em), 4-bit alpha:\n"
        f" * {bits_total} bytes of alpha, {meta_total} of glyph metrics, {total} in all. */\n"
        "#include \"statusui/deck_font.h\"\n\n"
    )
    tail = (
        f"const sui_font_size_t sui_font[] = {{\n{table},\n}};\n"
        "const int sui_font_sizes = (int)(sizeof sui_font / sizeof sui_font[0]);\n"
    )
    with open(OUT, "w", newline="\n") as f:
        f.write(head + "\n".join(body) + "\n" + tail)
    print(f"{os.path.relpath(OUT, ROOT)}: {len(SIZES)} sizes, alpha {bits_total} B, metrics {meta_total} B, "
          f"total {total} B ({total / 1024:.1f} KiB)")
    for px, asc, desc, cap, line in sizes:
        print(f"  {px:2d} px: asc {asc:2d} desc {desc:2d} cap {cap:2d} line {line:2d}")


if __name__ == "__main__":
    main()
