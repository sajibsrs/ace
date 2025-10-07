# Philosophy

## General Goals
1. Trust the programmar, but provide what they needs.
2. As simple/minimal as it goes.
3. Don't force thing what can be optional.
4. Performance is the ultimate goal.
5. Simple, clean, distinguishable and pragmatic syntax.
6. True to machine (less magic).
7. Reduce code nesting. (Applies to the language requirement only) [*]
8. Any additional and QOL features should possibily be zero cost.
9. Design that reduces code duplication. [*]

## Other Language Support
1. Assembly support.
2. Way to work with C/C++ code. Though, it shouldn't depend on them.

## Source
1. The initially proposed file extensions are **.acx** , **.ax**, **.cx**. Where **.acx** has been confirmed. [?]

## Parsing
1. Single lookahead parsing LL(1) with little or no backtracking.
2. Recursive descent top-down parsing implementation.

## Compiler
1. Make memory size available to programmar. (see: Functions #1)
2. Single pass compilation / two-if needed. [?]
3. Name mangling support. [?][!]

## Conditions
1. Two (multi-way) comparisons. [?][!]

## Functions
1. Support muti-return (e.g. return value, memory size, error). [?]

## Iterations
1. Memory size (or byte) based iteration.

## Decisions


## Cautions


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
