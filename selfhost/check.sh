#!/bin/sh
# Builds the self-hosted compiler with itself, checks that a debug build of it
# compiles itself without a runtime trap, then diffs its tokens against `ace --tokens`, its tree
# against `ace --ast` (stdout, exit status, and every error line), its module
# loader against the C one (every import error line; on success, the count of
# files loaded), its merged, lowered tree against `ace --resolved`, and its
# entered scopes and completed types against `ace --symbols`, and its function
# body checks against `ace --check` and its full check against `ace` over
# every repository .ace file, and its functions' IR against `ace --debug --ir` and `ace --release --ir`,
# and its object files against `ace --debug -c` and `ace --release -c`,
# then its package graph against `ace --pkg-graph`
# and its cross-package imports against `ace --pkg-check` over every repository ace.pkg, then times
# the lexers on the largest files. Run from anywhere.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
# std/ is found beside the running executable, so the module checks run a copy
# of the self-hosted binary in the repository root, as `ace` itself is.
self="$root/ace-self.$$"
stage1="$root/ace-stage1.$$"
debug="$root/ace-debug.$$"
trap 'rm -rf "$tmp" "$self" "$stage1" "$stage1.o" "$debug" "$debug.o"' EXIT

# Stage 1 is built by `ace`; stage 2, which every check below runs, is built
# by stage 1. Their objects must match byte for byte (ld records each object's
# directory, so the linked binaries themselves differ).
"$root/ace" -o "$stage1" "$here/main.ace" || exit 1
"$stage1" -o "$tmp/ace-self" "$here/main.ace" || exit 1
if ! cmp -s "$stage1.o" "$tmp/ace-self.o"; then
    echo "self-build: stage 2 object differs from stage 1"
    exit 1
fi
echo "self-build: stage 2 object matches stage 1"
cp "$tmp/ace-self" "$self"

# A debug build traps on integer overflow and bounds violations the release
# build passes silently; compiling itself in both modes exercises every pass.
"$root/ace" --debug -o "$debug" "$here/main.ace" || exit 1
for mode in --debug --release; do
    if ! (cd "$tmp" && "$debug" $mode -c "$here/main.ace" >/dev/null); then
        echo "debug-build: $mode -c of main.ace failed"
        exit 1
    fi
done
echo "debug-build: compiles itself in debug and release without a trap"

cd "$root"
files=$(find std pkg demo tests bench selfhost -name '*.ace' | sort)
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
cbad=0
cport=0
ebad=0
eport=0
itotal=0
isame=0
iport=0
ototal=0
obad=0
ibad=0
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
    # Check: every error line must match, unless selfhost stops on a construct
    # it does not port yet; those files are counted apart.
    ./ace --check "$f" >"$tmp/want" 2>"$tmp/werr"
    wrc=$?
    "$self" --check "$f" >"$tmp/got" 2>"$tmp/gerr"
    grc=$?
    if grep -q 'selfhost does not yet support' "$tmp/gerr"; then
        cport=$((cport + 1))
    elif [ "$wrc" -ne "$grc" ] || ! cmp -s "$tmp/werr" "$tmp/gerr"; then
        cbad=$((cbad + 1))
        echo "CHECK DIFF $f"
        diff "$tmp/werr" "$tmp/gerr" | head -4
    fi
    # Sema: the full check, end-of-file checks included, counted the same way.
    ./ace "$f" >"$tmp/want" 2>"$tmp/werr"
    wrc=$?
    "$self" --sema "$f" >"$tmp/got" 2>"$tmp/gerr"
    grc=$?
    if grep -q 'selfhost does not yet support' "$tmp/gerr"; then
        eport=$((eport + 1))
    elif [ "$wrc" -ne "$grc" ] || ! cmp -s "$tmp/werr" "$tmp/gerr"; then
        ebad=$((ebad + 1))
        echo "SEMA DIFF $f"
        diff "$tmp/werr" "$tmp/gerr" | head -4
    fi
    # IR: each function C lowers in a checked file must print identically,
    # unless selfhost stops on a construct it does not port yet (counted apart).
    for mode in --debug --release; do
    if ./ace $mode --ir "$f" >"$tmp/want" 2>/dev/null; then
        "$self" $mode --ir "$f" >"$tmp/got" 2>"$tmp/gerr"
        grc=$?
        if ! grep -q 'selfhost does not yet support' "$tmp/gerr"; then
            # one function per line, then the pairs side by side (IR has no tabs)
            awk 'BEGIN { RS = "" } { gsub(/\n/, "\001"); print }' "$tmp/want" >"$tmp/wf"
            awk 'BEGIN { RS = "" } { gsub(/\n/, "\001"); print }' "$tmp/got" >"$tmp/gf"
            if [ "$grc" -ne 0 ] || [ "$(wc -l <"$tmp/wf")" -ne "$(wc -l <"$tmp/gf")" ]; then
                n=$(wc -l <"$tmp/wf")
                itotal=$((itotal + n))
                ibad=$((ibad + n))
                echo "IR FAIL $mode $f"
                head -2 "$tmp/gerr"
            else
                counts=$(paste "$tmp/wf" "$tmp/gf" | awk -F '\t' '
                    $2 ~ /^; [^\001]*: not lowered \(selfhost does not yet support [^\001]*\)\001*$/ { p++; next }
                    $1 == $2 { s++; next }
                    { b++; if (!shown++) { gsub(/\001/, "\n", $1); gsub(/\001/, "\n", $2); print $1 > "/dev/stderr"; print "---" > "/dev/stderr"; print $2 > "/dev/stderr" } }
                    END { printf "%d %d %d\n", s, p, b }' 2>"$tmp/idiff")
                set -- $counts
                itotal=$((itotal + $1 + $2 + $3))
                isame=$((isame + $1))
                iport=$((iport + $2))
                ibad=$((ibad + $3))
                if [ "$3" -ne 0 ]; then
                    echo "IR DIFF $mode $f"
                    head -20 "$tmp/idiff"
                fi
            fi
        fi
    fi
    done
    # Objects: each one C writes must match byte for byte. -c writes <base>.o
    # beside the source, so a file that already has one is left alone.
    o=${f%.ace}.o
    [ -e "$o" ] || for mode in --debug --release; do
    if ./ace $mode -c "$f" >/dev/null 2>&1; then
        mv "$o" "$tmp/want.o"
        ototal=$((ototal + 1))
        "$self" $mode -c "$f" >/dev/null 2>"$tmp/gerr"
        if [ ! -e "$o" ] || ! cmp -s "$tmp/want.o" "$o"; then
            obad=$((obad + 1))
            echo "OBJECT DIFF $mode $f"
            head -2 "$tmp/gerr"
        fi
    fi
    rm -f "$o"
    done
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
echo "check: $((total - cbad))/$total files agree ($cport not yet ported)"
echo "sema: $((total - ebad))/$total files agree ($eport not yet ported)"
echo "ir (debug and release): $isame/$itotal functions identical ($iport not yet ported)"
echo "objects (debug and release): $((ototal - obad))/$ototal identical"
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
[ "$bad" -eq 0 ] && [ "$abad" -eq 0 ] && [ "$mbad" -eq 0 ] && [ "$rbad" -eq 0 ] && [ "$sbad" -eq 0 ] && [ "$cbad" -eq 0 ] && [ "$ebad" -eq 0 ] && [ "$gbad" -eq 0 ] && [ "$pbad" -eq 0 ]
