# Standard library

- Added typed `mem.copy[T]` and `mem.move[T]`, lowered like the byte forms
  with the length scaled by `sizeof(T)`; debug checks compare element counts.
  A 1000-element `u32` copy loop runs 7.5x faster. The self-hosted parser and
  `pkg/http`'s header copy drop their element loops.
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
- `fmt.printf(fd, format, ...)` writes to a `sys.Fd` with `printf`'s checks
  and result, as C's `dprintf`: the shared formatting sink passes a target to
  its drain. Neither `printf` form nor `str.format`'s formatted form can be
  taken as a function value through an explicit function type any more.
- `fmt.printf` (both forms) and `str.format` take `%e` and `%g` with C's
  exact output for every `f32`/`f64`: `float_decimal`'s exact rounder gains a
  significant-digit mode, and `piece_scientific[T]`/`piece_general[T]` render
  its digits, `%g` reusing the fixed renderer before trimming zeroes.
- `net.socket` drops its non-`tcp` branch: a `SocketType` value is always a
  member, so the branch could not run.
- `ascii.lower` reads a 256-entry `LOWER` table instead of range-checking, so
  case-insensitive `ascii.equal` compiles to loads with no data-dependent
  branch: `ascii-fold-64` drops from 154 to 75 ns (C `strncasecmp`: 79 ns).
- `utf8.valid` skips ASCII runs before calling `span`, and `span` reads a
  256-entry `LEADS` table for the width and second-byte range row (as Go's
  `first`/`acceptRanges`), testing each range with one wrapped compare:
  `utf8-valid` drops from 125 to 84 ns (C: 65 ns).
- Added `fmt.Buffer` and `mut buffer.printf(...)`: `printf`'s checked lowering
  writing through a sink kept in caller storage across calls, drained on fill
  or `flush`. `printf` decimals divide by a constant and one-byte literals
  store directly. The self-hosted compiler's `Out` gives way to it.
- Added a storage-only `fmt.Buffer.init(storage)`, `buffer.text()`, and
  `buffer.dropped()`: a sink with a null drain keeps what fits and counts the
  rest, checked only on the slow path. Error context is carried this way, by
  convention; the self-hosted parser's error message uses one.
- `tcp.receive(connection, buffer)` and `tcp.watch(q, connection)` reach a
  connection whose socket is now private (`watch` registers its current
  interest with `poll`). sdl's raw C handles, `Array.len`, `Map.len`, and
  the plain-data `net`, `sys`, and `poll` records mark their fields `pub`.
