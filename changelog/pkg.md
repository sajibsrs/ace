# Packages

- Added `demo/sdl3-cube-raytrace`: the cube-rotation scene traced on one
  thread, one ray per 2x2 cell, inverse-rotated into the cube frame for a slab
  test, Lambert-shaded, over a floor with shadow rays; cells draw as solid
  quads through `Renderer.geometry`.
