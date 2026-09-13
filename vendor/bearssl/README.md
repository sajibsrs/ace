# BearSSL backend pin

The experimental TLS client ABI proof uses the upstream BearSSL 0.6 release.
The dependency is not part of Ace's public API. Ace bundles the target-native
adapter object and archive used by the compiler at
`external/darwin-arm64/tls/bearssl/`; an ordinary compiler or test-suite run never
fetches it from the network.

- Source: `https://bearssl.org/bearssl-0.6.tar.gz`
- SHA-256: `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`
- Released: 2018-08-14
- License: MIT
- Protocol policy: TLS 1.2 only (`BR_TLS12` as both minimum and maximum)

BearSSL 0.6 has no TLS 1.3 support. It is a pinned proof backend, not a promise
that the eventual backend-neutral `std/tls` surface ends at TLS 1.2.

## Rebuilding the bundled artifacts

Ace needs BearSSL's public headers only while rebuilding its adapter and static
library. The BearSSL implementation source does not remain in the repository.
Build into these Git-ignored staging paths:

- `vendor/bearssl/local/inc/`
- `vendor/bearssl/local/lib/libbearssl.a`

Download, verify, build, and copy the minimal artifacts:

```sh
curl -fL https://bearssl.org/bearssl-0.6.tar.gz \
  -o /tmp/bearssl-0.6.tar.gz
echo "6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14  /tmp/bearssl-0.6.tar.gz" \
  | shasum -a 256 -c -
mkdir -p /tmp/ace-bearssl
tar -xzf /tmp/bearssl-0.6.tar.gz -C /tmp/ace-bearssl
make -C /tmp/ace-bearssl/bearssl-0.6
mkdir -p vendor/bearssl/local/inc vendor/bearssl/local/lib
cp -R /tmp/ace-bearssl/bearssl-0.6/inc/. vendor/bearssl/local/inc/
cp /tmp/ace-bearssl/bearssl-0.6/build/libbearssl.a \
  vendor/bearssl/local/lib/libbearssl.a
mkdir -p external/darwin-arm64/tls/bearssl
cc -std=c11 -O2 -Wall -Wextra -Werror -Ivendor/bearssl/local/inc \
  -c vendor/bearssl/adapter.c \
  -o external/darwin-arm64/tls/bearssl/adapter.o
cp vendor/bearssl/local/lib/libbearssl.a \
  external/darwin-arm64/tls/bearssl/libbearssl.a
```

The archive is native code. Build it on the target machine rather than copying
one from a different operating system or architecture. Preserve BearSSL's
`LICENSE.txt` with any redistributed binary package.

## Testing and usage

The deterministic proof adapter is `tests/driver/bearssl_adapter.c`. The first
blocking transport adapter is `vendor/bearssl/adapter.c`; both intentionally
keep all BearSSL declarations and class-table calls on the C side. With the local
installation present, `make test` compiles and runs the proof automatically.
To run it directly:

```sh
cc -Ivendor/bearssl/local/inc -c tests/driver/bearssl_adapter.c \
  -o /tmp/ace_bearssl_adapter.o
./ace -o /tmp/ace_bearssl_abi tests/driver/bearssl_abi.ace \
  /tmp/ace_bearssl_adapter.o vendor/bearssl/local/lib/libbearssl.a
/tmp/ace_bearssl_abi
```

This deterministic fixture supplies the adapter with a fixed 32-byte seed,
proves shorter seeds are rejected, and checks that equal seeds produce equal
DRBG output. The adapter injects that seed directly and marks BearSSL's guarded
system-RNG attempt complete, preventing `br_prng_seeder_system()` from running
during handshake initialization. The fixture also proves archive linkage, opaque
caller-owned context storage, supported alignment, TLS-1.2-only configuration,
record-buffer sizing, and the fixed-width/pointer-only Ace boundary. It does not
perform network I/O; the separate blocking adapter and `tls` module drive the
first public transport slice. The ordinary test suite separately exercises
Darwin `getentropy()` through the OS-neutral `sys.random` facade.

`vendor/bearssl/isrg-root-x1.der` is an explicit demonstration trust anchor for
override tests. Ordinary HTTPS uses the macOS system trust store through the
private adapter; no certificate-store type crosses into Ace.
