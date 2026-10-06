---
paths:
  - "spec/**/*"
  - "GRAMMAR.md"
  - "PHILOSOPHY.md"
  - "src/**/*"
  - "std/**/*"
  - "tests/**/*"
  - "demo/**/*"
  - "bench/**/*"
  - "*.ace"
---

# Fixed design boundaries

Follow `spec/` for exact rules; do not propose or assume away these decisions:

- Manual memory uses one process-wide strategy exposed by `mem`; no allocator
  values, GC, borrow checking, ownership, or hidden allocation.
- Errors are values with explicit branches; no exceptions or propagation
  operator. Panic aborts.
- Types and generic instantiations are explicit; no declaration elision,
  comptime, or trait solving.
- Assignment copies. Parameters are read-only values, passed as a copy or the
  caller's address at the compiler's choice, unless `mut`; mutation appears in
  the signature and call site. `noalias` states non-overlap.
- Every type has an all-bits-zero representation. Zero pointers and handles are
  `null`; there is no optional type operator. Globals are zeroed; locals require
  definite initialization.
- Composition and explicit contract values replace inheritance.
- Declarations use `name: type`; functions are keywordless with trailing
  returns; function values use signature types.
- The compiler uses recursive descent, the passes of `spec/compiler.md`
  Compiler #2 (a per-file top-level declaration pass, then in-order checking),
  and direct machine-code emission from a per-function IR;
  no whole-program or cross-function analysis beyond `inline` and compiling
  only reachable functions.
- Safety is cheap local checking plus removable debug-build diagnostics.
- There is no `goto`; use labeled blocks and loops. Iteration, switch, and
  `fallthrough` follow `spec/` exactly.
