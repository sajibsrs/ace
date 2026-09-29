# Compiler internals

The compiler runs a fixed, in-order sequence of passes (`SPEC.md` Compiler #2),
each working on one file, one merged unit, or one function at a time, with no
whole-program or cross-function analysis. This page maps that sequence onto
`src/`.

```
source → lex → parse → merge → check → lower → optimize → select/allocate/encode → object → link
          (1)    (2)     (3)     (4)     (5)      (6)              (7-10)
       text   tokens   AST    merged   checked  ─── SSA IR ───►  arm64 code       Mach-O .o
                                AST      AST
```

## Front end

- **`lexer.c`** (pass 1) — one scan of a source file into tokens.
- **`parser.c`** (pass 2) — recursive-descent tokens → AST; drops inactive
  `@when` declarations here.
- **`module.c`** (pass 3) — resolves imports into one merged compilation unit:
  loads every imported file the same way, renames through one scoped
  reference walk, and merges in dependency order. A module is one file or a
  directory rooted at `mod.ace` (`docs/module.md`).
- **`sema.c`** (pass 4) — one in-order walk of the merged unit: types, names,
  expression typing, generics, forward-call fixups (declaration before use),
  and the definite-initialization walk. The largest file in the compiler.

## IR backend

Every function compiles through a shared per-function SSA IR before any
target-specific code generation (`SPEC.md` Compiler #4): emitting straight
from the AST would need every optimization written once per source shape and
repeated in each additional backend.

- **`ir.h`** / **`ir.c`** — the IR's instruction set, construction,
  verification, and printer (`--ir`). Shared by every target. Its 128-bit
  vector types (`v4f32`, `v4i32`, `v2f64`) are internal to the optimizer: no source
  type lowers to one, and none crosses a call. `tests/vector_ir.c` builds
  vector IR by hand and checks the target's lanes against C.
- **`lower.c`** (pass 5) — walks each checked function once into SSA
  (Braun et al.): expands `inline` calls, turns `defer` into control flow,
  makes debug checks trapping instructions or explicit branches, splits every
  struct, tuple, string, and slice into scalar leaves (the IR has no
  aggregate values), and turns self tail recursion into a loop.
- **`optimizer.c`** (pass 6, release builds only) — passes over one
  function's IR, each run once, in order: sparse conditional constant
  propagation, global value numbering, loop strength reduction, dead-code and
  control-flow cleanup, then auto-vectorization over the cleaned loops (and
  cleanup again when it rewrote one). The vectorizer puts a four-lane loop
  (two-lane for `f64`, which takes arithmetic only and never shares a loop
  with 32-bit lanes), counting whole vectors, ahead of an innermost counted
  loop of lane-wise `f32`/`i32`/`f64` arithmetic whose accesses it proves
  independent; the original
  loop runs the remainder, or is dropped when the count is a constant number
  of whole vectors. A branch whose arms neither load nor store becomes a
  `select` on the compare's lane mask, and `i32(i)` under a constant count
  becomes a lane vector counting up from `<0, 1, 2, 3>`. A struct array
  whose same-typed fields all get the same computation (every field stored,
  each reading only its own field) vectorizes as the flat array of its
  fields: each vector iteration covers four structs (two for `f64`) as one
  16-byte vector per field, running field 0's computation once per vector.
  Its elements may be a `padd` scaled by the struct size or a loop strength
  reduction pointer phi, which the remainder loop resumes at the vector
  loop's end.
- **`arm64.c`** (passes 7-10, the AArch64 target) — instruction selection
  (including fused forms such as integer `madd`), leaving SSA (phis become
  copies, with coalescing), linear-scan register allocation, block layout,
  and encoding.
- **`x64.c`** (passes 7-10, the `linux-x64` target) — a sibling of `arm64.c`
  reusing passes 1-6 unchanged. Every value lives in its own frame slot and
  each instruction works through `rax`, `rcx`, and `rdx`; there is no
  register allocation yet. Calls follow System V: an argument, parameter, or
  result of up to 16 bytes travels in one register per eightbyte, and a
  larger aggregate on the stack as a copy, with a result's address in `rdi`.
  Floats pass in `xmm0`-`xmm7` and SSE eightbytes, and compute through
  `xmm0`-`xmm2` in scalar SSE, with the target's float conversions matching
  arm64's saturation bit for bit. Integers, floats, pointers, aggregates of
  them, the byte routines, and the debug allocator registry are covered; a
  function using vectors or `asm` is a compile error naming what it lacks.
  The target requires an x86-64-v3 CPU (FMA3 for `math.fma`).

## Object emission

- **`emit.c`** — stage four's object driver: runs passes 5-10 for every
  function body, then emits what no function body holds — global and
  constant data, static maps, vtables and their embed thunks, the runtime
  failure and allocator routines, and the shared byte routines. Also owns
  symbol mangling and the constant folding global initializers need. Until
  the IR migration, this file also held a direct AST-walking code generator;
  that path is gone, and everything here now sits *around* the IR backend
  rather than beside it.
- **`macho.c`** — the Mach-O arm64 object shared by every backend: sections,
  symbols, relocations, fixups, the DWARF line table, and the file image.
  Target-agnostic.
- **`backend.h`** — the `Obj` state shared by every code generator and the
  object writer.

## Driver and support

- **`main.c`** — wires the stages together (lex → parse → sema → emit →
  link) and handles the CLI flags described in `docs/compiler.md`.
- **`package.c`** — `ace.pkg` manifest parsing and dependency-graph
  resolution (`SPEC.md` Package System). No codegen or IR involvement.
- **`ace.h`** — shared definitions used across the compiler.
- **`sema.h`** — sema's public surface, consumed by `lower.c` and `emit.c`.

## See also

- `SPEC.md` Compiler #2 and #4 — the normative pass list and codegen
  rationale.
- `docs/compiler.md` — user-facing CLI usage (`--ir`, `--ast`, `-c`, `-o`,
  build modes).
- `CHANGELOG.md` — the IR migration's history, in the entries recording each
  step that moved a function shape off the old AST emitter and onto passes
  5-10.
