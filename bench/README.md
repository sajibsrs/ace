# Memory benchmark

Run the v1 raw-memory benchmark on Apple arm64:

```sh
./bench/run-memory.sh
```

The runner builds `bench/memory.ace` with the compiler report enabled, then
runs it under macOS `/usr/bin/time -l`. The Ace program checks allocation,
reallocation payload preservation, and release nulling. It reports operation
counts, nanoseconds per operation, requested live bytes, allocator-usable live
bytes from `malloc_size`, allocator overhead, a checksum, and failures.

The external timing report adds process-level statistics such as peak resident
set size. Timing and RSS vary by machine and system load, so this is a manual
v1 acceptance benchmark rather than a pass/fail performance test in
`tests/run.sh`. The only pass/fail condition is memory-operation correctness;
the program exits nonzero when it observes a failure.
