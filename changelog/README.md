# Ace — Implementation History

The sole historical record of implemented compiler and language work. Entries
state what landed and the essential implementation approach; `spec/` contains
current normative rules, and Git preserves patch-level detail.

Search it with `grep -rn <topic> changelog/`; do not read it whole.

New entries go at the end of the file for their area, matching the issue
`area:` labels, as one `- ` bullet of 2–4 lines:

- `lang.md`: language semantics and syntax.
- `compiler.md`: frontend, checker, IR, backends, driver.
- `std.md`: standard library.
- `pkg.md`: first-party packages and `ace pkg`.
- `tooling.md`: `.acefmt`, editor support, tests, benchmarks, documentation.

`archive/` holds every entry recorded before the split, in its original
sections: `compiler.md`, `lang.md` (core language, types, memory),
`modules.md`, `std.md`, `corrections.md`, `frontier.md`.
