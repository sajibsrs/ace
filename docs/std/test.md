# std.test

[Standard library](index.md)

`test` runs a table of test functions and reports each one. It allocates
nothing. A test takes `mut t: test.T` and checks values through it; a file
named `<name>.test.ace` lists its tests in `main`:

```ace
import test

pub sums(mut t: test.T) {
    mut t.equal(2 + 2, 4)
    mut t.equal("ace", "ace")
}

main(): int {
    return test.run([("sums", sums)])
}
```

```
ok   sums
1 passed, 0 failed
```

- `T.equal(mut self, got, want): bool` compares `int`, `uint`, `usize`, `bool`,
  or `string` (exact bytes). On a mismatch it prints
  `check N: got X, want Y`, where N counts the test's checks from 1, marks the
  test failed, and returns false. The test keeps running, so return early on
  `false` when later checks depend on this one.
- `T.fail(mut self, message)` fails the test, printing `check N: message`.
- `run(table: []Case): int` runs the cases in order, each on a fresh `T`,
  prints `ok   name` or `FAIL name` after the case's check lines, then
  `N passed, M failed`. It returns 0 when every case passed and 1 otherwise,
  so `main` returns it as the exit code.
- `Case` is `(string, (mut t: T): void)`. Every `mut self` call carries the
  call-site mark: `mut t.equal(a, b)`.

Output goes to stdout. Nothing reports a file or line; the case name and
check index locate a failure.

## Design rationale

The table is written by hand in `main` because function values are not
compile-time constants, so it cannot be a global. The `N passed, M failed`
line makes a test left out of the table visible as a lower count. Discovering
`pub` test functions and generating `main` is a toolchain step (`ace test`)
that only automates the `test.run` call, so test files written now need no
change when it lands.
