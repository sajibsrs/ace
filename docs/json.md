# json

[Documentation](README.md)

`json` parses a complete in-memory RFC 8259 document into a tagged `Json`
value, and formats a tree back to JSON text in caller-owned storage. Parsing
is eager and whole-document: it reads one buffer, returns one value, and
rejects anything after it. Every failure is an explicit returned error. A
`Parser` owns the storage of every tree it parses, so one `parser_wipe`
releases them all.

`json` is a standalone package at [`pkg/json`](../pkg/json), not a `std`
module. A consumer lists it under `[dependencies]` in its own `ace.pkg` with a
relative path to `pkg/json`; the source then writes a plain `import json`. See
[`demo/json-parse-file`](../demo/json-parse-file) for a consuming example.

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
parser allocated for that call. A `Json` is valid until `parser_wipe`; using
one afterwards is invalid. Trees from repeated `parse` calls accumulate and
stay valid side by side until then, and a failed parse leaves nothing to
clean up. An empty string, array, or object holds no storage. Release the
parser with `defer` right after `parser_new`:

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

## Writing

`json.format(value, output): (usize, Error)` writes a whole `Json` tree as
compact JSON into caller-owned storage and returns the byte count. It
allocates nothing:

```ace
import json

main(): int {
    parser: json.Parser = json.parser_new()
    defer json.parser_wipe(mut parser)
    root: json.Json, perr: Error = mut parser.parse("{ \"level\": \"info\", \"tags\": [\"a\", \"b\"] }")

    if perr != null {
        return 1
    }
    buf: [256]u8
    n: usize, err: Error = json.format(root, mut buf[..])

    if err != null {
        return 2
    }
    // buf[0..n] holds {"level":"info","tags":["a","b"]}
    return 0
}
```

Strings escape `"`, `\`, and control bytes below 0x20 (`\b`, `\f`, `\n`,
`\r`, `\t`, otherwise `\u00XX`); every other byte, including UTF-8, is
copied unchanged and not validated. Numbers use `str.format`, so an `f64`
takes its shortest round-trip spelling (`0.5`, `1e21`).

After a failure the contents of the output storage are unspecified. Errors
come from the `json.FormatErr` domain:

| Error | Meaning |
| --- | --- |
| `capacity` | the output storage is full |
| `depth` | more than 512 nested arrays or objects |
| `number` | a NaN or infinite `f64`, which JSON cannot spell |

A failed write cannot be resumed. When the output size is not known in
advance, call `format` again into a buffer twice the size on `capacity`.

## Tests

`pkg/json/tests` holds the package's own behavioral cases and runs under
`make test-packages`, not `make test`.
