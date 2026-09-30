---
paths:
  - "docs/**/*"
  - "editors/vscode/**/*"
  - "spec/**/*"
  - "GRAMMAR.md"
---

# Documentation and editor support

- Keep `docs/` consistent with `spec/`. Compiler-visible changes should update
  the relevant page and, when practical, add a checked example under
  `tests/sema/docs/` or an executable fixture.
- Update `editors/vscode/` for syntax or editor-visible changes when practical;
  otherwise note the remaining work in its README.
- Documentation-only changes require diff review and `git diff --check`, not the
  full suite unless behavior is uncertain.
