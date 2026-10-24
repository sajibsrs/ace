## Decision Queue

Only active or trigger-gated decisions live here. Implemented history is recorded
once in `changelog/`; uncommitted candidates are tracked as GitHub issues.

### Active

- Concurrency, in order: threads (#38), then the mutex and condition-variable
  module (#40), thread-local storage (#39), and signals (#41). Darwin
  implementations bind libSystem's pthread and platform libraries, never its
  libc. Open for #40: `pthread_mutex`/`pthread_cond` or `os_unfair_lock`, and
  the module name.

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
