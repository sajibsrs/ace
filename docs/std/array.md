# std.array

[Standard library](index.md)

`array` provides a growable sequence of elements over the process allocator.
An `Array[T]` holds `len` live elements at the front of `items`; `items.len` is
its capacity. The canonical zero value and the empty literal `Array[T]{}` are
valid empty arrays holding no storage, so no constructor is required and the
first `push` allocates. Growth goes through `mem.grow`, which doubles storage.
Type arguments are explicit, as everywhere: `array.Array[int]`,
`array.Array[int].new(8)`.

- `Array[T].new(capacity: usize): (Array[T], Error)` creates an empty array
  with room for `capacity` elements. Zero allocates nothing. Failure returns
  `ArrayErr.allocation`.
- `Array[T].filled(count: usize, value: T): (Array[T], Error)` creates
  `count` live copies of `value` in one zeroed allocation, writing elements
  only when `value` is not `blank(T)`, so a zero fill costs one `mem.zalloc`.
  `T` must support `==`; a blank fill of any type is `resize` on an empty
  array. Failure returns `ArrayErr.allocation`.
- `wipe()` releases the storage and leaves the array empty.
- `push(value: T): Error` appends, growing when full. Failure returns
  `ArrayErr.allocation` and leaves the array unchanged.
- `reserve(count: usize): Error` ensures capacity for at least `count` elements
  in total; it never shrinks.
- `pop(): (T, Error)` removes and returns the last element. An empty array
  returns `blank(T)` and `ArrayErr.empty`.
- `get(index: usize): T` and `set(index: usize, value: T)` access a live
  element. `get` also takes an `int` index, so an `int` loop variable or id
  needs no cast; a negative index is a bounds violation. `set` has no `int`
  form: a second overload would type an untyped literal value as `int`
  (Syntax #6), breaking `xs.set(i, 0)` on an `Array[u32]`.
- `view(): []T` is the live elements as a slice. It aliases the storage and is
  invalidated by any call that grows the array.
- `view(self): []T`, called as `xs.view()`, is the same slice
  writable in place: `live[i].field = v` edits an element without copying it
  out and back through `get`/`set`.
- `clear()` drops every live element and keeps the storage.
- `truncate(count: usize)` drops every live element from `count` onward and
  keeps the storage.
- `resize(count: usize): Error` sets the length to `count`. Growing appends
  `blank(T)` elements, so an array can be sized in one call instead of one
  `push` at a time; an array without storage gets it already zeroed from
  `mem.zalloc`. Shrinking keeps the storage, like `truncate`, but any `count`
  is valid. Failure returns `ArrayErr.allocation` and leaves the array
  unchanged.

Access is checked against `len`, not
capacity: `get` and `set` at an index at or beyond `len`, and `truncate` past
`len`, are bounds violations in debug builds and unchecked in release builds.

```ace
import array

items: array.Array[int] = array.Array[int]{}
defer items.wipe()

err: Error = items.push(42)

if err != null {
    /* allocation failed; items is unchanged */
}

last: int
last, err = items.pop()   /* 42, or 0 and ArrayErr.empty when empty */
```

Element types are bare named types (Generics #12), so an array of slices or of
another generic instantiation needs a named alias first. There is no
`insert` or single-element `remove`, sorting, or search; nothing copies
elements implicitly.
