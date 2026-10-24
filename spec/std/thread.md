## Stdlib: `thread`

27. `thread` (implemented on darwin-arm64) — explicit OS threads; their
    ordering rules are Memory, "Carved (memory model)". `std/thread/mod.ace`
    selects `std/thread/os/darwin-arm64.ace`, which binds libSystem's pthread
    library and no libc function. The module allocates nothing.
    - `Thread: distinct usize` is a joinable thread handle; its canonical zero
      names no thread.
    - `create(entry: extern "c" (mut *u8): mut *u8, argument: mut *u8):
      (Thread, Error)` starts a thread running `entry(argument)` with the
      platform's default attributes (on Darwin a 512 KiB stack). The entry is
      an `extern "c"` definition (C Interop #5) handed to the OS as is; there
      is no wrapper, captured context, or Ace function-value form. Failure
      returns `Thread(0)` with `CreateErr` carrying the code `pthread_create`
      returns.
    - `join(thread: Thread): (mut *u8, Error)` blocks until the thread's entry
      returns, then releases the thread and returns the entry's result.
      Failure returns `null` with `JoinErr` carrying the code `pthread_join`
      returns; `Thread(0)` fails with `JoinErr.not_found` without calling it.
      Each created thread is joined exactly once; joining a handle again is
      programmer error.
    - A thread ends when its entry returns. There is no thread exit: it would
      skip the deferred calls of the frames it leaves (C Interop #7). Ending
      the process, by returning from `main` or `sys.exit`, ends every thread,
      and a `fail` or debug trap in any thread aborts the process.
    - Detaching, stack size, and other attributes are additive.
