# Philosophy

The why and the principles. Concrete language rules, syntax, and the decision/carving queue live in spec.md.

## Why Ace (Motivation)
C is the gold standard — the goal is not to beat C at its own game, but to keep its machine-true performance model while fixing what surrounds it:

1. C is legacy; a few careful additions would shrink maintenance cost and error rates dramatically.
2. No boundary checks, no validation checks; errors (especially memory leaks) are hard to catch.
3. C's organization and abstraction tools are hacky and error-prone at scale — no OOP, no developer convenience. Times have changed; people want tools that are easy to work with.

## Priority Stack
When goals conflict, resolve in this order:
1. Performance (runtime)
2. Compile time — must stay close to C/C++ compilers; no Rust-style compilation cost
3. Safety
4. Simplicity

## General Goals
1. Trust the programmer, but provide what they need.
2. As simple/minimal as it goes.
3. Don't force things which can be optional.
4. Performance is the ultimate goal.
5. Simple, clean, distinguishable and pragmatic syntax.
6. True to machine (less magic).
7. Reduce code nesting. (Applies to the language requirement only) [*]
8. Any additional and QOL features should possibly be zero cost.
9. Design that reduces code duplication. [*]

## Other Language Support
1. Assembly support.
2. Way to work with C/C++ code. Though, it shouldn't depend on them.

## Principles
Distilled from the closed philosophy decisions; operative rules live in spec.md.
1. Memory is manual and explicit; safety comes from tooling (checked builds), never from language-enforced rules.
2. No hidden control flow, no hidden costs: no exceptions, no propagation sugar, no COW, no inference, no implicit instantiation.
3. Mutation is visible — in signatures and at call sites; an unmarked call changes nothing you passed.
4. Machine truth: layout, sizes, and stack vs heap are statically known and visible.
5. Composition over inheritance; dispatch is an explicit value, never hidden machinery.
6. Errors are values, checked by hand; panic is abort, never a channel.
7. Start strict, relax later — never tighten.

## Considerations
1. Start strict, relax later: adding convenience later is backward-compatible; removing it is a break. Explicitness-first is the safe default (applies to types, syntax, stdlib).

## Cautions
1. Direct emission caps the optimization ceiling — if runtime benchmarks lag C by a widening margin, revisit the backend; keep codegen seams clean enough that a swap stays possible.
2. Explicit everything (types, instantiations, checks) is verbose — watch real-world ergonomics; relax per Considerations #1, never tighten.
3. No safety net beyond checked builds — the stdlib and docs must make allocator discipline and error checks idiomatic, or correctness rests on memory the programmer never sees.

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
