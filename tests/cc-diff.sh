#!/bin/sh
# Builds ace with two C compilers and checks that both builds print the same
# --ir and write the same object for every repository .ace file that checks
# clean, in debug and release. C leaves the order of a call's argument
# evaluations unspecified, so two emitting calls passed to one function
# number or order IR differently under gcc and clang. Run from the root:
#   tests/cc-diff.sh [cc-a] [cc-b]
set -eu

if [ "$#" -gt 2 ]; then
    echo "usage: $0 [cc-a] [cc-b]" >&2
    exit 2
fi
cc_a=${1:-gcc}
cc_b=${2:-clang}
dir=$(mktemp -d "${TMPDIR:-/tmp}/ace-cc-diff.XXXXXX")
trap 'rm -rf "$dir"' EXIT HUP INT TERM

# std/ is found beside the running executable.
ln -s "$PWD/std" "$dir/std"
for side in a b; do
    eval "c=\$cc_$side"
    "$c" -std=c11 -D_DEFAULT_SOURCE -O2 -w -o "$dir/ace-$side" src/*.c
done

mkdir "$dir/out"
git ls-files '*.ace' | DIR=$dir xargs -P "${ACE_JOBS:-$(getconf _NPROCESSORS_ONLN)}" -n 1 sh -c '
    f=$1
    o=$DIR/out/$(printf %s "$f" | tr / _)
    "$DIR/ace-a" "$f" >/dev/null 2>&1 || exit 0
    for m in debug release; do
        "$DIR/ace-a" --$m --ir "$f" >"$o.a.ir" 2>&1 || :
        "$DIR/ace-b" --$m --ir "$f" >"$o.b.ir" 2>&1 || :
        cmp -s "$o.a.ir" "$o.b.ir" || echo "FAIL: --$m --ir $f"
        "$DIR/ace-a" --$m -c -o "$o.a" "$f" >/dev/null 2>&1 || :
        "$DIR/ace-b" --$m -c -o "$o.b" "$f" >/dev/null 2>&1 || :
        if [ -f "$o.a.o" ] || [ -f "$o.b.o" ]; then
            cmp -s "$o.a.o" "$o.b.o" || echo "FAIL: --$m -c $f"
        fi
        rm -f "$o.a.o" "$o.b.o"
    done
' _ >"$dir/fails"

sort "$dir/fails"
n=$(wc -l <"$dir/fails" | tr -d " ")
echo "$cc_a and $cc_b builds: $n differences"
[ "$n" -eq 0 ]
