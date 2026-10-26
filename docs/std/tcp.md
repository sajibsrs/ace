# std.tcp

[Standard library](index.md)

`tcp` is a pure-Ace blocking IPv4 layer over `net`. It adds common socket setup
and whole-buffer progress without adding allocation, capacity, ownership, or
automatic cleanup. Its `Listener` and `Conn` structs expose their
`net.Socket` field; copying either retains the ordinary manual descriptor-alias
hazards.

```ace
import net
import net/tcp

serve_one(): int {
    address: net.Addr4 = net.Addr4 {ip = [127, 0, 0, 1], port = u16(8080)}
    listener: tcp.Listener, err: Error = tcp.listen(address, usize(128))
    defer tcp.close(&listener)
    if err != null {
        return 1
    }
    connection: tcp.Conn, peer: net.Addr4, accept_err: Error = tcp.accept(listener)
    defer tcp.close(&connection)
    if accept_err != null {
        return 2
    }
    storage: [4]u8 = [0 : 4]
    request: []u8 = storage[0..]
    count: usize, receive_err: Error = tcp.receive_all(connection, request)
    if receive_err == null && count == 4 {
        count, receive_err = tcp.send_all(connection, "pong")
    }
    return 0
}
```

`listen(address, backlog)` performs `socket(tcp)`, `reuseaddr(true)`, `bind`,
then `listen`; it never enables `reuseport`. `connect(address)` performs socket
creation followed by blocking connect. Both close a created socket after later
setup failure, return the original setup error even if cleanup fails, and return
a canonical-zero wrapper on failure. `local(listener)` queries the bound local
address, and `accept(listener)` blocks for one connection and returns its peer
`net.Addr4`.

`receive(connection, buffer)` performs exactly one `net.receive` on the
connection and returns its count (zero at orderly EOF) and error unchanged. The
socket field itself is private.

`receive_all(connection, buffer)` fills the complete writable byte view. It
returns the full length and null on success, retained partial progress with
`ReceiveAllErr(0)` on orderly EOF, or retained progress with the unchanged
`net.ReceiveErr` on failure. `send_all(connection, data)` likewise consumes the
complete byte view; a send failure retains progress and its `net.SendErr`, while
a successful zero-progress send returns `SendAllErr(0)`. Both operations retry
only `sys.EINTR`, preserving progress, and empty views succeed with count zero.
Use `net.receive` or `net.send` when exactly one kernel attempt is required.

Each handle carries an `active` flag, set only by a successful constructor or
accept, so canonical zero (`tcp.Listener {}`, `tcp.Conn {}`) is the empty
handle even though fd 0 is valid. `close(&value)` on an inactive handle is a
silent no-op returning null; this is what lets `defer tcp.close(...)` sit
directly after the constructor, before its error check. On an active handle it
attempts `sys.close` and then zeros the complete caller value even if closing
fails, returning `sys.CloseErr` unchanged. Copies are not found or
deactivated; there is no destructor, alias invalidation, or ownership
tracking.

Stage 2 adds explicit nonblocking connection progress while leaving peer
identity, buffers, capacity, and logical receive occupancy with the caller:

```ace
connection: tcp.Conn, err: Error = tcp.connect_nonblock(address)
if err == null && tcp.interest(connection) == tcp.Interest.write {
    // After poll reports writable:
    event: tcp.Event = tcp.progress_connect(&connection)
    switch event {
        tcp.Event.connected {
            // interest(connection) is now read.
        }
        tcp.Event.failed(connect_err: Error) {
            tcp.close(&connection)
        }
        default {}
    }
}
```

`Interest` is `none`, `read`, `write`, or `both`; `none` is canonical zero.
`Event` has `none`, `data(n)`, `sent(n)`, `connected`, `disconnected`, and
`failed(error)` variants. Each progress call reports exactly one outcome in the
caller's control flow. `data(n)` and `sent(n)` are newly transferred bytes.
EOF and `ECONNRESET` report `disconnected`; other terminal failures retain the
underlying error in `failed`.

`connect_nonblock` creates and configures a nonblocking socket. Immediate
connection starts with read interest; `EINPROGRESS` starts with write interest.
After writable readiness, `progress_connect` owns the `SO_ERROR` query and the
write-to-read transition. `progress_receive` and `progress_send` retry `EINTR`,
map would-block to `none`, and update interest mechanically. Positive receive
suspends read interest until the caller attempts another receive, providing
backpressure without retaining its buffer.

`progress_send` retains only a cumulative offset. Until a source is completely
flushed, the caller must pass the same complete slice and unchanged backing
bytes on every call; re-slicing would advance twice. Completion resets the
offset and removes write interest. This stable-source rule is a programmer
precondition because `Conn` intentionally retains no buffer pointer or
length. Empty receive and send views perform no syscall: empty receive suspends
read and returns `none`; empty send resets its offset, removes write, and returns
`sent(0)`.

`watch(q, connection)` registers the connection with a `poll.Queue` under
exactly its current interest; call it again after each progress operation that
changes `interest(connection)`.

Do not call `send_all` while a progress send is unfinished; blocking and
progress send modes may be switched only between complete sources. Progress
operations take a connection from `connect_nonblock` or `accept_nonblock`; one
from blocking `accept` keeps its descriptor private, so it cannot be switched.

## Design rationale

Why the rules in `spec/std/net.md` are shaped as they are.

- `tcp` blocking conveniences: common blocking setup and whole-buffer progress
  are useful honest compositions, while allocation, capacities, cleanup, handle
  aliasing, and all failure paths remain visible and the low-level module
  retains one-kernel-operation semantics.
- `tcp` progress operations: `tcp` absorbs repeated would-block classification,
  interest transitions, partial-send accounting, connect completion, and
  disconnect recognition while the caller retains control flow, event ordering,
  storage, capacity, identity, and explicit cleanup.
