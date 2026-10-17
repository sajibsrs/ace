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

- Use accurate lexical pairs for relationships (`watch`/`unwatch`,
  `add`/`remove`); accuracy beats cosmetic symmetry.
- Put OS-specific standard-library code behind `std/<module>/mod.ace` in
  `std/<module>/os/<target>.ace`. Reserve `backend` for compiler/code-generation
  backends.
- By convention, keep a directory module's `mod.ace` at its root and its other
  implementation files under `std/<module>/inc/`; `mod.ace` remains the sole
  external import surface.
- A function that only builds or releases memory is named for it:
  `Type.new(...)` (free form `x_new`) when the value owns heap memory that
  must be released, whoever allocates it (`mem` or a C library); a matching
  `Type.wipe(mut self)` (`x_wipe`) releases it. `Type.init(...)` (`x_init`)
  when there is nothing to release, as for a value over caller storage. A
  function that does work on its input keeps its verb (`parse`, `compile`).
  A type that has a `wipe` owns memory, and all its constructors agree on
  that. `wipe` is safe on any value of the type, so a result can be deferred
  without checking which constructor made it. A verb function's doc comment
  says what its result owns.
- A property's accessors are one overloaded name, told apart by signature:
  `color()` reads it and `color(value)` sets it, never `get_color`/
  `set_color`. `get`/`set` remain ordinary method names for keyed access
  (`Map.get(key)`/`set(key, value)`).
- Start strict and add conveniences later.
- Release a resource that must be freed on every function exit with `defer`,
  placed immediately after its acquisition, before the error check (release
  is a no-op on `null`, so the failure path stays covered). Keep explicit
  release only where ownership transfers on success, since `defer` also runs
  on the success path.
- Module-level `const` declarations use all uppercase (`BACKLOG`). A
  same-named local (e.g. a parameter) may still shadow it per the ordinary
  nearest-binding rule; that is not an error, so no `DEFAULT_`-style prefix.
- `.acefmt` is normative; update it with formatting-rule changes.
- File names are nouns, matching their own import alias (`parsing.ace` as
  `parsing`, not `parse.ace`).
- Keep `demo/`, `bench/`, and `tests/` structurally minimal: a plain `.ace`
  file when `std` suffices, a directory of exact `.ace` path imports when it
  needs several files. Add an `ace.pkg` only to depend on a `pkg/` package or
  to test the package system, and a `mod.ace` only to test directory modules.
- Place each benchmark in the `bench/` section whose code it measures:
  `compiler/` (the compiler's own speed), `core/` (generated code for
  language features), `std/` (standard-library operations), or `package/`
  (first-party packages; not `pkg/`, which is reserved for dependency
  lookup). Benchmarks never live under `tests/`.
- `selfhost/` holds the self-hosted compiler written in Ace, flat (no
  subdirectories); `src/` remains the C bootstrap compiler. `selfhost/check.sh`
  diffs its output against `./ace`; it is not part of `make test`.
- Put Ace programs written to find language flaws under `experiment/<name>/`,
  structured like `demo/`. They are not part of `make test`; an experiment may
  carry its own check script.

## Scoped rules

Path-specific guidance lives in `.claude/rules/`:

- `language-boundaries.md`: fixed language and compiler design boundaries.
- `documentation.md`: documentation and editor-support requirements.
- `testing.md`: test selection, organization, and environment caveats.

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
