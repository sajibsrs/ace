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

`in mut` before the iterable opts the loop into aliasing. The value binder
then names the element's own storage: reads and writes hit the backing
sequence directly, with no per-iteration copy and no write-back. `&value`
yields a pointer into the backing storage, and `mut &value` yields a
`mut *T` whose writes land there:

```ace
xs: [4]i32 = [1, 2, 3, 4]
for item: i32 in mut xs {
    item *= 10 // lands in xs
}
```

The two-binder form works the same way, and the index binder stays an
ordinary `usize` copy:

```ace
for index: usize, item: i32 in mut xs {
    item += i32(index)
}
```

The iterable must establish a writable view. A writable lvalue naming array or
slice storage — a local, field path, mutable global, or `mut` parameter —
establishes directly; a `mut []T` or `mut string` view value establishes
through its type, with no storage check; and `in mut xs[lo..hi]` establishes a
writable window of the storage. By-value parameters, `const` globals, array
literals, call results, plain slicing results, and ranges are rejected. The
alias reads the loop's entry-captured pointer and length, so binder writes
never disturb the cursor: reslicing the iterable variable mid-loop changes
neither the running loop nor where the alias points.

Strings participate with no special case: `for c: u8 in mut text` requires
`text` to be a `mut string` (or a writable slice of one), and the alias writes
the string's bytes directly; a plain `string` is rejected by the same
writable-storage rule as any read-only view.

A plain (non-`mut`) loop keeps the per-iteration copy; `in mut` is a strict
superset, not a replacement.

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
next iteration instead of restarting a block. Ace has no `goto`.

## Deferred calls

`defer` schedules one call to run when the surrounding function exits. The
statement is legal only as a direct statement of the function's top-level body
block — not inside an `if`, `for`, a switch case, or a labeled block — and its
operand must be a call:

```ace
finish(): int {
    defer cleanup()
    work()
    return 0 // cleanup() runs after 0 is produced
}
```

A function's defers form one list in source order. At every exit — each
`return` and falling off the end of the body — they run in reverse order:

```ace
finish(): int {
    defer fmt.println("first")
    defer fmt.println("second")
    return 0 // prints "second", then "first"
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

- An argument marked `mut` at the call, and the receiver in the
  receiver-marked form `defer mut c.method()`, capture the variable's address
  instead. The exit-time call sees the variable's final value, and writes the
  callee makes land before the function's storage is released:

```ace
total: int

bump(mut n: int) {
    n += 1
}

work(): int {
    defer bump(mut total) // captures the global itself, not its value
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

  The callee position is always read-only-captured. `mut` marks a receiver,
  and through a function value there is no receiver, so `defer mut f()` marks
  nothing.

Defers run only at function exits, and nothing else runs them:

- `panic` and `sys.exit` terminate the process and run no defers. If either is
  itself deferred, reaching it terminates immediately and earlier pending
  defers do not run.
- `break` and `continue`, plain or labeled, leave the function running, so the
  defers wait for the real exit that ends the function.
- A `defer` written after the final `return` never runs; it is accepted
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
- Deferred calls #7: cleanup ordering is what C reaches for `goto` to obtain.
  Anchoring the list at the function keeps the exit set total — every `return`
  plus fall-off — with no block-scoped bookkeeping, which is what makes inline
  expansion possible at zero runtime cost in an in-order compiler. The two-tier
  capture keeps the ordinary `mut` call contract truthful at a distance:
  read-only arguments behave like call-by-value snapshots, while `mut`-marked
  arguments keep their by-reference meaning up to the function's final moment.
- Sequences #9: every view operation is explicit and allocation-free; permission
  is established where `mut` is written, carried in types thereafter, and never
  escalated — writable bytes exist only from proven-writable provenance, so the
  literal-storage fault class is rejected at declarations rather than handled at
  runtime. Removing sequence equality operators avoids both hidden
  arbitrary-element work and descriptor identity masquerading as value equality.
  Byte content remains available through the explicit `mem.equal` operation,
  while the literal-`null` form states nullness directly without granting
  sequence-to-sequence comparison.
