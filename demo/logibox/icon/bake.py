#!/usr/bin/env python3
"""Bakes the icons in this folder into ../icon.ace.

An element with a stroke is a bold line: its path flattens to a polyline that
the program draws as a line of fixed width. An element without one is a filled
shape: its path or circle flattens to a simple polygon, oriented clockwise on
screen, and is split into triangles here. Paths use only M m L l H h V v C c A a
Z z and <circle>. The table holds, per icon, a run of shapes on the 24 x 24 grid:

    -1, n, x0, y0 ... xn, yn           an open polyline of n points
    -3, n, x0, y0 ...                   a closed polyline of n points
    -2, n, x0, y0 ... , then 3 * (n - 2) vertex numbers     a polygon of n points
    -9                                  the end of the icon

    python3 -I icon/bake.py        (from demo/logibox)
"""

import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "icon.ace"
ORDER = [
    "source", "wire", "input", "output", "and", "or", "not", "nand", "nor",
    "xor", "xnor", "led", "bridge",
    "new", "open", "save", "save-as", "edit", "run", "step", "auto",
    "zoom-out", "zoom-in",
]
ARITY = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "A": 7, "Z": 0}
CURVE_STEPS = 10
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
            if any(inside(poly[m], a, b, c) for m in left if m not in (i, j, l)):
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
        for sh in shapes:
            if stroked:
                sh = [p for k, p in enumerate(sh) if k == 0 or math.dist(p, sh[k - 1]) > 1e-6]
                closed = len(sh) > 2 and math.dist(sh[0], sh[-1]) < 1e-6
                lines.append((sh[:-1] if closed else sh, closed))
                continue
            sh = [p for k, p in enumerate(sh) if k == 0 or math.dist(p, sh[k - 1]) > 1e-6]
            if math.dist(sh[0], sh[-1]) < 1e-6:
                sh = sh[:-1]
            if area(sh) < 0:
                sh.reverse()
            polys.append(sh)
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
        for p in polys:
            if len(p) > 48:
                sys.exit(f"{name}: polygon of {len(p)} points")
            flat += [-2.0, float(len(p))]
            for px, py in p:
                flat += [round(px, 2), round(py, 2)]
            flat += [float(v) for v in triangulate([(round(x, 2), round(y, 2)) for x, y in p])]
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
