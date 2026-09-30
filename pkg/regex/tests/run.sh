#!/bin/sh
# Behavioral tests owned by the pkg/regex package, not the compiler core.
# Decoupled from tests/run.sh/`make test` on purpose (see changelog/):
# these exercise regex's own parsing and matching logic, not compiler
# correctness, and shouldn't gate core compiler iteration. Run via
# `make test-packages` or directly as `pkg/regex/tests/run.sh`.
#
# One directory per case, each its own `app` package depending on `regex`
# (`regex: 0.1.0`), found as the repository's pkg/regex by the nearest-pkg/
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

for case_dir in */; do
    dir=${case_dir%/}
    f="$dir/ace.pkg"
    [ -f "$f" ] || continue
    name=$(sed -n 's/^name: *//p' "$f")
    exp=$(sed -n 's/^\/\/ *expect-exit:[[:space:]]*//p' "$f")
    check_ok=$(sed -n 's/^\/\/ *expect-check-ok[[:space:]]*.*/yes/p' "$f")
    rm -rf "$dir/build"
    if [ -n "$exp" ]; then
        if ! (cd "$dir" && "$ACE" pkg build --debug) >/dev/null 2>/tmp/ace_pkg_regex_test_errs; then
            echo "FAIL (pkg build should succeed): $dir"
            cat /tmp/ace_pkg_regex_test_errs
            fail=1
        else
            "$dir/build/$name" >/tmp/ace_pkg_regex_test_out 2>&1
            got=$?
            if [ "$got" -ne "$exp" ]; then
                echo "FAIL: $dir exited $got, want $exp"
                cat /tmp/ace_pkg_regex_test_out
                fail=1
            fi
        fi
    elif [ -n "$check_ok" ]; then
        if ! "$ACE" --pkg-check="$dir" >/dev/null 2>/tmp/ace_pkg_regex_test_errs; then
            echo "FAIL (pkg check should succeed): $dir"
            cat /tmp/ace_pkg_regex_test_errs
            fail=1
        fi
    else
        echo "FAIL: $f has no 'expect-exit'/'expect-check-ok' comment"
        fail=1
    fi
    rm -rf "$dir/build"
done

if [ $fail -eq 0 ]; then
    echo "pkg/regex: all tests passed"
fi
exit $fail
