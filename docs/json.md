# json

[Documentation](README.md)

`json` parses a complete in-memory RFC 8259 document into a tagged `Json`
value, and formats a tree back to JSON text in caller-owned storage. Parsing
is eager and whole-document: it reads one buffer, returns one value, and
rejects anything after it. Every failure is an explicit returned error, and a
failed parse releases everything it allocated.

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

`json.parse(text: string): (Json, Error)` parses `text` as exactly one value.
Surrounding space, tab, line feed, and carriage return are skipped. On error it
returns `Json.none` and an error from the `json.JsonErr` domain:

| Error | Meaning |
| --- | --- |
| `unexpected_token` | a byte that cannot continue the document, including a raw control byte inside a string |
| `unterminated_string` | a string with no closing quote |
| `invalid_escape` | an unknown `\` escape, a malformed `\uXXXX`, or an unpaired UTF-16 surrogate |
| `invalid_number` | a malformed number, including a leading zero such as `01` |
| `depth_exceeded` | more than 512 nested arrays or objects |
| `trailing_data` | non-whitespace bytes after the value |
| `empty_input` | no value at all |
| `allocation` | an allocation failed |

Strings are decoded: `\uXXXX` escapes, including surrogate pairs, become UTF-8.
Other bytes pass through unchanged and are not validated as UTF-8.

## Ownership

The result never aliases `text`. Each string, array, and object is its own heap
allocation, so the input buffer may be released as soon as `parse` returns. An
empty string, array, or object holds no allocation.

`json.wipe(value)` recursively releases every allocation a value owns, keys
included. It is a no-op for `none`, `bool`, and `number`, and it is safe on the
`Json.none` returned by a failed parse, so cleanup can be deferred before the
error check:

```ace
import fmt
import json

main(): int {
    root: json.Json, err: Error = json.parse("{\"name\": \"ace\", \"tags\": [1, 2]}")
    defer json.wipe(root)

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
    root: json.Json, perr: Error = json.parse("{ \"level\": \"info\", \"tags\": [\"a\", \"b\"] }")
    defer json.wipe(root)

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
