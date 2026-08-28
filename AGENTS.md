# AGENTS.md

Working rules for AI agents on this repository. Read this file fully before doing anything else.

## What this repository is
Ace is a systems programming language and, eventually, its compiler. The project is currently in the **design-complete phase** — no compiler code yet; three living design documents (philosophy, spec, grammar).

## Required reading, in order
1. `philosophy.md` — why Ace exists, the priority stack, the principles. **Closed**: do not change principles without explicit owner approval.
2. `spec.md` — the concrete language rules, carved incrementally. The Decision Queue at the bottom is the source of truth for status and what comes next.
3. `grammar.md` — EBNF for the carved surface, with lookahead/ambiguity notes for the implementer. Where it disagrees with `spec.md`, `spec.md` wins.

## How work proceeds (owner-established process)
- One topic at a time, closed one by one. Ask the owner clarifying questions in queue order; **decisions are made by the owner, never invented by the agent**.
- Every closed decision is recorded in `spec.md` (rule + rationale) and the Decision Queue is updated in the same edit.
- Philosophy level first, syntax carving second. Never carve syntax for a topic whose semantics are still open.
- Any contested design choice is resolved by the priority stack: **performance > compile time > safety > simplicity**.
- New sessions must resume from the Decision Queue, not restart settled debates.

## Invariants — never propose, generate, or assume these away
- Manual memory with explicit allocators. No GC, no borrow checker, no hidden allocation.
- Errors are values, checked with explicit branches. No exceptions, no propagation operator; panic = abort only.
- Fully explicit types, no inference. Explicit generic instantiation; no comptime, no trait solving.
- Assignment copies; parameters are by-reference — read-only unless `mut`, marked in the signature and at the call site; `noalias` params state non-overlap (debug-verifiable, release-free).
- Nullability is tracked in the type (`T?`); plain `T` is never null.
- Composition over inheritance; interface values are explicit; inheritance is rejected.
- Declarations are `name: type`; functions are keywordless with trailing returns; function values are typed by signature; no C pointer-to-function syntax.
- Single-pass compilation, recursive descent parsing, direct machine-code emission; no whole-program analysis.
- Safety lives in checked builds (bounds, overflow, allocator diagnostics); release strips checks.
- Control flow: no goto — labeled blocks with `break label`; `for-in` loops only (lowering to index loops over pointer+length); flow narrowing from explicit null checks; no fallthrough in switch.

## Conventions
- `[?]` marks undecided items, `[!][?]` controversial ones. Never resolve these markers yourself — only the owner resolves them.
- Spellings marked *provisional* may change during carving; their semantics may not.
- Start strict, relax later: propose new convenience as opt-in additions, never as reductions of explicitness.
- When design is contested, cite the relevant `philosophy.md` principle or priority — not general language-design folklore.

## Current status
- Philosophy phase: complete. Carving phase: complete — the full language surface is specified in `spec.md` (declarations, memory & allocators, errors, conditions & iteration, function ABI, generics, interfaces, imports/exports, names & scoping, inline assembly, stdlib).
- Provisional spellings are inventoried at the bottom of the Decision Queue in `spec.md`; semantics are final.
- Compiler implementation: **started**. Bootstrap language: C11 (clang, no dependencies) — fastest path to a self-hosting cycle; tcc-style direct emission. v1 target: AArch64 (Apple arm64).
  - [x] Lexer (`src/lexer.c`) — Go-style newline rule (newline token only after ident/literal/`)`/`]`/`}`/`?`/`return`/`break`/`continue`), `//` and `/* */` comments, hex/binary/underscore int literals, float literals with mandatory `.` before exponents, char/string literals with `\xNN` escapes (UTF-8 pass-through), full operator set incl. compound-assignment spellings (parser acceptance still pending owner decision), `..`/`...`, friendly errors (multi-error recovery, no-semicolon message).
  - [x] Parser (`src/parser.c`) — full carved surface → AST, dump via `--ast`: declarations (struct/interface/enum/alias/distinct/impl/extern incl. variadics, generics `[T]`), typed destructuring (`n: int, err: Err? = read()`), compound assignment (accepted, desugars at typecheck), comparison chains as one node, C precedence incl. shifts/bitwise, `?` binds loosest (`*T?` = nullable pointer), labeled blocks, asm bindings (in/out/clobbers, immediates), struct-literal suppression at block heads (Go-style), trailing commas everywhere.
   - [x] Typecheck (`src/sema.c`) — single-pass semantic walk in declaration order: two-namespace scopes (types/values), declaration-before-use with forward top-level calls resolved at EOF (fixup semantics), full type computation + layout for every type (size/align; niche optionals free, value optionals tagged, enum=i32, interface=16B), untyped literals with range checks, explicit casts only, mut discipline (read-only params/pointers, mut call-site marks), flow narrowing (`p != null`), method sugar + interface dispatch, impl structural conformance + named constructors, generic stamping per explicit instantiation (`bigger[int]` — body checked once per type tuple), destructuring vs multi-return, `size_of` builtin.
   - [ ] Direct AArch64 emitter (`src/emit.c`) → stdlib (`mem`, `sys`, `fmt`, `math`, `str`, `simd`). Core language codegen and source-level debugging are working; checked diagnostics and the stdlib remain.
     - [x] Pre-emitter decisions (recorded in spec.md): `main(): int`; Mach-O objects + system linker (`cc` driver); minimal DWARF line tables.
     - [x] Mach-O format pinned against clang reference objects (otool dumps): one nameless segment holding all sections, `r_length=2` for BRANCH26/PAGE21/PAGEOFF12 pairs (PAGEOFF12 pcrel=0), UNSIGNED `r_length=3`, locals/extdefs/undefs symbol order, section addrs continuous from 0; full AArch64 encoder set verified against `as` disassembly.
     - [x] Sema annotations for codegen: `Node.ty`/`Node.sym` (SType*/Sym* via `src/sema.h`) — expression types on every node (literals materialize at coercion), resolved symbols on idents/call targets/binders, enum-member indices, `size_of` constant, forward-call syms at EOF, `sema_stamp_body()` for per-stamp re-annotation, mut args must be lvalues. All existing tests green.
     - [x] Mach-O writer + initial codegen (chunk 4) — one nameless `LC_SEGMENT_64`, `__TEXT,__text`, relocatable `__DATA_CONST,__const`, `__DATA,__data`/`__bss`, symbol/relocation tables, `_main`, constant loading, internal-call fixups, and `-c`/`-o` driver modes. **Landmark one: hello world compiled and run** (`tests/emit/hello_min.ace` → exit 42).
     - [x] Full carved-surface codegen (chunk 5) — integer/floating expressions, casts, comparisons and short-circuiting, calls and internal CC (mut pointers, register aggregates, sret), locals/globals, multi-return, arrays/structs/slices, range and sequence loops, switch, labeled exits, generic stamps, interface vtables/dispatch, extern calls, and initial asm (`nop`/`svc`/`brk`). Covered by focused `tests/emit` executables; the normal and ASan/UBSan suites pass.
     - [x] Minimal DWARF line tables (chunk 6) — DWARF v4 `__debug_line` rows for function and statement PCs, plus the minimal compile-unit/function discovery metadata Darwin needs to retain them through linking. Local relocations track final text addresses; `dwarfdump`, `dsymutil`, and LLDB resolve `hello_min.ace` source. No variable/type DIEs or unwind data.
     - [x] First stdlib output slice (chunk 7) — `std/fmt.ace` exports allocation-free `fmt.print(string): int`: exact bytes, no implicit newline, bytes-written/`-1` result (owner approved). Bare `import fmt` is resolved from beside the compiler and merged ahead of the root module under a private prefix; multi-file DWARF rows retain the correct source file. `tests/emit/welcome.ace` compiles and prints exactly `Welcome Ace!\n`. This exposed and fixed two-word aggregate call-address clobbering and reversed incoming aggregate frame layout.
     - [ ] Module loader follow-up — current resolver intentionally handles only bare `fmt` (including aliases) in one merged compilation unit. Still missing: arbitrary std modules, local path imports, recursive dependency ordering, cycle diagnostics, qualified imported types/globals, complete privacy enforcement, and separate per-module incremental objects.
     - [ ] Stdlib follow-up — add `fmt.println`, then `fmt.printf`; integrate rich `Err` once the error layer exists; input remains lower priority than output. Other v1 modules (`sys`, `mem`, `math`, `str`, `simd`) are not implemented.
     - [ ] Next (chunk 8): harden the general module loader or add checked-build runtime diagnostics before growing formatting.
- Pre-Implementation Checklist `[?]` items: **all decided** and recorded in spec.md (`int`/`uint` = platform word, compound assignment, enum, `main(): int`, Mach-O objects + system linker, DWARF line tables). Nothing left gating the emitter.
- Build: `make` (produces `./ace`). Test: `make test` (fixtures: `tests/lexer/` + `tests/parser/` parse-only via `--ast`, `tests/sema/` full check, `tests/emit` compile+link+run via `// expect-exit: N` lines; `*.ok.ace` must pass, `*.err.ace` must fail). Modes: `./ace file.ace` = full check; `--ast` = parse only; `--tokens` = lex only; `-c` = emit object `<base>.o`; `-o NAME` = emit + link via `cc`.
