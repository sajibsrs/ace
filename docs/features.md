# Features at a glance

What Ace gives a C programmer today, in one page. Every item is implemented
on `darwin-arm64`; the linked guide pages hold the rules and examples.

## The machine, without the undefined behavior

- **No undefined behavior.** Integer arithmetic wraps, a shift count is taken
  modulo the width, and bounds, null dereference, and division by zero trap
  in every build. Debug builds add contract and allocator diagnostics.
  Release keeps the three traps and strips the rest.
  [Errors](errors.md), [Memory](memory.md)
- **Debug and release compute the same result.** The optimizer uses only
  facts the hardware guarantees or the programmer stated: no type-based
  aliasing, no float contraction, no inference from "this cannot happen".
  [Compiler usage](compiler.md)
- **Layout is a promise, not a detail.** A slice is two words, a string is a
  byte slice with no terminator, structs keep declaration order, enums and
  tagged unions have stated representations, and `sizeof`, `alignof`,
  `offsetof`, and `static_assert` check it at compile time.
  [Memory](memory.md), [Types](types.md)
- **Explicit struct layout.** `@packed`, `@align(N)`, `@offset(N)`, and
  overlapping structs for C unions. `bits u32 { enabled: 1, mode: signed 3, _: 28 }`
  declares exact bit fields over one integer. [Types](types.md)
- **`volatile` is an access, not a type.** `volatile reg = 1` is one store
  of exactly the field's width; nothing is widened, split, or merged.
  [Memory](memory.md)
- **Stack or heap is always visible.** A value lives on the stack unless
  `mem.alloc` put it on the heap; no hidden allocation exists anywhere in the
  language or `std`. [Memory](memory.md)

## Manual memory, with the sharp edges filed

- **One allocator, explicit failure.** `mem.alloc[T](n)` returns a typed
  slice or `null`; `mem.release[T](&data)` frees and nulls the caller's
  variable. No GC, no ownership system, no allocator parameters.
  [Memory](memory.md)
- **Definite initialization.** Reading a local before it is written is a
  compile error. Every type has a canonical all-zero value, so `blank(T)`
  and zeroed allocations are always valid. [Types](types.md)
- **Slices instead of pointer arithmetic.** `for b: u8 in buf` is a bounded
  pointer-bump loop with no per-iteration check. Pointer arithmetic exists
  but is narrow and explicit. [Control flow](control-flow.md)
- **Views that cannot escape their frame.** Returning or storing a view of
  a local is a compile error. [Memory](memory.md)
- **Read-only memory is enforced by the MMU.** String literals and `const`
  globals live in read-only pages; the type system adds no permission layer.

## Errors and cleanup without a runtime

- **Errors are one word.** `Error` packs a domain id and an `i32` code into
  64 bits, so `(usize, Error)` returns in registers. Names and messages are
  static data emitted only when used. [Errors](errors.md)
- **Nominal error domains.** `OpenErr: error { 2: not_found = "no such file" }`
  gives typed, comparable, self-describing failures with no allocation.
- **Multi-return, destructured.** `n: int, err: Error = read()`; the error
  is always last, `null` means success, and the check is a plain `if`.
- **`defer` expanded inline.** Cleanup runs at every block exit in reverse
  order with no registration, counter, or unwinder. Labeled `break` replaces
  `goto` for cleanup paths. [Control flow](control-flow.md)
- **`panic` is abort.** Never catchable, never a channel.

## Abstraction you can see the cost of

- **Contracts are a vtable and a data pointer.** `Shape(&circle)` builds
  one; `s.area()` is one indirect call. Each struct has one explicit signing;
  there is no trait solving or runtime lookup. [Types](types.md)
- **Composition, not inheritance.** `embed base: Base` stores a field and
  promotes its methods one level. [Types](types.md)
- **Explicit generics.** `max[int](a, b)` stamps plain code for exactly the
  types written. A `specialized` template with per-type bodies replaces
  constraint systems: a missing specialization is a compile error at the
  instantiation. [Functions](function.md)
- **Exact overloading.** Argument count and types must match one candidate;
  there is no conversion ranking. [Functions](function.md)
- **Tagged unions with exhaustive `switch`.** `Event: union { quit, key: u32 }`
  is a discriminant plus inline payload; every variant is named or `default`
  is. [Types](types.md)
- **Type-owned methods and associated functions.** `Circle.area(const self)`
  and `Map[K, V].new(capacity)`, both reached with `.`. [Functions](function.md)

## Syntax that fixes C's papercuts

- **One declaration shape.** `name: type` everywhere: locals, fields,
  parameters, `Name: struct { }`, `Name: enum { }`, `LIMIT: const int = 8`,
  `Meters: distinct f64`, `Id: type = u32`. [Types](types.md)
- **No inference, no implicit conversion.** Every type is written; a cast is
  `i32(x)`. Untyped numeric literals still fit where they are used.
  [Primitives](primitives.md)
- **Chained comparisons.** `lo <= x < hi` evaluates each operand once; C's
  `a < b < c` trap is gone.
- **Precedence that cannot bite.** `b & 0xC0 != 0x80` is a parse error; a
  nested `?:` must be parenthesized; `++` and `--` do not exist.
- **One loop keyword.** `for` covers ranges, sequences with optional index,
  conditions, three-clause heads, and infinite loops. Ranges are half-open
  and reverse by swapping endpoints. [Control flow](control-flow.md)
- **`switch` without fallthrough.** Cases, ranges, and union patterns; an
  explicit `fallthrough` statement when it is wanted.
- **Enums that do more.** Explicit backing types, open enums for values C
  may extend, `[E]T` arrays indexed only by `E` with no bounds check,
  `members(E)`, and `.name`. [Types](types.md)
- **`StaticMap[K]V`.** A compile-time perfect-hash table in read-only memory,
  looked up with `table[key]`.

## Modules and packages that stay explicit

- **File-level privacy.** `pub` on the declaration or field; nothing is
  re-exported by an import; import cycles are a compile error.
  [Modules](module.md)
- **Qualified by default.** `import math` then `math.sqrt(x)`; selective
  `import min, max from math` when wanted.
- **`@when` is the only conditional compilation.** A closed expression over
  the target name attached to one declaration. No macro layer.
- **Exact packages.** `ace.pkg` names one exact version per dependency, all
  found in a flat `pkg/` directory. Native link inputs are exact paths or a
  URL with its SHA-256. [Packages](package.md)
- **C interop in both directions.** `extern "c"` blocks import and export
  under a checked C-ABI whitelist; Ace function values convert to C function
  pointers with no wrapper. [Modules](module.md)

## A compiler that is its own toolchain

- **No external assembler or backend.** Ace lowers to SSA, optimizes, selects
  AArch64 instructions, allocates registers, and writes Mach-O itself. The
  compiler compiles to object far faster than `cc` at every corpus size.
  [Compiler usage](compiler.md), [bench](../bench/README.md)
- **Release optimizations that keep results identical.** Constant
  propagation, value numbering, check elimination, loop-invariant motion,
  strength reduction, small-leaf inlining, self-tail-call loops, and
  lane-wise auto-vectorization with bit-identical results.
- **Guaranteed `inline`.** A keyword that expands every direct call, not a
  hint.
- **Inline assembly with named registers.** `asm "svc #0" { in: x0 = fd, x16 = 3 }`;
  no constraint language.
- **Self-hosting.** A compiler written in Ace compiles, links, and checks
  itself against the bootstrap. [Self-hosting](selfhost.md)
- **Language server and agent server.** `ace-lsp` and `ace-mcp` from the
  selfhost front end. [Language server](lsp.md), [Agent server](mcp.md)

## A standard library that owes nothing to libc

- **Pure Ace math.** `sqrt`, `floor`, `ceil`, `fma` lower to instructions;
  `exp`, `log`, `pow`, `sin`, `cos` are Ace code within 1 ULP on the whole
  finite domain, identical on every target. [Standard library](std/index.md)
- **Allocation-visible collections.** `array.Array[T]` and `map.Map[K, V]`
  whose zero value is a valid empty container and whose growth reports
  failure as an `Error`.
- **Threads, atomics, and a memory model.** `thread.Thread`, `Mutex`,
  `Condition`, `threadlocal` variables, and lock-free `atomic` operations
  with stated happens-before rules.
- **Sockets, polling, files, processes, signals, SHA-256, UTF-8, SIMD.**
  Each behind a stable facade over the OS's public interface.
- **First-party packages.** HTTP/1.1 client and server, RFC 8259 JSON, a
  linear-time regex engine, an SDL3 binding, and an immediate-mode UI.
