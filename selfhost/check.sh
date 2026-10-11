#!/bin/sh
# Diffs the self-hosted compiler's tokens against `ace --tokens`, its tree
# against `ace --ast` (stdout, exit status, and every error line), its module
# loader against the C one (every import error line; on success, the count of
# files loaded), its merged, lowered tree against `ace --resolved`, and its
# entered scopes and completed types against `ace --symbols` over every repository .ace
# file, then its package graph against `ace --pkg-graph` and its cross-package
# imports against `ace --pkg-check` over every repository ace.pkg, then times
# the lexers on the largest files. Run from anywhere.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
# std/ is found beside the running executable, so the module checks run a copy
# of the self-hosted binary in the repository root, as `ace` itself is.
self="$root/ace-self.$$"
trap 'rm -rf "$tmp" "$self"' EXIT

"$root/ace" -o "$tmp/ace-self" "$here/main.ace" || exit 1
cp "$tmp/ace-self" "$self"

cd "$root"
files=$(find std pkg demo tests bench -name '*.ace' | sort)
# The loader's own diagnostics: C reports these before sema, so a C failure
# that shows one is one the self-hosted loader must have reported too.
import_errors='import cycle|not found \(tried|is ambiguous|cannot load module|member .[^ ]*. is not public|no public member|duplicate import binding|collides with (another import|a declaration)|must be relative|private to directory module|has no mod.ace|too long|too many segments|empty segment|escapes its package directory|reserved pkg/ directory'

total=0
bad=0
abad=0
mbad=0
msema=0
rbad=0
sbad=0
for f in $files; do
    total=$((total + 1))
    ./ace --tokens "$f" >"$tmp/want" 2>/dev/null
    "$tmp/ace-self" "$f" >"$tmp/got" 2>/dev/null
    if ! cmp -s "$tmp/want" "$tmp/got"; then
        bad=$((bad + 1))
        echo "DIFF $f"
        diff "$tmp/want" "$tmp/got" | head -4
    fi
    ./ace --ast "$f" >"$tmp/want" 2>"$tmp/werr"
    wrc=$?
    "$tmp/ace-self" --ast "$f" >"$tmp/got" 2>"$tmp/gerr"
    grc=$?
    if [ "$wrc" -ne "$grc" ] || ! cmp -s "$tmp/want" "$tmp/got" ||
        ! cmp -s "$tmp/werr" "$tmp/gerr"; then
        abad=$((abad + 1))
        echo "AST DIFF $f"
        diff "$tmp/want" "$tmp/got" | head -4
        diff "$tmp/werr" "$tmp/gerr" | head -4
    fi
    # Modules: C stops after the import stage on an error, so when ours fails
    # the two stderrs must match; when ours succeeds, C must too, or fail
    # only in a later stage (sema), and then load the same number of files with
    # the same source byte and token totals.
    ./ace --report "$f" >"$tmp/want" 2>"$tmp/werr"
    wrc=$?
    "$self" --modules "$f" >"$tmp/got" 2>"$tmp/gerr"
    grc=$?
    if [ "$grc" -ne 0 ]; then
        if [ "$wrc" -eq 0 ] || ! cmp -s "$tmp/werr" "$tmp/gerr"; then
            mbad=$((mbad + 1))
            echo "MODULES DIFF $f"
            diff "$tmp/werr" "$tmp/gerr" | head -4
        fi
    elif [ "$wrc" -eq 0 ]; then
        want=$(sed -n 's/^source files: *//p' "$tmp/werr" | tr -d ',')
        got=$(grep -c '^module ' "$tmp/got")
        differ=0
        if [ "$want" != "$got" ]; then
            differ=1
            echo "MODULES COUNT $f: C loaded $want, ours $got"
        fi
        for stat in 'source bytes' tokens; do
            want=$(sed -n "s/^$stat: *//p" "$tmp/werr" | tr -d ',')
            got=$(sed -n "s/^$stat: *//p" "$tmp/got")
            if [ "$want" != "$got" ]; then
                differ=1
                echo "MODULES $stat $f: C $want, ours $got"
            fi
        done
        mbad=$((mbad + differ))
    elif grep -Eq "$import_errors" "$tmp/werr"; then
        mbad=$((mbad + 1))
        echo "MODULES MISSED $f"
        head -2 "$tmp/werr"
    else
        msema=$((msema + 1))
    fi
    # Resolved: the merged, lowered tree and every error line must match.
    ./ace --resolved "$f" >"$tmp/want" 2>"$tmp/werr"
    wrc=$?
    "$self" --resolved "$f" >"$tmp/got" 2>"$tmp/gerr"
    grc=$?
    if [ "$wrc" -ne "$grc" ] || ! cmp -s "$tmp/want" "$tmp/got" ||
        ! cmp -s "$tmp/werr" "$tmp/gerr"; then
        rbad=$((rbad + 1))
        echo "RESOLVED DIFF $f"
        diff "$tmp/want" "$tmp/got" | head -4
        diff "$tmp/werr" "$tmp/gerr" | head -4
    fi
    # Symbols: the entered scopes and every error line must match.
    ./ace --symbols "$f" >"$tmp/want" 2>"$tmp/werr"
    wrc=$?
    "$self" --symbols "$f" >"$tmp/got" 2>"$tmp/gerr"
    grc=$?
    if [ "$wrc" -ne "$grc" ] || ! cmp -s "$tmp/want" "$tmp/got" ||
        ! cmp -s "$tmp/werr" "$tmp/gerr"; then
        sbad=$((sbad + 1))
        echo "SYMBOLS DIFF $f"
        diff "$tmp/want" "$tmp/got" | head -4
        diff "$tmp/werr" "$tmp/gerr" | head -4
    fi
done
# Packages: every ace.pkg as a root, for its manifest's target. The graph must
# match exactly; a check that fails must fail as C's does, and one that passes
# must pass in C too, or fail only in sema.
packages=$(find std pkg demo tests bench -name ace.pkg | sort)
ptotal=0
gbad=0
pbad=0
psema=0
for m in $packages; do
    ptotal=$((ptotal + 1))
    d=$(dirname "$m")
    t=$(sed -n 's/^\/\/ *target:[[:space:]]*//p' "$m")
    ./ace --pkg-graph="$d" ${t:+--target=$t} >"$tmp/want" 2>"$tmp/werr"
    wrc=$?
    "$self" --pkg-graph="$d" ${t:+--target=$t} >"$tmp/got" 2>"$tmp/gerr"
    grc=$?
    if [ "$wrc" -ne "$grc" ] || ! cmp -s "$tmp/want" "$tmp/got" ||
        ! cmp -s "$tmp/werr" "$tmp/gerr"; then
        gbad=$((gbad + 1))
        echo "GRAPH DIFF $d"
        diff "$tmp/want" "$tmp/got" | head -4
        diff "$tmp/werr" "$tmp/gerr" | head -4
    fi
    ./ace --pkg-check="$d" ${t:+--target=$t} >/dev/null 2>"$tmp/werr"
    wrc=$?
    "$self" --pkg-check="$d" ${t:+--target=$t} >/dev/null 2>"$tmp/gerr"
    grc=$?
    if [ "$grc" -ne 0 ]; then
        if [ "$wrc" -ne "$grc" ] || ! cmp -s "$tmp/werr" "$tmp/gerr"; then
            pbad=$((pbad + 1))
            echo "PACKAGE DIFF $d"
            diff "$tmp/werr" "$tmp/gerr" | head -4
        fi
    elif [ "$wrc" -ne 0 ]; then
        if grep -Eq "$import_errors" "$tmp/werr"; then
            pbad=$((pbad + 1))
            echo "PACKAGE MISSED $d"
            head -2 "$tmp/werr"
        else
            psema=$((psema + 1))
        fi
    fi
done

echo "tokens: $((total - bad))/$total files identical"
echo "ast: $((total - abad))/$total files identical"
echo "modules: $((total - mbad))/$total files agree ($msema fail only in sema)"
echo "resolved: $((total - rbad))/$total files agree"
echo "symbols: $((total - sbad))/$total files agree"
echo "graph: $((ptotal - gbad))/$ptotal packages identical"
echo "packages: $((ptotal - pbad))/$ptotal packages agree ($psema fail only in sema)"

echo "timing, ${runs:=200} runs per file (ms per run): file  ace --tokens  ace-self"
for f in $(ls -S $files | head -5); do
    a=$(python3 -c 'import subprocess,sys,time
t=time.perf_counter()
for _ in range(int(sys.argv[1])): subprocess.run(sys.argv[2:],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
print("%.2f"%((time.perf_counter()-t)*1000/int(sys.argv[1])))' "$runs" ./ace --tokens "$f")
    b=$(python3 -c 'import subprocess,sys,time
t=time.perf_counter()
for _ in range(int(sys.argv[1])): subprocess.run(sys.argv[2:],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
print("%.2f"%((time.perf_counter()-t)*1000/int(sys.argv[1])))' "$runs" "$tmp/ace-self" "$f")
    echo "$f $(wc -c <"$f" | tr -d ' ')B  $a  $b"
done
[ "$bad" -eq 0 ] && [ "$abad" -eq 0 ] && [ "$mbad" -eq 0 ] && [ "$rbad" -eq 0 ] && [ "$sbad" -eq 0 ] && [ "$gbad" -eq 0 ] && [ "$pbad" -eq 0 ]
