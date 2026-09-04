#!/bin/sh
# Fixtures: *_ok.ace must compile through the landed stages with zero errors;
# *_err.ace must fail. Fixtures live in feature-grouped directories — one
# directory per feature with a main fixture plus aspect-named companions.
# tests/lexer and tests/parser run parse-only (--ast); tests/sema runs the
# full check (default mode).
set -u
fail=0
tab=$(printf '\t')
ACE_BIN=${ACE_BIN:-./ace}
ACE_DIR=$(dirname "$ACE_BIN")
ACE=$(cd "$ACE_DIR" && pwd)/$(basename "$ACE_BIN")

for f in $(find tests/lexer tests/parser -name '*_ok.ace' | sort); do
    if ! "$ACE" --ast "$f" >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL (should parse clean): $f"
        cat /tmp/ace_test_errs
        fail=1
    fi
done

for f in $(find tests/lexer tests/parser -name '*_err.ace' | sort); do
    if "$ACE" "$f" >/dev/null 2>&1; then
        echo "FAIL (should report errors): $f"
        fail=1
    fi
done

unknown_decl=$("$ACE" tests/parser/unknown_decl_type_err.ace 2>&1)
if ! printf '%s\n' "$unknown_decl" | grep -qF \
    "tests/parser/unknown_decl_type_err.ace:3:8: error: unknown declaration type 'protocol'"; then
    echo "FAIL: unknown declaration type should point at the type name"
    fail=1
fi

for case in empty NUL long; do
    case "$case" in
        empty) file=extern_link_name/empty_err.ace
               want="extern link name cannot be empty" ;;
        NUL)   file=extern_link_name/nul_err.ace
               want="extern link name cannot contain NUL" ;;
        long)  file=extern_link_name/long_err.ace
               want="extern link name cannot exceed 510 bytes" ;;
    esac
    link_name_error=$("$ACE" "tests/parser/$file" 2>&1)
    if ! printf '%s\n' "$link_name_error" | grep -qF "$want"; then
        echo "FAIL: extern link-name $case diagnostic"
        fail=1
    fi
done
default_link_name_error=$("$ACE" tests/parser/extern_link_name/default_long_err.ace 2>&1)
if ! printf '%s\n' "$default_link_name_error" | grep -qF \
    "extern link name cannot exceed 510 bytes"; then
    echo "FAIL: default extern link-name length diagnostic"
    fail=1
fi

for f in $(find tests/sema -name '*_ok.ace' | sort); do
    if ! "$ACE" "$f" >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL (should check clean): $f"
        cat /tmp/ace_test_errs
        fail=1
    fi
done

for f in $(find tests/sema -name '*_err.ace' | sort); do
    if "$ACE" "$f" >/dev/null 2>&1; then
        echo "FAIL (should report errors): $f"
        fail=1
    fi
done

fallthrough_errors=$("$ACE" tests/sema/conditions/fallthrough_placement_err.ace 2>&1)
for want in \
    "fallthrough must be the final statement of a switch case" \
    "fallthrough is not allowed in the final switch case"; do
    if ! printf '%s\n' "$fallthrough_errors" | grep -qF "$want"; then
        echo "FAIL: missing fallthrough diagnostic '$want'"
        fail=1
    fi
done
fallthrough_union_error=$("$ACE" tests/sema/conditions/fallthrough_union_err.ace 2>&1)
if ! printf '%s\n' "$fallthrough_union_error" | grep -qF \
    "fallthrough is not allowed in a tagged-union switch"; then
    echo "FAIL: missing tagged-union fallthrough diagnostic"
    fail=1
fi

defer_placement_errors=$("$ACE" tests/sema/defer/placement_err.ace 2>&1)
defer_placement_count=$(printf '%s\n' "$defer_placement_errors" | grep -cF \
    "defer statement must appear at the top level of the function body")
if [ "$defer_placement_count" -ne 4 ]; then
    echo "FAIL: expected 4 defer placement diagnostics, got $defer_placement_count"
    printf '%s\n' "$defer_placement_errors"
    fail=1
fi
defer_noncall_errors=$("$ACE" tests/sema/defer/noncall_err.ace 2>&1)
defer_noncall_count=$(printf '%s\n' "$defer_noncall_errors" | grep -cF \
    "defer operand must be a call")
if [ "$defer_noncall_count" -ne 2 ]; then
    echo "FAIL: expected 2 defer operand diagnostics, got $defer_noncall_count"
    printf '%s\n' "$defer_noncall_errors"
    fail=1
fi

recursive_layout_errors=$("$ACE" tests/sema/types/recursive_layout_err.ace 2>&1)
for name in Direct ArrayCycle UnionCycle DistinctCycle; do
    if ! printf '%s\n' "$recursive_layout_errors" | grep -qF \
        "type '$name' has a recursive value layout"; then
        echo "FAIL: missing recursive value-layout diagnostic for $name"
        fail=1
    fi
done
generic_layout_error=$("$ACE" tests/sema/types/generic_recursive_layout_err.ace 2>&1)
if ! printf '%s\n' "$generic_layout_error" | grep -qF \
    "recursive value layout"; then
    echo "FAIL: missing recursive generic value-layout diagnostic"
    fail=1
fi
inline_forward_errors=$("$ACE" tests/sema/types/inline_forward_err.ace 2>&1)
for name in B D F; do
    if ! printf '%s\n' "$inline_forward_errors" | grep -qF "unknown type '$name'"; then
        echo "FAIL: inline forward type $name should be rejected"
        fail=1
    fi
done
unresolved_forward_error=$("$ACE" tests/sema/types/unresolved_forward_err.ace 2>&1)
if ! printf '%s\n' "$unresolved_forward_error" | grep -qF \
    "pointer-only forward type 'Missing' was never declared in this module"; then
    echo "FAIL: missing unresolved pointer-only forward-type diagnostic"
    fail=1
fi
premature_member_error=$("$ACE" tests/sema/types/premature_forward_member_err.ace 2>&1)
if ! printf '%s\n' "$premature_member_error" | grep -qF \
    "type 'B' is not declared yet; member access requires its declaration"; then
    echo "FAIL: missing premature forward-type member diagnostic"
    fail=1
fi

c_abi_errors=$("$ACE" tests/sema/c_abi/rejected_err.ace 2>&1)
for want in \
    "parameter 'value' has type bool, which is not C-ABI compatible" \
    "parameter 'value' has type string, which is not C-ABI compatible" \
    "return type (tuple) is not C-ABI compatible" \
    "return type [4]i32 is not C-ABI compatible" \
    "parameter 'value' has type *T, which is not C-ABI compatible" \
    "parameter 'value' has type BadField, which is not C-ABI compatible" \
    "parameter 'value' has type Box, which is not C-ABI compatible" \
    "parameter 'value' is 'mut' — mut parameters stay internal; pass a mut *T instead" \
    "function 'bad_generic' cannot be generic"; do
    if ! printf '%s\n' "$c_abi_errors" | grep -qF "$want"; then
        echo "FAIL: missing C ABI diagnostic '$want'"
        fail=1
    fi
done

printf_format_errors=$("$ACE" tests/sema/printf/formats_err.ace 2>&1)
for want in \
    "format must be a string literal or static-literal const global" \
    "incomplete fmt.printf conversion" \
    "unsupported fmt.printf conversion '%q'" \
    "width, flags, '*', and positional arguments are not supported" \
    "precision needs decimal digits" \
    "dynamic '*' precision is not supported" \
    "precision is supported only for %f" \
    "precision 325 exceeds the fixed-buffer limit 324" \
    "format requires 1 value argument" \
    "extra fmt.printf argument 1"; do
    if ! printf '%s\n' "$printf_format_errors" | grep -qF "$want"; then
        echo "FAIL: missing printf format diagnostic '$want'"
        fail=1
    fi
done
printf_type_errors=$("$ACE" tests/sema/printf/types_err.ace 2>&1)
for want in \
    "requires int, got i32" \
    "requires uint or usize, got u16" \
    "requires uint or usize, got Meters" \
    "requires f32 or f64, got integer literal" \
    "requires string, got u8" \
    "requires u8, got string" \
    "fmt.printf is direct-call-only"; do
    if ! printf '%s\n' "$printf_type_errors" | grep -qF "$want"; then
        echo "FAIL: missing printf type diagnostic '$want'"
        fail=1
    fi
done

array_repeat_errors=$("$ACE" tests/sema/arrays/repetition_err.ace 2>&1)
for want in \
    "array repetition count is 3, expected 4 for [4]i32" \
    "type mismatch: float literal given where i32 expected" \
    "array literal needs its array type from context"; do
    if ! printf '%s\n' "$array_repeat_errors" | grep -qF "$want"; then
        echo "FAIL: missing array repetition diagnostic '$want'"
        fail=1
    fi
done

mem_typed_errors=$("$ACE" tests/sema/memory/typed_err.ace 2>&1)
for want in \
    "type mismatch: mut []usize given where []i32 expected" \
    "type mismatch: i64 given where usize expected" \
    "'mut' marks mut parameters; mem.alloc[T] arguments are read-only" \
    "unknown type 'Missing'" \
    "mem.release[T] needs 'mut' at the call site: mem.release[T](mut data)" \
    "mem.release[T] requires a mutable []usize allocation variable" \
    "mem.release requires a mutable []u8 allocation variable — a typed slice states its element type: mem.release[T](mut data)" \
    "type mismatch: mut []u8 given where []usize expected" \
    "mem.alloc[T] takes exactly one element count" \
    "mem.alloc takes exactly size and alignment"; do
    if ! printf '%s\n' "$mem_typed_errors" | grep -qF "$want"; then
        echo "FAIL: missing typed allocation diagnostic '$want'"
        fail=1
    fi
done

string_view_errors=$("$ACE" tests/sema/sequences/view_rejections_err.ace 2>&1)
if ! printf '%s\n' "$string_view_errors" | grep -qF "cannot cast [2]u8 to string"; then
    echo "FAIL: missing string view-conversion rejection diagnostic"
    fail=1
fi
bounds_hi=$("$ACE" tests/sema/sequences/bounds_hi_err.ace 2>&1)
if ! printf '%s\n' "$bounds_hi" | grep -qF "slice upper bound 6 exceeds source length 5"; then
    echo "FAIL: missing constant slice upper-bound diagnostic"
    fail=1
fi
bounds_order=$("$ACE" tests/sema/sequences/bounds_order_err.ace 2>&1)
if ! printf '%s\n' "$bounds_order" | grep -qF "slice lower bound 3 exceeds upper bound 1"; then
    echo "FAIL: missing constant slice ordering diagnostic"
    fail=1
fi
bounds_neg=$("$ACE" tests/sema/sequences/bounds_negative_err.ace 2>&1)
if ! printf '%s\n' "$bounds_neg" | grep -qF "slice upper bound -2 is negative"; then
    echo "FAIL: missing constant slice negative-bound diagnostic"
    fail=1
fi
parse_unsupported=$("$ACE" tests/sema/sequences/parse_unsupported_err.ace 2>&1)
for want in \
    "call 'str\$parse[bool]' matches no specialization" \
    "call 'str\$parse[f64]' matches no specialization" \
    "call 'str\$parse[u8]' matches no specialization"; do
    if ! printf '%s\n' "$parse_unsupported" | grep -qF "$want"; then
        echo "FAIL: missing str.parse diagnostic '$want'"
        fail=1
    fi
done

spec_mismatch_error=$("$ACE" tests/sema/specialization/main_err.ace 2>&1)
if ! printf '%s\n' "$spec_mismatch_error" | grep -qF \
    "does not conform to any generic overload of 'pick'"; then
    echo "FAIL: missing specialization conformance diagnostic"
    fail=1
fi
duplicate_spec_error=$("$ACE" tests/sema/specialization/duplicate_err.ace 2>&1)
if ! printf '%s\n' "$duplicate_spec_error" | grep -qF \
    "duplicate specialization 'pick[i64]' — one exact type-argument tuple per generic overload"; then
    echo "FAIL: missing duplicate-specialization diagnostic"
    fail=1
fi
missing_generic_error=$("$ACE" tests/sema/specialization/missing_generic_err.ace 2>&1)
if ! printf '%s\n' "$missing_generic_error" | grep -qF \
    "a specialization must follow a generic declaration of 'pick'"; then
    echo "FAIL: missing missing-generic diagnostic"
    fail=1
fi
unspecialized_call_error=$("$ACE" tests/sema/specialization/unspecialized_call_err.ace 2>&1)
if ! printf '%s\n' "$unspecialized_call_error" | grep -qF \
    "matches no specialization — 'load' is declared 'specialized' (no default body) at line 4"; then
    echo "FAIL: missing no-default call-site diagnostic"
    fail=1
fi
zero_spec_error=$("$ACE" tests/sema/specialization/zero_err.ace 2>&1)
if ! printf '%s\n' "$zero_spec_error" | grep -qF \
    "is marked 'specialized' but no specialization is declared"; then
    echo "FAIL: missing zero-specialization declaration diagnostic"
    fail=1
fi

constructor_wins_error=$("$ACE" tests/sema/consts/main_err.ace 2>&1)
if ! printf '%s\n' "$constructor_wins_error" | grep -qF \
    "type mismatch: i32 given where u8 expected (casts are explicit)"; then
    echo "FAIL: missing same-named-function-wins diagnostic"
    fail=1
fi
constructor_arity_error=$("$ACE" tests/sema/consts/arity_err.ace 2>&1)
if ! printf '%s\n' "$constructor_arity_error" | grep -qF \
    "wrong number of arguments: 1 given, 2 expected"; then
    echo "FAIL: missing same-named-function arity diagnostic"
    fail=1
fi
constructor_fit_error=$("$ACE" tests/sema/consts/fit_err.ace 2>&1)
if ! printf '%s\n' "$constructor_fit_error" | grep -qF \
    "integer literal 300 out of range for u8"; then
    echo "FAIL: missing constructor-constant fit diagnostic"
    fail=1
fi

# Constructor-constant hazards: the constant folder writes the argument's own
# representation, so int<->float crossings must not fold. Each fixture fails
# to compile and documents its diagnostic on a `// error:` line; the check is
# driven through -c because the folder only runs when code is emitted.
if [ -d tests/emit_errors ]; then
    mkdir -p /tmp/ace_emit_errors
    for f in tests/emit_errors/*.ace; do
        name=$(basename "$f" .ace)
        want=$(sed -n 's/^.*error:[[:space:]]*//p' "$f" | head -1)
        cp "$f" "/tmp/ace_emit_errors/$name.ace"
        if (cd /tmp/ace_emit_errors && "$ACE" -c "$name.ace") >/dev/null 2>/tmp/ace_test_errs; then
            echo "FAIL (should report errors): $f"
            fail=1
        elif [ -z "$want" ] || ! grep -qF "$want" /tmp/ace_test_errs; then
            echo "FAIL: expected diagnostic '$want' from $f"
            cat /tmp/ace_test_errs
            fail=1
        fi
    done
fi

# A bare std import accepts either std/name.ace or std/name/module.ace, but
# diagnoses the std name when both definitions exist.
mkdir -p /tmp/ace_std_module/normal/std/directory_probe
mkdir -p /tmp/ace_std_module/ambiguous/std/ambiguous_probe
cp "$ACE" /tmp/ace_std_module/normal/ace
cp "$ACE" /tmp/ace_std_module/ambiguous/ace
cp tests/stdlib_module/module.ace \
   /tmp/ace_std_module/normal/std/directory_probe/module.ace
cp tests/stdlib_module/flat.ace \
   /tmp/ace_std_module/ambiguous/std/ambiguous_probe.ace
cp tests/stdlib_module/module.ace \
   /tmp/ace_std_module/ambiguous/std/ambiguous_probe/module.ace
if ! /tmp/ace_std_module/normal/ace -o /tmp/ace_std_module/normal/out \
     tests/stdlib_module/main.ace || ! /tmp/ace_std_module/normal/out; then
    echo "FAIL: std directory module should resolve through module.ace"
    fail=1
fi
ambiguous_std=$(/tmp/ace_std_module/ambiguous/ace tests/stdlib_module/ambiguous.ace 2>&1)
for want in "ambiguous_probe.ace" "ambiguous_probe/module.ace"; do
    if ! printf '%s\n' "$ambiguous_std" | grep -qF "$want"; then
        echo "FAIL: ambiguous std diagnostic should name '$want'"
        fail=1
    fi
done
missing_std=$(/tmp/ace_std_module/normal/ace tests/stdlib_module/missing.ace 2>&1)
for want in "absent_module_probe.ace" "absent_module_probe/module.ace"; do
    if ! printf '%s\n' "$missing_std" | grep -qF "$want"; then
        echo "FAIL: missing std diagnostic should name '$want'"
        fail=1
    fi
done

# emit fixtures: compile to an object, link with the system cc, run, and
# check the exit code. The expected code rides in a `// expect-exit: N` line.
if [ -d tests/emit ]; then
    rm -rf /tmp/ace_emit
    mkdir -p /tmp/ace_emit
    for f in $(find tests/emit -name '*.ace' | sort); do
        rel=${f#tests/emit/}
        name=${rel%.ace}
        exp=$(sed -n 's/^.*expect-exit:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$f")
        if [ -z "$exp" ]; then
            echo "FAIL: $f has no '// expect-exit: N' line"
            fail=1
            continue
        fi
        mkdir -p "/tmp/ace_emit/$(dirname "$rel")"
        cp "$f" "/tmp/ace_emit/$rel"
        rm -f /tmp/ace_test_run
        out=$(cd /tmp/ace_emit && { "$ACE" -c "$rel" && cc "$name.o" -o "$name" && "./$name"; } 2>/tmp/ace_test_run)
        got=$?
        if [ "$got" -ne "$exp" ]; then
            echo "FAIL: $f exited $got, want $exp"
            cat /tmp/ace_test_run
            fail=1
        fi
        expout=$(sed -n 's/^.*expect-output:[[:space:]]*//p' "$f")
        if [ -n "$expout" ] && [ "$out" != "$expout" ]; then
            echo "FAIL: $f output '$out', want '$expout'"
            fail=1
        fi
    done
    precision_out=$(/tmp/ace_emit/printf/precision)
    if [ "${#precision_out}" -ne 326 ] ||
       [ "${precision_out#0.}" = "$precision_out" ] ||
       [ "${precision_out#${precision_out%?}}" != "5" ]; then
        echo "FAIL: %.324f should emit 0., 323 zero bytes, and a final 5"
        fail=1
    fi
    precision_max_out=$(/tmp/ace_emit/printf/precision_max)
    if [ "${#precision_max_out}" -ne 635 ] ||
       [ "${precision_max_out#-17976931348623157}" = "$precision_max_out" ] ||
       [ "${precision_max_out#${precision_max_out%?}}" != "0" ]; then
        echo "FAIL: maximum negative f64 with %.324f must fit the 640-byte buffer"
        fail=1
    fi
    process_empty_out=$(/usr/bin/env -i /tmp/ace_emit/sys/process)
    process_empty_expected=$(printf '1\n/tmp/ace_emit/sys/process\n0')
    if [ "$process_empty_out" != "$process_empty_expected" ]; then
        echo "FAIL: sys empty argument/environment output did not match"
        printf 'got:\n%s\nwant:\n%s\n' "$process_empty_out" "$process_empty_expected"
        fail=1
    fi
    process_one_out=$(/usr/bin/env -i 'ACE_ENV_ONE=first' \
        /tmp/ace_emit/sys/process single)
    process_one_expected=$(printf '2\n/tmp/ace_emit/sys/process\nsingle\n1\nACE_ENV_ONE=first')
    if [ "$process_one_out" != "$process_one_expected" ]; then
        echo "FAIL: sys single argument/environment output did not match"
        printf 'got:\n%s\nwant:\n%s\n' "$process_one_out" "$process_one_expected"
        fail=1
    fi
    process_out=$(/usr/bin/env -i 'ACE_ENV_ONE=first' \
        'ACE_TEST_ENV=value=with=equals and spaces' \
        /tmp/ace_emit/sys/process 'one argument' 'quote"argument')
    process_expected=$(printf '3\n/tmp/ace_emit/sys/process\none argument\nquote"argument\n2\nACE_ENV_ONE=first\nACE_TEST_ENV=value=with=equals and spaces')
    if [ "$process_out" != "$process_expected" ]; then
        echo "FAIL: sys process arguments/environment output did not match"
        printf 'got:\n%s\nwant:\n%s\n' "$process_out" "$process_expected"
        fail=1
    fi
    if command -v nm >/dev/null 2>&1; then
        printf_undef=$(nm -u /tmp/ace_emit/printf/main.o 2>/dev/null)
        if printf '%s\n' "$printf_undef" |
           grep -Eq '_(printf|snprintf|vprintf|malloc|calloc|realloc|free)$'; then
            echo "FAIL: fmt.printf must not depend on C formatting, variadics, or allocation"
            fail=1
        fi
        if nm /tmp/ace_emit/printf/main.o 2>/dev/null |
           grep -qF '__ace$fn$fmt$printf$prstring$'; then
            echo "FAIL: checked fmt.printf calls must not emit a runtime printf body"
            fail=1
        fi
        net_undef=$(nm -u /tmp/ace_emit/net/main.o 2>/dev/null)
        if printf '%s\n' "$net_undef" |
           grep -Eq '_(malloc|calloc|realloc|free|printf|snprintf)$'; then
            echo "FAIL: net must not call a general-purpose allocator or C formatting"
            fail=1
        fi
        for symbol in _socket _setsockopt _fcntl _bind _listen _accept \
                      _connect _recv _send _getaddrinfo _freeaddrinfo \
                      _close ___error; do
            if ! printf '%s\n' "$net_undef" | grep -q "^$symbol$"; then
                echo "FAIL: net object is missing Darwin boundary '$symbol'"
                fail=1
            fi
        done
        process_undef=$(nm -u /tmp/ace_emit/sys/process.o 2>/dev/null)
        for symbol in __NSGetArgc __NSGetArgv __NSGetEnviron _strlen; do
            if ! printf '%s\n' "$process_undef" | grep -q "^$symbol$"; then
                echo "FAIL: sys process object is missing Darwin boundary '$symbol'"
                fail=1
            fi
        done
        if printf '%s\n' "$process_undef" |
           grep -Eq '_(malloc|calloc|realloc|free)$'; then
            echo "FAIL: sys process access must not allocate"
            fail=1
        fi
    fi
    # -o mode drives the same pipeline through the cc link step
    "$ACE" -o /tmp/ace_emit/direct tests/emit/basics/main.ace && /tmp/ace_emit/direct
    got=$?
    if [ "$got" -ne 42 ]; then
        echo "FAIL: -o mode exited $got, want 42"
        fail=1
    fi
    # --report writes whole-compilation statistics to stderr. welcome imports
    # fmt, so the source-file count also verifies imported-module accounting.
    report=$(cd /tmp/ace_emit/basics && "$ACE" --report -c welcome.ace 2>&1 >/dev/null)
    if ! printf '%s\n' "$report" | grep -q '^Ace compile report:$'; then
        echo "FAIL: --report heading missing"
        fail=1
    fi
    if ! printf '%s\n' "$report" | grep -q '^--------------------$'; then
        echo "FAIL: --report separator missing"
        fail=1
    fi
    if ! printf '%s\n' "$report" | grep -q 'source files:.*2$'; then
        echo "FAIL: --report should count root + imported fmt source"
        fail=1
    fi
    for label in 'source bytes:' 'tokens:' 'machine code:' 'object file:' 'compile time:'; do
        if ! printf '%s\n' "$report" | grep -q "$label"; then
            echo "FAIL: --report missing '$label'"
            fail=1
        fi
    done
    if ! printf '%s\n' "$report" | grep -q 'machine code:.*[1-9][0-9,]* bytes$'; then
        echo "FAIL: --report machine-code size should be nonzero"
        fail=1
    fi
    if ! printf '%s\n' "$report" | grep -q 'object file:.*[1-9][0-9,]* bytes$'; then
        echo "FAIL: --report object-file size should be nonzero"
        fail=1
    fi
    # Darwin's DWARF reader must see both the source rows and the minimal
    # compile-unit/function metadata used to preserve them through linking.
    if command -v dwarfdump >/dev/null 2>&1; then
        dwarf=$(dwarfdump --debug-info --debug-line /tmp/ace_emit/basics/main.o 2>/tmp/ace_test_errs)
        if [ $? -ne 0 ]; then
            echo "FAIL: dwarfdump rejected emitter debug info"
            cat /tmp/ace_test_errs
            fail=1
        elif ! printf '%s\n' "$dwarf" | grep -q 'DW_AT_name.*("main")'; then
            echo "FAIL: DWARF should describe the main function"
            fail=1
        elif ! printf '%s\n' "$dwarf" | grep -Eq '^0x[0-9a-f]+[[:space:]]+5[[:space:]]+1'; then
            echo "FAIL: DWARF should map main to basics/main.ace:5:1"
            fail=1
        elif ! printf '%s\n' "$dwarf" | grep -Eq '^0x[0-9a-f]+[[:space:]]+6[[:space:]]+5'; then
            echo "FAIL: DWARF should map return to basics/main.ace:6:5"
            fail=1
        fi
        multidwarf=$(dwarfdump --debug-line /tmp/ace_emit/basics/welcome.o 2>/tmp/ace_test_errs)
        if [ $? -ne 0 ] ||
           ! printf '%s\n' "$multidwarf" | grep -q '/std/fmt.ace' ||
           ! printf '%s\n' "$multidwarf" | grep -q '/welcome.ace'; then
            echo "FAIL: imported and root sources should both appear in DWARF"
            cat /tmp/ace_test_errs
            fail=1
        fi
    fi
    # Bare locals reserve stack storage but emit no initialization stores,
    # including a large array. The frame adjustment itself is expected.
    if command -v otool >/dev/null 2>&1; then
        nozero=$(otool -tvV /tmp/ace_emit/definite_init/no_implicit_zeroing.o)
        if printf '%s\n' "$nozero" | grep -Eq '[[:space:]]str[^a-z]|_memset'; then
            echo "FAIL: bare locals should not emit zeroing stores"
            fail=1
        fi
    fi
fi


# module fixtures: one directory per case with a main.ace. A `// expect-exit:
# N` line compiles + links + runs in place (companion modules stay beside it
# so relative imports resolve); without one the case must fail to compile.
if [ -d tests/modules ]; then
    mkdir -p /tmp/ace_modules
    for f in tests/modules/*/main.ace; do
        name=$(basename "$(dirname "$f")")
        exp=$(sed -n 's/^.*expect-exit:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$f")
        if [ -z "$exp" ]; then
            if "$ACE" "$f" >/dev/null 2>&1; then
                echo "FAIL (should report errors): $f"
                fail=1
            fi
            continue
        fi
        rm -f /tmp/ace_test_run
        out=$({ "$ACE" -o "/tmp/ace_modules/$name" "$f" && "/tmp/ace_modules/$name"; } 2>/tmp/ace_test_run)
        got=$?
        if [ "$got" -ne "$exp" ]; then
            echo "FAIL: $f exited $got, want $exp"
            cat /tmp/ace_test_run
            fail=1
        fi
        expout=$(sed -n 's/^.*expect-output:[[:space:]]*//p' "$f")
        if [ -n "$expout" ] && [ "$out" != "$expout" ]; then
            echo "FAIL: $f output '$out', want '$expout'"
            fail=1
        fi
    done
    # multi-file DWARF across a local module, a stdlib module, and the root
    if command -v dwarfdump >/dev/null 2>&1; then
        moddwarf=$(dwarfdump --debug-line /tmp/ace_modules/diamond.o 2>/tmp/ace_test_errs)
        if [ $? -ne 0 ] ||
           ! printf '%s\n' "$moddwarf" | grep -q '/shared.ace' ||
           ! printf '%s\n' "$moddwarf" | grep -q '/main.ace'; then
            echo "FAIL: module and root sources should both appear in DWARF"
            cat /tmp/ace_test_errs
            fail=1
        fi
    fi
fi

# driver fixtures: external link inputs and options through the -o pipeline.
# The cc launch is an argument vector with no shell, so paths with spaces and
# shell metacharacters link exactly as written; -L/-l and -framework cover
# archives and Apple frameworks; validation and linker failures are checked.
if [ -d tests/driver ]; then
    mkdir -p "/tmp/ace_driver/lib dir" "/tmp/ace_driver/space dir"
    cc -c tests/driver/helper.c -o /tmp/ace_driver/helper_raw.o
    cc -c tests/driver/abi.c -o /tmp/ace_driver/abi_c.o
    ar rcs "/tmp/ace_driver/lib dir/libhelper.a" /tmp/ace_driver/helper_raw.o
    cp /tmp/ace_driver/helper_raw.o '/tmp/ace_driver/evil;$(echo pwn)`id`.o'
    cp /tmp/ace_driver/helper_raw.o "/tmp/ace_driver/space dir/help lib.o"

    out=$("$ACE" -o /tmp/ace_driver/app_obj tests/driver/main_obj.ace \
          /tmp/ace_driver/helper_raw.o && /tmp/ace_driver/app_obj)
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: linking an extra .o exited $got, want 7"
        fail=1
    fi
    out=$("$ACE" -o /tmp/ace_driver/app_lib tests/driver/main_obj.ace \
          -L "/tmp/ace_driver/lib dir" -l helper && /tmp/ace_driver/app_lib)
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: -L/-l archive link exited $got, want 7"
        fail=1
    fi
    out=$("$ACE" -o /tmp/ace_driver/app_fw tests/driver/main_fw.ace \
          -framework CoreFoundation && /tmp/ace_driver/app_fw)
    got=$?
    if [ "$got" -ne 0 ]; then
        echo "FAIL: -framework link exited $got, want 0"
        fail=1
    fi
    # spaces and shell metacharacters must travel as plain argument bytes
    out=$("$ACE" -o "/tmp/ace_driver/space dir/my app" tests/driver/main_obj.ace \
          "/tmp/ace_driver/space dir/help lib.o" && "/tmp/ace_driver/space dir/my app")
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: paths with spaces exited $got, want 7"
        fail=1
    fi
    out=$("$ACE" -o /tmp/ace_driver/app_evil tests/driver/main_obj.ace \
          '/tmp/ace_driver/evil;$(echo pwn)`id`.o' && /tmp/ace_driver/app_evil)
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: shell metacharacters in names exited $got, want 7"
        fail=1
    fi
    # -o FILE.ace compiles that file in place: executable and object take the stem
    printf 'main(): int {\n    return 5\n}\n' > /tmp/ace_driver/stem_test.ace
    if ! "$ACE" -o /tmp/ace_driver/stem_test.ace >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL: -o FILE.ace in-place compile failed"
        cat /tmp/ace_test_errs
        fail=1
    elif ! [ -x /tmp/ace_driver/stem_test ] || ! [ -f /tmp/ace_driver/stem_test.o ]; then
        echo "FAIL: -o FILE.ace did not produce the stem executable and object"
        fail=1
    else
        /tmp/ace_driver/stem_test
        got=$?
        if [ "$got" -ne 5 ]; then
            echo "FAIL: -o FILE.ace executable exited $got, want 5"
            fail=1
        fi
    fi
    # Bidirectional C ABI oracle: clang owns the foreign half and calls Ace
    # exports; Ace calls the same C object through extern declarations.
    if ! "$ACE" -o /tmp/ace_driver/app_abi tests/driver/abi.ace \
         /tmp/ace_driver/abi_c.o >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL: C ABI oracle did not compile/link"
        cat /tmp/ace_test_errs
        fail=1
    else
        /tmp/ace_driver/app_abi
        got=$?
        if [ "$got" -ne 0 ]; then
            echo "FAIL: bidirectional C ABI oracle exited $got, want 0"
            fail=1
        fi
    fi

    driver_diag() {
        want=$1; shift
        if "$ACE" "$@" >/dev/null 2>/tmp/ace_test_errs; then
            echo "FAIL: expected driver rejection: $*"
            fail=1
        elif ! grep -qF "$want" /tmp/ace_test_errs; then
            echo "FAIL: expected diagnostic '$want' from: $*"
            cat /tmp/ace_test_errs
            fail=1
        fi
    }
    driver_diag "ace: -framework requires a framework name" \
        -o x tests/driver/main_obj.ace -framework
    driver_diag "ace: -l requires a library name" \
        -o x tests/driver/main_obj.ace -l
    driver_diag "ace: -L requires a directory" \
        -o x tests/driver/main_obj.ace -L
    driver_diag "ace: -o requires an output name" \
        tests/driver/main_obj.ace -o
    driver_diag "ace: -framework requires a framework name" \
        -o x tests/driver/main_obj.ace -framework -l helper
    driver_diag "ace: -l requires a library name" \
        -o x tests/driver/main_obj.ace -l -framework CoreFoundation
    driver_diag "ace: -L requires a directory" \
        -o x tests/driver/main_obj.ace -L -l helper
    driver_diag "ace: -o requires an output name" \
        tests/driver/main_obj.ace -o -l helper
    driver_diag "ace: only one root .ace source may be compiled" \
        -o x tests/driver/main_obj.ace tests/driver/main_fw.ace
    driver_diag "ace: only one root .ace source may be compiled" \
        -o tests/driver/main_obj.ace tests/driver/main_fw.ace
    driver_diag "ace: -o <file.ace> requires a name before '.ace'" \
        -o .ace
    driver_diag "ace: additional link inputs cannot be used with -c" \
        -c tests/driver/main_obj.ace /tmp/ace_driver/helper_raw.o
    driver_diag "ace: additional link inputs cannot be used with -c" \
        -c tests/driver/main_obj.ace -l helper
    driver_diag "ace: unknown option '--wat'" \
        -o x --wat tests/driver/main_obj.ace
    driver_diag "ace: unrecognized input 'notes.txt' (expected .ace, .o, or .a)" \
        -o x tests/driver/main_obj.ace notes.txt
    driver_diag "ace: link inputs require -o" \
        tests/driver/main_obj.ace /tmp/ace_driver/helper_raw.o
    # Neither the generated intermediate object nor the linked output may
    # overwrite an input, including aliases of the same existing file.
    cp /tmp/ace_driver/helper_raw.o /tmp/ace_driver/collision.o
    driver_diag "ace: generated object '/tmp/ace_driver/collision.o' would overwrite input '/tmp/ace_driver/collision.o'" \
        tests/driver/main_obj.ace /tmp/ace_driver/collision.o \
        -o /tmp/ace_driver/collision
    if ! cmp -s /tmp/ace_driver/helper_raw.o /tmp/ace_driver/collision.o; then
        echo "FAIL: generated-object collision changed the input"
        fail=1
    fi
    cp /tmp/ace_driver/helper_raw.o /tmp/ace_driver/alias_target.o
    ln -f /tmp/ace_driver/alias_target.o /tmp/ace_driver/alias_input.o
    driver_diag "ace: generated object '/tmp/ace_driver/alias_target.o' would overwrite input '/tmp/ace_driver/alias_input.o'" \
        tests/driver/main_obj.ace /tmp/ace_driver/alias_input.o \
        -o /tmp/ace_driver/alias_target
    if ! cmp -s /tmp/ace_driver/helper_raw.o /tmp/ace_driver/alias_target.o; then
        echo "FAIL: hard-link collision changed the input"
        fail=1
    fi
    cp /tmp/ace_driver/helper_raw.o /tmp/ace_driver/final_collision.o
    driver_diag "ace: linked output '/tmp/ace_driver/final_collision.o' would overwrite input '/tmp/ace_driver/final_collision.o'" \
        tests/driver/main_obj.ace /tmp/ace_driver/final_collision.o \
        -o /tmp/ace_driver/final_collision.o
    if ! cmp -s /tmp/ace_driver/helper_raw.o /tmp/ace_driver/final_collision.o; then
        echo "FAIL: linked-output collision changed the input"
        fail=1
    fi
    # cc's own diagnostics pass through; ace reports the exit status and fails
    if "$ACE" -o /tmp/ace_driver/app_bad tests/driver/main_obj.ace \
       -l ace_missing_lib_zz >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL: missing library should fail to link"
        fail=1
    elif ! grep -qF "ace: linker exited with status" /tmp/ace_test_errs; then
        echo "FAIL: linker failure should report the exit status"
        cat /tmp/ace_test_errs
        fail=1
    fi
fi

# lexer spot-checks
if ! "$ACE" --tokens tests/lexer/hello_ok.ace | grep -q "^3:1${tab}import$"; then
    echo "FAIL: first token of hello_ok.ace should be 'import'"
    fail=1
fi
if ! "$ACE" --tokens tests/lexer/numbers_ok.ace | grep -q "${tab}int${tab}255$"; then
    echo "FAIL: 0xFF should lex as int 255"
    fail=1
fi
if ! "$ACE" --tokens tests/lexer/numbers_ok.ace | grep -q "${tab}int${tab}172$"; then
    echo "FAIL: 0b1010_1100 should lex as int 172"
    fail=1
fi
if ! "$ACE" --tokens tests/lexer/numbers_ok.ace | grep -q "${tab}float${tab}0.0015$"; then
    echo "FAIL: 1.5e-3 should lex as float 0.0015"
    fail=1
fi
if ! "$ACE" --tokens tests/lexer/numbers_ok.ace | grep -q "${tab}char${tab}'A'$"; then
    echo "FAIL: 'A' should lex as char 'A'"
    fail=1
fi

# parser spot-checks on the full-surface fixture
ast() { "$ACE" --ast tests/parser/full_ok.ace; }
if ! ast | grep -q "(enum 'Color'"; then
    echo "FAIL: enum Color should appear in the AST"
    fail=1
fi
if ! ast | grep -q "(func 'scale' pub mut method"; then
    echo "FAIL: visibility-first mut type-owned method should appear in the AST"
    fail=1
fi
if ! ast | grep -q "(conformance"; then
    echo "FAIL: signed-contract conformance should appear in the AST"
    fail=1
fi
if ! ast | grep -q "(signing"; then
    echo "FAIL: canonical signing should appear in the AST"
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
if ! ast | grep -q "(param 'error'"; then
    echo "FAIL: destructuring binder 'error' should appear in the AST"
    fail=1
fi
uninitialized_scalar=$("$ACE" tests/sema/uninitialized/scalar_err.ace 2>&1)
if ! printf '%s\n' "$uninitialized_scalar" | grep -qF \
    "error: 'answer' was not initialized before use"; then
    echo "FAIL: scalar definite-initialization diagnostic"
    fail=1
fi
uninitialized_struct=$("$ACE" tests/sema/uninitialized/struct_err.ace 2>&1)
if ! printf '%s\n' "$uninitialized_struct" | grep -qF \
    "error: 'point' was not fully initialized before use"; then
    echo "FAIL: aggregate definite-initialization diagnostic"
    fail=1
fi
mixed_redecl=$("$ACE" tests/sema/mixed_assignment/typed_exists_err.ace 2>&1)
if ! printf '%s\n' "$mixed_redecl" | grep -qF \
    "error: redeclaration of 'err': a typed name under '=' must be new"; then
    echo "FAIL: mixed '=' typed-existing diagnostic"
    fail=1
fi
mixed_missing=$("$ACE" tests/sema/mixed_assignment/bare_missing_err.ace 2>&1)
if ! printf '%s\n' "$mixed_missing" | grep -qF \
    "error: undeclared name 'err': a bare name under '=' must already exist"; then
    echo "FAIL: mixed '=' bare-missing diagnostic"
    fail=1
fi
# shifts/bitwise parse with C precedence
if ! "$ACE" --ast tests/lexer/numbers_ok.ace | grep -qF "op='<<'"; then
    echo "FAIL: << operator should parse"
    fail=1
fi
if ! "$ACE" --ast tests/lexer/numbers_ok.ace | grep -qF "op='>>'"; then
    echo "FAIL: >> operator should parse"
    fail=1
fi

if [ $fail -eq 0 ]; then
    echo "\n--------------------"
    echo "all tests passed"
fi
exit $fail
