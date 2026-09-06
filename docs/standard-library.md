# Standard library

The standard library ships beside the compiler in `std/`. Bare imports
(`import fmt`) resolve only there — a local file can never shadow a standard
module. How imports resolve, aliasing, and visibility are described in
[Modules](module.md).

Today's modules:

- `fmt` — allocation-free text output
- `math` — minimal generic helpers
- `mem` — raw and typed allocation plus allocation-free byte operations
- `net` — allocation-free IPv4 TCP sockets
- `poll` — allocation-free readiness queues for general file descriptors
- `str` — allocation-free byte-exact string utilities
- `sys` — process arguments/environment, file descriptors, byte I/O, errno,
  and monotonic time

Everything else in the planned v1 set (`simd`) does not
exist yet.

`mem` exports `alloc`, `realloc`, `release`, `copy`, `move`, `fill`, `zero`,
`compare`, and `equal`. Each allocation name is one overload set with a raw byte form and an
explicitly instantiated typed form. Import the module qualified or select those
names explicitly; there are no unimported global memory functions. The current
bootstrap compiler lowers these exact standard-module functions directly to the
platform boundary, with no extra Ace call, allocation header, or registry. See
[Memory](memory.md).

```ace
import alloc, release from mem

main(): int {
    data: []u8 = alloc(64, 8)
    release(mut data)

    numbers: []usize = alloc[usize](4)
    if numbers == null {
        return 1
    }

    numbers[0] = 42
    release[usize](mut numbers)
    return 0
}
```

- `alloc(size: int, align: int): mut []u8` — writable raw bytes with an explicit size and
  power-of-two alignment.
- `alloc[T](count: usize): mut []T` — writable typed storage: `count * sizeof(T)` bytes, alignment
  taken from `T`, element length `count`, uninitialized storage, null on
  overflow/zero-size/failure.
- `realloc(data: []u8, new_size: int): mut []u8` — raw growth; failure leaves the
  original live.
- `realloc[T](data: []T, new_count: usize): mut []T` — typed growth with the same
  failure contract; the result is the only authoritative slice.
- `release(mut data: []u8)` and `release[T](mut data: []T)` — free the complete
  authoritative allocation and null only the passed variable; null is a no-op.
- `equal(a, b): bool` — exact, allocation-free content equality for every
  pairing of `[N]u8`, `[]u8`, and `string`; sequential `==`/`!=` is unavailable.

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
exact types, and emits direct typed formatting calls. Those calls are
exact-type specializations of the private per-shape generics in `std/fmt/module.ace`
— `print_counted[T](value: T): int` for text, bytes, and decimal integers,
`print_precision[T](value: T, precision: usize): int` for fixed floats, and
`print_hex[T](value: T, uppercase: bool): int` for hexadecimal — selected by
the format interpreter's concrete argument type (see
[Functions](function.md#generic-specialization)). Runtime-created or
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

## str

`str` is a pure-Ace module of allocation-free, byte-exact string utilities.
Plain strings are read-only UTF-8 byte views, `char` is the transparent `u8`
alias, and every operation works on views or caller storage: nothing here
allocates, decodes Unicode, normalizes, or uses the locale. Each
`suffix: bool` parameter selects a boundary with `false` for the prefix and
`true` for the suffix.

```ace
import str

main(): int {
    at: usize, found: bool = str.find("hello world", "world")
    if !found {
        return 1
    }
    trimmed: string, hit: bool = str.trim("hello world", "world", true)
    if !hit || trimmed != "hello " {
        return 2
    }
    return 0
}
```

Search and ordering:

- `find(text: string, needle: string): (usize, bool)` and
  `find(text: string, needle: char): (usize, bool)` — the first byte index and
  whether it was found; an empty string needle is found at zero and absence
  returns canonical zero.
- `has(text: string, needle: string): bool`,
  `has(text: string, needle: char): bool` — occurrence anywhere; an empty
  string needle is present.
- `has(text: string, part: string, suffix: bool): bool` — one boundary test;
  an empty string part matches either boundary.
- `compare(a: string, b: string): int` — unsigned byte-wise order returning
  exactly `-1`, `0`, or `1`; after an equal prefix the shorter string sorts
  first.

Caller storage and views:

- `copy(text: string, noalias destination: mut []u8): bool` — copies the
  complete text into `destination[0..len(text)]`; a shorter destination is
  unchanged and fails, and the unused suffix is untouched. Empty text succeeds
  without writing. Unlike `mem.copy`, the ranges need not be equal — only the
  source and the written range must not overlap.
- `split(text: string, separator: string, parts: mut []string): (usize, bool)`
  and the `separator: char` overload — splits left to right on non-overlapping
  exact matches. Separators are omitted, boundaries produce empty parts, empty
  input produces one empty part, and every written part is a zero-copy view
  into `text`. The result is the number written and whether the complete
  result fit; unwritten slots are unchanged. An empty separator returns
  `(0, false)` without writing.
- `trim(text: string, part: string, suffix: bool): (string, bool)` and the
  `part: char` overload — removes at most one exact boundary match, returning
  the remaining zero-copy view and whether it matched.
- `trim(text: string): string` — removes all leading and trailing ASCII
  whitespace bytes (`0x09`–`0x0D` and `0x20`) as a zero-copy view; no Unicode
  whitespace processing.

Numeric conversion — the explicitly instantiated `parse[T]` overloads and the
ordinary `format` overloads. The `parse[T]` templates are declared
`specialized` and are implemented entirely as generic specializations (see
[Functions](function.md#generic-specialization)): the callable surface is the
per-type specializations for `int`, `uint`, `usize`, `f32`, and `f64`, so any
other type argument is a call-site compile error naming the missing tuple:

```ace
import str

main(): int {
    n: int, ok: bool = str.parse[int]("-42")
    if !ok {
        return 1
    }
    r: usize, radix_ok: bool = str.parse[usize]("ff", u8(16))
    if !radix_ok || r != 255 {
        return 2
    }
    buf: [32]u8 = [0 : 32]
    dst: mut []u8 = mut buf[0..32]
    count: usize, fit: bool = str.format(dst, n)
    if !fit || count != 4 {
        return 3
    }
    return 0
}
```

- `parse[T](text: string): (T, bool)` — the default form admits exactly `int`,
  `uint`, `usize`, `f32`, and `f64`. Parsing consumes the complete nonempty
  string: no surrounding whitespace, no leading `+`, leading `-` only for
  `int` and the signed floats, and leading zeroes accepted.
- `parse[T](text: string, radix: u8): (T, bool)` — the radix form admits
  exactly the three integer types and radices 2 through 36. Digits above nine
  are ASCII letters, case-insensitive; the form accepts digits only, with no
  `0x` or `0b` prefix.
- Invalid text, an unsupported runtime radix, or overflow returns
  canonical-zero `T` and `false`; finite float underflow rounded to signed
  zero succeeds. Decimal floats accept ordinary decimal/scientific forms plus
  the exact non-finite spellings `fmt` produces (`nan`, `inf`, `-inf`).
  Naming any other `T` is a compile-time error at the call.
- `format(destination: mut []u8, value): (usize, bool)` — overloads for
  `int`, `uint`, `usize`, `f32`, and `f64`, plus radix-taking overloads for
  the three integer types. The complete representation is written or, when
  capacity is insufficient or the runtime radix is outside 2 through 36,
  nothing is written and the result is `(0, false)`. Integer radix digits are
  lowercase ASCII with no base prefix, only `int` may emit `-`, and the
  no-radix float representations are exactly the matching `fmt.print`
  overloads'.

## sys

`sys` is the thin operating-system boundary: process-start values, file I/O,
and monotonic time. It
never allocates — only callers allocate — and every failure is an ordinary
`Error` value checked with a branch.

```ace
import sys

main(): int {
    fd: sys.Fd, err: Error = sys.open("/tmp/notes.txt",
                                      sys.O_WRONLY | sys.O_CREAT | sys.O_TRUNC,
                                      sys.S_IRUSR | sys.S_IWUSR |
                                      sys.S_IRGRP | sys.S_IROTH)
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
- `stdin`, `stdout`, and `stderr` are `const Fd` values for the three standard
  descriptors (0, 1, and 2), valid from process start. They flow through the
  ordinary operations — `sys.read(sys.stdin, buf)` fills from standard
  input and `sys.write(sys.stderr, "oops")` writes to standard error.
- `open(path: string, flags: i32): (Fd, Error)` and the three-argument form
  add a `mode: u32` used with `O_CREAT`. The path is copied into a fixed
  1024-byte stack buffer and NUL-terminated; longer paths fail with
  `ENAMETOOLONG` through the normal error path, exactly as the kernel would
  reject them.
- `read(fd: Fd, buf: mut []u8): (int, Error)` fills the caller's buffer and
  returns the byte count; `0` with a null error is end of file.
- `write(fd: Fd, data: []u8): (int, Error)` and `write(fd: Fd, text: string)`
  return the byte count as the operating system reported it — partial writes
  are legitimate results and are never retried here.
- `close(fd: Fd): Error`.
- `stat(path: string): (FileInfo, Error)` follows symbolic links and returns
  byte size plus a `FileKind` of `unknown`, `regular`, `directory`, `symlink`,
  or `other`.
- `mkdir(path: string): Error` is the portable creation form. It requests all
  ordinary permissions (`0777` on POSIX), after which the process umask chooses
  the effective permissions. It creates one directory and does not create
  missing parents.
- `mkdir(path: string, mode: u32): Error` explicitly supplies Darwin/POSIX
  creation permissions. This overload is target-specific rather than a claim
  that POSIX mode bits model Windows ACLs.
- `rename(old_path: string, new_path: string): Error` and
  `unlink(path: string): Error` directly perform the corresponding filesystem
  operation without recursion or retry.
- `errno(): i32` reads the thread's raw errno. Query it immediately after a
  failed call; any intervening system call may overwrite it.

Directory traversal keeps its buffer and cursor in a caller-owned `Dir` value:

```ace
dir: sys.Dir, err: Error = sys.open_dir("/tmp")
if err != null {
    return 1
}
name_storage: [256]u8
name: mut []u8 = mut name_storage[0..256]
done: bool = false
for !done {
    count: usize, kind: sys.FileKind, at_end: bool, read_err: Error = sys.readdir(mut dir, name)
    if read_err != null {
        sys.close_dir(mut dir)
        return 2
    }
    done = at_end
    if !done {
        // The entry name is name[0..count]; no NUL byte is appended.
    }
}
if sys.close_dir(mut dir) != null {
    return 3
}
return 0
```

`readdir` skips `.` and `..` and returns values in the fixed order name byte
count, kind, end flag, and `Error`. End is `(0, unknown, true, null)`. A kind
may be `unknown`; use `stat` when classification is required. If the name
buffer is too small, `ReadDirErr` reports `ERANGE`, leaves the entry pending,
and writes no truncated name. Enumeration retains operating-system order.

The call-site mark in `readdir(mut dir, name)` permits updating the caller's
iteration state. It is independent of the type-position `mut` in
`name: mut []u8`, which permits writing the supplied bytes. Likewise,
`close_dir(mut dir)` closes the descriptor and replaces that caller's complete
`Dir` value with canonical zero. Copying a `Dir` creates an unmanaged alias;
closing one copy does not find or invalidate the others.

Process arguments and raw environment entries are zero-copy string views:

```ace
argument_count: usize = sys.argc()
for i: usize in 0 .. argument_count {
    fmt.println(sys.arg(i))
}

environment_count: usize = sys.envc() // capture once: envc() is O(n)
for i: usize in 0 .. environment_count {
    fmt.println(sys.env(i)) // raw KEY=VALUE bytes
}
```

`sys.exit(code: int)` terminates the process immediately with the requested
status and never returns. It is callable from any function and has no declared
return value. It does not unwind the stack, run function-scope defers, or invoke
C-runtime exit handlers. The operating system determines which status bits a
parent process observes. Use it for deliberate CLI termination; `panic` instead
reports a broken invariant best-effort and aborts with `SIGABRT`.

`arg(0)` is the program invocation name. `arg(index)` requires
`index < argc()`, and `env(index)` requires `index < envc()`. These are the
ordinary sequence bounds preconditions: future checked builds diagnose a
violation, while the current release path assumes valid indexes. `argc()` is
constant-time; `envc()` scans the null-terminated environment pointer array on
every call, so capture it before iteration. Each selected entry is scanned for
its byte length only when requested. Argument views remain valid for the
process lifetime. Behavior after linked C code mutates the environment is not
specified yet.

Flags are raw constants with the operating system's own values and spellings
(`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_NONBLOCK`, `O_APPEND`, `O_CREAT`,
`O_TRUNC`, `O_EXCL`), composed with `|`. Explicit POSIX creation permissions
use `S_IRWXU`, `S_IRUSR`, `S_IWUSR`, `S_IXUSR`, the corresponding `S_I*GRP`
constants, and the corresponding `S_I*OTH` constants. They compose with `|`;
for example, `S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH` requests `0644`.
The process umask may remove requested bits. Windows permissions are ACL-based,
so a future Windows target will keep the portable one-argument operation and
carve its richer security surface separately rather than pretending these bits
are universal.

Each failing operation returns an ordinary packed `Error` whose domain names
the operation — `sys.OpenErr`, `sys.ReadErr`, `sys.WriteErr`, `sys.CloseErr`,
`sys.StatErr`, `sys.MkdirErr`, `sys.RenameErr`, `sys.UnlinkErr`,
`sys.OpenDirErr`, `sys.ReadDirErr`, or `sys.CloseDirErr` —
so `err.domain == typeid(sys.ReadErr)` identifies what failed, and `err.code`
is the errno captured at the failure:

```ace
fd: sys.Fd, err: Error = sys.open("/tmp/missing", sys.O_RDONLY)
if err != null {
    if err.code == 2 { // ENOENT
        // handle a missing file
    }
}
```

`time_elapsed(): u64` and `time_awake(): u64` read separate system-wide
monotonic clocks in nanoseconds. Their origins reset at every boot:

- `time_elapsed()` keeps advancing during system sleep, so differences between
  its readings measure real elapsed time across sleep and wake cycles.
- `time_awake()` pauses during system sleep and resumes when the system wakes.
  It advances whenever the system is awake, regardless of whether this or any
  other process is running, scheduled, or blocked. It is not process CPU time.

Both clocks are immune to calendar-clock changes. Each returned value is a
clock reading, not a duration by itself; subtract two readings from the same
operation to obtain elapsed nanoseconds. Cross-clock differences are
meaningless. Neither call allocates or can fail.

```ace
import sys

main(): int {
    start: u64 = sys.time_awake()
    sum: int = 0
    for i: int in 0 .. 1_000_000 {
        sum += i
    }
    elapsed_ns: u64 = sys.time_awake() - start
    if elapsed_ns == 0 {
        return 1
    }
    return 0
}
```

Caller-buffer bulk argument/environment filling, keyed environment lookup,
environment mutation, directory operations, calendar time, and CPU time are
not implemented yet.

## net

`net` provides a thin IPv4 TCP layer with allocation-free core socket
operations. Its public values and operations are
OS-neutral; `std/net/module.ace` selects the supported Darwin/Apple-arm64 OS
layer through `@when(target == "darwin-arm64")`. Platform structures, constants, byte order, and C
bindings stay in that private-by-convention file. `net.Socket` is a transparent
alias of `sys.Fd`, so close sockets with `sys.close`.

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
Parsing, formatting, IPv6, and local/peer address queries are not part of this
initial surface.

The core socket operations each perform one explicit operation and never
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
- `receive(socket, buffer: mut []u8): (int, Error)` performs one receive.
  Partial counts are returned as-is; zero with null error is orderly peer
  shutdown.
- `send(socket, []u8)` and `send(socket, string)` each perform one send and
  return partial counts as-is. A closed peer becomes an ordinary send error
  rather than terminating the process with `SIGPIPE`.
- `resolve4(host, port, addresses: mut []Address4): (usize, Error)` performs one blocking
  hostname lookup and writes IPv4 TCP endpoints into caller-owned storage in
  resolver order. The returned count is the number written; each address uses
  the supplied host-order port. Empty storage, invalid hostname bytes, lookup
  failure, or no IPv4 result returns zero with `ResolveErr`.

```ace
storage: [8]net.Address4
addresses: mut []net.Address4 = mut storage[0..8]
count: usize, err: Error = net.resolve4("example.com", u16(80), addresses)
if err == null && count != 0 {
    sock: net.Socket, socket_err: Error = net.socket(net.SocketKind.tcp)
    if socket_err == null {
        socket_err = net.connect(sock, addresses[0])
        sys.close(sock)
    }
}
```

The Darwin resolver temporarily allocates its private result list and frees it
before `resolve4` returns. This is a narrow resolver-boundary exception: no
resolver-owned pointer escapes and the application owns all returned storage.
The initial API does not indicate that additional results were truncated or
expose the platform resolver's detailed status. A native caller-buffer DNS
implementation, richer diagnostics, truncation discovery, IPv6, asynchronous
resolution, caching, and resolver-policy controls remain future improvements.

Every network error is last and null on success. The operation-specific public
domains are `SocketErr`, `SetReuseAddressErr`, `SetReusePortErr`,
`SetNonblockingErr`, `BindErr`, `ListenErr`, `AcceptErr`, `ConnectErr`,
`ReceiveErr`, `SendErr`, and `ResolveErr`. Compare `error.domain` with the
corresponding `typeid(net.*Err)`. For socket-operation errors, `error.code` is
the captured errno; resolver failures preserve the direct resolver status code.
Closing uses `sys.close` and therefore
retains `sys.CloseErr`.

No retry policy is hidden. In particular, interrupted, aborted, would-block,
and resource failures from `accept` remain caller-visible. Nonblocking
connection completion, half-close, additional TCP options, UDP, and
higher-level server/client convenience remain future additions.

## poll

`poll` provides allocation-free, level-triggered readiness for general
`sys.Fd` values. Sockets are one consumer; pipes, TTYs, and other supported
descriptors use the same queue without depending on `net`.

`queue` creates a level-triggered readiness queue. `watch` establishes or
replaces the queue's relationship with a descriptor, while `unwatch` reverses
that relationship. Neither operation closes the descriptor, and closing the
queue closes none of the descriptors it watches.

The paired spelling is deliberate stdlib practice: operations that establish
and reverse the same relationship use a recognizable lexical pair when an
accurate pair exists. Semantic accuracy takes priority over superficial
symmetry; unrelated operations are not named as a pair merely to make an API
look balanced.

```ace
import poll
import sys

main(): int {
    q: poll.Queue, err: Error = poll.queue()
    if err != null {
        return 1
    }
    fd: sys.Fd = sys.Fd(0)

    if poll.watch(q, fd, true, false) != null { // readable only
        sys.close(sys.Fd(q))
        return 2
    }

    zero: poll.Event = poll.Event {
        fd: sys.Fd(0), readable: false, writable: false,
        error: false, hangup: false,
    }
    storage: [16]poll.Event = [zero : 16]
    events: mut []poll.Event = mut storage[0..16]
    count: usize, wait_err: Error = poll.wait(q, events, poll.wait_forever)

    if wait_err == null {
        for index: usize in 0 .. count {
            if events[index].readable {
                // One receive attempt can make progress, or observe EOF.
            }
        }
    }

    poll.unwatch(q, fd)
    sys.close(sys.Fd(q))
    return 0
}
```

The readiness surface is:

- `Queue: distinct sys.Fd`; failed creation returns `Queue(-1)`. Close it with
  `sys.close(sys.Fd(q))`.
- `watch(q, fd, readable, writable): Error` registers a descriptor or replaces
  its complete readable/writable interest. At least one interest must be true.
  A failed update may leave partial kqueue state; call `watch` or `unwatch`
  again before relying on that registration.
- `unwatch(q, fd): Error` removes an existing registration. Removing an
  unwatched descriptor is an error.
- `wait(q, events: mut []Event, timeout_ns): (usize, Error)` writes a fully initialized
  prefix of caller-owned storage. The suffix remains unchanged. An empty slice
  is an error. The OS layer uses bounded stack storage, allocates nothing, and
  may return fewer events than the supplied capacity.

An `Event` contains `fd`, `readable`, `writable`, `error`, and `hangup`. At
most one returned event represents each fd; simultaneous conditions are
combined. Readable may mean that a read will report EOF rather than positive
bytes. Hangup may coexist with readable buffered data. Error supplies no
errno value and may coexist with every other condition. Error and hangup need
no registered interest.

A timeout of `0` polls without blocking. `poll.wait_forever` waits without a
timeout; every other value is a finite nanosecond duration. An interrupted
wait returns `WaitErr`; it is never restarted privately. A successful zero
count means that the timeout expired or a poll found no readiness.

The public domains are `QueueErr`, `WatchErr`, `UnwatchErr`, and `WaitErr`.
Compare `error.domain` with the corresponding `typeid(poll.*Err)` and inspect
`error.code` for the errno captured at failure. Closing a poll queue uses
`sys.close` and therefore retains `sys.CloseErr`.

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
