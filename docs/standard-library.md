# Standard library

The standard library ships beside the compiler in `std/`. Bare imports
(`import fmt`) resolve only there — a local file can never shadow a standard
module. How imports resolve, aliasing, and visibility are described in
[Modules](module.md).

Today's modules:

- `fmt` — allocation-free text output
- `math` — minimal generic helpers
- `mem` — raw allocation and allocation-free byte operations
- `sys` — file descriptors, byte I/O, and the errno companion

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

Three overloads exist, selected exactly like any other overload set:

```ace
import fmt

main(): int {
    fmt.print("ready")           // prints: ready
    fmt.print(42)                // prints: 42
    fmt.print(usize(7))          // prints: 7
    return 0
}
```

- `print(text: string): int` — writes the bytes exactly.
- `print(value: int): int` — signed decimal, complete `int` range including
  `INT_MIN`, formatted through a fixed 20-byte stack buffer.
- `print(value: usize): int` — unsigned decimal for lengths, sizes, and
  indices, zero through `USIZE_MAX`.

Numeric output is ASCII with a `-` only for negative signed values. There is
no padding, no base selection, and no formatting language.

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

`sys` is the thin file-I/O boundary over the operating system. It never
allocates — only callers allocate — and every failure is an ordinary `Error`
value checked with a branch.

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

Time and other operating-system surfaces are not implemented yet.

## Current limitations

- No `fmt.println` or formatted printing yet; end lines by printing your own
  bytes.
- No output overloads for `uint`, floats, or narrower integer widths; an
  explicit cast selects between the existing `int` and `usize` overloads.
- `math` has no `sqrt`, floats, or libm binding.
- There is no user-configurable search path for `std/`.
- `sys` has no time, directory, or socket surface, and no `unlink`; tests
  that create files leave them behind.
- Extern C variadics declared with `...` are not callable with the stack
  argument convention Apple arm64 requires, so `sys` binds fixed-arity
  exports like `__open` instead.
