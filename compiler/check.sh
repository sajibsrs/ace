#!/bin/sh
# Diffs the self-hosted compiler's tokens against `ace --tokens` and its tree
# against `ace --ast` (stdout, exit status, and the first error line) over
# every repository .ace file, then times the lexers on the largest files. Run
# from anywhere.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$root/ace" -o "$tmp/ace-self" "$here/main.ace" || exit 1

cd "$root"
files=$(find std pkg demo tests bench -name '*.ace' | sort)
total=0
bad=0
abad=0
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
        [ "$(head -1 "$tmp/werr")" != "$(head -1 "$tmp/gerr")" ]; then
        abad=$((abad + 1))
        echo "AST DIFF $f"
        diff "$tmp/want" "$tmp/got" | head -4
        diff "$tmp/werr" "$tmp/gerr" | head -4
    fi
done
echo "tokens: $((total - bad))/$total files identical"
echo "ast: $((total - abad))/$total files identical"

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
[ "$bad" -eq 0 ] && [ "$abad" -eq 0 ]
