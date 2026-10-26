## Stdlib: HTTP (`pkg/http`)

Stdlib #17, numbered with the standard library it began in; the chapter's
shared rules are in `spec/std/README.md`.

17. HTTP (protocol core implemented; transport ergonomics carved) — `http` is
    the allocation-free HTTP/1.1 protocol layer. Its parser and formatter core
    performs no network I/O; its higher-level operations compose the concrete
    `net`, `tcp`, and `poll` modules directly, with no transport contract or
    module split. HTTP
    transports bytes: it parses protocol delimiters, method and header-name
    grammar, decimal fields, and case-insensitive names as ASCII, while header
    values and bodies remain byte views. It performs no Unicode decoding,
    validation, normalization, or case folding. A
    later owned-string convenience may compose with `http`, but the base API
    keeps every capacity and allocation decision with the caller.
    - Endpoint text and URLs are different values. The general networking
      convenience is `net.parse4(endpoint: string): (Addr4, bool)`, which
      accepts exactly a dotted-decimal IPv4 address and required decimal port
      separated by one colon, for example `127.0.0.1:8080`. It consumes the
      complete nonempty input, accepts four decimal octets in `0...255` and a
      port in `0...65535`, performs no name resolution, and returns canonical
      zero plus false for invalid text. Hostname resolution remains
      `net.resolve4`; endpoint formatting and IPv6 literals are additive.
    - `Url: struct { host: string, path: string, query: string, port: u16 }`
      contains zero-copy views into the input URL and a host-order port.
      Separating path and query makes a query-only URL representable without
      allocation: its path is the static `/` view and its query retains the
      leading `?`. `Url::parse(url:
      string): (Url, Error)` accepts an absolute `http` or `https` URL with a
      nonempty authority: `http://host` or `https://host`, an optional
      explicit decimal port, and an optional path/query request target. An
      omitted port becomes 80 for `http` and 443 for `https`; an omitted path
      becomes `/`; a query without a path therefore composes as `/?`. The host
      may be an ASCII DNS name or dotted IPv4 literal. DNS names are limited
      to 253 bytes with labels of at most 63 bytes. User information,
      fragments, bracketed IPv6 literals, embedded control/NUL bytes, empty or
      out-of-range ports, and every other scheme fail with `UrlErr`. Both
      schemes share this one parser even though the plaintext-only
      `get(url, &context)` rejects an `https://` URL before connecting —
      it never silently falls back to plaintext — through its own
      scheme-forced `Url::parse(url, false)` overload call. Parsing
      allocates nothing, performs no DNS lookup, preserves path/query bytes
      and percent escapes exactly, and never percent-decodes or
      Unicode-normalizes them.
    - The protocol core uses `Header: struct { name: string, value: string }`
      and caller-provided header/body/output storage. Methods are strings rather
      than a closed enum so extension methods need no API change. Serialization
      rejects invalid method and header-name bytes plus CR, LF, and NUL in
      caller-supplied header values. It writes a complete HTTP/1.1 message or
      reports insufficient capacity without emitting a partial message.
      Parsing is incremental state supplied and retained by the caller, accepts
      arbitrary receive fragmentation, returns zero-copy header/value views
      into caller-owned stable input storage, and reports one explicit progress,
      completion, capacity, syntax, or framing result per call.
      The public message surface is `Request { method, target, headers, body }`
      and `Response { version_minor = 1, status = 200, reason = null, headers =
      null, body = null }`. Request keeps its essential method and target
      required; response construction may name only the fields it overrides.
      Response fields remain directly accessible; getter/setter pairs add no
      protection while fields are visible and remain additive only after a
      concrete need. A null response reason selects the module's static
      conventional phrase for a known status, or the empty phrase for an
      unknown valid status. A non-null reason, including `""`, is emitted
      exactly after validation. Parsing preserves the received reason.
      Their
      read-only `format(output: []u8): (usize, Error)` methods write one
      complete message or leave insufficient output unchanged. Output must not
      overlap storage reachable through the message. Formatting preserves the
      supplied headers exactly and synthesizes neither `Host` nor framing
      fields; `FormatErr` distinguishes output capacity and invalid start-line
      or header fields. The incremental parser surface is
      `RequestParser.parse(self, input, headers, body, eof): RequestResult` and
      `ResponseParser.parse(self, input, headers, body, eof, no_body):
      ResponseResult`, with public phase, framing, capacity, and cumulative
      progress support types for each parser. Each call resumes from saved
      phase, line-scan, framing, header, and body progress; already-consumed
      bytes are not reparsed or recopied. `input` is the same stable complete
      occupied prefix extended with newly received bytes on every call. The
      parser retains no buffer pointer, while the caller preserves previously
      written header and body storage until completion. Reinitialize the parser
      value before starting another message.
    - Initial request framing supports `Content-Length`, `Transfer-Encoding:
      chunked`, and an empty body when neither is present. Initial response
      framing additionally supports explicit no-body responses and
      connection-close bodies. Chunk syntax is decoded into caller-owned body
      storage; extensions are ignored after syntactic validation and trailers
      are parsed for framing but not exposed initially. Conflicting or invalid
      framing is an error. Informational `1xx` responses are consumed before the
      one final response. Header names compare case-insensitively as ASCII;
      field values are otherwise preserved.
    - The nine standard request methods are public `string` constants spelled
      as their method tokens (`GET`, `POST`, …). Statuses remain plain `u16`
      numbers: response handling accepts every status in `100...999`, without
      duplicating the protocol catalogue as public numeric constants.
      `status(code: u16): string` returns the static conventional
      reason phrase for a known status code and `""` for an unknown code. The
      lookup is allocation-free and does not classify a response as an error;
      `Response.reason` remains the phrase actually supplied on the wire.
    - Blocking TCP socket convenience is the exact overload set
      `request(socket: net.Socket, request: Request, scratch: []u8): Error`,
      `request(socket: net.Socket, input: []u8, headers: []Header, body:
      []u8): (Request, Error)`, `response(socket: net.Socket, response:
      Response, scratch: []u8): Error`, and `response(socket: net.Socket,
      input: []u8, headers: []Header, body: []u8, no_body: bool):
      (Response, Error)`. The write forms format completely before performing
      all-byte sends; the read forms extend one stable occupied input prefix
      until the corresponding parser completes. They require blocking TCP
      sockets, retry interrupted I/O, preserve lower-layer error domains, and
      retain neither socket nor buffer. `no_body` makes HEAD-style response
      semantics explicit. Connection creation, DNS, retry, TLS, reuse, and
      keep-alive policy remain with the client caller.
    - One-shot client ergonomics use one owning entry point.
      `Storage: struct { addresses: []net.Addr4, output: []u8, target:
      []u8, input: []u8, headers: []Header, body: []u8, url:
      []u8 = null }`
      remains the internal neutral workspace for one HTTP exchange.
      `ClientContext: struct { storage: []Storage, redirects: RedirectPolicy }`
      owns and grows the plaintext workspace through `mem` and carries HTTP
      policy. `ClientContext.new` constructs it and `wipe` releases it. The
      exact public overload is
      `get(url: string, context: *ClientContext): (Response, Error)`, which
      accepts only plaintext HTTP. Allocation failure is explicit.
      Returned views remain valid only until the corresponding context
      is next mutated or wiped. This operation
      parses the absolute plaintext URL, resolves its host into the supplied address
      storage, tries resolved addresses in order with a fresh blocking TCP
      connection, performs exactly one request and response, and closes the
      connection before returning. `get` emits GET with no body. `post` emits
      POST with the supplied bytes and requires a valid nonempty content type.
      `get` synthesizes exactly `Host` and `Connection: close`; `post` also
      synthesizes `Content-Type` and `Content-Length`. Without an explicit
      redirect policy there is no
      redirect, retry-after-response, connection reuse, authentication, cookie,
      compression, proxy, or user-header policy. Request formatting,
      response parsing, DNS, TCP, close, and capacity failures preserve the
      existing error domains, with an unsuccessful address sequence returning
      the final connection error.
    - Parser failures mapped through those conveniences use the shared public
      `SyntaxErr`, `FramingErr`, and `CapacityErr` domains. Syntax and framing
      variants map to their matching domain, and premature EOF is framing
      failure. `CapacityErr` codes are the public `i32` constants
      `capacity_input`, `capacity_headers`, and `capacity_body`. Formatter
      failures remain `FormatErr`; lower-layer socket/TCP errors retain
      their original domains and codes.
    - Server convenience is exactly one blocking overload:
      `serve(endpoint: string, context: *Context, router: Router): Error`
      parses `endpoint` through `net.parse4`, binds a blocking listener, then
      loops forever, serially: accept one connection, read exactly one
      request into `context`'s buffers, dispatch it through `router`, format
      and write its response, close, repeat. `Router.handle(method, path,
      handler)` registers a handler for an exact method and a path-segment
      prefix (`"/home"` matches `/home` and `/home/about` but not
      `/homework`); `Router.dispatch` picks the longest matching registered
      prefix, ignoring any `?query` suffix, and returns a bare 404 when
      nothing matches. `Context` owns the `input`, `headers`, `body`, and
      `output` buffers used across every connection; each starts empty and
      grows through `mem.grow` as a request or response demands, with no
      size cap. `Context.backlog` (default 512) is the one plain, non-growing
      field, passed once to `tcp.listen` when `serve` binds `endpoint`; it is
      not adjusted at runtime. There is no poll queue or session table.
      `serve` returns only if the listener itself cannot be created; any
      per-connection failure
      (parse error, allocation failure, mid-write disconnect) just drops
      that connection and the loop continues. Response serialization adds
      exactly one `Connection: close` field and drops a handler-supplied
      `Connection` field along with the connection, while ordinary
      `Request.format` and `Response.format` continue to synthesize no
      headers. There is no concurrency — one connection is handled at a
      time — so this overload suits demos and local testing rather than
      serving concurrent traffic. A caller-driven nonblocking server core
      existed at one point but was implemented without a carve; it was
      removed pending a fresh carve of how much server mechanism belongs in
      the language versus a framework built on top (issue #67).
      Concurrency, persistent connections, and TLS server mode remain
      additive or deferred to that future carve.
    - `http` is plaintext-only; HTTPS/TLS support was removed (see the TLS
      client transport, issue #65) and is not part of the current
      surface. `Url` parsing still accepts an `https://` scheme for syntax
      purposes, but every client entry point rejects it with `UrlErr.invalid`
      since no secure transport exists to serve it.
    - Redirect following is opt-in context policy.
      `RedirectPolicy: struct { limit: usize = 0 }` is stored as
      `context.redirects`; zero performs exactly one exchange, while a nonzero
      value opts into at most that many followed responses.
      Statuses 301, 302, 303, 307, and 308 are followed only with a nonempty,
      valid `Location`; otherwise the ordinary 3xx response is returned.
      Absolute HTTP/HTTPS, scheme-relative, absolute-path, query-only, and
      path-relative references resolve against the preceding URL, but an
      HTTPS redirect target fails with `UrlErr.invalid` under the current
      plaintext-only client.
      Encountering another followable response after `limit` hops fails with
      `RedirectErr.limit`, including at a zero limit. The client reuses explicit
      owned URL workspace so each location is stable before response input is
      reused. Cookies, custom headers, authentication, connection reuse, and
      session state remain deferred.
    - Known initial limitations and next improvements: content
      decoding, cookies, authentication policy, caching, proxies, upgrades,
      multipart helpers, and exposed trailers are not part of the protocol core.
      Client concerns such as connection reuse, pooling, deadlines, and
      cancellation remain outside the initial conveniences. Each proof updates
      the user guide and compiler-checked or executable fixtures only for
      behavior that has actually landed.
