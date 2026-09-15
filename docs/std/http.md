# std.http

[Standard library](index.md)

`http` implements allocation-free HTTP/1.1 framing plus blocking one-shot GET
over plaintext TCP or TLS. The framing and parsing operations allocate nothing,
perform no I/O, and retain no caller buffer. Client DNS, connection, request,
response, and cleanup failures remain explicit in returned errors.

`parse_url(url): (Url, Error)` accepts absolute `http://` and `https://` URLs.
`Url { host, path, query, port }` contains byte views and a host-order port.
The default port is 80 and the default path is the static `/` view. A query
without a written path is represented as that path plus a `query` view which
includes `?`. DNS names up to 253 bytes with labels up to 63 bytes and dotted
IPv4 hosts are accepted. User information, fragments, bracketed IPv6, controls,
empty or invalid ports, and other schemes return `UrlErr`. HTTP defaults to
port 80 and HTTPS to port 443. Path/query
bytes and percent escapes are unchanged.

`get(url, mut context)` accepts only `http://`. `ClientContext` owns reusable
HTTP workspace and policy, is created with `client_context_new`, and is
released with `client_context_wipe`. This overload carries no TLS
external-component reference.

```ace
import fmt
import http

main(): int {
    context: http.ClientContext = http.client_context_new()
    response: http.Response, err: Error = http.get("http://example.com/", mut context)
    if err != null {
        fmt.printf("request failed: %d\n", int(err.code))
        http.client_context_wipe(mut context)
        return 1
    }
    fmt.printf("status %d, %d bytes\n", int(response.status), int(response.body.len))
    http.client_context_wipe(mut context)
    return 0
}
```

`get(url, mut client)` accepts either scheme. `Client` composes a `context`
with `tls: TlsConfig`, lazily retains ordinary and TLS workspace, and releases
it through `client_wipe`. Canonical `TlsConfig{}` uses system trust; assigning a
nonempty `client.tls.trust_anchor` selects one explicit DER root. HTTPS always
requires hostname verification and SNI.

Redirects remain opt-in. Assigning `RedirectPolicy { limit = 10 }` to
`context.redirects` or `client.context.redirects` follows 301, 302, 303, 307,
and 308 responses with valid
`Location` fields, resolving absolute HTTP/HTTPS URLs, scheme-relative URLs,
absolute paths, query-only references, and relative paths against the previous
URL. HTTP may upgrade to HTTPS; HTTPS-to-HTTP fails with
`RedirectErr.downgrade`. A chain requiring more than `limit` followed responses
fails with `RedirectErr.limit`. Missing or invalid locations return the original
3xx response. The canonical zero policy performs one request.

The two GET overloads share URL parsing, address iteration, request construction,
formatting, response parsing, error mapping, and TCP cleanup through one typed
transport exchange. BearSSL is the default bundled implementation selected by
the compiler, not a public type or API name.

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
request methods are public string constants such as `method_get` and
`method_patch`. Response statuses remain plain `u16` numbers. `status(code)`
returns the conventional phrase for a known code and `""` for an unknown code;
it does not restrict extension statuses in `100...999`.

`mut parser.parse(input, headers, body, eof)` on `RequestParser` and
`mut parser.parse(input, headers, body, eof, no_body)` on `ResponseParser`
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
EOF, and status 101 are explicit failures. Header names are compared using
ASCII case folding; values and payload bytes otherwise pass through unchanged.

## Server: `serve`

`server_context_new(endpoint, backlog, session_count, input_size, header_size,
body_size, output_size): (ServerContext, Error)` parses `endpoint` with
`net.parse4`, binds a nonblocking `tcp` listener, creates the `poll` queue, and
allocates `session_count` `Session` slots, each with its own fixed-size input,
header, body, and output storage. Capacities never grow after construction: a
request or response that does not fit its session closes only that session.
The overload `server_context_new(endpoint): (ServerContext, Error)` skips all
of that for a caller with no capacity preference, using fixed built-in
defaults: 512 backlog, 128 sessions, 8 KiB input/output, 64 headers, and
256 KiB body per session. `server_context_wipe(mut context)` closes every open session, the listener,
and the poll queue, and releases all storage.

`serve(mut context, timeout_ns): (Event, Error)` drives the server by exactly
one nonblocking step per call — at most one `poll.wait`, then one accept or
one session receive/send attempt — and returns one `Event`. When a single
`poll.wait` reports several ready descriptors, later `serve` calls drain them
before waiting again. There is no loop inside `http`: the caller's own loop
decides how many times to call `serve` and when to stop, so one slow
connection never blocks another. `Event` is the tagged union `none`,
`accepted(index)`, `request(index)`, `sent(index)`, `disconnected(index)`, and
`failed(Failure { index, err })`. A nonnull `Error` returned from `serve`
itself (as opposed to `Event.failed`) is a listener, poll-queue, or wait-level
failure and ends the server.

On `Event.request(index)`, `context.sessions[index].request` holds the parsed
`Request`; its views stay valid only until the next call that touches that
session. The caller inspects it, builds a `Response`, and calls
`respond(mut context, index, response): Error`, which formats the response
into the session's output storage, synthesizes exactly one `Connection: close`
header, and rejects a handler-supplied `Connection` header. Each session
closes and its slot returns to `free` once `Event.sent` reports the response
fully sent; a full session table simply leaves further connections in the OS
listen queue until a slot recycles.

## Minimal HTTP server demo

A runnable proof of concept lives at `demo/http-server/main.ace`: it opens a
listener with the low-effort `server_context_new(endpoint)` overload, loops on
`serve`, logs each `Event`, and answers `GET /` with a static body and
anything else with `404`. Build and run it with:

```sh
./ace demo/http-server/main.ace -o http-server
./http-server               # listens on 127.0.0.1:8080
./http-server 0.0.0.0:9000  # or an explicit host:port
```

Verify it with any HTTP client, for example `curl http://127.0.0.1:8080/`.

## Known limitations and next improvements

Redirects,
decompression, cookies, authentication policy, caching, proxies, upgrades,
connection reuse/pooling, exposed trailers,
deadlines, and cancellation are absent. TLS is blocking, has no server mode,
and BearSSL 0.6 is
limited to TLS 1.2; explicit override mode currently accepts one DER anchor.
URL path and query remain separate views;
callers composing a request target provide destination storage explicitly. Unicode helpers,
`rune`, and `yarn` are not required by this byte-oriented layer.

