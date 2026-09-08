# Types and declarations

Ace declarations use `name: type` for globals, locals, fields, parameters, and
destructuring bindings. Type inference is not implemented. One local-only
convenience may elide a single binding's type when its initializer already has
an independent type:

```ace
limit: const int = 1024

main(): int {
    count: int = 13
    next := count + 1
    count = 42
    return count + next
}
```

`name := expression` always declares a new local; it is never assignment. The
initializer is typed before the new name exists and receives no type context
from the declaration. Standalone integer, float, and character literals use
their existing defaults (`int`, `f64`, and `u8`). Values such as `null`, bare
aggregate literals, and multi-return calls need an explicit typed declaration.
The form is unavailable for globals and multi-bindings. This is declaration
type elision from an independently typed expression, not contextual inference.

Locals are mutable by default. `const` is an explicit type modifier and requires
an initializer. Assignment copies the value. A bare local declaration reserves
uninitialized stack storage; it does not emit a zero fill. The compiler rejects
every direct read until the local is definitely initialized.

A global `const` initializer must be a compile-time constant. A
constructor-shaped cast folds when the argument's family matches the target:
integer, character, and boolean arguments (negated forms included) fold into
integer primitives, enums, and distinct types over either, and float arguments
fold into float primitives and distinct types over floats. Aliases resolve to
their terminal type, so alias and primitive-cast constants fold directly. An
int-to-float crossing does not fold, casts over strings, contracts, unions,
`Error`, pointers, and aggregates never fold, and a same-named function still
wins over the constructor shape.

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

All-typed and all-bare lists remain valid. A typed name that already exists is
a redeclaration error, while a bare missing name is an undeclared-name error.
Names are checked against the scope before the statement, so one item cannot
declare a name for another item to reuse. Every form requires equal numbers of
names and values. New bindings remain uninitialized while the initializer is
evaluated. `:=` remains a single-name declaration and never accepts a type.

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
compile-time permission. `~pointer` explicitly dereferences, while field
access automatically dereferences pointers.

```ace
Point: struct { x: int, y: int }

read_x(point: *Point): int {
    return point.x
}

main(): int {
    point: Point = Point{ x: 13, y: 29 }
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
    first: Point = Point{ x: 13, y: 29 }
    second: Point = Point{ x: 42, y: 50 }

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

Pointer arithmetic is not implemented. Use arrays or slices for bounded
sequences.

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

The later name in a same-module field is permitted only when every type edge
from the field is a pointer (`*Later`, `**Later`); the same rule applies to a
tagged-union payload type. The declaration must appear later in the file.
Arrays, tuples, aliases, and distinct wrappers do not extend the pointer-only
path, so `[1]*Later` and `*[1]Later` still require `Later` to be declared first.
Before the declaration, member access, dereference, `sizeof`, and other
operations requiring the exact layout are rejected. An unresolved pointer-only
name is diagnosed at the end of the file.

Direct or indirect inline recursion is rejected instead of receiving a
placeholder size:

```ace
Invalid: struct {
    nested: Invalid // recursive value layout
}
```

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
    for i: usize in 0 .. len(data) {
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

    return int(plain[1]) + int(len(view)) - 3
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
`mut []char` and `mut []u8`. `len`, explicit byte-content
comparison, and `null` behave exactly as for `string`; it originates from proven writable
bytes, such as `mut array[0..]`, and re-slicing preserves it. Indexed
byte writes exist only through a `mut string`: `s[i] = c` assigns exactly one
byte (`u8` or `char`) and follows the slice-write bounds contract — a
compile-time error when constants prove the index out of range, a checked-build
diagnostic otherwise, and release assumes it. Plain `string` indexing stays a
value read.

`string`, `[]char`, and `[]u8` are three spellings of one type, not types joined
by implicit conversions. Their writable spellings likewise share identity and
permission. Use the spelling that states the value's intent. Strings iterate bytes as usual, and
`for b: u8 in mut text` runs the ordinary alias loop when `text` is a
`mut string`; a plain string is rejected by the same rule as any read-only
view.

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
The current bootstrap has checked/release emission modes for sequence bounds,
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
because it may execute zero times. Taking an address does not read the value;
pointer aliases, inline assembly, and extern code are programmer-trusted
boundaries with no hidden alias analysis.

## Arrays and slices

`[N]T` is an inline array containing exactly `N` elements. `[]T` is a two-word
view containing a pointer and length.

```ace
values: [4]int = [10, 20, 30, 40]
zeros: [4]int = [0 : 4]
middle: []int = values[1..3]
answer: int = middle[0] + middle[1]
```

An array repetition literal `[value : count]` takes its exact array type from
the surrounding declaration, assignment, return, or parameter. `count` must be
a nonnegative integer literal equal to the array type's length. `value` is
evaluated once and copied into every element in index order; no allocation or
shared element storage is introduced.

The intrinsic `len(sequence)` is available on built-in arrays (constant element
count), slices (stored element count), and strings (stored byte count), including
their writable forms. It returns `usize`, requires parentheses, and is not
provided by structs, tuples, unions, contracts, or user-defined wrappers. A
struct can still declare an ordinary field named `len`.

Slicing is uniform across arrays, slices, and strings with half-open
`[lo, hi)` bounds and the defaults `0` and `len(source)`: slicing an array or
slice yields `[]T`, and slicing a string yields a `string`; the result carries
the source's permission, so a writable source stays writable and slicing never
widens permission ([Writable views](#writable-views)). The result length is
`hi - lo` and the result pointer is the source pointer advanced by
`lo * sizeof(T)`; slicing a slice re-bases inside that slice's own window.
Empty results keep their computed pointer and are never canonicalized to
`null`. Bounds may be any integer; when constants prove
`0 <= lo <= hi <= len(source)` false, the program is rejected at compile time,
while checked output validates dynamic bounds and aborts with a standard-error
diagnostic on failure. Release output assumes the complete precondition.

All four bound forms are available:

```ace
source[lo..hi] // explicit half-open range [lo, hi)
source[..hi]   // prefix: lo defaults to 0
source[lo..]   // suffix: hi defaults to len(source)
source[..]     // complete view: 0 through len(source)
```

```ace
text: string = "hello world"
tail: string = text[6..]
letter: u8 = text[0]
word: const string = "hello"
window: string = word[1..len(word) - 1]
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
`0 <= index < len(sequence)`: static violations are compilation errors, checked
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
loop. Compare lengths alone with `len(a) == len(b)`. `mem.compare` remains the
lexicographic operation for byte slices.

Arrays and slices do not convert implicitly in either direction. The explicit
bridges are slicing (`arr[lo..hi]`, array to slice; `mut arr[lo..hi]`, array to
writable view) and `mem.copy` into an established writable view of the array.
`string`, `[]char`, and `[]u8` need no bridge because they share identity. A
fixed byte array reaches that view type only through slicing first.

## Multiple-return and function types

Multiple-return signatures use positional tuple layout. Function types use
parameter types followed by their return type.

```ace
operation: (int, int): int = add
```

Multiple-return values use the platform aggregate ABI and are consumed through
typed destructuring. A standalone tuple literal may initialize or replace an
explicit tuple when its independently resolved member types match exactly.

```ace
pair: (int, f64) = (20, 22.0)
pair = (21, 23.0)
```

Tuple construction performs no member-type inference. Integer, float, and
character literals first take their ordinary `int`, `f64`, and `u8` defaults;
write explicit casts for other member types.

## Structs, enums, and tagged unions

Struct fields retain declaration order and natural alignment. Struct values have
copy semantics.

```ace
Point: struct { x: i32, y: i32 }

point: Point = Point{ x: 13, y: 42 }
```

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
keywords. Construct a payloadless variant as a qualified value and a payload
variant with one argument:

```ace
event: Event = Event.quit
event = Event.key(42)
event = Event.cursor({ x: 10, y: 20 })
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

Every variant must appear once unless the switch contains `else`. Payloads
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

Tagged unions are not currently permitted in `extern` or `export` signatures.

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
    get(): int
}

Number: struct { value: int }

Number signs Value

Number.get(): int {
    return self.value
}

main(): int {
    number: Number = Number{ value: 13 }
    value: Value = Value(&number)
    return value.get()
}
```

Methods are owned by their struct. `self` is read-only by default; a mutating
method is declared and called with visible `mut`:

```ace
mut Number.set(value: int) {
    self.value = value
}

mut number.set(21)
```

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
calls. `Type.method(...)` is available for a disambiguated direct call, but a
bare `Type.method` is not a function value.

Signings and methods are private by default. Put visibility first:
`pub Number signs Value` and `pub mut Number.set(value: int)`. A contract
over const backing storage may be read, but mutating dispatch through it is a
compile error.

### Explicit embedding

An `embed` field is ordinary named inline composition whose methods are promoted
one level:

```ace
Named: struct { name: string }

Named.name(): string {
    return self.name
}

Human: struct {
    embed named: Named,
}

human: Human = Human { named: Named { name: "Ada" } }
name: string = human.name()       // same method as human.named.name()
```

Fields are not promoted: the stored name remains `human.named.name`. A method
owned directly by `Human` takes precedence over a promoted method; two matching
methods from separate embedded fields are an ambiguity error. Promotion does
not recurse through another embedded field.

Contracts flow only from the base to the embedder. If `Named signs Value`, a
promoted exact method may satisfy an explicit `Human signs Value`; the reverse
never happens. The compiler dispatches through a fixed-offset receiver thunk,
with no allocation, object metadata, subtype relation, or runtime search.

## Current limits

- Type inference and implicit generic instantiation are not implemented.
- Pointer arithmetic is not implemented.
- Generic aliases are not implemented by the current compiler; generic structs
  are supported.
- Generic struct literals are not implemented; declare the explicitly
  instantiated type and initialize its fields.
- Checked-build sequence bounds, integer arithmetic, `noalias`/length,
  null-use, and allocator diagnostics are implemented and stripped in release.

The examples are covered by `tests/sema/docs/type_ok.ace`.
