# Standard library

The standard library ships beside the compiler in `std/`. Bare imports
(`import fmt`) resolve only there — a local file can never shadow a standard
module. How imports resolve, aliasing, and visibility are described in
[Modules](../module.md).

Today's modules:

- [`array`](array.md) — growable sequence over the process allocator
- [`map`](map.md) — hash map over the process allocator
- [`bits`](bits.md) — explicit machine-representation access and operations
- [`fmt`](fmt.md) — allocation-free text output
- [`math`](math.md) — minimal generic helpers and float classification
- `mem` — raw and typed allocation plus allocation-free byte operations (see
  [`bits`](bits.md) and [Memory](../memory.md))
- [`net`](net.md) — allocation-free IPv4 TCP sockets
- [`poll`](poll.md) — allocation-free readiness queues for general file descriptors
- [`str`](str.md) — allocation-free byte-exact string utilities
- [`tcp`](tcp.md) — explicit blocking IPv4 TCP composition over `net`
- [`sys`](sys.md) — process arguments/environment, file descriptors, byte I/O, and errno
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

- `fmt.printf` supports only its documented conversions and literal float
  precision; runtime formats, width/flags, dynamic precision, and positional
  arguments are not implemented. Binary/octal/hexadecimal output therefore
  cannot yet request fixed width, zero padding, or a base prefix.
- No output overloads for narrower integer widths; an explicit cast selects
  between the `int`, `uint`, and `usize` overloads.
- `math` has no `sqrt`, floats, or libm binding.
- There is no user-configurable search path for `std/`.
- `sys` has no directory surface, calendar-time or CPU-time clock, or
  `unlink`; tests that create files leave them behind.
- Apple-arm64 extern C variadic calls use the platform's stack convention for
  unnamed arguments. `sys` still binds fixed-arity `__open` because its public
  wrapper has a fixed signature and needs no variadic boundary.
