# std.tcp

[Standard library](index.md)

`tcp` is a pure-Ace blocking IPv4 layer over `net`. It adds common socket setup
and whole-buffer progress without adding allocation, capacity, ownership, or
automatic cleanup. Its `Listener` and `Conn` structs expose their
`net.Socket` field; copying either retains the ordinary manual descriptor-alias
hazards.

```ace
import net
import tcp

serve_one(): int {
    address: net.Addr4 = net.Addr4 {ip = [127, 0, 0, 1], port = u16(8080)}
    listener: tcp.Listener, err: Error = tcp.listen(address, usize(128))
    if err != null {
        return 1
    }
    connection: tcp.Conn, peer: net.Addr4, accept_err: Error = tcp.accept(listener)
    if accept_err != null {
        tcp.close(mut listener)
        return 2
    }
    storage: [4]u8 = [0 : 4]
    request: mut []u8 = mut storage[0..]
    count: usize, receive_err: Error = tcp.receive_all(connection, request)
    if receive_err == null && count == 4 {
        count, receive_err = tcp.send_all(connection, "pong")
    }
    tcp.close(mut connection)
    tcp.close(mut listener)
    return 0
}
```

`listen(address, backlog)` performs `socket(tcp)`, `reuse_addr(true)`, `bind`,
then `listen`; it never enables `reuse_port`. `connect(address)` performs socket
creation followed by blocking connect. Both close a created socket after later
setup failure, return the original setup error even if cleanup fails, and return
a canonical-zero wrapper on failure. `local(listener)` queries the bound local
address, and `accept(listener)` blocks for one connection and returns its peer
`net.Addr4`.

`receive_all(connection, buffer)` fills the complete writable byte view. It
returns the full length and null on success, retained partial progress with
`ReceiveAllErr(0)` on orderly EOF, or retained progress with the unchanged
`net.ReceiveErr` on failure. `send_all(connection, data)` likewise consumes the
complete byte view; a send failure retains progress and its `net.SendErr`, while
a successful zero-progress send returns `SendAllErr(0)`. Both operations retry
only `sys.EINTR`, preserving progress, and empty views succeed with count zero.
Use `net.receive` or `net.send` when exactly one kernel attempt is required.

`close(mut value)` attempts `sys.close` and then zeros the complete caller value
even if closing fails. It returns `sys.CloseErr` unchanged. Canonical zero is
not an ownership state and may contain fd 0, which is valid; close only a live
handle you own. There is no destructor, alias invalidation, or ownership
tracking.

Stage 2 adds explicit nonblocking connection progress while leaving peer
identity, buffers, capacity, and logical receive occupancy with the caller:

```ace
connection: tcp.Conn, err: Error = tcp.connect_nonblock(address)
if err == null && tcp.interest(connection) == tcp.Interest.write {
    // After poll reports writable:
    event: tcp.Event = tcp.progress_connect(mut connection)
    switch event {
        tcp.Event.connected {
            // interest(connection) is now read.
        }
        tcp.Event.failed(connect_err: Error) {
            tcp.close(mut connection)
        }
        else {}
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

Do not call `send_all` while `send_offset` is nonzero; blocking and progress
send modes may be switched only between complete sources. A connection returned
by blocking `accept` must first be configured with
`net.nonblock(connection.socket, true)` before using progress operations.

