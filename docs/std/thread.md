# std.thread

[Standard library](index.md)

`thread` starts and joins OS threads and provides the `Mutex` and
`Condition` they share data under. A thread runs an `extern "c"` entry that
takes and returns one pointer; creating a thread and joining it synchronize
as described in [Memory](../memory.md#memory-model), so a value the creator
writes before `create` is visible to the entry, and one the entry writes
before returning is visible after `join`. Nothing allocates. On darwin-arm64
the module calls libSystem's pthread library for threads and its platform
library (`os_unfair_lock`, `os_sync_wait_on_address`) for locks and waits; it
uses no libc function.

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

## Mutex

```ace
import thread

guard: thread.Mutex // zero is unlocked
total: u64

add(amount: u64) {
    mut guard.lock()
    total += amount
    mut guard.unlock()
}
```

- `Mutex` is valid at canonical zero, so a global, a `blank` value, or a
  zero-filled allocation is an unlocked mutex with nothing to construct or
  release.
- `lock(mut self)` blocks until this thread holds the mutex, and
  `unlock(mut self)` releases it. Neither can fail. Everything a thread
  writes before `unlock` is visible to the next thread whose `lock` returns.
- The thread that locked a mutex unlocks it, and locking it again from the
  same thread is not allowed. Darwin aborts the process on either mistake, in
  every build.
- Through a pointer the call keeps its `mut`: `mut shared.guard.lock()`.

## Condition

```ace
Queue: struct { guard: thread.Mutex, changed: thread.Condition, count: u64 }

take(queue: mut *Queue) {
    mut queue.guard.lock()
    for queue.count == 0 { mut queue.changed.wait(mut &queue.guard) }
    queue.count -= 1
    mut queue.guard.unlock()
}

put(queue: mut *Queue) {
    mut queue.guard.lock()
    queue.count += 1
    mut queue.changed.wake_one()
    mut queue.guard.unlock()
}
```

- `Condition` is also valid at zero. `wait(mut self, mutex: mut *Mutex)` takes
  the mutex the caller holds, releases it while sleeping, and holds it again
  when it returns.
- A wait can return without a wake, so always wait in a loop that rechecks
  the condition, as above.
- `wake_one(mut self)` wakes one waiting thread and `wake_all(mut self)`
  wakes every one; with nothing waiting they do nothing. They need no mutex
  held, but the change they announce must be made under the mutex, or a
  waiter can check the condition just before it and sleep through the wake.
- Never copy a `Mutex` or `Condition` while a thread holds or waits on it,
  and use neither from a signal handler.

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
- `Mutex` is `os_unfair_lock`, as Zig's `std.Thread.Mutex` is on Darwin, and
  not `pthread_mutex_t`: the pthread types are 64 and 48 bytes with a nonzero
  signature word, so a zero-filled one fails with `EINVAL`, and every value
  would need an `init` call and every `lock` an `Error`. `os_unfair_lock` is
  4 bytes, zero when unlocked, checks its owner, and records the owner so the
  system can resolve priority inversions.
- `Condition` is a counter waited on with `os_sync_wait_on_address`, the
  algorithm of Rust's futex condition variable and Zig's `Condition`. Its
  only platform needs are wait-while-equal and wake, which Linux's `futex`
  also provides.
- `wait` and `wake` name the pair as futexes, `os_sync`, and Zig's `Futex` do;
  POSIX `signal` and `broadcast` would collide with process signals.
- On Darwin, threads go through libSystem rather than raw system calls, whose
  thread interface is private and changes between releases; Go moved to
  libSystem there for the same reason.
