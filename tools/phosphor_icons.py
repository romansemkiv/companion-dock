#!/usr/bin/env python3
"""Phosphor Icons (MIT, @phosphor-icons/core) as the deck's icon outlines (Regular weight).

Reads the chosen SVGs of one weight from a directory and writes components/deckui/phosphor.c and phosphor.h: each
glyph's outline as closed contours (its curves and arcs flattened finely enough that no facet shows at 120 px), the
points in fixed point (PH_UNIT per unit of Phosphor's 256-unit box), filled with the nonzero rule (rs_fill_contours);
and the weight's stroke and the slash's clearance as measured on its wifi-slash (for the crossed signs).

    python3 -I tools/phosphor_icons.py [--weight regular|bold] <dir with the SVGs> [components/deckui]

regular (the default): assets/regular/<name>.svg; bold: assets/bold/<name>-bold.svg.

The SVGs are untrusted downloads: only one <path d="..."> per file is read, its data checked against the path
grammar's characters, and nothing else in the file is used. They are never committed; the generated C carries
Phosphor's MIT notice, the package version and every source name, so it can be regenerated. Standard library only.
"""
import math
import os
import re
import sys

VERSION = "2.1.1"  # @phosphor-icons/core
UNIT = 16          # fixed point: 1/16 of a Phosphor unit (the 256 box is 0..4096)
TOL = 0.12         # flattening: the most a chord strays from its curve, in Phosphor units (0.06 px on a 120 px box)
STROKE = {"regular": 16, "bold": 24}  # each weight's stroke, in Phosphor units
SLASH = ((48, 40), (208, 216))        # the slash's centreline in Phosphor's slash glyphs (wifi-slash)

# The glyphs, in the order of their PH_* ids: (the C id, the Phosphor name)
GLYPHS = [
    ("WIFI_HIGH", "wifi-high"), ("WIFI_MEDIUM", "wifi-medium"), ("WIFI_LOW", "wifi-low"),
    ("NETWORK", "network"), ("GLOBE", "globe"), ("SQUARES_FOUR", "squares-four"),
    ("HARD_DRIVE", "hard-drive"), ("BROADCAST", "broadcast"), ("INFO", "info"), ("CARET_LEFT", "caret-left"),
    ("CARET_RIGHT", "caret-right"), ("LIST", "list"), ("ARROW_CLOCKWISE", "arrow-clockwise"), ("POWER", "power"),
    ("ARROWS_CLOCKWISE", "arrows-clockwise"), ("TOGGLE_LEFT", "toggle-left"), ("TOGGLE_RIGHT", "toggle-right"),
    ("QR_CODE", "qr-code"), ("LIST_BULLETS", "list-bullets"), ("SUN_DIM", "sun-dim"), ("SUN", "sun"),
    ("SPEAKER_HIGH", "speaker-high"), ("SPEAKER_SLASH", "speaker-slash"), ("LOCK", "lock"), ("X", "x"),
    ("BACKSPACE", "backspace"), ("LIGHTNING", "lightning"), ("PENCIL_SIMPLE", "pencil-simple"),
    ("CIRCUITRY", "circuitry"), ("MAGNIFYING_GLASS", "magnifying-glass"), ("WIFI_SLASH", "wifi-slash"),
    ("WEBHOOKS_LOGO", "webhooks-logo"),  # Companion
]

NOTICE = """\
 * Phosphor Icons (https://phosphoricons.com), @phosphor-icons/core {ver}, {weight} weight. MIT License:
 *
 * Copyright (c) 2023 Phosphor Icons
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 * the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."""


def path_data(svg_text, name):
    """The one path's d attribute of a Phosphor SVG (checked: one path, nothing else drawn, path characters only)."""
    if not svg_text.lstrip().startswith("<svg") or 'viewBox="0 0 256 256"' not in svg_text:
        raise ValueError(f"{name}: not a 256-unit Phosphor SVG")
    for tag in ("<circle", "<rect", "<line", "<poly", "<ellipse", "<g ", "transform", "fill-rule", "<use", "<image"):
        if tag in svg_text:
            raise ValueError(f"{name}: has {tag}, not handled")
    ds = re.findall(r'<path\b[^>]*\bd="([^"]*)"', svg_text)
    if len(ds) != 1 or svg_text.count("<path") != 1:
        raise ValueError(f"{name}: {len(ds)} paths, want 1")
    d = ds[0]
    if len(d) > 20000 or not re.fullmatch(r"[MmLlHhVvCcSsQqTtAaZz0-9.,eE+\- \n\t]*", d):
        raise ValueError(f"{name}: unexpected path data")
    return d


def tokens(d):
    for m in re.finditer(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", d):
        yield m.group(0)


def bezier_flat(p0, p1, p2, p3, out, depth=0):
    """A cubic into out (its points after p0), subdivided until it is within TOL of its chord."""
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = p0, p1, p2, p3
    dx, dy = x3 - x0, y3 - y0
    n = math.hypot(dx, dy)
    if n > 1e-9:
        d1 = abs((x1 - x3) * dy - (y1 - y3) * dx) / n
        d2 = abs((x2 - x3) * dy - (y2 - y3) * dx) / n
    else:
        d1, d2 = math.hypot(x1 - x0, y1 - y0), math.hypot(x2 - x0, y2 - y0)
    if (d1 + d2) <= TOL * 0.75 or depth > 16:
        out.append(p3)
        return
    m01 = ((x0 + x1) / 2, (y0 + y1) / 2)
    m12 = ((x1 + x2) / 2, (y1 + y2) / 2)
    m23 = ((x2 + x3) / 2, (y2 + y3) / 2)
    a = ((m01[0] + m12[0]) / 2, (m01[1] + m12[1]) / 2)
    b = ((m12[0] + m23[0]) / 2, (m12[1] + m23[1]) / 2)
    m = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    bezier_flat(p0, m01, a, m, out, depth + 1)
    bezier_flat(m, b, m23, p3, out, depth + 1)


def arc_flat(p0, rx, ry, phi_deg, large, sweep, p1, out):
    """An SVG elliptical arc (endpoint form, F.6.5 of the SVG spec) into out (its points after p0)."""
    x1, y1 = p0
    x2, y2 = p1
    if (x1, y1) == (x2, y2):
        return
    rx, ry = abs(rx), abs(ry)
    if rx == 0 or ry == 0:
        out.append(p1)
        return
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    dx2, dy2 = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx2 + sp * dy2, -sp * dx2 + cp * dy2
    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    r = max(rx, ry)
    step = 2 * math.acos(max(-1.0, min(1.0, 1 - TOL / r))) if r > TOL else math.pi / 2
    n = max(1, int(math.ceil(abs(dt) / step)))
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        ex, ey = rx * math.cos(t), ry * math.sin(t)
        out.append((cp * ex - sp * ey + cx, sp * ex + cp * ey + cy) if i < n else p1)


def contours_of(d, name):
    """The path's closed contours, each a list of points (the closing point not repeated)."""
    toks = list(tokens(d))
    i = 0
    cmd = None
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    last_c2 = None  # the last cubic's second control point (S)
    last_q = None   # the last quadratic's control point (T)
    contours, pts = [], []

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def flag():
        nonlocal i
        t = toks[i]
        if t in ("0", "1"):
            i += 1
            return int(t)
        # a flag run written without separators: "01" or "1-17.76" never reach here (the tokenizer splits numbers)
        if re.fullmatch(r"[01]+(\.\d*)?", t):
            f = int(t[0])
            toks[i] = t[1:]
            return f
        raise ValueError(f"{name}: bad arc flag {t!r}")

    def close():
        nonlocal pts
        if len(pts) >= 2:
            if pts[0] == pts[-1]:
                pts.pop()
            contours.append(pts)
        pts = []

    while i < len(toks):
        t = toks[i]
        if re.fullmatch(r"[A-Za-z]", t):
            cmd = t
            i += 1
            if cmd in "Zz":
                close()
                cur = start
                last_c2 = last_q = None
                continue
        elif cmd is None:
            raise ValueError(f"{name}: data before a command")
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = cur if rel else (0.0, 0.0)
        if c == "M":
            if pts:
                close()
            cur = (ox + num(), oy + num())
            start = cur
            pts = [cur]
            cmd = "l" if rel else "L"  # further pairs are lines
            last_c2 = last_q = None
        elif c == "L":
            cur = (ox + num(), oy + num())
            pts.append(cur)
            last_c2 = last_q = None
        elif c == "H":
            cur = ((cur[0] if rel else 0.0) + num(), cur[1])
            pts.append(cur)
            last_c2 = last_q = None
        elif c == "V":
            cur = (cur[0], (cur[1] if rel else 0.0) + num())
            pts.append(cur)
            last_c2 = last_q = None
        elif c in "CS":
            if c == "C":
                c1 = (ox + num(), oy + num())
            else:
                c1 = (2 * cur[0] - last_c2[0], 2 * cur[1] - last_c2[1]) if last_c2 else cur
            c2 = (ox + num(), oy + num())
            end = (ox + num(), oy + num())
            bezier_flat(cur, c1, c2, end, pts)
            cur, last_c2, last_q = end, c2, None
        elif c in "QT":
            if c == "Q":
                q = (ox + num(), oy + num())
            else:
                q = (2 * cur[0] - last_q[0], 2 * cur[1] - last_q[1]) if last_q else cur
            end = (ox + num(), oy + num())
            c1 = (cur[0] + 2 / 3 * (q[0] - cur[0]), cur[1] + 2 / 3 * (q[1] - cur[1]))
            c2 = (end[0] + 2 / 3 * (q[0] - end[0]), end[1] + 2 / 3 * (q[1] - end[1]))
            bezier_flat(cur, c1, c2, end, pts)
            cur, last_q, last_c2 = end, q, None
        elif c == "A":
            rx, ry, phi = num(), num(), num()
            large, sweep = flag(), flag()
            end = (ox + num(), oy + num())
            arc_flat(cur, rx, ry, phi, large, sweep, end, pts)
            cur = end
            last_c2 = last_q = None
        else:
            raise ValueError(f"{name}: command {cmd} not handled")
    if pts:
        close()
    return contours


def area(c):
    return sum(c[k][0] * c[(k + 1) % len(c)][1] - c[(k + 1) % len(c)][0] * c[k][1] for k in range(len(c))) / 2


def slash_clearance(contours):
    """How far wifi-slash's glyph keeps clear of its slash on the slash's upper right: the nearest point, from the
    slash's centreline, of the contours wholly on that side (the arcs cut clear of it)."""
    (ax, ay), (bx, by) = SLASH
    n = math.hypot(bx - ax, by - ay)
    ux, uy = (bx - ax) / n, (by - ay) / n
    best = None
    for c in contours:
        d = [(x - ax) * uy - (y - ay) * ux for x, y in c]
        if min(d) > 0:
            best = min(d) if best is None else min(best, min(d))
    if best is None:
        raise ValueError("wifi-slash: no contour clear of the slash")
    return best


def main():
    args = sys.argv[1:]
    weight = "regular"
    if len(args) >= 2 and args[0] == "--weight":
        weight, args = args[1], args[2:]
    if weight not in STROKE or len(args) not in (1, 2):
        print(__doc__)
        return 2
    src = args[0]
    out_dir = args[1] if len(args) == 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                                          "components", "deckui")
    asset = (lambda n: f"{n}.svg") if weight == "regular" else (lambda n: f"{n}-{weight}.svg")
    pts_all, ends, glyphs, report = [], [], [], []
    clear = None
    for cid, name in GLYPHS:
        with open(os.path.join(src, asset(name)), encoding="utf-8") as f:
            text = f.read(65536)
        cs = contours_of(path_data(text, name), name)
        if name == "wifi-slash":
            clear = slash_clearance(cs)
        first = len(ends)
        xs, ys = [], []
        for c in cs:
            q = []
            for x, y in c:
                p = (int(round(x * UNIT)), int(round(y * UNIT)))
                if not q or p != q[-1]:
                    q.append(p)
            if len(q) > 1 and q[0] == q[-1]:
                q.pop()
            if len(q) < 3:
                continue
            for x, y in q:
                if not (-UNIT * 8 <= x <= UNIT * 264 and -UNIT * 8 <= y <= UNIT * 264):
                    raise ValueError(f"{name}: a point outside the box")
                xs.append(x)
                ys.append(y)
            pts_all.extend(q)
            ends.append(len(pts_all))
        glyphs.append((cid, name, first, len(ends) - first))
        report.append(f"{name}: {len(ends) - first} contours, {sum(ends[k] - (ends[k - 1] if k else 0) for k in range(first, len(ends)))} points, "
                      f"x {min(xs) / UNIT:.1f}..{max(xs) / UNIT:.1f}, y {min(ys) / UNIT:.1f}..{max(ys) / UNIT:.1f}")
    if len(pts_all) > 65535:
        raise ValueError("too many points for uint16 ends")

    wname = weight.capitalize()
    h = ["/* Output of tools/phosphor_icons.py: do not edit (regenerate it from the SVGs named below).", " *",
         NOTICE.format(ver=VERSION, weight=wname), " */", "#pragma once", "#include <stdint.h>", "",
         f"#define PH_UNIT {UNIT} /* the points' fixed point: per unit of Phosphor's 256-unit box */",
         "#define PH_BOX 256",
         f'#define PH_WEIGHT "{weight}"',
         f"#define PH_STROKE {STROKE[weight]} /* the weight's stroke, in units of the box */",
         f"#define PH_SLASH_CLEAR {clear:.2f}f /* wifi-slash: its glyph clear of the slash's centreline on its upper right */",
         "", "enum {"]
    for cid, name, _, _ in glyphs:
        h.append(f"    PH_{cid}, /* {name} */")
    h += ["    PH_COUNT", "};", "",
          "typedef struct {", "    uint16_t c0, nc; /* its contours: ph_end[c0 .. c0 + nc - 1] */", "} ph_glyph_t;", "",
          "/* The points (x, y pairs; contour k's points are ph_end[k - 1] .. ph_end[k] - 1, ph_end[-1] being 0), each",
          " * contour closed (its last point joins its first), filled with the nonzero rule. */",
          "extern const int16_t ph_pts[];", "extern const uint16_t ph_end[];", "extern const ph_glyph_t ph_glyph[PH_COUNT];",
          f"#define PH_NPTS {len(pts_all)}", f"#define PH_NCONTOURS {len(ends)}", ""]
    c = ["/* Output of tools/phosphor_icons.py: do not edit (regenerate it from the SVGs named below).", " *",
         NOTICE.format(ver=VERSION, weight=wname), " *",
         f" * Sources (assets/{weight}/{asset('<name>')}), flattened to within {TOL} of a unit: " +
         ", ".join(n for _, n in GLYPHS) + ".", " */", '#include "phosphor.h"', ""]
    c.append("const int16_t ph_pts[] = {")
    flat = [v for p in pts_all for v in p]
    for k in range(0, len(flat), 16):
        c.append("    " + ", ".join(str(v) for v in flat[k:k + 16]) + ",")
    c += ["};", "", "const uint16_t ph_end[] = {"]
    for k in range(0, len(ends), 16):
        c.append("    " + ", ".join(str(v) for v in ends[k:k + 16]) + ",")
    c += ["};", "", "const ph_glyph_t ph_glyph[PH_COUNT] = {"]
    for cid, name, first, nc in glyphs:
        c.append(f"    [PH_{cid}] = {{{first}, {nc}}}, /* {name} */")
    c += ["};", ""]
    with open(os.path.join(out_dir, "phosphor.h"), "w", encoding="utf-8") as f:
        f.write("\n".join(h))
    with open(os.path.join(out_dir, "phosphor.c"), "w", encoding="utf-8") as f:
        f.write("\n".join(c))
    for line in report:
        print(line)
    print(f"{weight}: {len(glyphs)} glyphs, {len(ends)} contours, {len(pts_all)} points ({len(pts_all) * 4 + len(ends) * 2} bytes), "
          f"the slash's clearance {clear:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
