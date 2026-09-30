# Tooling

- Split `CHANGELOG.md` into `changelog/` by area (`lang`, `compiler`, `std`,
  `pkg`, `tooling`), moving every earlier entry verbatim into
  `changelog/archive/` under its original section so sessions grep history
  rather than read it; new entries are 2–4 lines.
- Moved the rationale of implemented rules out of `spec/` (14 chapters, 20 KB)
  verbatim into a "Design rationale" section of the matching `docs/` page;
  `spec/` keeps rationale only for open decisions.
