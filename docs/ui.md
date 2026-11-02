# ui

Immediate-mode drawing and controls over [`sdl`](sdl.md). A program calls its
controls every frame; each call places the control, draws it, and returns
what input did to it. The application owns all state, so there are no widget
objects.

```ace
editor.ui.begin()
bar: ui.Area = ui.area(head, ui.Flow.across)
ui.muted("File")
if ui.button(save_icon, "Save") { save() }
ui.toggle(snap_icon, &snap)
ui.fill()
ui.text(name)
bar.end()
editor.ui.end()
```

## Frames

The application owns a `Context`, all zeros at start, and sets its `canvas`
and `theme`. Each frame it fills `mouse` from input, with `keyboard.add` for
the frame's keys and text (bounded; the window needs `start_text_input`),
then calls `begin`, which makes the context current: every control is a free
function acting on it. After the controls it reads its own shortcuts from
what they left, and `end` clears the frame's input. Every area and id scope
closes before `end`.

## Layout

An `Area` is a region controls are placed in, opened by `ui.area(box)` and
closed by `end()`, which suits `defer`. Areas nest and close in reverse.
Down an area (the default) each control is as wide as it; across one
(`ui.Flow.across`) each is as wide as its content and centered on the area's
height, the next `theme.gap` further on.

- `ui.row(n)` puts the next `n` controls on one line in equal columns; the
  line ends after them.
- `ui.grid(n, count)` puts the next `count` controls in lines of `n` equal
  columns.
- `ui.width(w)` sets the next control's width; `ui.width(label)` makes it as
  wide as it would be showing `label`, so a button whose label changes keeps
  its size.
- `ui.space(w)` moves the next control `w` further on.
- `ui.fill()` puts the rest of an across area at its far edge, the first
  control after it at the edge.
- `ui.place(w, h)` reserves a rectangle in the area for a custom control;
  `ui.track(id, box)` then reports what the pointer did to it as a
  `Response`: `hover` and `held`, which last, and `press` and `click`, which
  hold for one frame.

A control is a frame `theme.icon + 2 * theme.gap` high; a line of text is
`TEXT_SIZE` high. `ui.last()` gives the last control's `Item`: `hovered`,
`held`, its `rect`, and a field's `submit`.

Below areas, `Rect.cut_left` and `cut_top` cut a strip from a rectangle,
return it, and leave the rest in the receiver; `inset` and the two `square`
forms place a box within one.

## Drawing

`Canvas` batches triangles for one layer: `fill` (with an optional radius),
`outline`, `outline_dashed`, `stroke`, `icon`, and `text`, placed by a pad or
an `Align`. Everything lands in draw order; `flush` submits the layer. Icons
are baked point data passed as an `Icon`. The font is Lilex SemiBold,
printable ASCII, 16 px; regenerate `inc/font.ace` with
`python3 -I font/bake.py` from `pkg/ui`.

## Theme

Controls draw from `Context.theme`: `text`, `muted`, `header`, `button` (a
`Fill` for rest, hover and press), `selected`, `field`, `radius`, `padding`,
`gap` and `icon` (an icon's side). Set it once. For a variant, copy it, change
a field for the controls that follow, and assign the copy back after.

## Controls

Controls return `bool`: a button whether it was clicked, a value control
whether its value changed.

- `button(label)`, `button(icon, label)`, `button(icon)`; `button(icon,
  label, on)` rests in `selected` while `on`; `button(icon, label, fill)`
  uses its own `Fill`.
- `toggle(icon, label, &value)` and `toggle(icon, &value)` flip a `bool` and
  rest in `selected` while it is set.
- `checkbox(label, &value)` ticks a box before its label; a click anywhere on
  it flips the `bool`.
- `slider(&value, min, max)` drags an `f32` across its bar and shows it.
  `slider(&value, min, max, text)` drags an `int` over equal zones, one for
  each whole number, fills to the far edge of the value's zone, and shows
  `text`. Across an area a slider is twelve glyphs wide unless `width` sets
  it.
- `field(&input, allow)` edits a caller's `TextInput` in one line, as wide as
  its buffer across an area. A click or `ui.focus()` before it gives it the
  keyboard, and a press elsewhere takes it away; the application may also set
  `Context.focus`. Focused, it consumes every key and the frame's text, so an
  owner reads its own keys (Esc) first; within a frame, keys apply before
  text. `allow` lists the bytes it accepts. Enter sets `last().submit`.
- `list(items, &chosen, rows, &scroll)` shows `rows` of `items` from row
  `scroll` on, every other row on `field`. A press on a row picks it. Under
  the pointer and overflowing, it consumes the wheel, a row a notch, and draws
  a thumb; otherwise the wheel passes on. `list_height(rows)` gives its
  height, for sizing what holds it.
- `swatch(icon, color, selected)`: an icon in its own color, on `selected`
  while chosen; the pointer changes nothing. Square, a column wide in a grid.
- `text(s)` and `muted(s)` show a line; `heading(s)` a section title on a
  `header` strip that reaches half the padding past the area's sides;
  `readout(s)` a value centered on `header`.
- `popup(box)` opens a modal area filled as a button rests: while it is up,
  from the next frame on, only controls inside it take the pointer.
- `key(code)` consumes a press of `code` with no modifiers.

`ui.line()` gives the context's scratch `fmt.Buffer`, emptied, for formatted
text: `l.printf(...)`, then `ui.text(l.text())`. The next call empties it.

A control is named by its label, by its icon when it shows no label, or by
the address of the value it edits: a toggle's, checkbox's or slider's value,
a field's buffer, a list's scroll. So a control keeps its id while its label
changes. `push_id(n)` opens a scope in which the same name names a different
control, as in a loop; `pop_id` closes it. `ui.id(...)` gives an id. A debug
build aborts when two controls share an id in one frame.

An input a widget uses is consumed, so application shortcuts run after the
controls and see only the rest: unused `keyboard.keys` entries, and
`mouse.wheel` if nonzero. `keyboard.consume(code, mod)` consumes one key with
exactly those modifier groups, and `keyboard.consume()` the frame's typed
text. A click is a release on the control that took the press; a list picks
on the press, so the press can also close what holds it.

A custom control takes its rectangle from `place`, its state from `track`
with an id from `id`, and draws on `Context.canvas` from `Context.theme`;
`Fill.shade(response)` picks a fill's color for the pointer's state.

## Files

`mod.ace` is the module's surface: type aliases and the functions above.
`inc/canvas.ace` holds rectangles and drawing, `inc/input.ace` the frame's
input, `inc/context.ace` the context with its ids and layout, and
`inc/controls.ace` the controls, which reach the context through
module-internal functions.

## Design rationale

The model is Dear ImGui's: a control is an id, a measured size, a place from
the layout cursor (`place`, as ImGui's `ItemSize`/`ItemAdd`), the pointer's
state for it (`track`, as `ButtonBehavior`), and drawing from the theme.
Immediate mode needs no retained tree, callbacks or widget lifetimes, which
suits a language with manual memory and no closures; areas lay out without
allocation.

Ids come from what a control already has, as in ImGui and egui, because a
control must keep one id while it holds a drag, and a call-order counter
would shift whenever a control before it appears. ImGui's `##` suffix is not
needed: an icon-only button has its icon, and a control whose label changes
edits a value whose address is stable.

ImGui's `SameLine` and its push/pop style stacks are replaced by smaller
pieces: `row(n)` ends its line by itself, so there is nothing to forget to
close, and a theme is a plain value that a caller copies and restores. An
area is a value closed by a method so that `defer` pairs it with its opening.

`mod.ace` wraps the implementation rather than holding it because a method
lives in its type's file and an unmarked field is private to that file, so
the context, its layout and ids stay in one file whose internals the controls
reach through functions the facade does not export.

A current context lets calls read `ui.button("Save")` instead of passing the
context through every drawing function; the application still owns it, and
`begin` and `end` bound when it is current. A popup blocks from its second
frame, because what lies under it was drawn first; ImGui's modals have the
same one-frame lag. A heading reaches into the area's padding as ImGui's
collapsing header does, so headers stand out from their content without a
second area.

Consuming input instead of routing it needs no event tree: call order is the
priority, so widgets go before shortcuts. Fields take an `allow` byte set
rather than a callback, since filtering by set needs no state. A list
scrolls by whole rows and does not clip, because batched triangles would have
to flush around a clip. `line` exists because a library function cannot
forward a `printf` format: `printf` is lowered at its call site.
