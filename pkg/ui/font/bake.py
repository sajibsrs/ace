#!/usr/bin/env python3
"""Bakes printable ASCII from Lato Light, Regular, and Bold into ../inc/font.ace.

Each glyph's outlines flatten to closed contours in em units of EM = 64 per em,
y down, the baseline at 0; the program rasterizes them at run time, at any
size. POINTS holds, per glyph, its contours as n then n (x, y) pairs, then -9;
winding is as the font's (nonzero fill, holes wind the other way).

    python3 -I font/bake.py        (from pkg/ui)

The fonts are reader-checked only for what Lato uses: TrueType outlines, simple
and offset-only composite glyphs, a format 4 or 12 Unicode map.
"""

import importlib.util
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "inc" / "font.ace"
WEIGHTS = ["Light", "Regular", "Bold"]
FIRST, LAST = 32, 126
EM = 64.0
# Flatness of curves, in em units: 0.05 pixel at 100 pixels per em.
FLAT = 0.05 * EM / 100.0

spec = importlib.util.spec_from_file_location("icon_bake", HERE.parents[2] / "demo" / "binbox" / "icon" / "bake.py")
icon_bake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icon_bake)


def tables(data):
    n = struct.unpack(">H", data[4:6])[0]
    out = {}
    for i in range(n):
        tag, _, off, length = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        out[tag.decode()] = (off, length)
    return out


class Font:
    def __init__(self, data):
        self.d = data
        t = tables(data)
        self.t = {k: v[0] for k, v in t.items()}
        self.upem = struct.unpack(">H", data[self.t["head"] + 18:self.t["head"] + 20])[0]
        loca_long = struct.unpack(">h", data[self.t["head"] + 50:self.t["head"] + 52])[0]
        glyphs = struct.unpack(">H", data[self.t["maxp"] + 4:self.t["maxp"] + 6])[0]
        o, ln = self.t["loca"], t["loca"][1]
        self.loca = list(struct.unpack(">%d%s" % (glyphs + 1, "I" if loca_long else "H"), data[o:o + (4 if loca_long else 2) * (glyphs + 1)]))
        if not loca_long:
            self.loca = [v * 2 for v in self.loca]
        self.hmetrics = struct.unpack(">H", data[self.t["hhea"] + 34:self.t["hhea"] + 36])[0]
        self.cmap = self.read_cmap()

    def read_cmap(self):
        d, base = self.d, self.t["cmap"]
        n = struct.unpack(">H", d[base + 2:base + 4])[0]
        best = None
        for i in range(n):
            plat, enc, off = struct.unpack(">HHI", d[base + 4 + 8 * i:base + 12 + 8 * i])
            if (plat, enc) in ((3, 10), (3, 1), (0, 3), (0, 4)):
                fmt = struct.unpack(">H", d[base + off:base + off + 2])[0]
                if fmt in (4, 12) and (best is None or fmt == 12):
                    best = (base + off, fmt)
        if best is None:
            sys.exit("no Unicode cmap")
        off, fmt = best
        out = {}
        if fmt == 12:
            groups = struct.unpack(">I", d[off + 12:off + 16])[0]
            for g in range(groups):
                a, b, gid = struct.unpack(">III", d[off + 16 + 12 * g:off + 28 + 12 * g])
                for c in range(a, b + 1):
                    out[c] = gid + c - a
            return out
        segs = struct.unpack(">H", d[off + 6:off + 8])[0] // 2
        end = struct.unpack(">%dH" % segs, d[off + 14:off + 14 + 2 * segs])
        start_at = off + 16 + 2 * segs
        start = struct.unpack(">%dH" % segs, d[start_at:start_at + 2 * segs])
        delta_at = start_at + 2 * segs
        delta = struct.unpack(">%dh" % segs, d[delta_at:delta_at + 2 * segs])
        range_at = delta_at + 2 * segs
        rng = struct.unpack(">%dH" % segs, d[range_at:range_at + 2 * segs])
        for s in range(segs):
            for c in range(start[s], end[s] + 1):
                if c == 0xFFFF:
                    continue
                if rng[s] == 0:
                    gid = (c + delta[s]) & 0xFFFF
                else:
                    at = range_at + 2 * s + rng[s] + 2 * (c - start[s])
                    gid = struct.unpack(">H", d[at:at + 2])[0]
                    if gid:
                        gid = (gid + delta[s]) & 0xFFFF
                out[c] = gid
        return out

    def advance(self, gid):
        i = min(gid, self.hmetrics - 1)
        return struct.unpack(">H", self.d[self.t["hmtx"] + 4 * i:self.t["hmtx"] + 4 * i + 2])[0]

    def contours(self, gid):
        """Closed contours of glyph `gid` as lists of (x, y, on-curve)."""
        d = self.d
        a, b = self.loca[gid], self.loca[gid + 1]
        if a == b:
            return []
        g = self.t["glyf"] + a
        n = struct.unpack(">h", d[g:g + 2])[0]
        p = g + 10
        if n < 0:
            out = []
            while True:
                flags, sub = struct.unpack(">HH", d[p:p + 4])
                p += 4
                if flags & 1:
                    dx, dy = struct.unpack(">hh", d[p:p + 4])
                    p += 4
                else:
                    dx, dy = struct.unpack(">bb", d[p:p + 2])
                    p += 2
                if not flags & 2:
                    sys.exit("point-matched composite")
                if flags & (8 | 0x40 | 0x80):
                    sys.exit("scaled composite")
                for c in self.contours(sub):
                    out.append([(x + dx, y + dy, on) for x, y, on in c])
                if not flags & 0x20:
                    return out
        ends = struct.unpack(">%dH" % n, d[p:p + 2 * n])
        p += 2 * n
        ilen = struct.unpack(">H", d[p:p + 2])[0]
        p += 2 + ilen
        count = ends[-1] + 1
        flags = []
        while len(flags) < count:
            f = d[p]
            p += 1
            flags.append(f)
            if f & 8:
                flags += [f] * d[p]
                p += 1
        xs, ys, v = [], [], 0
        for f in flags:
            if f & 2:
                v += d[p] if f & 16 else -d[p]
                p += 1
            elif not f & 16:
                v += struct.unpack(">h", d[p:p + 2])[0]
                p += 2
            xs.append(v)
        v = 0
        for f in flags:
            if f & 4:
                v += d[p] if f & 32 else -d[p]
                p += 1
            elif not f & 32:
                v += struct.unpack(">h", d[p:p + 2])[0]
                p += 2
            ys.append(v)
        out, s = [], 0
        for e in ends:
            out.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)])
            s = e + 1
        return out


def flatten(contour, upem):
    """A TrueType contour -> points in em units (y down)."""
    k = EM / upem
    pts = [(x * k, -y * k, on) for x, y, on in contour]
    # Start on an on-curve point; two off-curve points have one halfway.
    exp = []
    for i, (x, y, on) in enumerate(pts):
        nx, ny, non = pts[(i + 1) % len(pts)]
        exp.append((x, y, on))
        if not on and not non:
            exp.append(((x + nx) / 2, (y + ny) / 2, True))
    first = next(i for i, p in enumerate(exp) if p[2])
    exp = exp[first:] + exp[:first]
    out = [exp[0][:2]]
    i = 1
    while i <= len(exp):
        cur = exp[i % len(exp)]
        if cur[2]:
            out.append(cur[:2])
            i += 1
            continue
        end = exp[(i + 1) % len(exp)]
        p0, p1, p2 = out[-1], cur[:2], end[:2]
        dev = math.hypot(p0[0] - 2 * p1[0] + p2[0], p0[1] - 2 * p1[1] + p2[1]) / 4
        steps = max(1, math.ceil(math.sqrt(dev / FLAT)))
        for s in range(1, steps + 1):
            t = s / steps
            u = 1 - t
            out.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                        u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))
        i += 2
    ring = [(round(x, 2), round(y, 2)) for x, y in out]
    ring = [p for k_, p in enumerate(ring) if k_ == 0 or p != ring[k_ - 1]]
    if ring[0] == ring[-1]:
        ring.pop()
    # Drop points that lie on the line through their neighbors.
    again = True
    while again and len(ring) > 3:
        again = False
        for k_ in range(len(ring)):
            a, b, c = ring[k_ - 1], ring[k_], ring[(k_ + 1) % len(ring)]
            if abs(icon_bake.cross(a, b, c)) <= FLAT * math.dist(a, c) * 0.25:
                ring.pop(k_)
                again = True
                break
    return ring


def bbox_top(font, code):
    """The highest point of the glyph for `code`, in em units."""
    gid = font.cmap[code]
    g = font.t["glyf"] + font.loca[gid]
    return struct.unpack(">h", font.d[g + 8:g + 10])[0] * EM / font.upem


def main():
    fonts = [Font((HERE / f"Lato-{w}.ttf").read_bytes()) for w in WEIGHTS]
    start, adv, flat, worst = [], [], [], (0, "")
    for font in fonts:
        for code in range(FIRST, LAST + 1):
            start.append(len(flat))
            adv.append(round(font.advance(font.cmap[code]) * EM / font.upem, 3))
            for c in font.contours(font.cmap[code]):
                ring = flatten(c, font.upem)
                if len(ring) < 3:
                    continue
                if len(ring) > worst[0]:
                    worst = (len(ring), chr(code))
                flat.append(float(len(ring)))
                for x, y in ring:
                    flat += [round(x, 3), round(y, 3)]
            flat.append(-9.0)
    regular = fonts[1]
    xh = [round(bbox_top(f, ord("x")), 3) for f in fonts]
    lines = [
        "/* font: printable ASCII from Lato Light, Regular, and Bold, flattened to contours in em units.",
        " *",
        " * Generated by font/bake.py from font/Lato-*.ttf (SIL Open Font License 1.1,",
        " * see font/LICENSE.txt); do not edit by hand. The layout of POINTS is described",
        " * there.",
        " */",
        "",
        "/* Em units per em; y runs down from the baseline. */",
        f"pub EM: const f32 = {EM}",
        "/* The weights baked, in the order START and ADVANCE hold them. */",
        f"pub WEIGHTS: const usize = {len(WEIGHTS)}",
        "/* The height of a capital above the baseline, in the regular weight. */",
        f"pub CAP: const f32 = {round(bbox_top(regular, ord('H')), 3)}",
        "/* The height of a lowercase x above the baseline, per weight, in em units. */",
        f"pub XHEIGHT: [{len(WEIGHTS)}]f32 = {xh}",
        "/* The first and last character code baked. */",
        f"pub FIRST: const usize = {FIRST}",
        f"pub LAST: const usize = {LAST}",
        "",
        "/* How far the pen moves after each glyph, in em units: weight w, character c at",
        " * w * (LAST - FIRST + 1) + c - FIRST. */",
        f"pub ADVANCE: [{len(adv)}]f32 = [",
    ]
    for i in range(0, len(adv), 12):
        lines.append("    " + ", ".join(repr(float(v)) for v in adv[i:i + 12]) + ",")
    lines += ["]", "", "/* Where each glyph starts in POINTS, indexed like ADVANCE. */", f"pub START: [{len(start)}]u32 = ["]
    for i in range(0, len(start), 12):
        lines.append("    " + ", ".join(str(v) for v in start[i:i + 12]) + ",")
    lines += ["]", "", "/* Each glyph's contours, then -9; see font/bake.py. */", f"pub POINTS: [{len(flat)}]f32 = ["]
    for i in range(0, len(flat), 8):
        lines.append("    " + ", ".join(repr(float(v)) for v in flat[i:i + 8]) + ",")
    lines.append("]")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"{OUT}: {len(start)} glyphs, {len(flat)} numbers, widest contour {worst[0]} points (\"{worst[1]}\")")


if __name__ == "__main__":
    main()
