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
- It allocates because an argument list has no useful fixed bound (Darwin
  `ARG_MAX` is 1 MiB) and caller storage would push C's pointer-array layout
  onto every caller. That allocation is why it is not in `sys`: `sys` imports
  nothing, and nearly every program compiles it.
