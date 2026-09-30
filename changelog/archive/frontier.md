# Archive — Current frontier

Entries recorded before the changelog split; oldest first.


- Added `ace.pkg` highlighting to `editors/vscode/`: a separate `ace-pkg`
  language matched by exact filename (not `.pkg`, which macOS installers
  use), with a TextMate grammar following `GRAMMAR.md` §7. Added language
  icons for `.ace` (including `mod.ace`) and `ace.pkg`, shown when the
  active file icon theme has none.
- Unified `pkg/sdl`'s value types: one `Point` and one `Color`, both `f32`
  (SDL_FPoint/SDL_FColor layouts), replacing `FPoint`, `FColor`, and the
  `u8` `Color`; `Renderer.color` now wraps `SDL_{Set,Get}RenderDrawColorFloat`.
  Field defaults (`Color.a = 1`, zero elsewhere) make `Vertex {}` blank and
  leave `tex_coord` unwritten; `Color::hex(rgb)` builds an opaque color
  from a 24-bit `0xRRGGBB` value (e.g. `0xF44336`). Renamed
  `demo/sdl3-rgb-cube` to `demo/sdl3-cube-rotation`.
- Typed `pkg/sdl`'s flags and event values: `InitFlags` (`bits u32`) and
  `WindowFlags` (`bits u64`) replace the `INIT_*`/`WINDOW_*` constants, and
  the `u32`-backed enums `EventType` and `Keycode` replace `EVENT_*`/`KEY_*`
  as the types of `Event.kind` and `KeyboardEvent.key`.
- A `mut` method call now initializes its receiver for definite
  initialization, as `mut &receiver` does, instead of requiring it to be
  initialized first (Mutability #6). `pkg/sdl` uses it: `mut Event.poll()`
  replaces `poll_event(mut *Event)`, so loops read `for mut event.poll()`.
- `pkg/sdl` `Renderer.geometry` with empty `indices` now passes `NULL` so SDL
  draws the vertices as consecutive triangles, instead of drawing nothing;
  `indices` without `vertices` returns false.
- `pkg/sdl` `Window::new` and `Renderer.debug_text` take plain Ace strings:
  each copies the text into a `TEXT_MAX + 1` (256) byte stack buffer and
  appends the NUL SDL reads, with no allocation; longer text fails.
- `pkg/sdl` `Keycode` names `backspace`, `tab`, `enter`, `escape`, `space`,
  `digit_0`–`digit_9`, `a`–`z`, and the four arrows; other keys stay
  reachable as `Keycode(c)` with `u32(key)` for range checks.
- `pkg/sdl` `Event.keyboard(): (KeyboardEvent, bool)` returns the keyboard
  member with false unless `kind` is `key_down` or the new `key_up`; the raw
  field stays for direct access.

- Extended auto-vectorization to `f64` lanes (Compiler #6): the IR's
  `v2f64` holds two lanes in 128 bits and takes `add`, `sub`, `mul`, `div`,
  `neg`, `splat`, `load`, `store`, and `phi`, which `arm64.c` emits as NEON
  `.2d` forms (`dup .2d`, or `fmov .2d`/`movi` for an 8-bit immediate). A
  loop's lane types share one width, so `f64` never mixes with `f32`/`i32`
  lanes, and a compare on `f64` keeps a loop scalar; the vector loop counts
  `n >> 1` vectors at the same 16-byte stride. `bench/core` `saxpy-f64` drops
  from 0.508 to 0.249 ns/element (2.04x; C `-O2` scalar 0.485, vectorized
  0.182). `tests/ir/vectorize.ace` adds accepted and rejected `f64` loops, and
  `tests/vectorize/lanes.ace` `f64` counts with NaN, signed-zero, infinite,
  and subnormal lanes.
- Extended auto-vectorization to strided struct fields (Compiler #6): a
  struct array whose same-typed fields each get the same computation, every
  field stored and each reading only its own field, vectorizes as the flat
  array of its fields, running field 0's computation once per 16-byte vector
  of a group of structs. The loop's elements may be a `padd` scaled by the
  struct size or a strength-reduced pointer phi, which the remainder loop
  resumes at `base + i * size`; accesses at different field offsets of one
  struct are independent. `bench/core/run-hotloop.sh` `particle-vec3` went
  from 1.11 to 0.281 ns/particle (3.96x; C `-O2` scalar 0.735, vectorized
  with fused multiply-adds 0.274). `tests/ir/vectorize.ace` adds accepted
  `Vec3` and `Vec2` loops and rejected uneven, partial, and crossed ones;
  `tests/vectorize/lanes.ace` adds `Vec3` `f32`, two-field `f64`, and
  four-field `i32` arrays.
- Fixed auto-vectorization reading `blocks[-1]`: every two-predecessor block
  is a candidate header, and the body walk reached a return block before the
  back-edge check, so ordinary `if` joins (e.g. in `mem.grow`) read out of
  bounds and `demo/http-simple-server` crashed. The walk now rejects a
  candidate at any block that leaves through a return.
- Moved the `TODO.md` survey to GitHub issues: one issue per entry with kind,
  status, area, and target labels, the priority bands as milestones, and the
  measured-gaps table as issue #72. `TODO.md` now explains the labels and how
  to query them.
- Split `SPEC.md` into chapter files under `spec/`, moving every line
  unchanged: `spec/README.md` maps each section to its file, the Decision Queue
  is `spec/queue.md`, and the Stdlib items keep one numbering across
  `spec/std/` and `spec/pkg/http.md`. References now name the chapter file.

The checked-build queue is complete. Additive networking, concurrency, fuller
math/SIMD, and a second
backend remain open; their status belongs in the `spec/queue.md` Decision Queue and
GitHub issues, not in this history.
