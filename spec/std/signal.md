## Stdlib: `signal`

28. `signal` (implemented on darwin-arm64) — POSIX signal dispositions,
    per-thread masks, synchronous waits, and delivery. `std/signal/mod.ace`
    imports `sys` and `thread` and selects `std/signal/os/darwin-arm64.ace`,
    which binds libSystem's `sigaction`, `pthread_sigmask`, `sigwait`, `kill`,
    `pthread_kill`, and `pthread_self` (C Interop #4). The module allocates
    nothing and installs exactly the handler the caller names: no trampoline,
    thread, or handler table of its own.
    - `Signal: distinct i32` uses the target's numbering; `SIGHUP` through
      `SIGUSR2` are `const Signal` with the OS spellings and Darwin values.
    - `Set: struct` holds the target's `sigset_t` bits, signal n at bit n - 1,
      with Ace bit operations; canonical zero is empty. `add(self, s)`,
      `remove(self, s)`, and `has(self, s): bool` take a signal in the
      target's range; another value is programmer error.
    - `HandleOptions: struct { mask: Set, restart: bool }`; canonical zero
      blocks only the handled signal while its handler runs and sets no
      `SA_RESTART`, so an interrupted call fails as interrupted.
    - `handle(s, handler: extern "c" (Signal): void)` and
      `handle(s, handler, options: HandleOptions): Error` install `handler`,
      handed to the OS as is (C Interop #5). `ignore(s): Error` discards the
      signal. Both refuse SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT,
      SIGSYS, SIGKILL, SIGSTOP, and numbers outside the range with
      `HandleErr.invalid`/`IgnoreErr.invalid` and errno `EINVAL`, before any
      call. `reset(s): Error` restores the default action; the OS judges `s`.
    - `block(set)`, `unblock(set)`, `mask(): Set`, and `mask(set)` act on the
      calling thread's mask and cannot fail.
    - `wait(set): (Signal, Error)` takes one pending signal of `set`, sleeping
      until one is; an empty set fails with `WaitErr.invalid`. Darwin's
      `sigwait` reports an interrupted wait as success with no signal, which
      `wait` retries.
    - `send(process: sys.Process, s)` and `send(t: thread.Thread, s): Error`
      direct `s` at a process or a thread of this one; a process of zero or
      below, which `kill` reads as a group, fails with `SendErr.invalid`.
      `raise(s): Error` sends `s` to the calling thread.
    - A process-directed signal goes to one thread that does not block it; a
      thread-directed one to that thread; blocked everywhere, it stays
      pending. Masks are per thread and `Thread.create` inherits the creator's,
      so the idiom is to block in `main` before creating threads, then have
      one thread `wait`. `exec.spawn` starts a child with an empty mask (#24);
      ignored dispositions are inherited.
    - A handler is its own execution context (Memory, "Carved (memory
      model)"): it shares memory with the interrupted code only through
      `atomic` operations, and calls only `atomic.*`, `sys.read`, `sys.write`,
      `sys.close`, `sys.exit`, `sys.errno()` and `sys.errno(value)`,
      `signal.send`, `raise`, `block`, `unblock`, `mask`, and `panic`; never
      `mem`, `thread.Mutex` or `Condition`, `fmt`, or a `threadlocal` (Values &
      Passing #13). One that may make a failing call saves errno first and
      restores it before returning. A handler returns or ends the process; it
      never unwinds the interrupted frame (C Interop #7). These rules are not
      checked.
    - Error domains: `HandleErr`, `IgnoreErr`, `ResetErr`, `WaitErr`,
      `SendErr`, each carrying the code its call returns.
    - Additive: the previous action, `SA_SIGINFO`, `SA_RESETHAND`/`SA_NODEFER`,
      an alternate stack and fault handling, `pending()`, a timed wait,
      `thread.current()`, a kqueue `EVFILT_SIGNAL` `poll` filter, and a
      `SpawnOptions` knob to inherit the mask.
