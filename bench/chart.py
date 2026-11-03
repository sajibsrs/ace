#!/usr/bin/env python3
"""Draws the README's charts, docs/speed.svg and docs/compile.svg, from the
numbers below. Update them from bench/core/run-*.sh (C ns / Ace ns, cc -O2)
and bench/compiler's corpus table, then run: python3 bench/chart.py"""

import os

ACE = "#cc4152"  # the logo's red
C = "#8b8f98"
CPP = "#5b7fbf"
RUST = "#4a9d8f"
INK = "#888"  # reads on light and dark backgrounds
FONT = "-apple-system,Segoe UI,Helvetica,Arial,sans-serif"

# Speed as a multiple of plain cc -O2's (inlining and vectorization on: the
# C columns run-calls.sh and run-hotloop.sh call "C inlined" and "C vector"):
# Ace --release, clang++ -std=c++20 -O2, and rustc -C opt-level=2
# -C codegen-units=1 -C overflow-checks=off. C++ and Rust are measured
# against C built the same way (noinline in calls, vectorization on in
# hotloop's kernels).
SPEED = [
    ("method call", 1.0, 1.0, 1.0), ("plain calls", 1.0, 1.0, 1.0),
    ("particle vec3", 0.97, 1.0, 1.0), ("CPU mix", 0.93, 1.0, 0.94),
    ("ray tracer", 0.91, 1.01, 0.86), ("cube", 0.76, 1.0, 1.08),
    ("saxpy f32", 0.68, 1.0, 1.0), ("tree walk", 0.67, 1.03, 0.99),
]

# Seconds to compile the 5,000-function idiomatic generic corpus to an object, debug builds
# against debug builds and release against release.
# Two panels of 5,000 functions each: (title, [(group, bars)]).
COMPILE = [
    ("Plain C-shaped code", [
        ("Debug", [("Ace --debug", 0.13, ACE), ("cc -O0", 0.94, C),
                   ("clang++ -O0", 1.03, C), ("rustc -O0", 2.61, C)]),
        ("Release", [("Ace --release", 0.16, ACE), ("cc -O2", 7.78, C),
                     ("clang++ -O2", 7.86, C), ("rustc -O2", 3.48, C)]),
    ]),
    ("Idiomatic generic code", [
        ("Debug", [("Ace --debug", 0.309, ACE), ("cc -O0", 1.54, C),
                   ("clang++ -O0", 6.84, C), ("rustc -O0", 6.16, C)]),
        ("Release", [("Ace --release", 0.391, ACE), ("cc -O2", 9.69, C),
                     ("clang++ -O2", 120.8, C), ("rustc -O2", 12.8, C)]),
    ]),
]


def speed():
    w, h, l, r, t, b = 900, 330, 44, 8, 52, 28
    ph, top = h - t - b, 1.15
    y = lambda v: t + ph - v / top * ph
    g = (w - l - r) / len(SPEED)
    bw = g * 0.2
    keys = (("Ace --release", ACE), ("C -O2", C), ("C++ -O2", CPP), ("Rust -O2", RUST))
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" font-family="{FONT}">',
         f'<text x="{l}" y="20" font-size="18" font-weight="600" fill="{INK}">Runtime vs C (cc -O2) — higher is faster</text>']
    kx = w - 450
    for name, c in keys:
        o.append(f'<rect x="{kx}" y="7" width="14" height="14" fill="{c}"/><text x="{kx + 20}" y="19" font-size="13" fill="{INK}">{name}</text>')
        kx += 40 + 7.4 * len(name)
    for v in (0, 0.25, 0.5, 0.75, 1):
        o.append(f'<line x1="{l}" x2="{w - r}" y1="{y(v):.1f}" y2="{y(v):.1f}" stroke="{INK}" stroke-opacity="{0.5 if v == 0 else 0.18}"/>')
        o.append(f'<text x="{l - 8}" y="{y(v) + 4:.1f}" font-size="12" fill="{INK}" text-anchor="end">{v:g}x</text>')
    for i, (name, ace, cpp, rust) in enumerate(SPEED):
        cx = l + g * i + g / 2
        for j, (v, (_, c)) in enumerate(zip((ace, 1, cpp, rust), keys)):
            bx = cx - 2 * bw - 1.5 + j * (bw + 1)
            o.append(f'<rect x="{bx:.1f}" y="{y(v):.1f}" width="{bw:.1f}" height="{y(0) - y(v):.1f}" rx="2" fill="{c}"/>')
        o.append(f'<text x="{cx:.1f}" y="{y(0) + 18:.1f}" font-size="12" fill="{INK}" text-anchor="middle">{name}</text>')
    return o + ["</svg>"]


def compile_time():
    w, pw, l, r, t, bh, gap, head = 900, 440, 100, 56, 70, 26, 10, 30
    rows = max(sum(len(bars) for _, bars in groups) for _, groups in COMPILE)
    h = t + 2 * head + rows * (bh + gap) + 4
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" font-family="{FONT}">',
         f'<text x="0" y="20" font-size="18" font-weight="600" fill="{INK}">Time to compile 5,000 functions, single file — shorter is faster</text>']
    for i, (title, groups) in enumerate(COMPILE):
        px = i * (w - pw)
        top = max(v for _, bars in groups for _, v, _ in bars)
        x = lambda v: px + l + v / top * (pw - l - r)
        o.append(f'<text x="{px}" y="50" font-size="15" font-weight="600" fill="{INK}">{title}</text>')
        y = t
        for group, bars in groups:
            o.append(f'<text x="{px}" y="{y + 18}" font-size="13" font-weight="600" fill="{INK}" fill-opacity="0.7">{group}</text>')
            y += head
            for name, v, c in bars:
                label = f"{v:.1f} s" if v >= 1 else f"{v:.2f} s"
                o.append(f'<text x="{px + l - 10}" y="{y + bh / 2 + 5}" font-size="13" fill="{INK}" text-anchor="end">{name}</text>')
                o.append(f'<rect x="{px + l}" y="{y}" width="{max(x(v) - px - l, 3):.1f}" height="{bh}" rx="2" fill="{c}"/>')
                o.append(f'<text x="{x(v) + 6:.1f}" y="{y + bh / 2 + 5}" font-size="13" font-weight="600" fill="{c}">{label}</text>')
                y += bh + gap
    return o + ["</svg>"]

docs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
for name, lines in (("speed.svg", speed()), ("compile.svg", compile_time())):
    with open(os.path.join(docs, name), "w") as f:
        f.write("\n".join(lines) + "\n")
