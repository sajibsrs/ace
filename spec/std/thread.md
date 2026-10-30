## Stdlib: `thread`

27. `thread` (implemented on darwin-arm64) — explicit OS threads and their
    synchronization; the ordering rules are Memory, "Carved (memory model)".
    `std/thread/mod.ace` selects `std/thread/os/darwin-arm64.ace`, which binds
    libSystem's pthread and platform libraries. libSystem serves OS services
    only, never a C convenience implementation, whatever sub-library hosts it
    (C Interop #4). The module allocates nothing.
    - `Thread: distinct usize` is a joinable thread handle; its canonical zero
      names no thread.
    - `create(entry: extern "c" (*u8): void, argument: *u8):
      (Thread, Error)` starts a thread running `entry(argument)` with the
      platform's default attributes (on Darwin a 512 KiB stack). The entry is
      an ordinary named Ace function taking one pointer and returning nothing;
      it converts to the C function type with no wrapper (C Interop #5), and
      there is no captured context or Ace function-value form. The caller
      keeps what `argument` points to alive until the thread is joined.
      Failure returns `Thread(0)` with `CreateErr` carrying the code
      `pthread_create` returns.
    - `join(thread: Thread): Error` blocks until the thread's entry returns,
      then releases the thread. The entry returns no result; threads pass
      results through the shared state `argument` points to, which join
      orders. Failure returns `JoinErr` carrying the code `pthread_join`
      returns; `Thread(0)` fails with `JoinErr.not_found` without calling it.
      Each created thread is joined exactly once; joining a handle again is
      programmer error.
    - A thread ends when its entry returns. There is no thread exit: it would
      skip the deferred calls of the frames it leaves (C Interop #7). Ending
      the process, by returning from `main` or `sys.exit`, ends every thread,
      and a `fail` or debug trap in any thread aborts the process.
    - `Mutex: struct` is a mutual-exclusion lock over one private `u32`
      (Darwin `os_unfair_lock`); canonical zero is unlocked, so it needs no
      constructor and has no `wipe`. `lock(self)` blocks until this thread
      holds it; `unlock(self)` releases it. Neither fails. The locking
      thread unlocks it and it is not recursive; Darwin aborts the process
      otherwise.
    - `Condition: struct` is a sequence counter over one private `u32`;
      canonical zero has no waiters. `wait(self, mutex: *Mutex)`, with
      `mutex` held, reads the counter, unlocks, sleeps until the counter
      changes (Darwin `os_sync_wait_on_address`), and locks again; the read
      before the unlock means a wake after it is never lost. A wait may also
      return without a wake, so callers recheck their condition in a loop.
      `wake_one(self)` and `wake_all(self)` advance the counter and
      wake one or every sleeper; they need no mutex held.
    - Copying a `Mutex` or `Condition` while it is locked, contended, or
      waited on is programmer error. Neither is usable from a signal handler.
    - A created thread starts with its creator's signal mask (Stdlib #28).
    - Per-thread state is a `threadlocal` module variable (Values & Passing
      #13): a created thread starts with fresh copies, and its copies end when
      its entry returns.
    - Detaching, stack size, and other attributes; `try_lock`, timed waits,
      and process-shared locks are additive.
