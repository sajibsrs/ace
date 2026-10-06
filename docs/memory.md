# Memory

Ace memory management is manual. Values use stack or static storage unless the
program explicitly calls the one process-wide allocation layer exported by
`mem`. There is no garbage collector, allocator value, ownership checker,
hidden allocation, or automatic cleanup.

## Size, alignment, and length

`sizeof(T)` is a compile-time `usize`: the storage occupied by one `T`. Heap
allocation never changes it. For example, `sizeof([]u8)` is the size of the
two-word slice descriptor, not the number of bytes it views.

`alignof(T)` is the compile-time `usize` alignment required by one `T`, in
bytes. Raw allocation can therefore use the compiler's target-native layout
instead of repeating it manually:

```ace
storage: mut []u8 = mem.alloc(int(sizeof(Point)), int(alignof(Point)))
```

Both operands use the full type grammar, so inline sequence types need no named
alias: `sizeof([]u8)` and `sizeof([]char)` are 16, `sizeof([3]char)` is 3,
`alignof([]u8)` is 8, and `alignof([3]char)` is 1. `char` is the transparent
`u8` alias, so its size and alignment are both 1.

`offsetof(T, field.path)` returns the compile-time byte offset of a direct or
nested inline-struct field. `static_assert(condition)` is valid at module or
function scope, emits no code, and rejects false or non-constant conditions:

```ace
static_assert(offsetof(Packet, header.length) == 8)
```

## Explicit aggregate layout

Struct fields normally retain declaration order with natural alignment and
padding. `@packed` changes each unannotated field's effective alignment to one;
`@align(N)` raises the alignment of a struct or one field. Attributes occupy
their own lines:

```ace
@packed
Packet: struct {
    packet_type: u8,

    @align(4)
    length: u32,
    tail: u16,
}

static_assert(offsetof(Packet, length) == 4)
static_assert(offsetof(Packet, tail) == 8)
static_assert(sizeof(Packet) == 12)
static_assert(alignof(Packet) == 4)
```

Outside a packed struct, field alignment cannot be reduced below the field
type's natural alignment. Within a packed struct, an explicit field alignment
raises only that field's packed baseline; the next unannotated field still has
alignment one. A struct-level alignment cannot reduce the alignment computed
from its fields. Alignment expressions are compile-time `usize` values and the
current target accepts positive powers of two through 16.

Nested values keep their internal layouts. Packing changes where a nested value
starts, not the offsets inside it. Struct size includes any trailing padding
needed for its alignment and therefore supplies the correct stride in arrays.
Use explicit byte-array fields for reserved bytes. Layout attributes do not
specify byte order, bit fields, access observability, or synchronization.

Packed and explicitly aligned structs may cross a C boundary behind a pointer
when their complete shape is otherwise C-compatible and matches the external
declaration. Passing or returning such a struct directly is currently rejected
until its target-specific by-value ABI has been separately verified.

`@offset(N)` places a field at exactly byte `N`, which must be a multiple of the
field's alignment; the fields after it follow it as usual. Fields placed so
that they share bytes make an overlapping struct, which is how a C union is
written:

```ace
Event: struct {
    @offset(0)
    kind: u32,

    @offset(0)
    key: KeyboardEvent,

    @offset(0)
    padding: [128]u8,
}

event: Event
if poll(mut &event) && event.kind == KEY_DOWN {
    handle(event.key.code)
}
```

Every field of an overlapping struct must be plain bits (below), and none may
have a default. Reading a field reads its bytes; writing one writes only its
bytes. There is no literal and no `==`. Any member write counts as
initializing the whole struct. Like other explicit layouts, it crosses a C
boundary only behind a pointer. On a field, `@offset` comes before `@align`.

## Untyped storage

Memory has no dynamic type. Reading storage as `T` reads its `sizeof(T)` bytes
as `T`, and the compiler never assumes two accesses are disjoint just because
their types differ; only `noalias` and read-only parameters state non-overlap.

Plain-bits types are those where every byte pattern is a value: integers,
`char`, floating types, enums, bits types, data pointers, C function values, and structs,
arrays, and distinct types built only from them. `bool` counts too, trusted to
hold `0` or `1`. Tagged unions, `Error`, `typeid`, contracts, Ace function
values, slices, strings, and tuples are created only by the language and are
never produced by reinterpreting storage.

`bitcast(U, value)` copies a value's bits as another plain-bits type of the
same size, in native byte order:

```ace
raw: u32 = bitcast(u32, value)             // value: f32
color: Rgba = bitcast(Rgba, u32(0x04030201)) // Rgba: four u8 fields
```

The value must be typed (`bitcast(u64, f64(1.0))`, not `bitcast(u64, 1.0)`),
and its type must have no padding bytes; use `@packed` or explicit reserved
fields where a layout has gaps.

## Volatile access

`volatile` marks one access, not a variable or type:

```ace
status: u32 = volatile device.status
volatile device.control = command
volatile *register_pointer = value
```

The complete addressing expression and, for a write, the assigned expression
are each evaluated once. The final load or store occurs exactly once at the
width of its scalar type. A volatile `u8` packed field therefore performs one
byte access; the compiler does not load a containing word and mask it. Packing
and alignment affect the address but never the access width.

The initial eligible types are integers, floating-point values, pointers,
explicitly backed enums, and distinct forms of those types. Complete arrays,
structs, tuples, tagged unions, default enums, slices, strings, contracts,
function values, `Error`, and `typeid` are not volatile-access shapes. A tagged
union's switch binder remains an ordinary local payload copy rather than an
addressable view into the union.

Ordinary permissions still apply: `volatile` cannot write through a read-only
pointer or view. It guarantees an externally observable access but provides no
atomicity, thread synchronization, memory barrier, or hardware-specific
ordering. The programmer must also use an address and alignment accepted by the
target device.

The direct read-only property `sequence.len` belongs only to built-in sequences:

- `array.len` for `[N]T` is the constant element count `N`.
- `slice.len` for `[]T` is the runtime element count stored in the slice.
- `text.len` for `string` is the runtime byte count stored in the string view.

A zero slice or string has length zero, so reading `.len` is valid. An empty
view created by slicing (`data[5..5]`) is an ordinary value: it keeps its
computed pointer and is never canonicalized to null. Comparing it with `null`
is a nullness test on the complete descriptor, so it is false; comparing it
with another empty slice is a content comparison, so it is true. Other types
have no built-in `.len`; a struct field with that name is an ordinary declared
field. The former `len(value)` call spelling is not a form.

The layout facts are normative — code may rely on them, including inline-asm
bindings and descriptor inspection through `&`. A slice or string value is
exactly two words: a data pointer at offset 0 and a count at offset 8, so
`sizeof([]T)` and `sizeof(string)` are 16 with alignment 8. There is no capacity
word and no hidden field. An array `[N]T` is `N` contiguous elements in index
order with no metadata and no inter-element padding: `sizeof([N]T)` is
`N * sizeof(T)` with the element's alignment. Strings are never NUL-terminated,
and a string is a read-only view — its bytes cannot be written through it.

## Pointer and slice construction

`usize(pointer)` extracts the exact machine address from any data pointer.
`*T(address)` and `mut *T(address)` convert an exact `usize` address back to the
written pointer type; the writable form is an explicit assertion of write
authority. Debug output diagnoses dynamic nonzero misalignment, while a
locally known misaligned address is always a compile error.

The same spelling reinterprets another data pointer, viewing the same address
as a different plain-bits type:

```ace
event: Event
key: mut *KeyboardEvent = mut *KeyboardEvent(mut &event)
bits: u32 = *(*u32(&value))                // value: f32
```

Both pointee types must be plain bits, and permission never widens: a writable
view needs a writable source. A view no more aligned than its source costs
nothing; a more-aligned one gets the same debug alignment check as an address.

An existing extent can be made into a slice without allocation:

```ace
plain: []Packet = []Packet(pointer, count)
writable: mut []Packet = mut []Packet(writable_pointer, count)
```

The pointer element type must match exactly and `count` is exactly `usize`.
The plain form may shrink pointer permission; the writable form requires a
writable pointer. A null pointer is valid only with count zero. Debug output
also diagnoses dynamic byte-extent and end-address overflow; release output
assumes these local preconditions and emits only the descriptor construction.
A non-null pointer with count zero remains a non-null empty view.

This operation asserts a view extent only. It does not validate mapping,
lifetime, allocation membership, or actual accessibility, and it does not make
the slice authoritative for `mem.realloc` or `mem.release`. No reverse
possibly-empty-slice-to-pointer conversion is provided.

## Raw allocation

The `mem` standard-library module exports:

```ace
mem.alloc(size: int, align: int): mut []u8
mem.realloc(data: []u8, new_size: int): mut []u8
mem.release(mut data: []u8)
```

> Allocation returns a writable slice descriptor, not a bare pointer. The
> descriptor contains the allocation address and requested byte length. Calling
> it a pointer hides an essential part of the API's semantics. Copying or
> returning the slice copies only this descriptor, not the allocated bytes.

Import the module for qualified calls, or explicitly select the names:

```ace
import mem
// or: import alloc, realloc, release from mem
```

`mem.alloc` requires a positive byte size. On the current Apple arm64 target,
alignment must be a positive power of two no greater than 16. Invalid requests
and allocation failure return null; memory is uninitialized.

After a successful non-null check, `data.len` is exactly the requested byte size:

```ace
import mem

data: mut []u8 = mem.alloc(16, 8)
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
unchanged. On success, only the returned slice is authoritative, and `.len` of
that slice is the new requested byte size.

`mem.release` requires a mutable slice binding; its content type may be `[]u8`
or `mut []u8`. Passing null is a no-op. Otherwise it frees the storage and
writes canonical null—zero pointer and zero length—back to that lvalue. The
call-site `mut` mark makes the descriptor mutation visible:

```ace
mem.release(mut data)
```

`mem.usable(data: []u8): usize` reports the usable size of the live
allocation whose base pointer `data` holds. It is at least the requested
length and allocator-defined beyond it, so it exposes allocator overhead. Null
slices, pointers the allocator does not own, and interior pointers report 0.
The argument is read-only and takes no call-site `mut`.

```ace
data: mut []u8 = mem.alloc(100, 16)
if data != null {
    usable: usize = mem.usable(data) // >= 100
    mem.release(mut data)
}
```

## Typed allocation

The same module exports explicitly instantiated generic overloads that return
typed slices, so values no longer need manual byte packing:

```ace
mem.alloc[T](count: usize): mut []T
mem.zalloc[T](count: usize): mut []T
mem.realloc[T](data: []T, new_count: usize): mut []T
mem.release[T](mut data: []T)
```

The type argument is always spelled out — generic instantiation stays
explicit — and deallocation reads `mem.release[T](mut data)`, not
`mem.release(mut data)`:

```ace
import mem

COUNT: const usize = 20

fibonacci(count: usize): []usize {
    values: mut []usize = mem.alloc[usize](count)

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

`mem.zalloc[T](count)` is the zero-filling form. It follows every other rule of
`mem.alloc[T]`, but each byte of the storage is zero, which is a valid `T` for
every type, so a container needs no initialization loop:

```ace
slots: mut []Slot = mem.zalloc[Slot](1024) // every Slot is all zero
defer mem.release[Slot](mut slots)
```

A multiplication overflow, an unsupported type alignment, a zero byte-size
request (`sizeof(T) == 0` or `count == 0`), or allocation failure returns the
canonical null `mut []T`; nothing aborts. It has the same representation as a
plain null slice and may shrink implicitly. An overflowing count is rejected
before the platform allocator is called.

`mem.realloc[T]` requires the complete authoritative non-null typed slice
returned by typed allocation or a previous successful typed reallocation of
the same exact `T`. It may move the storage. On success the result is the
only authoritative writable slice and its length is `new_count`; on overflow, invalid
size, or failure it returns null while the original allocation remains live
and unchanged. Newly added elements are not initialized and removed elements
are not destroyed.

`mem.release[T](mut data)` releases the complete authoritative typed
allocation. The `[T]` instantiation must match the argument's `[]T` element
type exactly; its content permission may be plain or writable. A null typed
slice is a no-op; otherwise the passed caller variable is set to canonical
zero—zero pointer and zero length—and no element destruction or hidden
iteration occurs.

Typed slices alias exactly like raw ones: copying copies the descriptor, a
`release` nulls only its argument, and copied slices become dangling aliases
that must not be used or released.

The raw byte forms and the typed overloads are one overload set per name:
`mem.alloc(16, 8)` returns `mut []u8`, and there is no implicit
conversion between `[]u8` and `[]T` — reinterpretation is deliberately not
part of this surface.

Allocation and successful reallocation are writability origins: their fresh
storage is returned as `mut []T` and can be written directly. Reallocation and
release still accept plain `[]T` content permission because they consume the
authoritative descriptor rather than writing elements. Release's call-site
`mut` is the independent permission to replace the caller's descriptor with
null, so a handle deliberately shrunk to `[]T` remains releasable.

## Byte operations

The same module exports allocation-free operations over byte-slice views.
Write destinations carry type-position `mut` — their argument must be a
`mut []u8` writable view, with no call-site mark:

```ace
mem.copy(noalias dst: mut []u8, src: []u8)
mem.move(dst: mut []u8, src: []u8)
mem.fill(data: mut []u8, value: u8)
mem.zero(data: mut []u8)
mem.compare(a: []u8, b: []u8): int
mem.equal(a, b): bool
```

`copy` and `move` require equal slice lengths and copy exactly the complete
source view. Their typed overloads take any element type, compare element
counts, and copy `len * sizeof(T)` bytes through the same routines and debug
checks:

```ace
mem.copy[T](noalias dst: mut []T, src: []T)
mem.move[T](dst: mut []T, src: []T)
```

Like `release[T]`, they state `T` explicitly; `mem.copy(a, b)` on `[]u32`
views is a type error, since `[]u32` does not convert to `[]u8`. They do not resize or otherwise change either slice descriptor.
`equal` accepts every pair of `[N]u8`, `[]u8`, and `string` values, compares
length and exact bytes, and allocates nothing. It is the explicit content test;
sequential values do not support `==` or `!=` against one another.
Slice larger storage to the exact destination range before calling, and
establish the writable view where `mut` is written:

```ace
import mem

source_array: [3]u8 = [10, 20, 30]
target_array: [8]u8 = [0, 0, 0, 0, 0, 0, 0, 0]
source: []u8 = source_array[0..3]
target: mut []u8 = mut target_array[0..3]
mem.copy(target, source)
```

`copy` requires the complete source and destination ranges not to overlap.
Its `noalias` destination states that optimizer fact in the declaration; it is
not repeated at the call site. Use `move` whenever the ranges might overlap.
`move` preserves the source bytes correctly in either overlap direction.

`fill` writes `value` to every byte in its slice; `zero` writes zero to every
byte. All four write operations return nothing. Empty slices are valid.

The compiler emits these operations natively rather than calling platform
memory routines. `copy`, `move`, `fill`, and `zero` call one private routine
per object file that selects a size class: up to 64 bytes, it covers the range
with two overlapping chunks after loading every source byte, so the same code
serves overlapping moves; larger ranges run 32-byte loops from a 32-byte-aligned
destination, with the far-end chunk loaded before the loop starts. `move`
walks backward only when the destination begins inside the source.
`compare` and `equal` share one ordering routine: it scans 16- or 32-byte
chunks for a difference, then orders the first differing byte by comparing
byte-reversed 64-bit words, so `equal` costs no more than a scan. On Apple
arm64 these routines use NEON registers internally; this is instruction
selection only and adds no vector facility to the language. The compiler
reuses both routines internally: aggregate values larger than 32 bytes
(assignments, arguments, returns, sequence elements, tuple and pattern
binders) copy through the copy routine, and `StaticMap` string-key checks
call the ordering routine; smaller aggregates copy inline.

`compare` reads complete slices as unsigned bytes and returns exactly `-1`,
`0`, or `1`. It is lexicographic: the first differing byte decides; when the
common prefix is equal, the shorter slice sorts first. Unlike copy and move,
the compared slices need not have equal lengths.

No byte operation allocates. An unequal-length copy/move or overlapping copy
is a programmer error. Debug builds diagnose these contracts at the call
boundary; release builds emit the direct byte operation without validation.
Debug-build allocation also tracks exact live base/extent pairs, diagnoses invalid
or repeated release/reallocation, and reports allocations still live when
`main` returns normally. The registry and its metadata are absent in release.

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

## Views of local storage

A local array, a plain parameter, and a `[]T` array literal with runtime
elements live in the function's frame. Returning a view of one, or storing it
in a module variable or `mut` parameter, is a compile error:

```ace
leak(x: int): []int {
    a: [2]int = [x, x]
    return a[..] // error: returned value views storage of this function
}
```

The check reads only the returned or stored expression itself, through tuples,
aggregate literals, and view conversions. A view first bound to a variable
(`s: []int = a[..]; return s`) or passed through a call is not tracked. A
constant `[]T` literal is static data and may be returned. To return elements,
return the array by value or copy them into caller-supplied or allocated
storage.

## Current diagnostics

The `mem` declarations are compiler-backed in the current bootstrap. Release
calls lower directly to the platform `malloc`, `realloc`, and `free` boundary
and add no Ace allocation header or registry. Debug builds maintain a live
allocation registry that diagnoses invalid or repeated release/reallocation and
reports allocations still live when `main` returns normally. It does not detect
arbitrary access through a dangling alias.

The examples on this page are checked by
`tests/sema/docs/memory_ok.ace`; executable allocation and byte-operation
behavior is covered by `tests/emit/allocation/main.ace`,
`tests/emit/allocation/typed.ace`, and `tests/emit/allocation/bytes.ace`. For manual
performance and process-memory measurement on Apple arm64, run
`./bench/std/run-memory.sh`; it reports debug and release allocator timings,
usable bytes, compiler statistics, peak resident size, and related macOS
process counters without turning variable benchmark numbers into test
thresholds. `./bench/core/run-cpu.sh` compares one bounded integer workload across
Ace debug/release output and equivalent C `-O0`/`-O2` builds.

## Design rationale

Why the rules in `spec/memory.md` are shaped as they are.

- Operators (Memory #7): distinct operators keep pointer access, Boolean logic,
  and integer bit manipulation visible and type-specific while retaining the
  familiar machine-level spellings. Floats have no `%`: the target has no
  remainder instruction, and an exact one is a data-dependent loop that belongs
  behind an explicit call, not an operator.
- Pointer arithmetic: this exposes the required machine operation without making
  a pointer resemble a sequence, widening permission, adding metadata, or
  implying iterator, ownership, or provenance machinery.
- Pointer-integer and pointer reinterpretation: kernels, embedded and
  memory-mapped access, tagged pointers, integer-address ABIs, custom runtimes,
  renderers, and address diagnostics require the exact machine facility. The
  explicit conversion is the trust boundary; Ace adds no tagged-pointer
  abstraction, hidden metadata, ownership rule, or runtime validation beyond the
  cheap local alignment diagnostic.
- Strings and byte views (Memory #19): use `string` for complete text, `[]char`
  for character-oriented parsing or framing, and `[]u8` for raw bytes; these are
  source-level intent conventions, not runtime or overload distinctions.
- Untyped storage: the machine has no typed memory, and Ace already states
  aliasing explicitly, so type-based disjointness would buy nothing Ace relies
  on while making every foreign union, record header, and device layout
  undefined. Restricting reinterpretation to plain bits keeps compiler-created
  values unforgeable, so the facts passes rely on for them need no debug check.
- Volatile access: access-site marking exposes each observable operation without
  a contagious qualifier system and remains independent of byte layout.
- Volatile access implementation: ordinary and volatile
  fields share one typed-place path which computes the address once, takes width
  only from the final scalar type, and selects volatile behavior at the
  exact-width load/store primitive.
- Typed allocation: typed allocation removes manual byte packing while changing
  nothing about the allocation model — one fixed process-wide strategy,
  machine-true sizes, explicit failure as the null slice, and zero release-mode
  cost (the lowering is the raw platform call plus a constant multiply).
  Reinterpretation (`[]u8` to `[]T`) and implicit conversions stay out
  (Sequences #7): the typed form is a separate allocation path, not a view cast.
- Views of local storage (Memory #29): returning or storing a direct view of
  the frame is a mistake visible in one expression, so it is rejected at no
  runtime cost, as Zig does and as Clang (`-Wreturn-stack-address`) and GCC
  (`-Wreturn-local-addr`) warn. Go instead moves escaping storage to the heap,
  which would make allocation implicit (Memory #5); Rust proves every path with
  lifetimes. Following a view through variables or calls needs that alias and
  lifetime analysis, which Ace leaves out (Memory #12). A plain parameter counts
  as frame storage because small arguments are passed in registers and copied
  into the callee.
- Zero-filled typed allocation (Memory #24): the zero representation is
  universal, so the only cost of offering it is one name, while a hand-written
  clear loop was measured 1.5x to 3x slower to initialize tables of 2^14 to 2^22
  24-byte slots.
- Initialization through views (Values & Passing #8): a writable view hands
  storage to be filled without a zero fill, and tracking array slices whole
  keeps branch joins constant-cost.
- Constant initializers (Values & Passing #12): an initialized constant remains
  a constant across an explicit module boundary, allowing an OS-neutral facade
  to re-export target-selected values without duplicating their machine values.
- Mutability (section decision): local mutability is invisible to optimization
  (SSA); mutation through references is what blocks alias analysis. Syntax is
  spent only where the compiler and reader profit.
