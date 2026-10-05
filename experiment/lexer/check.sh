#!/bin/sh
# Diffs the Ace lexer's output against `ace --tokens` over every repository
# .ace file, then times both on the largest files. Run from anywhere.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$root/ace" -o "$tmp/lexer" "$here/main.ace" || exit 1

cd "$root"
files=$(find std pkg demo tests bench -name '*.ace' | sort)
total=0
bad=0
for f in $files; do
    total=$((total + 1))
    ./ace --tokens "$f" >"$tmp/want" 2>/dev/null
    "$tmp/lexer" "$f" >"$tmp/got" 2>/dev/null
    if ! cmp -s "$tmp/want" "$tmp/got"; then
        bad=$((bad + 1))
        echo "DIFF $f"
        diff "$tmp/want" "$tmp/got" | head -4
    fi
done
echo "$((total - bad))/$total files identical"

echo "timing, ${runs:=200} runs per file (ms per run): file  ace --tokens  ace-lexer"
for f in $(ls -S $files | head -5); do
    a=$(python3 -c 'import subprocess,sys,time
t=time.perf_counter()
for _ in range(int(sys.argv[1])): subprocess.run(sys.argv[2:],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
print("%.2f"%((time.perf_counter()-t)*1000/int(sys.argv[1])))' "$runs" ./ace --tokens "$f")
    b=$(python3 -c 'import subprocess,sys,time
t=time.perf_counter()
for _ in range(int(sys.argv[1])): subprocess.run(sys.argv[2:],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
print("%.2f"%((time.perf_counter()-t)*1000/int(sys.argv[1])))' "$runs" "$tmp/lexer" "$f")
    echo "$f $(wc -c <"$f" | tr -d ' ')B  $a  $b"
done
[ "$bad" -eq 0 ]
