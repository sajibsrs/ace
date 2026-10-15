# std.sort

[Standard library](index.md)

`sort` orders a slice in place. It allocates nothing and cannot fail. Type
arguments are explicit, as everywhere:

```ace
import sort

main(): int {
    values: [5]int = [4, 1, 3, 5, 2]
    sort.unstable[int](mut values[0..5])   /* 1 2 3 4 5 */
    return 0
}
```

- `unstable[T](items: mut []T)` orders `items` ascending by `<`; `T` must
  support it.
- `unstable[T](items: mut []T, less: (T, T): bool)` orders by `less`, which
  returns true when its first argument must come before its second. `less` must
  be a strict weak ordering.
- `sorted[T](items: []T): bool` and `sorted[T](items: []T, less): bool` report
  whether no element comes before its predecessor.

```ace
by_age(a: Person, b: Person): bool { return a.age < b.age }

sort.unstable[Person](mut people[0..count], by_age)
```

`unstable` may reorder equal elements. Equal integers or floats are
indistinguishable, but records sorted by one key can change the relative order
of ties; order by every field that matters, or wait for a stable sort. The
comparator is a plain function: it cannot capture variables, so state it needs
must come from the elements themselves.

The algorithm is introsort: quicksort with a median-of-three Hoare partition,
heapsort once the recursion is deeper than twice the base-2 logarithm of the
length, and insertion sort for ranges under 16 elements. The worst case is
O(n log n) time with O(log n) stack.

## Design rationale

Why the rules in `spec/std/collections.md` (Stdlib #26) are shaped as they are.

- Stdlib #26: the name states the guarantee instead of naming the algorithm,
  so the implementation can change and callers must choose between `unstable`
  and a later `stable`. A sort that needs no scratch memory cannot fail, which
  keeps sorting numbers free of an `Error` result.
- The `<` overload passes a function value to the shared algorithm, so each
  comparison is an indirect call. A copy specialized on `<` sorted 2,000,000
  random `int`s in 205 ms against 252 ms (about 1.2x) in release mode on the
  development machine; the single shared algorithm was kept to avoid
  duplicating about 100 lines.
