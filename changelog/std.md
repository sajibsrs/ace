# Standard library

- Marked `mem.grow` `@cold`, keeping `Array.push` growth out of its callers'
  loops.
- `math` Payne-Hanek reduction reads 2/pi from bit e-1, so the quotient
  lands at a fixed position of a three-word product held in registers,
  replacing the ten-limb sum in memory; results are bit-identical.
  `bench/std` sin-large 63 to 22 ns on Apple arm64 (8.1x to 2.9x of libm).
- `utf8.valid` checks each sequence's width with a private `span` instead of
  decoding it, and `decode` assembles its code point from that width:
  `bench/std` utf8-valid 3.3x to 1.4x of C.
- `math` Payne-Hanek reduction reads the quotient, the fraction's top bit,
  and its two 53-bit pieces as 64-bit windows of the limb sum, not bit by
  bit; results are bit-identical. `bench/std` sin-large 6.5x to 1.4x of C.
- Renamed the internal `std/@` modules after their contents: `rendering` to
  `formatting` (the compiler finds the printf piece writers by that name),
  `float_parse` to `float_parsing`, `float_parse_fast[_tables]` to
  `eisel_lemire[_table]`, and `float_shortest[_tables]` to `ryu[_table]`.
- Added `str.cstring`, which copies text and a trailing NUL into caller
  storage and rejects an embedded NUL; `pkg/sdl` uses it. `sys` paths holding
  a NUL now fail with `EINVAL` instead of opening the shorter path; `sys`
  keeps its own copy, since it imports nothing.
