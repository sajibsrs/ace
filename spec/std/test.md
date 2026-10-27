## Stdlib: `test`

29. `test` (implemented) — a table-driven test runner. `std/test.ace` imports
    only `fmt` and `mem` and allocates nothing.
    - A test is `pub name(t: *test.T)` in a `<name>.test.ace` file.
      `ace test PATH...` builds each such file (a file, or every one under a
      directory, skipping `fixtures/` and dot directories, in sorted order)
      with a generated `main` returning `test.run([("name", name), ...])` over
      its non-method `pub` functions in declaration order, links one binary
      per file, runs it, and exits 1 if any build or run failed. A `pub`
      function of another signature is a type error at its declaration; a
      user `main`, or a file with no `pub` function, is a compile error.
    - A test file inside a package (nearest `ace.pkg` at or above its
      directory) resolves that package's dependencies and links their native
      inputs, as `ace pkg build` does; outside any package only `std` and
      relative imports resolve.
    - Importing a `*.test.ace` file is a compile error.
    - `T: struct` has private fields: whether a check failed and how many ran.
    - `T.equal(self, got, want): bool` overloads `int`, `uint`, `usize`,
      `bool`, and `string` (bytes, by `mem.equal`). A mismatch marks the test
      failed, prints `check N: got X, want Y` with N the test's 1-based check
      index, and returns false; the test keeps running.
    - `equal[E](t: *T, got: E, want: E): bool` compares enum values with
      `==` and prints them by `.name` (Type System #14), as `T.equal` does.
    - `T.fail(self, message)` fails unconditionally as `check N: message`.
    - `Case: type = (string, (t: *T): void)`. `run(table: []Case): int`
      runs cases in order on a fresh `T`, prints `ok   name` or `FAIL name`
      to stdout after each case's check lines, then `N passed, M failed`, and
      returns 0 when none failed, otherwise 1.
    - No file or line is reported: Ace has no caller-location builtin.
