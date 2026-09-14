---
paths:
  - "Makefile"
  - "src/**/*"
  - "std/**/*"
  - "tests/**/*"
---

# Testing

- Run the smallest relevant checks first.
- Tests are grouped under `tests/`: lexer/parser tests parse; sema tests check
  diagnostics; emit and module tests compile/link/run; stdlib-module tests use a
  synthetic std tree; emit-errors fail during `-c`; driver tests exercise
  external linking.
- Sandboxes may deny network and loopback sockets, causing `net`, `poll`, `tcp`,
  and HTTP fixtures to fail. Confirm environmental restrictions before treating
  those failures as regressions.
