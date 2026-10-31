# Packages

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
