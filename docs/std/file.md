# std.file

[Standard library](index.md)

`file` reads and writes whole files. It is pure Ace over `sys` descriptor
calls and `mem`, and it is a module of its own so that importing `sys` never
links the allocator.

```ace
data: []u8, err: Error = file.read("notes.txt")
defer mem.release[u8](&data)
if err != null { return 1 }
err = file.write("copy.txt", data)
```

- `read(path: string): ([]u8, Error)` returns the whole file in one
  allocation; release it with `mem.release[u8](&data)`. On failure the slice
  is null, and an empty file is a null slice with no error, so a `defer`red
  release placed before the error check is always safe. The buffer is sized
  from `sys.stat` once: a file that grows meanwhile yields its leading bytes.
- `write(path: string, data: []u8): Error` replaces the file's contents with
  `data`. A new file is created with `0666` less the process umask (`0644`
  under the usual `022`), the same as C's `fopen`. A failed close is reported,
  since it can carry a deferred write error.

Both retry interrupted reads and writes. Errors from `sys.open`, `sys.stat`,
`sys.read`, `sys.write`, and `sys.close` pass through unchanged; `read` adds
its own `FileErr`:

| `FileErr`    | Meaning                          |
| ------------ | -------------------------------- |
| `allocation` | the buffer could not be allocated |
| `truncated`  | the file shrank while being read |

## Design rationale

Why the rules in `spec/std/collections.md` are shaped as they are.

- Stdlib: `array`, `map`, and `file` #20: reading or writing a whole file is the
  common case and composes existing one-to-one operations; the allocation stays
  visible in the returned slice, released by the ordinary `mem.release` rather
  than a wrapper type. A module of its own keeps `sys` allocation-free, since
  every imported function is emitted and linked.
