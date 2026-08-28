#!/bin/sh
# Fixtures: *.ok.ace must compile through the landed stages with zero errors;
# *.err.ace must fail. tests/lexer and tests/parser run parse-only (--ast);
# tests/sema runs the full check (default mode).
set -u
fail=0
tab=$(printf '\t')

for f in tests/lexer/*.ok.ace tests/parser/*.ok.ace; do
    if ! ./ace --ast "$f" >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL (should parse clean): $f"
        cat /tmp/ace_test_errs
        fail=1
    fi
done

for f in tests/lexer/*.err.ace tests/parser/*.err.ace; do
    if ./ace "$f" >/dev/null 2>&1; then
        echo "FAIL (should report errors): $f"
        fail=1
    fi
done

for f in tests/sema/*.ok.ace; do
    if ! ./ace "$f" >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL (should check clean): $f"
        cat /tmp/ace_test_errs
        fail=1
    fi
done

for f in tests/sema/*.err.ace; do
    if ./ace "$f" >/dev/null 2>&1; then
        echo "FAIL (should report errors): $f"
        fail=1
    fi
done

# emit fixtures: compile to an object, link with the system cc, run, and
# check the exit code. The expected code rides in a `// expect-exit: N` line.
if [ -d tests/emit ]; then
    ACE=$(cd . && pwd)/ace
    mkdir -p /tmp/ace_emit
    for f in tests/emit/*.ace; do
        name=$(basename "$f" .ace)
        exp=$(sed -n 's/^.*expect-exit:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$f")
        if [ -z "$exp" ]; then
            echo "FAIL: $f has no '// expect-exit: N' line"
            fail=1
            continue
        fi
        cp "$f" "/tmp/ace_emit/$name.ace"
        ( cd /tmp/ace_emit && "$ACE" -c "$name.ace" && cc "$name.o" -o "$name" && "./$name" )
        got=$?
        if [ "$got" -ne "$exp" ]; then
            echo "FAIL: $f exited $got, want $exp"
            fail=1
        fi
    done
    # -o mode drives the same pipeline through the cc link step
    "$ACE" -o /tmp/ace_emit/direct tests/emit/hello_min.ace && /tmp/ace_emit/direct
    got=$?
    if [ "$got" -ne 42 ]; then
        echo "FAIL: -o mode exited $got, want 42"
        fail=1
    fi
    # Darwin's DWARF reader must see both the source rows and the minimal
    # compile-unit/function metadata used to preserve them through linking.
    if command -v dwarfdump >/dev/null 2>&1; then
        dwarf=$(dwarfdump --debug-info --debug-line /tmp/ace_emit/hello_min.o 2>/tmp/ace_test_errs)
        if [ $? -ne 0 ]; then
            echo "FAIL: dwarfdump rejected emitter debug info"
            cat /tmp/ace_test_errs
            fail=1
        elif ! printf '%s\n' "$dwarf" | grep -q 'DW_AT_name.*("main")'; then
            echo "FAIL: DWARF should describe the main function"
            fail=1
        elif ! printf '%s\n' "$dwarf" | grep -Eq '^0x[0-9a-f]+[[:space:]]+2[[:space:]]+1'; then
            echo "FAIL: DWARF should map main to hello_min.ace:2:1"
            fail=1
        elif ! printf '%s\n' "$dwarf" | grep -Eq '^0x[0-9a-f]+[[:space:]]+3[[:space:]]+5'; then
            echo "FAIL: DWARF should map return to hello_min.ace:3:5"
            fail=1
        fi
    fi
fi

# lexer spot-checks
if ! ./ace --tokens tests/lexer/hello.ok.ace | grep -q "^2:1${tab}import$"; then
    echo "FAIL: first token of hello.ok.ace should be 'import'"
    fail=1
fi
if ! ./ace --tokens tests/lexer/numbers.ok.ace | grep -q "${tab}int${tab}255$"; then
    echo "FAIL: 0xFF should lex as int 255"
    fail=1
fi
if ! ./ace --tokens tests/lexer/numbers.ok.ace | grep -q "${tab}int${tab}172$"; then
    echo "FAIL: 0b1010_1100 should lex as int 172"
    fail=1
fi
if ! ./ace --tokens tests/lexer/numbers.ok.ace | grep -q "${tab}float${tab}0.0015$"; then
    echo "FAIL: 1.5e-3 should lex as float 0.0015"
    fail=1
fi
if ! ./ace --tokens tests/lexer/numbers.ok.ace | grep -q "${tab}char${tab}'A'$"; then
    echo "FAIL: 'A' should lex as char 'A'"
    fail=1
fi

# parser spot-checks on the full-surface fixture
ast() { ./ace --ast tests/parser/full.ok.ace; }
if ! ast | grep -q "(enum 'Color'"; then
    echo "FAIL: enum Color should appear in the AST"
    fail=1
fi
if ! ast | grep -qF "op='+='"; then
    echo "FAIL: compound assignment += should appear in the AST"
    fail=1
fi
if ! ast | grep -qF "op='<<='"; then
    echo "FAIL: compound assignment <<= should appear in the AST"
    fail=1
fi
if ! ast | grep -q "(cmpchain"; then
    echo "FAIL: chained comparison should parse as one cmpchain node"
    fail=1
fi
if ! ast | grep -q "(label 'scan'"; then
    echo "FAIL: labeled block 'scan' should appear in the AST"
    fail=1
fi
if ! ast | grep -q "(func_t"; then
    echo "FAIL: function value type should appear in the AST"
    fail=1
fi
if ! ast | grep -q "(externfn 'printf'.* varargs"; then
    echo "FAIL: variadic extern printf should carry the varargs flag"
    fail=1
fi
if ! ast | grep -q "(param 'err'"; then
    echo "FAIL: destructuring binder 'err' should appear in the AST"
    fail=1
fi
# `?` binds loosest: *Point? is an optional pointer (opt wrapping ptr)
if ! ast | grep -A2 "(opt " | grep -q "(ptr "; then
    echo "FAIL: *T? should parse as opt(ptr)"
    fail=1
fi
# shifts/bitwise parse with C precedence
if ! ./ace --ast tests/lexer/numbers.ok.ace | grep -qF "op='<<'"; then
    echo "FAIL: << operator should parse"
    fail=1
fi
if ! ./ace --ast tests/lexer/numbers.ok.ace | grep -qF "op='>>'"; then
    echo "FAIL: >> operator should parse"
    fail=1
fi

if [ $fail -eq 0 ]; then
    echo "all tests passed"
fi
exit $fail
