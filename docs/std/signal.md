# std.signal

[Standard library](index.md)

`signal` installs signal handlers, blocks signals per thread, waits for them
synchronously, and sends them. A handler is an `extern "c"` definition that
`signal` passes to the OS unchanged; there is no hidden thread, trampoline, or
handler table. Nothing allocates. On darwin-arm64 the module calls libSystem's
`sigaction`, `pthread_sigmask`, `sigwait`, `kill`, and `pthread_kill`.

```ace
import atomic
import signal

stop: u32

extern "c" on_term(s: signal.Signal) {
    atomic.store_relaxed(&stop, u32(1))
}

main(): int {
    if signal.handle(signal.SIGTERM, on_term) != null { return 1 }
    for atomic.load_relaxed(&stop) == 0 {
        // work in bounded steps, checking `stop` between them
    }
    return 0
}
```

- `Signal` is a signal number; `SIGHUP` … `SIGUSR2` have the OS spellings and
  Darwin values.
- `Set` is a set of signals, empty at zero: `add(self, s)`,
  `remove(self, s)`, `has(self, s): bool`.
- `handle(s, handler)` and `handle(s, handler, options: HandleOptions)` run
  `handler` when `s` arrives. `HandleOptions.mask` lists further signals to
  hold back while the handler runs (`s` itself always is), and
  `HandleOptions.restart` makes the calls a handler interrupts resume where
  the OS can. Without it a blocking `sys.read`, `sys.wait`, or `net.accept`
  fails as interrupted; `poll.wait` fails as interrupted either way.
- `ignore(s)` discards `s`; `reset(s)` restores the OS's default action.
- `block(set)`, `unblock(set)`, `mask()`, and `mask(set)` read and change the
  calling thread's mask. A blocked signal stays pending, once, until
  unblocked or taken by `wait`.
- `wait(set): (Signal, Error)` sleeps until a signal of `set` is pending and
  takes it, running no handler.
- `send(process, s)` signals a process, `send(t, s)` a thread of this
  process, and `raise(s)` the calling thread.

`handle` and `ignore` refuse SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT,
SIGSYS, SIGKILL, SIGSTOP, and numbers outside the range with `invalid`. `send`
refuses a process of zero or below, which `kill` would read as a process group
or every process.

| Domain                   | `invalid` means                               |
|--------------------------|-----------------------------------------------|
| `HandleErr`, `IgnoreErr` | signal cannot be handled or ignored           |
| `ResetErr`               | signal number is out of range                 |
| `WaitErr`                | empty set                                     |
| `SendErr`                | bad signal or target; also `not_found`, `permission` |

## Writing a handler

A handler interrupts its thread at any instruction, so it may only touch
memory through `atomic` operations and may only call `atomic.*`, `sys.read`,
`sys.write`, `sys.close`, `sys.exit`, `sys.errno`, `signal.send`, `raise`,
`block`, `unblock`, `mask`, or `panic`. It must not allocate, lock a `Mutex`,
format output, or use a `threadlocal` (whose first use on a thread
allocates). None of this is checked. A handler that may fail a call saves and
restores errno so the interrupted code still sees its own:

```ace
wake: sys.Fd // the write end of a pipe a poll loop watches

extern "c" on_term(s: signal.Signal) {
    saved: i32 = sys.errno()
    byte: [1]u8 = [1]
    sys.write(wake, byte[0..1])
    sys.errno(saved)
}
```

That self-pipe turns a signal into a readable descriptor, so an event loop
sees it beside its sockets and shuts down through its ordinary `defer`s
(`tests/emit/signal/shutdown.ace`). A handler returns or ends the process; it
never jumps out of the interrupted code.

## Threads and children

A process-directed signal (`send(process, s)`, a terminal's Ctrl-C) goes to
one thread that does not block it; a thread-directed one to that thread. Each
thread has its own mask and a created thread copies its creator's, so block
in `main` first and let one thread `wait`:

```ace
set: signal.Set = blank(signal.Set)
set.add(signal.SIGTERM)
set.add(signal.SIGINT)
signal.block(set) // before any thread.create
// ... create workers, then in one thread:
s: signal.Signal, err: Error = signal.wait(set)
```

`exec.spawn` starts children with an empty mask; a signal this process
ignores stays ignored in them. `sys.fork` copies the mask unchanged.

## Design rationale

- The handler is the caller's `extern "c"` function given to `sigaction`
  unchanged, as in C and Zig's `std.posix.sigaction`. Go instead owns every
  handler in its runtime and forwards signals to channels, which needs a
  runtime thread and a queue; Ace has neither. A channel-like form can be
  built on `wait` in a thread.
- Fault signals are refused because a handler for SIGSEGV or SIGBUS
  returning re-executes the faulting instruction, and leaving it needs an
  unwind past Ace frames that skips their `defer`s. Fault handling with an
  alternate stack can be added once its exit path is decided.
- `restart` is off by default, unlike BSD's `signal()`: an interrupted call
  failing visibly lets a loop notice a shutdown request, while a silent
  restart leaves a blocking `accept` asleep through it. `sigaction` itself
  and Zig's `std.posix.sigaction` also default to no restart.
- A `threadlocal` is ruled out in handlers as C11 7.14.1.1 and C++17
  [support.signal] rule out thread storage: on Darwin the first access
  allocates.
- `Set` operations are bit operations on the `sigset_t` word rather than
  calls to `sigaddset` and its family, which are C conveniences, not OS
  services. `sigaction` itself is used although libSystem hosts it in its C
  library: it is the documented interface to the kernel's signal state.
- Darwin's `sigwait` returns success without storing a signal when
  interrupted, so `wait` retries until it has one.
- `Set` is a struct over one `u32`, as `thread.Mutex` is, because methods
  belong to structs; the layout is the target's `sigset_t`.
