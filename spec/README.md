# Ace — Language Specification

Concrete rules and syntax, split into chapters by topic. The why behind every
rule lives in PHILOSOPHY.md (closed). The formal grammar for this surface lives
in GRAMMAR.md. These files are carved topic by topic and are the single ledger
of language rules and decision records (rule + rationale); implementation
history lives in changelog/.

A reference such as "Memory #15" names item 15 of the Memory section; the table
gives each section's file. Stdlib items keep one numbering across their files.

| File | Sections |
|---|---|
| `queue.md` | Decision Queue (active and trigger-gated) |
| `syntax.md` | Source, Syntax & Parsing, Names & Scoping |
| `compiler.md` | Compiler, Functions & ABI, Inline Assembly |
| `memory.md` | Memory, Values & Passing, Mutability, Zero, Null & Absence |
| `safety-errors.md` | Safety & Debug Builds, Errors |
| `types.md` | Type System, Scalar-backed Bit Fields, Abstraction, Tagged Unions, Generics |
| `control-flow.md` | Conditions, Iterations, Deferred calls, Sequences |
| `modules-packages.md` | Modules, Package System, C Interop |
| `std/README.md` | Stdlib: conventions, #1–9 and #21 |
| `std/sys.md` | Stdlib #10 `sys`, #11 `time`, #24 `exec` |
| `std/fmt.md` | Stdlib #12 formatted output |
| `std/net.md` | Stdlib #13 `net`, #14 `poll`, #15 `tcp` |
| `std/text.md` | Stdlib #16 character and string utilities |
| `std/atomic.md` | Stdlib #25 `atomic` |
| `std/thread.md` | Stdlib #27 `thread` |
| `std/signal.md` | Stdlib #28 `signal` |
| `std/collections.md` | Stdlib #18 `array`, #19 `map`, #20 `file`, #26 `sort` |
| `pkg/http.md` | Stdlib #17 HTTP (`pkg/http`) |
