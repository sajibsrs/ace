#!/usr/bin/env python3
"""Bakes the icons in this folder into ../icon.ace.

An element with a stroke is a bold line: its path flattens to a polyline that
the program draws as a line of fixed width. An element without one is a filled
shape: its path or circle flattens to a polygon, oriented clockwise on screen,
and is split into triangles here. A subpath of a filled path that lies inside
another subpath of it is a hole (even-odd): each hole is joined to its outline
by a zero-width bridge cut, which makes one simple polygon with a pair of
coincident bridge edges. Paths use only M m L l H h V v C c A a Z z and
<circle>. The table holds, per icon, a run of shapes on the 24 x 24 grid:

    -1, n, x0, y0 ... xn, yn           an open polyline of n points
    -3, n, x0, y0 ...                   a closed polyline of n points
    -2, n, x0, y0 ... , then 3 * (n - 2) vertex numbers     a polygon of n points
    -4, n, x0, y0 ... , then 3 * (n - 2) vertex numbers, then m and m pairs a, c
                                        a polygon with m bridges: the bridge edges
                                        run a -> a + 1 and c -> c + 1 (modulo n),
                                        a + 1 and c lie at one point, as do c + 1
                                        and a; the fringe skips them
    -9                                  the end of the icon

    python3 -I icon/bake.py        (from demo/binbox)
"""

import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "icon.ace"
ORDER = [
    "source", "wire", "read", "write", "and", "or", "not", "nand", "nor",
    "xor", "xnor", "led", "bridge", "clock", "toggle",
    "new", "open", "save", "save-as", "edit", "run", "step", "auto", "reset",
    "zoom-out", "zoom-in", "select", "home", "bars", "depth", "panel-left", "panel-right",
]
ARITY = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "A": 7, "Z": 0}
CURVE_STEPS = 10
SHAPE_MAX = 48  # also ui.SHAPE_MAX
ARC_STEP = math.pi / 12


def arc(x1, y1, rx, ry, rot, large, sweep, x2, y2):
    """SVG endpoint arc -> points after the start (circular arcs only)."""
    if rx != ry or rot != 0:
        sys.exit("elliptical or rotated arc")
    if (x1, y1) == (x2, y2):
        return []
    r = rx
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    d2 = dx * dx + dy * dy
    if d2 > r * r:
        r = math.sqrt(d2)
    k = math.sqrt(max(0.0, (r * r - d2) / d2))
    if large == sweep:
        k = -k
    cx = (x1 + x2) / 2 + k * dy
    cy = (y1 + y2) / 2 - k * dx
    a1 = math.atan2(y1 - cy, x1 - cx)
    a2 = math.atan2(y2 - cy, x2 - cx)
    da = a2 - a1
    if sweep and da < 0:
        da += 2 * math.pi
    if not sweep and da > 0:
        da -= 2 * math.pi
    n = max(2, math.ceil(abs(da) / ARC_STEP))
    pts = [(cx + r * math.cos(a1 + da * i / n), cy + r * math.sin(a1 + da * i / n)) for i in range(1, n)]
    return pts + [(x2, y2)]


def path(d):
    """Strokes (lists of points) of one path."""
    strokes, cur = [], []
    x = y = sx = sy = 0.0
    for cmd, args in re.findall(r"([MmLlHhVvCcAaZz])([^MmLlHhVvCcAaZz]*)", d):
        nums = [float(v) for v in re.findall(r"-?(?:\d+\.?\d*|\.\d+)", args)]
        up = cmd.upper()
        rel = cmd != up
        n = ARITY[up]
        if n == 0:
            if cur:
                cur.append((sx, sy))
                strokes.append(cur)
            cur = []
            x, y = sx, sy
            continue
        for i in range(0, len(nums), n):
            a = nums[i:i + n]
            if up == "M":
                if cur:
                    strokes.append(cur)
                nx, ny = (x + a[0], y + a[1]) if rel else (a[0], a[1])
                cur = [(nx, ny)]
                sx, sy = nx, ny
                x, y = nx, ny
                up = "L"  # implicit repeats of M are L
                continue
            if up == "L":
                nx, ny = (x + a[0], y + a[1]) if rel else (a[0], a[1])
                cur.append((nx, ny))
            elif up == "H":
                nx, ny = (x + a[0] if rel else a[0]), y
                cur.append((nx, ny))
            elif up == "V":
                nx, ny = x, (y + a[0] if rel else a[0])
                cur.append((nx, ny))
            elif up == "C":
                o = (x, y) if rel else (0, 0)
                p1 = (o[0] + a[0], o[1] + a[1])
                p2 = (o[0] + a[2], o[1] + a[3])
                p3 = (o[0] + a[4], o[1] + a[5])
                for s in range(1, CURVE_STEPS + 1):
                    t = s / CURVE_STEPS
                    u = 1 - t
                    cur.append((
                        u**3 * x + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                        u**3 * y + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1],
                    ))
                nx, ny = p3
            elif up == "A":
                nx, ny = (x + a[5], y + a[6]) if rel else (a[5], a[6])
                cur.extend(arc(x, y, a[0], a[1], a[2], int(a[3]), int(a[4]), nx, ny))
            x, y = nx, ny
    if cur:
        strokes.append(cur)
    return strokes


def circle(cx, cy, r):
    n = 24
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def area(poly):
    return sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
               for i in range(len(poly))) / 2


def cross(o, a, b):
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])


def inside(p, a, b, c):
    return cross(a, b, p) >= 0 and cross(b, c, p) >= 0 and cross(c, a, p) >= 0


def seg_cross(p, q, r, t):
    """True when segments pq and rt cross at a point inside both."""
    d1, d2 = cross(p, q, r), cross(p, q, t)
    d3, d4 = cross(r, t, p), cross(r, t, q)
    return d1 * d2 < -1e-9 and d3 * d4 < -1e-9


def on_seg(p, q, r):
    """True when r lies on segment pq, ends excluded."""
    if abs(cross(p, q, r)) > 1e-9:
        return False
    d = (q[0] - p[0]) * (r[0] - p[0]) + (q[1] - p[1]) * (r[1] - p[1])
    return 1e-9 < d < (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2 - 1e-9


def contains(poly, pt):
    """Even-odd point in polygon."""
    c = False
    for k in range(len(poly)):
        a, b = poly[k], poly[(k + 1) % len(poly)]
        if (a[1] > pt[1]) != (b[1] > pt[1]) and pt[0] < a[0] + (pt[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1]):
            c = not c
    return c


def merge(outline, holes):
    """Outline (positive area) and holes -> (one polygon, bridges as (a, c))."""
    nodes = [[p, ("o", i)] for i, p in enumerate(outline)]  # [point, role]
    segs = [(poly[k], poly[(k + 1) % len(poly)]) for poly in [outline] + holes for k in range(len(poly))]
    used = set()
    for hole in sorted(holes, key=lambda h: -max(x for x, _ in h)):
        best = None
        for i, q in enumerate(outline):
            if i in used:
                continue
            for j, p in enumerate(hole):
                d = math.dist(p, q)
                if best is not None and d >= best[0]:
                    continue
                if any(seg_cross(p, q, a, b) for a, b in segs):
                    continue
                if any(on_seg(p, q, v) for poly in [outline] + holes for v in poly):
                    continue
                mid = ((p[0] + q[0]) / 2, (p[1] + q[1]) / 2)
                if not contains(outline, mid) or any(contains(h, mid) for h in holes):
                    continue
                best = (d, i, j)
        if best is None:
            sys.exit("cannot bridge a hole")
        _, i, j = best
        used.add(i)
        segs.append((outline[i], hole[j]))
        at = next(k for k, n in enumerate(nodes) if n[1] == ("o", i))
        h = len(hole)
        ins = [[hole[(j + t) % h], None] for t in range(h + 1)] + [[outline[i], None]]
        ins[0][1] = ("b", i)
        ins[h][1] = ("c", i)
        ins[h + 1][1] = ("d", i)
        nodes[at + 1:at + 1] = ins
    bridges = [(next(k for k, n in enumerate(nodes) if n[1] == ("o", i)),
                next(k for k, n in enumerate(nodes) if n[1] == ("c", i))) for i in sorted(used)]
    return [n[0] for n in nodes], bridges


def triangulate(poly):
    """Ear clipping of a simple polygon with positive area; vertex numbers."""
    left = list(range(len(poly)))
    out = []
    while len(left) > 3:
        for k in range(len(left)):
            i, j, l = left[k - 1], left[k], left[(k + 1) % len(left)]
            a, b, c = poly[i], poly[j], poly[l]
            if cross(a, b, c) <= 1e-9:
                continue
            if any(inside(poly[m], a, b, c) for m in left if m not in (i, j, l) and poly[m] not in (a, b, c)):
                continue
            out += [i, j, l]
            left.pop(k)
            break
        else:
            sys.exit(f"cannot triangulate {poly}")
    return out + left


def shapes_of(name):
    svg = (HERE / f"{name}.svg").read_text()
    lines, polys = [], []
    for m in re.finditer(r'<(path|circle)\b([^>]*)/>', svg):
        attrs = dict(re.findall(r'([\w-]+)="([^"]*)"', m.group(2)))
        stroked = "stroke" in attrs
        if m.group(1) == "circle":
            shapes = [circle(float(attrs["cx"]), float(attrs["cy"]), float(attrs["r"]))]
        else:
            shapes = path(attrs["d"])
        rings = []
        for sh in shapes:
            if stroked:
                sh = [p for k, p in enumerate(sh) if k == 0 or math.dist(p, sh[k - 1]) > 1e-6]
                closed = len(sh) > 2 and math.dist(sh[0], sh[-1]) < 1e-6
                lines.append((sh[:-1] if closed else sh, closed))
                continue
            sh = [p for k, p in enumerate(sh) if k == 0 or math.dist(p, sh[k - 1]) > 1e-6]
            if math.dist(sh[0], sh[-1]) < 1e-6:
                sh = sh[:-1]
            rings.append(sh)
        # Even-odd: a ring inside an odd number of others is a hole of the
        # innermost one that holds it.
        owner = {}
        for k, ring in enumerate(rings):
            held = [m for m, other in enumerate(rings) if m != k and contains(other, ring[0])]
            if len(held) % 2:
                owner[k] = max(held, key=lambda m: sum(contains(rings[n], rings[m][0]) for n in range(len(rings)) if n != m))
        for k, ring in enumerate(rings):
            if k in owner:
                continue
            if area(ring) < 0:
                ring.reverse()
            holes = []
            for h, o in owner.items():
                if o == k:
                    holes.append(rings[h][::-1] if area(rings[h]) > 0 else rings[h])
            polys.append(merge(ring, holes) if holes else (ring, []))
    return lines, polys


def main():
    start, flat = [], []
    for name in ORDER:
        start.append(len(flat))
        lines, polys = shapes_of(name)
        for s, closed in lines:
            flat += [-3.0 if closed else -1.0, float(len(s))]
            for px, py in s:
                flat += [round(px, 2), round(py, 2)]
        for p, bridges in polys:
            if len(p) > SHAPE_MAX:
                sys.exit(f"{name}: polygon of {len(p)} points")
            flat += [-4.0 if bridges else -2.0, float(len(p))]
            for px, py in p:
                flat += [round(px, 2), round(py, 2)]
            flat += [float(v) for v in triangulate([(round(x, 2), round(y, 2)) for x, y in p])]
            if bridges:
                flat += [float(len(bridges))]
                for a, c in bridges:
                    flat += [float(a), float(c)]
        flat += [-9.0]
    members = [n.replace("-", "_") for n in ORDER]

    def fmt(v):
        return repr(float(v))

    lines = [
        "/* icon: the bar and block-row icons, flattened to lines and polygons on a 24 x 24 grid.",
        " *",
        " * Generated by icon/bake.py from icon/*.svg; edit those, not this file. The layout of",
        " * POINTS is described there.",
        " */",
        "",
        "pub Id: enum u8 { " + ", ".join(members) + " }",
        "",
        "/* Where each icon starts in POINTS. */",
        "pub START: [Id]u16 = [",
    ]
    lines += [f"    Id.{m} = {s}," for m, s in zip(members, start)]
    lines += ["]", "", f"pub POINTS: [{len(flat)}]f32 = ["]
    for i in range(0, len(flat), 8):
        lines.append("    " + ", ".join(fmt(v) for v in flat[i:i + 8]) + ",")
    lines.append("]")
    OUT.write_text("\n".join(lines) + "\n")
    print(f"{OUT}: {len(ORDER)} icons, {len(flat)} numbers")


main()
