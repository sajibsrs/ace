# ui

Immediate-mode drawing and controls over [`sdl`](sdl.md). A program calls its
controls every frame; each call draws the control and returns what the pointer
did to it. The application owns all state, so there are no widget objects.

```ace
row: ui.Rect = area.inset(ui.Insets.all(4.0))
if ctx.button(1, row.take_left(80.0), shape, "Save", look).click { save() }
```

## Layout

`Rect.take_left` and `take_top` cut a strip from a rectangle, return it, and
leave the rest in the receiver. `inset`, `center` and `lead` place a box within
one. A gap is a cut whose result is ignored.

## Drawing

`Canvas` batches triangles for one layer: `fill`, `round`, `outline`, `dashed`,
`stroke`, `icon`, and text through `left` and `center`. Everything lands in
draw order; `flush` submits the layer. Icons are baked point data passed as an
`Icon`. The font is Lilex SemiBold, printable ASCII, 16 px; regenerate
`inc/font.ace` with `python3 -I font/bake.py` from `pkg/ui`.

## Controls

Fill `Ctx.x`, `y`, `canvas`, `pressed` and `released` from input, call controls,
then `Ctx.end`. `id`s are nonzero and unique per control. `blocked` keeps the
pointer from controls under a dialog.

`Events` holds `hover` and `held`, which last, and `press` and `click`, which
hold for one frame. A click is a release on the control that took the press.

## Design rationale

Immediate mode needs no retained tree, callbacks or widget lifetimes, which
suits a language with manual memory and no closures. Cuts lay out without
allocation. Their cost is that a parent cannot size from its children.
