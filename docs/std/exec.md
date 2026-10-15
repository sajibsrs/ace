# std.exec

[Standard library](index.md)

`exec` starts programs as child processes. It is pure Ace over `sys` and
`mem` plus one `posix_spawnp` call per target, and it is a module of its own so
that importing `sys` never loads the allocator.

```ace
child: sys.Process, err: Error = exec.spawn(["cc", "main.o", "-o", "main"])
if err != null { return 1 }
status: sys.ProcessStatus, werr: Error = sys.wait(child)
```

- `spawn(argv: []string): (sys.Process, Error)` starts `argv[0]`, searched in
  `PATH`, with the arguments `argv` and this process's environment. No shell
  runs in between, so arguments need no quoting. Wait for the child with
  `sys.wait`, as for a forked one. Failure returns `sys.Process(-1)`.
- `spawn(argv: []string, options: SpawnOptions): (sys.Process, Error)` adds a
  working directory and redirection. `SpawnOptions` has `dir: string` (empty
  keeps this process's directory), and, for each of `stdout` and `stderr`, a
  `Redirect` (`inherit` or `fd`) with the descriptor in `stdout_fd` /
  `stderr_fd`, read only for `fd`. Canonical zero changes nothing, so
  `blank(SpawnOptions)` is the all-inherit value. The same descriptor on both
  streams is `2>&1`. The caller keeps its descriptors and closes them.

```ace
fd: sys.Fd, err: Error = sys.open("out.txt", sys.O_WRONLY | sys.O_CREAT, sys.S_IRUSR | sys.S_IWUSR)
options: exec.SpawnOptions = blank(exec.SpawnOptions)
options.dir = "build"
options.stdout = exec.Redirect.fd
options.stdout_fd = fd
options.stderr = exec.Redirect.fd
options.stderr_fd = fd
child: sys.Process, serr: Error = exec.spawn(["make"], options)
```

| `SpawnErr`  | Meaning                                        |
|-------------|------------------------------------------------|
| `not_found` | program not found                              |
| `access`    | permission denied                              |
| `format`    | program is not executable                      |
| `too_big`   | argument list too long                         |
| `memory`    | insufficient memory to create process          |
| `retry`     | temporary process resource limit reached       |
| `invalid`   | empty `argv`, or an argument holding a NUL byte |

## Design rationale

- `spawn` wraps `posix_spawnp` rather than `fork` plus `exec`, as Go
  `os.StartProcess` and Zig `std.process.Child` do: one call, no duplicated
  address space, and exec failures reported to the parent.
- `Redirect` is an enum with `inherit` as its zero value, as Zig
  `std.process.Child` (`stdout_behavior`) and Rust `Stdio` do; Python's
  negative sentinels and Go's `nil` (the null device, not inherit) leave a
  zeroed value wrong. `null` and `pipe` can be added as further enumerators.
- Directory and redirection are `posix_spawn` file actions
  (`addchdir_np`, `adddup2`), so no shell and no `fork` window are involved.
  `sys.open` and `sys.pipe` mark descriptors close-on-exec so a child holds
  only the descriptors it is given; otherwise a second child would inherit the
  first's pipe write end and its reader would never see end of input.
- It allocates because an argument list has no useful fixed bound (Darwin
  `ARG_MAX` is 1 MiB) and caller storage would push C's pointer-array layout
  onto every caller. That allocation is why it is not in `sys`: `sys` imports
  nothing, and nearly every program compiles it.
