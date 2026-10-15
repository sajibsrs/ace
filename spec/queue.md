## Decision Queue

Only active or trigger-gated decisions live here. Implemented history is recorded
once in `changelog/`; uncommitted candidates are tracked as GitHub issues.

### Active

- Atomics and the memory model (#36, #37): carved in Memory, "Carved (memory
  model)" and Stdlib #25. Staged: arm64 lowering and tests (done), x64
  (done), then selfhost. Open limitations, each resolved or dropped before
  the topic closes:
  - Staging: selfhost has no `atomic` support.
  - Coverage: no test exercises real cross-context behavior (no threads yet),
    only single-context semantics and the emitted instructions. The x64 code
    has never run: it is checked by disassembly (`tests/x64_atomic`, which
    skips without an x86-64 `objdump`) and executes only when `make test` runs
    on a linux-x64 host; its debug null trap is likewise unrun.
  - Surface: no `u8`, `u16`, pointer, or 128-bit atomics; `usize` needs an
    explicit pointer conversion.
  - Language: an untyped integer literal does not choose between the `u32`
    and `u64` overloads, so every value is cast (`u32(1)`); a tuple result is
    read only by destructuring.
  - Checking: data races are unchecked in every build.
  - x64: a fetch-and, -or, or -xor whose result is read loops on `cmpxchg`;
    every read-modify-write is sequentially consistent whatever its name says.
  - arm64: assumes ARMv8.1 atomics (no fallback); an acquire load uses `ldar`,
    not the cheaper `ldapr`; the debug alignment check expands at every site.

### Trigger-gated

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
- Threads follow the networking proof; their semantics are not yet carved.
