# Standard library

- `Map.set`/`unset` are `inline`, and `set`, `unset`, and `rehash` read the
  slot slice into a local once, so stores to slots no longer force reloads of
  the map's fields. `map-set` 1.33x, `map-unset` 1.32x faster; selfhost
  `__text` +1.2% (#156).

- `map` hashes integer keys with one multiply by the golden-ratio constant and
  starts probing at the hash's top bits (as Linux `hash_64`); `set` probes once
  per insert. Structured keys (ids, strides, `b << 32 | v`) run 2.7–3.3x faster;
  random keys are unchanged.

- Added the `sort` module: `sort.unstable[T](items)` and `unstable[T](items, less)`
  (introsort: median-of-three Hoare quicksort, heapsort fallback past
  2·log2(n) depth, insertion below 16 elements; allocation-free and infallible)
  plus `sorted[T]` (`tests/emit/sort`).

- `Map.len` is now a method, `m.len()`: the entry count is a private field, so
  code outside `map` cannot write it and corrupt the table.

- Added `exec.spawn(argv, options)` with `SpawnOptions`: a working directory
  and per-stream `Redirect` (`inherit`/`fd`, zero is inherit), as `posix_spawn`
  file actions (#148). `sys.open` and `sys.pipe` now mark descriptors
  close-on-exec (`pipe2` on linux, `fcntl` on darwin) so concurrent children do
  not inherit each other's pipe ends.

- `sys.FileInfo` carries `device` and `inode` from `stat`/`lstat` on both
  targets (Darwin's signed `st_dev` widened as raw bits). Selfhost's
  output-overwrite check compares them, as the C driver compares `st_dev` and
  `st_ino`, so a hard link to an input is refused too.
- Added the `exec` module: `exec.spawn(argv)` and `SpawnErr` over
  `posix_spawnp`. The facade copies the arguments and environment into one
  NUL-terminated `mem` block and `os.spawn` only makes the call; it is its own
  module so `sys` keeps importing nothing. Selfhost will link through it (#134).
- Added `sys.lstat`, `sys.canonical`, and `sys.executable`: the kernel
  resolves paths (darwin `getattrlist`, linux `/proc/self/fd`), so selfhost
  drops its `realpath`, `_NSGetExecutablePath`, and `readlink` externs.
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
- `Array[T].view` is overloaded on the receiver (#111): `view(mut self): mut []T`
  returns the live elements writable in place, so `mut xs.view()` edits a field
  without copying the element out and back. The self-hosted loader uses it.
- `Array[T].resize(count): Error` sets the length, appending `blank(T)`
  elements (from `mem.zalloc` when the array has no storage) or dropping the
  tail.
- Added `Array[T].filled(count, value): (Array[T], Error)`: one `resize`
  allocation, element writes only for a non-blank `value`. selfhost's
  `regalloc.filled` and its push-in-a-loop fills use it or `resize`:
  `--release -c` on itself 0.69 → 0.65 s.
- Added `bits.mul_wide(a, b: u64): (u64, u64)`, the 128-bit product's high
  and low words, lowered through a new IR op `mulh` (`umulh`; x64 `mul`).
  `math`'s Payne-Hanek reduction uses it in place of a 32-bit-half
  `mul_high`: `bench/std` sin-large 23.3 to 18.4 ns on Apple arm64.
- `math`'s Payne-Hanek `reduce_large` is `inline` into `sin`, `cos`, and
  their `f32` forms: `bench/std` sin-large 18.7 to 15.2 ns (2.4x to 1.9x of
  libm) for about 1 KB of code.
- `array.Array.get` also takes an `int` index (`get(index: int)`); a negative
  index is a bounds violation. selfhost drops 264 `usize(...)` casts from
  `get` calls (2.2 KB); `set` stays `usize`-only, since a second overload
  would make an untyped literal value `int` (Syntax #6).
- Added the `atomic` module: `u32` and `u64` load, store, exchange, add, sub,
  and, or, xor, cas, and fence, the ordering in the name (unsuffixed is
  `seq_cst`). One `IR_ATOMIC` lowers on darwin-arm64 to ARMv8.1 `swp`/`ld<op>`/
  `cas`, `ldar`/`stlr`, and `dmb`; linux-x64 refuses it (#36, #37).
- `atomic` lowers on linux-x64 to `xchg`, `lock xadd`, `lock cmpxchg`, locked
  `and`/`or`/`xor` (a `cmpxchg` loop when the result is read), and `mfence`
  for a seq_cst fence only, relying on x64's total store order.
- `atomic` also takes `usize` words (its own overloads, lowered as the IR's
  64-bit word). `tests/emit/atomic/processes.ace` has four forked processes
  share a mapping behind a start gate; a plain increment loses updates there.
- Selfhost supports `atomic` on darwin-arm64: `IR_ATOMIC` and its printer, the
  intrinsic marking and signature check, lowering with the debug alignment and
  null checks, and the arm64 emission, byte-identical to the C compiler's
  (IR and objects, debug and release).
- Closed the atomics topic (#36, #37). Dropped: `u8`, `u16`, pointer, and
  128-bit atomics; a data-race check; thread-level tests until threads exist
  (#38); and `ldapr` for an acquire load (about 6% under contention on an M4,
  none uncontended). The untyped-literal overload question moved to its own
  issue; x64 atomics wait with the paused x64 work.
