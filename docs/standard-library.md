# Standard library

The standard library ships beside the compiler in `std/`. Bare imports
(`import fmt`) resolve only there — a local file can never shadow a standard
module. How imports resolve, aliasing, and visibility are described in
[Modules](module.md).

Today's modules:

- `fmt` — allocation-free text output
- `math` — minimal generic helpers
- `mem` — raw allocation and allocation-free byte operations
- `net` — allocation-free IPv4 TCP sockets
- `sys` — file descriptors, byte I/O, errno, and monotonic time

Everything else in the planned v1 set (`str`, `simd`) does not
exist yet.

`mem` exports `alloc`, `realloc`, `release`, `copy`, `move`, `fill`, `zero`, and
`compare`. Import it qualified or select those names explicitly; there are no
unimported global memory functions. The current bootstrap compiler lowers
these exact standard-module functions directly to the platform boundary, with
no extra Ace call, allocation header, or registry. See [Memory](memory.md).

```ace
import alloc, release from mem

main(): int {
    data: []u8 = alloc(64, 8)
    release(mut data)
    return 0
}
```

## fmt

`fmt.print` writes bytes to standard output (file descriptor 1). It never adds
a newline, never allocates, and returns the number of bytes written, or the
non-positive result reported by the OS on failure. Partial writes are retried
until done or error.

Six overloads exist, selected exactly like any other overload set:

```ace
import fmt

main(): int {
    fmt.print("ready")           // prints: ready
    fmt.print(42)                // prints: 42
    fmt.print(uint(7))           // prints: 7
    fmt.print(usize(7))          // prints: 7
    fmt.print(0.5)               // prints: 0.5
    fmt.print(1.0e21)            // prints: 1e21
    return 0
}
```

- `print(text: string): int` — writes the bytes exactly.
- `print(value: int): int` — signed decimal, complete `int` range including
  `INT_MIN`, formatted through a fixed 20-byte stack buffer.
- `print(value: uint): int` — unsigned platform-word decimal, zero through
  `UINT_MAX`. Values typed `u64` select it directly.
- `print(value: usize): int` — unsigned decimal for lengths, sizes, and
  indices, zero through `USIZE_MAX`.
- `print(value: f32): int` and `print(value: f64): int` — shortest
  round-trip decimal: the fewest significant digits that read back as the
  exact original value.

Float output spells the special values `nan`, `inf`, and `-inf`, and keeps
the sign of a negative zero (`-0`). Values print in decimal notation when
the scientific exponent is between -6 and 20; anything larger or smaller
uses scientific spelling like `1e21`, `1.5e-7`, or `3.4028235e38` — no `+`,
no exponent padding. Whole-valued floats print without `.0` (`3.0` prints
as `3`).

Numeric output is ASCII with a `-` only for negative signed values. There is
no padding, no base selection, and no formatting language.

Integer widths narrower than the platform word have no overload; convert
explicitly at the call site — `fmt.print(int(my_i32))`,
`fmt.print(uint(my_u16))` — so the printed type is always visible.

`fmt.println` writes exactly the representation of the matching `fmt.print`
overload followed by one newline. It allocates nothing and returns the total
bytes written, including the newline. If writing the value fails, that
failure is returned and no newline is attempted; if the value is written but
the newline write fails, the newline's failure result is returned. An empty
string still emits the newline, so `fmt.println("")` returns `1`. There is
no zero-argument form — `fmt.print("\n")` already writes a bare newline.

The same six overload types are available (`string`, `int`, `uint`,
`usize`, `f32`, `f64`), selected exactly like `print`; narrower integer
widths convert explicitly (`fmt.println(int(my_i32))`).

```ace
import fmt

main(): int {
    fmt.println("ready")  // the bytes `ready` plus one newline
    fmt.println(42)       // the bytes `42` plus one newline
    fmt.println(0.5)      // the bytes `0.5` plus one newline
    return 0
}
```

`fmt.printf` combines static text and runtime values while keeping the runtime
as direct as separate typed `print` calls:

```ace
import fmt

main(): int {
    name: string = "Ace"
    count: int = 3
    ratio: f64 = 1.25
    return fmt.printf("name=%s count=%d ratio=%.2f\n", name, count, ratio)
}
```

The format must be a string literal or a const-global `string` initialized
from a static literal. The compiler parses it, checks the argument count and
exact types, and emits direct typed formatting calls. Runtime-created or
runtime-selected formats are rejected. There is no runtime parser, argument
pack, type tag, reflection, allocation, or C variadic call.

The initial conversions are deliberately small:

- `%d` accepts `int` and writes signed decimal.
- `%u` accepts `uint` or `usize` and writes unsigned decimal.
- `%x` and `%X` accept `uint` or `usize` and write hexadecimal digits in the
  selected case, without a prefix.
- `%f` accepts `f32` or `f64` and writes fixed decimal with six fractional
  digits by default. `%.Nf` writes exactly `N` fractional digits for
  `N` from 0 through 324. Rounding is nearest with exact ties going to an
  even final digit, independent of the caller's floating-point rounding mode;
  that mode is restored before `printf` returns. Negative zero and negative
  values rounded to zero keep their sign; special values are `nan`, `inf`,
  and `-inf`.
- `%s` accepts `string` and writes every byte exactly, including embedded NUL.
- `%c` accepts `u8` and writes exactly one byte. Wider Unicode characters use
  their UTF-8 bytes through `%s`; a future character facility may add more.
- `%%` writes one percent sign and consumes no argument.

Transparent aliases work as their target type. Distinct types and narrower
integers must be converted explicitly. A literal receives the conversion's
natural type: `int`, `uint`, `f64`, or `u8`; write `usize(...)` when that is
the intended unsigned type.

Width, padding, alignment, sign and alternate-form flags, dynamic `*`, and
positional arguments are not implemented. The only modifier is the literal
precision on `%f`.

All runtime arguments are evaluated exactly once, in source order, before
output begins. On success `printf` returns the complete byte count. Positive
partial writes are retried; a zero or negative OS result stops later output
and is returned unchanged, matching `print` and `println`. Formatting uses a
fixed 640-byte stack buffer and never allocates. `fmt.printf` is direct-call
only and cannot be stored as a function value because its checked argument
shape depends on the static format.

## math

Minimal, pure-Ace, no floating-point functions yet. All three are generic and
require explicit instantiation:

```ace
import fmt
import math

main(): int {
    fmt.print(math.min[int](3, 9))   // prints: 3
    fmt.print(math.max[int](3, 9))   // prints: 9
    fmt.print(math.abs[int](0 - 7))  // prints: 7
    return 0
}
```

- `min[T](a: T, b: T): T`
- `max[T](a: T, b: T): T`
- `abs[T](x: T): T` — negates when `x < 0`

The body is checked per instantiation; `T` must support the comparison and
arithmetic used by the function.

## sys

`sys` is the thin operating-system boundary: file I/O and monotonic time. It
never allocates — only callers allocate — and every failure is an ordinary
`Error` value checked with a branch.

```ace
import sys

main(): int {
    fd: sys.Fd, err: Error = sys.open("/tmp/notes.txt",
                                      sys.O_WRONLY | sys.O_CREAT | sys.O_TRUNC,
                                      u32(0x1A4))
    if err != null {
        return 1
    }
    n: int, werr: Error = sys.write(fd, "hello sys")
    if werr != null {
        return 2
    }
    if sys.close(fd) != null {
        return 3
    }
    return 0
}
```

- `Fd: distinct i32` is the descriptor handle. Zero is fd 0 (standard input,
  a valid descriptor), so zero is not an invalid marker; a failed `open`
  returns `fd == -1` alongside its error.
- `open(path: string, flags: i32): (Fd, Error)` and the three-argument form
  add a `mode: u32` used with `O_CREAT`. The path is copied into a fixed
  1024-byte stack buffer and NUL-terminated; longer paths fail with
  `ENAMETOOLONG` through the normal error path, exactly as the kernel would
  reject them.
- `read(fd: Fd, mut buf: []u8): (int, Error)` fills the caller's buffer and
  returns the byte count; `0` with a null error is end of file.
- `write(fd: Fd, data: []u8): (int, Error)` and `write(fd: Fd, text: string)`
  return the byte count as the operating system reported it — partial writes
  are legitimate results and are never retried here.
- `close(fd: Fd): Error`.
- `errno(): i32` reads the thread's raw errno. Query it immediately after a
  failed call; any intervening system call may overwrite it.

Flags are raw constants with the operating system's own values and spellings
(`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_NONBLOCK`, `O_APPEND`, `O_CREAT`,
`O_TRUNC`, `O_EXCL`), composed with `|`. Permission bits are plain hex
(`u32(0x1A4)` is `0644`).

Each failing operation returns an error whose descriptor domain names the
operation — `sys.OpenErr`, `sys.ReadErr`, `sys.WriteErr`, `sys.CloseErr` —
so `err.domain == typeid(sys.ReadErr)` identifies what failed, and
`sys.errno()` says why:

```ace
fd: sys.Fd, err: Error = sys.open("/tmp/missing", sys.O_RDONLY)
if err != null {
    if sys.errno() == 2 { // ENOENT
        // handle a missing file
    }
}
```

`time_mono(sleep: bool): u64` reads a monotonic clock in nanoseconds from an
unspecified origin. The flag chooses between the two flavors of monotonic
time:

- `sleep == true` — the clock keeps advancing while the system is asleep.
- `sleep == false` — the clock pauses while the system is asleep and advances
  whenever the system is awake, including while this process is idle or
  blocked. It is not process CPU time.

Both modes are immune to calendar-clock changes. Subtract only readings taken
with the same `sleep` argument — cross-mode differences are meaningless. The
call allocates nothing and cannot fail.

```ace
import sys

main(): int {
    start: u64 = sys.time_mono(false)
    sum: int = 0
    for i: int in 0 .. 1_000_000 {
        sum += i
    }
    elapsed_ns: u64 = sys.time_mono(false) - start
    if elapsed_ns == 0 {
        return 1
    }
    return 0
}
```

Other operating-system surfaces (directory operations, calendar time, CPU
time) are not implemented yet.

## net

`net` provides a thin, allocation-free IPv4 TCP layer. Its public values and
operations are platform-neutral; Darwin socket structures, constants, byte
order, and C bindings stay private inside the module. `net.Socket` is a
transparent alias of `sys.Fd`, so close sockets with `sys.close`.

```ace
import net
import sys

main(): int {
    sock: net.Socket, err: Error = net.socket(net.SocketKind.tcp)
    if err != null {
        return 1
    }
    ip: [4]u8 = [u8(127), u8(0), u8(0), u8(1)]
    address: net.Address4 = net.Address4{ip: ip, port: u16(8080)}
    if net.set_reuse_address(sock, true) != null ||
       net.bind(sock, address) != null ||
       net.listen(sock, usize(128)) != null {
        sys.close(sock)
        return 2
    }
    if sys.close(sock) != null {
        return 3
    }
    return 0
}
```

`Address4: struct { ip: [4]u8, port: u16 }` stores IP octets in ordinary
textual order and the port in host order. Its canonical zero is
`0.0.0.0:0`, usable as the wildcard address with an OS-selected port.
Parsing, formatting, DNS, IPv6, and local/peer address queries are not part of
this initial surface.

The core operations each perform one explicit socket operation and never
allocate, buffer, retry, close, or replace a socket on failure:

- `socket(SocketKind.tcp): (Socket, Error)` creates an IPv4 TCP socket.
- `set_reuse_address` and `set_reuse_port` reversibly set their named option.
- `set_nonblocking(socket, enabled): Error` reversibly changes blocking mode.
- `bind(socket, Address4): Error` binds without enabling options implicitly.
- `listen(socket, backlog: usize): Error` starts listening. The OS may cap the
  backlog; a value too large for the platform call fails instead of truncating.
- `accept(listener): (Socket, Address4, Error)` makes one blocking acceptance
  attempt. It returns the peer address and does not configure the accepted
  socket. Failure returns `Socket(-1)` and canonical-zero `Address4`.
- `connect(socket, Address4): Error` makes one connection attempt without
  retry, cleanup, or configuration.
- `receive(socket, mut buffer: []u8): (int, Error)` performs one receive.
  Partial counts are returned as-is; zero with null error is orderly peer
  shutdown.
- `send(socket, []u8)` and `send(socket, string)` each perform one send and
  return partial counts as-is. A closed peer becomes an ordinary send error
  rather than terminating the process with `SIGPIPE`.

Every error is last and null on success. The operation-specific public domains
are `SocketErr`, `SetReuseAddressErr`, `SetReusePortErr`,
`SetNonblockingErr`, `BindErr`, `ListenErr`, `AcceptErr`, `ConnectErr`,
`ReceiveErr`, and `SendErr`. Compare `error.domain` with the corresponding
`typeid(net.*Err)`; query `sys.errno()` immediately to learn the OS reason.
Closing uses `sys.close` and therefore retains `sys.CloseErr`.

No retry policy is hidden. In particular, interrupted, aborted, would-block,
and resource failures from `accept` remain caller-visible. Nonblocking
connection completion, readiness queues, half-close, additional TCP options,
UDP, and higher-level server/client convenience remain future additions.

## Current limitations

- `fmt.printf` supports only its documented initial conversions and literal
  float precision; runtime formats, width/flags, dynamic precision, and
  positional arguments are not implemented.
- No output overloads for narrower integer widths; an explicit cast selects
  between the `int`, `uint`, and `usize` overloads.
- `math` has no `sqrt`, floats, or libm binding.
- There is no user-configurable search path for `std/`.
- `sys` has no directory surface, calendar-time or CPU-time clock, or
  `unlink`; tests that create files leave them behind.
- Apple-arm64 extern C variadic calls use the platform's stack convention for
  unnamed arguments. `sys` still binds fixed-arity `__open` because its public
  wrapper has a fixed signature and needs no variadic boundary.
