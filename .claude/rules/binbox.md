---
paths:
  - "demo/binbox/**/*"
---

# binbox

The user-facing guide is `demo/binbox/README.md` (blocks, rules, controls,
`.bnbx` format, file map). Read it before changing behavior.

- Build and run from `demo/binbox` with `ace pkg build` / `ace pkg run`;
  `./ace main.ace` cannot resolve `sdl` (it comes from `ace.pkg`). Rebuild after
  every change before asking the owner to look.
- Icons: edit `icon/*.svg`, then `python3 -I icon/bake.py` regenerates
  `icon.ace` (never hand-edit it), then rebuild. Every stroked element draws at
  the fixed `ui.STROKE` (3.5 of 24), whatever its `stroke-width`; thinner lines
  must be filled shapes. The baker reads only M L H V C A Z and `<circle>`, no
  `transform`, and fills only simple polygons (build rings and outlines from
  hole-free pieces). Untracked SVGs cannot be restored by git: keep a copy
  before redrawing, and show drafts for approval before baking.
- Toolbar layout is `bar.place` (button widths, group gaps); icon sizes are
  drawn in `render.ace` and `program.ace`.
- Diagrams: one byte per cell after an 8-byte header; codes are `CODE` in
  `diagram.ace`. Verify a diagram with a temporary `*.test.ace` that loads it
  through `diagram.decode` and drives `sim.Sim` (`toggle`, `tick`, `level`),
  run as `ace test <file>`; delete it afterwards. Do not write a separate
  simulator.
