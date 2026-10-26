# std.net

[Standard library](index.md)

`net` provides a thin IPv4 TCP layer with allocation-free core socket
operations. Its public values and operations are
OS-neutral; `std/net/mod.ace` selects the darwin-arm64 or linux-x64 OS
layer through `@when(_PLATFORM_ == "...")`. Platform structures, constants, byte
order, and C bindings stay in that private-by-convention file. Error codes are
the target's errno values; glibc's resolver codes are negative. `net.Socket` is a transparent
alias of `sys.Fd`, so close sockets with `sys.close`.

```ace
import net
import sys

main(): int {
    sock: net.Socket, err: Error = net.socket(net.SocketType.tcp)
    if err != null {
        return 1
    }
    ip: [4]u8 = [u8(127), u8(0), u8(0), u8(1)]
    address: net.Addr4 = net.Addr4{ip = ip, port = u16(8080)}
    if net.reuseaddr(sock, true) != null ||
       net.bind(sock, address) != null ||
       net.listen(sock, usize(128)) != null {
        sys.close(sock)
        return 2
    }
    if sys.close(sock) != null {
        return 3
    }
    return 0
}
```

`Addr4: struct { ip: [4]u8, port: u16 }` stores IP octets in ordinary
textual order and the port in host order. Its canonical zero is
`0.0.0.0:0`, usable as the wildcard address with an OS-selected port.
`parse4(endpoint): (Addr4, bool)` parses a complete dotted-decimal IPv4
endpoint such as `127.0.0.1:8080`. All four octets and the decimal port are
required; invalid or trailing text returns canonical-zero `Addr4` and false.
It performs no lookup or allocation. Endpoint formatting, IPv6, and peer-address
queries remain additive.

The core socket operations each perform one explicit operation and never
allocate, buffer, retry, close, or replace a socket on failure:

- `socket(SocketType.tcp): (Socket, Error)` creates an IPv4 TCP socket.
- `reuseaddr` and `reuseport` reversibly set their named option.
- `nonblock(socket, enabled): Error` reversibly changes blocking mode.
- `bind(socket, Addr4): Error` binds without enabling options implicitly.
- `listen(socket, backlog: usize): Error` starts listening. The OS may cap the
  backlog; a value too large for the platform call fails instead of truncating.
- `accept(listener): (Socket, Addr4, Error)` makes one blocking acceptance
  attempt. It returns the peer address and does not configure the accepted
  socket. Failure returns `Socket(-1)` and canonical-zero `Addr4`.
- `connect(socket, Addr4): Error` makes one connection attempt without
  retry, cleanup, or configuration.
- `local(socket): (Addr4, Error)` queries the socket's current local
  endpoint without changing it. This exposes the assigned port after binding
  port zero; failure returns canonical-zero `Addr4` with `LocalErr`.
- `receive(socket, buffer: []u8): (int, Error)` performs one receive.
  Partial counts are returned as-is; zero with null error is orderly peer
  shutdown.
- `send(socket, data: []u8)` performs one send and accepts `string`/`[]char`
  values by shared identity. It returns partial counts as-is. A closed peer becomes an ordinary send error
  rather than terminating the process with `SIGPIPE`.
- `resolve4(host, port, addresses: []Addr4): (usize, Error)` performs one blocking
  hostname lookup and writes IPv4 TCP endpoints into caller-owned storage in
  resolver order. The returned count is the number written; each address uses
  the supplied host-order port. Empty storage, invalid hostname bytes, lookup
  failure, or no IPv4 result returns zero with `ResolveErr`.

```ace
storage: [8]net.Addr4
addresses: []net.Addr4 = storage[0..8]
count: usize, err: Error = net.resolve4("example.com", u16(80), addresses)
if err == null && count != 0 {
    sock: net.Socket, socket_err: Error = net.socket(net.SocketType.tcp)
    if socket_err == null {
        socket_err = net.connect(sock, addresses[0])
        sys.close(sock)
    }
}
```

The platform resolver temporarily allocates its private result list and frees it
before `resolve4` returns. This is a narrow resolver-boundary exception: no
resolver-owned pointer escapes and the application owns all returned storage.
The initial API does not indicate that additional results were truncated or
expose the platform resolver's detailed status. A native caller-buffer DNS
implementation, richer diagnostics, truncation discovery, IPv6, asynchronous
resolution, caching, and resolver-policy controls remain future improvements.

Every network error is last and null on success. The operation-specific public
domains are `SocketErr`, `ReuseaddrErr`, `ReuseportErr`,
`NonblockErr`, `BindErr`, `ListenErr`, `AcceptErr`, `ConnectErr`,
`PendingErrorErr`, `ReceiveErr`, `SendErr`, `ResolveErr`, and `LocalErr`. Compare `error.domain`
with the corresponding `typeid(net.*Err)`. For socket-operation errors,
`error.code` is
the captured errno; resolver failures preserve the direct resolver status code.
Closing uses `sys.close` and therefore
retains `sys.CloseErr`.

No retry policy is hidden. In particular, interrupted, aborted, would-block,
and resource failures from `accept` remain caller-visible. Nonblocking
connection completion is composed by `tcp`; half-close, additional TCP options,
UDP, and broader server/client convenience remain future additions.

`pending_error(socket): Error` performs one pending-connect `SO_ERROR` query.
It returns null when no stored connection error exists, `ConnectErr(code)` for
the stored connection result, or `PendingErrorErr(errno)` if the query itself
fails. Higher-level nonblocking connection completion belongs to `tcp`.

## Design rationale

Why the rules in `spec/std/net.md` are shaped as they are.

- Module shape (Stdlib #13): networking is a large coherent family deserving a
  predictable module boundary; hiding incidental platform ABI details improves
  modularity without compromising performance or machine truth. IPv4 first
  proves that boundary and the later event model with the smallest coherent
  surface. The explicit `Addr4` value keeps byte order and platform layout out
  of application code while leaving `Addr6` additive. The alias gives networking
  a domain name while retaining zero-cost interoperability with ordinary
  descriptors and the future event queue. One type-parameterized constructor
  keeps the family predictable as protocols are added; its Ace enum is privately
  mapped to platform constants. Separate option, bind, and nonblocking
  operations compose across socket roles and keep every kernel-state transition
  explicit; convenience belongs above this core rather than inside it. Result
  shapes follow Ace's existing explicit error convention without returning
  redundant values from operations on an existing socket.
- `listen`: `usize` expresses a nonnegative count, and retaining a single
  transition preserves the core composition rule.
- `accept`: the one-attempt rule avoids hidden control flow or renewed blocking,
  while leaving retry policy and accepted-socket configuration explicit.
- Module organization: the directory groups one coherent multi-file module,
  compile-time selection reflects actual support, and the facade preserves one
  stable API without redundant target-to-target forwarding.
- `connect`: blocking connect completes the initial client proof with one kernel
  transition, while the deferred facilities require independent semantics and
  are unnecessary for the first blocking client or nonblocking servers.
- `local`: the nonblocking proof genuinely requires passing an ephemeral
  listener endpoint to its client, and the short `local` name leaves the
  accurate `local`/`peer` pair for endpoint queries.
- `resolve4`: caller-owned result storage keeps persistent memory and capacity
  explicit, one blocking call matches the initial blocking TCP layer, and the
  narrow platform boundary supplies immediately useful host lookup without first
  adding UDP, DNS packet parsing, resolver configuration, retry policy, and TCP
  fallback to Ace.
