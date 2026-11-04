#!/usr/bin/env python3
"""PNG fixtures for test_png.c: images in every colour type and depth a PNG can have, each written by Pillow (not
by the decoder under test) next to <name>.rgb, the RGB the decoder must give: 8-bit RGB, alpha composited over black
as (c * a + 127) // 255, 16-bit samples by their high byte.

Run it again (Pillow needed) to remake them: python3 make_fixtures.py
"""
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def pattern(w, h):
    """An RGBA image that exercises the PNG filters: gradients, hard edges, a flat area, varied alpha."""
    px = []
    for y in range(h):
        for x in range(w):
            r = (x * 255) // max(1, w - 1)
            g = (y * 255) // max(1, h - 1)
            b = 255 if (x // 4 + y // 4) % 2 else 32
            if w // 3 <= x < 2 * w // 3 and h // 3 <= y < 2 * h // 3:
                r, g, b = 20, 120, 220  # a flat block, as on a button
            a = (x * 7 + y * 13) % 256
            px.append((r, g, b, a))
    return px


def over_black(px):
    out = bytearray()
    for r, g, b, a in px:
        out += bytes(((r * a + 127) // 255, (g * a + 127) // 255, (b * a + 127) // 255))
    return bytes(out)


def save(name, img, rgb, **kw):
    img.save(os.path.join(HERE, name + '.png'), **kw)
    with open(os.path.join(HERE, name + '.rgb'), 'wb') as f:
        f.write(rgb)


def main():
    w = h = 16
    px = pattern(w, h)
    opaque = [(r, g, b, 255) for r, g, b, _ in px]

    # RGB 8-bit, and RGBA 8-bit opaque and with alpha
    img = Image.new('RGB', (w, h))
    img.putdata([p[:3] for p in px])
    save('rgb8', img, over_black(opaque), optimize=True)
    img = Image.new('RGBA', (w, h))
    img.putdata(opaque)
    save('rgba8_opaque', img, over_black(opaque))
    img = Image.new('RGBA', (w, h))
    img.putdata(px)
    save('rgba8_alpha', img, over_black(px))

    # grey 8-bit, grey + alpha, grey 16-bit (v * 257: the high byte is v), 1-bit
    grey = [(x * 16 + y) % 256 for y in range(h) for x in range(w)]
    img = Image.new('L', (w, h))
    img.putdata(grey)
    save('grey8', img, over_black([(v, v, v, 255) for v in grey]))
    img = Image.new('LA', (w, h))
    img.putdata([(v, (v * 3) % 256) for v in grey])
    save('grey_alpha', img, over_black([(v, v, v, (v * 3) % 256) for v in grey]))
    img = Image.new('I;16', (w, h))
    img.putdata([v * 257 for v in grey])
    save('grey16', img, over_black([(v, v, v, 255) for v in grey]))
    bw = [255 if (x + y) % 3 == 0 else 0 for y in range(h) for x in range(w)]
    img = Image.new('1', (w, h))
    img.putdata(bw)
    save('bw1', img, over_black([(v, v, v, 255) for v in bw]))

    # palette: 8-bit, 4-bit (16 colours), and with a transparent entry (tRNS)
    pal = [((i * 37) % 256, (i * 91) % 256, (i * 53) % 256) for i in range(256)]
    idx = [(x * 5 + y * 3) % 200 for y in range(h) for x in range(w)]
    img = Image.new('P', (w, h))
    img.putpalette([c for rgb in pal for c in rgb])
    img.putdata(idx)
    save('palette8', img, over_black([pal[i] + (255,) for i in idx]))
    idx4 = [(x + y) % 16 for y in range(h) for x in range(w)]
    img = Image.new('P', (w, h))
    img.putpalette([c for rgb in pal[:16] for c in rgb])
    img.putdata(idx4)
    save('palette4', img, over_black([pal[i] + (255,) for i in idx4]), bits=4)
    img = Image.new('P', (w, h))
    img.putpalette([c for rgb in pal[:16] for c in rgb])
    img.putdata(idx4)
    save('palette_trns', img, over_black([pal[i] + (0 if i == 5 else 255,) for i in idx4]), transparency=5)

    # a button-sized image, as Companion sends an XL key (96 x 96, RGB)
    bw_, bh_ = 96, 96
    big = pattern(bw_, bh_)
    img = Image.new('RGB', (bw_, bh_))
    img.putdata([p[:3] for p in big])
    save('button96', img, over_black([p[:3] + (255,) for p in big]), optimize=True)


if __name__ == '__main__':
    main()
