# std.crypto

[Standard library](index.md)

Basic cryptographic hashes for integrity checks.

```ace
import crypto

digest: [32]u8 = crypto.sha256("abc")
```

`sha256(data: []u8): [32]u8` returns the FIPS 180-4 digest of `data`. It
allocates nothing.

## Design rationale

`std` covers what checking a download needs. Encryption and other work that
must resist attackers belongs to a proven library, not a new implementation.
