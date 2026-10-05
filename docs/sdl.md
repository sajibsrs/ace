# sdl

[Documentation](README.md)

`sdl` is a minimal binding to the host's [SDL3](https://libsdl.org): a
window, a renderer with a draw color, colored triangle geometry, debug text,
and vsync, fragment shaders on SDL's GPU render driver, and the keyboard and
quit events of a classic `SDL_PollEvent` loop. It covers only what its demos
need and grows with its users.

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

See [`demo/sdl3-window`](../demo/sdl3-window),
[`demo/sdl3-cube-rotation`](../demo/sdl3-cube-rotation),
[`demo/sdl3-cube-raytrace`](../demo/sdl3-cube-raytrace), and
[`demo/sdl3-cube-raytrace-gpu`](../demo/sdl3-cube-raytrace-gpu) for consuming
examples.

## Window and renderer

```ace
if !sdl.init(sdl.InitFlags { video = 1 }) { return 1 }
defer sdl.quit()

window: sdl.Window = sdl.Window.new("Ace + SDL3", 640, 480, sdl.WindowFlags { resizable = 1 })
defer mut window.wipe()
if !window.ok() { return 1 }   // sdl.error() says why

renderer: sdl.Renderer = sdl.Renderer.new(window)
defer mut renderer.wipe()
if !renderer.ok() { return 1 }

renderer.color(sdl.Color.hex(0x204080))
renderer.clear()
renderer.present()
```

`InitFlags` and `WindowFlags` are bits types over SDL's flag words: set the
fields you want and the rest stay zero. Only the flags this binding uses are
named so far.

`Window` and `Renderer` hold SDL's heap objects, so they follow the
`.new`/`wipe` pair; `ok()` reports whether creation succeeded, and `wipe` on
a failed handle is harmless. A title longer than `sdl.TEXT_MAX` (255) bytes
fails creation. A `Color` has `f32` channels from 0 to 1, with `a`
defaulting to 1 (opaque); `Color.hex(rgb)` builds an opaque one from a
24-bit `0xRRGGBB` value such as `0xF44336`. `Renderer.color(): (Color, bool)` reads the draw
color back; `color(c)` sets it. `sdl.error()` returns SDL's last error message
as a string view into SDL's own buffer, valid until the next SDL call.

## Geometry

`Renderer.geometry(vertices, indices)` draws untextured triangles: every
three entries of `indices` name one triangle's corners in `vertices`, and SDL
interpolates the corners' colors across it. A `Vertex` holds a `position`
(`Point`, in render pixels), a `color`, and a `tex_coord`, which is unused
without a texture; every field defaults to zero (opaque black for `color`),
so `sdl.Vertex {}` is a blank vertex. With an empty `indices`, every three
consecutive vertices form one triangle. Empty `vertices` with empty `indices`
draws nothing and succeeds; non-empty `indices` without `vertices` returns
false.

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
(`sdl.DEBUG_TEXT_SIZE`) with the current draw color; text longer than `sdl.TEXT_MAX`
bytes draws nothing and returns false. `Renderer.scale(x, y)` multiplies every later
drawing coordinate, which enlarges that font on high-density displays:

```ace
renderer.scale(2.0, 2.0)
renderer.color(sdl.Color { r = 224, g = 224, b = 224, a = 255 })
renderer.debug_text(6.0, 6.0, "vsync on")
renderer.scale(1.0, 1.0)
```

## Shaders

`Renderer.new(window, driver)` creates the renderer on a named SDL render
driver. `sdl.GPU_DRIVER` draws through SDL's GPU API, and its renderers can
fill geometry with a fragment shader of your own:

```ace
renderer: sdl.Renderer = sdl.Renderer.new(window, sdl.GPU_DRIVER)
defer mut renderer.wipe()
if !renderer.ok() { return 1 }

program: sdl.Shader = sdl.Shader.new(renderer, sdl.ShaderFormats { spirv = 1 }, code[..], 1)
defer mut program.wipe()
if !program.ok() { return 1 }

state: sdl.RenderState = sdl.RenderState.new(renderer, program)
defer mut state.wipe()
if !state.ok() { return 1 }

state.uniforms(0, scene[..])
renderer.state(state)
renderer.geometry(quad[..], corners[..])
renderer.state(blank(sdl.RenderState))
```

`Renderer.shader_formats()` returns the `ShaderFormats` the renderer accepts,
which depend on the GPU API beneath it: `spirv` on Vulkan, `msl` on Metal. A
program that runs on both carries its shader in both and passes the one the
renderer names. `Shader.new(renderer, format, code, uniform_buffers)` takes
the code as bytes in the one format set in `format`, and the number of uniform
buffers the shader reads. The shader's entry point is `main`, or `main0` in
MSL. Uniform buffer `n` is `layout(set = 3, binding = n)` in SPIR-V and
`[[buffer(n)]]` in MSL, and the only input untextured geometry passes on is
its vertex color, at location 0.

`RenderState.new(renderer, shader)` wraps the shader for drawing, and the
shader must outlive it; with `defer` in the order above, it does.
`RenderState.uniforms(slot, values)` sets one uniform buffer from `f32`
values, which later draws read until it is set again. `Renderer.state(state)`
makes later geometry use the state, and `blank(sdl.RenderState)` restores the
renderer's own shaders, as debug text needs.

On a renderer of any other driver, `shader_formats()` is empty and
`Shader.new` fails. These calls need SDL 3.4.

## Events

`Event` mirrors SDL's `SDL_Event` union as an overlapping struct: every
member starts at offset zero, and `kind` is the event type shared by all of
them.

```ace
event: sdl.Event
for mut event.poll() {
    if event.kind == sdl.EventType.quit || event.kind == sdl.EventType.window_close_requested {
        running = false
    } else if event.kind == sdl.EventType.key_down && event.keyboard.key == sdl.Keycode.escape {
        running = false
    }
}
```

`event.keyboard` is a `KeyboardEvent` (`key`, `scancode`, `mod`, `down`,
`repeat`, …) and `event.quit` a `QuitEvent`; read the member that `kind`
names. `event.key(): (KeyboardEvent, bool)` does that check for you,
returning false unless `kind` is `key_down` or `key_up`:

```ace
key: sdl.KeyboardEvent, is_key: bool = event.key()
if is_key && key.key == sdl.Keycode.space { ... }
```

Field names follow SDL's, except the shared `type` field, which is
`kind` because `type` is an Ace keyword. `kind` is an `EventType` and `key` a
`Keycode`, `u32`-backed enums naming the values this binding handles; SDL may
deliver other values, which simply match no named member. `Keycode` names
`backspace`, `tab`, `enter`, `escape`, `space`, `digit_0`…`digit_9`, `a`…`z`,
and the arrows `left`, `right`, `up`, `down`. A key is its unshifted
character, so any other one is `sdl.Keycode('/')`, and `u32(key)` gives the
integer for a range check:

```ace
if u32(key) >= 'a' && u32(key) <= 'z' { ... }
```

## Testing

`pkg/sdl/tests/run.sh` runs under `make test-packages` and is skipped where
the hinted directory holds no `libSDL3`.
