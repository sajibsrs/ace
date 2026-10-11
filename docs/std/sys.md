# std.sys

[Standard library](index.md)

`sys` is the thin operating-system boundary: process-start values, file I/O,
secure entropy. It
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
  stack buffer of the target's `PATH_MAX` (1024 bytes on Darwin, 4096 on
  Linux) and NUL-terminated; longer paths fail with
  `ENAMETOOLONG` through the normal error path, exactly as the kernel would
  reject them. A path holding a NUL byte fails with `EINVAL` rather than
  opening the shorter path C would read.
- `read(fd: Fd, buf: mut []u8): (int, Error)` fills the caller's buffer and
  returns the byte count; `0` with a null error is end of file.
- `write(fd: Fd, data: []u8): (int, Error)` accepts `string`/`[]char` by shared
  identity and returns the byte count as the operating system reported it — partial writes
  are legitimate results and are never retried here.
- `close(fd: Fd): Error`.
- `ioctl(fd: Fd, request: uint): Error` issues a descriptor-control
  request with no additional argument. The overload
  `ioctl(fd: Fd, request: uint, data: mut []u8): Error` passes caller-owned,
  nonempty byte storage as the request argument. It performs no allocation,
  typed reinterpretation, request-size inference, or retry. These are raw
  target operations rather than an OS-independent device-control abstraction.
- `stat(path: string): (FileInfo, Error)` follows symbolic links and returns
  byte size plus a `FileType` of `unknown`, `regular`, `directory`, `symlink`,
  or `other`. `lstat(path: string): (FileInfo, Error)` is the same query
  without following a final symlink.
- `canonical(path: string, buf: mut []u8): (usize, Error)` writes the absolute
  path of an existing file, symlinks and `.`/`..` resolved, into `buf` (no NUL)
  and returns its length; `CanonicalErr.capacity` when `buf` is too small.
  `executable(buf: mut []u8): (usize, Error)` writes the running executable's
  canonical path the same way.
- `mkdir(path: string): Error` is the portable creation form. It requests all
  ordinary permissions (`0777` on POSIX), after which the process umask chooses
  the effective permissions. It creates one directory and does not create
  missing parents.
- `mkdir(path: string, mode: u32): Error` explicitly supplies POSIX
  creation permissions. This overload is target-specific rather than a claim
  that POSIX mode bits model Windows ACLs.
- `rename(old_path: string, new_path: string): Error` and
  `unlink(path: string): Error` directly perform the corresponding filesystem
  operation without recursion or retry.
- `errno(): i32` reads the thread's raw errno. Query it immediately after a
  failed call; any intervening system call may overwrite it.
- `random(storage: mut []u8): Error` fills caller-owned storage with
  cryptographically secure OS entropy and allocates nothing. Empty storage
  succeeds. `getentropy` requests are chunked to at most 256 bytes, retries
  `EINTR`, and returns other failures through `RandomErr`.

Directory traversal keeps its buffer and cursor in a caller-owned `Dir` value:

```ace
dir: sys.Dir, err: Error = sys.opendir("/tmp")
if err != null {
    return 1
}
name_storage: [256]u8
name: mut []u8 = mut name_storage[0..256]
done: bool = false
for !done {
    count: usize, file_type: sys.FileType, at_end: bool, read_err: Error = sys.readdir(mut dir, name)
    if read_err != null {
        sys.closedir(mut dir)
        return 2
    }
    done = at_end
    if !done {
        // The entry name is name[0..count]; no NUL byte is appended.
    }
}
if sys.closedir(mut dir) != null {
    return 3
}
return 0
```

`readdir` skips `.` and `..` and returns values in the fixed order name byte
count, file type, end flag, and `Error`. End is `(0, unknown, true, null)`. A file
type may be `unknown`; use `stat` when classification is required. If the name
buffer is too small, `ReaddirErr` reports `ERANGE`, leaves the entry pending,
and writes no truncated name. Enumeration retains operating-system order.

The call-site mark in `readdir(mut dir, name)` permits updating the caller's
iteration state. It is independent of the type-position `mut` in
`name: mut []u8`, which permits writing the supplied bytes. Likewise,
`closedir(mut dir)` closes the descriptor and replaces that caller's complete
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

`sys.fork(): (Process, Error)` duplicates the current process once. The parent
receives the child handle, the child receives `Process(0)`, and failure returns
`Process(-1)` with `ForkErr`. Because execution continues in both processes,
callers branch on the returned handle explicitly. `sys.wait(process)` blocks
for that exact positive child handle and returns
`ProcessStatus {exited, code, signal}`. Zero and negative handles fail with
`WaitErr`; they never acquire process-group or any-child meanings. Normal
exit sets `exited` and `code`; signal termination clears `exited` and sets
`signal`. Failure returns canonical-zero status with `WaitErr`. Neither
operation retries interruption, allocates, or closes inherited descriptors.

`sys.pipe(): (Fd, Fd, Error)` creates a unidirectional byte pipe and returns
the read end followed by the write end. Failure returns `Fd(-1)` for both with
`PipeErr`. The caller closes both descriptors explicitly.

`arg(0)` is the program invocation name. `arg(index)` requires
`index < argc()`, and `env(index)` requires `index < envc()`. These are the
ordinary preconditions of these compiler-backed calls; their debug-build
diagnostics have not landed in the sequence-bounds stage, so both modes
currently assume valid indexes. `argc()` is
constant-time; `envc()` scans the null-terminated environment pointer array on
every call, so capture it before iteration. Each selected entry is scanned for
its byte length only when requested. Argument views remain valid for the
process lifetime. Behavior after linked C code mutates the environment is not
specified yet. On linux-x64 an Ace `main` records argc and argv at entry, so an
Ace object linked under a C `main` sees `argc()` as zero; the environment
is read from glibc's `environ` at each call.

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

`EAGAIN` and `EWOULDBLOCK` are the target-defined errno values for retryable
nonblocking operations. They are equal on Darwin and Linux, but both standard spellings
are exported so code can name the condition described by its operation instead
of embedding a platform number.

Darwin (darwin-arm64 only) additionally exposes `BIOCGBLEN`, `BIOCPROMISC`,
`BIOCSETIF`, and `BIOCIMMEDIATE` for the BPF proof of concept, together with `EBUSY` and
`EACCES` so device probing can distinguish contention from access denial. The
request parameter is `uint`, matching Darwin's 64-bit `unsigned long`; callers
must not narrow it to `u32`. An ioctl failure uses `IoctlErr` and contains the
errno captured at the failing call.

Each failing operation returns an ordinary packed `Error` whose domain names
the operation — `sys.OpenErr`, `sys.ReadErr`, `sys.WriteErr`, `sys.CloseErr`,
`sys.StatErr`, `sys.MkdirErr`, `sys.RenameErr`, `sys.UnlinkErr`,
`sys.OpendirErr`, `sys.ReaddirErr`, `sys.ClosedirErr`, `sys.ForkErr`,
`sys.WaitErr`, `sys.PipeErr`, or `sys.IoctlErr` —
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

Caller-buffer bulk argument/environment filling, keyed environment lookup,
environment mutation and directory operations are not implemented yet.

## Darwin BPF packet-sniffer demo

The Apple-arm64 proof of concept opens the first available device from
`/dev/bpf0` through `/dev/bpf7`, binds it to `en0` by default, enables
promiscuous and immediate modes, queries the native capture-buffer size, and
prints the first complete IPv4 Ethernet frame. An optional first argument
selects another interface. It uses one fixed-capacity caller-owned buffer and
manually decodes bounded byte slices; it creates no typed view over packet or
BPF-header storage. Build and run it with:

```sh
./ace demo/darwin-packet-sniffer/main.ace -o bpf-sniffer
sudo ./bpf-sniffer
```

For example, capture from `en1` with `sudo ./bpf-sniffer en1`.
Pass `--debug` as the second argument to report completed BPF reads and skipped
or malformed records while diagnosing an interface, for example
`sudo ./bpf-sniffer en1 --debug`.

Live capture is a manual check because BPF access and traffic are machine
state. Automated fixtures instead verify the Darwin SDK layouts and request
values, the native 18-byte BPF header length (which excludes two bytes of C
structure tail padding), four-byte record alignment, truncated-input rejection,
and Ethernet/IPv4 decoding for TCP, UDP, and ICMP.

## Design rationale

Why the rules in `spec/std/sys.md` are shaped as they are.

- `sys` imports nothing, and keeps its own NUL-terminating path copy rather
  than using `str.cstring`: importing `str` made a `sys`-only program compile
  15 files instead of 3 (about 1.9 ms to 11.7 ms) for a six-line loop.

- `canonical` asks the kernel instead of walking components in Ace (Go
  `filepath.EvalSymlinks`) or calling libc `realpath`, which is a userspace
  algorithm C Interop #4 rules out: one or two syscalls, no race with a tree
  changing mid-walk, and no read permission needed. Zig's `realpath` takes the
  same route.

- Stdlib: `sys` and `time` #10: distinct-typed handles prevent silent integer
  misuse where the OS cannot diagnose it; flags and POSIX permission bits stay
  plain because the kernel validates their combinations; per-operation domains
  plus the errno query keep one fact in one place without bending the
  static-descriptor rule.
- Stdlib: `sys` and `time` #10: one allocation-free escape hatch is smaller and
  more honest than a premature portable device-control abstraction, while
  caller-owned bytes preserve explicit layout and make the existing bounded
  manual parsing baseline sufficient for BPF.
- Stdlib: `sys` and `time` #10: caller-owned batching gives one-entry iteration
  without libc allocation, truncation, or an OS-specific record surface;
  explicit `mut` makes cursor and invalidation changes visible; the deliberately
  small metadata value avoids freezing incidental Darwin ABI while leaving
  richer queries additive.
- Stdlib: `sys` and `time` #10: the networking proofs require a real concurrent
  peer but should contain no platform externs; distinct handles prevent
  accidental process/fd interchange, while one-operation wrappers preserve
  machine-visible control flow and keep policy with the caller.
- Stdlib: `sys` and `time` #10: indexed access exposes the existing process
  storage without manufacturing an incompatible `[]string` descriptor array,
  hidden allocation, runtime-sized stack storage, or an arbitrary fixed cap.
