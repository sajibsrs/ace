# ui

Immediate-mode drawing and controls over [`sdl`](sdl.md). A program calls its
controls every frame; each call draws the control and returns what input did
to it. The application owns all state, so there are no widget objects.

```ace
row: ui.Rect = area.inset(ui.Insets.all(4.0))
if context.button(1, row.cut_left(80.0), shape, "Save", look).click { save() }
```

## Layout

`Rect.cut_left` and `cut_top` cut a strip from a rectangle, return it, and
leave the rest in the receiver. `inset` and the two `square` forms place a box
within one. A gap is a cut whose result is ignored.

## Drawing

`Canvas` batches triangles for one layer: `fill` (with an optional radius),
`outline`, `outline_dashed`, `stroke`, `icon`, and `text`, placed by a pad or an
`Align`. Everything lands in
draw order; `flush` submits the layer. Icons are baked point data passed as an
`Icon`. The font is Lilex SemiBold, printable ASCII, 16 px; regenerate
`inc/font.ace` with `python3 -I font/bake.py` from `pkg/ui`.

## Controls

Fill `Context.canvas` and `mouse` from input, with `keyboard.add` for the
frame's keys and text (bounded; the window needs `start_text_input`). Call
controls, read shortcuts, then `Context.end` clears it all. `id`s are nonzero and unique per control. `blocked` keeps the pointer
from controls under a dialog.

`Response` holds `hover` and `held`, which last, and `press`, `click`, `changed`
and `submit`, which hold for one frame. A click is a release on the control
that took the press.

An input a widget uses is consumed, so application shortcuts run after the
controls and see only the rest: unused `keyboard.keys` entries, and `mouse.wheel`
if nonzero. `keyboard.consume(code, mod)` consumes one key with exactly those modifier groups.

`text_field` edits a caller's `TextInput`. A click takes `focus`, and a press elsewhere
drops it; the application may also set `focus`. Focused, it consumes every key
and the frame's text, so an owner reads its own keys (Esc) first. Enter gives
`submit`; `allow` lists the bytes it accepts. Within a frame, keys apply before
text.

`scroll_bar` keeps a `Scroll` offset in range and consumes the wheel when the pointer
is over content that overflows; otherwise the wheel passes on. It draws a
thumb, and the caller draws the rows that the offset shows.

## Design rationale

Immediate mode needs no retained tree, callbacks or widget lifetimes, which
suits a language with manual memory and no closures. Cuts lay out without
allocation. Their cost is that a parent cannot size from its children.

Consuming input instead of routing it needs no event tree: call order is the
priority, so widgets go before shortcuts. Fields take an `allow` byte set
rather than a callback, since filtering by set needs no state. `scroll_bar`
does not clip, because batched triangles would have to flush around a clip;
callers show whole rows.
