# Packages

- `pkg/ui` popup titles sit on a darker `Theme.header` strip in white
  `Theme.on_header` bold text.
- `pkg/ui` `Fill` gains `ink`, the label and icon color a style sets (when it
  has an alpha; else `look.on` picks), so the light gallery's Danger, Success,
  Warning and Info buttons carry white labels on saturated fills. `look.on`
  goes white below luma 0.44, and a held `chosen` fill darkens 8%.
- `pkg/ui` follows Dear ImGui's model: controls, value controls included
  (`checkbox`, `slider`, `field`, `list`), are free functions on the context
  current between `begin` and `end`, placed in `Area`s and drawn from
  `Context.theme`; ids hash a label, icon or value address (FNV-1a).
- `pkg/sdl` creates system cursors: `Cursor.new(SystemCursor)`, released with
  `wipe()`, and made active with `sdl.cursor(c)`.
- `pkg/sdl` delivers `MouseWheelEvent` as an `Event` member, read through
  `Event.mouse_wheel()`.
- `pkg/sdl` `MouseMotionEvent.state` is now `MouseButtons`, replacing
  `buttons()`, since `bits` types cross the C boundary.
- `pkg/sdl` delivers mouse events: `MouseButtonEvent` and `MouseMotionEvent`
  as `Event` members, read through `Event.mouse_button()` and
  `Event.mouse_motion()`. Motion's `state` stays a `u32`, since a `bits` field
  makes the event struct non-C-ABI; `buttons()` reads it as `MouseButtons`.
- `regex.Output.new` is now `Output.init` (it wraps caller storage) and
  `Regex.new` is now `Regex.compile` (it does work on its input).
  `AGENTS.md` names memory-only constructors `new`/`init`/`wipe` and keeps a
  verb (`parse`, `compile`) for functions that do work (#53).
- `http`'s internal `exchange` takes a `tcp.Conn` and calls `tcp.send_all` and
  `net.receive` directly, replacing the single-instantiation `exchange[C]` and
  its function-value send/receive parameters.
- `sdl`: `Event.keyboard()` is now `Event.key()`, since a method may no longer
  share its name with the `keyboard` field.
- `pkg/regex` skips, while no thread is live, to the next byte that can begin
  a match, collected once per program from the entry's non-consuming paths
  (`str.find` for a single byte): `literal` 11.4x faster, `captures` 1.94x.
  `bench/package/regex.c` mirrors it.
- Added `demo/sdl3-cube-raytrace`: the cube-rotation scene traced on one
  thread, one ray per 2x2 cell, inverse-rotated into the cube frame for a slab
  test, Lambert-shaded, over a floor with shadow rays; cells draw as solid
  quads through `Renderer.geometry`.
- `pkg/sdl` fills geometry with a fragment shader on SDL's GPU render driver:
  `Renderer::new(window, driver)`, `Renderer.shader_formats`, `Shader`, and
  `RenderState` over SDL 3.4's GPU render state. `demo/sdl3-cube-raytrace-gpu`
  traces the raytrace scene per pixel with it, in SPIR-V or MSL.
- `demo/sdl3-cube-raytrace-gpu`'s Metal shader marks its fragment input
  `[[stage_in]]`, without which Metal rejected it and the demo failed on macOS.
- Text parsers report a failure position beside `Error` (#1):
  `json.Parser.parse` returns `(Value, Error, usize)` and `Json.parse`
  `(Error, usize)`, the byte offset of the failure (an unterminated string
  reports its opening quote), 0 on success, matching `regex.Syntax::parse`.
- `json.Parser.parse` and `Json.parse` take an optional `mut diag:
  fmt.Buffer` for error context (what was expected at line:column, then each
  enclosing element or key) and no longer return a byte offset.
- `regex.Syntax.parse` and `Regex.new` take an optional `mut diag:
  fmt.Buffer` for error context (the error at line:column, then each enclosing
  class or group) and no longer return a byte offset (Standard Library #23).
- `sdl.Window.refresh` returns the refresh rate of the window's display. binbox
  Auto ticks for what a frame has left after the last draw and a margin (the
  display's period, 60 Hz until known), and its Speed slider reaches 1 MHz; the
  footer shows the reached rate with three digits while Auto runs.
- binbox groups every touching block of one kind: wire, read pins and write
  pins form one net (a run of pins touching no device is an error), and touching
  LEDs, sources and same-span clocks are one block (`group.Groups`, `Sim`).
- binbox's maximum zoom is 2× (`view.ZOOM_MAX`).
- binbox has a dropper tool in the Tools widget, and the I key to select it: a
  click on a block makes it the pencil's block (`App.drop`).
- binbox bridge runs join pin to pin as well as wire to wire or pin to wire
  (`group.Groups`, `Sim.carries`); a run still goes straight and gates, LEDs,
  and sources never cross it.
- `sdl.mod_state` returns the modifier keys held now. binbox Cmd-drag on a
  selection carries a copy and leaves the original in place.
- binbox's Blocks widget icons are colored by default (each block's dimmer
  color, the selected one brighter) instead of grey until hovered; the pointer
  no longer changes them (`render.ace`).
- binbox's board is 256×128. The `.bnbx` header is 10 bytes (16-bit size) and
  the shipped diagrams sit in the top-left corner. `Sim`, the
  undo stacks, and `Selection` keep their per-cell tables on the heap
  (`Sim.new`/`wipe`, `History.wipe`, `Selection.wipe`) since they no longer fit
  the stack; gate numbers are `i32`.
- `http.serve(listener, context, router)` serves a caller-bound `tcp.Listener`
  forever; `serve(endpoint, ...)` binds and calls it. Threads share one
  listener with a `ServerContext` each (`demo/http-multi-threaded-server`).
- `demo/http-multi-threaded-event-loop-server` runs one `poll` queue per thread
  over non-blocking `net` sockets, with heap per-connection parse and write
  progress found through `poll` context and a 10-second deadline from accept.
- `http.ServerConnection` is sans-I/O per-client state: parsing, response
  framing, and pipelined leftovers. `Request` gained `version_minor` and
  `keep_alive()`, the parser skips empty lines before a request, and `serve`
  now uses it, so it writes `Content-Length`, honors HEAD, and no longer drops a
  response whose handler set `Connection`. The event-loop demo keeps connections
  alive, with a 5-second idle timeout and a 10-second per-request deadline.
  `ServerConnection` answers `Expect: 100-continue`; `net.close_write` and
  `tcp.close_write` half-close, and servers linger before closing.
- `pkg/sdl` gains render-target `Texture` (`new`, `blend`, `alpha`),
  `Renderer.target`, `Renderer.copy` and `BLEND_ADD`; binbox uses them for
  bloom, blurring high blocks through halved textures and adding them back.
- `pkg/ui` holds binbox's drawing layer and controls: `Canvas` (rounded boxes,
  icons, baked text), `Rect` cuts, and an immediate-mode `Ctx` whose `button`,
  `slider` and `row` return `Events`. binbox keeps its icons and app widgets.
- `pkg/ui` renames `Events` to `Response`; `Ctx` gains frame input (`scroll`,
  keys, text), `focus`, consuming `key_pressed`, `text_field` and `scroll_area`.
  `pkg/sdl` gains text-input events; binbox's Save-as box and Open list use
  these, and its shortcuts run after widgets on what they left.
- `pkg/ui` regroups its API around Ace types: `Ctx` becomes `Context` with
  `Mouse` and `Keyboard`, `ButtonStyle` becomes `Style` with `Fill` and `Lead`
  over `Color`, `TextInput` and `Scroll` hold control state, and overloads replace
  `round`, `left`, `center` and `lead`; strips are `cut_left` and `cut_top`.
- `[link]` gains fetched inputs: `url:` plus a required `sha256:` download through
  curl into `~/.ace/cache/<sha256>/`, verified on every use, only for
  the input actually linked. `framework:` becomes one comma list per section. A
  dependency with `[link]` sections but none for the target is a link error.
- First-party package tests move from per-package `tests/run.sh` harnesses of
  exit-code app cases to `std/test` files in each package's `test/`, run by
  `ace pkg test`, as is `make test-packages` (#149). sdl no longer skips itself
  without `libSDL3`; http's compile-only API case now also runs.
- `pkg/json`'s `escape` tests the common case (a byte written as itself) first
  rather than after seven compares: `bench/package/json` format 1.29x
  (15.6 → 12.1 µs per document).
- `pkg/ui`'s row height moved to the theme as `Theme.row()`, and
  `list_height(rows)` became `rows(n)` (Naming #7); every `pub` declaration has
  a doc comment (Comments #1). `font/bake.py` finds the icon baker in binbox.
- `pkg/ui` centers a button's icon and label however wide it is placed, adds
  `Align.right` with `text`, `muted`, and `heading` overloads taking an
  `Align`, and evens `outline_dashed`'s gaps so each corner holds a dash.
  `demo/ui-gallery` shows every control, layout call, and primitive.
- `pkg/ui` draws every control from `inc/look.ace`, apart from its placement,
  id and tracking; `Theme`, `Fill` and `Response` move to `inc/theme.ace`.
  `Theme.init(Scheme)` builds a light or dark palette: translucent gray fills,
  an accent and a card color, and a 28 px control `height` on an 8 px grid.
  `Canvas.shadow` and a rounded `outline` draw from rings of vertex-colored
  bands; a popup gets a shadow and an `edge` rim from the theme, and
  `popup(box, title)` a flush title strip, drawn by `fill(box, top, bottom,
  color)`. New controls: a switch (`toggle(label, &value)`), `picker`, and
  `card(cells)` for 40 px cells; sliders drop their text for a knob, stop dots
  and the nearest stop, and lists draw pills on a padded card. A control that
  is on fills with `Theme.chosen()`; a label on an opaque fill takes white or
  black by the fill's luminance.
- `pkg/ui` sets its text in Lato Light, Regular, and Bold instead of the
  monospaced Lilex: `font/bake.py` bakes all three with per-glyph advances,
  `text_width(s, w)` replaces `GLYPH_WIDTH`, and `Canvas.text` takes an
  optional `Weight`. `heading` is bold; a field or slider
  still sizes by `0.6 * text_size()` per character.
  `font/bake.py` bakes only flattened outlines and advances. Text is drawn
  from a size-independent glyph atlas: `set_text_size(px)` replaces the 16 px
  `TEXT_SIZE`, and each glyph is rasterized by exact area coverage per
  weight, size and quarter-pixel phase (x-height fitted to whole rows, 15%
  stem darkening) into one RGBA texture that also serves untextured
  geometry through a white texel. `set_text_density(d)` rasterizes for a
  scaled renderer's device pixels. `Texture.image`, `Texture.update`,
  `Renderer.geometry(texture, ...)` and `Renderer.flush` in `pkg/sdl`.
  Theme text colors are opaque (`flat`): a glyph's triangles overlap a little,
  and a translucent color showed the overlaps as dark dots.
- `pkg/ui` buttons: `ui.height(h)` sizes the next control, `ui.disable()` draws
  the next button faint and deaf to the pointer, `Fill.edge` outlines a button,
  and `button(label, fill)` styles a text-only one. The gallery shows button
  sizes and styles and scrolls the whole page with the wheel.
