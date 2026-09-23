# std.poll

[Standard library](index.md)

`poll` provides allocation-free, level-triggered readiness for general
`sys.Fd` values. Sockets are one consumer; pipes, TTYs, and other supported
descriptors use the same queue without depending on `net`.

`queue` creates a level-triggered readiness queue. `watch` establishes or
replaces the queue's relationship with a descriptor, while `unwatch` reverses
that relationship. Neither operation closes the descriptor, and closing the
queue closes none of the descriptors it watches.

The paired spelling is deliberate stdlib practice: operations that establish
and reverse the same relationship use a recognizable lexical pair when an
accurate pair exists. Semantic accuracy takes priority over superficial
symmetry; unrelated operations are not named as a pair merely to make an API
look balanced.

```ace
import poll
import sys

main(): int {
    q: poll.Queue, err: Error = poll.queue()
    if err != null {
        return 1
    }
    fd: sys.Fd = sys.Fd(0)

    if poll.watch(q, fd, true, false) != null { // readable only
        sys.close(sys.Fd(q))
        return 2
    }

    zero: poll.Event = poll.Event {
        fd = sys.Fd(0), readable = false, writable = false,
        error = false, hangup = false,
    }
    storage: [16]poll.Event = [zero : 16]
    events: mut []poll.Event = mut storage[0..16]
    count: usize, wait_err: Error = poll.wait(q, events, poll.FOREVER)

    if wait_err == null {
        for index: usize in 0 .. count {
            if events[index].readable {
                // One receive attempt can make progress, or observe EOF.
            }
        }
    }

    poll.unwatch(q, fd)
    sys.close(sys.Fd(q))
    return 0
}
```

The readiness surface is:

- `Queue: distinct sys.Fd`; failed creation returns `Queue(-1)`. Close it with
  `sys.close(sys.Fd(q))`.
- `watch(q, fd, readable, writable): Error` registers a descriptor or replaces
  its complete readable/writable interest. At least one interest must be true.
  A failed update may leave partial kqueue state; call `watch` or `unwatch`
  again before relying on that registration.
- `unwatch(q, fd): Error` removes an existing registration. Removing an
  unwatched descriptor is an error.
- `wait(q, events: mut []Event, timeout_ns): (usize, Error)` writes a fully initialized
  prefix of caller-owned storage. The suffix remains unchanged. An empty slice
  is an error. The OS layer uses bounded stack storage, allocates nothing, and
  may return fewer events than the supplied capacity.

An `Event` contains `fd`, `readable`, `writable`, `error`, and `hangup`. At
most one returned event represents each fd; simultaneous conditions are
combined. Readable may mean that a read will report EOF rather than positive
bytes. Hangup may coexist with readable buffered data. Error supplies no
errno value and may coexist with every other condition. Error and hangup need
no registered interest.

A timeout of `0` polls without blocking. `poll.FOREVER` waits without a
timeout; every other value is a finite nanosecond duration. An interrupted
wait returns `WaitErr`; it is never restarted privately. A successful zero
count means that the timeout expired or a poll found no readiness.

The executable fixtures demonstrate the intended server composition. The
smaller `tests/emit/net/nonblocking_echo.ace` proof watches a nonblocking
listener, accepts one peer per ready attempt, configures it explicitly, and
replaces its read/write interest as buffered progress changes.

`tests/emit/net/nonblocking_chat.ace` extends that proof into a deterministic
single-threaded chat server. It accepts two clients, assembles deliberately
fragmented messages, retains partial broadcast progress independently for each
recipient, handles disconnects, and proves that a later client reuses the
vacated fixed-capacity slot. Forked children are only test clients; the server
event loop has no thread, atomic operation, hidden allocation, or retry policy.
Would-block remains an ordinary operation error, and all client, buffer, and
event storage remains caller-owned.

The public domains are `QueueErr`, `WatchErr`, `UnwatchErr`, and `WaitErr`.
Compare `error.domain` with the corresponding `typeid(poll.*Err)` and inspect
`error.code` for the errno captured at failure. Closing a poll queue uses
`sys.close` and therefore retains `sys.CloseErr`.

