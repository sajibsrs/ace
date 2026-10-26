## Decision Queue

Only active or trigger-gated decisions live here. Implemented history is recorded
once in `changelog/`; uncommitted candidates are tracked as GitHub issues.

### Active

- Writable by default, no undefined behavior (Philosophy #3, #9–#11).
  Decided (the `mut` removal is implemented; #158 tracks its open problems):
  - `string` stays the same type as `[]u8`/`[]char`; literals stay in
    read-only memory and writing one faults. Precedent: C string literals in
    `__TEXT`/`.rodata`.
  - Globals, Ace and C (`extern`), are writable by default; `const` opts into
    read-only placement (not yet enforced on `extern` globals).
  - Defined failures: integer overflow follows one fixed rule; oversized
    shifts are defined; bounds, null dereference, and division by zero trap;
    reading uninitialized storage stays a compile error. Precedent: Go (bounds,
    nil, divide traps; wrapping overflow; defined shifts).
  - Use after free through another alias: debug builds detect it (quarantine,
    guard pages) and trap, like `noalias` debug checks; release has machine
    semantics. Precedent: Zig's debug allocator, Vale.
  - Data races (Memory, concurrency) and a C `longjmp` across an Ace frame
    (C Interop #7) are restated under machine semantics; "undefined
    behavior" and "unspecified" leave the spec.
  Open:
  - [open] Spelling of the by-value receiver (e.g. `val self`).
  - [open] The overflow rule: wrap in every build, or trap in every build.
  - [open] The oversized-shift rule: mask the count (arm64 hardware), or
    yield zero / sign fill.
  - [open] Bounds, null, and divide traps: in release too (Go), or debug only
    with machine semantics in release (today's Safety #3, #6).
  - [open] `noalias` restated: "no other parameter, global, or pointer reaches
    the same bytes during the call", and which arguments the debug check now
    pairs, since every slice may write.
  - [open] Whether `const` globals remain distinct types (a read-only `*T`
    from `&CONST`) or only a placement, so a write through a pointer to one
    faults at run time.

### Trigger-gated

- The Ace fixture runner (#147) replacing `tests/run.sh`, with child-output
  capture (#151) and the process and filesystem gaps in #155. It is
  non-blocking tooling, not part of the language; resume when the shell
  runner becomes a bottleneck. Open: #151's result ownership, size limit, and
  separate or merged stderr; #155's module placement and the environment
  option's spelling.
- The canonical source formatter (`ace fmt`: in-place, check-only, stdout
  modes; compiler/toolchain functionality, not `std/fmt`) is deferred until
  timely. Decided: `.acefmt` stays normative, searched upward with built-in
  defaults; one shared lexer/parser pass produces either the compact compiler
  AST or, in format mode only, a separate lossless syntax tree (tokens,
  trivia, preserved comments/literal spellings, every `@when` retained) —
  never both. Out of scope: semantic analysis, resolution, reordering, LSP,
  `std/fmt`. Formatting must be deterministic, idempotent, and enforce the
  120-column limit without partially rewriting invalid input; a
  comma-separated list stays on one line if it fits, otherwise one item per
  continuation line with a trailing comma. Resume with staged implementation
  carving when formatter work becomes an active priority.
- Package-system manifest, display, and command syntax remain open under
  Package System #3, #13, and #15; remote packages through a central registry
  or resolver remain open under #9. Resume only when package implementation is
  selected.
- x64 (linux, windows) work waits until darwin is complete, then proceeds as
  its own effort. Atomics there are lowered (`src/x64.c`, checked by
  `tests/x64_atomic` disassembly) but have never run: they execute only on a
  linux-x64 host, as does the debug null trap. A fetch-and, -or, or -xor whose
  result is read loops on `cmpxchg`, and every read-modify-write is
  sequentially consistent whatever its name says. Selfhost has no x64 backend.
- A second target remains trigger-gated until its backend and OS implementations
  are genuinely supported; directory-module facades already select target files
  consistently through `@when`.
- Freestanding targets (kernels, drivers, bootloaders, no-libSystem programs)
  are in scope but need a separately carved runtime contract (entry, failure
  without an OS, memory source, linking without C runtime objects) and target
  contract (naming, `@when` selection, available `std` modules). Hosted
  user-space access such as Darwin BPF does not establish either. Resume when
  selected, independent of other topics.
- IPv6, UDP, address queries, half-close,
  extra TCP options, and other explicitly additive networking work are carved
  only after the nonblocking proof identifies the required order.
