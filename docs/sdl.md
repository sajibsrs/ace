# sdl

[Documentation](README.md)

`sdl` is a minimal binding to the host's [SDL3](https://libsdl.org): a
window, a renderer with a draw color, and the keyboard and quit events of a
classic `SDL_PollEvent` loop. It covers only what its demo needs and grows
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

See [`demo/sdl3-window`](../demo/sdl3-window) for a consuming example.

## Window and renderer

```ace
if !sdl.init(sdl.INIT_VIDEO) { return 1 }
defer sdl.quit()

window: sdl.Window = sdl.Window::new("Ace + SDL3\0", 640, 480, sdl.WINDOW_RESIZABLE)
defer mut window.wipe()
if !window.ok() { return 1 }   // sdl.error() says why

renderer: sdl.Renderer = sdl.Renderer::new(window)
defer mut renderer.wipe()
if !renderer.ok() { return 1 }

renderer.color(sdl.Color { r = 32, g = 64, b = 128, a = 255 })
renderer.clear()
renderer.present()
```

`Window` and `Renderer` hold SDL's heap objects, so they follow the
`::new`/`wipe` pair; `ok()` reports whether creation succeeded, and `wipe` on
a failed handle is harmless. The window title is passed to SDL as a C string,
so it must end in `"\0"`. `Renderer.color(): (Color, bool)` reads the draw
color back; `color(c)` sets it. `sdl.error()` returns SDL's last error message
as a string view into SDL's own buffer, valid until the next SDL call.

## Events

`Event` mirrors SDL's `SDL_Event` union as an overlapping struct: every
member starts at offset zero, and `kind` is the event type shared by all of
them.

```ace
event: sdl.Event
for sdl.poll_event(mut &event) {
    if event.kind == sdl.EVENT_QUIT || event.kind == sdl.EVENT_WINDOW_CLOSE_REQUESTED {
        running = false
    } else if event.kind == sdl.EVENT_KEY_DOWN && event.keyboard.key == sdl.KEY_ESCAPE {
        running = false
    }
}
```

`event.keyboard` is a `KeyboardEvent` (`key`, `scancode`, `mod`, `down`,
`repeat`, …) and `event.quit` a `QuitEvent`; read the member that `kind`
names. Field names follow SDL's, except the shared `type` field, which is
`kind` because `type` is an Ace keyword.

## Testing

`pkg/sdl/tests/run.sh` runs under `make test-packages` and is skipped where
the hinted directory holds no `libSDL3`.
