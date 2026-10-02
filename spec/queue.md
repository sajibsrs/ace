## Decision Queue

Only active or trigger-gated decisions live here. Implemented history is recorded
once in `changelog/`; uncommitted candidates are tracked as GitHub issues.

### Active

- Auto-vectorization (Compiler #6) — semantics decided; the IR represents
  vectors as internal value types (`v4f32`, `v4i32`, `v2f64`) the target emits
  lane-wise. Accepted so far: an innermost counted loop of lane-wise
  `f32`/`i32` arithmetic, or of `f64` arithmetic alone (two lanes, no
  compare), over elements `base[i]`, with no call after inline
  expansion, whose stores provably touch no other access's element; a branch
  whose arms neither load nor store becomes a lane select, and `i32(i)` under
  a constant count is a lane value. A struct array whose same-typed fields
  each get the same computation, every field stored, runs as the flat array
  of its fields. The original loop runs the remainder. Struct fields computed
  differently (interleaved `ld3`/`st3`) are parked: on AArch64 they measured
  1.28x over scalar C, below the bar, and no bench kernel needs them; so is an
  `f64` reduction over interleaved `f32` records (`length-vec3`), lane-gathered
  with in-order lane sums, at 1.10x. Acceptance:
  `bench/core/run-hotloop.sh` `cube-transform`, `saxpy-f32`, `saxpy-f64`, and
  `particle-vec3` against C `-O2` with vectorization on.

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
- Threads and atomics follow the networking proof; their semantics are not yet
  carved.
