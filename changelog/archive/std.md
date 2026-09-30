# Archive — Standard library

Entries recorded before the changelog split; oldest first.


- Folded secure HTTP composition into the shared transport module and marked
  its redirecting client helper with the TLS external component, preserving
  standalone plaintext objects without a separate HTTP TLS file. Plaintext and
  TLS redirect entry points are now private overloads over shared redirect
  state preparation and advancement.
- Reorganized `std/http` as a directory module with focused parsing,
  formatting, client, redirect, transport, and TLS implementation files, and
  replaced six `get` overloads with plaintext `Context` and
  TLS-capable composed `Client { context, tls }` entry points. Redirect and TLS
  choices now live on those reusable values without multiplying overloads.
- Added opt-in bounded HTTP redirects to the owning client through explicit
  `RedirectPolicy` overloads, including relative URL resolution, HTTP-to-HTTPS
  upgrades, downgrade rejection, deterministic limits, and reusable owned URL
  workspace; existing GET overloads retain one-response behavior.
- Implemented the first blocking secure-client slice: a private TLS adapter uses
  system trust by default or decodes one explicit DER override, enforces
  hostname verification and SNI, and exposes partial record/application I/O to
  backend-neutral `tls` storage
  over `tcp`. Consolidated HTTPS into an exact allocation-free `http.get`
  overload with backend-neutral `TlsConfig`, default-null TLS storage, and one
  generic HTTP exchange path. Added compiler-owned conditional external components:
  TLS use automatically links the bundled, selectable BearSSL recipe, while the
  allocation-free plaintext HTTP path emits none of it. BearSSL engine failures retain
  their raw backend code behind a private offset.
- Moved secure entropy into allocation-free `sys.random`, with Darwin
  `getentropy` chunking, `EINTR` retry, and explicit errno failures; collapsed
  the target-neutral TLS facade to `std/tls.ace`; and renamed the compiler-owned
  conditional marker and internal requirement terminology from `@native` to
  `@extern`.
- Generalized compiler-managed external components to ordered recipes keyed by
  component, target, and selected backend. Migrated distributed target-native
  TLS artifacts to `external/` and BearSSL source, headers, rebuild guidance,
  and staging files to `vendor/`; the Darwin BearSSL recipe now carries its
  object, archive, Security, and CoreFoundation inputs.
- Added Darwin system-trust HTTPS as the default behind the stable private TLS
  adapter ABI, while preserving a nonempty DER anchor as an explicit override.
  The adapter feeds BearSSL's delivered chain to the Security framework with an
  SSL hostname policy; SNI remains mandatory. The owning `http.Client` now
  accepts both URL schemes and lazily manages TLS workspace, while the
  caller-storage HTTPS path remains allocation-free.
- Fixed owning HTTPS clients to allocate DNS address storage and grow chunked
  response bodies to the active chunk size instead of retrying unchanged
  capacity indefinitely.
- Pinned BearSSL 0.6 as an experimental TLS-client proof backend and added a
  private adapter/fixture proving direct archive linkage, opaque caller-owned
  client/X.509 storage, supported alignment, half-duplex record-buffer sizing,
  enforced TLS-1.2-only policy, chunked Darwin `getentropy` acquisition through
  `sys.random`,
  and explicit caller-supplied BearSSL seeding without expanding Ace's C ABI or
  expanding Ace's C ABI.
- Standardized concise stdlib naming across public facades, selected OS layers,
  documentation, and fixtures. Subject words remain where a module can expose
  multiple categories (`net.SocketKind`, `sys.FileKind`); established
  abbreviations shorten `Addr` and `Err` without
  clipping ambiguous ordinary words; reversible networking properties use
  `reuse_addr`, `reuse_port`, and `nonblock`; and directory operations use the
  consistent `open_dir` / `read_dir` / `close_dir` family.
- Implemented allocation-free `fmt.print` and `println` overloads using small
  fixed stack buffers and retrying partial writes; portable formatting lives in
  its facade while libc primitives stay in its selected target layer.
- Implemented compiler-planned `fmt.printf` for static format strings. Semantic
  analysis validates exact argument types and builds a format plan; emission
  calls typed helpers directly, with no runtime parser, argument tags, reflection,
  allocation, or C variadic formatter.
- Zero-initialized every deferred-call record, which read uninitialized arena
  memory under the sanitizer build.
- Rejected `fmt.printf` value arguments whose type is still unresolved when the
  plan is built (calls deferred to end of file), which previously matched the
  first helper specialization and silently dropped output.
- Added `%b` binary and `%o` octal `fmt.printf` conversions for `uint` and
  `usize`, generalizing hexadecimal's private helper into one fixed-buffer
  unsigned-radix path. Prefixes, width, and zero padding remain explicit
  additive work.
- Indexed compile-time name lookup: scopes past 32 names build an
  open-addressing hash over their value and type lists, and emit interns symbols
  through a hash table, removing the quadratic scans that made 20,000 generated
  functions take 12.9 s (now 0.4 s). Added `bench/scaling-corpus.sh`, the
  20,000-function point, and growth columns to `bench/run-compile.sh`.
- Removed the remaining per-function quadratic scans behind long-function
  compile time: a `PtrMap` (open-addressing pointer hash in `lexer.c`) indexes
  sema's definite-initialization variables and emit's frame slots and promotion
  candidates, and select lowering looks a name up among the few selections
  before it searches the locals. A 20,000-local function compiles in 92 ms
  (was 2.35 s) with identical machine code.
- Removed the cubic merge from definite initialization. Joining two branch
  snapshots searched each for every variable of the base snapshot; since
  `k->vars` only grows at its head and snapshots list it head first, an
  earlier snapshot is the tail of a later one, so each base variable is found
  by offset. A function of 5,000 locals and 2,500 `if`/`else` statements took
  378G instructions to check, now 2.5G; the regex and json package checks gain
  0.5%. Snapshot cloning itself stays linear in the declared variables.
- Replaced definite initialization's whole-function snapshots with an undo
  trail. A variable's state is saved before its first change after a branch
  point; a branch collects the variables it changed and unwinds, and the paths
  reaching a join intersect only those, a path that left one alone standing in
  with the unwound state. `if`, `switch`, loops, and labeled blocks cost what
  their branches change, not every variable declared so far. A function of
  10,000 locals and 5,000 loops checks in 376M instructions (was 4.2G), with
  5,000 `if`/`else` 298M (was 9.4G); the regex and json package checks gain
  0.55-0.75%. Random programs mixing partial writes with every branch form
  report identical diagnostics under both.
- Added `tests/definite-init-diff.sh`, the differential check behind that
  change: an awk generator writes random functions mixing partial struct and
  array writes with every branch form, and two compilers must report
  byte-identical diagnostics. Both compilers must exist, any baseline output
  other than initialization diagnostics (another diagnostic, a crash) fails
  the run, and mismatching programs are kept.
- Materialized a float constant with `fmov Vd, #imm8` when it is one of the 256
  encodable values (0.125 to 31.0 in sixteenths of a power of two, either sign),
  and folded the scaled index into a scalar array or slice load as
  `ldr Rt, [base, index, lsl #n]` when the element is 1, 2, 4, or 8 bytes wide
  (`index_addr`; stores and other strides keep the separate add). On a `saxpy`
  loop over `f64` arrays, 1.25 ns fell to 0.97; a branchy `f32` collision loop
  fell 19%, a `u8` blend 9%, and a dot product 5%.
- Corrected the float load and store fallbacks for an offset the scaled `imm12`
  cannot reach (over 32 KiB for `f64`, 16 KiB for `f32`). Both materialized the
  byte offset in `x16` and then emitted the unsigned-offset opcode, where the
  `Rm` field lands inside `imm12`: the load ignored `x16` and read a wrong
  constant displacement instead. They now emit the register-offset form,
  `ldr`/`str Vt, [base, x16]`, matching the sibling integer paths. Reachable
  through a scalar tuple member past that distance.
- Read a global integer `const` with a compile-time initializer as an immediate
  instead of a memory load, emit `and`/`orr`/`eor` with an encodable constant
  operand as one bitmask-immediate instruction, and let call-free functions keep
  every used local and parameter in x9-x14 even when read once, removing the
  argument spill and reload from scalar leaf calls (`call-1` 5.7 to 3.2 ns).
- Returned struct literals and tuples whose leaves are call-free scalars, one
  whole 8-byte integer or float per result register (`gen_return_regs`),
  evaluate straight into `x0`-`x1` or `d0`-`d3` instead of a frame temp that is
  then reloaded; leaf w is computed with temp w free so earlier leaves stay live.
  Other shapes (packed sub-8-byte fields, missing fields, call-bearing leaves,
  sret) keep the temp path. `struct-8` 9.3 to 7.6, `struct-16` 9.8 to 7.5,
  `struct-hfa` 13.7 to 12.3, `tuple-return` 16.4 to 12.9 ns.
- A call whose result returns in registers as whole 8-byte chunks (an HFA, or a
  struct or tuple whose size is a multiple of 8) stores them straight into a
  frame-resident local destination instead of a staged temp plus copy: `x = f()`
  and `x: T = f()` into a plain local slot or a field of one, and typed tuple
  destructuring into whole 8-byte scalar binders (`gen_call_to_slot`,
  `gen_call_to_binders`). A one-shot `call_sink` armed by the statement is
  consumed by `emit_call` only for the matching call node and return type, so
  nested calls in arguments never take it. `struct-8` 7.6 to 6.1, `struct-16`
  7.5 to 5.5, `struct-hfa` 12.3 to 9.8, `tuple-return` 13.0 to 11.6 ns.
- A by-value aggregate argument that is a plain frame-resident local (or a
  field of one) and rides registers is loaded straight from its slot at
  marshal time (`ldur x<n>` / `ldur d<n>`) instead of through an address temp
  that was spilled and reloaded; the calling-convention units are now computed
  before the arguments are evaluated. Slice arguments keep the address temp in
  checked builds for the `noalias` range check. Text of `bench/calls.ace`
  fell 33,984 to 33,712 bytes; the call benchmarks moved within layout noise.
- A call-free function's read-only by-value HFA parameter whose every use is a
  constant read of one float leaf (`p.x`, `r.max.y`) keeps only the leaves it
  reads in v23 downward, copied once from the incoming argument registers, with
  no frame slot (`plan_param_homes`, `param_home`). Any whole-value use, `&`,
  runtime index, or call in the body, or more than eight homed leaves within
  the fp temps the body leaves free (`vtf_cap`), keeps the slot. Float `+ - * /`
  reads a homed operand in place, as `src_reg` does for integers. v24-v31 are
  the byte helpers' scratch, so homes sit at the top of the fp temp pool.
  `struct-hfa` 9.7 to 4.9 ns.
- The same read-only leaf-read rule homes whole 8-byte integer-class leaves of a
  by-value struct of at most 16 bytes that arrives in `x` registers: each read
  leaf is copied once into the x14-down registers leaf promotion leaves unused,
  within the int temps (`vti_cap`), and `src_reg` reads it in place. Packed or
  mixed-class structs and narrower leaves keep the slot. `struct-8` 6.5 to 3.9,
  `struct-16` 5.4 to 2.4 ns.
- Float `+ - * /` reads a promoted float local or a register-homed parameter
  leaf in its own register (`fsrc_reg`, the float twin of `src_reg`; the HFA
  fast path in `gen_binop` now goes through it), and an assignment to a promoted
  float local (`x = a op b`, `x op= y`) emits the final operation straight into
  its register. The old `fmov` copy in and out cost about 1 ns each on a
  loop-carried chain. Call-free sum 3.4 to 1.2, multiply-add 3.4 to 1.3,
  `x = x * 0.5 + 1.0` 4.9 to 2.7 ns; the call-carried `float-acc` workload
  4.8 to 2.4 ns.
- Fixed a multi-register return with a pending `defer` returning a corrupted
  second register: `emit_defers` addressed save-area unit w above the slot
  (`retsave_off + w * 8`), which fell below `sp`, instead of within it
  (`retsave_off - w * 8`), so the deferred call's frame overwrote it.
- A union `switch` loads the scrutinee's tag once, before its compare chain,
  instead of reloading the parked address and the tag for every pattern
  (`emit_union_switch`). A scalar field read folds the field's constant offset
  into its load, `ldr Rt, [base, #off]` or the register-offset form past the
  scaled `imm12`, instead of a separate `add` (`field_base`, shared with
  `gen_addr`). The regex bench retires about 5.8% fewer instructions and runs
  3-11% faster (`pathological` 244 to 216, `word-ing` 89 to 79 ns/B); `calls`
  retires 3.3% fewer.
- Register promotion plans integer-class locals by live interval instead of
  for the whole function (`plan_promotion`). `promo_scan` numbers positions in
  emission order and gives each candidate [first, last]: a local is defined
  after its initializer, a parameter at entry; an operand the emitter reads in
  place lives until its consumer runs, a direct call argument to that call's
  `bl` and any other use to the end of its statement part; a use in a loop (or
  labeled block) begun after the definition extends to its end; a use inside a
  `defer` lives to the exits. Real calls are recorded at their `bl`, except the
  deferred call itself, whose arguments still run at the statement. Candidates
  go heaviest first into a register holding no overlapping interval: x14 down
  to x9 in any function when the interval crosses no call, else x19 up.
  `emit_call` errors if a call lies strictly inside an x9-x14 interval or was
  never scanned, except for exit-time defer calls. `store_preg` retargets the
  instruction that just produced a value to write the promoted register
  instead of copying after it, when no label is bound in between and the
  result already has `set_preg`'s width (`fold_preg`). Integer union payload
  binders are promoted and load their payload straight into the register.
  The regex bench retires 7.2-8.3% fewer instructions per workload and runs
  8-17% faster; `calls` retires 1.6% fewer and `cpu` 3.3%.
- `ins` rewrites the instruction just emitted to absorb the next one when no
  label is bound and no site recorded between them (`peep`): `mov t, p` folds
  into a following op that reads and overwrites `t` (add/sub and logical
  immediates, `sbfm`/`ubfm`, shifted add/sub, logical register ops other than
  the mov alias, 2- and 3-source ops, integer loads by unsigned or register
  offset; a 32-bit copy feeds only 32-bit ops), and `add t, s, #a` folds into
  a following `ldr t, [t, #b]` or `add t, t, #b` when the sum fits. Fixups,
  branch sites, relocations, and line rows set `peep_bar`, and it runs only
  inside a function body without `asm`.
- A by-reference parameter's pointer (method receivers, `mut` parameters
  under the internal ABI) is an integer-class promotion candidate: it never
  escapes, takes part in interval sharing, call crossing, and the `emit_call`
  x9-x14 check like any local, and at entry moves from its argument register
  or stack word into its register with no slot. Addressing copies it into a
  temp (`bref_reg`), which `peep` folds into the consuming load, and a scalar
  read loads straight through it. With the peephole, the regex bench retires
  11.1-14.0% fewer instructions per workload (the whole bench 2.72x C, from
  3.10x) and spends 1.9-7.4% fewer cycles, repeatable across two interleaved
  rounds; the peephole alone had saved 5.6-7.4% of instructions but no time.
  `calls` and `cpu` are unchanged.
- A register range loop that ascends by one (`range_up`: step omitted or a
  constant 1, starting at a constant zero over an unsigned type or between
  two constant endpoints in order) loads its start into the cursor and its
  end into a second register, or a cmp immediate up to 4095, and runs
  `cmp`/`b.hs` (`b.ge` when signed) then `add #1`: no bound slots, direction
  test, or division, and one or two loop registers rather than three
  (`loop_own_regs`). Every other range keeps the counted lowering. The regex
  bench retires 7.6-11.0% fewer instructions per workload (the whole bench
  2.47x C, from 2.72x) and spends 5.1-11.2% fewer cycles on all but
  `captures`, flat; `calls` retires 1.2% fewer but spends 1.7% more cycles, all
  in two cases whose smaller frame puts a struct's slots within `ldur` reach
  (see `TODO.md`).
- Checked builds test a runtime range step once at loop entry, after both
  bounds and even over an empty interval: at the step's own width (narrow
  steps extended by the type's signedness), a signed step must be above zero
  and an unsigned one nonzero, or the loop aborts with `file:line:col: range
  step violation` at the step. The located-diagnostic writer behind bounds
  failures is shared (`emit_located_fail_call`). Constant steps, already
  checked by sema, and release builds emit nothing.
- `pkg/regex` declares its `NIL` sentinels (`matcher.ace`, `program.ace`) as
  `const`, so each read is an immediate rather than a global load. The regex
  bench retires 1.5-1.9% fewer instructions per workload, with cycles within
  1% on each and 2.9-3.3% lower for the whole bench across two rounds.
- Removed the quadratic scans live-interval promotion brought back to
  long-function compile time, where interval sharing lets thousands of locals
  hold registers: candidates are ranked once by weight (ties in declaration
  order) instead of rescanned per pick; each register keeps its intervals
  sorted, so an overlap test is one binary search; a loop keeps the list of
  candidates it holds live instead of `scan_loop_pop` visiting all of them;
  and past 16 assignments `promo_reg`/`bref_reg` look a binder up through a
  `PtrMap` and the `emit_call` x9-x14 check searches merged interval
  interiors. Machine code is byte-identical. A 20,000-local function compiles
  in 87-91 ms again, from 1.60 s; the function and type corpora are unchanged.
  `bench/run-compile.sh` times `cc -O2` for the scaling shapes too.
- `&&` and `||` as a value branch on each operand but the last as a condition
  (`gen_branch`, flags straight to `b.cond`) to the short-circuit constant,
  and take the last operand's own 0 or 1 as the result: only the final
  compare materializes a bool, and a stored bool is normalized as `!` does
  (`bool_exact`). `Queue.has` in `pkg/regex` falls from 23 to 17
  instructions. The regex bench retires 1.2-2.1% fewer instructions per
  workload and spends 0.3-1.3% fewer cycles (1.4% for the whole bench) over
  ten interleaved runs; `calls` and `cpu` are unchanged. Settling the last
  operand through a branch too measured no better; a by-hand nested-`if`
  rewrite stays about two points faster in cycles, unexplained.
- A single-binder local of a struct with at most four direct scalar fields
  and no padding (not packed or over-aligned) lives in registers field by
  field. Each field gets a placeholder binder that competes for a register as
  a scalar local does (`split_add` in `promo_scan`), so live intervals, call
  crossing, and the `emit_call` x9-x14 check apply per field; a field that
  loses stays in the struct's slot, allocated only then. A field read or
  write uses its register (`split_reg`, read in place by `src_reg`). Whole
  assignments go field by field (`split_write`): a literal evaluates every
  value before writing any, a copy from another such local reads all fields
  first since the two may share registers, and any other source loads each
  field at its own width from its address; a store to a place is one store
  per field (`split_store_at`). Passed by value, returned, or assigned from a
  register-returned call, the fields move straight between their registers
  and the argument or result registers, packed with `bfi` and split with
  `ubfx` (`split_unit`, and `split_take` through the call sink). Any other
  use keeps the struct whole in its slot: an address, `mut`, a method
  receiver, a field used as a place, `==`, a deferred argument, or passing a
  struct the ABI sends by address (over 16 bytes, not an HFA), which as
  fields would be copied to a temp at every call. `place = T{...}` of such a
  type stores each evaluated field straight into the place instead of
  building a temp and copying it (`lit_store_at`). The `pkg/regex` matcher
  now loads each popped frame field at its width and stores each pushed
  field. The regex bench retires 2.9-3.4% fewer instructions per workload
  and spends 7% fewer cycles on `captures` and 3.0-3.5% for the whole bench
  over two interleaved rounds (`pathological` flat); `calls` retires 2.3%
  fewer and spends 3.6% fewer cycles, with `struct-8` at 1.20x noinline C
  from 1.70x, `struct-16` 1.00-1.09x from 1.38x, `struct-hfa` 3.06x from
  3.38x, and `struct-24` unchanged.
- A union payload binder of such a struct type is split the same way
  (`split_add` at its `N_VARPAT`): the matching arm loads each field that won
  a register straight from the payload, copies only the remaining fields to
  the binder's slot, and allocates that slot only for them. An address or
  other whole-struct use keeps the full payload copy. A loop whose `pc` feeds
  through a `Fork` payload's field ran at 1.00 ns per iteration, from 3.49 ns,
  and `pkg/regex`'s `Matcher.add` no longer stores and reloads its split
  targets; the regex bench is unchanged at four controlled code placements.
- A register-homed float leaf of an HFA parameter now stays in the argument
  register it arrives in instead of being copied to v23 down. Float temps map
  through a per-function table (`vreg`, `vmap`), and `plan_param_homes` swaps
  each home's top temp index with its argument register's index, so no temp
  ever lands on a live home. A float result that the last instruction
  computed into a dead temp is retargeted to its result register (`fold_fp`,
  deleting an `fmov` that becomes `fmov d, d`), for a scalar return and each
  leaf of a returned literal (`gen_return_regs`). A literal leaf whose result
  register is the home of a parameter leaf a later leaf reads waits in its
  temp, which the swap placed in v16-v23, and moves after every leaf is
  computed. `vec_add` is three `fadd`s, and `calls` runs `struct-hfa` at 3.39
  from 4.42 ns and `float-acc` at 1.93 from 2.41 ns at eight controlled code
  placements, with every other row and the `cpu`, `string`, `hotloop`,
  `memory`, and regex code unchanged.
- Implemented `str` character/string operations and explicit
  `str.parse[T]` specializations in pure Ace where practical, with libc float
  conversion isolated in its selected target layer. `char` remains the
  transparent byte alias and strings remain UTF-8 byte sequences.
- Implemented `sys` file-descriptor I/O, descriptor constants, errno capture,
  command-line arguments, and environment access through thin platform
  bindings.
- Implemented `mem.alloc_size` as a compiler-backed raw `mem` name lowered to
  the platform allocator's size query; the benchmarks use it and `time.awake`
  in place of their private externs.
- Implemented `time.elapsed` and `time.awake` as separate monotonic clocks in
  the `time` module over a private Darwin binding.
- Renamed `json.writer_new` to `json.writer`: a constructor carries the `_new`
  suffix exactly when a matching `_wipe` releases what it returns, and the
  writer is a plain value over a caller-owned buffer.
- Added the Darwin Apple-arm64 `sys.ioctl` escape hatch with the native
  `unsigned long` request width, variadic-call ABI, caller-owned byte arguments,
  immediate `IoctlErr` capture, and SDK-verified BPF constants/layouts; the
  allocation-free BPF demo manually walks aligned native headers and bounded
  Ethernet/IPv4 bytes, with deterministic decoding and ABI fixtures.
- Added immediate `sys.exit(code)` through the OS-neutral `sys` facade and
  a private Darwin OS layer calling libSystem `_exit`; it runs neither defers
  nor C-runtime exit handlers and requires no compiler intrinsic.
- Implemented blocking IPv4 TCP `net`: socket creation, explicit options,
  bind/listen/accept/connect, partial send/receive, peer addresses, DNS
  resolution, and operation-specific error domains. The public directory module
  wraps a private Darwin OS layer and owns no hidden allocation.
- Implemented the first allocation-free HTTP slice: strict IPv4 endpoint and
  HTTP URL parsing, all-or-nothing caller-storage request serialization, and
  incremental response parsing for bodyless, fixed-length, chunked,
  connection-close, and informational framing with caller-owned views/storage.
- Completed the symmetric HTTP message surface with `RequestParser` and
  `RequestResult`, renamed response state parsing to `ResponseParser.parse`,
  and exposed all-or-nothing `Request.format` and `Response.format` methods.
- Made both HTTP parsers genuinely incremental by preserving phase, line scan,
  framing, header, and body progress across calls instead of reparsing the
  complete occupied input prefix.
- Added HTTP message and parser-progress field defaults, named method and
  status constants, and conventional response reason selection for a null
  reason while preserving explicit and unknown-status empty phrases.
- Renamed the HTTP formatting error domain to `FormatErr`, matching the public
  `Request.format` and `Response.format` operations.
- Added named, allocation-free descriptions to the existing HTTP URL,
  formatting, and client error codes without changing their domains or numeric
  values; HTTP now constructs those failures through their named entries.
- Exposed the existing allocation-free HTTP status text lookup as
  `http.status(code)`, returning conventional phrases for known codes and an
  empty string for unknown codes without changing `Response`; removed the
  redundant public numeric status constants in favor of ordinary status codes.
- Added named descriptions for standard-library-defined TCP terminal conditions,
  network and poll validation failures, resolver statuses, and deterministic
  system-facade validation failures while preserving raw OS error passthrough.
- Added allocation-free `http.get` over neutral caller-owned `Storage` and an
  owning heap-backed `Client` whose workspace buffers are lazily allocated and
  automatically resized, with the conventional explicit `client_new` and
  `client_wipe` lifecycle operations.
- Added pure-Ace blocking IPv4 `tcp` composition over `net`: listener and
  connection constructors with failure cleanup, blocking accept, exact receive,
  all-byte send, EINTR retry with retained progress, and explicit wrapper close.
  Lower-layer operation errors remain unchanged; only premature EOF and
  successful zero-progress send introduce TCP-level domains. A fresh blocking
  echo fixture establishes the Stage 2 carving baseline without preselecting a
  nonblocking event model or public reactor state shape.
- Implemented the selected `tcp` nonblocking connection-progress model with
  caller-owned per-call buffers, explicit tagged events, managed poll interest,
  partial-send offsets, backpressure, and nonblocking connect completion.
  Added the one-operation `net.pending_error` prerequisite and named Darwin
  errno constants without adding OS code or externs to `tcp`.
- Reshaped the public `tcp` vocabulary around its module scope: `Conn`,
  `Interest`, and `Event` keep type names compact; blocking and nonblocking
  construction remain the distinct `connect` and `connect_nonblock` operations;
  `interest` exposes readiness state; and whole-buffer receive/send use the
  paired `receive_all` / `send_all` names with `ReceiveAllErr` / `SendAllErr`.
- Implemented general descriptor `poll` with allocation-free queues, paired
  watch/unwatch, level-triggered read/write interest, error/hangup reporting,
  caller-owned event storage, and finite/infinite nanosecond waits. Darwin kqueue
  is private to the OS layer.
- Proved nonblocking TCP server composition over `poll` with a forked loopback
  echo fixture: the listener and accepted peer are configured explicitly,
  readiness drives one-attempt accept/receive/send operations, would-block stays
  caller-visible, partial progress is retained, and all descriptors are closed
  explicitly. It exposed the need to query an OS-selected local endpoint.
- Extended the proof into a deterministic multi-client chat fixture with one
  single-threaded event loop, fixed caller-owned client and buffer storage, and
  forked child test clients. It covers multiple accepts, readiness-driven
  fragmented reads and writes, per-recipient broadcast progress, orderly
  disconnects, and reuse of a vacated client slot without adding API surface,
  allocation, threads, atomics, or retry policy.
- Added allocation-free `sys.Process`, `fork`, and `wait` wrappers with decoded
  exit/signal status, keeping process-control externs in the private Darwin OS
  layer. Added `net.local` for querying an ephemeral bound endpoint; networking
  fixtures now bind port zero instead of guessing ports from process IDs.
- Removed generated programs' `strlen` dependency: `sys.arg` and `sys.env` now
  scan their OS-provided NUL-terminated entry bytes directly in emitted code.
- Removed generated programs' `memcmp` dependency: `mem.compare`, `mem.equal`,
  and byte-sequence equality now use direct emitted unsigned-byte loops.
- Removed generated programs' `memset` dependency: `mem.fill` and `mem.zero`
  now use direct emitted byte-store loops.
- Removed generated programs' `memcpy` and `memmove` dependencies: `mem.copy`
  emits a forward byte loop and `mem.move` selects forward or backward traversal
  from the source and destination addresses.
- Added the public `bits` standard module with exact compiler-backed
  `from_float` and `to_float` overloads for `f32`/`u32` and `f64`/`u64`; all
  four preserve complete IEEE-754 representations through register moves with
  no runtime call. Added the restricted trailing `intrinsic` declaration form;
  admitted declarations receive real callable symbols while direct calls may
  inline the same instruction sequence.
- Extended `bits` with exact `u8`/`u16`/`u32`/`u64` population count,
  leading/trailing-zero count, byte-swap, and rotate overloads. Counts return
  `uint`, zero-count operations return the operand width, and rotations reduce
  every `uint` count modulo that width; direct and indirect calls use native
  Apple-arm64 scalar/SIMD instruction sequences with no runtime dependency.
- Shortened the public zero-count families to `bits.lead_zeros` and
  `bits.trail_zeros`, retaining their exact overloads and lowering.
- Added pure-Ace `math.isnan`, `math.isinf`, and `math.isfinite` overloads for
  `f32` and `f64`, using `bits` to classify every IEEE-754 encoding without
  allocation or a libm dependency.
- Added `math.sqrt`, `math.floor`, and `math.ceil` for `f32` and `f64` as
  compiler-provided intrinsics (`BI_MATH_*`, marked in `module.c`, signature-
  checked in `sema.c`) lowering to in-place `fsqrt`, `frintm`, and `frintp` on
  the operand's FP temp, with the same instruction in the callable symbol. The
  intrinsic whitelist grew from `bits` to include these `math` overloads.
- Added pure-Ace `math.exp` and `math.log` for `f32` and `f64`, within 1 ULP over
  the whole finite domain. They use the fdlibm scheme: `exp` reduces by
  `k * ln2` (split high/low) and evaluates a rational polynomial, scaling by
  exact exponent-bit construction in two steps for subnormal results; `log`
  splits the exponent, normalizes the significand into `[sqrt(2)/2, sqrt(2))`,
  and evaluates a polynomial in `s = f / (2 + f)`. The `f32` overloads compute at
  `f64` and round once. Verified against platform libm on 2.2M inputs with none
  beyond 1 ULP; libm is a test reference only.
- Added pure-Ace `math.sin` and `math.cos` for `f32` and `f64`, within 1 ULP
  over the whole finite domain. Small arguments use the fdlibm sine and cosine
  kernels; arguments below about 1.6e6 use three-stage Cody-Waite reduction by
  pi/2; larger arguments use Payne-Hanek reduction: the 53-bit significand
  times eight 32-bit limbs of a 1280-bit 2/pi table (generated with big-integer
  arithmetic) forms a 256-bit fixed-point sum whose truncation error is 2^-170,
  from which the quadrant and two exact 53-bit fraction pieces are taken, then
  multiplied by pi/2 as a double-double (Dekker product, no fused multiply-add).
  Verified against 960K libm comparisons and, independent of libm, against
  exact big-integer arithmetic: 22K random arguments (worst 0.80 ULP) and 2.5K
  continued-fraction worst cases whose reduced fraction reaches 2^-62 (worst
  0.50 ULP).
- Added pure-Ace `math.pow` for `f32` and `f64`, table-driven, with tables and
  constants derived in `std/@/pow_tables.ace` from 100-digit arithmetic.
  `log(x)` is `k*ln2 + log(c) + log1p(r)` with `c = 1/invc` from a 128-entry
  table whose interval holding 1 uses `c = 1`; `invc` is a short binary
  fraction, so `r = z*invc - 1` splits into exact halves and `k*ln2hi + logc`
  is exact. The log is a high and low word with a Taylor tail to `r^10`;
  `y*log(x)` is formed in extended precision (26-bit halves); the result is
  `2^e * 2^(j/128) * exp(er)` from a table of scales and relative corrections
  with a degree-5 Taylor polynomial, where `n = 128e + j` is kept as a signed
  integer so no unsigned wrap is needed. Sign is a final flip; exponents near
  the ends of the range scale in two steps so only the last multiply rounds,
  which keeps subnormal results within 1 ULP. `f32` computes at `f64` and
  rounds once. Against an exact 70-digit reference the worst error is 0.506 ULP
  over 93K normal-result inputs (0.61 ULP in the subnormal range); against
  libm 200K inputs match bit-for-bit or differ by 1 ULP. Exact integer powers,
  C99 Annex F cases, and subnormal and overflow edges are tests. `pow` runs at
  51 ns against libm's 9.5 ns; the remaining gap is backend code quality.
- Added a dedicated `f32` `math.pow` kernel replacing `f32(pow(f64, f64))`:
  plain-`f64` arithmetic over the same tables (no tail word), a log series to
  `r^5`, a degree-3 exponential, and one final conversion that rounds into the
  subnormal range or to infinity. Zero, infinite, NaN, and negative bases and
  non-finite exponents defer to the `f64` overload. Worst error is 0.5025 ULP
  over 59,890 inputs against a 60-digit exact reference; 54 ns fell to 17 ns.
- Inlined finite negative bases with ordinary exponents into `pow(f64)`: the
  units bit of `y` gives the parity by one shift, replacing the `pow_special`
  call and its tuple return. Bit-identical to the old path over 400K operands;
  negative-base integer powers went from 76 ns to 50 ns, positive bases unchanged.
- Added exact fast paths to `pow` for both widths when the exponent is 2, 1,
  -1, or 0.5: a zero-mantissa test selects the powers of two, then `x * x`, `x`,
  `1 / x`, and `sqrt(x)` (positive bases only, so `pow(-0, 0.5)` and
  `pow(-inf, 0.5)` keep the general path). Results are correctly rounded and
  agree with the kernel within 1 ULP over 8M operands; the general path shows no
  measurable cost, and these exponents run at about 7 ns against 49 ns (`f64`).
- Added `math.fma` for `f32` and `f64` as a compiler-provided intrinsic
  (`BI_MATH_FMA`) lowering to `fmadd`: `a * b + c` with one rounding of the
  exact product and sum. The three existing math intrinsics are one-operand and
  write their result in place, so the three-operand form needed its own
  signature clause in `sema.c`, its own emitter, and its own inline expansion,
  which evaluates the operands into FP temps and frees the last two. An
  ordinary `a * b + c` stays two instructions: nothing contracts it.
- Put `exp`'s and `log`'s polynomials on `math.fma` and nested `log`'s
  single-use names: `exp`'s five-term Horner chain becomes five `fmadd`s, and
  `log`'s `t1`, `t2`, and `r` collapse into one expression whose two Horner
  chains and closing `s * (half_f2 + r) + dk * LN2_LO` step fuse as well. Both
  kernels are latency-bound, their dependent chains running about three times
  their throughput, so cutting a Horner step from a multiply plus an add to one
  fused instruction pays on the chain and not on throughput: `exp` fell from
  39.8 to 35.1 ns and `log` from 48.4 to 42.4 ns on a dependent chain, with
  throughput unmoved. The nesting on its own is bit-identical. Against an exact
  90-digit reference over 25,200 inputs covering the finite domain, both
  reduction boundaries, and dense bands near zero and near 1, the worst case is
  unchanged at 0.8025 ULP for `exp` and 0.7517 ULP for `log`; 12 results move at
  all, none past 0.7 ULP, and each of `exp`'s seven lands closer to the exact
  value than before.
- Gave `sin` and `cos` the same treatment, and folded away the `reduce`
  dispatcher: `sin` and `cos` now choose between Cody-Waite and Payne-Hanek
  reduction themselves. That wrapper was one call deep and returned a
  three-value tuple, and since float promotion reaches `d24-d31` every call
  spills and reloads those registers, so removing the layer was worth 28 per
  cent of `sin` on its own while leaving every result bit-identical. On top of
  it, both kernel polynomials moved to `math.fma`, `kernel_cos`'s single-use
  `hz` folded into its one use, `reduce_medium`'s leading Cody-Waite product
  fused (`fn * PIO2_1` is exact, so `r` cannot change), and `product_error`
  disappeared: the low part of a product is by definition `fma(a, b, -p)`, one
  instruction where Dekker's splitting took eleven, which also retires the
  `SPLIT` constant. In the Cody-Waite band `sin` fell from 39.0 to 29.0 ns and
  `cos` from 38.2 to 27.6 ns, below pi/4 from 13.4 to 13.1 and 15.8 to 11.5 ns,
  and beyond 1.6e6 from 607 to 595 ns; the dependent chains fell about a fifth.
  Against an exact reference built from Machin's pi and Taylor series, over
  57,000 arguments spanning the finite domain and 24,000 of them placed within
  a few ULP of a multiple of pi/2 where the reduced argument cancels hardest,
  the worst case is unchanged at 0.7871 ULP for `sin` and 0.7884 ULP for `cos`;
  only `sin` moves at all, on 96 of 33,000, and lands closer to the exact value
  on 66 of those. Reassociating the kernels' accurate-tail expression onto
  `fma` was measured too and rejected: it bought nothing and pushed the worst
  case to 1.48 ULP, past the documented bound.
- Rewrote `exp` and `log` as table-driven kernels over the two 128-entry tables
  `pow` already uses, so all three now share `pow_tables`. `exp` reduces by
  `n*ln2/128` instead of `k*ln2`, reads `2^(j/128)` from the table, and applies
  a fifth-degree correction, which retires the rational form and with it the
  division `(r*c)/(c-2.0)` that sat on the dependent chain; `log` splits the
  exponent, takes `log(c)` from the table, and evaluates `log1p` of the residue,
  which retires the seven-term polynomial in `s = f/(2+f)` and its division too.
  `pow` carries its logarithm as a high and low double because the value must
  survive multiplication by the exponent, and that is the bookkeeping a
  standalone kernel does not need: `exp` and `log` return one rounded double, so
  the low-word arithmetic collapses to a single compensation term. Two table
  facts make the short form exact where it has to be. `z*invc - 1` is
  representable for every entry, so one `fma(z, invc, -1.0)` replaces `pow`'s
  split of `z` into halves and carries `r` with no error at all; and `1.0` sits
  at the exact centre of entry 71, whose `invc`, `logc`, and `logctail` are
  `1.0`, `0.0`, and `0.0`, so near 1 the kernel degenerates to a plain `log1p`
  series with nothing to cancel and `log(1)` is `+0` by construction. Elsewhere
  `w + r` does cancel, losing up to half an ulp of itself, and `(w - t) + r` is
  that loss exactly; carrying it is what keeps a single rounded double honest,
  and dropping it costs a full ULP. `|r| <= 0.00585`, so `log1p` needs the
  series only to `r^8`: `LOG_C9` and `LOG_C10`, which `pow` still uses, do not
  appear. Instruction counts barely move — `exp` 208 to 204 and `log` 196 to 192
  — because the win is dependency-chain depth, not code size. Over 4096 inputs,
  32 repeats, best of seven rounds, glue-subtracted through a non-inlined
  identity: `exp`'s dependent chain fell from 38.3 to 15.2 ns over [-1, 1] and
  from 50.7 to 15.2 ns over [-700, 700], its throughput from 13.1 to 5.8 ns;
  `log`'s chain fell from 45.1 to 23.8 ns and its throughput from 15.5 to 7.8
  ns. The old figures carry the wider spread: `exp`'s branch on `k == 0` makes
  its cost input-dependent, so its baseline throughput ranges over 13-15 ns and
  its chain over 38-53 ns run to run, while the table-driven kernels are
  branch-free on the hot path and reproduce to 0.2 per cent. Accuracy improves,
  so the 1 ULP guarantee is untouched: against an exact 90-digit reference over
  48,216 inputs covering all 128 table-entry edges and their neighbours, 400 ULP
  either side of 1, every power of two, subnormal inputs and subnormal results,
  and both of `exp`'s boundaries, the worst case falls from 0.8050 to 0.7495 ULP
  for `exp`, at -709.04535484224982, and from 0.8128 to 0.5000 ULP for `log`, at
  0.99999999999999978. The `f32` overloads, which round the `f64` result once
  more, are correctly rounded at 0.5000 ULP over 185,065 inputs covering every
  `f32` exponent. `pow_special`'s literal `234187180623265792` became
  `LOG_SUB_OFF`, which `log` shares for the same subnormal rescaling, and the
  sixteen constants the old scheme needed — `LN2_HI`, `LN2_LO`, `INV_LN2`,
  `EXP_HALF_LN2`, `EXP_P1`-`EXP_P5`, and `LOG_L1`-`LOG_L7` — are gone with it.
  `exp`'s hot-path guard admits powers down to -1022, where the scale stops
  being a normal double, rather than reusing `pow_extreme`'s -1000: the two are
  output-identical over 18,139 inputs in the underflow band, because between
  them the result is still normal and the two-step path's closing multiply by
  2^-1000 is an exact shift rather than a second rounding, so the wider bound
  was taken only for stating the guard's own limit. Spelling that guard as one
  unsigned range test instead of two signed compares was measured and rejected:
  0.7 per cent on the chain and 0.6 per cent against on throughput, a wash
  either way. The constant block was regrouped to match: `POW_` named only one
  of three callers once `exp` and `log` joined `pow` on the tables — nine of the
  twenty-nine were pow-only against nineteen already shared — so the prefix now
  states purpose instead. `LOG_*` covers the logarithm reduction and series,
  `EXP_*` the exponential reduction, `TWO_52` and `TWO_M60` are bare powers of
  two, and `POW_ABS` and `POW_SIGN` are all that pow alone still needs. The
  rename is behaviour-preserving, checked rather than assumed: an
  order-sensitive checksum over `exp`, `log`, `pow(f64)`, and `pow(f32)` across
  four million arguments each is unchanged.
- Folded every hot-path call out of `sin` and `cos`. Each call was reaching
  `isnan`, `isinf`, `reduce_medium` — which called `exponent_field` two or three
  times of its own — and then `kernel_sin` or `kernel_cos`: five or six
  branch-with-links, each storing and reloading `d24-d31`, against one in `exp`
  and two in `log`. Splitting the cost by band first showed where a table would
  and would not have helped: below pi/4, where only the kernel runs, `sin`'s
  dependent chain was 19.8 ns, while in the Cody-Waite band it was 64.7 ns, so
  the reduction was about seventy per cent of it. A table shrinks the kernel
  polynomial and needs a finer reduction to do it, which is the smaller half and
  the wrong direction; the calls were the larger half. `reduce_medium`,
  `kernel_sin`, and `kernel_cos` are now inlined at each use and deleted,
  `exponent_field`'s body is spelled out in the folded reduction and the helper
  is left to its one remaining caller in `reduce_large`, and classification
  collapses to a single `(raw & ABS_BITS) >= INF_BITS`, which covers NaN and
  infinity together and yields the magnitude without a branch. `cos`'s
  below-pi/4 path is specialized rather than sharing the folded kernel: the
  unreduced argument has no tail, so every `y1` term drops out. That
  specialization mattered — sharing the general kernel there had pushed `cos` to
  456 instructions and regressed its small-angle chain from 28.1 to 32.0 ns,
  promotion being whole-function, and specializing turned that into 24.6 ns.
  Throughput fell from 31.8 to 11.4 ns for `sin` and 29.6 to 12.2 ns for `cos`
  in the Cody-Waite band, and from 7.2 to 4.0 and 17.3 to 4.7 ns below pi/4; the
  dependent chains fell from 64.7 to 50.7 and 63.0 to 51.8 ns in the band, and
  19.8 to 11.7 and 28.1 to 24.6 ns below pi/4. Beyond 1.6e6 nothing moves, at
  about 590 ns, since `reduce_large` is untouched and still the only call either
  function makes. The cost is code size: `sin` grows from 180 to 392
  instructions and `cos` from 164 to 432, because both kernels and the reduction
  now appear in each. Every result is bit-identical to the previous
  implementation over 48 million comparisons spanning the finite domain in both
  signs, so the documented 0.7871 and 0.7884 ULP bounds carry over unchanged
  rather than needing a fresh reference sweep. `POW_ABS` became `ABS_BITS` and
  gained `INF_BITS` beside it, since the magnitude mask is no longer pow's
  alone.
- Gave the `f32` `sin` and `cos` their own kernels in place of rounding the
  `f64` result: musl's four-term `f64` polynomials, after an inline reduction
  by pi/2 in a 25-bit head and a tail that stays exact below 2^28 * pi/2, and
  Payne-Hanek beyond it. They fell from 7.9 to 4.8 ns (`sin`) and 7.1 to 4.9 ns
  (`cos`) on `bench/std/run-math.sh`, with unchanged checksums; every finite
  input below 1.6e6 and a 1-in-997 sample above it stays within 1 ULP of the
  rounded `f64` result.
- Widened float register promotion past the callee-saved eight: `d24-d31`,
  which nothing else reserves between statements, now take up to eight more
  promoted locals. They are caller-saved and the shared byte routines clobber
  them too, so `fp_leaves` stores and reloads them inside `emit_bl_internal`,
  `emit_bl_extern`, and the indirect-call path — every branch-with-link the
  emitter produces, so no call site can be missed. A function whose loop
  carries a call, or a value of aggregate type over 32 bytes that copies
  through `__ace$mem_copy`, keeps frame slots rather than pay that per
  iteration. `pow(f64)` fell from 28.5 to 23.2 ns and its dependent chain from
  69.3 to 59.0 ns, with slot address computations on its hot path down from 43
  to 17; the `calls` and `hotloop` benchmarks moved within +-0.6% best-of-five,
  and `pow` results are identical on 24M operand pairs and on 11,024 exact
  cases. A promoted local held across a call had no test: one is added, and it
  fails on a build that omits the save.
- Rewrote `pow(f64)`'s extended-precision product on `math.fma`: `e_hi` is now
  `y * log_hi` and its rounding error comes back exactly from
  `fma(y, log_hi, -e_hi)`, which deletes the 26-bit splitting of both operands
  along with `POW_HIGH26`, `y_hi`, and `l_hi` — the split existed only to make
  the leading product exact without a fused multiply-add. 31 ns fell to 29 ns
  and the dependent chain from 78.7 to 69.3 ns. Against an exact 80-digit
  reference over 11,024 inputs covering near-1 bases, log-table entry edges,
  subnormal results, and `y*log(x)` across -745..710, the worst error is
  unchanged at 0.5031 ULP for normal results and 0.7325 in the subnormal range.
  The two forms agree on all but 33 of 24M random operand pairs, each by one
  step, and on those the fused form is the closer one 20 times to 13, with the
  worst case improving from 0.5098 to 0.5071 ULP. Mechanically fusing the
  remaining multiply-add pairs was measured separately at 3% with no effect on
  the chain, and fusing the `f32` kernel at 1%; neither was kept.
- Restructured both `math.pow` kernels around the emitter's register promotion,
  which gives callee-saved float registers only to named locals used at least
  twice: a local that misses out costs a store where it is set and a load at
  every mention. Single-use values are now nested in the expression that
  consumes them, cheap ones (`kd`, `zhi`, the `-r^2/2` term, the 26-bit half of
  `y`, `er^2`, the exponential's correction) are recomputed instead of named,
  the power-of-two entry the extreme path needs moved into that branch, and the
  `pow_signed` call became a sign-bit xor, since `flip` already holds that bit.
  Every step preserves each rounding, so the results are bit-identical: 24M
  `f64` and 6M `f32` random bit patterns plus all 576 special-value pairs agree
  exactly, in checked and release mode alike. `pow(f64)` fell from 49.8 to
  31.3 ns and `pow(f32)` from 13.7 to 11.6 ns; dependent-chain latency fell from
  94.8 to 78.4 ns and 48.1 to 44.1 ns. Rebalancing the correction sums into
  shallower trees, binding the log-table entry through a pointer, and reaching
  the exact fast paths through the base's bits were all measured and gained
  nothing.
- Fixed `pow` returning a NaN for a negative zero or negative infinite base with
  a fractional exponent (`pow(-0, 0.5)`, `pow(-inf, 0.5)`): `pow_special` now
  settles zero and infinite bases before the fractional-exponent NaN, which
  applies only to finite negative bases, so they give `+0` or `+inf` per Annex F.
- Fixed compound untyped constants under a float cast (`f32(-1.0)`,
  `f32(0.0 - 1.0)`, `f32(-3)`, `f32(2 * 3)`), which stored a wrong value or hit
  "untyped operand reached codegen": the cast check now settles the operand
  through `coerce` (float math at the target type, integer math as `i64`).
- Fixed global initializers with a negated float literal (`G: f32 = -2.5`,
  `f32(-2.5)`, struct and array elements), which were rejected as not
  compile-time constants: the constant-initializer folder now folds a negated
  float literal and admits it under a same-family float cast.
- Replaced generated float formatting's `ecvt`/`fcvt`, parse-back, `floor`,
  floating-environment, and memory-helper dependencies with a shared native
  exact binary-to-decimal engine under `std/@`. Fixed-capacity base-2^32
  arithmetic produces shortest round-trip `f32`/`f64` text and fixed `%f`
  rounding through precision 324 without allocation or platform conversion.
- Replaced shortest `f32`/`f64` formatting's exact bigint search with an
  Apache-2.0-attributed Ace adaptation of Ulf Adams' Ryu algorithm. Fixed-width
  products and cached powers now select the shortest ties-to-even coefficient;
  fixed `%f` retains the bigint path through precision 324.
- Replaced `str.parse[f32/f64]` platform parsing with native exact
  decimal-to-IEEE conversion. Fixed-capacity bigint comparisons select adjacent
  values and round exact midpoints to even, including subnormal and overflow
  boundaries, without allocation, locale state, `strtod`, or `strtof`.
- Added a three-tier decimal-to-float parser: exact small integers, a
  fast_float-attributed Eisel-Lemire fixed-width path for ordinary decimals,
  and the existing bigint comparison path for long exact fallback cases. The
  tier-selecting scan also performs the complete syntax validation once.
  Specialized the f32/f64 fast converters after generated-AArch64 profiling:
  fixed-step leading-zero normalization, scalar out-parameters, integrated
  special-value classification, and fused cached-power multiplication avoid
  the generic call chain and its aggregate spill/copy overhead.
- Added a compiler-shipped internal native-endian unaligned word load and used
  it to scan finite float digit runs eight bytes at a time. A branchless SWAR
  digit mask, fixed-step trailing-zero count, packed eight-digit accumulation,
  and scalar short-tail path preserve the shared three-tier parser validation.
- Added short and long-mantissa float-parse benchmark cohorts as supporting
  diagnostics and made an explicit 80%-short/20%-long blend the canonical
  comparison, keeping scan-path performance visible without conflating it with
  the greater-than-19-digit bigint conversion fallback.
- Added `sys.pipe` so descriptor-polling fixtures also use the public OS facade
  instead of declaring a test-local C binding.
- Exposed the target-defined `sys.EAGAIN` and `sys.EWOULDBLOCK` errno constants
  so nonblocking callers do not embed platform numbers.
- Standardized OS-specific stdlib organization as a public `module.ace` facade
  selecting `os/<target>.ace`; `fmt`, `net`, `poll`, and `sys` use the layout,
  and each target file implements its facade operations directly. Flattened
  `str` back to `std/str.ace` after its final OS dependency was removed.
- `math` currently contains only its minimal implemented integer/generic
  helpers; floating libm and SIMD remain open work.
- Added `.acefmt` `single-statement-body = same-line-if-fits` (bodies of functions, `if`, `for`, `while` holding one
  statement stay on the opener's line within 120 columns) and applied it across `std/`, `pkg/`, `demo/`, `bench/`, and
  `tests/`; `else` branches, every branch of an `if`/`else` chain, and a body whose statement is itself a body stay
  multi-line. Fixtures whose line layout is under test (`tests/emit/basics/main.ace` for DWARF rows,
  `tests/modules/location` for `@line`) keep their layout.
- Added `.acefmt` `function-body = multi-line`: a non-empty function, method, or associated-function body always opens
  on its own line, even for one statement; `single-statement-body` now governs only `if`, `for`, and `while`. Applied
  across `std/`, `pkg/`, `demo/`, `bench/`, and `tests/`, with a blank line between each now multi-line declaration.
- Applied the one-word naming rule to std: `ascii.control`/`lower`/`equal`,
  `utf8.valid`, `mem.usable`, `poll.FOREVER`; `bits.popcount`/`clz`/`ctz`/
  `bswap`/`rotl`/`rotr`/`wradd`/`wrsub`/`wrmul`, and one `bits.cast` overload
  set whose declared parameter type (float or unsigned) selects the existing
  read or write intrinsic; the POSIX spellings `sys.opendir`/`readdir`/
  `closedir` and `net.reuseaddr`/`reuseport`, with their error domains and
  private `os/` twins. `tcp`'s `*_nonblock`, `progress_*`, and `*_all` forms
  and `net.pending_error` keep their identifying word.
- Applied the same rule to `pkg/http`: the method constants are the method
  tokens themselves (`http.GET`, `http.POST`, …), and the internal helpers are
  `message.tchar` (RFC 9110's term), `transport.connect`/`get`, and
  `redirection.redirects`. Removed `redirection`'s unused method-constant
  duplicates and `transport`'s private `GET` alias.
