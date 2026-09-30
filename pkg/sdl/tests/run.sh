#!/bin/sh
# Behavioral tests owned by the pkg/sdl package, not the compiler core.
# Decoupled from tests/run.sh/`make test` on purpose (see changelog/):
# these exercise the SDL3 binding, not compiler correctness, and shouldn't
# gate core compiler iteration. Run via `make test-packages` or directly as
# `pkg/sdl/tests/run.sh`.
#
# The cases link the host's SDL3 through pkg/sdl's [[link]] path hint; where
# that library is missing, the suite is skipped, not failed.
#
# One directory per case, each its own `app` package depending on `sdl`
# (`sdl: 0.1.0`), found as the repository's pkg/sdl by the nearest-pkg/
# walk-up.
# Markers live as `//`-comments in ace.pkg: `// expect-exit: N` builds and
# runs the case, expecting exit code N; `// expect-check-ok` expects
# `--pkg-check` to succeed with no run.
set -u
fail=0
cd "$(dirname "$0")"
ACE_BIN=${ACE_BIN:-../../../ace}
ACE_DIR=$(dirname "$ACE_BIN")
ACE=$(cd "$ACE_DIR" && pwd)/$(basename "$ACE_BIN")

# pkg/sdl's manifest hints where SDL3 lives ([[link]] path); a host without
# the library there skips the suite rather than failing it.
lib_dir=$(sed -n 's/^path: *//p' ../ace.pkg)
if [ ! -e "$lib_dir/libSDL3.dylib" ]; then
    echo "pkg/sdl: skipped (no libSDL3 in '$lib_dir')"
    exit 0
fi

for case_dir in */; do
    dir=${case_dir%/}
    f="$dir/ace.pkg"
    [ -f "$f" ] || continue
    name=$(sed -n 's/^name: *//p' "$f")
    exp=$(sed -n 's/^\/\/ *expect-exit:[[:space:]]*//p' "$f")
    check_ok=$(sed -n 's/^\/\/ *expect-check-ok[[:space:]]*.*/yes/p' "$f")
    rm -rf "$dir/build"
    if [ -n "$exp" ]; then
        if ! (cd "$dir" && "$ACE" pkg build --debug) >/dev/null 2>/tmp/ace_pkg_sdl_test_errs; then
            echo "FAIL (pkg build should succeed): $dir"
            cat /tmp/ace_pkg_sdl_test_errs
            fail=1
        else
            "$dir/build/$name" >/tmp/ace_pkg_sdl_test_out 2>&1
            got=$?
            if [ "$got" -ne "$exp" ]; then
                echo "FAIL: $dir exited $got, want $exp"
                cat /tmp/ace_pkg_sdl_test_out
                fail=1
            fi
        fi
    elif [ -n "$check_ok" ]; then
        if ! "$ACE" --pkg-check="$dir" >/dev/null 2>/tmp/ace_pkg_sdl_test_errs; then
            echo "FAIL (pkg check should succeed): $dir"
            cat /tmp/ace_pkg_sdl_test_errs
            fail=1
        fi
    else
        echo "FAIL: $f has no 'expect-exit'/'expect-check-ok' comment"
        fail=1
    fi
    rm -rf "$dir/build"
done

if [ $fail -eq 0 ]; then
    echo "pkg/sdl: all tests passed"
fi
exit $fail
