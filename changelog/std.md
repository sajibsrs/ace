# Standard library

- `utf8.valid` checks each sequence's width with a private `span` instead of
  decoding it, and `decode` assembles its code point from that width:
  `bench/std` utf8-valid 3.3x to 1.4x of C.
- Renamed the internal `std/@` modules after their contents: `rendering` to
  `formatting` (the compiler finds the printf piece writers by that name),
  `float_parse` to `float_parsing`, `float_parse_fast[_tables]` to
  `eisel_lemire[_table]`, and `float_shortest[_tables]` to `ryu[_table]`.
- Added `str.cstring`, which copies text and a trailing NUL into caller
  storage and rejects an embedded NUL; `pkg/sdl` uses it. `sys` paths holding
  a NUL now fail with `EINVAL` instead of opening the shorter path; `sys`
  keeps its own copy, since it imports nothing.
