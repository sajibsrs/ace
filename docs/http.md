# http

[Documentation](README.md)

`http` implements allocation-free HTTP/1.1 framing plus blocking one-shot GET
over plaintext TCP. The framing and parsing operations allocate nothing,
perform no I/O, and retain no caller buffer. Client DNS, connection, request,
response, and cleanup failures remain explicit in returned errors.

`http` is a standalone package at [`pkg/http`](../pkg/http), not a `std`
module — it has no compiler-owned mechanism tying it to the shipped `std/`
tree. A consumer depends on it like any other package (an `http: 0.1.0`
`[dependency]` entry in its own `ace.pkg`, found as the nearest `pkg/http`);
the bare `import http` in source stays the same either way. See
[`demo/http-simple-client`](../demo/http-simple-client) and
[`demo/http-simple-server`](../demo/http-simple-server) for consuming
examples.

HTTPS/TLS support was removed pending a full redesign; see the TLS client
transport issue (#65). `Url.parse` still accepts an `https://` scheme
for syntax purposes, but every client entry point rejects it with
`UrlErr.invalid`.

`Url.parse(url): (Url, Error)` accepts absolute `http://` and `https://` URLs,
autodetecting the scheme from `url`'s prefix. The overload
`Url.parse(url, secure): (Url, Error)` forces the scheme instead of
autodetecting it. `Url { host, path, query, port }` contains byte views and a
host-order port.
The default port is 80 and the default path is the static `/` view. A query
without a written path is represented as that path plus a `query` view which
includes `?`. DNS names up to 253 bytes with labels up to 63 bytes and dotted
IPv4 hosts are accepted. User information, fragments, bracketed IPv6, controls,
empty or invalid ports, and other schemes return `UrlErr`. HTTP defaults to
port 80 and HTTPS to port 443. Path/query
bytes and percent escapes are unchanged.

`get(url, &context)` accepts only `http://`. `ClientContext` owns reusable
HTTP workspace and policy, is created with `ClientContext.new`, and is
released with `wipe`.

```ace
import fmt
import http

main(): int {
    context: http.ClientContext = http.ClientContext.new()
    response: http.Response, err: Error = http.get("http://example.com/", &context)
    if err != null {
        fmt.printf("request failed: %d\n", int(err.code))
        context.wipe()
        return 1
    }
    fmt.printf("status %d, %d bytes\n", int(response.status), int(response.body.len))
    context.wipe()
    return 0
}
```

Redirects remain opt-in. Assigning `RedirectPolicy { limit = 10 }` to
`context.redirects` follows 301, 302, 303, 307,
and 308 responses with valid
`Location` fields, resolving absolute HTTP/HTTPS URLs, scheme-relative URLs,
absolute paths, query-only references, and relative paths against the previous
URL, but a redirect target requiring HTTPS fails with `UrlErr.invalid` since
this client is plaintext-only. A chain requiring more than `limit` followed responses
fails with `RedirectErr.limit`. Missing or invalid locations return the original
3xx response. The canonical zero policy performs one request.

`get` shares URL parsing, address iteration, request construction,
formatting, response parsing, error mapping, and TCP cleanup through one
exchange over the TCP connection.

`Request.format(output): (usize, Error)` and
`Response.format(output): (usize, Error)` write one complete HTTP message or
leave insufficient `output` unchanged. Output must not overlap storage
reachable through the message. `Request` contains a string method and target;
`Response` contains an HTTP minor version, status, and reason. Both contain
caller header and body views. Formatting validates start-line and header bytes,
preserves header order and content, and does not invent `Host`,
`Content-Length`, or `Transfer-Encoding`. `FormatErr` code 1 means output
capacity; request codes 2–5 identify method, target, header-name, and
header-value validation. Response codes 4–8 identify header-name, header-value,
version, status, and reason validation.

Request headers and body default to null, while method and target remain
required. Every `Response` field has a construction default: HTTP/1.1, status
200, and null reason, headers, and body. A null reason formats with the
conventional phrase for a named status, or an empty phrase for another valid
status; an explicit reason, including `""`, is preserved. The nine common
request methods are public string constants spelled as their tokens,
such as `http.GET` and `http.PATCH`. Response statuses remain plain `u16`
numbers. `status(code)` returns the conventional phrase for a known code and `""`
for an unknown code; it does not restrict extension statuses in `100...999`.

`parser.parse(input, headers, body, eof)` on `RequestParser` and
`parser.parse(input, headers, body, eof, no_body)` on `ResponseParser`
incrementally parse HTTP/1.0 or HTTP/1.1 messages. `input` is the complete
occupied prefix of the same stable caller-owned receive storage on every call.
Each call resumes from saved state and examines only newly available line bytes
or body bytes, rather than reparsing the prefix. `headers` stores zero-copy
name/value views; `body` receives payload bytes and decoded chunk data. The
caller preserves both buffers and their written prefixes until completion;
the parser retains no buffer pointer. Each public parser records line scanning,
consumed input, header/body counts, framing, phase, and chunk progress. The
response parser additionally records consumed informational responses; its
`eof` completes a connection-close body, and `no_body` supplies request context
such as HEAD. A request without an explicit framing header has an empty body.
Reinitialize the parser value before starting another message.
All numeric and Boolean parser-progress fields have zero defaults, so a fresh
parser literal needs to name only its initial `phase` and `framing` fields.

`RequestResult` and `ResponseResult` report exactly one of cumulative
`progress`, `complete`, `capacity(headers|body)`, `syntax`, or `framing`.
Completion exposes the parsed message and its caller-owned views. Both parsers
handle `Content-Length` and chunked coding with syntax-checked discarded
trailers. Response parsing additionally handles bodyless, connection-close, and
informational-response framing. Conflicting lengths, length plus transfer
coding, unsupported transfer coding, malformed chunks, premature fixed/chunked
EOF, and status 101 are explicit failures. A framing conflict takes precedence
over a full header buffer (`framing`, not `capacity(headers)`). Header names are compared using
ASCII case folding; values and payload bytes otherwise pass through unchanged.

## Simple HTTP client demo

A runnable proof of concept lives at `demo/http-simple-client/main.ace`: it
calls `get(url, &context)` directly, printing the response status and
body. Build and run it with:

```sh
./ace demo/http-simple-client/main.ace -o http-simple-client
./http-simple-client
```

## Server: blocking `serve`

`serve(endpoint, context, router): Error` binds a blocking `tcp` listener with
`context.backlog`, then loops forever, serially: accept one connection, read
exactly one request into `context`'s growing buffers, dispatch it through
`router`,
format and write the response with synthesized `Content-Length` and
`Connection: close` fields, close, repeat. If the client sent bytes past its request, `serve` first
half-closes and discards input for up to 500 ms, so the client can read the
response; only such a client can delay the serial loop by that wait. It stays one request per connection
because it is serial: an idle persistent client would block every other client.
`serve` returns only if the
listener itself cannot be created; any per-connection failure (parse error,
allocation failure, mid-write disconnect) just drops that connection and the
loop continues. There is no concurrency — one connection is handled at a
time — so this overload is meant for demos and local testing rather than
serving concurrent traffic.

A caller-driven, nonblocking `ServerContext`/`Event` core existed here at one
point but was implemented directly, without going through the Decision Queue
carve process. It has been removed; a fresh carve of how much server
mechanism belongs in the language versus a framework built on top is tracked
in issue #67 before anything like it returns.

## Simple HTTP server demo

A runnable proof of concept lives at `demo/http-simple-server/main.ace`: it
calls `serve(endpoint, context, router)` directly, with no `ServerContext`, event loop,
or explicit respond call — just an endpoint and a handler. Build and run it
with:

```sh
./ace demo/http-simple-server/main.ace -o http-simple-server
./http-simple-server  # listens on 127.0.0.1:8080
```

Verify it with any HTTP client, for example `curl http://127.0.0.1:8080/`.

## Multi-threaded HTTP server demo

`serve(listener, context, router)` serves an already bound `tcp.Listener`
forever, with the same per-connection behavior as `serve(endpoint, ...)`. The
caller owns the listener. Several threads may serve one listener, each with its
own `ServerContext` (the buffers are mutable and not shareable): the kernel
hands every accepted connection to exactly one blocked `accept`.
`demo/http-multi-threaded-server/main.ace` binds once, starts four `thread`
workers over that listener, and joins them. One idle connection occupies one
worker rather than the whole server. Handlers run concurrently, so they must be
thread-safe; the router is read-only after registration and is shared.

```sh
cd demo/http-multi-threaded-server && ace pkg run  # listens on 127.0.0.1:8080
```

## Server connections

`ServerConnection` is one client's HTTP state over any byte transport. It
parses requests, frames responses, and keeps pipelined bytes, and it never
touches a socket, so the caller owns I/O, polling, and timeouts:

```ace
room: []u8 = connection.room()      // null: over the limit or out of memory
connection.received(count)          // after receiving into room
switch connection.next() {          // request, need_input, or failed
    http.ServerStep.request(request: http.Request) {
        connection.respond(request, response)
        connection.sent(sent_count) // after sending connection.output()
    }
    default {}
}
```

`next` returns `send` when the client sent `Expect: 100-continue` and its body
has not arrived: send `output` as usual, then call `next` again. Closing is the
caller's: half-close with `net.close_write` and discard input for a bounded time
before `sys.close`, or unread bytes can reset the connection before the client
reads the response.

Call `next` again after a response is sent, before receiving: pipelined requests
produce no socket readiness. `persistent()` says whether to keep the connection
after the response, and `idle()` says no part of a next request has arrived,
which separates an idle timeout from a request deadline. Request views last
until the response is completely sent.

`respond` synthesizes `Content-Length`, keeping a HEAD handler's own value, and
omits the body and length as HEAD, 1xx, 204, and 304 require. It replaces
handler `Connection` fields, though a handler's `close` ends the connection, and
confirms HTTP/1.0 persistence with `Connection: keep-alive`. A handler
`Transfer-Encoding` fails with `ServerErr.framing`, since the body is complete.

`Request.version_minor` reports the received version, and
`Request.keep_alive()` applies the persistence rules: `close` always ends the
connection, HTTP/1.1 persists by default, and HTTP/1.0 needs `keep-alive`.

## Multi-threaded event-loop HTTP server demo

`demo/http-multi-threaded-event-loop-server/main.ace` keeps the demo above's
one listener and four threads, but each thread runs its own `poll` queue over
non-blocking sockets, so an idle or slow client holds a `Connection` rather than
a worker. It drives a `ServerConnection` with `Router.dispatch`.

- Each `Connection` adds the socket, deadline, and read or write interest to
  its `ServerConnection`. Connections persist, and pipelined requests are
  answered without waiting for readiness. Each wakeup receives once, so a busy
  client takes turns.
- Each connection is allocated at accept, registered as its socket's `poll`
  context, and released at close. Its worker keeps them on a linked list for
  expiry.
- All workers watch the shared listener and accept once per readiness event.
  Level-triggered readiness spreads a backlog across workers, and one worker
  owns each connection, so connection state needs no lock. Darwin's
  `SO_REUSEPORT` does not balance connections between listeners.
- A connection is closed after 5 idle seconds between requests. A request has
  10 seconds from its first byte to its last response byte, however much the
  client sends, so trickling cannot hold it open. Requests over 64 KiB are
  dropped. After a final response the connection half-closes and discards input
  for at most 1 second.
- Handlers run on the event loop: one that blocks stalls that worker's
  connections.

```sh
cd demo/http-multi-threaded-event-loop-server && ace pkg run  # listens on 127.0.0.1:8080
```

## Known limitations and next improvements

HTTPS/TLS,
decompression, cookies, authentication policy, caching, proxies, upgrades,
client connection reuse/pooling, exposed trailers,
client deadlines, and cancellation are absent.
URL path and query remain separate views;
callers composing a request target provide destination storage explicitly.

## Design rationale

Why the rules in `spec/pkg/http.md` are shaped as they are.

- Stdlib: HTTP #17: explicit overloads preserve existing one-request behavior,
  make network work opt-in, and bound it in accordance with `PHILOSOPHY.md`
  Principles #2, #3, and #7.
- Stdlib: HTTP #17: keeping the framing core independent while composing
  concrete TCP operations at the ergonomic boundary preserves machine-visible
  storage, explicit errors, and the lack of hidden allocation without adding
  dependency inversion for a TCP-only protocol. Byte-oriented parsing matches
  HTTP and permits UTF-8 payloads to pass through unchanged without making
  Unicode text processing part of networking.
- Stdlib: HTTP #17: `ServerConnection` performs no I/O, like Python's h11, so
  every event loop shares the persistence and framing rules while keeping its
  own socket, timer, and thread policy. Keep-alive removes connection close as
  the end of a response, so the server must frame every response itself.
