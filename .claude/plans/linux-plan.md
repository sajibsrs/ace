# Linux (linux-x64) port plan

Linux support is added on the `linux` branch (`AGENTS.md` Process,
`spec/queue.md`), which stands in for `master` in every other `AGENTS.md`
rule: work in the working tree, present it for review, then commit one topic
at a time. Each step stays under 200k tokens, compiles, and names its own
tests (Rule 16). Run the full `make test` and `make test-packages` once at the
end. CI is out of scope; it gets decided later.

Host: Arch Linux x86_64, kernel 7.2, gcc 16.2.1, clang, glibc, SDL3 0.4.18 in
`/usr/lib`. Baseline measured at `47ab948a` (2026-11-04). Tracking: milestone
"7. Linux x64 port" (#68 umbrella).

## Status

| | Baseline (`47ab948a`) | Now (`ddfa4c91`) |
|---|---|---|
| `make test` | 31 FAIL | 26 FAIL (measured at `55ed156d`; 2.1 added a passing test) |
| `pkg/json`, `pkg/regex` | pass | pass |
| `pkg/http` | 8/8 modules fail | pass |
| `pkg/sdl`, `pkg/ui` | fail (no Linux link) | fail (step 3.2) |
| demos | 3/14 build | 5/14 build (+ http-simple-server, http-simple-client) |
| gcc vs clang build | IR differs | identical (`tests/cc-diff.sh`) |

The 26 remaining failures: thread and signal (14, steps 2.4 and 2.5),
`threadlocal` (1, step 2.6), poll context (3, step 2.3), `--shared` on ELF
(8, step 3.1).

Done:

| Step | Commit | What |
|---|---|---|
| 1.1 | `e0852709` | x64 traps on a zero divisor in release (`IRF_NONZERO`), as arm64 does |
| 1.2 | `17714873` | Lowering sequences IR-emitting call arguments; `tests/cc-diff.sh` compares gcc- and clang-built output |
| 1.3 | `ba4058cc` | `package_fetch.sh` and the far-offset disassembly check no longer assume a darwin host |
| 1.4 | `55ed156d` | `tests/run.sh` sets `ulimit -c 0`; no core files stored (the shell still prints "core dumped" under a pipe `core_pattern`) |
| 1.5 | no change | `slot_base`'s int32-overflow fallback (`src/x64.c`) is unreachable today (every pointer access is `.chk`, and `fold_address` never folds one); kept as the guard for when the fold returns |
| 2.1 | `ddfa4c91` | `net.close_write` / `tcp.close_write` on Linux; new `tests/emit/tcp/close_write.ace` |

Open from done steps: run an HTTP demo server and client against each other
(not done yet).

## Decisions (settled 2026-11-04)

- D1 Signals: OS-only constants are gated with `@when`; `signal.SIGINFO` on
  Linux and `signal.SIGPWR` on darwin are compile errors. Recorded in
  `spec/std/signal.md` with step 2.5.
- D2 Mutex: Linux uses a 3-state futex lock on the existing `u32 word` (0
  free, 1 locked, 2 contended) and a futex on `Condition.sequence`. Layouts
  stay the same on every target, with no `pthread_mutex_t`.
- D3 TLS: local-exec only. `threadlocal` in a `--shared` library stays a
  compile error until something needs it.
- D4 SDL: `pkg/sdl` declares `[link: linux-x64] path: /usr/lib/libSDL3.so`;
  `docs/sdl.md` documents the root-package override for other distros.
- Out of this plan: selfhost x64, performance work, #19, #20 (Linux builds on
  a Linux host only), #52 rpath.

## Remaining steps

### Phase 2: std parity

| Step | Change | Files | Verify | Agent |
|---|---|---|---|---|
| 2.2 | `sys.errno` setter on Linux (`__errno_location`); replace the private `errno_set` copies in the Linux `net` and `poll` files | `std/sys/os/linux-x64.ace`, `std/sys/mod.ace`, `std/net/os/linux-x64.ace`, `std/poll/os/linux-x64.ace` | `tests/emit/net`, `tests/emit/poll`, `tests/stdlib_module` | main session |
| 2.3 | `poll` context on epoll (`epoll_event.data` holds the context or the fd; one registration per fd, so read and write are watched together); un-gate `poll.watch(…, context)`, `Event.context`, the `tcp.watch` overloads | `std/poll/os/linux-x64.ace`, `std/poll/mod.ace`, `std/net/tcp/mod.ace` | `tests/emit/net/poll_context.ace`, `tests/emit/tcp/poll_batch.ace`, `tests/emit/tcp/poll_context.ace` | implementer |
| 2.4 | `std/thread/os/linux-x64.ace`: pthread create/join/detach; futex `Mutex`/`Condition` (D2) | new OS file, `std/thread/mod.ace` | `tests/emit/thread/{join,detach,mutex,condition}.ace`; a contention stress test (N threads × M increments under `Mutex`, `Condition` ping-pong) that fails if the lock is broken; the two multi-threaded HTTP demos build | precision-engineer |
| 2.5 | `std/signal/os/linux-x64.ace`: Linux numbers, glibc `sigaction` / `sigset_t` layouts, gating per D1; a C layout verifier like `tests/darwin_bpf_layout.c`, run on a Linux host | new OS file, `std/signal/mod.ace`, `spec/std/signal.md`, new `tests/linux_signal_layout.c` | `tests/emit/signal/*.ace` (8), layout verifier | implementer |
| 2.6 | ELF TLS (D3): `.tdata`/`.tbss`, `STT_TLS`, `R_X86_64_TPOFF32`, `%fs:`-relative access in x64; lift the darwin-only refusal at `src/emit.c:1444` | `src/elf.c`, `src/x64.c`, `src/emit.c`, `src/backend.h` | `tests/emit/thread/local.ace`, `tests/emit_errors/threadlocal_runtime_initializer.ace`, per-thread values across 2+ threads, `readelf -S` shows TLS sections | precision-engineer |

### Phase 3: Linking

| Step | Change | Files | Verify | Agent |
|---|---|---|---|---|
| 3.1 | `--shared` on ELF: `-shared -Wl,-soname,lib<name>.so` instead of `-dynamiclib -install_name`, a `.so` suffix in the CLI check and `pkg build` output; driver tests check `readelf -d` SONAME on Linux and `otool -D` on darwin. Confirm external calls use PLT32 and the `.so` has no TEXTREL | `src/main.c` (`arg_push_shared`, the `.dylib` check and output name, help text), `src/x64.c`/`src/elf.c` if relocations need it, `tests/run.sh` driver section, `tests/package_cli/shared_*` | the driver `--shared` checks; `package_cli/shared_flag`, `shared_over_archive`; `readelf -d` shows no TEXTREL | implementer |
| 3.2 | `[link: linux-x64]` for `pkg/sdl` (D4); docs note on the per-distro override | `pkg/sdl/ace.pkg`, `docs/sdl.md` | `ace pkg test --dir=pkg/sdl` and `--dir=pkg/ui`; SDL demos build; one opens a window (by hand) | main session |

### Phase 4: Coverage

| Step | Change | Verify | Agent |
|---|---|---|---|
| 4.1 | #16: objdump (Intel syntax) counterparts of the otool checks in `tests/run.sh`: alignment, local slots, register operands, loop registers, range limits, logical values, frame fields, … Each must fail on a deliberately broken input | that section on Linux | implementer |
| 4.2 | #154: run atomics under real threads (after 2.4); keep `cmpxchg` loops only where a fetch-and/or/xor result is read | `checked_atomic`, the thread stress test | precision-engineer |
| 4.3 | `tests/ir` fixtures with `// target: linux-x64` for x64-specific output, where useful | `tests/ir` | implementer |

### Phase 5: Docs and issues

Update `README.md` (badge, and the "macOS only" note at line 37),
`docs/compiler.md`, and the `spec/compiler.md` Compiler #4 target note (TLS,
shared libraries). Close or edit #68, #154, #16.

## Done when

- `make test` on a Linux x86_64 host: 0 FAIL, and no section is silently
  skipped except the darwin-only ones (BPF layout, frameworks).
- `make test-packages`: all five packages pass.
- Every demo except `darwin-packet-sniffer` builds; an HTTP server and client
  exchange a request; one SDL demo opens a window.
- `tests/cc-diff.sh` reports 0 differences.

## Tracked elsewhere

Out of this plan: #186 selfhost x64 backend and ELF writer, #187 Linux
benchmarks, #73 vector-scaled address folding, #188 AVX2 lanes, #17 stack
objects aligned beyond 16, #20 cross-compilation, #52 run-time library paths,
#19 x64 asm operand syntax, #189 linux-arm64.

Found on the way, not Linux-specific: #190 (a local array of 2^31 bytes or
more), `demo/sdl3-cube-raytrace-gpu` failing sema (`const 'shader.SPIRV'
cannot give a writable view`, probably stale on every target), DWARF with line
info only (same as darwin), and in `std/net/tcp/mod.ace` the comment for
`close` sitting above `close_write`.

Darwin-only by nature: `demo/darwin-packet-sniffer` (BPF).
