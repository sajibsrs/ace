# AGENTS.md

Operating rules for AI agents in this repository. `SPEC.md` is normative for the
language. `CHANGELOG.md` is the sole implementation-history record.

## Repository

Ace is a systems programming language and its bootstrap C11 compiler. The
language design is complete; compiler implementation and additive carving are
active.

## Read by task

1. Read this file first.
2. For design or carving, read `PHILOSOPHY.md`, the relevant `SPEC.md` sections,
   and the active Decision Queue.
3. For compiler work, read the relevant `SPEC.md` and `GRAMMAR.md` sections,
   then search `CHANGELOG.md` and the implementation by topic.
4. For planning, read the active Decision Queue and relevant `TODO.md` entries.
5. Read an entire document only when the task spans it. Human documentation in
   `docs/` is for implemented behavior, not agent startup context.

## Process

- Work on one topic at a time in Decision Queue order. The owner makes language
  decisions; agents do not resolve `[open]` or `[controversial]` items.
- Propose before changing code: describe the issue and recommended solution,
  then obtain owner permission. If options are viable, present them with a
  recommendation.
- Philosophy precedes semantics; semantics precede syntax and implementation.
  Resolve contested designs by: performance > simplicity > modularity > compile
  time > safety.
- Record current normative rules and rationale in `SPEC.md`; keep only active
  decisions in its queue. Record what was implemented and its essential approach
  once, briefly, in `CHANGELOG.md`.
- `TODO.md` is a non-normative survey. Promote items only through the Decision
  Queue, updating the survey in the same edit.
- Split work likely to exceed 100k tokens into 2–3 owner-approved steps. Each
  step must compile and pass its relevant tests.

## Fixed design boundaries

Do not propose or assume away these decisions; follow `SPEC.md` for exact rules.

- Manual memory through one process-wide strategy exposed by `mem`; no allocator
  values, GC, borrow checking, ownership, or hidden allocation.
- Errors are values with explicit branches; no exceptions or propagation
  operator. Panic aborts.
- Types are explicit; generic instantiation is explicit; no comptime or trait
  solving. Local `:=` is narrowly specified type elision.
- Assignment copies. Parameters are by reference, read-only unless `mut`; the
  signature and call site show mutation. `noalias` states non-overlap.
- Every type has an all-bits-zero representation. Pointer/handle zero is `null`;
  no optional type operator. Globals are zeroed; locals require definite
  initialization before use.
- Composition and explicit contract values replace inheritance.
- Declarations use `name: type`; functions are keywordless with trailing
  returns; function values use signature types.
- The compiler uses recursive descent, single-pass compilation, and direct
  machine-code emission; no whole-program analysis.
- Safety is cheap local checking plus removable checked-build diagnostics.
- No `goto`; use labeled blocks/loops. Iteration, switch, and `fallthrough`
  follow `SPEC.md` exactly.

## Conventions

- Relationship-forming operations use accurate lexical pairs when available
  (`watch`/`unwatch`, `add`/`remove`). Accuracy beats cosmetic symmetry.
- `[open]` is undecided; `[controversial]` is contested. Only the owner removes
  either marker. Provisional spellings may change; their semantics may not.
- Start strict and relax later through additive conveniences.
- Cite `PHILOSOPHY.md` when resolving contested design, not general folklore.
- `.acefmt` is the normative Ace formatting configuration. Update it with any
  formatting-rule change.
- Do not commit unless explicitly asked. Commit messages are one imperative
  subject line with no body.

## Documentation and editor support

- `docs/` describes only implemented behavior and must agree with `SPEC.md`.
  Compiler-visible changes update the relevant page and, when practical, a
  compiler-checked example under `tests/sema/docs/` or an executable fixture.
- Syntax or editor-visible changes update `editors/vscode/` when practical;
  otherwise record the remaining work in its README.
- Documentation-only changes require diff review and `git diff --check`, not the
  full suite unless compiler behavior is genuinely uncertain.

## Source index

- Rules and active decisions: `SPEC.md`
- Formal syntax and parser notes: `GRAMMAR.md`
- Principles and priority: `PHILOSOPHY.md`
- Implemented surfaces and essential approach: `CHANGELOG.md`
- Uncommitted future candidates: `TODO.md`
- Formatting: `.acefmt`

## Build and test

- Build: `make` (produces `./ace`). Full suite: `make test`.
- `./ace file.ace`: check; `--tokens`: lex; `--ast`: parse; `--report`:
  successful-run statistics; `-c`: object; `-o NAME`: emit and link.
- Tests are feature-grouped under `tests/`: lexer/parser are parse-only, sema
  checks diagnostics, emit and modules compile/link/run, stdlib-module tests use
  a synthetic std tree, emit-errors fail during `-c`, and driver tests exercise
  external linking.
- Run the smallest relevant checks first. Follow the documentation-only rule
  above when no executable behavior changed.
