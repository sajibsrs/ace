# json

[Documentation](README.md)

`json` parses a complete in-memory RFC 8259 document into a tagged `Json`
value, and formats JSON text from runtime data with a `Formatter`. Parsing is
eager and whole-document: it reads one buffer, returns one value, and rejects
anything after it. Every failure is an explicit returned error. A `Parser`
owns the storage of every tree it parses and a `Formatter` owns the buffer it
writes, so one `parser_wipe` or `formatter_wipe` releases everything.

`json` is a standalone package at [`pkg/json`](../pkg/json), not a `std`
module. A consumer lists it under `[dependencies]` in its own `ace.pkg` with a
relative path to `pkg/json`; the source then writes a plain `import json`. See
[`demo/json-parse-file`](../demo/json-parse-file) and
[`demo/json-format-file`](../demo/json-format-file) for consuming examples.

## Values

```ace
Json: union {
    none,
    bool: bool,
    number: f64,
    string: string,
    array: []Json,
    object: []Pair,
}

Pair: struct {
    key: string,
    value: Json,
}
```

JSON `null` is `Json.none`, which is also the zero value. Every number is an
`f64`. Objects keep their pairs in source order, and duplicate keys are kept
rather than merged.

## Parsing

`json.parser_new(): Parser` makes a parser and `json.parser_wipe(mut parser)`
releases it. `mut parser.parse(text: string): (Json, Error)` parses `text` as
exactly one value. Surrounding space, tab, line feed, and carriage return are
skipped. On error it returns `Json.none` and an error from the `json.ParseErr`
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

Strings are decoded: `\uXXXX` escapes, including surrogate pairs, become UTF-8.
Other bytes pass through unchanged and are not validated as UTF-8.

## Ownership

The result never aliases `text`, so the input buffer may be released as soon
as `parse` returns. Every string, array, and object lives in storage the
parser allocated for that call. A `Json` is valid until `reset` or
`parser_wipe`; using one afterwards is invalid. Trees from repeated `parse`
calls accumulate and stay valid side by side until then, and a failed parse
leaves nothing to clean up. An empty string, array, or object holds no
storage. Release the parser with `defer` right after `parser_new`:

```ace
import fmt
import json

main(): int {
    parser: json.Parser = json.parser_new()
    defer json.parser_wipe(mut parser)
    root: json.Json, err: Error = mut parser.parse("{\"name\": \"ace\", \"tags\": [1, 2]}")

    if err != null {
        fmt.printf("parse failed: %d\n", int(err.code))
        return 1
    }

    switch root {
        json.Json.object(pairs: []json.Pair) {
            fmt.printf("%d keys\n", int(pairs.len))
        }
        default {}
    }
    return 0
}
```

Read a value with `switch`; each payload binder is an independent copy, so
matching does not need write access to the value.

To parse many documents in turn, call `mut parser.reset()` between them. It
drops every tree the parser produced but keeps its storage, so the next parses
reuse it instead of allocating: about 24% faster than a fresh parser per
document on 139-byte documents. Without `reset`, trees accumulate until
`parser_wipe`.

## Writing

`json.formatter_new(): Formatter` makes a formatter and
`json.formatter_wipe(mut formatter)` releases it. A formatter writes compact
JSON one call at a time into a buffer it owns and grows. It needs no `Json`
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

write_reading(mut f: json.Formatter, r: Reading) {
    mut f.object()
    mut f.field("sensor", r.sensor)
    mut f.field("celsius", r.celsius)
    mut f.field("ok", r.ok)
    mut f.end()
}

main(): int {
    readings: [2]Reading
    readings[0] = Reading{sensor = "north", celsius = 21.5, ok = true}
    readings[1] = Reading{sensor = "south", celsius = 19.0, ok = false}
    f: json.Formatter = json.formatter_new()
    defer json.formatter_wipe(mut f)
    mut f.array()

    for i: usize in 0 .. readings.len {
        write_reading(mut f, readings[i])
    }
    mut f.end()
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
| `value(x)` | a value: `string`, `int`, `uint`, `usize`, `f64`, `bool`, or a whole `Json` tree |
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
the formatter and is valid until `reset` or `formatter_wipe`. A formatter holds
one document at a time: call `mut formatter.reset()` before starting the next,
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
