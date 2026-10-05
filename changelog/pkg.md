# Packages

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
