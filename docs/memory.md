# Memory

Ace memory management is manual. Values use stack or static storage unless the
program explicitly calls the one process-wide allocation layer exported by
`mem`. There is no garbage collector, allocator value, ownership checker,
hidden allocation, or automatic cleanup.

## Size and length

`sizeof(T)` is a compile-time `usize`: the storage occupied by one `T`. Heap
allocation never changes it. For example, `sizeof([]u8)` is the size of the
two-word slice descriptor, not the number of bytes it views.

The operand uses the full type grammar, so inline sequence types need no named
alias: `sizeof([]u8)` and `sizeof([]char)` are 16, `sizeof([3]char)` is 3, and
`sizeof(char)` is 1 (`char` is the transparent `u8` alias).

Intrinsic `.len` belongs only to sequences:

- `[N]T.len` is the constant element count `N`.
- `[]T.len` is the runtime element count stored in the slice.
- `string.len` is the runtime byte count stored in the string view.

A zero slice or string has length zero, so querying `.len` is valid. An empty
view created by slicing (`data[5..5]`) is an ordinary value: it keeps its
computed pointer and is never canonicalized to null. Comparing it with `null`
is a nullness test on the complete descriptor, so it is false; comparing it
with another empty slice is a content comparison, so it is true. Other types
have no intrinsic `.len`; a struct field with that name is an ordinary declared
field.

The layout facts are normative — code may rely on them, including inline-asm
bindings and descriptor inspection through `&`. A slice or string value is
exactly two words: a data pointer at offset 0 and a count at offset 8, so
`sizeof([]T)` and `sizeof(string)` are 16 with alignment 8. There is no capacity
word and no hidden field. An array `[N]T` is `N` contiguous elements in index
order with no metadata and no inter-element padding: `sizeof([N]T)` is
`N * sizeof(T)` with the element's alignment. Strings are never NUL-terminated,
and a string is a read-only view — its bytes cannot be written through it.

## Raw allocation

The `mem` standard-library module exports:

```ace
mem.alloc(size: int, align: int): []u8
mem.realloc(data: []u8, new_size: int): []u8
mem.release(mut data: []u8)
```

Import the module for qualified calls, or explicitly select the names:

```ace
import mem
// or: import alloc, realloc, release from mem
```

`mem.alloc` requires a positive byte size. On the current Apple arm64 target,
alignment must be a positive power of two no greater than 16. Invalid requests
and allocation failure return null; memory is uninitialized.

After a successful non-null check, `.len` is exactly the requested byte size:

```ace
import mem

data: []u8 = mem.alloc(16, 8)
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

`mem.release` requires a mutable `[]u8` lvalue. Passing null is a no-op.
Otherwise it frees the storage and writes canonical null—zero pointer and zero
length—back to that lvalue. The `mut` mark makes the mutation visible:

```ace
mem.release(mut data)
```

## Typed allocation

The same module exports explicitly instantiated generic overloads that return
typed slices, so values no longer need manual byte packing:

```ace
mem.alloc[T](count: usize): []T
mem.realloc[T](data: []T, new_count: usize): []T
mem.release[T](mut data: []T)
```

The type argument is always spelled out — generic instantiation stays
explicit — and deallocation reads `mem.release[T](mut data)`, not
`mem.release(mut data)`:

```ace
import mem

COUNT: const usize = 20

fibonacci(count: usize): []usize {
    values: []usize = mem.alloc[usize](count)

    if values == null {
        return null
    }

    previous: usize = 0
    current: usize = 1
    for index: usize in 0 .. count {
        values[index] = current
        previous, current = current, previous + current
    }
    return values
}

main(): int {
    numbers: []usize = fibonacci(COUNT)

    if numbers == null {
        return 1
    }

    mem.release[usize](mut numbers)
    return 0
}
```

`mem.alloc[T](count)` requests storage for exactly `count` contiguous `T`
elements. The byte size is `count * sizeof(T)` and the alignment is selected
automatically from `T`; the caller never supplies either. The returned slice
holds the allocation pointer and `count` as its element length — no capacity,
ownership marker, or other metadata is added. Storage uses the target's
native representation, alignment, and byte order: this is typed native
memory, not serialization.

The storage is uninitialized, exactly like the raw allocation. No value
initialization, zeroing, constructor, or hidden loop occurs. Establishing a
valid `T` in each element is the programmer's job under the ordinary
definite-initialization model; the compiler adds no ownership, lifetime, or
alias analysis, and reading an element before a valid `T` was established is
programmer error.

A multiplication overflow, an unsupported type alignment, a zero byte-size
request (`sizeof(T) == 0` or `count == 0`), or allocation failure returns the
canonical null `[]T`; nothing aborts. An overflowing count is rejected before
the platform allocator is called.

`mem.realloc[T]` requires the complete authoritative non-null typed slice
returned by typed allocation or a previous successful typed reallocation of
the same exact `T`. It may move the storage. On success the result is the
only authoritative slice and its length is `new_count`; on overflow, invalid
size, or failure it returns null while the original allocation remains live
and unchanged. Newly added elements are not initialized and removed elements
are not destroyed.

`mem.release[T](mut data)` releases the complete authoritative typed
allocation. The `[T]` instantiation must match the argument's `[]T` element
type exactly. A null typed slice is a no-op; otherwise the passed caller
variable is set to canonical zero—zero pointer and zero length—and no element
destruction or hidden iteration occurs.

Typed slices alias exactly like raw ones: copying copies the descriptor, a
`release` nulls only its argument, and copied slices become dangling aliases
that must not be used or released.

The raw byte forms and the typed overloads are one overload set per name:
`mem.alloc(16, 8)` still returns `[]u8` unchanged, and there is no implicit
conversion between `[]u8` and `[]T` — reinterpretation is deliberately not
part of this surface.

## Byte operations

The same module exports allocation-free operations over byte-slice views:

```ace
mem.copy(noalias mut dst: []u8, src: []u8)
mem.move(mut dst: []u8, src: []u8)
mem.fill(mut data: []u8, value: u8)
mem.zero(mut data: []u8)
mem.compare(a: []u8, b: []u8): int
```

`copy` and `move` require equal slice lengths and copy exactly the complete
source view. They do not resize or otherwise change either slice descriptor.
Slice larger storage to the exact destination range before calling:

```ace
import mem

source_array: [3]u8 = [10, 20, 30]
target_array: [8]u8 = [0, 0, 0, 0, 0, 0, 0, 0]
source: []u8 = source_array[0..3]
target: []u8 = target_array[0..3]
mem.copy(mut target, source)
```

`copy` requires the complete source and destination ranges not to overlap.
Its `noalias` destination states that optimizer fact in the declaration; it is
not repeated at the call site. Use `move` whenever the ranges might overlap.
`move` preserves the source bytes correctly in either overlap direction.

`fill` writes `value` to every byte in its slice; `zero` writes zero to every
byte. All four write operations return nothing. Empty slices are valid.

`compare` reads complete slices as unsigned bytes and returns exactly `-1`,
`0`, or `1`. It is lexicographic: the first differing byte decides; when the
common prefix is equal, the shorter slice sorts first. Unlike copy and move,
the compared slices need not have equal lengths.

No byte operation allocates. An unequal-length copy/move or overlapping copy
is a programmer error. Checked builds are specified to diagnose these contract
violations; the current bootstrap has no checked/release mode yet and runs the
release fast path without those runtime checks.

## Slices alias; they do not own

Copying a slice copies its pointer and length, not its backing bytes:

```ace
import alloc, release from mem

a: []u8 = alloc(100, 8)
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
`tests/sema/docs/memory_ok.ace`; executable allocation and byte-operation
behavior is covered by `tests/emit/allocation/main.ace`,
`tests/emit/allocation/typed.ace`, and `tests/emit/allocation/bytes.ace`. For manual
performance and process-memory measurement on Apple arm64, run
`./bench/run-memory.sh`; it reports allocator timings and usable bytes,
compiler statistics, peak resident size, and related macOS process counters
without turning variable benchmark numbers into test thresholds.
