# Control flow

Ace control flow is statement-based. Conditions require `bool`; `if`, `switch`,
and `for` do not produce values.

## Conditions

`if`, `else if`, and `else` use blocks. `else` must remain on the same physical
line as the preceding `}`.

```ace
if value < 0 {
    category = -1
} else if value == 0 {
    category = 0
} else {
    category = 1
}
```

Comparisons may be chained. Each operand is evaluated once:

```ace
inside: bool = 0 <= value < limit
```

`switch` compares one scalar value against cases and uses `default` for the
catch-all case, which must be the last case. Cases do not fall through
implicitly. A non-final case may end with the contextual statement
`fallthrough` to continue directly into the immediately following case without
testing that case's pattern:

```ace
switch value {
    1 {
        count += 1
        fallthrough
    }
    2 {
        count += 2
    }
    default {}
}
```

The switched value is a tagged union or a type with `==`; tuples, strings,
arrays, slices, and structs are rejected. A case lists its values on one line,
or, when they do not fit, inside parentheses across lines (Syntax #7):

```ace
switch kind {
    (TokKind.ident, TokKind.int, TokKind.float,
     TokKind.rparen, TokKind.rbrack) { return true }
    default {}
}
```

A parenthesized list on one line is rejected, so each layout has one
spelling; `(1 + 2)` alone stays grouping.

A case value may be a range `lo .. hi` over an integer or `char` switch value.
It is half-open like every range: it matches `lo` up to, not including, `hi`,
and reversed bounds select the same set. The end bound of a character range is
the next character, so write `'0' .. '9' + 1` for the digits:

```ace
switch c {
    '0' .. '9' + 1 { return 1 }
    'a' .. 'z' + 1, 'A' .. 'Z' + 1 { return 2 }
    default { return 0 }
}
```

Bounds are constants of the switch value's type. A range over a float, string,
or enum, a non-constant bound, and an empty range (`3 .. 3`) are errors. A value
matched by two case values or ranges is an error, so case order never decides
which case runs. A range ending at the type's maximum cannot be written, since
`255 + 1` overflows `u8`; list those values singly or test them in an `if`.

`fallthrough` takes no target or condition and must be the case's final
statement. It is rejected in the final case. The spelling remains an ordinary
identifier outside this exact statement position.

`fallthrough` is rejected in tagged-union switches. A union case is an active
variant pattern, so entering the next case without a match would contradict its
pattern; retesting it would never match a different active variant.

For a tagged union, cases name variants and payload variants bind an explicitly
typed local copy. The switch must cover every variant or include `default`:

```ace
switch event {
    Event.quit {
        return
    }
    Event.key(code: u32) {
        code += 1
    }
    default {}
}
```

## Ranges

`a .. b` is half-open and endpoint-ordered. The endpoints select the same
numeric interval regardless of order, while their written order selects the
traversal direction:

```ace
for i: int in 0 .. 4 {
    // 0, 1, 2, 3
}

for i: int in 4 .. 0 {
    // 3, 2, 1, 0
}
```

Equal endpoints produce an empty range. The first endpoint is evaluated once,
then the second endpoint once, before the loop. Assigning to the loop variable
does not alter the internal cursor.

A loop that only repeats its body omits the binder. The counter then has the
range's bound type (`i64` when both bounds are untyped literals), and the
bounds and step are evaluated exactly as with a binder:

```ace
for 0 .. n {
    // runs n times
}
```

Only ranges may omit the binder: `for values { }` would be a condition head,
so a sequence loop that ignores its elements iterates `0 .. values.len`.

An optional positive step follows the second endpoint. `step` is contextual,
not reserved, so it remains available as an ordinary identifier:

```ace
step: int = 3

for i: int in 0 .. 9 step step {
    // 0, 3, 6
}

for i: int in 9 .. 0 step step {
    // 6, 3, 0
}
```

The step has exactly the bounds' integer type and may be an arbitrary
expression. It is evaluated once after both endpoints, including for an empty
range. A statically known zero or negative step is rejected. Runtime-computed
steps must also be positive: debug output tests the step once at loop entry
and aborts with `file:line:col: range step violation` naming the step, while
release output assumes the precondition and adds no check.

Range bounds are ordinary captured values, not aliases to expressions used to
produce them. If a slice variable is resliced or reassigned inside an index
range loop, the captured endpoint does not change, while later indexing uses
the slice's current descriptor. Debug output applies the ordinary dynamic
index guard at that access; release output assumes the current slice is large
enough. Direct sequence iteration uses its compiler-controlled pointer-bump
loop and needs no per-element guard.

## Loop heads

One keyword carries every loop. Beside the range and sequence forms, `for`
takes a condition-only head, an empty infinite head, and a three-clause head:

```ace
running: bool = true
for running {
    running = false
}

for {
    break
}

for i: int = 0; i < 4; i += 1 {
    total += i
}
```

The three clauses share one physical line, and `;` is valid only between
for-head clauses — statements themselves still end at newlines, with no
semicolons. An omitted condition is always true (`for ;;` loops forever), and
a present condition must be `bool`.

The init clause runs once before the first condition test, and its scopes
cover the condition, the post clause, and the body — a variable declared there
is invisible outside the loop. The init may also be an assignment or an
expression statement:

```ace
n: int = 0
for n = 1; n < 3; n += 1 {
    total += n
}
```

The post clause accepts assignments and expression statements. `continue`
runs the post clause, then the condition — so a three-clause loop always
advances on a `continue`. A head declaration without an initializer follows
the ordinary definite-initialization rule and must be assigned before any
read.

## Labels on loops

A label attaches to a `for` of any head form:

```ace
outer: for i: int in 0 .. 3 {
    inner: for j: int in 0 .. 3 {
        if j == 2 {
            continue inner
        }
        if i == 1 {
            continue outer
        }
        if i == 2 {
            break outer
        }
    }
}
```

Labels share one namespace with labeled blocks. Plain `break` and `continue`
bind to the nearest `for`; `break label` exits that loop; `continue label` on
a loop starts its next iteration — distinct from the block-restart meaning
`continue label` has on a labeled block. Only enclosing labels may be named.

## Arrays and slices

A one-binder sequence loop exposes each element value:

```ace
values: [4]i32 = [10, 20, 30, 40]
for value: i32 in values {
    // value is a per-iteration copy
}
```

A two-binder loop exposes the original zero-based index followed by the value:

```ace
for index: usize, value: i32 in values {
    values[index] = value + 1
}
```

Binders are always explicitly typed; the first binder of a two-binder loop
must be `usize`, and the value binder must exactly match the element type.
Both are per-iteration copies: assigning either changes only the local copy,
never the loop cursor or the backing sequence. There is no `_` discard binder;
use the one-binder form when the index is unwanted, or a range when only
indices are wanted:

```ace
for index: usize in 0 .. values.len {
}
```

## Mutating iteration

A sequence loop's binders are copies. To write elements, iterate the indices:

```ace
xs: [4]i32 = [1, 2, 3, 4]
for i: usize in 0 .. xs.len { xs[i] *= 10 }
```

## Strings

Strings iterate as their stored UTF-8 bytes — the encoding, never decoded code
points. The value binder is `u8` (or its transparent alias `char`), and the
two-binder form indexes bytes:

```ace
text: string = "héllo"
total: usize = 0
for c: u8 in text {
    total += usize(c)
}
for index: usize, byte: char in text {
    total += usize(byte) + index
}
```

The first binder must be `usize`, and the second binder must exactly match the
element type. Assigning to either binder does not affect loop progress or the
backing sequence.

Direct array, slice, or string iteration captures its pointer and length at
loop entry. Reassigning the variable does not change that running loop.

## Exits and labeled blocks

`continue` starts the next iteration and `break` exits the nearest loop. A
labeled block permits a structured exit across nested control flow:

```ace
work: {
    for value: i32 in values {
        if value < 0 {
            break work
        }
    }
}
```

`break label` may target only an enclosing label — a labeled block or a
labeled loop. On a labeled block, `continue label` restarts it from its
beginning, making indefinite repetition explicit without a separate loop
construct:

```ace
forever: {
    if finished {
        break forever
    }

    do_work()
    continue forever
}
```

Reaching the closing brace still exits normally. Plain `continue` advances
the nearest `for`; on a labeled loop, `continue label` starts that loop's
next iteration instead of restarting a block. Ace has no `goto` and no
non-local jump: there is no `setjmp`/`longjmp` equivalent, so a function
exits only by `return`, falling off its end (a function without a return
type), or `panic`.

## Deferred calls

`defer` schedules one call to run when the enclosing block exits. It may appear
in any block — the function body, an `if`, a loop body, a switch case, or a
labeled block — and its operand must be a call:

```ace
finish(): int {
    defer cleanup()
    work()
    return 0 // cleanup() runs after 0 is produced
}
```

Each block's defers form one list in source order. Whenever control leaves the
block — its end, a `return`, or a `break`, `continue`, labeled restart, or
`fallthrough` past it — they run in reverse order:

```ace
finish(): int {
    defer fmt.println("first")
    defer fmt.println("second")
    return 0 // prints "second", then "first"
}
```

An exit that leaves several blocks runs the innermost block's defers first. A
`defer` in a loop body runs once per iteration, so scratch storage can live in
the branch or iteration that needs it:

```ace
for line: string in lines {
    words: array.Array[string] = split(line)
    defer words.wipe() // released at the end of every iteration
    if words.len == 0 { continue } // also released here
    count(words.view())
}
```

Arguments are captured when the `defer` statement runs, not at the exit:

- Read-only arguments are evaluated once, right there; the exit-time call uses
  those values even if the variables changed in between:

```ace
report(value: int) {
    fmt.println(value)
}

work(): int {
    n: int = 1
    defer report(n) // captures 1
    n = 2
    return 0 // prints 1
}
```

- An `&x` argument snapshots the variable's address, and the receiver of
  `defer c.method()` is captured by address too. The exit-time call sees the variable's final value, and writes the
  callee makes land before the function's storage is released:

```ace
total: int

bump(n: *int) {
    *n += 1
}

work(): int {
    defer bump(&total) // captures the global itself, not its value
    total = 10
    return total // returns 10; the bump lands after, leaving total at 11
}
```

- A deferred call through a function value snapshots the callee itself at the
  `defer` statement, exactly like a read-only argument. Reassigning the
  variable afterwards cannot change which function the exit-time call
  dispatches to:

```ace
a(x: int): int {
    seen = 1
    return 0
}

b(x: int): int {
    seen = 2
    return 0
}

work(): int {
    f: (int): int = a
    defer f(0) // snapshots a
    f = b
    return 0 // the exit-time call runs a, not b
}
```

Only leaving the block runs its defers:

- `panic` and `sys.exit` terminate the process and run no defers. If either is
  itself deferred, reaching it terminates immediately and earlier pending
  defers do not run.
- A `break` or `continue` runs the defers of the blocks it leaves and no
  others; a block it stays inside keeps its defers pending.
- A `defer` written after an exit in its block never runs; it is accepted
  without an unreachable-code diagnostic.
- A deferred call's result is discarded — it cannot change the function's own
  return value.
- There is no check for deferring the same call twice.

`main` defers work like any other function's.

## Design rationale

Why the rules in `spec/control-flow.md` are shaped as they are.

- Conditions #2: a fixed position makes the catch-all the last thing a reader
  meets, so no case after it can look reachable, and a distinct word keeps
  `else` meaning only the alternative branch of `if`. The rule costs one check
  in sema and does not change emitted code.
- Conditions #2, case ranges: a range reuses the loop range's half-open
  meaning, so one spelling means one interval everywhere and the end bound of
  a switch range is the start of the next. Rejecting overlap makes case order
  irrelevant, which lets a range lower to a single unsigned compare. Floats and
  strings are left out: a float range needs NaN and signed-zero rules, and a
  string range needs an ordering the language does not define.
- Iterations #7, binder-less ranges: a count loop omits the binder as a
  sequence loop omits its index, so the language keeps one rule (omit what is
  unwanted) and `_` stays a value-drop that never appears in declaration
  position. Go 1.22's `for range n` does the same; Rust, Odin, and Zig spell a
  discard binder instead. The generated code is identical to a loop with an
  unread binder.
- Conditions #4: `defer` is expanded inline at each exit (Deferred calls #6),
  so a jump that leaves a frame without passing through one of those exits
  would skip cleanup and leave the leak reporter's accounting wrong. A
  `setjmp` target would also be a function that returns twice, which the
  register allocator and definite-initialization checking do not model and
  which would force `volatile` locals. Errors as values, `break label`, and
  `panic` already cover every use.
- Deferred calls #2, #7: cleanup ordering is what C reaches for `goto` to
  obtain. Anchoring each list at its block keeps every exit statically known —
  the block's end and each jump past it — so inline expansion stays free of
  runtime machinery, and an address capture of a block-local never outlives
  its storage. A function-level list fed from nested blocks (Go's model) needs a
  runtime record of which defers ran; Zig, Swift, Odin, and C's `cleanup`
  attribute scope defers to blocks as Ace does. Snapshotting every argument
  keeps a deferred call meaning what it would at the `defer` statement; an
  `&x` argument or a receiver still reaches the variable's final state.
- Sequences #9: every view operation is explicit and allocation-free; a write
  into literal storage faults through the platform's page protection rather
  than a type-level permission (Philosophy #10). Removing sequence equality operators avoids both hidden
  arbitrary-element work and descriptor identity masquerading as value equality.
  Byte content remains available through the explicit `mem.equal` operation,
  while the literal-`null` form states nullness directly without granting
  sequence-to-sequence comparison.
