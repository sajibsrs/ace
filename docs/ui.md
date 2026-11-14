# ui

Immediate-mode drawing and controls over [`sdl`](sdl.md). A program calls its
controls every frame; each call places the control, draws it, and returns
what input did to it. The application owns all state, so there are no widget
objects.

```ace
editor.ui.begin()
bar: ui.Area = ui.area(head, ui.Flow.across)
ui.text("File", editor.ui.theme.font.in(editor.ui.theme.muted))
if ui.button(save_icon, "Save") { save() }
ui.toggle(snap_icon, &snap)
ui.fill()
ui.text(name)
bar.end()
editor.ui.end()
```

The package is a skeleton: the mechanism (frames, layout, ids, input, the
canvas) and one plain control of each kind, in a light or dark palette and a
monospace font. [`demo/ui-fancy`](../demo/ui-fancy/main.ace) shows every
control, layout call, and canvas primitive in one window (`ace pkg run`
there), in a look of its own built on the public API (`fancy.ace`): Lato,
translucent fills, shadows, a pill switch and a knob slider. [`demo/ui-default`](../demo/ui-default/main.ace) shows the
same window in the package's own look; Cmd+D switches light and dark.

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
- `ui.row([1, 2, 1])` weights the columns instead, like CSS `fr`: the width
  less the gaps is shared by weight. `ui.row(cols)` and `ui.grid(cols, count)`
  take `ui.Col { fr, min, max }` (`0` no limit, at most 16 columns): a column
  whose share falls outside its limits is held there and the others share the
  rest, as CSS flex resolves it; minimums wider than the area overflow it.
  `ui.row([ui.Col { fr = 1.0, min = 180.0 }, ui.Col { fr = 3.0 }])` is a
  quarter-width sidebar never under 180.
- `ui.area_width()` and `ui.area_height()` are the open area's size, inside a
  card's padding. The UI is rebuilt each frame, so a breakpoint is an `if`:
  `if ui.area_width() < 600.0 { ui.row(1) } else { ui.row(3) }`.
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

A control is `theme.height` high (a button or picker 4 more, 2 above and below,
unless `ui.height` set it; an icon-only button is as wide as it is high); a line of text is
its style's `size` high. `ui.last()` gives the last control's `Item`: `hovered`,
`held`, its `rect`, and a field's `submit`.

Below areas, `Rect.cut_left` and `cut_top` cut a strip from a rectangle,
return it, and leave the rest in the receiver; `inset` and the two `square`
forms place a box within one.

## Drawing

`Canvas` batches triangles for one layer: `fill` and `outline` (each with an
optional radius, and `fill(box, top, bottom, color)` with its own top and
bottom corners, and `fill_until(box, radius, cut, color)` masked to the part
left of `cut`, with a straight edge there; an outline under a pixel thick draws a pixel's width at that
fraction of its alpha), `outline_dashed` (its gaps evened so a dash holds every
corner), `shadow(box, radius, blur, color)` (a soft shadow fading across
`blur` either side of the box's edge; the caller offsets the box), `stroke`,
`icon`, and `text(box, s, style)`, placed by the style's `Align`. Everything lands in draw order; `flush` submits the layer. Icons
are baked point data passed as an `Icon`.

Text is drawn in a `Text` style: `font`, `size` (pixels per em, 6 to 96),
`color`, `spacing` (extra advance per glyph) and `align`; `style.in(color)`
and `style.at(align)` give a copy with one field changed. A `Font` is baked
outline data for printable ASCII, each glyph advancing by its own width
(`text_width(s, style)` measures a line; no kerning). `ui.mono()` is the
package's own, Lilex SemiBold. `python3 -I font/bake.py FONT.ttf OUT.ace NAME
IMPORT` bakes one font into a module exporting `NAME(): Font`, `IMPORT` being
the path it imports `pkg/ui` by (`ui` from a package, a relative path inside
`pkg/ui`); `inc/font.ace` is `mono` baked this way, and ui-fancy bakes Lato's
three weights. Glyphs are rasterized on first use into a glyph atlas keyed by
font and size, by exact area coverage (x-height fitted to whole rows, a
quarter-pixel x phase, edges darkened 15%) and drawn as textured quads; the
atlas is one 1024 px texture shared with every other shape, cleared when
full. `set_text_density(d)`, called with the renderer's scale, rasterizes at
size × `d` device pixels and draws at 1 / `d`, so text stays sharp on a
high-density display.

## Theme

Controls draw from `Context.theme`, plain data: tokens, and a style per
component that `Theme.derive()` fills from them.

The tokens are colors and sizes. `window` is the application's clear,
`surface` fills cards, popups, lists and a focused field, `text` is the label
and icon color, `muted` hints and a list's scroll thumb, `accent` what is on
or chosen, `on_accent` text on it, `control` a control's resting fill,
`border` outlines and a card's separators, and `track` a slider's ticks.
`font` is the `Text` style labels start from; `derive` sets its color to
`text`. `height` is a control's height, `gap` the space between controls
and between an icon and its label, `padding` the inset inside one, and
`radius` its corners.

A component's style is built from parts:

- `Look` is how a part looks in one state: `fill`, `border` (its outline's
  color, which also colors its rules), `text` (its label's color), `tint`
  (what colors its icons) and `shadow`. `Look.fade(a)` scales every alpha.
- `States[T]` holds a `T` for `rest`, `hover`, `press` and `off`
  (disabled); `pick(response, disabled)` chooses off over press over hover
  over rest, and `States[T].all(v)` puts `v` in every state. `ui.Looks` is
  `States[Look]`, and `ui.Icons` is `States[Icon]`, an icon per state:
  `ui.Icons.all(i)` makes one from a single icon.
- `Box` is a frame: `radius`, `padding` (`Insets`), `border` (an outline
  width, 0 for none), `look` and `on` (`States[Look]`, the second while the
  control is on: checked, chosen, focused or highlighted), and `focus` (a
  `Shadow` glow round it while it has the keyboard).
- `Graphic` is an icon: its `size`, its `gap` to the label, and its `Fit`
  (`contain`, `cover`, `stretch`).
- `Rule` is a part's lines: their width `w`, their `gap` from what they mark,
  and `room`, the least space between two before they are drawn.

A style is one of three shapes, named for what it has: `Labeled` (a `box`
and a `label` `Text`), `Graphical` (those and a `graphic`) and `Ruled`
(those and a `rule`); `popup` is a bare `Box`. `glyphs` holds the icons the
package draws itself (`check`, `chevron`, `close`).

The rules:

- Only a `Look` varies with state; radius, padding, border width and sizes
  do not, so a control never changes shape on hover.
- Styles have no margins: layout owns spacing (`gap`, `space`, areas).
- Content goes in calls (labels, icons, values), style in the theme.
- `Look.tint` colors icons; `Look.border` colors both outlines and rules.

`derive()` fills every style from the tokens. A control's gray fills come
from `control`: hover steps it 10/255 per channel toward `text`, press 20/255,
so they lighten on a dark scheme and darken on a light one. `on` and
`primary` use `accent`, lightened 12% on hover and darkened 8% on press.
Off looks are each state's at 40% alpha. `Theme.init(scheme)` sets the
tokens for `Scheme.light` or `Scheme.dark` (flat fills, thin borders, no
shadows) and calls `derive`.

What each style's fields mean, as `derive` fills them and the controls draw
them (a dash: unused):

| Style | `box.look` | `box.on` | `padding` | `border` | `radius` | `focus` | `graphic` | `rule` |
|---|---|---|---|---|---|---|---|---|
| `button` | gray fill, label, icon tint | on fill (`button(icons, label, on)`) | sides | outline | corners | — | icon side, gap to label | — |
| `primary` | accent fill | same | sides | outline | corners | — | as button | — |
| `toggle` | gray fill | while set | sides | outline | corners | — | as button | — |
| `swatch` | none (icon `tint`) | gray fill while selected | — | outline | corners | — | icon side | — |
| `slider` | rest: bar fill, `text` the value, `border` the ticks | rest `fill`: the filled part | — | outline | bar corners | — | — | ticks: width, drop under bar, least spacing |
| `card` | rest: fill, outline, separator color | — | sides | outline | corners | — | — | separators: width, left inset |
| `menu` | an item at rest | the highlighted item | label inset (left past the check) | — | corners | — | — | dividers: width, side inset, `room` = divider height |
| `field` | unfocused fill, text, `tint` caret | focused look | text inset | outline | corners | glow while focused | — | — |
| `check` | the box: gray fill, outline | ticked: accent fill, `tint` tick | tick inset in box | outline | box corners | — | `size` box side, `gap` to label | — |
| `picker` | rest: track fill, label | rest: chosen segment fill, label | sides | outline | corners | — | — | — |
| `tip` | rest: fill, outline, text | — | sides | outline | corners | — | — | — |
| `header` | rest: strip fill, title text | — | title inset | — | top corners | — | — | — |
| `list` | a row by state (rest clear); rest `tint` the scroll thumb | chosen row | card inset | outline | row corners | — | — | — |
| `popup` (a `Box`) | rest: fill, outline, shadow | — | — | outline | corners | — | — | — |

Notes per component:

- The slider's `on.rest.fill` is the filled part of the bar, and
  `look.rest.border` colors the ticks, drawn when stops are `rule.room` apart.
- The field's `on` is its focused look; `tint` colors the caret.
- `menu` is one item: `look` at rest and `on` while highlighted; its rule is
  the dividers between groups, `room` their height. pkg/ui has no menu
  control; the demos draw menus from this style.
- The list's resting `tint` is its scroll thumb.
- The check's `graphic.size` is the box's side and `graphic.gap` the gap to
  its label, drawn in `label.color`.

Set it once, usually from `Theme.init`. For a variant, copy it, change
tokens and call `derive()`, then override what tokens cannot say, and assign
it back: ui-fancy's `fancy.ace` changes the palette, fonts and radius,
derives, then sets the popup's shadow, the header's strip and the field's
glow. For one control, pass a style:

```ace
t: *ui.Theme = &ctx.theme
ui.button(save_icon, "Save", &t.primary)
danger: ui.Graphical = t.button
danger.box.look = t.states(red, red_hover, red_press)
if ui.button("Delete", &danger) { remove() }
ui.button(ui.Icons.all(close_icon), "", false)
```

`t.states(rest, hover, press)` builds `Looks` filled in those colors, its
label and icons picked to read on `rest` (`Theme.on(fill)`: `text` over a
translucent fill, `on_accent` over a dark one, near-black over a light one),
faint while off; `t.states(rest, hover, press, ink)` sets the ink.

A control of an application's own builds on `place`, `track` and
`Context.canvas` and draws itself.

## Controls

Controls return `bool`: a button whether it was clicked, a value control
whether its value changed.

- `button(label)`, `button(icon, label)`, and `button(icons, label, on)`,
  which draws the `on` looks while `on`; an icon alone takes an empty label,
  `button(icon, "")`. With a trailing style each makes six forms:
  `button("Save", &t.primary)`. A button's icon and label are centered
  together, however wide it is placed.
- `toggle(icon, label, &value)` and `toggle(icons, label, &value)` flip a
  `bool` and draw the `on` looks while it is set.
- `checkbox(label, &value)` ticks a box before its label; a click on the box or
  the label flips the `bool`, not the space after it.
- `picker(items, &chosen)` shows equal segments on a track, the one at
  `chosen` raised; a click (a release on the pressed segment) picks one, and
  it returns whether `chosen` changed. Across an area it is as wide as its
  labels, padded.
- `slider(&value, min, max)` is a bar dragged across to set an `f32`, filled
  with the accent up to it and showing it to two places; with `int`s it stops
  at each whole number, the one nearest the pointer, shows it, and has a tick
  under each stop when they are 6 px apart or more.
  `slider(&value, min, max, label)` shows `label` in place of the number,
  for a stop that stands for something else, such as a rate.
  Across an area a slider is 115 px wide unless `width` sets it.
- `field(&input, allow)` edits a caller's `TextInput` in one line, as wide as
  its buffer across an area; focused, it draws its `on` look (`surface`, an
  `accent` outline) in its box's `focus` glow, with the caret in `tint`. While empty it shows `TextInput.hint` in `muted`. A click or `ui.focus()` before it gives it the keyboard, and a press elsewhere takes it away; the application may also set
  `Context.focus`. Focused, it consumes every key and the frame's text, so an
  owner reads its own keys (Esc) first; within a frame, keys apply before
  text. `allow` lists the bytes it accepts. Enter sets `last().submit`.
- `list(items, &chosen, rows, &scroll)` shows `rows` of `items` from row
  `scroll` on, inset 4, the chosen one on an `accent` pill and
  the hovered one on a gray pill. A press on a row picks it. Under
  the pointer and overflowing, it consumes the wheel, a row a notch, and draws
  a thumb; otherwise the wheel passes on. `rows(n)` gives the height of a
  list of `n` rows, its inset included, for sizing what holds it; `Theme.row()` is one row's.
- `swatch(icon, color, selected)` and `swatch(icons, color, selected)`: an
  icon in its own color, on the resting control gray while chosen; the pointer changes nothing. Square, a column wide in a grid.
- `text(s)` shows a line in the theme's text, and `text(s, style)` one in
  `style`, as wide as it at `Align.left` and across the area at `center` or
  `right`; `readout(s)` a value centered on a resting button's fill.
- `popup(box)` opens a modal area framed by `theme.popup`, a bare `Box`:
  while it is up, from the next frame on, only controls inside it take the
  pointer.
  `popup(box, title)` adds `title` on a `theme.header` strip `Theme.title()` high
  across its top; its controls go down the rest.
- `key(code)` consumes a press of `code` with no modifiers.
- `tooltip(text)` after a control shows `text` by the pointer once it has
  rested on that control for half a second (30 frames), drawn at
  `Context.end` over everything; a press hides it until the pointer leaves.

`ui.line()` gives the context's scratch `fmt.Buffer`, emptied, for formatted
text: `l.printf(...)`, then `ui.text(l.text())`. The next call empties it.

A control is named by its label, by its icon when it shows no label, or by
the address of the value it edits: a toggle's, checkbox's or
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
`States.pick(response, disabled)` picks a style's `Look` for the pointer's
state.

## Files

`mod.ace` is the module's surface: type aliases and the functions above.
`inc/canvas.ace` holds rectangles, drawing and `Text`, `inc/face.ace` the `Font` type, `inc/font.ace` the baked mono font, `inc/input.ace` the frame's
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

The theme is tokens plus per-component styles rather than one flat set of
colors, as CSS custom properties feed component rules, Flutter's
`ThemeData` holds a `ButtonStyle` per button kind, and SwiftUI's
`ButtonStyle` restyles one kind: a flat theme needs a new field for each
place a color differs,
while a style per component holds every such difference with no new names.
One `Look` varies with state, through `States`, and nothing else does, as
Flutter's `WidgetStateProperty` resolves per state; geometry that held still
under the pointer is the rule, so it is not per-state data. Parts are named
by shape (`Labeled`, `Graphical`, `Ruled`) rather than by component, so
fourteen styles share three types and one drawing helper per part.
`derive` makes the tokens enough for a new palette, as Material derives a
scheme from seed colors, while a theme can still override any style after.
Styles carry no margins because the layout already owns spacing; two owners
would disagree.

Drawing sits in one file, apart from behavior, rather than behind a choice
of looks: one look is all the package draws, and keeping it in `inc/look.ace`
makes changing it an edit to one file with direct calls and no indirection.
If code outside the package ever needs to swap the drawing at run time, the
functions there can become a table of function values the context holds, as
Qt's `QStyle` is. `inc/theme.ace` sits apart from the context so `look.ace`
can import `Theme` without an import cycle.

The package is mechanism plus one plain control of each kind; a look is
the application's. Shadows, glows, rounder panels and translucent fills are
`Theme` data, so a demo restyles every control without touching the package,
and controls that are a look rather than a kind (a pill switch beside the
checkbox, a knob slider beside the slider) live with the look, built from
`place`, `track`, `id` and the canvas as any custom control is. Keeping the
package plain keeps its surface small and its default legible anywhere. A
scheme is an enum, not a `bool`, so a call site reads `Scheme.dark`, SDL's
`SDL_GetSystemTheme` maps onto it directly, and a high-contrast scheme can
join it. `height` is its own token rather than an icon's side plus twice `gap`:
controls keep a fixed height whatever icon or spacing a caller picks. A font
is data passed in a `Text` rather than a global weight and size, so faces and
sizes mix in one frame and the atlas keys glyphs by both.

A shadow is rings of the rounded box's outline at fixed distances from its
edge, joined by bands whose vertex colors SDL blends, rather than a blurred
texture: it batches with everything else and needs no render target or
shader. Five rings, at 0, 1 and 2 standard deviations either side of the
edge, follow the normal distribution's cover to within about 2% of the color, for
534 vertices; a single feathered band costs 174 but shows its ends. A
rounded outline is the same rings 2 to 4 deep, 240 or 360 vertices, and its
outer fade lands on the fill's, so a rim meets its card with no seam and no
larger fill behind it. A popup drawn with both adds 774 vertices to a frame;
ui-fancy's popup frame then drew 149,064. A popup sets itself off by its
shadow rather than by dimming the window, as the mockup's sheet does with a
light scrim, so a scrim, if wanted, is the application's.

A knob's center travels from
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
stays exact.
