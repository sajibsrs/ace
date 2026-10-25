## Stdlib: `test`

29. `test` (implemented) — a table-driven test runner. `std/test.ace` imports
    only `fmt` and `mem` and allocates nothing.
    - A test is `pub name(mut t: test.T)` in a `<name>.test.ace` file whose
      `main` returns `test.run([("name", name), ...])`; the table is a literal
      in `main` because function values are not compile-time constants.
    - `T: struct` has private fields: whether a check failed and how many ran.
    - `T.equal(mut self, got, want): bool` overloads `int`, `uint`, `usize`,
      `bool`, and `string` (bytes, by `mem.equal`). A mismatch marks the test
      failed, prints `check N: got X, want Y` with N the test's 1-based check
      index, and returns false; the test keeps running.
    - `T.fail(mut self, message)` fails unconditionally as `check N: message`.
    - `Case: type = (string, (mut t: T): void)`. `run(table: []Case): int`
      runs cases in order on a fresh `T`, prints `ok   name` or `FAIL name`
      to stdout after each case's check lines, then `N passed, M failed`, and
      returns 0 when none failed, otherwise 1.
    - No file or line is reported: Ace has no caller-location builtin.
