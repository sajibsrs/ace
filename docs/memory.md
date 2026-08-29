# Memory

Ace memory management is manual. Values use stack or static storage unless the
program explicitly calls the one process-wide allocation layer exported by
`mem`. There is no garbage collector, allocator value, ownership checker,
hidden allocation, or automatic cleanup.

## Size and length

`sizeof(T)` is a compile-time `usize`: the storage occupied by one `T`. Heap
allocation never changes it. For example, `sizeof([]u8)` is the size of the
two-word slice descriptor, not the number of bytes it views.

Intrinsic `.len` belongs only to sequences:

- `[N]T.len` is the constant element count `N`.
- `[]T.len` is the runtime element count stored in the slice.
- `string.len` is the runtime byte count stored in the string view.

An optional sequence must first be checked against null. Other types have no
intrinsic `.len`; a struct field with that name is an ordinary declared field.

## Raw allocation

The `mem` standard-library module exports:

```ace
mem.alloc(size: int, align: int): []u8?
mem.realloc(data: []u8, new_size: int): []u8?
mem.release(mut data: []u8?)
```

Import the module for qualified calls, or explicitly select the names:

```ace
import mem
// or: import alloc, realloc, release from mem
```

`mem.alloc` requires a positive byte size. On the current Apple arm64 target,
alignment must be a positive power of two no greater than 16. Invalid requests
and allocation failure return null; memory is uninitialized.

After narrowing a successful result, `.len` is exactly the requested byte size:

```ace
import mem

data: []u8? = mem.alloc(16, 8)
if data != null {
    bytes: usize = data.len
    data[0] = 42
    mem.release(mut data)
}
```

`mem.realloc` requires the complete non-null slice returned by `mem.alloc` or
the most recent successful `mem.realloc`, plus a positive new size. It may move
the storage.
Null means the request failed and the original allocation remains live and
unchanged. On success, only the returned slice is authoritative and its `.len`
is the new requested byte size.

`mem.release` requires a mutable `[]u8?` lvalue. Passing null is a no-op.
Otherwise it frees the storage and writes canonical null—zero pointer and zero
length—back to that lvalue. The `mut` mark makes the mutation visible:

```ace
mem.release(mut data)
```

## Slices alias; they do not own

Copying a slice copies its pointer and length, not its backing bytes:

```ace
import alloc, release from mem

a: []u8? = alloc(100, 8)
if a != null {
    b: []u8 = a
    release(mut a)
    // a is null; b is dangling and must not be used or released.
}
```

Ace performs no alias search, move analysis, or automatic nulling of copies.
Non-null means only that the stored pointer is nonzero; it does not prove that
the storage is still live. Independent bytes require a separate explicit
allocation and copy.

## Current diagnostics

The `mem` declarations are compiler-backed in the current bootstrap. Calls
lower directly to the platform `malloc`, `realloc`, and `free` boundary and
add no Ace allocation header or registry. This keeps the implemented path at
C-like allocation cost. Invalid release, double release,
use-after-free, and leaks are therefore not diagnosed by the current compiler;
they are programmer errors. Checked live-allocation tracking and stronger
memory-access instrumentation have not landed yet.

The examples on this page are checked by
`tests/sema/docs_memory.ok.ace`; executable allocation behavior is covered by
`tests/emit/allocation.ace`.
