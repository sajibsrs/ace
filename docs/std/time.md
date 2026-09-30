# std.time

[Standard library](index.md)

`time` reads system-wide monotonic clocks. It never allocates and has no
`Error` return.

`elapsed(): u64` and `awake(): u64` read separate system-wide
monotonic clocks in nanoseconds. Their origins reset at every boot:

- `elapsed()` keeps advancing during system sleep, so differences between
  its readings measure real elapsed time across sleep and wake cycles.
- `awake()` pauses during system sleep and resumes when the system wakes.
  It advances whenever the system is awake, regardless of whether this or any
  other process is running, scheduled, or blocked. It is not process CPU time.

Both clocks are immune to calendar-clock changes. Each returned value is a
clock reading, not a duration by itself; subtract two readings from the same
operation to obtain elapsed nanoseconds. Cross-clock differences are
meaningless. Neither call allocates or can fail.

```ace
import time

main(): int {
    start: u64 = time.awake()
    sum: int = 0
    for i: int in 0 .. 1_000_000 {
        sum += i
    }
    elapsed_ns: u64 = time.awake() - start
    if elapsed_ns == 0 {
        return 1
    }
    return 0
}
```

On Darwin, `elapsed()` reads `CLOCK_MONOTONIC_RAW` and `awake()`
`CLOCK_UPTIME_RAW`. On Linux, `elapsed()` reads `CLOCK_BOOTTIME`, whose rate
NTP may slew, and `awake()` reads `CLOCK_MONOTONIC_RAW`.

Calendar time, process CPU time, duration conversion, and process sleep are not
implemented yet.

## Design rationale

Why the rules in `spec/std/sys.md` are shaped as they are.

- Stdlib: `sys` and `time` #11: the clocks are distinct operations with distinct
  time domains, so separate names state whether the reading measures all elapsed
  time or awake time without a boolean argument.
