# Types and declarations

Ace declarations use `name: type` for globals, locals, fields, parameters, and
destructuring bindings. Type inference and declaration elision are not
implemented:

```ace
limit: const int = 1024

main(): int {
    count: int = 13
    next: int = count + 1
    count = 42
    return count + next
}
```

The written type makes the programmer's expectation visible and lets the
compiler check it independently against the initializer. The same rule applies
to literals, null values, aggregates, and functions with one or several return
values.

Locals are mutable by default. `const` is an explicit type modifier and requires
an initializer. Assignment copies the value. A bare local declaration reserves
uninitialized stack storage; it does not emit a zero fill. The compiler rejects
every direct read until the local is definitely initialized.

A global `const` initializer must be a compile-time constant. A
constructor-shaped cast folds when the argument's family matches the target:
integer, character, and boolean arguments (negated forms included) fold into
integer primitives, enums, and distinct types over either, and float arguments
fold into float primitives and distinct types over floats. Aliases resolve to
their terminal type, so alias and primitive-cast constants fold directly.
Integer and float primitives also fold constant arithmetic (integers: `+ - * /
% & | ^ << >>`, unary `-` and `~`; floats: `+ - * /`, unary `-`) over literals,
initialized global constants, and casts, including an integer cast to a float;
`G: f32 = f32(-3)` and `N: int = 2 * 3` are valid globals. Operations are
evaluated at the destination's width, and overflow, division or remainder by
zero, and an out-of-range shift count are compile errors. Casts over strings,
contracts, unions, `Error`, pointers, and aggregates never fold, and a
same-named function still wins over the constructor shape.

```ace
A: type = u8

size: const A = A(5)
mask: const u32 = u32(255)
ratio: const f64 = f64(0.5)
```

Multiple names can be handled by one `=` statement. A name with a type declares
a fresh binding; a bare name reassigns an existing binding. The two forms may
mix in any position, and values are matched by position:

```ace
number: int, message: string = 1, "old"
written: int, message = 2, "new"
```

Multi-assignment evaluates and preserves every value on the right before it
writes any target:

```ace
number, message = 2, "new"
```

A single assignment also evaluates its right side first, then its target, so
a call that grows the slice being assigned into writes to the new storage:

```ace
s.xs[0] = mut s.add() // add may move s.xs; the store goes to where it is now
```

All-typed and all-bare lists remain valid. A typed name that already exists is
a redeclaration error, while a bare missing name is an undeclared-name error.
Names are checked against the scope before the statement, so one item cannot
declare a name for another item to reuse. Every form requires equal numbers of
names and values. New bindings remain uninitialized while the initializer is
evaluated.

This also provides a direct swap without a user-declared temporary:

```ace
a, b = b, a
```

## Aliases and distinct types

A type alias is transparent; both names denote the same type.

```ace
Count: type = int

total: Count = 13
```

Aliases may take explicit type parameters. Each instantiation is exactly its
substituted target type; it does not introduce a new nominal identity.

```ace
Pair[T]: type = (T, T)

coordinates: Pair[int]
```

A distinct type has the same machine layout as its base but a separate nominal
identity. Crossing the boundary requires an explicit constructor-shaped cast.

```ace
Meters: distinct f64

distance: Meters = Meters(12.5)
raw: f64 = f64(distance)
```

## Pointers

`*T` is a read-only pointer and may be null. `mut *T` grants writes through that
pointer level. Address-of requires an lvalue: `&value` produces `*T`, while
`mut &value` produces `mut *T` and is rejected for const or otherwise
read-only storage. Both forms emit the same machine address; `mut` is a
compile-time permission. `*pointer` explicitly dereferences, while field
access and method calls automatically dereference pointers: `pointer.m()` is
`(*pointer).m()`, so a mutating method needs a `mut *T` and the usual `mut`
receiver mark (`mut pointer.bump()`).

```ace
Point: struct { x: int, y: int }

read_x(point: *Point): int {
    return point.x
}

main(): int {
    point: Point = Point{ x = 13, y = 29 }
    pointer: *Point = &point
    return read_x(pointer)
}
```

Parameter mutation and pointee mutation are independent. A `mut` parameter
may replace the value stored in the caller; a `mut *T` may write its pointee:

```ace
set_x(point: mut *Point, value: int) {
    point.x = value       // pointee writable; parameter itself read-only
}

retarget(mut point: *Point, replacement: *Point) {
    point = replacement   // caller's pointer writable; pointee read-only
}

main(): int {
    first: Point = Point{ x = 13, y = 29 }
    second: Point = Point{ x = 42, y = 50 }

    writable: mut *Point = mut &first
    set_x(writable, 21)

    current: *Point = &first
    retarget(mut current, &second)
    return current.x
}
```

`mut point: mut *Point` combines both permissions. Multiple pointer levels
retain them independently: `mut **T` may replace the stored inner pointer,
`*mut *T` may read that inner pointer and write `T`, and `mut *mut *T` permits
both. Only the level performing a write needs `mut`.

Pointer arithmetic is typed and deliberately narrow. For `pointer: *T` or
`mut *T` and `offset: usize`, `pointer + offset` and `pointer - offset` advance
or retreat by `offset * sizeof(T)` bytes and preserve the pointer's exact
permission. Nonnegative integer literals materialize as `usize`; other integer
types require an explicit conversion. An assignable pointer binding also
supports `+=` and `-=`.

```ace
second: mut *Point = mut &points[0] + 1
second.x = 42
second -= 1
```

The pointee must have complete, nonzero-sized layout. Debug builds diagnose a
null starting pointer, scaled-offset overflow, and address overflow or
underflow. Pointers carry no bounds or provenance, so use arrays or slices for
bounded traversal. Pointer indexing, pointer-pointer difference and ordering,
and `offset + pointer` are intentionally unavailable; an extent-free access is
written explicitly as `*(pointer + offset)`.

Every inline value layout must be finite. A pointer breaks a layout cycle, so
self-pointers and mutually pointer-linked structs are valid:

```ace
Node: struct {
    next: *Node,
}

Left: struct {
    right: *Right,
}

Right: struct {
    left: *Left,
}
```

A slice breaks a cycle for the same reason a pointer does: its
pointer-plus-count representation doesn't depend on the element's size. Two
nominal types can therefore reference each other through a runtime-sized
collection without boxing every element behind a pointer:

```ace
Json: union {
    none,
    object: []Pair,
}

Pair: struct {
    key: string,
    value: Json,
}
```

Direct or indirect inline recursion is rejected instead of receiving a
placeholder size:

```ace
Invalid: struct {
    nested: Invalid // recursive value layout
}
```

## Declaration order

Top-level declarations (types, aliases, globals, constants, imports,
functions, specializations, and signings) may be declared in any order within
a file: a field, array length, alias, initializer, signature, or body may name
a declaration written after it, inline or through a pointer. Each declaration
is resolved when it is first needed, so its layout is the same as if it had
been written first; every function signature and signing is resolved before
any body is checked. Locals inside a function body still follow
declaration-before-use ([Declaration order](function.md#declaration-order)).

```ace
buffer: [SIZE]u8

SIZE: const usize = 2 * LINE
LINE: const usize = 64
```

A declaration that needs itself to be resolved, a function signature
included, is an error naming the cycle:

```ace
A: const int = B + 1 // 'A' depends on itself: 'A' -> 'B' -> 'A'
B: const int = A

P: type = *P // an alias expands to itself, even through a pointer

S: struct {
    x: [sizeof(S)]u8, // type 'S' depends on its own layout
}
```

### Design rationale

Go, Zig, Rust, Swift, Java, C#, D, and Odin all make top-level declarations
order-independent; only C and C++ require prototypes or ordering. One rule at
every position replaces the former pointer-only and root-slice forward-type
exceptions, whose boundaries (`[1]*Later` rejected, `[]*Later` accepted) had
to be memorized, and the forward-call fixups, which exempted a later callee
but not its arguments and checked the calling body a second time. The
compile-time cost is two loops that enter every name before checking,
measured at about 2% of a check-only run on a 20,000-function file;
resolution itself happens once per declaration, only in a different order,
and every body is checked once.

## Writable views

Type-position `mut` marks writable content: `mut *T` grants writes through the
pointer, `mut []T` grants writes to the slice's elements, and `mut string` is
the text-intent spelling of `mut []char`/`mut []u8`. Each `mut` marks writable
content at exactly one view level. It is independent of the `mut name: T`
parameter mark, which grants replacing the caller's stored value, and the two
compose per level (`mut data: mut []u8`, `mut point: mut *Point`).

A writable view exists only where `mut` is written at establishment;
afterwards the permission travels in the type:

```ace
fill(data: mut []u8, value: u8) {
    for i: usize in 0 .. data.len {
        data[i] = value
    }
}

main(): int {
    buffer: [4]u8 = [0, 0, 0, 0]
    fill(mut buffer[0..4], 7) // establishment at the call site

    w: mut []u8 = mut buffer[0..4]
    w[0] = 42
    plain: []u8 = w // permission shrink is implicit

    letters: [3]u8 = [u8('a'), u8('c'), u8('e')]
    view: mut string = mut letters[0..]

    return int(plain[1]) + int(view.len) - 3
}
```

`mut buffer[0..4]` is an establishment expression: it yields a `mut []u8` over
the array's owned writable element storage. Re-slicing an already-writable view
propagates permission without restating `mut` (`w[2..4]` is again `mut []u8`).
Applying the establishment form to a plain slice or plain string is always an
error, even when its descriptor variable is locally rebindable: descriptor
replacement cannot manufacture content permission. Once permission shrinks to
a plain view, it cannot grow again for that binding.

Plain forms are read-only content regardless of backing storage. Writing
`v[i] = x` requires the slice's own type to be `mut []T`; a `[]T` parameter
cannot write its elements even when the caller passed writable storage.
Permission shrink is implicit and widening never happens: a `mut []T` assigns,
passes, and returns wherever `[]T` is required, while the reverse is rejected.
The mut-qualified forms are distinct types everywhere a type can appear —
overload sets, struct fields, unions, and generic instantiations — so
`f(s: []u8)` and `f(s: mut []u8)` are different overloads. Exact matches win
first; when no exact candidate fits, the call may still resolve through the
implicit shrink.

`mut string` participates in the same rules because it is the same type as
`mut []char` and `mut []u8`. `.len`, explicit byte-content
comparison, and `null` behave exactly as for `string`; it originates from proven writable
bytes, such as `mut array[0..]`, and re-slicing preserves it. Indexed
byte writes exist only through a `mut string`: `s[i] = c` assigns exactly one
byte (`u8` or `char`) and follows the slice-write bounds contract — a
compile-time error when constants prove the index out of range, a debug-build
diagnostic otherwise, and release assumes it. Plain `string` indexing stays a
value read.

`string`, `[]char`, and `[]u8` are three spellings of one type, not types joined
by implicit conversions. Their writable spellings likewise share identity and
permission. Use the spelling that states the value's intent. Strings iterate bytes as usual, and
`for b: u8 in mut text` runs the ordinary alias loop when `text` is a
`mut string`; a plain string is rejected by the same rule as any read-only
view.

## Scalar-backed bit fields

A `bits` declaration defines a nominal value with the size, alignment, and raw
representation of an exact unsigned backing integer. Fields are packed from bit
0 upward in declaration order; `_` reserves an inaccessible range.

```ace
Permissions: bits u8 { read: 1, write: 1, level: signed 3, _: 3 }

permissions: Permissions = Permissions { read = 1, level = -2 }
raw: u8 = u8(permissions)
restored: Permissions = Permissions(raw)
restored.write = 1
```

Widths must be positive and fill the backing `u8`, `u16`, `u32`, or `u64`
exactly. Unsigned fields read as the backing unsigned type; signed fields read
as its signed counterpart with exact two's-complement sign extension. Literals
start from all-bits-zero, so named fields may be omitted and anonymous ranges
remain zero.

Named fields can be read and assigned but have no address: they cannot be used
with address-of, a `mut` argument, `offsetof`, or individual volatile access.
Assignment preserves every other backing bit. Constant out-of-range writes are
compile errors; dynamic writes abort in debug builds and retain the low field
bits in release builds. Whole values copy normally, compare with `==`/`!=` only
against the same nominal type, compose in aggregates, and support whole-value
volatile loads and stores. C boundaries use the backing integer plus an explicit
conversion; a `bits` type itself is not C-ABI-compatible.

## Zero and absence

Every type has an all-bits-zero representation. Pointer, slice, string,
function, contract, `Error`, and `typeid` values spell that representation
`null`; no optional type constructor or hidden presence tag exists.

```ace
maybe: *Point = null
```

Zero-capable values may be compared directly with `null`. Dereferencing a zero
pointer, calling a zero function, or dispatching through a zero contract is
invalid. A zero `Error` is success and exposes zero domain/code bits. When zero is ordinary application data,
represent presence explicitly with a struct containing a `present: bool` field.

`blank(T)` spells the all-bits-zero value of any type, including a generic type
parameter where `null` and literals cannot: `0` for `int`, `0.0` for floats,
`false` for `bool`, and an all-zero struct, array, slice, or string. It takes one
type in the full type grammar and does no work beyond zero-filling. It is a
filler, not a marker of absence, so a function that has nothing to return
pairs it with an `Error`:

```ace
pop(): (T, Error) {
    if self.len == 0 { return blank(T), ArrayErr.empty }
    ...
}
```

The current bootstrap has debug/release emission modes for sequence bounds,
but null-use diagnostics have not landed; invalid zero operations therefore
still follow the unchecked path.

## Definite initialization

A local without `= value` is uninitialized storage, not an implicitly zeroed
value. Whole assignment initializes it. Struct fields and constant array
indexes can be initialized separately; using the whole aggregate requires all
of its parts to be initialized.

```ace
Point: struct { x: int, y: int }

main(): int {
    point: Point
    point.x = 40
    point.y = 2
    return point.x + point.y
}
```

After a branch, an initialization fact survives only if every continuing path
establishes it. A loop alone cannot establish initialization after the loop,
because it may execute zero times.

A view of a local is checked once, where it is created. A read-only view
(`&path`, `array[lo..hi]`) reads what it may reach: a constant path exactly,
and the whole array for a dynamic index or any array slice. A writable view
(`mut &path`, `mut array[lo..hi]`) reads nothing and initializes a constant
path, or the whole array when the slice spans every element; you promise the
storage is written before it is read. A buffer filled by dynamic index is
therefore filled through one full-range writable view:

```ace
fill_prefix(bytes: mut []u8): usize {
    bytes[0] = u8(97)
    return 1
}

main(): int {
    storage: [16]u8
    bytes: mut []u8 = mut storage[0..16] // storage is now initialized
    count: usize = fill_prefix(bytes)
    return int(string(bytes[0..count]).len)
}
```

When every element is written by a loop, start from a zeroed array
(`table: [256]u8 = [0 : 256]`): the fill is one `memset`, small beside the loop
itself. Heap storage from `mem.alloc` is uninitialized and needs no fill.

Nothing is tracked through a view afterwards: pointer aliases, inline
assembly, and extern code are programmer-trusted boundaries with no hidden
alias analysis.

## Arrays and slices

`[N]T` is an inline array containing exactly `N` elements, where `N` is a
compile-time constant expression: a literal, a `const` identifier,
`sizeof`/`alignof`/`offsetof`, or integer/bitwise arithmetic over these — the
same restricted surface `@align(N)` uses. `[]T` is a two-word view containing
a pointer and length.

```ace
values: [4]int = [10, 20, 30, 40]
zeros: [4]int = [0 : 4]
middle: []int = values[1..3]
answer: int = middle[0] + middle[1]

BUFFER_SIZE: const usize = 8
buffer: [BUFFER_SIZE]u8 = [0 : BUFFER_SIZE]
```

An array repetition literal `[value : count]` takes its exact array type from
the surrounding declaration, assignment, return, or parameter. `count` is a
compile-time constant expression and must evaluate to the array type's
length. `value` is evaluated once and copied into every element in index
order; no allocation or shared element storage is introduced. A constant
whose encoding repeats one byte, or a runtime one-byte integer, fills the whole
array in one pass; any other value is stored into element 0 and the filled
prefix is doubled by copies, so generated code grows with `log2(count)`.

Where a plain `[]T` is expected (a parameter, declaration, assignment,
return, struct field, or `for` iterable, whose `T` is the value binder's
type), an array literal supplies its own length and takes
`T` from the context; it is an anonymous `[N]T` viewed as `[]T`:

```ace
total(xs: []int): int {
    sum: int = 0
    for x: int in xs { sum += x }
    return sum
}

primes(): []int {
    return [2, 3, 5, 7]
}

six: int = total([1, 2, 3])

for name: string in ["mem", "fmt"] { … }
for c: u8 in [1, 2] { … } // constants materialize as u8
```

When every element is a compile-time constant, the array is read-only static
data emitted once, so the view stays valid for the whole program and
`primes()` may return it; the call costs no stores. Otherwise the array is
stack storage of the enclosing function, built exactly as a named
`tmp: [N]T = [...]` would be, and the view is valid while that function runs.
An empty literal is the null view. A cast supplies the context too: `[]int([])`
is a typed empty view and `[2]int([x, y])` an array where no target type is
expected. The view is plain: a `mut []T` context or `for … in mut`
rejects a literal, since nothing could read what is written through it. A
keyed literal needs its `[E]T`. Overload selection gives a literal the candidate
element type its elements share (`int` for integers, `f64` for floats, `u8`
for characters, a typed element's own type), so `pick([1, 2])` chooses
`pick(xs: []int)` over `pick(xs: []string)`. It matches both `[]E` and an
`[N]E` of its length, so a set holding both is ambiguous for it. A nested
literal recurses: inner literals must agree on their innermost type at the
same depth, and each level matches a `[]E` with inner literals of any length
(`f([[1], [2, 3]])` picks `f(xs: [][]int)`) or an `[N]E` with exactly `N`.
Elements without one shared candidate (`[1, 2.5]`, `[]`, `[[1], ["a"]]`,
`[[1], [[2]]]`) match no overload; cast an element or name the array instead.

### Design rationale

The literal's candidate follows the scalar rule element by element rather
than ranking conversions, as C++ deduces `std::initializer_list<T>` only from
agreeing elements: selection stays exact, and `f([1])` succeeds exactly when
`f(1)` would pick the same element type.

The direct read-only property `sequence.len` is available on built-in arrays (constant element
count), slices (stored element count), and strings (stored byte count), including
their writable forms. It returns `usize`, performs no call or dispatch, and is
not provided by structs, tuples, unions, contracts, or user-defined wrappers. A
struct can still declare an ordinary field named `len`.

Slicing is uniform across arrays, slices, and strings with half-open
`[lo, hi)` bounds and the defaults `0` and `source.len`: slicing an array or
slice yields `[]T`, and slicing a string yields a `string`; the result carries
the source's permission, so a writable source stays writable and slicing never
widens permission ([Writable views](#writable-views)). The result length is
`hi - lo` and the result pointer is the source pointer advanced by
`lo * sizeof(T)`; slicing a slice re-bases inside that slice's own window.
Empty results keep their computed pointer and are never canonicalized to
`null`. Bounds may be any integer; when constants prove
`0 <= lo <= hi <= source.len` false, the program is rejected at compile time,
while debug output validates dynamic bounds and aborts with a standard-error
diagnostic on failure. Release output assumes the complete precondition.

All four bound forms are available:

```ace
source[lo..hi] // explicit half-open range [lo, hi)
source[..hi]   // prefix: lo defaults to 0
source[lo..]   // suffix: hi defaults to source.len
source[..]     // complete view: 0 through source.len
```

```ace
text: string = "hello world"
tail: string = text[6..]
letter: u8 = text[0]
word: const string = "hello"
window: string = word[1..word.len - 1]
```

`string`, `[]char`, and `[]u8` share identity and may be assigned, passed, and
returned directly under any of those intent spellings. A fixed array remains a
different owned-storage type and reaches a view only through explicit slicing.

```ace
arr: [3]u8 = [u8('o'), u8('k'), u8('!')]
view: string = arr[0..3]
arr[0] = u8('n')
same_bytes: bool = view == "nk!"
```

`sizeof` and `alignof` accept the full inline type grammar, so layouts are
directly expressible: `sizeof([]u8)`, `sizeof([]char)`, and
`sizeof([3]char)` are compile-time values 16, 16, and 3 on the current target;
`alignof([]u8)` and `alignof([3]char)` are 8 and 1.

Array assignment copies all elements. Slice assignment copies only its pointer
and length, so both values view the same backing storage; it never copies the
elements. Slicing does not allocate. See [Memory](memory.md) for allocation and
lifetime rules. Every element read or write requires
`0 <= index < sequence.len`: static violations are compilation errors, debug
output validates dynamic indices, and release output emits no guard.

These layout facts are normative — code may rely on them. A slice is exactly two
words: the element pointer at offset 0 and the element count at offset 8, giving
`sizeof([]T)` = 16 with alignment 8; there is no capacity word and no hidden
field. An array stores `N` contiguous elements in index order with no metadata
and no inter-element padding, so `sizeof([N]T)` = `N * sizeof(T)` and array
alignment is the element's alignment. A string uses the identical two-word
descriptor over bytes (pointer, byte count; `sizeof(string)` = 16) and is
never NUL-terminated. Plain strings are read-only content; the writable
`mut string` form is described under [Writable views](#writable-views).

Sequential values — arrays, slices, and strings — do not use `==` or `!=`.
Compare byte content explicitly with `mem.equal`, which accepts every pairing
of `[N]u8`, `[]u8`, and `string`, including fixed arrays of different lengths.
Lengths must match, then every stored byte must match exactly:

```ace
import mem

prefix: [12]u8 = [72, 84, 84, 80, 47, 49, 46, 49, 32, 50, 48, 48]
view: []u8 = prefix[0..12]

array_match: bool = mem.equal(prefix, "HTTP/1.1 200")
slice_match: bool = mem.equal("HTTP/1.1 200", view)
```

The call does not allocate or convert either value. Strings are UTF-8 by
convention, but `mem.equal` compares their exact stored encoding without Unicode
decoding or normalization.

A comparison whose other operand is the literal `null` is a nullness test, not a
content comparison: it is true only when both descriptor words are zero. An
empty but non-null slice may match another empty sequence through `mem.equal`,
while it does not equal `null`.

Non-byte arrays and slices have no built-in content comparison; use an explicit
loop. Compare lengths alone with `a.len == b.len`. `mem.compare` remains the
lexicographic operation for byte slices.

Arrays and slices do not convert implicitly in either direction. The explicit
bridges are slicing (`arr[lo..hi]`, array to slice; `mut arr[lo..hi]`, array to
writable view) and `mem.copy` into an established writable view of the array.
`string`, `[]char`, and `[]u8` need no bridge because they share identity. A
fixed byte array reaches that view type only through slicing first.

### Enum-indexed arrays

`[E]T` is an array with one `T` per member of `E`, indexed by `E` itself. `E`
must be dense: its members are exactly `0 .. n-1`, in any declaration order.
The layout is `[n]T`'s, but the type is distinct from `[n]T` and neither
converts to the other.

```ace
Color: enum { red, green, blue }

NAME: [Color]string = [
    Color.red = "red",
    Color.green = "green",
    Color.blue = "blue",
]

name(c: Color): string {
    return NAME[c]
}
```

The keyed literal names every member exactly once, in any order; a missing,
repeated, or foreign key is a compile error naming it, so the table cannot fall
out of step with the enum. Values are evaluated in written order. A positional
literal is rejected; `[v : n]` still repeats one value.

An index is exactly `E`: an integer or another enum is a type error. No bounds
check is emitted in either mode, because a debug build already checks every
conversion into `E`. `.len` is the member count; `for c: Color, s: string in
NAME` binds the index as `Color`; a slice `NAME[Color.green..]` takes `E` bounds
and yields `[]string`. Writing every member by its constant index initializes
the array, as for constant integer indexes.

## Compile-time static lookup maps

`StaticMap[K]V` declares an immutable module-level lookup table:

```ace
status: StaticMap[u16]string = {
    100: "Continue",
    200: "OK",
    404: "Not Found",
}

phrase: string = status[code]
```

The declaration does not take `const`; the type itself makes the binding and
entries immutable. Keys and values are global constant expressions, duplicate
keys are rejected, and construction performs no runtime work. Keys may be
Booleans, characters, integers, enums, `typeid`, strings, or distinct types
backed by one of those types. String keys compare exact bytes.

Indexing is the complete operation surface. There is no mutation, iteration,
membership operation, insertion, removal, or method lookup. A missing key
returns the value type's canonical zero. This deliberately makes a missing key
indistinguishable from an entry that stores canonical zero; for `string`, the
missing result is null rather than a non-null empty literal.

## Multiple-return and function types

Multiple-return signatures use positional tuple layout. Function types use
parameter types followed by their return type.

```ace
operation: (int, int): int = add
```

Multiple-return values use the platform aggregate ABI and are consumed through
typed destructuring. A tuple literal may initialize or replace an explicit
tuple of the same arity. Each member meets its declared member type the way a
struct field initializer does: a literal takes that type and must fit it, and a
typed value must match it exactly.

```ace
pair: (int, f64) = (20, 22.0)
pair = (21, 23.0)
narrow: (i32, u8) = (-1, 'z')
```

A struct field of function type is called in place. `x.f(args)` calls the
stored value with exactly `args`, no receiver, and needs only read access to
`x`; `(x.f)(args)` is the same call. The field is reached as any field access
is, through pointers, chains, and indexing:

```ace
Button: struct { on_click: (int): int }

button: Button = Button{ on_click = twice }
clicked: int = button.on_click(3)
held: *Button = &button
again: int = (held.on_click)(4)
```

A struct's fields and its own methods share one namespace: a method named like
a field is an error whatever its signature, so a field never joins a method
overload set. Calling a field that does not hold a function is "not callable",
and `(x.m)` naming a method is an error, because methods are not values.

## Structs, enums, and tagged unions

Struct fields retain declaration order and natural alignment. Struct values have
copy semantics.

```ace
Point: struct { x: i32, y: i32 }

point: Point = Point{ x = 13, y = 42 }
```

A field may declare a compile-time default. Named struct literals may omit only
defaulted fields, and an empty literal is valid only when every field has a
default. An explicit initializer overrides the default.

```ace
Request: struct { retries: u8 = u8(3), path: string }

request: Request = Request { path = "/status" }
custom: Request = Request { retries = u8(5), path = "/health" }
```

Defaults apply only to struct literals. Bare locals, globals, allocations,
arrays, and canonical-zero values remain uninitialized or zeroed according to
their ordinary storage rules; a field default does not change layout or ABI.

An enum is a nominal type with distinct-typed named constants. By default its
members are numbered from zero and it has `i32` layout.

```ace
State: enum { idle, running, stopped }

state: State = State.running
```

Select a fixed integer backing type and explicit discriminants when an external
format owns the values or representation. The first member must remain zero;
omitted values continue from the preceding member.

```ace
Opcode: enum u8 {
    none,
    load = 0x10,
    store, // 0x11
}
```

The backing type must be a fixed-width integer type. Values must fit it and may
not be duplicated. Size, alignment, fields, arrays, parameters, and returns use
that exact representation; conversions to and from integers remain explicit.
An enum value is always a member: a constant that names no member is a
compile-time error, whether written `State(7)` or taken implicitly (`s: State =
7`), and `--debug` traps a runtime conversion that names no member, testing
the value before it narrows to the backing type.

`.name` on an enum value is the member's source spelling as a read-only
`string`; on the type, `State.name` would still name a member called `name`.

```ace
label: string = state.name // "running"
```

The names are static data, and their lookup is emitted only for an enum whose
`.name` is used. It maps any discriminant, dense or sparse, without allocating,
and reads through pointers as other fields do. There is no general reflection:
`.name` is the only such question, and only an enum value answers it.

An enum whose member list ends with `_` is open: every value of its backing
type is a value of the enum, as for a C enumeration a library may extend. It
must state its backing type. Conversions and constants take any value without
a membership check, `.name` of a non-member is `""`, and it cannot index an
array. A switch reaches `default` for a value no case names.

```ace
Key: enum u32 { unknown, a = 'a', b = 'b', _ }
slash: Key = Key('/')   // a value, though no member names it; slash.name is ""
```

A closed enum read from C, as a return, a field C wrote, or a callback
argument, must hold a member; declare a C enumeration that can grow open.

A tagged union is a closed set of named alternatives stored inline. Variants
may carry no data or one explicitly typed payload:

```ace
Event: union {
    none,
    quit,
    key: u32,
    cursor: Point,
}
```

Variant names are chosen by the programmer. `none` and `quit` are not
keywords, and a member name may even be one: fields, enum members, variants,
error entries, and methods are only declared in their list or written after
`.`, so `e.type`, `Kind.if`, and `Config.default()` are unambiguous. Bindings
(locals, parameters, globals, types, functions) keep every keyword reserved. Construct a payloadless variant as a qualified value and a payload
variant with one argument:

```ace
event: Event = Event.quit
event = Event.key(42)
event = Event.cursor({ x = 10, y = 20 })
```

Inspect a union with `switch`. A payload binder is an explicitly typed local
copy, so changing it does not change the union:

```ace
switch event {
    Event.none {}
    Event.quit {}
    Event.key(code: u32) {
        code += 1
    }
    Event.cursor(point: Point) {
        point.x = 0
    }
}
```

Every variant must appear once unless the switch contains `default`. Payloads
cannot be accessed outside their matching arm. Replacing a payload or variant
uses whole-value assignment.

The representation is an `i32` tag followed by aligned storage for the largest
payload. Declaration order assigns tags from zero, but numeric tags are not
exposed. The first variant is the canonical all-bits-zero value, including for
a global without an initializer. There is no allocation, boxing, reflection,
`any`, or niche optimization.

Payloadless variants are complete values and can be compared directly when all
of the union's payload types support equality:

```ace
Status: union { none, quit, key: u32 }
status: Status = Status.quit

if status == Status.quit {
    return
}
```

Whole-union `==` and `!=` compare the tag and then the active payload, never
inactive storage or padding. Generic unions use explicit instantiation just
like generic structs:

```ace
Result[T]: union { empty, value: T }
result: Result[int] = Result[int].value(42)
```

Tagged unions are not currently permitted in `extern "c"` signatures.

## Generic types

Generic structs name their type parameters in brackets. Instantiation always
states the arguments explicitly.

```ace
Box[T]: struct { value: T }

box: Box[int]
box.value = 13
```

Every used type tuple has a concrete layout. Ace performs no constraint or trait
solving.

## Contracts

A contract value is two words: a const vtable pointer and a data pointer. A
struct has one canonical signing listing every contract it fulfills, and
construction explicitly names the target contract. There is no inheritance,
runtime lookup, or trait solving.

An Ace contract is a required method set. It does not introduce preconditions,
postconditions, assertions, or hidden runtime checking.

```ace
Value: contract {
    get(self): int
}

Number: struct { value: int }

Number signs Value

Number.get(self): int {
    return self.value
}

main(): int {
    number: Number = Number{ value = 13 }
    value: Value = Value(&number)
    return value.get()
}
```

Methods are owned by their struct. A method names its receiver as its first
parameter, `self`, which takes no type: it is the owner. `self` is read-only;
a mutating method takes `mut self` and is called with visible `mut`:

```ace
Number.set(mut self, value: int) {
    self.value = value
}

mut number.set(21)
```

A function declared on a type without `self` is an associated function, for
constructors and other operations with no receiver. It is declared and called
through the type with the same `.`; calling it through a value is an error:

```ace
Number.zero(): Number {
    return Number{ value = 0 }
}

number: Number = Number.zero()
```

`self` is what tells the two kinds apart, so a method and an associated
function may not share a name on the same type. A contract method names its
receiver the same way: `get(self): int`, `set(mut self, value: int)`.

Inside a generic, a type parameter reaches its bound type's associated
functions the same way, checked in each instantiation: `x: T = T.zero()`
calls `Number.zero()` in `f[Number]`, and `f[int]` is an error because `int`
owns none.

One signing may list multiple contracts on one line:

```ace
Number signs Value, Printable
```

Bare comma-separated lists cannot cross a newline. Use parentheses when the
contract list needs multiple lines:

```ace
Number signs (
    Value,
    Printable,
)
```

Single-line contract lists omit the parentheses, giving each layout one
canonical spelling.
A second `Number signs` declaration is an error. Contract calls dispatch through
the generated const vtable; concrete method overloads remain compile-time direct
calls. Methods are called only on a value (`x.m(...)`); `Type.m(x, ...)` is an
error and a bare `Type.method` is not a function value.

A contract method's own parameters may not be `mut` — dispatch has no way to
validate mut agreement against the implementing method (the receiver's own
`mut self` is unaffected).

Signings and methods are private by default. Put visibility first:
`pub Number signs Value` and `pub Number.set(mut self, value: int)`. A contract
over const backing storage may be read, but mutating dispatch through it is a
compile error.

### Explicit embedding

An `embed` field is ordinary named inline composition whose methods are promoted
one level:

```ace
Named: struct { text: string }

Named.name(self): string {
    return self.text
}

Human: struct {
    embed named: Named,
}

human: Human = Human { named = Named { text = "Ada" } }
name: string = human.name()       // same method as human.named.name()
```

Fields are not promoted: the stored text remains `human.named.text`. A method
owned directly by `Human` takes precedence over a promoted method; a field of
`Human` named like a promoted method is an error; two matching
methods from separate embedded fields are an ambiguity error. Promotion does
not recurse through another embedded field.

Contracts flow only from the base to the embedder. If `Named signs Value`, a
promoted exact method may satisfy an explicit `Human signs Value`; the reverse
never happens. The compiler dispatches through a fixed-offset receiver thunk,
with no allocation, object metadata, subtype relation, or runtime search.

## Current limits

- Type inference and implicit generic instantiation are not implemented.
- Generic aliases are not implemented by the current compiler; generic structs
  are supported.
- Debug-build sequence bounds, integer arithmetic, `noalias`/length,
  null-use, and allocator diagnostics are implemented and stripped in release.

The examples are covered by `tests/sema/docs/type_ok.ace`.

## Design rationale

Why the rules in `spec/syntax.md`, `spec/types.md` are shaped as they are.

- Syntax & Parsing #8 (slice-context literals): passing a list inline, as Go
  `[]int{1, 2}`, Zig `&.{1, 2}`, and Rust `&[1, 2]` do, removes a named array
  whose only use is `name[..]`. Ace never allocates implicitly, so the storage
  is static data when constant (Rust's promotion to `'static`, Zig's comptime
  data) and stack storage otherwise (C99 compound literals); Go's heap escape
  is unavailable. A literal-backed view stays plain, as string literals do.
- Type System #13, Syntax & Parsing #16: a table parallel to an enum (display
  names, precedence, dispatch) stays in step by construction, where a
  positional `[N]T` shifts silently when a member is added. Indexing by `E`
  costs one load with no cast (Pascal `array[E] of T`; Odin `[E]T`, which also
  admits integer indexes and so loses the guarantee). Keys use `=` as struct
  literals do; `:` would collide with repetition `[x : n]`.
- Safety #2 (enum conversions): checking `E(n)` catches a bad value where it is
  made rather than where it is used, covers `switch` and every table as well,
  and runs less often than indexing (Zig checks `@enumFromInt` likewise). A
  dense enum costs one unsigned compare.
- Type System #15 (open enums): a C enumeration is open, and a library may add
  values a binding never named, while a closed enum's "always a member"
  guarantee is what lets `[E]T` skip bounds checks. Marking openness on the
  type keeps that guarantee for closed enums and costs an open one nothing.
  Zig's non-exhaustive `enum(u32) { ..., _ }` is the same design; Swift imports
  non-frozen C enums as open, and Rust's bindgen emits constants instead
  because an unknown discriminant is undefined behavior. `_` already means the
  unnamed part of a `bits` type. `.name` gives `""` for a non-member rather
  than rejecting `.name`, so code can print what it knows.
- Type System #14 (enum `.name`): a hand-written name table repeats every
  member and drifts when one is added; the compiler already holds the
  spelling. Zig `@tagName`, Rust `#[derive(Debug)]`, Go `stringer`, and Nim `$`
  provide it. It answers one fixed question about a value, emitted only when
  asked (as `Error.name`, Errors #14), so the no-reflection rule stands. It is
  `name`, not `tag`: "tag" is a union's discriminant number, and `Error.name`
  already means a declaration's spelling.

- Source #3: a member name is never bare — it follows `.` or sits in its
  declaration list — so a keyword there cannot be misread, and the next token
  already decides each position. Mirroring C names (`SDL_Event.type`) and
  naming kinds after keywords (`Kind.if`) then needs no rename or escape;
  JavaScript property names and Swift member access work the same way, while
  Rust (`r#type`) and Zig (`@"type"`) pay with escape syntax.
- Memory #9 (assignment order): evaluating the right side before the target
  is the one order under which a call that reallocates or rewrites the target
  cannot leave its address stale, and it matches multi-assignment. C++17
  (P0145) and Rust order `=` this way; C leaves it unsequenced, and Java, C#,
  and Go compute the target's array first, so they store into the old one. A
  scalar value waits in a register, so it costs nothing.
- Syntax & Parsing #9: one declaration rule works uniformly for literals,
  aggregates, null values, and single- or multi-return expressions while making
  the programmer's expected type locally visible and independently checkable
  against the initializer.
- Syntax & Parsing #13: omission explicitly authorized by the type author
  removes repetitive construction without hidden work or weakening definite
  initialization, while per-field opt-in keeps required state visible. `=`
  consistently supplies a value, while `:` consistently associates a name with a
  type.
- Syntax & Parsing #14: error entries are declarative records rather than an
  expression list; a line-oriented form stays easy to scan and parse without
  redundant separators.
- Syntax & Parsing #15: a compile-time constant identifier is exactly as fixed
  and safe as a literal, so a literal-only grammar bought no safety — only
  friction for named sizes shared across declarations (`INPUT_SIZE: const usize
  = 8192` backing `input: [INPUT_SIZE]u8`).
- Names & Scoping #11, Abstraction #3: one namespace for fields and methods
  makes `x.f(...)` mean one thing from the declaration alone, as in Go, where a
  field and method may not share a name. Letting a field join the overload set
  would have the argument types silently pick between a stored value and a
  method; rejecting the collision keeps every call site's meaning local, costs
  only a rename, and the call itself is the plain indirect call a local
  function value already compiles to.
- Abstraction #3, #9, #15: an explicit `self` makes the receiver visible in
  every signature, as in Zig and Python, and lets one separator serve every
  type-owned function: `.` after a type reaches the type (associated
  functions, as enum members and union variants already did), `.` after a
  value reaches the value. A separate `::` spelling existed only to tell the
  two kinds apart, which `self` now does at the declaration.
- Names & Scoping #2: forbidding a type and a value from sharing a name in
  overlapping scopes gives `X.m` one lookup with no fallback. Java resolves
  the same ambiguity by letting the value obscure the type (JLS 6.4.2), a rule
  the reader has to remember at every use; rejecting the collision costs only
  a rename where the names are declared.
- Type System #3: the destination only settles how a value with no type of its
  own is stored, or picks among existing declarations; it never creates an
  instantiation, so every stamp is spelled at its use. Type-argument deduction
  from arguments or destinations, multi-binding deduction, and declaration type
  elision remain absent and could only be added later as opt-in relaxations
  (PHILOSOPHY.md, Considerations #1).
- Type System #9: explicit controls complete machine-true wire, hardware, and C
  representations without changing ordinary struct layout.
- Type System #11: explicit values and widths make opcodes, wire values,
  registers, and integer-backed ABI fields machine-true while the strict zero,
  fit, and uniqueness checks preserve Ace's local safety and leave aliases or
  other relaxations additive (PHILOSOPHY.md, Priority Stack and Considerations
  #1).
- Type System #12: sparse standard-library LUTs need direct declarative source
  without hand-written ranges, holes, or switches. A module-only lookup surface
  supplies that use while preserving recursive descent, in-order compilation,
  direct machine-code emission, explicit values, and no hidden allocation or
  general compile-time execution.
- Scalar-backed Bit Fields #7: deterministic machine-register and compact binary
  layouts are useful without importing C's implementation-defined bit fields or
  weakening Ace's explicit mutation, debug-build, layout, and access-site
  volatile rules.
