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

[`demo/ui-gallery`](../demo/ui-gallery/main.ace) shows every control, layout
call, and canvas primitive in one window (`ace pkg run` there).

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
- `ui.height(h)` sets the next control's height, for a taller or shorter
  button than the theme's `height`.
- `ui.disable()` draws the next button faint; it takes no pointer and never
  reports a click.
- `ui.space(w)` moves the next control `w` further on.
- `ui.fill()` puts the rest of an across area at its far edge, the first
  control after it at the edge.
- `ui.place(w, h)` reserves a rectangle in the area for a custom control;
  `ui.track(id, box)` then reports what the pointer did to it as a
  `Response`: `hover` and `held`, which last, and `press` and `click`, which
  hold for one frame.

`ui.card(cells)` places a card of `cells` cells, each 40 high, across the
area, hairlines between them, and opens a down area over it, closed by
`end()`: each control, or each `row` line, takes one cell, inset `padding` at
the sides and centered on the cell's height. A debug build aborts when more
are placed than it has cells.

A control is `theme.height` high (a button 4 more, 2 above and below), a heading 18; a line of text is
`text_size()` high. `ui.last()` gives the last control's `Item`: `hovered`,
`held`, its `rect`, and a field's `submit`.

Below areas, `Rect.cut_left` and `cut_top` cut a strip from a rectangle,
return it, and leave the rest in the receiver; `inset` and the two `square`
forms place a box within one.

## Drawing

`Canvas` batches triangles for one layer: `fill` and `outline` (each with an
optional radius, and `fill(box, top, bottom, color)` with its own top and
bottom corners; an outline under a pixel thick draws a pixel's width at that
fraction of its alpha), `outline_dashed` (its gaps evened so a dash holds every
corner), `shadow(box, radius, blur, color)` (a soft shadow fading across
`blur` either side of the box's edge; the caller offsets the box), `stroke`,
`icon`, and `text`, placed by a pad or an `Align`. Everything lands in draw order; `flush` submits the layer. Icons
are baked point data passed as an `Icon`. The font is Lato Light, Regular,
and Bold (`Weight`), printable ASCII, each glyph advancing by its own
width (`text_width(s, w)` measures a line; no kerning). `set_text_size(px)` sets
the size in pixels per em (6 to 96, default 16) and layout follows it. Text is drawn
from a glyph atlas: each glyph is rasterized on first use at the current size
by exact area coverage (the baked outlines' x-height fitted to whole rows, a
quarter-pixel x phase, edges darkened 15%) and drawn as a textured quad; the
atlas is one 1024 px texture shared with every other shape, cleared when
full. `set_text_density(d)`, called with the renderer's scale, rasterizes at
size × `d` device pixels and draws at 1 / `d`, so text stays sharp on a
high-density display. Regenerate `inc/font.ace` (flattened
outlines) with `python3 -I font/bake.py` from `pkg/ui`.

## Theme

Controls draw from `Context.theme`, plain data. Its colors are `window` (for
the application's clear), `text`, `muted`, `heading` (section titles: black in light), `button` (a `Fill` for rest, hover
and press, behind buttons, unticked boxes, a picker's track, fields and
the hovered list row), `accent` (a toggle that is on, a ticked box, a
slider's fill, the chosen row, a caret), `on_accent` (text on it), `header` and `on_header` (a popup's darker title
strip and its white text), `card`
(a popup, card or focused field), `shadow` (a popup's shadow),
`separator` (between a card's cells), `track` (a switch that is off, a
slider's track and ticks),
`on` (a switch that is on), `knob` (a switch's and slider's knob), `segment`
(a picker's chosen segment), `lift` (the shadow a knob or segment casts) and
`contact` (the shadow 1 below a knob, segment or card). Its sizes are `height` (a control's), `radius`, `padding`, `gap` and
`icon` (an icon's side). Set it once, usually from
`Theme.init(Scheme.light)` or `Theme.init(Scheme.dark)`, which differ only in
color. For a variant, copy it, change a field for the controls that follow,
and assign the copy back after.

There is one look, in `inc/look.ace`: flat rounded fills; cards
rounder than a control, on a 1 px contact shadow; knobs
and a picker's segment raised by a shadow 2 below, blurred 6; and a popup
rounder still, raised by a shadow 18 below it, blurred 48. The theme changes
its colors and sizes; a different look is an edit to that file alone, since
where a control goes, its id, and what it does with the pointer and keyboard
live elsewhere. A control of an application's own builds on `place`, `track`
and `Context.canvas` and draws itself.

## Controls

Controls return `bool`: a button whether it was clicked, a value control
whether its value changed.

- `button(label)`, `button(icon, label)`, `button(icon)`; `button(icon,
  label, on)` rests on `accent` while `on`; `button(label, fill)` and
  `button(icon, label, fill)` use their own `Fill`, outlined in its `edge`
  and labeled in its `ink` when those have an alpha (else the label is
  near-black on a light fill and white on a dark one). A button's icon and label are centered together,
  however wide it is placed.
- `toggle(icon, label, &value)` and `toggle(icon, &value)` flip a `bool` and
  rest on `accent` while it is set.
- `toggle(label, &value)` is a switch: `label`, then a track at the far end,
  on `on` with its knob right while the `bool` is set. A click on the label
  or the track flips the `bool`, not the space between.
- `checkbox(label, &value)` ticks a box before its label; a click on the box or
  the label flips the `bool`, not the space after it.
- `picker(items, &chosen)` shows equal segments on a track, the one at
  `chosen` raised; a click (a release on the pressed segment) picks one, and
  it returns whether `chosen` changed. Across an area it is as wide as its
  labels, padded.
- `slider(&value, min, max)` drags a knob along a track for an `f32`, the
  track filled up to it; `slider(&value, min, max)` with `int`s stops at each
  whole number, the one nearest the pointer, with a tick under each when stops
  are 6 px apart or more. The knob stays inside the slider's box. A slider
  shows no value: the caller prints it, as a muted line beside or above it.
  Across an area a slider is 115 px wide unless `width` sets it.
- `field(&input, allow)` edits a caller's `TextInput` in one line, as wide as
  its buffer across an area; focused, it sits on `card` with an `accent`
  caret. A click or `ui.focus()` before it gives it the keyboard, and a press elsewhere takes it away; the application may also set
  `Context.focus`. Focused, it consumes every key and the frame's text, so an
  owner reads its own keys (Esc) first; within a frame, keys apply before
  text. `allow` lists the bytes it accepts. Enter sets `last().submit`.
- `list(items, &chosen, rows, &scroll)` shows `rows` of `items` from row
  `scroll` on, inset 4 with a faint line under each row, the chosen one on an `accent` pill and
  the hovered one on a `button` pill. A press on a row picks it. Under
  the pointer and overflowing, it consumes the wheel, a row a notch, and draws
  a thumb; otherwise the wheel passes on. `rows(n)` gives the height of a
  list of `n` rows, its inset included, for sizing what holds it; `Theme.row()` is one row's.
- `swatch(icon, color, selected)`: an icon in its own color, on the
  `button` rest gray while chosen; the pointer changes nothing. Square, a column wide in a grid.
- `text(s)` and `muted(s)` show a line, and `text(s, align)` and
  `muted(s, align)` one at `Align.left`, `center`, or `right` across the area;
  `heading(s)` and `heading(s, align)` a bold 18 px section title
  with no background;
  `readout(s)` a value centered on that fill.
- `popup(box)` opens a modal area on `card`: while it is up,
  from the next frame on, only controls inside it take the pointer.
  `popup(box, title)` adds a title strip `theme.title()` high across its top,
  flush with its edges, and opens the area below it.
- `key(code)` consumes a press of `code` with no modifiers.

`ui.line()` gives the context's scratch `fmt.Buffer`, emptied, for formatted
text: `l.printf(...)`, then `ui.text(l.text())`. The next call empties it.

A control is named by its label, by its icon when it shows no label, or by
the address of the value it edits: a toggle's, switch's, checkbox's or
slider's value, a picker's `chosen`, a field's buffer, a list's scroll. So a control keeps its id while its label
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
input, `inc/theme.ace` the theme, `inc/context.ace` the context with its ids
and layout, and `inc/controls.ace` the controls, which reach the context
through module-internal functions. Each control places and tracks itself,
then calls its drawing in `inc/look.ace`, which sees only the canvas, the
theme, and what the control passes it.

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
same one-frame lag.

Consuming input instead of routing it needs no event tree: call order is the
priority, so widgets go before shortcuts. Fields take an `allow` byte set
rather than a callback, since filtering by set needs no state. A list
scrolls by whole rows and does not clip, because batched triangles would have
to flush around a clip. `line` exists because a library function cannot
forward a `printf` format: `printf` is lowered at its call site.

Drawing sits in one file, apart from behavior, rather than behind a choice
of looks: one look is all the package draws, and keeping it in `inc/look.ace`
makes changing it an edit to one file with direct calls and no indirection.
If code outside the package ever needs to swap the drawing at run time, the
functions there can become a table of function values the context holds, as
Qt's `QStyle` is. `inc/theme.ace` sits apart from the context so `look.ace`
can import `Theme` without an import cycle.

Control fills are one neutral gray at an alpha rather than opaque colors, as
Apple's fill colors are: the same `button` reads on the window, a card, or a
popup, a tint of whatever is under it, so no surface needs its own control
color and the light and dark schemes differ only in the gray's alpha. A
scheme is an enum, not a `bool`, so a call site reads `Scheme.dark`, SDL's
`SDL_GetSystemTheme` maps onto it directly, and a high-contrast scheme can
join it. `height` is its own token rather than `icon` plus twice `gap`, as
it was: controls keep a fixed height on the 8-point grid whatever icon or
spacing a caller picks, and widening the gap no longer grows them.

A shadow is rings of the rounded box's outline at fixed distances from its
edge, joined by bands whose vertex colors SDL blends, rather than a blurred
texture: it batches with everything else and needs no render target or
shader. Five rings, at 0, 1 and 2 standard deviations either side of the
edge, follow the normal distribution's cover to within about 2% of the color, for
534 vertices; a single feathered band costs 174 but shows its ends. A
rounded outline is the same rings 2 to 4 deep, 240 or 360 vertices, and its
outer fade lands on the fill's, so a rim meets its card with no seam and no
larger fill behind it. A popup drawn with both adds 774 vertices to a frame;
ui-gallery's popup frame then drew 149,064. A popup sets itself off by its
shadow rather than by dimming the window, as the mockup's sheet does with a
light scrim, so a scrim, if wanted, is the application's.

The 1 px contact shadow under knobs, segments and cards is a plain rounded
fill 1 below the shape (174 vertices) rather than `shadow` with blur 1 (534):
a fill's own 1 px feathered edge is that blur. A knob's center travels from
half a knob inside one end of the slider's box to half inside the other, so
the knob never leaves the box a layout gave it, and a stepped slider takes the
stop nearest the pointer rather than equal zones, since the knob sits on stop
`i / (n - 1)` of that travel and an equal zone would put each stop's knob off
its zone's center. A slider draws no value: under the knob there is no room
for text, and a value beside it would make the travel depend on the text's
width, so a drag would feed back on itself; GTK's `Scale` (`draw_value` off
by default), Qt's `QSlider` and AppKit's `NSSlider` leave it to the caller
too. A list sits on its own padded card with pills for the chosen and hovered
rows, as a table inside a sheet, so it reads as one control wherever it is
placed; `rows(n)` includes the padding so a caller sizing a popup for it
stays exact. The switch is a `toggle` overload because `switch` is a keyword.
The step 3 controls add about 5,550 vertices to ui-gallery's frame (4.5%):
130,173 against 124,623 with the same five-row list, the bulk being knob and
segment shadows.
