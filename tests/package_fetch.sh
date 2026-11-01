#!/bin/sh
# Exercises a fetched [link] input end to end against a local HTTP server, under
# the given compiler (default ./ace): download, cache hit offline, a damaged
# cache, a wrong hash, a missing target, and a root override. Run from the root.
set -u
ace=${1:-./ace}
case "$ace" in /*) ;; *) ace="$PWD/$ace" ;; esac
tmp=$(mktemp -d)
trap 'kill "$server" 2>/dev/null; rm -rf "$tmp"' EXIT
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: expected $2, got $3"; fails=$((fails + 1)); fi; }

mkdir -p "$tmp/www" "$tmp/app/pkg/dep" "$tmp/home"
printf 'int fetched_value(void) { return 42; }\n' > "$tmp/v.c"
cc -c "$tmp/v.c" -o "$tmp/v.o" && ar rcs "$tmp/www/libv.a" "$tmp/v.o"
hash=$(shasum -a 256 "$tmp/www/libv.a" | cut -d' ' -f1)
"$ace" -o "$tmp/server" tests/fetch_server.ace || exit 1
"$tmp/server" "$tmp/www" > "$tmp/port" &
server=$!
i=0
while [ ! -s "$tmp/port" ] && [ $i -lt 50 ]; do i=$((i + 1)); sleep 0.1; done
port=$(cat "$tmp/port")

cat > "$tmp/app/ace.pkg" <<M
name: app
version: 0.1.0
ace: 0.1
type: exe
root: main.ace

[dependency]
dep: 0.1.0
M
printf 'import dep\nmain(): int { return dep.value() }\n' > "$tmp/app/main.ace"
printf 'extern "c" { fetched_value(): int }\npub value(): int { return fetched_value() }\n' > "$tmp/app/pkg/dep/mod.ace"
dep() {
    cat > "$tmp/app/pkg/dep/ace.pkg" <<M
name: dep
version: 0.1.0
ace: 0.1
type: lib
root: mod.ace

[link$2]
url: http://127.0.0.1:$port/libv.a
sha256: $1
M
}
build() { (cd "$tmp/app" && rm -rf build && HOME="$tmp/home" "$ace" pkg build 2>"$tmp/err"); echo $?; }
run() { "$tmp/app/build/bin/app"; echo $?; }
cached="$tmp/home/.ace/cache/$hash/libv.a"

dep "$hash" ""
check "download" 0 "$(build)"
check "linked" 42 "$(run)"
check "cached" true "$([ -f "$cached" ] && echo true)"
rm "$tmp/www/libv.a"
check "cache hit needs no server" 0 "$(build)"
printf x >> "$cached"
check "damaged cache cannot be refetched" 1 "$(build)"
check "damaged cache reports fetch" 1 "$(grep -c 'could not be fetched' "$tmp/err")"
ar rcs "$tmp/www/libv.a" "$tmp/v.o"
hash2=$(shasum -a 256 "$tmp/www/libv.a" | cut -d' ' -f1)
dep "$hash2" ""
rm -rf "$tmp/home/.ace"
build >/dev/null
printf x >> "$tmp/home/.ace/cache/$hash2/libv.a"
check "damaged cache redownloads" 0 "$(build)"
dep 0000000000000000000000000000000000000000000000000000000000000000 ""
check "wrong hash fails" 1 "$(build)"
check "wrong hash reports" 1 "$(grep -c 'does not match its sha256' "$tmp/err")"
dep "$hash2" ": linux-x64"
check "no section for the target fails" 1 "$(build)"
check "no section reports" 1 "$(grep -c 'no \[link\] section' "$tmp/err")"
mkdir -p "$tmp/app/vendor"
cp "$tmp/www/libv.a" "$tmp/app/vendor/libv.a"
printf '\n[link]\npath: vendor/libv.a\n' >> "$tmp/app/ace.pkg"
dep "$hash2" ""
kill "$server" 2>/dev/null
rm -rf "$tmp/home/.ace"
check "root path overrides without fetching" 0 "$(build)"
check "override linked" 42 "$(run)"
check "override left no cache" false "$([ -e "$tmp/home/.ace" ] && echo true || echo false)"
exit "$fails"
