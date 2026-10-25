# Standard library

The standard library ships beside the compiler in `std/`. Bare imports
(`import fmt`) resolve only there — a local file can never shadow a standard
module. How imports resolve, aliasing, and visibility are described in
[Modules](../module.md).

Today's modules:

- [`array`](array.md) — growable sequence over the process allocator
- [`map`](map.md) — hash map over the process allocator
- [`sort`](sort.md) — in-place allocation-free sorting of slices
- [`atomic`](atomic.md) — lock-free `u32` and `u64` operations with explicit ordering
- [`bits`](bits.md) — explicit machine-representation access and operations
- [`exec`](exec.md) — starting programs as child processes
- [`file`](file.md) — whole-file reads and writes over `sys`
- [`fmt`](fmt.md) — allocation-free text output
- [`math`](math.md) — minimal generic helpers and float classification
- `mem` — raw and typed allocation plus allocation-free byte operations (see
  [`bits`](bits.md) and [Memory](../memory.md))
- [`net`](net.md) — allocation-free IPv4 TCP sockets
- [`poll`](poll.md) — allocation-free readiness queues for general file descriptors
- [`signal`](signal.md) — signal handlers, per-thread masks, waits, and delivery
- [`str`](str.md) — allocation-free byte-exact string utilities
- [`tcp`](tcp.md) — explicit blocking IPv4 TCP composition over `net`
- [`test`](test.md) — table-driven test runner
- [`sys`](sys.md) — process arguments/environment, file descriptors, byte I/O, and errno
- [`thread`](thread.md) — OS threads, mutexes, and condition variables
- [`time`](time.md) — monotonic clocks
- [`ascii`](ascii.md) — allocation-free ASCII-only text operations
- [`utf8`](utf8.md) — allocation-free strict UTF-8 encoding and decoding

Everything else in the planned v1 set (`simd`) does not
exist yet.

The `std/@/` directory contains compiler-reserved standard-library
implementation, not public modules. Ordinary standard-library modules use the
visibly distinct unquoted form `import big: @bigint`, and reserved files may
import one another the same way. Applications and packages cannot use these
modules; canonicalization prevents traversal and symlink aliases from bypassing
that boundary. Equivalent source copied outside the compiler-shipped `std/`
tree is ordinary code. The suffixless `@name` form is reserved import syntax,
not a general path prefix or user-definable namespace.

Public names use module context to stay short without becoming cryptic. Types
and error domains use compact PascalCase, functions use lowercase words with
underscores where separation helps, and only established unambiguous terms are
shortened (`Fd`, `Addr`, `Err`). Identifying words otherwise remain complete:
for example, `sys.ProcessStatus` is not shortened to the ambiguous
`sys.ProcessStat`. A directory-module facade and its selected OS layer expose
the same names.

## Current limitations

- `fmt.printf` supports only its documented conversions, literal width, the
  `0` flag, and literal float precision; runtime formats, other flags,
  dynamic width or precision, and positional arguments are not implemented.
- No output overloads for narrower integer widths; an explicit cast selects
  between the `int`, `uint`, and `usize` overloads.
- `math` has no `sqrt`, floats, or libm binding.
- There is no user-configurable search path for `std/`.
- `sys` has no directory surface, calendar-time or CPU-time clock, or
  `unlink`; tests that create files leave them behind.
- Apple-arm64 extern C variadic calls use the platform's stack convention for
  unnamed arguments. `sys` still binds fixed-arity `__open` because its public
  wrapper has a fixed signature and needs no variadic boundary.

## Design rationale

Why the rules in `spec/std/README.md` are shaped as they are.

- Stdlib: module context and conventional abbreviations remove noise without
  introducing a private shorthand vocabulary or making related facade and target
  APIs differ.
- Stdlib #5: one conventional entry name gives a directory module an explicit,
  auditable boundary without repeating its public name, while exact user paths
  and ambiguity diagnostics keep resolution simple. The suffixless `@name`
  spelling presents a reserved module rather than an implementation filename and
  remains visibly distinct from userland and ordinary standard modules.
- Stdlib #8: the compact established names remain unambiguous under the `math`
  qualifier, while representation access stays centralized in `bits`.
- Stdlib #8: each operation has one exact result, so no accuracy contract is
  needed, and a compiler-provided instruction is the machine truth.
- Stdlib #8: a stated bound is testable and portable, and in-Ace implementations
  leave `math` usable without a system library.
- Stdlib #8: source states honestly that no Ace body exists, the public API
  remains first-class, and the exact whitelist adds machine operations without
  exposing backend internals or an unrestricted intrinsic namespace.
- Stdlib #21: convenience must not cost control. Keeping every capability public
  lets callers replace a convenience when its policy does not fit, and since Ace
  has no allocator values, the returned ownership is what makes allocation
  visible.
