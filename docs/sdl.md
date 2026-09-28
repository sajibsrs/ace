# sdl

[Documentation](README.md)

`sdl` is a minimal binding to the host's [SDL3](https://libsdl.org): a
window, a renderer with a draw color, colored triangle geometry, debug text,
and vsync, and the keyboard and quit events of a classic `SDL_PollEvent`
loop. It covers only what its demo needs and grows
with its users.

`sdl` is a standalone package at [`pkg/sdl`](../pkg/sdl), not a `std`
module. A consumer lists it under `[dependencies]` in its own `ace.pkg` as
`sdl: 0.1.0`, and the source writes a plain `import sdl`. The package
manifest links `SDL3` with Homebrew's Apple Silicon directory as a hint:

```
[[link]]
library: SDL3
path: /opt/homebrew/lib
```

Where `libSDL3` is there, the build links that exact file; elsewhere it
falls back to the linker's own search. An app whose SDL3 lives somewhere
else gives the same record with its own `path` in its `ace.pkg`; the app's
path overrides the hint, and a missing file there is a manifest error.

See [`demo/sdl3-window`](../demo/sdl3-window) and
[`demo/sdl3-cube-rotation`](../demo/sdl3-cube-rotation) for consuming examples.

## Window and renderer

```ace
if !sdl.init(sdl.InitFlags { video = 1 }) { return 1 }
defer sdl.quit()

window: sdl.Window = sdl.Window::new("Ace + SDL3\0", 640, 480, sdl.WindowFlags { resizable = 1 })
defer mut window.wipe()
if !window.ok() { return 1 }   // sdl.error() says why

renderer: sdl.Renderer = sdl.Renderer::new(window)
defer mut renderer.wipe()
if !renderer.ok() { return 1 }

renderer.color(sdl.Color::hex(0x204080))
renderer.clear()
renderer.present()
```

`InitFlags` and `WindowFlags` are bits types over SDL's flag words: set the
fields you want and the rest stay zero. Only the flags this binding uses are
named so far.

`Window` and `Renderer` hold SDL's heap objects, so they follow the
`::new`/`wipe` pair; `ok()` reports whether creation succeeded, and `wipe` on
a failed handle is harmless. The window title is passed to SDL as a C string,
so it must end in `"\0"`. A `Color` has `f32` channels from 0 to 1, with `a`
defaulting to 1 (opaque); `Color::hex(rgb)` builds an opaque one from a
24-bit `0xRRGGBB` value such as `0xF44336`. `Renderer.color(): (Color, bool)` reads the draw
color back; `color(c)` sets it. `sdl.error()` returns SDL's last error message
as a string view into SDL's own buffer, valid until the next SDL call.

## Geometry

`Renderer.geometry(vertices, indices)` draws untextured triangles: every
three entries of `indices` name one triangle's corners in `vertices`, and SDL
interpolates the corners' colors across it. A `Vertex` holds a `position`
(`Point`, in render pixels), a `color`, and a `tex_coord`, which is unused
without a texture; every field defaults to zero (opaque black for `color`),
so `sdl.Vertex {}` is a blank vertex. An empty `vertices` or
`indices` draws nothing.

```ace
red: sdl.Color = sdl.Color { r = 1.0 }
triangle: [3]sdl.Vertex = [
    sdl.Vertex { position = sdl.Point { x = 320.0, y = 80.0 }, color = red },
    sdl.Vertex { position = sdl.Point { x = 80.0, y = 400.0 }, color = red },
    sdl.Vertex { position = sdl.Point { x = 560.0, y = 400.0 }, color = red },
]
corners: [3]i32 = [0, 1, 2]
renderer.geometry(triangle[..], corners[..])
```

`Renderer.size(): (i32, i32, bool)` reports the current output size in render
pixels, which follows window resizes. `sdl.ticks()` returns the milliseconds
since `sdl.init`, for animation, and `sdl.ticks_ns()` the nanoseconds, for
timing work.

## Presentation and text

`Renderer.vsync(1)` makes `present` wait for the display's next refresh, so
frames arrive evenly and the loop needs no delay of its own; `2` waits for
every other refresh, `sdl.VSYNC_ADAPTIVE` tears only when a frame is late, and
`sdl.VSYNC_DISABLED` never waits. It returns `false` where the renderer cannot
honor the interval.

`Renderer.debug_text(x, y, text)` draws one line in SDL's built-in 8x8 font
(`sdl.DEBUG_TEXT_SIZE`) with the current draw color; like a window title, the
text must end in `"\0"`. `Renderer.scale(x, y)` multiplies every later
drawing coordinate, which enlarges that font on high-density displays:

```ace
renderer.scale(2.0, 2.0)
renderer.color(sdl.Color { r = 224, g = 224, b = 224, a = 255 })
renderer.debug_text(6.0, 6.0, "vsync on\0")
renderer.scale(1.0, 1.0)
```

## Events

`Event` mirrors SDL's `SDL_Event` union as an overlapping struct: every
member starts at offset zero, and `kind` is the event type shared by all of
them.

```ace
event: sdl.Event
for sdl.poll_event(mut &event) {
    if event.kind == sdl.EventType.quit || event.kind == sdl.EventType.window_close_requested {
        running = false
    } else if event.kind == sdl.EventType.key_down && event.keyboard.key == sdl.Keycode.escape {
        running = false
    }
}
```

`event.keyboard` is a `KeyboardEvent` (`key`, `scancode`, `mod`, `down`,
`repeat`, …) and `event.quit` a `QuitEvent`; read the member that `kind`
names. Field names follow SDL's, except the shared `type` field, which is
`kind` because `type` is an Ace keyword. `kind` is an `EventType` and `key` a
`Keycode`, `u32`-backed enums naming the values this binding handles; SDL may
deliver other values, which simply match no named member.

## Testing

`pkg/sdl/tests/run.sh` runs under `make test-packages` and is skipped where
the hinted directory holds no `libSDL3`.
