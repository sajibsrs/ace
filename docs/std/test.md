# std.test

[Standard library](index.md)

`test` runs a table of test functions and reports each one. It allocates
nothing. A test is a `pub` function taking `t: *test.T` in a file named
`<name>.test.ace`; `ace test` generates its `main`:

```ace
import test

pub sums(t: *test.T) {
    t.equal(2 + 2, 4)
    t.equal("ace", "ace")
}
```

```
$ ace test sums.test.ace
== sums.test.ace
ok   sums
1 passed, 0 failed
1 module, 0 failed
```

`ace test [--debug | --release] PATH...` takes files and directories; a
directory runs every `*.test.ace` beneath it in sorted order, skipping
`fixtures/` (test inputs, including deliberately invalid sources) and dot
directories. Each file becomes its own binary, so an `assert` abort fails
only that module, and the command exits 1 when any module failed to build or
run. Every non-method `pub` function is a test, in declaration order: one with
another signature is a type error at its declaration. A test file may not
define `main`, must have a test, and cannot be imported.

- `T.equal(self, got, want): bool` compares `int`, `uint`, `usize`, `bool`,
  or `string` (exact bytes). On a mismatch it prints
  `check N: got X, want Y`, where N counts the test's checks from 1, marks the
  test failed, and returns false. The test keeps running, so return early on
  `false` when later checks depend on this one.
- `test.equal[E](t, got, want): bool` compares enum values the same way
  and prints members by name (`got east, want north`). It is a free generic
  function rather than a method because methods take no type parameters of
  their own; `E` must be an enum type.
- `T.fail(self, message)` fails the test, printing `check N: message`.
- `run(table: []Case): int` runs the cases in order, each on a fresh `T`,
  prints `ok   name` or `FAIL name` after the case's check lines, then
  `N passed, M failed`. It returns 0 when every case passed and 1 otherwise,
  so `main` returns it as the exit code.
- `Case` is `(string, (t: *T): void)`. A test calls methods through the
  pointer: `t.equal(a, b)`.

Output goes to stdout. Nothing reports a file or line; the case name and
check index locate a failure.

## Design rationale

`ace test` writes the table into a generated `main` because function values
are not compile-time constants, so it cannot be a global. Discovery is by
signature on `pub` functions, as Go's `go test` does by name: every `pub`
function of a test file is a test, so a misspelled signature is an error, not
a silently skipped test. Builds run in a forked child per file so compiler
state never carries between modules.
