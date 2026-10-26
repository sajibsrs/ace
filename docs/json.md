# json

[Documentation](README.md)

`json` parses a complete in-memory RFC 8259 document into a tagged `Value`,
and formats JSON text from runtime data with a `Formatter`. Parsing is
eager and whole-document: it reads one buffer, returns one value, and rejects
anything after it. Every failure is an explicit returned error. A `Parser`
owns the storage of every tree it parses and a `Formatter` owns the buffer it
writes, so one `wipe()` releases everything.

`Json` is the one-object surface: it parses a document and reads it by path,
and it writes JSON text. `Parser` and `Formatter` are the core it composes;
use them directly to reuse storage across many parses or to skip `Json`'s
extra call per write.

`json` is a standalone package at [`pkg/json`](../pkg/json), not a `std`
module. A consumer lists it under `[dependency]` in its own `ace.pkg` as
`json: 0.1.0`, found as the nearest `pkg/json`; the source then writes a plain
`import json`. See
[`demo/json-parse-file`](../demo/json-parse-file) and
[`demo/json-format-file`](../demo/json-format-file) for consuming examples.

## Values

```ace
Value: union {
    none,
    bool: bool,
    number: f64,
    string: string,
    array: []Value,
    object: []Pair,
}

Pair: struct {
    key: string,
    value: Value,
}
```

JSON `null` is `Value.none`, which is also the zero value. Every number is an
`f64`. Objects keep their pairs in source order, and duplicate keys are kept
rather than merged.

## Json

A `Json` holds two independent things: `root`, the tree `parse` produced,
which is input read by path; and the text its write calls produce, which is
output that `format` returns. `doc.wipe()` releases both, so `defer` it
right after `new`.

### Reading

```ace
import fmt
import json

main(): int {
    doc: json.Json = json.Json.new()
    defer doc.wipe()
    err: Error = doc.parse("{\"name\": \"ace\", \"tags\": [1, 2]}")
    if err != null { return 1 }

    fmt.println(doc.string("/name"))  // ace
    fmt.println(doc.number("/tags/1")) // 2
    return 0
}
```

`json.Json.new(): Json` makes an empty `Json`, and `doc.parse(text:
string): Error` parses `text` as one value into `doc.root`. On failure `root`
is `Value.none` and the error is a `json.ParseErr`; `doc.parse(text,
&diag)` also explains it, as for `Parser.parse`. Each `parse` reuses the
storage of the one before, so the previous `root` and every string read from
it are invalid afterwards. Parse many documents with one `Json`:

```ace
for i: usize in 0 .. texts.len {
    err: Error = doc.parse(texts[i])
    if err != null { continue }

    fmt.println(doc.string("/id"))
}
```

Reusing one `Json` takes about 36% less time per document than a fresh `Json`
for each on 114-byte documents, and about 21% less on 460-byte ones.

Values are read by [JSON Pointer](https://www.rfc-editor.org/rfc/rfc6901):
`""` is the whole tree, and each `/token` steps into an object member or an
array index. `~1` spells `/` and `~0` spells `~` in a key; an index is decimal
with no leading zero. An object step takes the first member with that key.

| Call | Returns |
| --- | --- |
| `get(path)` | `(Value, bool)`: the value, or `Value.none` and false when absent |
| `string(path)` | the string, or `null` when absent or not a string |
| `number(path)` | the number, or `0` when absent or not a number |
| `bool(path)` | the boolean, or `false` when absent or not a boolean |

Strings read from a `Json` are valid until the next `parse` or `wipe`. `root`
may be reassigned.
The pointer walk is the core function `json.get(tree: Value, path: string):
(Value, bool)`, which works on any tree, including one from a `Parser`.

### Writing

```ace
import json

main(): int {
    doc: json.Json = json.Json.new()
    defer doc.wipe()
    doc.object()
    doc.field("name", "ace")
    doc.field("version", 1)
    doc.end()
    text: string, err: Error = doc.format()
    if err != null { return 1 }

    // text is {"name":"ace","version":1}
    return 0
}
```

A `Json`'s write calls are exactly the `Formatter` calls below (`object`,
`array`, `end`, `key`, `none`, `value`, `field`), each forwarded to the
`Formatter` it holds, and `doc.format(): (string, Error)` is `finish`: the one value written, or the
first `json.FormatErr`. `format` returns only what the write calls wrote,
never `root` on its own, so writing a parsed tree back is explicit:

```ace
doc.value(doc.root)
text: string, err: Error = doc.format()
```

The text is borrowed from the `Json` and valid until `reset` or `wipe`.
`doc.reset()` discards the written text to start the next one and leaves
`root` unchanged. Each forwarded call costs one extra call: writing a
12-record, 648-byte document takes about 3.5% longer than the same calls on a
`Formatter`.

## Parsing

`json.Parser.new(): Parser` makes a parser and `parser.wipe()`
releases it. `parser.parse(text: string): (Value, Error)` parses `text` as
exactly one value. Surrounding space, tab, line feed, and carriage return are
skipped. On error it returns `Value.none` and an error from the `json.ParseErr`
domain:

| Error | Meaning |
| --- | --- |
| `unexpected_token` | a byte that cannot continue the document, including a raw control byte inside a string |
| `unterminated_string` | a string with no closing quote |
| `invalid_escape` | an unknown `\` escape, a malformed `\uXXXX`, or an unpaired UTF-16 surrogate |
| `invalid_number` | a malformed number, including a leading zero such as `01` |
| `depth` | more than 512 nested arrays or objects |
| `trailing_data` | non-whitespace bytes after the value |
| `empty_input` | no value at all |
| `allocation` | an allocation failed |

`parser.parse(text, diag: *fmt.Buffer)` also appends the failure's
context to `diag` (see [error context](std/fmt.md)), innermost first: what was
expected at which line:column (an unterminated string at its opening quote),
then one line per enclosing array element or object member:

```
invalid literal at 2:9
in key "a"
in array element 2
in key "items"
```

Strings are decoded: `\uXXXX` escapes, including surrogate pairs, become UTF-8.
Other bytes pass through unchanged and are not validated as UTF-8.

## Ownership

The result never aliases `text`, so the input buffer may be released as soon
as `parse` returns. Every string, array, and object lives in storage the
parser allocated for that call. A `Value` is valid until `reset` or
`wipe`; using one afterwards is invalid. Trees from repeated `parse`
calls accumulate and stay valid side by side until then, and a failed parse
leaves nothing to clean up. An empty string, array, or object holds no
storage. Release the parser with `defer` right after `Parser.new`:

```ace
import fmt
import json

main(): int {
    parser: json.Parser = json.Parser.new()
    defer parser.wipe()
    root: json.Value, err: Error = parser.parse("{\"name\": \"ace\", \"tags\": [1, 2]}")

    if err != null {
        fmt.printf("parse failed: %d\n", int(err.code))
        return 1
    }

    switch root {
        json.Value.object(pairs: []json.Pair) {
            fmt.printf("%d keys\n", int(pairs.len))
        }
        default {}
    }
    return 0
}
```

Read a value with `switch`; each payload binder is an independent copy, so
matching does not need write access to the value.

To parse many documents in turn, call `parser.reset()` between them. It
drops every tree the parser produced but keeps its storage, so the next parses
reuse it instead of allocating: about 24% faster than a fresh parser per
document on 139-byte documents. Without `reset`, trees accumulate until
`wipe`.

## Writing

`json.Formatter.new(): Formatter` makes a formatter and
`formatter.wipe()` releases it. A formatter writes compact
JSON one call at a time into a buffer it owns and grows. It needs no `Value`
tree, so records are written straight from runtime data of any shape or size,
and the compiler picks each `value` and `field` overload from the argument's
type:

```ace
import json

Reading: struct {
    sensor: string,
    celsius: f64,
    ok: bool,
}

write_reading(f: *json.Formatter, r: Reading) {
    f.object()
    f.field("sensor", r.sensor)
    f.field("celsius", r.celsius)
    f.field("ok", r.ok)
    f.end()
}

main(): int {
    readings: [2]Reading
    readings[0] = Reading{sensor = "north", celsius = 21.5, ok = true}
    readings[1] = Reading{sensor = "south", celsius = 19.0, ok = false}
    f: json.Formatter = json.Formatter.new()
    defer f.wipe()
    f.array()

    for i: usize in 0 .. readings.len {
        write_reading(&f, readings[i])
    }
    f.end()
    text: string, err: Error = f.finish()

    if err != null {
        return 1
    }
    // text is [{"sensor":"north","celsius":21.5,"ok":true},{"sensor":"south","celsius":19,"ok":false}]
    return 0
}
```

Mapping your own types to JSON is explicit: write one function per type, as
`write_reading` does. There is no reflection.

| Call | Writes |
| --- | --- |
| `object()`, `array()` | opens a container |
| `end()` | closes the innermost container, `}` or `]` as its kind requires |
| `key(name)` | an object member's name |
| `value(x)` | a value: `string`, `int`, `uint`, `usize`, `f64`, `bool`, or a whole `Value` tree |
| `none()` | `null` |
| `field(name, x)` | `key(name)` then `value(x)`, for the same value types |
| `finish()` | returns `(string, Error)`: the text of the one complete value, or the first error |
| `reset()` | discards the current document and error, keeping the buffer |

The first call picks the document's root: an object, an array, or a single
scalar. Integers are written with their exact digits. A parsed tree is written
back with `value(root)`.

Strings escape `"`, `\`, and control bytes below 0x20 (`\b`, `\f`, `\n`,
`\r`, `\t`, otherwise `\u00XX`); every other byte, including UTF-8, is
copied unchanged and not validated. An `f64` takes its shortest round-trip
spelling (`0.5`, `1e21`).

The first failure is kept and every later call does nothing, so a sequence of
writes needs one check at `finish`. The text `finish` returns is borrowed from
the formatter and is valid until `reset` or `wipe`. A formatter holds
one document at a time: call `formatter.reset()` before starting the next,
which keeps the buffer and so avoids an allocation per document (about 8%
faster than a fresh formatter per document on 139-byte documents). Starting a
second document without `reset` is a `structure` error. Errors come from the
`json.FormatErr` domain:

| Error | Meaning |
| --- | --- |
| `allocation` | the buffer could not grow |
| `depth` | more than 512 nested arrays or objects |
| `number` | a NaN or infinite `f64`, which JSON cannot spell |
| `structure` | the calls do not form exactly one value: a value where a key belongs, a key outside an object, a key with no value, `end` with nothing open, a second top-level value, or `finish` with a container open or nothing written |

## Tests

`pkg/json/tests` holds the package's own behavioral cases and runs under
`make test-packages`, not `make test`.
