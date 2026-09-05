# Philosophy

The why and the principles. Concrete language rules, syntax, and the decision/carving queue live in SPEC.md.

## Why Ace (Motivation)
C is the gold standard — the goal is not to beat C at its own game, but to keep its machine-true performance model while fixing what surrounds it:

1. C is legacy; a few careful additions would shrink maintenance cost and error rates dramatically.
2. No boundary checks, no validation checks; errors (especially memory leaks) are hard to catch.
3. C's organization and abstraction tools are hacky and error-prone at scale — no OOP, no developer convenience. Times have changed; people want tools that are easy to work with.

## Priority Stack
When goals conflict, resolve in this order:
1. Performance (runtime)
2. Simplicity — keep the language, compiler, runtime model, and generated code minimal
3. Modularity — large programs must decompose through explicit, independent boundaries
4. Compile time — stay close to C/C++ compilers; avoid analysis-heavy compilation
5. Safety — provide a small, practical improvement over C, never a Rust-style ownership model

## General Goals
1. Trust the programmer, but provide what they need.
2. As simple/minimal as it goes.
3. Don't force features that can remain opt-in.
4. Performance is the ultimate goal.
5. Simple, clean, distinguishable and pragmatic syntax.
6. True to machine (less magic).
7. Reduce code nesting. (Applies to the language requirement only) [*]
8. Additional and quality-of-life features should be zero-cost whenever possible.
9. Design that reduces code duplication. [*]
10. Safety is deliberately limited: prefer cheap local checks and checked-build tooling; reject borrow checking, lifetime systems, and whole-program proofs.

## Other Language Support
1. Assembly support.
2. Way to work with C/C++ code. Though, it shouldn't depend on them.

## Principles
Distilled from the closed philosophy decisions; operative rules live in SPEC.md.
1. Memory is manual and explicit. Ace permits only small, local compile-time safety rules with zero runtime cost; broader diagnostics belong in checked builds, never in ownership or lifetime machinery.
2. No hidden control flow, no hidden costs: no exceptions, no propagation sugar, no COW, no inference, no implicit instantiation.
3. Mutation is visible — in signatures and at call sites; an unmarked call changes nothing you passed.
4. Machine truth: layout, sizes, and stack vs heap are statically known and visible.
5. Composition over inheritance; dispatch is an explicit value, never hidden machinery.
6. Errors are values, checked by hand; panic is abort, never a channel.
7. Start strict, relax later — never tighten.
8. Modules are explicit compilation boundaries; modularity must not require hidden runtime machinery or whole-program analysis.

## Considerations
1. Start strict, relax later: adding convenience later is backward-compatible; removing it is a break. Explicitness-first is the safe default (applies to types, syntax, stdlib).

## Cautions
1. Direct emission caps the optimization ceiling — if runtime benchmarks lag C by a widening margin, revisit the backend; keep codegen seams clean enough that a swap stays possible.
2. Explicit everything (types, instantiations, checks) is verbose — watch real-world ergonomics; relax per Considerations #1, never tighten.
3. Safety remains intentionally narrower than Rust: definite initialization and type checking catch cheap local mistakes, while aliasing, lifetimes, allocator discipline, and error checks remain the programmer's responsibility with checked-build assistance.

## Inspiration
- Assembly
- B
- C
- Go
- Odin
- C++
- Zig
- Rust
- PHP
- JavaScript
- Java
- Kotlin
