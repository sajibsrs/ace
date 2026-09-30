# http

[Documentation](README.md)

`http` implements allocation-free HTTP/1.1 framing plus blocking one-shot GET
over plaintext TCP. The framing and parsing operations allocate nothing,
perform no I/O, and retain no caller buffer. Client DNS, connection, request,
response, and cleanup failures remain explicit in returned errors.

`http` is a standalone package at [`pkg/http`](../pkg/http), not a `std`
module — it has no compiler-owned mechanism tying it to the shipped `std/`
tree. A consumer depends on it like any other package (an `http: 0.1.0`
`[dependencies]` entry in its own `ace.pkg`, found as the nearest `pkg/http`);
the bare `import http` in source stays the same either way. See
[`demo/http-simple-client`](../demo/http-simple-client) and
[`demo/http-simple-server`](../demo/http-simple-server) for consuming
examples.

HTTPS/TLS support was removed pending a full redesign; see the TLS client
transport issue (#65). `Url::parse` still accepts an `https://` scheme
for syntax purposes, but every client entry point rejects it with
`UrlErr.invalid`.

`Url::parse(url): (Url, Error)` accepts absolute `http://` and `https://` URLs,
autodetecting the scheme from `url`'s prefix. The overload
`Url::parse(url, secure): (Url, Error)` forces the scheme instead of
autodetecting it. `Url { host, path, query, port }` contains byte views and a
host-order port.
The default port is 80 and the default path is the static `/` view. A query
without a written path is represented as that path plus a `query` view which
includes `?`. DNS names up to 253 bytes with labels up to 63 bytes and dotted
IPv4 hosts are accepted. User information, fragments, bracketed IPv6, controls,
empty or invalid ports, and other schemes return `UrlErr`. HTTP defaults to
port 80 and HTTPS to port 443. Path/query
bytes and percent escapes are unchanged.

`get(url, mut context)` accepts only `http://`. `ClientContext` owns reusable
HTTP workspace and policy, is created with `ClientContext::new`, and is
released with `wipe`.

```ace
import fmt
import http

main(): int {
    context: http.ClientContext = http.ClientContext::new()
    response: http.Response, err: Error = http.get("http://example.com/", mut context)
    if err != null {
        fmt.printf("request failed: %d\n", int(err.code))
        mut context.wipe()
        return 1
    }
    fmt.printf("status %d, %d bytes\n", int(response.status), int(response.body.len))
    mut context.wipe()
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
formatting, response parsing, error mapping, and TCP cleanup through one typed
transport exchange.

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
EOF, and status 101 are explicit failures. A framing conflict takes precedence
over a full header buffer (`framing`, not `capacity(headers)`). Header names are compared using
ASCII case folding; values and payload bytes otherwise pass through unchanged.

## Simple HTTP client demo

A runnable proof of concept lives at `demo/http-simple-client/main.ace`: it
calls `get(url, mut context)` directly, printing the response status and
body. Build and run it with:

```sh
./ace demo/http-simple-client/main.ace -o http-simple-client
./http-simple-client
```

## Server: blocking `serve`

`serve(endpoint, handle): Error` is the only server convenience: no
`ServerContext`, no poll queue, no session table, and no caller-visible
backlog or capacity tuning. It parses `endpoint`, binds a blocking `tcp`
listener, then loops forever, serially: accept one connection, read exactly
one request into a fixed internal buffer, call `handle(request): Response`,
format and write the response with a synthesized `Connection: close` (a
handler-supplied one is dropped along with the connection), close, repeat.
Buffer sizes are fixed and not configurable. `serve` returns only if the
listener itself cannot be created; any per-connection failure (parse error,
oversized request, mid-write disconnect) just drops that connection and the
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
calls `serve(endpoint, handle)` directly, with no `ServerContext`, event loop,
or explicit respond call — just an endpoint and a handler. Build and run it
with:

```sh
./ace demo/http-simple-server/main.ace -o http-simple-server
./http-simple-server  # listens on 127.0.0.1:8080
```

Verify it with any HTTP client, for example `curl http://127.0.0.1:8080/`.

## Known limitations and next improvements

HTTPS/TLS,
decompression, cookies, authentication policy, caching, proxies, upgrades,
connection reuse/pooling, exposed trailers,
deadlines, and cancellation are absent.
URL path and query remain separate views;
callers composing a request target provide destination storage explicitly.

