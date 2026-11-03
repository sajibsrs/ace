# AGENTS.md

Rules for AI agents working on Ace, a systems language and bootstrap C11
compiler. `spec/` is normative; `changelog/` is the sole implementation
history.

## Before working

- Read this file first, then only the relevant source sections.
- Read `spec/README.md`, then only the chapters the task touches; its table
  maps each section (and references such as "Memory #15") to a file. Never
  read every chapter.
- Design or carving: read `PHILOSOPHY.md`, `spec/queue.md`, and the affected
  chapters.
- Compiler work: read `spec/compiler.md`, the chapters for the feature
  involved, and `GRAMMAR.md`, then grep `changelog/` and the implementation
  by topic.
- When the `ace` MCP tools are available (`ace-mcp`, registered in
  `.mcp.json`; see `docs/mcp.md`), prefer them to grep and reading files:
  `std_lookup` for a std signature and doc comment, `spec_section` for a
  reference such as "Memory #15", `definition`/`hover`/`references` for a
  name in the code, `examples` for real uses, and `diagnostics` to check a
  file. Rebuild with `./ace -o ace-mcp mcp/main.ace` after changing
  `mcp/`, `lsp/`, or `selfhost/`.
- Standard-library work: inspect the relevant `std/` modules and their
  `spec/std/` chapter (`spec/std/README.md` holds the shared rules). Reuse
  existing operations; do not duplicate parsing, memory, text, or platform
  utilities. Prefer explicit generics, type-owned methods, or overloads over
  free helpers when ownership is clearer.
- Planning: read `spec/queue.md` and the relevant GitHub issues
  (`gh issue list -m <milestone>`, `-l <label>`; see `TODO.md`).
- `docs/` describes implemented behavior; do not use it as design authority.

## Process

- Work on one topic at a time in Decision Queue order.
- Target darwin-arm64 only. x64 (linux, windows) work is paused: add or extend
  no x64 lowering, tests, or limitations in any topic, and leave existing x64
  code as it is. It resumes as its own separate effort once darwin is complete.
- Implement every code change (`src/`, `std/`, `pkg/`, `tests/`, `bench/`)
  directly in the working tree on `master`, never in a throwaway prototype, and
  leave it uncommitted. Implement, test, and measure there against `HEAD`, then
  present the result with viable alternatives for the owner's review. This
  holds even when the change is already agreed in principle (e.g. an open
  issue). Propose language-design decisions (semantics, syntax) before
  implementing them.
- The owner resolves `[open]` and `[controversial]` items and removes those
  markers. Provisional spelling may change; semantics may not.
- Confirm each step before proceeding. Implement only what was explicitly
  asked; do not add unrequested options, flags, or behavior (e.g. CLI-argument
  parsing nobody asked for).
- Prioritize correctness > performance > convenience, in code and in
  resolving issues alike: never trade a correct result for speed, nor speed
  for convenience.
- Compilation is fast enough: add a pass or layer that speeds up generated
  code even at some compile-time cost, and state that cost with the gain.
  Never slow compilation for its own sake.
- Order decisions as philosophy, semantics, syntax, then implementation. For
  contested design, cite `PHILOSOPHY.md` and prioritize
  performance > simplicity > modularity > compile time > safety.
- Judge a proposed change by the value it adds against that priority stack,
  never by whether it breaks with what's already decided or implemented.
  "That's not how it's currently done" is not a counterargument on its own.
- While a change is under review, run only the checks it touches (one
  package's `tests/run.sh`, a demo, a doc example). Run the full suites
  (`make test`, `make test-packages`) only after the owner has approved the
  design and reviewed the change, before the commit; they are slow.
- Measure an optimization's gain against `HEAD` (several runs; a hand-edited
  disassembly or microbenchmark against a saved baseline for a quick first
  estimate) and state it when presenting the change. Drop a negligible gain:
  every added line is more surface for bugs and maintenance.
- Judge a change by whether it is genuinely needed, not by whether existing
  code or tests reach it: a new feature or fix has no callers yet. Keep a
  needed change and add the test that covers it.
- Record current rules in the `spec/` chapter they belong to, keeping only
  active decisions in `spec/queue.md`. Rationale stays in `spec/` only while a
  decision is open; an implemented rule's rationale goes in the "Design
  rationale" section of its `docs/` page. Record implemented behavior and its
  essential approach once, briefly, as a 2–4 line entry in `changelog/`. Keep
  `spec/`, `changelog/`, `GRAMMAR.md`, and issue bodies minimal but sufficient;
  full detail belongs only in `docs/`. `spec/` entries and issues state current
  rules and open questions only; what was removed, replaced, or how something
  got here is `changelog/`'s history alone, not restated elsewhere.
- Issues are non-normative. Promote one through the Decision Queue and update
  it in the same change. An issue reflects present state only: edit its body
  rather than appending history, and close it when resolved (`not planned`
  when dropped). Label every issue with one kind (`bug`, `feature`, `task`),
  one area, a status label from `TODO.md` when one applies, a target label
  when platform-specific, and a milestone.
- Update `spec/`, `changelog/`, and `docs/` in the same commit as the
  change they describe, not a follow-up, and open, edit, or close the
  affected issues when that commit lands.
- Split work likely to exceed 200k tokens into owner-approved steps; each step
  must stay under 200k tokens, compile, and pass relevant tests.
- Commit only after the owner has reviewed the change and explicitly
  permitted the commit, as one commit per topic. Keep a patch of each measured
  stage outside the repository; park an unfinished topic with `git stash`.
  Every commit message is exactly one
  imperative subject line — no body paragraph, no rationale, no bullet list,
  even when the change feels like it needs explaining; a `Co-Authored-By`
  trailer is allowed. That explanation belongs in `changelog/`, not the commit.

## Conventions

- Coding rules (naming, resources, comments, layout) live in `spec/style.md`;
  fetch an item with `spec_section` (e.g. "Naming #2").
- Start strict and add conveniences later.
- `selfhost/check.sh` diffs the selfhost's output against `./ace`; it and
  `experiment/` check scripts are not part of `make test`.

## Scoped rules

Path-specific guidance lives in `.claude/rules/`:

- `language-boundaries.md`: fixed language and compiler design boundaries.
- `documentation.md`: documentation and editor-support requirements.
- `testing.md`: test selection, organization, and environment caveats.
- `binbox.md`: building, icons, toolbar, and diagram checks for `demo/binbox`.

## Commands

- Build compiler: `make`
- Run test suite: `make test`
- Lex file: `./ace --tokens file.ace`
- Parse file: `./ace --ast file.ace`
- Print IR: `./ace --ir file.ace`
- Check sema: `./ace file.ace`
- Show successful-run statistics: `./ace --report file.ace`
- Compile object: `./ace -c file.ace`
- Compile binary: `./ace -o output file.ace`
