# std.sys

[Standard library](index.md)

`sys` is the thin operating-system boundary: process-start values, file I/O,
secure entropy, and monotonic time. It
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
- `write(fd: Fd, data: []u8): (int, Error)` accepts `string`/`[]char` by shared
  identity and returns the byte count as the operating system reported it — partial writes
  are legitimate results and are never retried here.
- `close(fd: Fd): Error`.
- `ioctl(fd: Fd, request: uint): Error` issues a Darwin descriptor-control
  request with no additional argument. The overload
  `ioctl(fd: Fd, request: uint, data: mut []u8): Error` passes caller-owned,
  nonempty byte storage as the request argument. It performs no allocation,
  typed reinterpretation, request-size inference, or retry. These are raw
  target operations rather than an OS-independent device-control abstraction.
- `stat(path: string): (FileInfo, Error)` follows symbolic links and returns
  byte size plus a `FileType` of `unknown`, `regular`, `directory`, `symlink`,
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
- `random(storage: mut []u8): Error` fills caller-owned storage with
  cryptographically secure OS entropy and allocates nothing. Empty storage
  succeeds. Darwin chunks `getentropy` requests to at most 256 bytes, retries
  `EINTR`, and returns other failures through `RandomErr`.

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
    count: usize, file_type: sys.FileType, at_end: bool, read_err: Error = sys.read_dir(mut dir, name)
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

`read_dir` skips `.` and `..` and returns values in the fixed order name byte
count, file type, end flag, and `Error`. End is `(0, unknown, true, null)`. A file
type may be `unknown`; use `stat` when classification is required. If the name
buffer is too small, `ReadDirErr` reports `ERANGE`, leaves the entry pending,
and writes no truncated name. Enumeration retains operating-system order.

The call-site mark in `read_dir(mut dir, name)` permits updating the caller's
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
ordinary preconditions of these compiler-backed calls; their checked
diagnostics have not landed in the sequence-bounds stage, so both modes
currently assume valid indexes. `argc()` is
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

`EAGAIN` and `EWOULDBLOCK` are the target-defined errno values for retryable
nonblocking operations. They are equal on Darwin, but both standard spellings
are exported so code can name the condition described by its operation instead
of embedding a platform number.

Darwin additionally exposes `BIOCGBLEN`, `BIOCPROMISC`, `BIOCSETIF`, and
`BIOCIMMEDIATE` for the BPF proof of concept, together with `EBUSY` and
`EACCES` so device probing can distinguish contention from access denial. The
request parameter is `uint`, matching Darwin's 64-bit `unsigned long`; callers
must not narrow it to `u32`. An ioctl failure uses `IoctlErr` and contains the
errno captured at the failing call.

Each failing operation returns an ordinary packed `Error` whose domain names
the operation — `sys.OpenErr`, `sys.ReadErr`, `sys.WriteErr`, `sys.CloseErr`,
`sys.StatErr`, `sys.MkdirErr`, `sys.RenameErr`, `sys.UnlinkErr`,
`sys.OpenDirErr`, `sys.ReadDirErr`, `sys.CloseDirErr`, `sys.ForkErr`,
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

