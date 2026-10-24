# std.thread

[Standard library](index.md)

`thread` starts and joins OS threads. A thread runs an `extern "c"` entry that
takes and returns one pointer; creating a thread and joining it synchronize
as described in [Memory](../memory.md#memory-model), so a value the creator
writes before `create` is visible to the entry, and one the entry writes
before returning is visible after `join`. Nothing allocates. On darwin-arm64
the module calls `pthread_create` and `pthread_join` in libSystem's pthread
library; it uses no libc function.

```ace
import thread

Job: struct { input: u64, output: u64 }

extern "c" square(argument: mut *u8): mut *u8 {
    job: mut *Job = mut *Job(argument)
    job.output = job.input * job.input
    return argument
}

main(): int {
    job: Job = Job { input = 7, output = 0 }
    argument: mut *u8 = mut *u8(mut &job)
    worker: thread.Thread, err: Error = thread.create(square, argument)
    if err != null { return 1 }
    result: mut *u8, jerr: Error = thread.join(worker)
    if jerr != null { return 2 }
    return int(job.output) // 49
}
```

- `Thread` is a joinable thread handle; `Thread(0)` names no thread.
- `create(entry: extern "c" (mut *u8): mut *u8, argument: mut *u8):
  (Thread, Error)` starts a thread running `entry(argument)` with the
  platform's default attributes. Darwin gives such a thread a 512 KiB stack,
  far less than the main thread's 8 MiB, so keep large arrays off its stack.
  Failure returns `Thread(0)`.
- `join(thread: Thread): (mut *u8, Error)` waits for the entry to return,
  releases the thread, and returns the entry's result. Join every created
  thread exactly once; joining a handle again is programmer error.
  `Thread(0)` fails with `JoinErr.not_found`.

A thread ends when its entry returns. Ending the process, by returning from
`main` or calling `sys.exit`, ends every thread with it, and a `fail` or a
debug trap in any thread aborts the whole process. Threads share memory under
the [memory model](../memory.md#memory-model): two threads touching the same
bytes, one of them writing, need an `atomic` operation, `create`, or `join`
to order them.

| `CreateErr`  | Meaning                                  |
|--------------|------------------------------------------|
| `retry`      | temporary thread resource limit reached  |
| `invalid`    | thread attributes are invalid            |
| `permission` | thread scheduling not permitted          |

| `JoinErr`   | Meaning                  |
|-------------|--------------------------|
| `not_found` | no such thread           |
| `deadlock`  | thread would join itself |
| `invalid`   | thread is not joinable   |

## Design rationale

- The entry is an `extern "c"` definition handed to the OS unchanged, as C11
  `thrd_create` and POSIX `pthread_create` take it. Accepting an ordinary Ace
  function value instead, as Zig `std.Thread.spawn` and Odin `thread.create`
  do, needs an internal wrapper and storage for the function and argument that
  outlives `create`: either a hidden allocation or a lifetime rule on the
  caller. That form can be added later without changing this one.
- There is no thread exit. `pthread_exit` leaves the Ace frames above it
  without running their deferred calls, the same hazard as a `longjmp` across
  an Ace frame; returning from the entry already ends the thread. Zig's
  `std.Thread` and Rust's `std::thread` have no exit either.
- `join` checks `Thread(0)` itself, as `sys.wait` rejects a zero process, so
  a handle from a failed `create` fails cleanly instead of reaching the OS.
- On Darwin, threads go through libSystem rather than raw system calls, whose
  thread interface is private and changes between releases; Go moved to
  libSystem there for the same reason.
