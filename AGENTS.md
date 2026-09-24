# AGENTS.md

Rules for AI agents working on Ace, a systems language and bootstrap C11
compiler. `SPEC.md` is normative; `CHANGELOG.md` is the sole implementation
history.

## Before working

- Read this file first, then only the relevant source sections.
- Design or carving: read `PHILOSOPHY.md`, `SPEC.md`, and the active Decision
  Queue.
- Compiler work: read `SPEC.md` and `GRAMMAR.md`, then search `CHANGELOG.md` and
  the implementation by topic.
- Standard-library work: inspect the relevant `std/` modules and normative
  `SPEC.md` surface. Reuse existing operations; do not duplicate parsing,
  memory, text, or platform utilities. Prefer explicit generics, type-owned
  methods, or overloads over free helpers when ownership is clearer.
- Planning: read the active Decision Queue and relevant `TODO.md` entries.
- `docs/` describes implemented behavior; do not use it as design authority.

## Process

- Work on one topic at a time in Decision Queue order.
- Implement every code change (`src/`, `std/`, `pkg/`, `tests/`, `bench/`)
  directly in the working tree on `master`, never in a throwaway prototype, and
  leave it uncommitted. Implement, test, and measure there against `HEAD`, then
  present the result with viable alternatives for the owner's review. This
  holds even when the change is already agreed in principle (e.g. noted in
  `TODO.md`). Propose language-design decisions (semantics, syntax) before
  implementing them.
- The owner resolves `[open]` and `[controversial]` items and removes those
  markers. Provisional spelling may change; semantics may not.
- Confirm each step before proceeding. Implement only what was explicitly
  asked; do not add unrequested options, flags, or behavior (e.g. CLI-argument
  parsing nobody asked for).
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
  estimate) and state it when presenting the change. If the gain is
  negligible, drop the change: the return on investment is low, and every
  added line is more surface for bugs and more to extend and maintain.
- Record current rules and rationale in `SPEC.md`, keeping only active decisions
  in its queue. Record implemented behavior and its essential approach once,
  briefly, in `CHANGELOG.md`. Keep `SPEC.md`, `CHANGELOG.md`, `GRAMMAR.md`, and
  `TODO.md` entries minimal but sufficient; full detail belongs only in
  `docs/`. `SPEC.md` and `TODO.md` entries state current rules and open
  questions only; what was removed, replaced, or how something got here is
  `CHANGELOG.md`'s history alone, not restated elsewhere.
- `TODO.md` is non-normative. Promote entries through the Decision Queue and
  update the survey in the same edit. It reflects present state only; remove
  resolved entries outright, no resolution notes.
- Update `SPEC.md`, `CHANGELOG.md`, `TODO.md`, and `docs/` in the same commit
  as the change they describe, not a follow-up.
- Split work likely to exceed 100k tokens into 2–3 owner-approved steps; each
  step must compile and pass relevant tests.
- Commit only after the owner has reviewed the change and explicitly
  permitted the commit, as one commit per topic. Keep a patch of each measured
  stage outside the repository; park an unfinished topic with `git stash`.
  Every commit message is exactly one
  imperative subject line, full stop — no body paragraph, no rationale, no
  bullet list, even when the change feels like it needs explaining. That
  explanation belongs in `CHANGELOG.md`, not the commit.

## Conventions

- Use accurate lexical pairs for relationships (`watch`/`unwatch`,
  `add`/`remove`); accuracy beats cosmetic symmetry.
- Put OS-specific standard-library code behind `std/<module>/mod.ace` in
  `std/<module>/os/<target>.ace`. Reserve `backend` for compiler/code-generation
  backends.
- By convention (not a requirement), keep a directory module's `mod.ace` at its
  root and its other implementation files under `std/<module>/inc/`. This is
  organizational only; `mod.ace` remains the sole external import surface
  regardless of where its implementation files sit.
- A constructor carries the `_new` suffix exactly when a matching `_wipe`
  releases what it returns (`parser_new`/`parser_wipe`). A constructor with
  nothing to release takes the bare name. A type-owned pair instead uses
  `Type::new(...)`/`mut Type.wipe()` (associated function and method).
- Start strict and add conveniences later.
- Release a resource that must be freed on every function exit with `defer`,
  placed immediately after its acquisition, before the error check (release
  is a no-op on `null`, so the failure path stays covered). Keep explicit
  release only where ownership transfers on success, since `defer` also runs
  on the success path.
- Module-level `const` declarations use all uppercase (`BACKLOG`). A
  same-named local (e.g. a parameter) may still shadow it per the ordinary
  nearest-binding rule; that shadowing is not an error, so there is no need for
  a `DEFAULT_`-style prefix to dodge a collision that cannot occur.
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
- Check sema: `./ace file.ace`
- Show successful-run statistics: `./ace --report file.ace`
- Compile object: `./ace -c file.ace`
- Compile binary: `./ace -o output file.ace`
