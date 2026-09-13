# BearSSL backend pin

The experimental TLS client ABI proof uses the upstream BearSSL 0.6 release.
The dependency is not part of Ace's public API. Its local installation is
ignored by Git and is not fetched by an ordinary compiler or test-suite run.

- Source: `https://bearssl.org/bearssl-0.6.tar.gz`
- SHA-256: `6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14`
- Released: 2018-08-14
- License: MIT
- Protocol policy: TLS 1.2 only (`BR_TLS12` as both minimum and maximum)

BearSSL 0.6 has no TLS 1.3 support. It is a pinned proof backend, not a promise
that the eventual backend-neutral `std/tls` surface ends at TLS 1.2.

## Local installation

Ace needs only BearSSL's public headers and static library. The BearSSL
implementation source does not remain in the repository. Install the dependency
at these stable, Git-ignored paths:

- `external/bearssl/local/inc/`
- `external/bearssl/local/lib/libbearssl.a`

Download, verify, build, and copy the minimal artifacts:

```sh
curl -fL https://bearssl.org/bearssl-0.6.tar.gz \
  -o /tmp/bearssl-0.6.tar.gz
echo "6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14  /tmp/bearssl-0.6.tar.gz" \
  | shasum -a 256 -c -
mkdir -p /tmp/ace-bearssl
tar -xzf /tmp/bearssl-0.6.tar.gz -C /tmp/ace-bearssl
make -C /tmp/ace-bearssl/bearssl-0.6
mkdir -p external/bearssl/local/inc external/bearssl/local/lib
cp -R /tmp/ace-bearssl/bearssl-0.6/inc/. external/bearssl/local/inc/
cp /tmp/ace-bearssl/bearssl-0.6/build/libbearssl.a \
  external/bearssl/local/lib/libbearssl.a
```

The archive is native code. Build it on the target machine rather than copying
one from a different operating system or architecture. Preserve BearSSL's
`LICENSE.txt` with any redistributed binary package.

## Testing and usage

The proof adapter is `tests/driver/bearssl_adapter.c`. It intentionally keeps
all BearSSL declarations and class-table calls on the C side. With the local
installation present, `make test` compiles and runs the proof automatically.
To run it directly:

```sh
cc -Iexternal/bearssl/local/inc -c tests/driver/bearssl_adapter.c \
  -o /tmp/ace_bearssl_adapter.o
./ace -o /tmp/ace_bearssl_abi tests/driver/bearssl_abi.ace \
  /tmp/ace_bearssl_adapter.o external/bearssl/local/lib/libbearssl.a
/tmp/ace_bearssl_abi
```

This deterministic fixture supplies the adapter with a fixed 32-byte seed,
proves shorter seeds are rejected, and checks that equal seeds produce equal
DRBG output. The adapter injects that seed directly and marks BearSSL's guarded
system-RNG attempt complete, preventing `br_prng_seeder_system()` from running
during handshake initialization. The fixture also proves archive linkage, opaque
caller-owned context storage, supported alignment, TLS-1.2-only configuration,
record-buffer sizing, and the fixed-width/pointer-only Ace boundary. It does not
perform network I/O or claim that the still-open public TLS transport has been
carved. The ordinary test suite separately exercises Darwin `getentropy()`
through the private TLS OS layer.
