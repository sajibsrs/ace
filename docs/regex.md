# regex

[Documentation](README.md)

`regex` is a byte-oriented regular-expression package. A pattern is a byte
string and every construct matches bytes, never decoded code points. Matching
is leftmost-first, as in Perl and RE2, and runs as a Pike VM in time linear in
the text whatever the pattern, so there is no catastrophic backtracking; the
price is that backreferences and lookaround do not exist.

Most code needs only `Regex`, the convenience layer. It is built entirely
from the public core beneath it — `Syntax` (the parsed tree), `Program` (the
compiled instructions), and `Matcher` (the VM that runs a program) — so a
caller who needs different control can compose those directly.

`regex` is a standalone package at [`pkg/regex`](../pkg/regex), not a `std`
module. A consumer lists it under `[dependencies]` in its own `ace.pkg` as
`regex: 0.1.0`, found as the nearest `pkg/regex`; the source then writes a
plain `import regex`.
See [`demo/regex-search-file`](../demo/regex-search-file) for a consuming
example.

## Matching

```ace
re: regex.Regex, err: Error = regex.Regex::new("(\\d+)-(\\d+)")
defer mut re.wipe()
if err != null { return 1 }   // a regex.ParseErr or a regex.CompileErr

if mut re.find("call 12-345", 0) {
    fmt.println(re.group(2))           // 345
}

at: usize = 0
for mut re.next(text, mut at) {
    fmt.println(re.group(0))           // every match in turn
}
```

`Regex::new` parses and compiles the pattern and sizes every buffer a search
needs, so it is the only call that allocates or fails. When it fails with a
`ParseErr`, `Syntax::parse` on the same pattern reports the byte offset. A
search reuses that scratch state, which is why each search method takes
`mut self`; none of them allocates or can fail.

| Method                          | Result                                                     |
| ------------------------------- | ---------------------------------------------------------- |
| `match(text): bool`             | whether the pattern matches anywhere                       |
| `find(text, from): bool`        | whether a leftmost-first match starts at `from` or later   |
| `next(text, mut at): bool`      | `find` from `at`, then moves `at` past the match           |
| `count(text): usize`            | the number of matches `next` visits                        |
| `group(k): string`              | the text of group k of the last match, 0 the whole match   |
| `span(k): Span`                 | the bytes `start..end` of that group                       |
| `groups(): u32`                 | the number of capturing groups                             |

`find` and `next` record the match they find; `group` and `span` read it back
until the next search. `group` returns a view into the searched text, valid
only while that text is. A group that took no part in the match, a `k` past
`groups()`, or a search that found nothing gives a null `group` and a `span`
of `UNSET` ends; a group that matched the empty string is an empty non-null
view. `match` stops at the first match any thread reaches rather than the
leftmost-first one, so it records nothing.

Inside a repetition a group reports its last iteration: `(a|b)*` on `ab` gives
group 1 = `1..2`. An iteration that matches only the empty string does not
count, so `(a*)*` on `b` matches the empty span `0..0` with group 1 unset;
PCRE instead reports group 1 as `0..0`.

A search takes the complete text and a start byte rather than a subslice, so
`^`, `$`, and `\b` see the text's real ends and the bytes around the start:
`^b` does not match `ab` from byte 1, and `\bb` does not either, since `a`
before it is a word byte. `next` moves `at` to the end of each match, or one
byte past an empty one, so a loop over it visits every match once and ends.

Leftmost-first means the earliest starting position wins, and among matches
starting there the pattern's own preferences decide: the left alternative
first, greedy repetitions as long as possible, lazy ones as short as possible.
`a|ab` on `ab` matches `a`, where POSIX leftmost-longest would give `ab`.

### How matching works

The program is run by a Pike VM. A thread is an instruction index plus a row
of capture positions. All threads advance together, one input byte per step,
in a queue kept in priority order; the queue is a sparse set, so no
instruction is queued twice in a step and clearing it costs nothing. That
bound, at most one thread per instruction, is what makes matching linear: the
cost is O(text length × program size), and `(a|aa)*c` against a hundred
thousand `a`s finishes like any other search.

A new thread is seeded at each position with the lowest priority until a match
is found, so a thread that started earlier always outranks one that started
later, and one pass over the text yields the leftmost-first match. When a
thread reaches `match`, every lower-priority thread is dropped. `Regex.match`
instead stops at the first match any thread reaches.

## Parsing

```ace
s: regex.Syntax, err: Error, at: usize = regex.Syntax::parse("a(b|c)*")
defer mut s.wipe()

if err != null {
    // err is a regex.ParseErr; `at` is the byte offset of the problem
}
```

`Syntax::parse` reads the complete pattern. On success the offset is zero. On
failure the returned `Syntax` holds no storage and the offset names the byte at
which the error was found, with three exceptions that point at something more
useful than the end of the pattern: an unclosed group reports its `(`, an
unterminated class its `[`, and an invalid repetition count its `{`. An invalid
escape reports its backslash.

| `ParseErr`           | Example           | Offset |
| -------------------- | ----------------- | ------ |
| `missing_paren`      | `a(b(c)`          | 1      |
| `unexpected_paren`   | `ab)`             | 2      |
| `missing_operand`    | `a**`, `{2}`      | 2, 0   |
| `invalid_repeat`     | `a{3,1}`          | 1      |
| `invalid_range`      | `[z-a]`           | 1      |
| `unterminated_class` | `[abc`            | 0      |
| `invalid_escape`     | `\q`              | 0      |
| `depth`              | 513 nested groups | 512    |
| `allocation`         | —                 | —      |

## Syntax

| Form                    | Meaning                                          |
| ----------------------- | ------------------------------------------------ |
| `a`                     | the byte `a`                                     |
| `.`                     | any byte except `\n`                             |
| `^`, `$`                | start, end of input                              |
| `\b`, `\B`              | word boundary, not a word boundary               |
| `xy`, `x\|y`            | concatenation, alternation (left branch first)   |
| `(x)`, `(?:x)`          | capturing group, non-capturing group             |
| `x*`, `x+`, `x?`        | zero or more, one or more, zero or one           |
| `x{n}`, `x{n,}`, `x{n,m}` | counted repetition, `n` and `m` at most 1000   |
| `x*?`, `x{n,m}?`, ...   | lazy form of any repetition                      |
| `[abc]`, `[a-z]`, `[^a]` | class, range, negated class                     |
| `\d`, `\w`, `\s`        | `[0-9]`, `[0-9A-Za-z_]`, `[\t\n\f\r ]`           |
| `\D`, `\W`, `\S`        | their complements                                |
| `\n`, `\t`, `\r`, `\xHH` | newline, tab, carriage return, byte by hex      |

The parser is deliberately strict. A metacharacter is literal only when
escaped: `\\ \. \+ \* \? \( \) \| \[ \] \{ \} \^ \$ \-`. Any other escape is an
error, including digits, so backreferences can never be written by accident.
Inside a class, `]` must be written `\]`, a `-` is literal only first or last,
and `\d \w \s` and their complements may appear but not as a range endpoint.
A `{` always starts a repetition count; `}` and `]` outside a class are
ordinary bytes. Repeating a repetition (`a**`) is rejected rather than read as
nesting; group it instead. Empty alternatives (`a|`) and empty groups (`()`)
are valid and match the empty string. Groups are numbered from 1 in order of
their opening parentheses; `Syntax.groups` is the count.

## Tree

```ace
Node: union {
    empty, any, bol, eol, word, not_word,
    byte: u8,
    class: u32,        // index into Syntax.classes
    group: Group,      // { index: u32, child: u32 }; index 0 is non-capturing
    repeat: Repeat,    // { child: u32, min: u32, max: u32, greedy: bool }
    concat: List,      // { first: u32, count: u32 } into Syntax.children
    alt: List,
}
```

Every node sits in `Syntax.nodes` and names its children by `u32` index, so a
tree of any size is a handful of allocations and one `wipe()` frees it.
Concatenation and alternation are n-ary: a `List` is a run of
`Syntax.children`. Tree depth therefore grows only with group nesting, which is
capped at 512, never with pattern length. A repetition with no upper bound has
`max == regex.UNBOUNDED`. A `Class` is a 256-bit set stored as `[4]u64`;
`Class.has(byte)` tests membership.

`Syntax.format(destination)` writes a canonical dump of the tree into
caller-owned storage and returns its length and `true`, or zero and `false`
when the storage is too small. It exists for tests and diagnostics:

```
a(b|c)*?   cat{lit{a} star?{cap1{alt{lit{b} lit{c}}}}}
[a-c_]     cc{_ a-c}
a{2,5}     rep{2,5 lit{a}}
```

## Program

```ace
p: regex.Program, err: Error = regex.Program::compile(s)
defer mut p.wipe()
```

`Program::compile` lowers a syntax tree to a flat instruction array. The
program copies the tree's classes, so the `Syntax` may be wiped as soon as
compilation returns. A failed compilation holds no storage.

```ace
Inst: union {
    match,
    byte: u8, any, class: u32,        // consume one byte
    bol, eol, word, not_word,         // assertions
    save: u32,                        // record the position in a capture slot
    jmp: u32,
    split: Fork,                      // { x: u32, y: u32 }, x preferred
}
```

Execution starts at instruction 0. The program itself is anchored: it
matches only at the position a thread starts from, and searching comes from
the VM seeding a thread at every position. `Program.slots` is the number of
capture positions, two per group: slots 0 and 1 bound the whole match, and
group k uses slots 2k and 2k+1.

Leftmost-first priority is encoded entirely in `split`: `x` is preferred over
`y`. A greedy repetition prefers its body, a lazy one its exit, and an
alternation prefers its left branch.

| Pattern  | Program                                                        |
| -------- | -------------------------------------------------------------- |
| `x\|y`   | `split L1 L2` · `L1: x` · `jmp E` · `L2: y` · `E:`             |
| `x*`     | `L: split B E` · `B: x` · `jmp L` · `E:`                       |
| `x+`     | `L: x` · `split L E` · `E:`                                    |
| `x?`     | `split B E` · `B: x` · `E:`                                    |
| `x{m,n}` | m copies of `x`, then n−m nested optional copies `(x(x)?)?`    |
| `x{m,}`  | m−1 copies of `x`, then `x+`                                   |
| `(x)`    | `save 2k` · `x` · `save 2k+1`                                  |

The optional copies of a counted repetition all skip straight to its end
rather than to the next copy, so each copy is reachable by one path only.

A program holds at most 65,536 instructions. Counted repetitions multiply
(`((a{1000}){1000}){1000}` would need a billion), so compilation stops with
`CompileErr.too_large` once the limit is reached.

`Program.format(destination)` writes one instruction per line for tests and
diagnostics, with the same return convention as `Syntax.format`:

```
a{2,4}   0 save 0
         1 byte a
         2 byte a
         3 split 4 7
         4 byte a
         5 split 6 7
         6 byte a
         7 save 1
         8 match
```

## Matcher

```ace
m: regex.Matcher, err: Error = regex.Matcher::new(p)
defer mut m.wipe()

if mut m.run(p, text, 0, false) {
    // m.best[2k] .. m.best[2k + 1] bound group k
}
```

`Matcher::new` sizes every buffer the VM needs from a program, so
`run(code, text, from, earliest)` never allocates or fails. It searches from
`from` and returns whether a match was found; `earliest` stops at the first
match any thread reaches instead of the leftmost-first one. After a
leftmost-first run, `best` holds the match's `Program.slots` capture
positions, `UNSET` for a group that took no part. One `Program` may be run by
several `Matcher`s, each sized from it.
