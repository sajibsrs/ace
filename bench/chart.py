#!/usr/bin/env python3
"""Draws the README's charts, docs/speed.svg and docs/compile.svg, from the
numbers below. Update them from bench/core/run-*.sh (C ns / Ace ns, cc -O2)
and bench/compiler's corpus table, then run: python3 bench/chart.py"""

import os

ACE = "#cc4152"  # the logo's red
C = "#8b8f98"
INK = "#888"  # reads on light and dark backgrounds
FONT = "-apple-system,Segoe UI,Helvetica,Arial,sans-serif"

# Ace --release speed as a multiple of plain cc -O2's (inlining and
# vectorization on: the C columns run-calls.sh and run-hotloop.sh call
# "C inlined" and "C vector").
SPEED = [
    ("method call", 1.0), ("plain calls", 1.0), ("particle vec3", 0.97),
    ("CPU mix", 0.93), ("ray tracer", 0.91), ("cube", 0.76),
    ("saxpy f32", 0.68), ("tree walk", 0.67),
]

# Seconds to compile the 20,000-function corpus to an object.
COMPILE = [
    ("Ace --debug", 0.585, ACE), ("Ace --release", 0.692, ACE),
    ("cc -O0", 3.62, C), ("cc -O2", 30.9, C),
]


def speed():
    w, h, l, r, t, b = 900, 330, 44, 8, 52, 28
    ph, top = h - t - b, 1.15
    y = lambda v: t + ph - v / top * ph
    g = (w - l - r) / len(SPEED)
    bw = g * 0.32
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" font-family="{FONT}">',
         f'<text x="{l}" y="20" font-size="18" font-weight="600" fill="{INK}">Ace runtime vs C (cc -O2) — higher is faster</text>',
         f'<rect x="{w - 240}" y="7" width="14" height="14" fill="{ACE}"/><text x="{w - 220}" y="19" font-size="13" fill="{INK}">Ace --release</text>',
         f'<rect x="{w - 120}" y="7" width="14" height="14" fill="{C}"/><text x="{w - 100}" y="19" font-size="13" fill="{INK}">C -O2</text>']
    for v in (0, 0.25, 0.5, 0.75, 1):
        o.append(f'<line x1="{l}" x2="{w - r}" y1="{y(v):.1f}" y2="{y(v):.1f}" stroke="{INK}" stroke-opacity="{0.5 if v == 0 else 0.18}"/>')
        o.append(f'<text x="{l - 8}" y="{y(v) + 4:.1f}" font-size="12" fill="{INK}" text-anchor="end">{v:g}x</text>')
    for i, (name, v) in enumerate(SPEED):
        cx = l + g * i + g / 2
        o.append(f'<rect x="{cx - bw - 1:.1f}" y="{y(v):.1f}" width="{bw:.1f}" height="{y(0) - y(v):.1f}" rx="2" fill="{ACE}"/>')
        o.append(f'<rect x="{cx + 1:.1f}" y="{y(1):.1f}" width="{bw:.1f}" height="{y(0) - y(1):.1f}" rx="2" fill="{C}"/>')
        o.append(f'<text x="{cx - bw / 2 - 1:.1f}" y="{y(v) - 6:.1f}" font-size="12" font-weight="600" fill="{ACE}" text-anchor="middle">{v:.2f}x</text>')
        o.append(f'<text x="{cx:.1f}" y="{y(0) + 18:.1f}" font-size="12" fill="{INK}" text-anchor="middle">{name}</text>')
    return o + ["</svg>"]


def compile_time():
    w, l, r, t, bh, gap = 900, 120, 70, 44, 30, 14
    h = t + len(COMPILE) * (bh + gap) + 4
    top = max(v for _, v, _ in COMPILE)
    x = lambda v: l + v / top * (w - l - r)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" font-family="{FONT}">',
         f'<text x="0" y="20" font-size="18" font-weight="600" fill="{INK}">Time to compile 20,000 functions — shorter is faster</text>']
    for i, (name, v, c) in enumerate(COMPILE):
        y = t + i * (bh + gap)
        label = f"{v:.1f} s" if v >= 1 else f"{v:.2f} s"
        o.append(f'<text x="{l - 10}" y="{y + bh / 2 + 5}" font-size="13" fill="{INK}" text-anchor="end">{name}</text>')
        o.append(f'<rect x="{l}" y="{y}" width="{max(x(v) - l, 3):.1f}" height="{bh}" rx="2" fill="{c}"/>')
        o.append(f'<text x="{x(v) + 8:.1f}" y="{y + bh / 2 + 5}" font-size="13" font-weight="600" fill="{c}">{label}</text>')
    return o + ["</svg>"]


docs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
for name, lines in (("speed.svg", speed()), ("compile.svg", compile_time())):
    with open(os.path.join(docs, name), "w") as f:
        f.write("\n".join(lines) + "\n")
