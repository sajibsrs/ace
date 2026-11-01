# std.test

[Standard library](index.md)

`test` runs a table of test functions and reports each one. It allocates
nothing. A test is a `pub` function taking `t: *test.T` in a test file; the
runner generates its `main`:

```ace
import test

pub sums(t: *test.T) {
    t.equal(2 + 2, 4, @file, @line)
    t.equal("ace", "ace", @file, @line)
}
```

```
$ ace test sums.ace
== sums.ace
ok   sums
1 passed, 0 failed
1 module, 0 failed
```

A package names its test directory in `ace.pkg` (`test: test`), and
`ace pkg test [--debug | --release] [--dir=DIR] [FILE]` runs the `.ace` files
directly in it, or the one FILE among them, in sorted order. It works from
anywhere in the package, runs tests in the package root, and links the
package's dependencies and native inputs. A package without `test` has no tests,
and the command fails.

`ace test [--debug | --release] [--dir=DIR] PATH...` reads no manifest. Each
PATH is a file, or a directory whose direct `.ace` files run; `--dir` acts as a
prior `cd`, and tests run there. Only `std` and relative imports resolve.

File names carry no meaning. Subdirectories hold helpers and data and never
run. In a package, a test file cannot be imported, and a helper can be imported
only from inside the test directory. A library of test helpers is an ordinary
module or package that imports `test`. Each file becomes its own binary, so an
`assert` abort fails only that module, and the command exits 1 when any module
failed to build or run. Every non-method `pub` function is a test, in
declaration order: one with another signature is a type error at its
declaration. A test file may not define `main` and must have a test.

Every check takes the location to report as its last two arguments, written
`@file, @line`. These name where they are written, so a helper takes
`file: string, line: int` and forwards its caller's values.

- `T.equal(self, got, want, file, line): bool` compares `int`, `uint`, `usize`, `bool`,
  or `string` (exact bytes). On a mismatch it prints
  `file:line: got X, want Y`, marks the test failed, and returns false. The test keeps running, so return early on
  `false` when later checks depend on this one.
- `test.equal[E](t, got, want, file, line): bool` compares enum values the same way
  and prints members by name (`got east, want north`). It is a free generic
  function rather than a method because methods take no type parameters of
  their own; `E` must be an enum type.
- `T.fail(self, message, file, line)` fails the test, printing
  `file:line: message`.
- `run(table: []Case): int` runs the cases in order, each on a fresh `T`,
  prints `ok   name` or `FAIL name` after the case's check lines, then
  `N passed, M failed`. It returns 0 when every case passed and 1 otherwise,
  so `main` returns it as the exit code.
- `Case` is `(string, (t: *T): void)`. A test calls methods through the
  pointer: `t.equal(a, b, @file, @line)`.

Output goes to stdout. Nothing reports a file or line; the case name and
check index locate a failure.

## Design rationale

`ace test` writes the table into a generated `main` because function values
are not compile-time constants, so it cannot be a global. Discovery is by
signature on `pub` functions, as Go's `go test` does by name: every `pub`
function of a test file is a test, so a misspelled signature is an error, not
a silently skipped test. Builds run in a forked child per file so compiler
state never carries between modules.
