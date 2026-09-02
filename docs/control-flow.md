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

`switch` compares one scalar value against cases and uses `else` for the default
case. Cases do not fall through implicitly. A non-final case may end with the
contextual statement `fallthrough` to continue directly into the immediately
following case without testing that case's pattern:

```ace
switch value {
    1 {
        count += 1
        fallthrough
    }
    2 {
        count += 2
    }
    else {}
}
```

`fallthrough` takes no target or condition and must be the case's final
statement. It is rejected in the final case. The spelling remains an ordinary
identifier outside this exact statement position.

`fallthrough` is rejected in tagged-union switches. A union case is an active
variant pattern, so entering the next case without a match would contradict its
pattern; retesting it would never match a different active variant.

For a tagged union, cases name variants and payload variants bind an explicitly
typed local copy. The switch must cover every variant or include `else`:

```ace
switch event {
    Event.quit {
        return
    }
    Event.key(code: u32) {
        code += 1
    }
    else {}
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
steps must also be positive; the current compiler has no checked-build mode and
therefore assumes that precondition without inserting validation.

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
backing sequence. There is no `_` discard binder; use the one-binder form when
the index is unwanted. To iterate only indices, use a range:

```ace
for index: usize in 0 .. values.len {
}
```

Range bounds are ordinary captured values, not aliases to expressions used to
produce them. If a slice variable is resliced or reassigned inside an index
range loop, the captured endpoint does not change, while later indexing uses
the slice's current descriptor. The current compiler does not insert runtime
bounds checks, so such code must keep the current slice large enough.

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

`break label` may target only an enclosing labeled block. `continue label`
restarts one from its beginning, making indefinite repetition explicit without
a separate loop construct:

```ace
forever: {
    if finished {
        break forever
    }

    do_work()
    continue forever
}
```

Reaching the closing brace still exits normally. Plain `continue` advances the
nearest `for-in` loop; only its labeled form restarts a block. Ace has no `goto`.
