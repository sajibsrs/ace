#!/bin/sh
# Fixtures: *_ok.ace must compile through the landed stages with zero errors;
# *_err.ace must fail. Fixtures live in feature-grouped directories — one
# directory per feature with a main fixture plus aspect-named companions.
# tests/lexer and tests/parser run parse-only (--ast); tests/sema runs the
# full check (default mode). A sema or IR fixture's `// target: NAME` line
# checks it for that target instead of the host's (its asm names registers).
set -u
fail=0
tab=$(printf '\t')
ACE_BIN=${ACE_BIN:-./ace}
ACE_DIR=$(dirname "$ACE_BIN")
ACE=$(cd "$ACE_DIR" && pwd)/$(basename "$ACE_BIN")
rm -rf /tmp/ace_sections
mkdir -p /tmp/ace_sections
JOBS=${ACE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
target_of() { sed -n 's/^\/\/ target: *\(.*\)/--target=\1/p' "$1"; }
# libdwarf installs a `dwarfdump` too, with different flags and output.
DWARFDUMP=
for tool in llvm-dwarfdump dwarfdump; do
    if "$tool" --version 2>/dev/null | grep -q LLVM; then
        DWARFDUMP=$tool
        break
    fi
done

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

struct_literal_colon_error=$($ACE tests/parser/struct_literal_colon_err.ace 2>&1)
if ! printf '%s\n' "$struct_literal_colon_error" | grep -qF \
    "struct literal field initialization uses '='; ':' is for declarations and type annotations"; then
    echo "FAIL: former struct literal colon spelling should receive the migration diagnostic"
    fail=1
fi

mut_array_type=$("$ACE" tests/parser/mut_array_type_err.ace 2>&1)
if ! printf '%s\n' "$mut_array_type" | grep -qF \
    "a fixed array is already writable storage; take a writable view with 'mut name[..]'"; then
    echo "FAIL: 'mut' on a fixed array type should point to the writable view spelling"
    fail=1
fi

bitwise_compare=$("$ACE" tests/parser/bitwise_compare/equality_err.ace 2>&1)
if ! printf '%s\n' "$bitwise_compare" | grep -qF \
    "equality_err.ace:4:14: error: a bitwise operator and a comparison need parentheses to mix"; then
    echo "FAIL: an equality as a bitwise operand should require parentheses"
    fail=1
fi

unknown_decl=$("$ACE" tests/parser/unknown_decl_type_err.ace 2>&1)
if ! printf '%s\n' "$unknown_decl" | grep -qF \
    "tests/parser/unknown_decl_type_err.ace:3:8: error: unknown declaration type 'protocol'"; then
    echo "FAIL: unknown declaration type should point at the type name"
    fail=1
fi

# syntax errors: one per statement or declaration, up to 10 per file
several=$("$ACE" tests/parser/recovery/several_err.ace 2>&1)
if [ "$several" != "tests/parser/recovery/several_err.ace:5:14: error: expected expression, got ')'
tests/parser/recovery/several_err.ace:10:8: error: expected parameter name, got ':'
tests/parser/recovery/several_err.ace:16:12: error: expected type, got '='
tests/parser/recovery/several_err.ace: 3 errors" ]; then
    echo "FAIL: each syntax error should be reported once, and checking blocked"
    printf '%s\n' "$several"
    fail=1
fi
limit=$("$ACE" tests/parser/recovery/limit_err.ace 2>&1)
if [ "$(printf '%s\n' "$limit" | grep -c ': error: ')" -ne 10 ] ||
    [ "$(printf '%s\n' "$limit" | tail -2)" != "tests/parser/recovery/limit_err.ace: stopped at 10 errors; the rest of the file was not parsed
tests/parser/recovery/limit_err.ace: 10 errors" ]; then
    echo "FAIL: syntax errors should stop at 10 per file"
    printf '%s\n' "$limit"
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
if ! "$ACE" tests/lexer/ident_long_err.ace 2>&1 | grep -qF \
    "3:1: error: identifier exceeds 128 characters"; then
    echo "FAIL: identifier length diagnostic"
    fail=1
fi

for f in $(find tests/sema -name '*_ok.ace' | sort); do
    if ! "$ACE" $(target_of "$f") "$f" >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL (should check clean): $f"
        cat /tmp/ace_test_errs
        fail=1
    fi
done

for f in $(find tests/sema -name '*_err.ace' | sort); do
    if "$ACE" $(target_of "$f") "$f" >/dev/null 2>&1; then
        echo "FAIL (should report errors): $f"
        fail=1
    fi
done

# inline declarations: each placement and declaration rule reports its own
# diagnostic (spec Functions & ABI #8)
while IFS='|' read -r f want; do
    if ! "$ACE" "$f" 2>&1 | grep -qF "$want"; then
        echo "FAIL: $f should report: $want"
        fail=1
    fi
done <<'INLINE_RULES'
tests/parser/inline/visibility_order_err.ace|visibility must come before 'inline'
tests/parser/long_name_err.ace|4:14: error: expected newline or '}' after statement, got 'nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn'
tests/parser/inline/duplicate_err.ace|duplicate 'inline' modifier
tests/parser/inline/not_function_err.ace|1:8: error: 'inline' applies only to function declarations
tests/parser/inline/extern_err.ace|'extern' blocks take no pub/inline
tests/parser/extern_def/link_name_err.ace|extern link name cannot be empty
tests/parser/extern_def/global_init_err.ace|expected '=' and an initializer
tests/parser/extern_def/variadic_err.ace|variadic functions cannot be defined in Ace
tests/parser/extern_def/bodiless_err.ace|expected '{'
tests/parser/associated/export_err.ace|methods and associated functions cannot use the C ABI
tests/parser/receiver/self_not_first_err.ace|2:13: error: 'self' must be the first parameter
tests/parser/receiver/self_typed_err.ace|2:9: error: 'self' takes no type; its type is the method's owner
tests/parser/receiver/self_free_err.ace|1:3: error: 'self' is only the receiver of a method declaration
tests/parser/receiver/leading_mut_err.ace|2:1: error: a top-level declaration takes no leading 'mut'; a mutating receiver is written 'mut self'
tests/parser/receiver/contract_double_mut_err.ace|2:5: error: a contract method takes no leading 'mut'; a mutating receiver is written 'mut self'
tests/parser/associated/coloncolon_err.ace|6:13: error: '::' is not an operator; a type's functions are reached with '.'
tests/parser/receiver/contract_no_self_err.ace|4:5: error: a contract method's first parameter is its receiver 'self' or 'mut self'
tests/sema/mutability/call_array_err.ace|7:7: error: not an assignable target
tests/sema/mutability/call_view_plain_err.ace|7:10: error: cannot write through the read-only view []P
tests/sema/discard/typed_err.ace|6:5: error: '_' in a binding list takes no type; write it bare, as in '_, found: bool = f()'
tests/sema/discard/mixed_typed_err.ace|7:5: error: '_' in a binding list takes no type; write it bare, as in '_, found: bool = f()'
tests/sema/arrays/arithmetic_err.ace|6:19: error: operator * does not apply to [2]i64
tests/sema/shadowing/module_member_err.ace|5:12: note: local 'mem' hides the imported module 'mem'
tests/sema/mutability/receiver_call_err.ace|8:11: error: method 'wipe' takes 'mut self'; it needs 'mut' at the call: mut x.wipe(...)
tests/sema/mutability/assoc_arg_err.ace|10:19: error: 'mut' marks mut parameters; parameter 'storage' of 'init' is not one
tests/sema/associated/value_receiver_err.ace|9:22: error: 'new' is an associated function, called on the type: Router.new(...)
tests/sema/inline/noalias_err.ace|inline function 'sum' cannot take a noalias parameter
tests/sema/inline/defer_err.ace|4:5: error: inline function 'guarded' cannot contain 'defer'
tests/sema/inline/asm_err.ace|inline function 'spin' cannot contain inline assembly
tests/sema/inline/specialized_err.ace|'inline' needs a body; 'weight' is marked 'specialized'
tests/sema/inline/intrinsic_err.ace|'inline' cannot mark intrinsic declaration 'cast'
tests/sema/inline/main_err.ace|the entry function cannot be 'inline'
tests/sema/inline/recursion_err.ace|inline function 'count' reaches itself through inline calls
tests/sema/inline/mutual_err.ace|inline function 'even' reaches itself through inline calls
tests/sema/names/module_type_name_err.ace|6:32: error: type mismatch: net.SocketType given where i64 expected
INLINE_RULES

# local escape (spec Memory #29): each direct return or outer store of a view
# of this function's storage is reported at the view
f=tests/sema/memory/escape_err.ace
want=""
for at in 12:14 16:12 19:13 22:15 26:12 30:20 34:14 38:21 42:18; do
    want="$want$f:$at: error: returned value views storage of this function, which ends when it returns
"
done
for at in 46:14 47:12 50:12; do
    want="$want$f:$at: error: stored value views storage of this function, which ends when it returns
"
done
if [ "$("$ACE" "$f" 2>&1 | grep ': error: ')" != "${want%?}" ]; then
    echo "FAIL: $f should report each local escape"
    fail=1
fi

# function types: 'void' marks no return value and binds type parameters (spec Syntax & Parsing #5)
while IFS='|' read -r f want; do
    if ! "$ACE" "$f" 2>&1 | grep -qF "$want"; then
        echo "FAIL: $f should report: $want"
        fail=1
    fi
done <<'FUNCTION_TYPE_RULES'
tests/parser/function_type/void_declaration_err.ace|3:8: error: a function with no return value omits its return clause
tests/parser/function_type/void_value_err.ace|4:9: error: 'void' is allowed only as a function type's return
tests/parser/function_type/single_type_err.ace|'(T)' is not a type
tests/parser/function_type/empty_type_err.ace|'()' is not a type
tests/parser/function_type/c_return_err.ace|expected ':' and a return type or 'void'
tests/sema/fields/method_collision_err.ace|5:8: error: method 'on_click' collides with a field of Button
tests/sema/fields/promoted_collision_err.ace|11:5: error: field 'area' collides with a method promoted from embedded 'base'
tests/modules/selective_type_value/main.ace|3:8: error: imported name 'Thing' collides with a declaration
tests/modules/selective_value_type/main.ace|3:8: error: imported name 'make' collides with a declaration
tests/modules/selective_local_type/main.ace|6:5: error: 'Thing' names a type; a value may not share its name
tests/modules/error_display/main.ace|util.ace:6:17: error: 'util.count' is not callable
tests/modules/overload_mismatch/main.ace|6:21: error: no matching overload 'util.pick' for these argument types
tests/modules/overload_private/main.ace|6:22: error: the overload of 'util.scale' matching these arguments is private
tests/modules/field_private/main.ace|10:46: error: field 'tag' of 'geo.Point' is private
tests/modules/field_private/main.ace|11:7: error: field 'tag' of 'geo.Point' is private
tests/modules/field_private/main.ace|12:35: error: field 'secret' of 'geo.Inner' is private
tests/modules/field_private/main.ace|14:34: error: field 'n' of 'geo.Hidden' is private
tests/modules/field_private/main.ace|14:46: error: field 'secret' of 'geo.Inner' is private
tests/sema/associated/nongeneric_owner_args_err.ace|8:23: error: 'P' is not generic and takes no type arguments
tests/sema/associated/type_param_nonstruct_err.ace|4:18: error: 'T' is i64, which has no associated functions
tests/sema/names/function_type_err.ace|3:1: error: 'N' names a type; a value may not share its name
tests/sema/names/global_type_err.ace|3:1: error: 'Point' names a type; a value may not share its name
tests/sema/names/local_type_err.ace|4:5: error: 'Point' names a type; a value may not share its name
tests/sema/names/param_type_err.ace|3:3: error: 'Point' names a type; a value may not share its name
tests/sema/names/local_primitive_err.ace|2:5: error: 'u8' names a type; a value may not share its name
tests/sema/names/import_type_err.ace|3:1: error: 'mem' names a value; a type may not share its name
tests/sema/fields/qualified_method_err.ace|11:27: error: methods are called on a value: x.get(...), not Counter.get(...)
tests/sema/fields/not_callable_err.ace|7:23: error: field 'value' of type i64 is not callable
tests/sema/fields/method_value_err.ace|11:23: error: 'get' is not a field of Counter; methods are not values
tests/sema/fields/arg_count_err.ace|7:26: error: wrong number of arguments: 2 given, 1 expected
tests/sema/functions/void_value_mismatch_err.ace|(i64): void given where (i64): i64 expected
tests/sema/functions/void_type_argument_err.ace|3:16: error: 'void' (as T) is allowed only as a function type's return or a type argument
tests/sema/generics/void_field_err.ace|3:25: error: 'void' (as T) is allowed only
tests/sema/generics/void_local_err.ace|4:13: error: 'void' (as R) is allowed only
tests/sema/generics/void_local_err.ace|12:5: note: in 'keep[void]' instantiated here
tests/sema/generics/void_field_err.ace|6:11: note: in 'Box[void]' instantiated here
tests/sema/generics/instantiation_site_err.ace|9:12: note: in 'inner[string]' instantiated here
tests/sema/generics/instantiation_site_err.ace|26:17: note: in 'outer[string]' instantiated here
tests/sema/generics/instantiation_site_err.ace|28:19: note: in 'Box[string].get' instantiated here
tests/sema/generics/instantiation_site_err.ace|29:29: note: in 'Box[string].make' instantiated here
tests/sema/generics/type_arity_err.ace|4:1: error: generic type expects 1 type arguments, got 2
tests/sema/generics/alias_recursive_err.ace|3:1: error: generic alias 'Loop' has a recursive expansion
tests/sema/generics/alias_recursive_err.ace|5:21: note: in 'Loop[i64]' instantiated here
tests/sema/generics/field_stamp_note_err.ace|6:27: note: in 'Inner[[4]u8]' instantiated here
tests/sema/generics/field_stamp_note_err.ace|8:25: note: in 'Outer[[4]u8]' instantiated here
tests/sema/c_abi/function_pointer_err.ace|7:33: error: C function-pointer parameter type [2]u8 is not C-ABI compatible
tests/sema/c_abi/function_pointer_err.ace|7:40: error: C function-pointer return type string is not C-ABI compatible
tests/sema/c_abi/function_pointer_err.ace|8:29: error: C function-pointer parameter type *Fat is not C-ABI compatible
tests/sema/defaults/operators_err.ace|7:23: error: '&&' needs bool operands, got bool and integer literal
tests/sema/defaults/operators_err.ace|8:20: error: '%' needs integer operands of one type, got integer literal and float literal
tests/sema/defaults/operators_err.ace|9:19: error: cannot negate unsigned u8
tests/sema/defaults/operators_err.ace|10:21: error: invalid operand for '~': bool
tests/sema/defaults/operators_err.ace|11:26: error: comparison chain needs one numeric type, got string and string
tests/sema/defaults/operators_err.ace|12:23: error: comparison chain needs one numeric type, got u8 and float literal
tests/sema/defaults/operators_err.ace|13:26: error: type mismatch: i64 + u8 — operands need one type (casts are explicit)
tests/sema/defaults/operators_err.ace|14:26: error: operator + does not apply to bool
tests/sema/defaults/operators_err.ace|15:19: error: float literal out of range for f32
tests/sema/defaults/operators_err.ace|16:16: error: integer literal 300 out of range for u8
tests/sema/defaults/operators_err.ace|17:20: error: 5 is not a member of Flag
tests/sema/defaults/operators_err.ace|18:19: error: type mismatch: integer literal given where *u8 expected (casts are explicit)
tests/sema/defaults/literals_err.ace|8:31: error: type Point has no field 'z'
tests/sema/defaults/literals_err.ace|8:22: error: missing field 'y' in Point literal
tests/sema/defaults/literals_err.ace|9:29: error: duplicate field 'x' in Point literal
tests/sema/defaults/literals_err.ace|10:29: error: cannot mix named and positional fields
tests/sema/defaults/literals_err.ace|11:28: error: too many fields: Point has 2
tests/sema/defaults/literals_err.ace|12:20: error: missing fields: Point has 2, literal has 1
tests/sema/defaults/literals_err.ace|13:21: error: missing field 'b' in Pair literal
tests/sema/defaults/literals_err.ace|14:17: error: bare struct literal needs its type from context
tests/sema/defaults/literals_err.ace|15:22: error: i64 does not take a literal
tests/sema/defaults/literals_err.ace|16:21: error: array length is 2, literal has 3 elements
tests/sema/defaults/literals_err.ace|17:23: error: array repetition count is 3, expected 2 for [2]i64
tests/sema/defaults/literals_err.ace|18:26: error: [Color]i64 takes a keyed literal ([Color.member = value, ...]), not a positional one
tests/sema/defaults/literals_err.ace|19:22: error: keys apply only to an enum-indexed array, not [2]i64
tests/sema/defaults/literals_err.ace|20:53: error: member Color.red is keyed twice
tests/sema/defaults/literals_err.ace|21:28: error: keyed literal of [Color]i64 is missing member Color.green
tests/sema/defaults/literals_err.ace|22:30: error: a key of [Color]i64 is a Color member, not integer literal
tests/sema/defaults/literals_err.ace|22:37: error: a key of [Color]i64 is a Color member, not integer literal
tests/sema/defaults/literals_err.ace|23:25: error: tuple has 3 values where 2 expected
tests/sema/defaults/casts_err.ace|11:20: error: cannot cast string to i32
tests/sema/defaults/casts_err.ace|12:19: error: a cast takes exactly one argument
tests/sema/defaults/casts_err.ace|13:25: error: unions have no constructor call — construct a variant: Shape.<variant>(...)
tests/sema/defaults/casts_err.ace|14:22: error: 7 is not a member of Flag
tests/sema/defaults/casts_err.ace|15:26: error: unknown type 'nothing'
tests/sema/defaults/casts_err.ace|16:28: error: error-domain names have no value layout; use Error
tests/sema/defaults/casts_err.ace|17:25: error: error-domain names have no value layout; use Error
tests/sema/defaults/casts_err.ace|18:37: error: offsetof field paths traverse inline structs only
tests/sema/defaults/casts_err.ace|19:36: error: type Outer has no field 'missing'
tests/sema/defaults/casts_err.ace|20:25: error: enum Flag has no member 'middle'
tests/sema/defaults/casts_err.ace|21:26: error: error domain Fault has no entry 'worse'
tests/sema/defaults/casts_err.ace|22:28: error: type Error has no field 'size'
tests/sema/defaults/casts_err.ace|23:21: error: 'panic' is a builtin call, not a value
tests/sema/defaults/casts_err.ace|24:20: error: '_' discards a value and cannot be read
tests/sema/defaults/casts_err.ace|25:26: error: constant does not fit Mode.low unsigned 4-bit field
tests/sema/defaults/casts_err.ace|26:25: error: bits literals require named fields
tests/sema/defaults/casts_err.ace|27:23: error: bits type Mode has no accessible field 'wide'
tests/sema/defaults/views_err.ace|13:24: error: null is not a value of i64
tests/sema/defaults/views_err.ace|14:28: error: array literal cannot establish a writable view: mut []i64 needs storage that is written
tests/sema/defaults/views_err.ace|15:21: error: keyed array literal needs an [E]T context, not []i64
tests/sema/defaults/views_err.ace|16:21: error: bare struct literal needs its type from context
tests/sema/defaults/views_err.ace|17:20: error: array literal needs its array type from context
tests/sema/defaults/views_err.ace|18:22: error: overlapping struct Word has no literal; declare it and assign a member
tests/sema/defaults/views_err.ace|19:45: error: cannot compare Point and Point
tests/sema/defaults/calls/main_err.ace|48:21: error: wrong number of arguments: 2 given, 1 expected
tests/sema/defaults/calls/main_err.ace|49:20: error: no matching overload 'two' for these argument types
tests/sema/defaults/calls/main_err.ace|50:21: error: ambiguous call to overloaded function 'amb'
tests/sema/defaults/calls/main_err.ace|51:23: error: generic function 'gen' needs type arguments: gen[T](...)
tests/sema/defaults/calls/main_err.ace|52:23: error: overloaded function 'two' needs an explicit function-value type: 'two' is overloaded
tests/sema/defaults/calls/main_err.ace|53:23: error: 'mut' marks mut parameters; parameter 1 is read-only
tests/sema/defaults/calls/main_err.ace|54:25: error: mut parameter needs 'mut' at the call site
tests/sema/defaults/calls/main_err.ace|55:22: error: type mismatch: u8 given where i64 expected (casts are explicit)
tests/sema/defaults/calls/main_err.ace|57:29: error: the overload of 'other.pick' matching these arguments is private
tests/sema/defaults/calls/main_err.ace|58:23: error: 'VALUE' is not callable
tests/sema/generics/void_memory_err.ace|'void' names no value and cannot be a memory element type
tests/sema/functions/return_no_value_err.ace|7:16: error: type mismatch: no value given where i64 expected
tests/sema/types/predeclared_name_err.ace|3:1: error: 'bool' is a predeclared type name and cannot be redeclared
tests/sema/types/predeclared_name_err.ace|5:1: error: 'void' is a predeclared type name and cannot be redeclared
FUNCTION_TYPE_RULES

# @cold declarations (spec Functions & ABI #10)
while IFS='|' read -r f want; do
    if ! "$ACE" "$f" 2>&1 | grep -qF "$want"; then
        echo "FAIL: $f should report: $want"
        fail=1
    fi
done <<'COLD_RULES'
tests/parser/cold/duplicate_err.ace|a declaration accepts at most one @cold attribute
tests/parser/cold/same_line_err.ace|@cold must occupy its own line before a declaration
tests/parser/cold/not_function_err.ace|2:1: error: @cold applies only to Ace function declarations
tests/parser/cold/extern_err.ace|@cold applies only to Ace function declarations
tests/sema/cold/inline_err.ace|inline function 'rare' cannot be @cold
tests/sema/cold/specialized_err.ace|@cold needs a body; 'weight' has none
tests/sema/cold/main_err.ace|the entry function cannot be @cold
COLD_RULES

# generic function values name their type arguments (spec Generics #6), a
# parameter's type selects an overloaded function value (Names & Scoping #8),
# and diagnostics render composite types in full
while IFS='|' read -r f want; do
    if ! "$ACE" "$f" 2>&1 | grep -qF "$want"; then
        echo "FAIL: $f should report: $want"
        fail=1
    fi
done <<'GENERIC_VALUE_RULES'
tests/sema/generics/value_bare_err.ace|'larger' is a type or generic — used bare here
tests/sema/generics/value_arity_err.ace|no generic 'larger' takes 2 type arguments
tests/sema/generics/value_memory_err.ace|mem.alloc[T] is direct-call-only: it has no body to take as a value
tests/sema/generics/value_specialized_err.ace|'load[f64]' matches no specialization
tests/sema/generics/value_overload_mismatch_err.ace|no generic overload of 'combine' has the required function type
tests/sema/generics/value_overload_ambiguous_err.ace|ambiguous generic function value 'combine'
tests/sema/generics/not_generic_value_err.ace|'plain' is not generic; name it without type arguments
tests/sema/generics/not_generic_call_err.ace|'plain' is not generic; call it without type arguments
tests/sema/types/composite_name_err.ace|(i32, i32): i32 given where (i64): i64 expected
tests/sema/types/composite_name_err.ace|(i32, bool) given where (i64, bool) expected
tests/sema/types/composite_name_err.ace|*Point given where *i32 expected
tests/sema/overloads/value_param_callee_err.ace|overloaded function 'twice' needs an explicit function-value type: 'run' is overloaded
tests/sema/overloads/value_param_mismatch_err.ace|no overload of 'twice' has the required function type
GENERIC_VALUE_RULES

# tuple literal members meet their declared member types (spec Syntax #11)
tuple_member_errors=$("$ACE" tests/sema/types/tuple_literal_member_type_err.ace 2>&1)
for want in \
    "6:21: error: type mismatch: i64 given where i32 expected" \
    "7:20: error: integer literal 300 out of range for u8" \
    "8:21: error: tuple has 3 values where 2 expected"; do
    if ! printf '%s\n' "$tuple_member_errors" | grep -qF "$want"; then
        echo "FAIL: missing tuple member diagnostic '$want'"
        fail=1
    fi
done

# an array literal selects an overload by its candidate element type (spec Names #6)
array_overload_errors=$("$ACE" tests/sema/overloads/array_literal_err.ace 2>&1)
for want in \
    "10:18: error: no matching overload 'take'" \
    "11:18: error: no matching overload 'both'" \
    "12:18: error: no matching overload 'both'" \
    "13:25: error: integer literal 300 out of range for u8" \
    "15:18: error: no matching overload 'take'"; do
    if ! printf '%s\n' "$array_overload_errors" | grep -qF "$want"; then
        echo "FAIL: missing array-literal overload diagnostic '$want'"
        fail=1
    fi
done

nested_overload_errors=$("$ACE" tests/sema/overloads/nested_array_literal_err.ace 2>&1)
for want in \
    "13:15: error: no matching overload 'f'" \
    "14:15: error: no matching overload 'f'" \
    "15:15: error: no matching overload 'g'" \
    "16:18: error: ambiguous call to overloaded function 'both'"; do
    if ! printf '%s\n' "$nested_overload_errors" | grep -qF "$want"; then
        echo "FAIL: missing nested array-literal overload diagnostic '$want'"
        fail=1
    fi
done

# A call that settles on its sole overload, declared before or after it, is
# checked where its value lands.
for f in sole_overload_result_decl sole_overload_result_return forward_result_decl; do
    if ! "$ACE" "tests/sema/functions/${f}_err.ace" 2>&1 |
         grep -qF "type mismatch: u64 given where"; then
        echo "FAIL: a sole overload's result must meet its consumer's type: $f"
        fail=1
    fi
done

# A local is in scope from its declarator, in the root file and in an
# imported module alike.
while IFS='|' read -r f want; do
    if ! "$ACE" "$f" 2>&1 | grep -qF "$want"; then
        echo "FAIL: $f should report: $want"
        fail=1
    fi
done <<'OWN_INITIALIZER'
tests/sema/shadowing/own_initializer_err.ace|10:21: error: 'get' is not callable
tests/sema/shadowing/own_initializer_module_err.ace|own_initializer.ace:4:20: error: 'total' was not initialized before use
OWN_INITIALIZER
if ! "$ACE" tests/sema/types/forward_call_default_err.ace 2>&1 |
     grep -qF "default for field 'v' must be a compile-time expression"; then
    echo "FAIL: a field default must not call a function declared later"
    fail=1
fi

untyped_float_mix=$("$ACE" tests/sema/operators/untyped_float_mix_err.ace 2>&1)
if ! printf '%s\n' "$untyped_float_mix" | grep -qF \
    "type mismatch: integer literal given where f64 expected (casts are explicit)"; then
    echo "FAIL: a compound untyped integer beside f64 must need an explicit cast"
    fail=1
fi

float_remainder=$("$ACE" tests/sema/operators/float_remainder_err.ace 2>&1)
for want in \
    "4:39: error: '%' needs integer operands of one type, got f64 and f64" \
    "6:40: error: '%' needs integer operands of one type, got f32 and f32" \
    "8:29: error: '%' needs integer operands of one type, got float literal and float literal"; do
    if ! printf '%s\n' "$float_remainder" | grep -qF "$want"; then
        echo "FAIL: float remainder should report: $want"
        fail=1
    fi
done

# a binary expression is positioned at its operator
binary_position=$("$ACE" tests/sema/operators/binary_position_err.ace 2>&1)
for want in \
    "5:26: error: type mismatch: i32 given where i64 expected" \
    "10:16: error: type mismatch: i32 + i64"; do
    if ! printf '%s\n' "$binary_position" | grep -qF "$want"; then
        echo "FAIL: binary expression should report: $want"
        fail=1
    fi
done

# a mut argument, an expression statement, and a range are positioned at
# their own first token or operator
node_position=$("$ACE" tests/sema/operators/node_position_err.ace 2>&1)
for want in \
    "9:18: error: 'mut' marks mut parameters; mem.equal parameter 1 is read-only" \
    "10:5: error: untyped literal expression has no effect" \
    "14:22: error: range bounds must be one integer type, got i32 and i64"; do
    if ! printf '%s\n' "$node_position" | grep -qF "$want"; then
        echo "FAIL: node position should report: $want"
        fail=1
    fi
done

while IFS='|' read -r f want; do
    if ! "$ACE" --ast "$f" 2>&1 | grep -qF "$want"; then
        echo "FAIL: $f should report: $want"
        fail=1
    fi
done <<'RECEIVER_MUT'
tests/parser/mut_statement_err.ace|6:5: error: 'mut' at statement level marks a call receiver
tests/parser/defer_mut_err.ace|6:11: error: 'mut' after 'defer' marks a call receiver
RECEIVER_MUT

compound_operand=$("$ACE" tests/sema/operators/compound_operand_err.ace 2>&1)
for want in \
    "7:11: error: '%=' needs an integer target, got f64" \
    "8:11: error: '&=' needs an integer target, got f64" \
    "9:11: error: '<<=' needs an integer target, got f64" \
    "11:10: error: '+=' needs a numeric target, got bool" \
    "13:11: error: '*=' needs a numeric target, got Point" \
    "15:10: error: '+=' needs a numeric target, got string"; do
    if ! printf '%s\n' "$compound_operand" | grep -qF "$want"; then
        echo "FAIL: compound assignment should report: $want"
        fail=1
    fi
done

float_alias_overload=$("$ACE" tests/sema/types/float_alias_overload_err.ace 2>&1)
if ! printf '%s\n' "$float_alias_overload" | grep -qF \
    "duplicate overload 'measure': parameter signatures must differ"; then
    echo "FAIL: float and f64 should have one overload identity"
    fail=1
fi

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

default_last_errors=$("$ACE" tests/sema/conditions/default_not_last_err.ace 2>&1)
default_last_count=$(printf '%s\n' "$default_last_errors" | grep -cF "'default' must be the last case")
if [ "$default_last_count" -ne 3 ]; then
    echo "FAIL: expected 3 'default' must be the last case diagnostics, got $default_last_count"
    fail=1
fi
switch_else_error=$("$ACE" tests/sema/conditions/switch_else_err.ace 2>&1)
if ! printf '%s\n' "$switch_else_error" | grep -qF "'else' is not a switch case; use 'default'"; then
    echo "FAIL: missing switch 'else' diagnostic"
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
enum_explicit_errors=$("$ACE" tests/sema/types/enum_explicit_err.ace 2>&1)
for want in \
    "enum backing type must be i8, i16, i32, i64, u8, u16, u32, or u64" \
    "first enum member must have discriminant zero" \
    "duplicate enum discriminant 4" \
    "integer literal 256 out of range for u8" \
    "negative enum discriminant does not fit u16" \
    "implicit enum discriminant overflows u8" \
    "integer literal 2147483648 out of range for i32" \
    "enum discriminant must be a compile-time integer expression"; do
    if ! printf '%s\n' "$enum_explicit_errors" | grep -qF "$want"; then
        echo "FAIL: missing explicit enum diagnostic '$want'"
        fail=1
    fi
done
structured_error_errors=$("$ACE" tests/sema/errors/structured_err.ace 2>&1)
for want in \
    "error code 2147483648 is outside i32" \
    "error code -2147483649 is outside i32" \
    "duplicate error entry name 'duplicate_name'" \
    "duplicate error code 3" \
    "error message must be a string literal" \
    "type Error has no field 'address'"; do
    if ! printf '%s\n' "$structured_error_errors" | grep -qF "$want"; then
        echo "FAIL: missing structured error diagnostic '$want'"
        fail=1
    fi
done
cycle_errors=$("$ACE" tests/sema/types/declaration_cycle_err.ace 2>&1)
for want in \
    "error: 'A' depends on itself: 'A' -> 'B' -> 'C' -> 'A'" \
    "error: 'P' depends on itself: 'P' -> 'Q' -> 'P'" \
    "error: type 'S' depends on its own layout" \
    "error: type 'M' has a recursive value layout" \
    "error: 'f' depends on itself: 'f' -> 'Z' -> 'f'" \
    "error: 'D' depends on itself: 'D' -> 'E' -> 'D'"; do
    if ! printf '%s\n' "$cycle_errors" | grep -qF "$want"; then
        echo "FAIL: missing declaration-cycle diagnostic: $want"
        fail=1
    fi
done
if [ "$(printf '%s\n' "$cycle_errors" | grep -c 'error:')" -ne 6 ]; then
    echo "FAIL: each declaration cycle should be reported exactly once"
    printf '%s\n' "$cycle_errors"
    fail=1
fi

c_abi_errors=$("$ACE" tests/sema/c_abi/rejected_err.ace 2>&1)
for want in \
    "parameter 'value' has type string, which is not C-ABI compatible" \
    "return type (i32, i32) is not C-ABI compatible" \
    "return type [4]i32 is not C-ABI compatible" \
    "parameter 'value' has type *string, which is not C-ABI compatible" \
    "parameter 'value' has type BadField, which is not C-ABI compatible" \
    "parameter 'value' has type Box, which is not C-ABI compatible" \
    "parameter 'value' is 'mut' — mut parameters stay internal; pass a mut *T instead" \
    "function 'bad_generic' cannot be generic"; do
    if ! printf '%s\n' "$c_abi_errors" | grep -qF "$want"; then
        echo "FAIL: missing C ABI diagnostic '$want'"
        fail=1
    fi
done

exported_global_errors=$("$ACE" tests/sema/c_abi/exported_globals_err.ace 2>&1)
for want in \
    "5:11: error: C global 'text' has type string, which is not C-ABI compatible" \
    "8:21: error: C global 'bytes' has type []u8, which is not C-ABI compatible" \
    "11:5: error: C global 'limit' is read-only" \
    "12:30: error: C global 'limit' is read-only"; do
    if ! printf '%s\n' "$exported_global_errors" | grep -qF "$want"; then
        echo "FAIL: missing exported C global diagnostic '$want'"
        fail=1
    fi
done
c_global_errors=$("$ACE" tests/sema/c_abi/globals_err.ace 2>&1)
for want in \
    "13:5: error: C global 'limit' is read-only — mark its declaration 'mut'" \
    "14:30: error: C global 'limit' is read-only" \
    "15:27: error: C global 'names' is read-only" \
    "16:5: error: C global 'names' is read-only" \
    "C global 'bad_text' has type string, which is not C-ABI compatible" \
    "C global 'bad_bytes' has type []u8, which is not C-ABI compatible"; do
    if ! printf '%s\n' "$c_global_errors" | grep -qF "$want"; then
        echo "FAIL: missing C global diagnostic '$want'"
        fail=1
    fi
done
for case in export mut_function; do
    case $case in
        export)       want="extern link name cannot be empty" ;;
        mut_function) want="'mut' marks an extern variable: expected ':' after its name" ;;
    esac
    if ! "$ACE" --ast "tests/parser/extern_var/${case}_err.ace" 2>&1 | grep -qF "$want"; then
        echo "FAIL: extern variable $case diagnostic"
        fail=1
    fi
done

overlap_errors=$("$ACE" tests/sema/overlap/main_err.ace 2>&1)
if [ "$(printf '%s\n' "$overlap_errors" | grep -c 'error:')" -ne 7 ]; then
    echo "FAIL: overlapping struct diagnostics"
    fail=1
fi

bitcast_errors=$("$ACE" tests/sema/bitcast/main_err.ace 2>&1)
if [ "$(printf '%s\n' "$bitcast_errors" | grep -c 'error:')" -ne 7 ]; then
    echo "FAIL: bitcast diagnostics"
    fail=1
fi

reinterpret_errors=$("$ACE" tests/sema/pointers/reinterpret_err.ace 2>&1)
if [ "$(printf '%s\n' "$reinterpret_errors" | grep -c 'pointer reinterpretation')" -ne 9 ]; then
    echo "FAIL: pointer reinterpretation diagnostics"
    fail=1
fi

printf_format_errors=$("$ACE" tests/sema/printf/formats_err.ace 2>&1)
for want in \
    "format must be a string literal or static-literal const global" \
    "incomplete fmt.printf conversion" \
    "unsupported fmt.printf conversion '%q'" \
    "flags other than '0', '*', and positional arguments are not supported" \
    "zero flag needs a width" \
    "width 1000001 exceeds the limit 1000000" \
    "zero padding is supported only for numeric conversions" \
    "precision needs decimal digits" \
    "dynamic '*' precision is not supported" \
    "precision is supported only for %f, %e, and %g" \
    "precision 325 exceeds the fixed-buffer limit 324" \
    "15:25: error: fmt.printf precision 325 exceeds the fixed-buffer limit 324" \
    "22:25: error: fmt.printf precision 325 exceeds" \
    "23:25: error: fmt.printf precision 325 exceeds" \
    "unsupported fmt.printf conversion '%E'" \
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
    "19:31: error: fmt.printf argument 1 for conversion at decoded byte 0 requires f32 or f64, got integer literal" \
    "20:31: error: fmt.printf argument 1 for conversion at decoded byte 0 requires f32 or f64, got i32" \
    "requires string, got u8" \
    "requires u8, got string" \
    "fmt.printf is direct-call-only"; do
    if ! printf '%s\n' "$printf_type_errors" | grep -qF "$want"; then
        echo "FAIL: missing printf type diagnostic '$want'"
        fail=1
    fi
done

str_format_errors=$("$ACE" tests/sema/printf/str_format_err.ace 2>&1)
for want in \
    "str.format argument 1 for conversion at decoded byte 0 requires int" \
    "str.format format requires 2 value arguments, got 1" \
    "extra str.format argument 2 is not consumed by the format" \
    "str.format format must be a string literal or static-literal const global" \
    "unsupported str.format conversion '%q'" \
    "[]u8 given where mut []u8 expected"; do
    if ! printf '%s\n' "$str_format_errors" | grep -qF "$want"; then
        echo "FAIL: missing str.format diagnostic '$want'"
        fail=1
    fi
done

printf_fd_errors=$("$ACE" tests/sema/printf/fd_err.ace 2>&1)
for want in \
    "11:45: error: fmt.printf argument 1 for conversion at decoded byte 0 requires int" \
    "12:19: error: fmt.printf format requires 2 value arguments, got 1" \
    "13:20: error: fmt.printf writes to the descriptor; it takes no 'mut'" \
    "14:19: error: fmt.printf requires a compile-time-static format string" \
    "15:40: error: fmt.printf is direct-call-only" \
    "16:32: error: fmt.printf is direct-call-only" \
    "17:53: error: str.format is direct-call-only"; do
    if ! printf '%s\n' "$printf_fd_errors" | grep -qF "$want"; then
        echo "FAIL: missing fmt.printf(fd, ...) diagnostic '$want'"
        fail=1
    fi
done

printf_buffer_errors=$("$ACE" tests/sema/printf/buffer_err.ace 2>&1)
for want in \
    "10:22: error: Buffer.printf appends to the buffer; it needs 'mut' at the call" \
    "11:13: error: parameter 'held' is read-only" \
    "12:28: error: Buffer.printf argument 1 for conversion at decoded byte 0 requires int" \
    "13:21: error: Buffer.printf format requires 2 value arguments, got 1" \
    "14:26: error: type mismatch: fmt.Buffer given where string expected"; do
    if ! printf '%s\n' "$printf_buffer_errors" | grep -qF "$want"; then
        echo "FAIL: missing Buffer.printf diagnostic '$want'"
        fail=1
    fi
done

if ! "$ACE" tests/sema/printf/no_value_arg_err.ace 2>&1 | grep -qF \
    "6:29: error: fmt.printf argument has no value"; then
    echo "FAIL: missing printf no-value-argument diagnostic"
    fail=1
fi

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

slice_literal_errors=$("$ACE" tests/sema/arrays/slice_literal_err.ace 2>&1)
for want in \
    "11:11: error: array literal cannot establish a writable view: mut []i64" \
    "12:24: error: keyed array literal needs an [E]T context, not []string" \
    "13:24: error: type mismatch: string given where i64 expected" \
    "14:23: error: array literal cannot establish a writable view for mut iteration" \
    "15:16: error: ambiguous call to overloaded function 'pick'"; do
    if ! printf '%s\n' "$slice_literal_errors" | grep -qF "$want"; then
        echo "FAIL: missing slice literal diagnostic '$want'"
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
    "mem.zalloc[T] takes exactly one element count" \
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
aggregate_readonly=$("$ACE" tests/sema/sequences/aggregate_readonly_err.ace 2>&1)
if ! printf '%s\n' "$aggregate_readonly" | grep -qF \
    "cannot write through the read-only view []Point"; then
    echo "FAIL: missing read-only aggregate-slice diagnostic"
    fail=1
fi
aggregate_identity=$("$ACE" tests/sema/sequences/aggregate_identity_err.ace 2>&1)
identity_mismatches=$(printf '%s\n' "$aggregate_identity" | grep -cF "type mismatch:")
if [ "$identity_mismatches" -ne 2 ]; then
    echo "FAIL: expected distinct and generic aggregate identity diagnostics"
    fail=1
fi
nested_uninitialized=$("$ACE" \
    tests/sema/uninitialized/nested_aggregate_array_err.ace 2>&1)
if ! printf '%s\n' "$nested_uninitialized" | grep -qF \
    "'values' was not fully initialized before use"; then
    echo "FAIL: missing nested aggregate initialization diagnostic"
    fail=1
fi
aggregate_bounds=$("$ACE" tests/sema/sequences/aggregate_bounds_err.ace 2>&1)
if ! printf '%s\n' "$aggregate_bounds" | grep -qF \
    "slice upper bound 3 exceeds source length 2"; then
    echo "FAIL: missing constant aggregate-slice bounds diagnostic"
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
    "call 'str.parse[bool]' matches no specialization" \
    "call 'str.parse[f64]' matches no specialization" \
    "call 'str.parse[u8]' matches no specialization"; do
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
ambiguous_spec_error=$("$ACE" tests/sema/specialization/ambiguous_err.ace 2>&1)
if ! printf '%s\n' "$ambiguous_spec_error" | grep -qF \
    "11:1: error: specialization 'pick[i64]' conforms to more than one generic overload of 'pick'"; then
    echo "FAIL: missing ambiguous-specialization diagnostic"
    fail=1
fi
domain_signature_error=$("$ACE" tests/sema/errors/domain_signature_err.ace 2>&1)
for want in \
    "2:8: error: error-domain names are identities, not return types; use Error" \
    "4:6: error: error-domain names are identities, not parameter types; use Error"; do
    if ! printf '%s\n' "$domain_signature_error" | grep -qF "$want"; then
        echo "FAIL: missing error-domain signature diagnostic '$want'"
        fail=1
    fi
done
missing_generic_error=$("$ACE" tests/sema/specialization/missing_generic_err.ace 2>&1)
if ! printf '%s\n' "$missing_generic_error" | grep -qF \
    "no generic declaration of 'pick' in this file"; then
    echo "FAIL: missing missing-generic diagnostic"
    fail=1
fi
type_param_error=$("$ACE" tests/sema/specialization/type_named_param_err.ace 2>&1)
if ! printf '%s\n' "$type_param_error" | grep -qF \
    "'T' names a type here — rename the type parameter to declare a generic"; then
    echo "FAIL: missing type-named-parameter hint"
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
        if (cd /tmp/ace_emit_errors && "$ACE" --debug -c "$name.ace") >/dev/null 2>/tmp/ace_test_errs; then
            echo "FAIL (should report errors): $f"
            fail=1
        elif [ -z "$want" ] || ! grep -qF "$want" /tmp/ace_test_errs; then
            echo "FAIL: expected diagnostic '$want' from $f"
            cat /tmp/ace_test_errs
            fail=1
        fi
    done
fi

# A bare std import accepts either std/name.ace or std/name/mod.ace, but
# diagnoses the std name when both definitions exist.
mkdir -p /tmp/ace_std_module/normal/std/directory_probe
mkdir -p /tmp/ace_std_module/normal/app
mkdir -p /tmp/ace_std_module/ambiguous/std/ambiguous_probe
cp "$ACE" /tmp/ace_std_module/normal/ace
cp "$ACE" /tmp/ace_std_module/ambiguous/ace
cp tests/stdlib_module/mod.ace \
   /tmp/ace_std_module/normal/std/directory_probe/mod.ace
cp tests/stdlib_module/impl.ace \
   /tmp/ace_std_module/normal/std/directory_probe/impl.ace
cp tests/stdlib_module/deep_import.ace \
   /tmp/ace_std_module/normal/app/deep_import.ace
cp tests/stdlib_module/flat.ace \
   /tmp/ace_std_module/ambiguous/std/ambiguous_probe.ace
cp tests/stdlib_module/mod.ace \
   /tmp/ace_std_module/ambiguous/std/ambiguous_probe/mod.ace
if ! /tmp/ace_std_module/normal/ace -o /tmp/ace_std_module/normal/out \
     tests/stdlib_module/main.ace || ! /tmp/ace_std_module/normal/out; then
    echo "FAIL: std directory module should resolve through mod.ace and re-export its type"
    fail=1
fi
cp tests/stdlib_module/location_probe.ace \
   /tmp/ace_std_module/normal/std/location_probe.ace
if ! /tmp/ace_std_module/normal/ace -o /tmp/ace_std_module/normal/location \
     tests/stdlib_module/location.ace || ! /tmp/ace_std_module/normal/location; then
    echo "FAIL: @file in a std module should be std/location_probe.ace"
    fail=1
fi
ambiguous_std=$(/tmp/ace_std_module/ambiguous/ace tests/stdlib_module/ambiguous.ace 2>&1)
for want in "ambiguous_probe.ace" "ambiguous_probe/mod.ace"; do
    if ! printf '%s\n' "$ambiguous_std" | grep -qF "$want"; then
        echo "FAIL: ambiguous std diagnostic should name '$want'"
        fail=1
    fi
done
missing_std=$(/tmp/ace_std_module/normal/ace tests/stdlib_module/missing.ace 2>&1)
for want in "absent_module_probe.ace" "absent_module_probe/mod.ace"; do
    if ! printf '%s\n' "$missing_std" | grep -qF "$want"; then
        echo "FAIL: missing std diagnostic should name '$want'"
        fail=1
    fi
done
deep_std=$(/tmp/ace_std_module/normal/ace \
    /tmp/ace_std_module/normal/app/deep_import.ace 2>&1)
if ! printf '%s\n' "$deep_std" | grep -qF \
    "implementation file 'std/directory_probe/impl.ace' is private to directory module 'std/directory_probe/mod.ace'"; then
    echo "FAIL: directory module implementation should reject external deep imports"
    fail=1
fi

# Recursive nested modules: a subdirectory with its own mod.ace is addressable
# by a `/`-separated path, and only the leaf segment binds.
mkdir -p /tmp/ace_std_module/normal/std/outer/inner
mkdir -p /tmp/ace_std_module/normal/std/outer/amb_leaf
mkdir -p /tmp/ace_std_module/normal/std/outer_missing
mkdir -p /tmp/ace_std_module/normal/nested_quoted_dir
cp tests/stdlib_module/nested_outer_mod.ace \
   /tmp/ace_std_module/normal/std/outer/mod.ace
cp tests/stdlib_module/nested_outer_inner_mod.ace \
   /tmp/ace_std_module/normal/std/outer/inner/mod.ace
cp tests/stdlib_module/nested_outer_missing_inner.ace \
   /tmp/ace_std_module/normal/std/outer_missing/inner.ace
cp tests/stdlib_module/nested_outer_amb_leaf.ace \
   /tmp/ace_std_module/normal/std/outer/amb_leaf.ace
cp tests/stdlib_module/nested_outer_amb_leaf_mod.ace \
   /tmp/ace_std_module/normal/std/outer/amb_leaf/mod.ace
cp tests/stdlib_module/nested_quoted_mod.ace \
   /tmp/ace_std_module/normal/nested_quoted_dir/mod.ace
cp tests/stdlib_module/nested_quoted_leaf.ace \
   /tmp/ace_std_module/normal/nested_quoted_dir/leaf.ace
cp tests/stdlib_module/nested_quoted.ace \
   /tmp/ace_std_module/normal/app/nested_quoted.ace
if ! /tmp/ace_std_module/normal/ace -o /tmp/ace_std_module/normal/nested_bare \
     tests/stdlib_module/nested_bare.ace || ! /tmp/ace_std_module/normal/nested_bare; then
    echo "FAIL: two-level nested import (bare form) should bind the leaf segment"
    fail=1
fi
if ! /tmp/ace_std_module/normal/ace -o /tmp/ace_std_module/normal/nested_alias \
     tests/stdlib_module/nested_alias.ace || ! /tmp/ace_std_module/normal/nested_alias; then
    echo "FAIL: two-level nested import (aliased form) should bind the leaf segment"
    fail=1
fi
if ! /tmp/ace_std_module/normal/ace -o /tmp/ace_std_module/normal/nested_quoted \
     /tmp/ace_std_module/normal/app/nested_quoted.ace ||
   ! /tmp/ace_std_module/normal/nested_quoted; then
    echo "FAIL: quoted relative import without .ace should resolve as a nested-module path"
    fail=1
fi
nested_notrans=$(/tmp/ace_std_module/normal/ace tests/stdlib_module/nested_no_transitivity.ace 2>&1)
if ! printf '%s\n' "$nested_notrans" | grep -qF "unknown name 'outer'"; then
    echo "FAIL: a nested import should not also bind its non-leaf segments"
    fail=1
fi
nested_missing_mod=$(/tmp/ace_std_module/normal/ace tests/stdlib_module/nested_missing_mod.ace 2>&1)
if ! printf '%s\n' "$nested_missing_mod" | grep -qF \
    "'outer_missing' has no mod.ace — 'outer_missing/inner' is not a nested module"; then
    echo "FAIL: a non-leaf segment without mod.ace should be a clear nested-module error"
    fail=1
fi
nested_ambiguous=$(/tmp/ace_std_module/normal/ace tests/stdlib_module/nested_ambiguous.ace 2>&1)
for want in "outer/amb_leaf.ace" "outer/amb_leaf/mod.ace"; do
    if ! printf '%s\n' "$nested_ambiguous" | grep -qF "$want"; then
        echo "FAIL: ambiguous nested leaf diagnostic should name '$want'"
        fail=1
    fi
done

# The compiler-shipped std/@ subtree is shared by std sources but is not
# importable by applications, including through traversal or symlink aliases.
mkdir -p /tmp/ace_std_internal/std/second
mkdir -p /tmp/ace_std_internal/std/@
mkdir -p /tmp/ace_std_internal/app
mkdir -p /tmp/ace_std_internal/app/copied
cp "$ACE" /tmp/ace_std_internal/ace
cp tests/stdlib_internal/first.ace /tmp/ace_std_internal/std/first.ace
cp tests/stdlib_internal/relative_std_err.ace /tmp/ace_std_internal/std/relative_std_err.ace
cp tests/stdlib_internal/second/mod.ace /tmp/ace_std_internal/std/second/mod.ace
cp tests/stdlib_internal/@/shared.ace /tmp/ace_std_internal/std/@/shared.ace
cp tests/stdlib_internal/@/leaf.ace /tmp/ace_std_internal/std/@/leaf.ace
cp tests/stdlib_internal/app/*.ace /tmp/ace_std_internal/app/
cp tests/stdlib_internal/copied/*.ace /tmp/ace_std_internal/app/copied/
ln -sf ../std/@/shared.ace /tmp/ace_std_internal/app/internal-link.ace
if ! /tmp/ace_std_internal/ace -o /tmp/ace_std_internal/out \
     /tmp/ace_std_internal/app/main.ace || ! /tmp/ace_std_internal/out; then
    echo "FAIL: ordinary std modules should share std/@ sources"
    fail=1
fi
if ! /tmp/ace_std_internal/ace /tmp/ace_std_internal/std/@/shared.ace; then
    echo "FAIL: an internal source should compile directly and import another internal source"
    fail=1
fi
relative_std=$(/tmp/ace_std_internal/ace /tmp/ace_std_internal/std/relative_std_err.ace 2>&1)
if ! printf '%s\n' "$relative_std" | grep -qF \
    "compiler-reserved standard-library implementation must use @name import syntax"; then
    echo "FAIL: reserved std implementation should require visible @name syntax"
    fail=1
fi
if ! /tmp/ace_std_internal/ace -o /tmp/ace_std_internal/copied \
     /tmp/ace_std_internal/app/copied_ok.ace || ! /tmp/ace_std_internal/copied; then
    echo "FAIL: equivalent source outside std/@ should remain ordinary code"
    fail=1
fi
internal_diag="compiler-reserved standard-library implementation 'std/@/shared.ace' may be imported only by standard-library sources"
for case in direct_err traversal_err local_err symlink_err cached_err; do
    internal_out=$(/tmp/ace_std_internal/ace "/tmp/ace_std_internal/app/$case.ace" 2>&1)
    if ! printf '%s\n' "$internal_out" | grep -qF "$internal_diag"; then
        echo "FAIL: $case should enforce the canonical std/@ boundary"
        fail=1
    fi
done
bare_internal=$(/tmp/ace_std_internal/ace /tmp/ace_std_internal/app/bare_err.ace 2>&1)
if ! printf '%s\n' "$bare_internal" | grep -qF \
    "standard library module 'internal' not found"; then
    echo "FAIL: bare import internal should remain invalid"
    fail=1
fi

# @when filters declarations before import loading, binding collisions, and
# semantic checking, while inactive source still has to parse correctly.
if ! "$ACE" --debug -o /tmp/ace_when tests/modules/when/main.ace || ! /tmp/ace_when; then
    echo "FAIL: @when should select exactly the host's import and declaration"
    fail=1
fi
unsupported_target=$("$ACE" --target=linux-arm64 tests/parser/when/main_ok.ace 2>&1)
if ! printf '%s\n' "$unsupported_target" | grep -qF \
    "unsupported target 'linux-arm64'"; then
    echo "FAIL: unsupported --target diagnostic"
    fail=1
fi

# Emission modes are accepted by every pipeline stage but affect only emitted
# code. Release is the emission default and contains no bounds machinery.
mkdir -p /tmp/ace_checked_bits
for f in runtime_range runtime_literal_range; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_bits/${f}_debug" "tests/checked_bits/$f.ace"; then
        echo "FAIL: checked bits range fixture did not compile: $f"
        fail=1
    elif "/tmp/ace_checked_bits/${f}_debug" >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL: checked out-of-range bit-field operation did not abort: $f"
        fail=1
    fi
    if ! "$ACE" --release -o "/tmp/ace_checked_bits/${f}_release" "tests/checked_bits/$f.ace" ||
       ! "/tmp/ace_checked_bits/${f}_release"; then
        echo "FAIL: release bit-field operation did not retain low field bits: $f"
        fail=1
    fi
done
# Self tail recursion: release lowers `return f(args)` inside f to a branch, so
# ten million levels run in one frame; debug keeps every call, so each level
# stays in a backtrace. A function whose levels' own storage is observable
# keeps its calls in both.
mkdir -p /tmp/ace_self_tail_recursion
if ! "$ACE" --release -o /tmp/ace_self_tail_recursion/deep tests/self_tail_recursion/deep.ace ||
   ! /tmp/ace_self_tail_recursion/deep; then
    echo "FAIL: release self tail recursion should run in one frame"
    fail=1
fi
if ! "$ACE" --debug -o /tmp/ace_self_tail_recursion/deep_debug tests/self_tail_recursion/deep.ace ||
   ! { otool -tv /tmp/ace_self_tail_recursion/deep_debug 2>/dev/null ||
       objdump -d /tmp/ace_self_tail_recursion/deep_debug; } |
     grep -Eq '(bl|call).*_ace\$fn\$sum\$'; then
    echo "FAIL: debug builds should keep self tail recursion as calls"
    fail=1
fi
for mode in --debug --release; do
    if ! "$ACE" "$mode" -o /tmp/ace_self_tail_recursion/frame tests/self_tail_recursion/frame.ace ||
       ! /tmp/ace_self_tail_recursion/frame; then
        echo "FAIL: self tail recursion over observable frame storage must stay calls ($mode)"
        fail=1
    fi
done
# auto-vectorization: release (vectorized) and debug (scalar) print the same bits
mkdir -p /tmp/ace_vectorize
if ! "$ACE" --release -o /tmp/ace_vectorize/release tests/vectorize/lanes.ace ||
   ! "$ACE" --debug -o /tmp/ace_vectorize/debug tests/vectorize/lanes.ace ||
   ! /tmp/ace_vectorize/release >/tmp/ace_vectorize/release.txt ||
   ! /tmp/ace_vectorize/debug >/tmp/ace_vectorize/debug.txt ||
   ! cmp -s /tmp/ace_vectorize/release.txt /tmp/ace_vectorize/debug.txt; then
    echo "FAIL: vectorized loops should compute what scalar loops do"
    fail=1
fi
# Runs in the background with its own scratch files; its log is
# printed at the end in section order.
(
T=/tmp/ace_sections/checked
mkdir -p "$T"
mkdir -p /tmp/ace_checked_bounds
cp tests/checked_bounds/*.ace /tmp/ace_checked_bounds/
if ! "$ACE" --debug tests/sema/sequences/main_ok.ace ||
   ! "$ACE" --release tests/sema/sequences/main_ok.ace; then
    echo "FAIL: check-only mode flags should not change semantic checking"
    fail=1
fi
if ! "$ACE" --tokens tests/lexer/hello_ok.ace >/tmp/ace_mode_plain ||
   ! "$ACE" --debug --tokens tests/lexer/hello_ok.ace >/tmp/ace_mode_debug ||
   ! "$ACE" --release --tokens tests/lexer/hello_ok.ace >/tmp/ace_mode_release ||
   ! cmp -s /tmp/ace_mode_plain /tmp/ace_mode_debug ||
   ! cmp -s /tmp/ace_mode_plain /tmp/ace_mode_release; then
    echo "FAIL: mode flags changed --tokens output"
    fail=1
fi
if ! "$ACE" --ast tests/parser/full_ok.ace >/tmp/ace_mode_plain ||
   ! "$ACE" --debug --ast tests/parser/full_ok.ace >/tmp/ace_mode_debug ||
   ! "$ACE" --release --ast tests/parser/full_ok.ace >/tmp/ace_mode_release ||
   ! cmp -s /tmp/ace_mode_plain /tmp/ace_mode_debug ||
   ! cmp -s /tmp/ace_mode_plain /tmp/ace_mode_release; then
    echo "FAIL: mode flags changed --ast output"
    fail=1
fi
for flags in "--debug --release" "--release --debug"; do
    if "$ACE" $flags tests/sema/sequences/main_ok.ace >/dev/null 2>$T/errs ||
       ! grep -qF -- "--debug and --release are mutually exclusive" $T/errs; then
        echo "FAIL: mode mutual exclusion for $flags"
        fail=1
    fi
done
for bad in --debug=yes --release=yes; do
    if "$ACE" "$bad" tests/sema/sequences/main_ok.ace >/dev/null 2>$T/errs ||
       ! grep -qF "unknown option '$bad'" $T/errs; then
        echo "FAIL: invalid mode spelling $bad"
        fail=1
    fi
done
for mode in --debug --release; do
    if "$ACE" "$mode" >/dev/null 2>$T/errs; then
        echo "FAIL: $mode without a source should show usage"
        fail=1
    fi
done
for mode in --debug --release; do
    for f in tests/sema/sequences/index_read_negative_err.ace \
             tests/sema/sequences/index_read_hi_err.ace \
             tests/sema/sequences/bounds_negative_err.ace \
             tests/sema/sequences/bounds_hi_err.ace; do
        if "$ACE" "$mode" "$f" >/dev/null 2>&1; then
            echo "FAIL: statically invalid bounds accepted in $mode: $f"
            fail=1
        fi
    done
done
if ! "$ACE" -o /tmp/ace_checked_bounds/default /tmp/ace_checked_bounds/valid.ace ||
   ! /tmp/ace_checked_bounds/default ||
   ! "$ACE" --debug -o /tmp/ace_checked_bounds/checked /tmp/ace_checked_bounds/valid.ace ||
   ! /tmp/ace_checked_bounds/checked ||
   ! "$ACE" --release -o /tmp/ace_checked_bounds/release /tmp/ace_checked_bounds/valid.ace ||
   ! /tmp/ace_checked_bounds/release; then
    echo "FAIL: valid dynamic bounds program failed in an emission mode"
    fail=1
fi
if ! "$ACE" --debug -o /tmp/ace_checked_bounds/proven_iteration \
     /tmp/ace_checked_bounds/proven_iteration.ace ||
   ! /tmp/ace_checked_bounds/proven_iteration; then
    echo "FAIL: proven/iteration checked fixture failed"
    fail=1
fi
for f in array_negative array_equal aggregate_array_equal array_slice_upper array_slice_order \
         aggregate_slice_greater slice_greater string_equal \
         mut_string_greater slice_lower slice_upper slice_order \
         slice_source_length; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_bounds/$f" \
         "/tmp/ace_checked_bounds/$f.ace" >/dev/null 2>$T/errs; then
        echo "FAIL: checked bounds fixture did not compile: $f"
        cat $T/errs
        fail=1
    elif "/tmp/ace_checked_bounds/$f" >/dev/null 2>$T/errs; then
        echo "FAIL: checked bounds fixture did not abort: $f"
        fail=1
    elif ! grep -qxF "/tmp/ace_checked_bounds/$f.ace:$(sed -n 's|^// expect-bounds: *||p' "tests/checked_bounds/$f.ace"): bounds violation" \
         $T/errs; then # dash also writes its "Aborted" job line here
        echo "FAIL: checked bounds fixture lacked its located diagnostic: $f"
        cat $T/errs
        fail=1
    fi
done
if command -v nm >/dev/null 2>&1; then
    if nm /tmp/ace_checked_bounds/default.o 2>/dev/null | grep -qF '_ace$bounds_fail'; then
        echo "FAIL: default emission retains debug bounds machinery"
        fail=1
    fi
    if nm /tmp/ace_checked_bounds/release.o 2>/dev/null |
         grep -Eq '_ace\$bounds|[[:space:]]U _?(abort|write)$'; then
        echo "FAIL: release object retains debug failure machinery"
        fail=1
    fi
    if nm /tmp/ace_checked_bounds/proven_iteration.o 2>/dev/null |
         grep -qF '_ace$bounds_fail'; then
        echo "FAIL: proven access or direct iteration retained a bounds guard"
        fail=1
    fi
fi
if command -v strings >/dev/null 2>&1 &&
   strings /tmp/ace_checked_bounds/release.o | grep -qF 'bounds violation'; then
    echo "FAIL: release object retains bounds diagnostic metadata"
    fail=1
fi

# Remaining debug-build contracts: arithmetic, call ranges, null use,
# allocator tracking, and the language-level assert primitive.
mkdir -p /tmp/ace_checked_features
if ! "$ACE" --debug -o /tmp/ace_checked_features/arithmetic_valid \
     tests/checked_arithmetic/valid.ace ||
   ! /tmp/ace_checked_features/arithmetic_valid; then
    echo "FAIL: valid checked arithmetic fixture"
    fail=1
fi
if ! "$ACE" --debug -o /tmp/ace_checked_features/arithmetic_wrap \
     tests/checked_arithmetic/wrap.ace ||
   ! /tmp/ace_checked_features/arithmetic_wrap; then
    echo "FAIL: bits wrapping arithmetic must not trap in debug builds"
    fail=1
fi
for f in signed_add unsigned_mul narrow_mul div_zero neg_min shift_count compound \
         imm_signed_add imm_unsigned_sub imm_narrow_add \
         pointer_null pointer_scale pointer_add pointer_sub; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_features/arithmetic_$f" \
         "tests/checked_arithmetic/$f.ace" >/dev/null 2>$T/errs ||
       "/tmp/ace_checked_features/arithmetic_$f" >/dev/null 2>$T/errs ||
       ! grep -qF "ace: arithmetic violation" $T/errs; then
        echo "FAIL: checked arithmetic fixture $f"
        fail=1
    fi
done
# release arithmetic wraps: the same immediate-operand overflows produce the
# fixed-width result, including narrow values computed in registers
for f in imm_signed_add imm_unsigned_sub imm_narrow_add narrow_release; do
    if ! "$ACE" --release -o "/tmp/ace_checked_features/arithmetic_${f}_release" \
         "tests/checked_arithmetic/$f.ace" ||
       ! "/tmp/ace_checked_features/arithmetic_${f}_release"; then
        echo "FAIL: release arithmetic fixture $f must wrap"
        fail=1
    fi
done
for f in noalias copy_length copy_overlap copy_typed_length copy_typed_overlap; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_features/contract_$f" \
         "tests/checked_contracts/$f.ace" >/dev/null 2>$T/errs ||
       "/tmp/ace_checked_features/contract_$f" >/dev/null 2>$T/errs ||
       ! grep -qF "ace: call contract violation" $T/errs; then
        echo "FAIL: checked call-contract fixture $f"
        fail=1
    fi
done
if ! "$ACE" --debug -o /tmp/ace_checked_features/contract_valid \
     tests/checked_contracts/valid.ace; then
    echo "FAIL: valid checked call-contract fixture did not compile"
    fail=1
else
    /tmp/ace_checked_features/contract_valid
    status=$?
    if [ "$status" -ne 42 ]; then
        echo "FAIL: valid checked call-contract fixture exited $status, want 42"
        fail=1
    fi
fi
# A conversion to an enum must give a member (Safety #2): debug builds test
# the value before narrowing, dense or sparse, and name the conversion.
for f in dense sparse negative wide; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_features/enum_$f" \
         "tests/checked_enums/$f.ace" >/dev/null 2>$T/errs ||
       "/tmp/ace_checked_features/enum_$f" >/dev/null 2>$T/errs ||
       ! grep -qF "tests/checked_enums/$f.ace:4:" $T/errs ||
       ! grep -qF ": enum value is not a member" $T/errs; then
        echo "FAIL: checked enum-conversion fixture $f"
        fail=1
    fi
done
# A runtime range step must be positive: debug builds test it once at loop
# entry, at the step's own width and even over an empty interval, and name
# the step's position.
for f in zero_step negative_step narrow_step empty_step; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_features/range_$f" \
         "tests/checked_ranges/$f.ace" >/dev/null 2>$T/errs ||
       "/tmp/ace_checked_features/range_$f" >/dev/null 2>$T/errs ||
       ! grep -qF "tests/checked_ranges/$f.ace:5:" $T/errs ||
       ! grep -qF ": range step violation" $T/errs; then
        echo "FAIL: checked range-step fixture $f"
        fail=1
    fi
done
for mode in --debug --release; do
    if ! "$ACE" "$mode" -o /tmp/ace_checked_features/range_valid \
         tests/checked_ranges/valid.ace; then
        echo "FAIL: valid range-step fixture did not compile in $mode"
        fail=1
    else
        /tmp/ace_checked_features/range_valid
        status=$?
        if [ "$status" -ne 42 ]; then
            echo "FAIL: valid range-step fixture exited $status in $mode, want 42"
            fail=1
        fi
    fi
done
for f in pointer function c_function; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_features/null_$f" \
         "tests/checked_null/$f.ace" >/dev/null 2>$T/errs ||
       "/tmp/ace_checked_features/null_$f" >/dev/null 2>$T/errs ||
       ! grep -qF "ace: null use violation" $T/errs; then
        echo "FAIL: checked null-use fixture $f"
        fail=1
    fi
done
if ! "$ACE" --debug -o /tmp/ace_checked_features/allocator_valid \
     tests/checked_allocator/valid.ace ||
   ! /tmp/ace_checked_features/allocator_valid; then
    echo "FAIL: valid checked allocator fixture"
    fail=1
fi
for f in double_release truncated_release leak; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_features/allocator_$f" \
         "tests/checked_allocator/$f.ace" >/dev/null 2>$T/errs ||
       "/tmp/ace_checked_features/allocator_$f" >/dev/null 2>$T/errs ||
       ! grep -qF "ace: allocator violation" $T/errs; then
        echo "FAIL: checked allocator fixture $f"
        fail=1
    fi
done
if ! "$ACE" --debug -o /tmp/ace_checked_features/assert_false \
     tests/assert/checked_false.ace ||
   /tmp/ace_checked_features/assert_false >/dev/null 2>$T/errs ||
   ! grep -qx "checked_false.ace:6:11: assertion failed" $T/errs; then
    echo "FAIL: debug assert failure"
    fail=1
fi
if ! "$ACE" --release -o /tmp/ace_checked_features/assert_release \
     tests/assert/release_stripped.ace ||
   ! /tmp/ace_checked_features/assert_release ||
   ! "$ACE" --debug -o /tmp/ace_checked_features/assert_true \
     tests/assert/true_ok.ace ||
   ! /tmp/ace_checked_features/assert_true; then
    echo "FAIL: assert true/release stripping"
    fail=1
fi
if "$ACE" tests/assert/false_err.ace >/dev/null 2>$T/errs ||
   ! grep -qF "assert condition is always false" $T/errs; then
    echo "FAIL: compile-time false assert diagnostic"
    fail=1
fi
if ! "$ACE" --release -c -o /tmp/ace_checked_features/release \
     tests/checked_arithmetic/signed_add.ace; then
    echo "FAIL: release checked-feature object"
    fail=1
elif command -v nm >/dev/null 2>&1 &&
     nm /tmp/ace_checked_features/release.o 2>/dev/null |
       grep -Eq '_ace\$(arithmetic|contract|null|registry)|[[:space:]]U _?(abort|write)$'; then
    echo "FAIL: release object retains debug machinery"
    fail=1
fi
if ! "$ACE" --release -c -o /tmp/ace_checked_features/pointer_release \
     tests/emit/pointers/arithmetic.ace; then
    echo "FAIL: release pointer-arithmetic object"
    fail=1
elif command -v nm >/dev/null 2>&1 &&
     nm /tmp/ace_checked_features/pointer_release.o 2>/dev/null |
       grep -Eq '_ace\$(arithmetic|null)|[[:space:]]U _?(abort|write)$'; then
    echo "FAIL: release pointer arithmetic retains debug machinery"
    fail=1
fi
if command -v strings >/dev/null 2>&1 &&
   strings /tmp/ace_checked_features/pointer_release.o |
     grep -Eq 'ace: (arithmetic|null) violation'; then
    echo "FAIL: release pointer arithmetic retains diagnostic metadata"
    fail=1
fi
if ! "$ACE" --release -o /tmp/ace_checked_features/far_offset \
     tests/emit/pointers/far_offset.ace ||
   { /tmp/ace_checked_features/far_offset; [ $? -ne 42 ]; }; then
    echo "FAIL: release far stack-object offset did not exit 42"
    fail=1
fi
if [ "$(uname -m)" = x86_64 ] && command -v objdump >/dev/null 2>&1; then
    if ! "$ACE" --release -c -o /tmp/ace_checked_features/far_offset \
         tests/emit/pointers/far_offset.ace ||
       ! objdump -d -M intel /tmp/ace_checked_features/far_offset.o >$T/dis ||
       grep -qF '[rbp+0x7fff' $T/dis ||
       ! grep -qF 'lea    rax,[rbp-' $T/dis; then
        echo "FAIL: x64 far stack-object access wrapped its rbp displacement"
        fail=1
    fi
fi

for f in null overflow alignment reinterpret_alignment; do
    if ! "$ACE" --debug -o "/tmp/ace_checked_features/pointer_construct_$f" \
         "tests/checked_pointer_construct/$f.ace" >/dev/null 2>$T/errs; then
        echo "FAIL: checked pointer/slice construction fixture did not compile: $f"
        fail=1
    elif "/tmp/ace_checked_features/pointer_construct_$f" \
         >/dev/null 2>$T/errs; then
        echo "FAIL: checked pointer/slice construction did not abort: $f"
        fail=1
    elif ! grep -Eq 'ace: (arithmetic violation|null use violation)' $T/errs; then
        echo "FAIL: checked pointer/slice construction lacked diagnostic: $f"
        fail=1
    fi
done
echo "$fail" >"$T/fail"
) >/tmp/ace_sections/checked.log 2>&1 &

# Darwin SDK BPF declarations (tests/darwin_bpf_layout.c) match the verified
# darwin-arm64 sys layout, on a Darwin host only
if [ "$(uname -s)" = Darwin ]; then
    if ! cc -target arm64-apple-macos tests/darwin_bpf_layout.c \
            -o /tmp/ace_darwin_bpf_layout 2>/tmp/ace_test_errs; then
        echo "FAIL: Darwin SDK BPF declarations do not match the verified sys layout"
        cat /tmp/ace_test_errs
        fail=1
    elif ! /tmp/ace_darwin_bpf_layout; then
        echo "FAIL: Darwin SDK BPF layout verifier failed"
        fail=1
    fi
fi

# vector IR (tests/vector_ir.c): hand-built functions verify, print as
# tests/vector_ir.ir, and match C lane for lane through the host's target
if ! cc -std=c11 -D_DEFAULT_SOURCE -O1 -DEMIT -o /tmp/ace_vector_emit tests/vector_ir.c $(ls src/*.c | grep -v src/main.c) \
        2>/tmp/ace_test_errs; then
    echo "FAIL: vector IR emitter did not build"
    cat /tmp/ace_test_errs
    fail=1
elif ! /tmp/ace_vector_emit /tmp/ace_vector.o >/tmp/ace_vector.ir 2>/tmp/ace_test_errs; then
    echo "FAIL: vector IR did not verify or emit"
    cat /tmp/ace_test_errs
    fail=1
elif ! diff -u tests/vector_ir.ir /tmp/ace_vector.ir; then
    echo "FAIL: vector IR printing differs"
    fail=1
elif ! cc -o /tmp/ace_vector tests/vector_ir.c /tmp/ace_vector.o 2>/tmp/ace_test_errs || ! /tmp/ace_vector >/dev/null; then
    echo "FAIL: vector IR results differ from C"
    cat /tmp/ace_test_errs
    fail=1
fi

# ELF object writer (tests/elf_object.c): a hand-encoded object links with
# the system cc and runs, on a linux-x64 host only
if [ "$(uname -s)-$(uname -m)" = Linux-x86_64 ]; then
    if ! cc -std=c11 -D_DEFAULT_SOURCE -o /tmp/ace_elf_emit tests/elf_object.c $(ls src/*.c | grep -v src/main.c) \
            2>/tmp/ace_test_errs; then
        echo "FAIL: ELF object writer test did not build"
        cat /tmp/ace_test_errs
        fail=1
    elif ! /tmp/ace_elf_emit /tmp/ace_elf.o 2>/tmp/ace_test_errs; then
        echo "FAIL: ELF object was not written"
        cat /tmp/ace_test_errs
        fail=1
    elif ! cc -o /tmp/ace_elf /tmp/ace_elf.o 2>/tmp/ace_test_errs; then
        echo "FAIL: ELF object did not link"
        cat /tmp/ace_test_errs
        fail=1
    else
        out=$(/tmp/ace_elf); code=$?
        if [ "$out" != "elf object" ] || [ $code -ne 42 ]; then
            echo "FAIL: ELF object printed '$out' and exited $code, want 'elf object' and 42"
            fail=1
        fi
    fi
fi

# emit fixtures: compile to an object, link with the system cc, run, and
# check the exit code. The expected code rides in a `// expect-exit: N` line;
# a `// expect-link: ARGS` line appends linker arguments (e.g. `-lm`).
if [ -d tests/emit ]; then
    rm -rf /tmp/ace_emit
    mkdir -p /tmp/ace_emit
    # Fixtures run in parallel (ACE_JOBS, default: CPU count); each writes
    # its failure report to its own file, printed afterward in sorted order.
    rm -rf /tmp/ace_emit_fail
    mkdir -p /tmp/ace_emit_fail
    find tests/emit -name '*.ace' | sort |
    ACE="$ACE" xargs -P "$JOBS" -n 1 sh -c '
        f=$1
        rel=${f#tests/emit/}
        name=${rel%.ace}
        report=/tmp/ace_emit_fail/$(printf "%s" "$rel" | tr / _)
        exp=$(sed -n "s/^.*expect-exit:[[:space:]]*\([0-9][0-9]*\).*/\1/p" "$f")
        if [ -z "$exp" ]; then
            echo "FAIL: $f has no '"'"'// expect-exit: N'"'"' line" >"$report"
            exit 0
        fi
        link=$(sed -n "s/^.*expect-link:[[:space:]]*//p" "$f")
        mkdir -p "/tmp/ace_emit/$(dirname "$rel")"
        cp "$f" "/tmp/ace_emit/$rel"
        out=$(cd /tmp/ace_emit && { "$ACE" --debug -c "$rel" && cc "$name.o" -o "$name" $link && "./$name"; } 2>"/tmp/ace_emit/$name.err")
        got=$?
        if [ "$got" -ne "$exp" ]; then
            { echo "FAIL: $f exited $got, want $exp"; cat "/tmp/ace_emit/$name.err"; } >>"$report"
        fi
        expout=$(sed -n "s/^.*expect-output:[[:space:]]*//p" "$f")
        if [ -n "$expout" ] && [ "$out" != "$expout" ]; then
            echo "FAIL: $f output '"'"'$out'"'"', want '"'"'$expout'"'"'" >>"$report"
        fi
    ' sh
    for report in /tmp/ace_emit_fail/*; do
        [ -f "$report" ] || continue
        cat "$report"
        fail=1
    done
    precision_out=$(/tmp/ace_emit/printf/precision)
    if [ "${#precision_out}" -ne 326 ] ||
       [ "${precision_out#0.}" = "$precision_out" ] ||
       [ "${precision_out#${precision_out%?}}" != "5" ]; then
        echo "FAIL: %.324f should emit 0., 323 zero bytes, and a final 5"
        fail=1
    fi
    if command -v strings >/dev/null 2>&1 &&
       strings /tmp/ace_emit/errors/no_descriptions.o |
       grep -qF 'ACE_DESCRIPTION_METADATA_MUST_BE_LAZY'; then
        echo "FAIL: unused Error description metadata was emitted"
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
        # undef OBJ lists OBJ's undefined symbols by C name: GNU nm prints
        # "U name", and Mach-O names carry one more leading underscore.
        if [ "$(uname -s)" = Darwin ]; then
            c_prefix=_ errno_symbol=__error
            process_boundary="_NSGetArgc _NSGetArgv _NSGetEnviron"
        else
            c_prefix= errno_symbol=__errno_location process_boundary=environ
        fi
        undef() { nm -u "$1" 2>/dev/null | sed -e 's/^ *U //' -e "s/^$c_prefix//"; }
        printf_undef=$(undef /tmp/ace_emit/printf/main.o)
        if printf '%s\n' "$printf_undef" |
           grep -Eq '^(printf|snprintf|vprintf|malloc|calloc|realloc|free)$'; then
            echo "FAIL: fmt.printf must not depend on C formatting, variadics, or allocation"
            fail=1
        fi
        if nm /tmp/ace_emit/printf/main.o 2>/dev/null |
           grep -qF '_ace$fn$fmt$printf$prstring$'; then
            echo "FAIL: checked fmt.printf calls must not emit a runtime printf body"
            fail=1
        fi
        net_undef=$(undef /tmp/ace_emit/net/main.o)
        if printf '%s\n' "$net_undef" |
           grep -Eq '^(malloc|calloc|realloc|free|printf|snprintf)$'; then
            echo "FAIL: net must not call a general-purpose allocator or C formatting"
            fail=1
        fi
        for symbol in socket setsockopt fcntl bind listen accept connect \
                      getsockname recv send getaddrinfo freeaddrinfo fork \
                      waitpid close "$errno_symbol"; do
            if ! printf '%s\n' "$net_undef" | grep -qx "$symbol"; then
                echo "FAIL: net object is missing OS boundary '$symbol'"
                fail=1
            fi
        done
        process_undef=$(undef /tmp/ace_emit/sys/process.o)
        for symbol in $process_boundary; do
            if ! printf '%s\n' "$process_undef" | grep -qx "$symbol"; then
                echo "FAIL: sys process object is missing OS boundary '$symbol'"
                fail=1
            fi
        done
        if printf '%s\n' "$process_undef" | grep -qx strlen; then
            echo "FAIL: sys process access must scan entry bytes directly"
            fail=1
        fi
        if printf '%s\n' "$process_undef" | grep -Eq '^(malloc|calloc|realloc|free)$'; then
            echo "FAIL: sys process access must not allocate"
            fail=1
        fi
        if undef /tmp/ace_emit/strings/byte_equality.o | grep -qx memcmp; then
            echo "FAIL: byte comparison must be emitted directly"
            fail=1
        fi
        if undef /tmp/ace_emit/mut_string/main.o | grep -qx memset; then
            echo "FAIL: byte fill and zero must be emitted directly"
            fail=1
        fi
        if undef /tmp/ace_emit/memory/copy_move.o | grep -Eq '^(memcpy|memmove)$'; then
            echo "FAIL: byte copy and move must be emitted directly"
            fail=1
        fi
        if ! undef /tmp/ace_emit/allocation/zeroed.o | grep -qx calloc; then
            echo "FAIL: mem.zalloc must lower to the platform zero-filling allocator"
            fail=1
        fi
        if { undef /tmp/ace_emit/internal/float_bits.o
             undef /tmp/ace_emit/internal/integer_bits.o; } |
           grep -Eq '(^|_)(memcpy|memmove|ecvt|strtod|strtof|popcount|clz|ctz|bswap|rotl|rotr)[0-9_]*$'; then
            echo "FAIL: bits operations must have no runtime dependency"
            fail=1
        fi
        if { undef /tmp/ace_emit/fmt/float.o
             undef /tmp/ace_emit/printf/precision.o; } |
           grep -Eq '^(ecvt|fcvt|strtod|strtof|floor|fegetround|fesetround|memcpy|memmove)$'; then
            echo "FAIL: float formatting must use the native exact engine"
            fail=1
        fi
        if undef /tmp/ace_emit/internal/native_float_parse.o | grep -Eq '^(strtod|strtof)$'; then
            echo "FAIL: float parsing must use the native exact engine"
            fail=1
        fi
    fi
    # Function entries and source-loop heads (every backward branch target in
    # a user function) start on 16-byte boundaries: the last hex digit is 0.
    if command -v nm >/dev/null 2>&1 && command -v otool >/dev/null 2>&1; then
        placement_o=/tmp/ace_emit/control_flow/code_placement.o
        if ! nm "$placement_o" | awk '
            $2 ~ /^[tT]$/ && ($3 ~ /^__ace\$fn\$/ || $3 == "_main") &&
            $1 !~ /0$/ { bad = 1 } END { exit bad }'; then
            echo "FAIL: function entries must be 16-byte aligned"
            fail=1
        fi
        if ! otool -tv "$placement_o" | awk '
            /^[^0-9].*:$/ { user = ($0 ~ /^__ace\$fn\$/ || $0 == "_main:"); next }
            user && $2 ~ /^(b|b\..*|cbn?z|tbn?z)$/ && $NF ~ /^0x/ {
                t = substr($NF, 3)
                t = substr("0000000000000000", length(t) + 1) t
                if (t <= $1 && t !~ /0$/) bad = 1
            }
            END { exit bad }'; then
            echo "FAIL: loop heads must be 16-byte aligned"
            fail=1
        fi
        # Globals no compiled function references are not emitted; C exports are.
        if command -v nm >/dev/null 2>&1; then
            syms=$(nm /tmp/ace_emit/basics/unreferenced_globals.o)
            if printf '%s\n' "$syms" | grep -Eq 'ace\$g\$(UNUSED|spare)$' ||
               ! printf '%s\n' "$syms" | grep -q 'ace\$g\$counter$' ||
               ! printf '%s\n' "$syms" | grep -q ' _exported$'; then
                echo "FAIL: unreferenced globals emitted, or referenced/exported ones missing"
                fail=1
            fi
        fi
        # Scalar locals, parameters, and range-loop state are never reached
        # through a computed frame address.
        if ! otool -tv /tmp/ace_emit/basics/local_slots.o | awk '
            /^[^0-9].*:$/ { hot = ($0 ~ /^__ace\$fn\$hot\$/); next }
            hot { seen = 1 }
            hot && $2 ~ /^(add|sub)$/ && $4 ~ /^(x29|sp),$/ && $3 !~ /^(x29|sp),$/ { bad = 1 }
            END { exit bad || !seen }'; then
            echo "FAIL: scalar locals must use direct frame slot access"
            fail=1
        fi
        # Local aggregate fields and descriptor words load, and tuple members
        # store, at x29 directly; a load is never moved back into the
        # register it loaded through.
        if ! otool -tv /tmp/ace_emit/basics/frame_fields.o | awk '
            /^[^0-9].*:$/ {
                fn = 0
                if ($0 ~ /^__ace\$fn\$(text_sum|through)\$/) fn = 2
                else if ($0 ~ /^__ace\$fn\$parts\$/) fn = 1
                last = ""; next
            }
            fn == 2 && $2 == "sub" && $4 == "x29," { bad = 1 }
            fn == 1 && $2 == "sub" && $3 ~ /^x1[5-7],$/ && $4 == "x29," { bad = 1 }
            fn && last != "" && $2 == "mov" && $4 == last { bad = 1 }
            {
                last = ""
                if (fn && $2 ~ /^ldr/ && $3 ~ /^[wx]/)
                    last = "x" substr($3, 2, length($3) - 2)
            }
            END { exit bad }'; then
            echo "FAIL: local aggregate access must address the frame directly"
            fail=1
        fi
        # Promoted integer locals are operands in their own registers:
        # arithmetic, compares, bool tests, and 64-bit indexes read x19-x28
        # (or x9-x14 in these call-free functions) directly, and arith copies
        # none of them into a temp. An index is read directly by the address
        # add or as the folded register-offset load's index register.
        if ! otool -tv /tmp/ace_emit/basics/register_operands.o | awk '
            /^[^0-9].*:$/ { fn = $0; next }
            {
                n = 0
                for (i = 3; i <= NF; i++) {
                    r = $i; gsub(/[][,]/, "", r)
                    if (r ~ /^[xw](9|1[0-4]|19|2[0-8])$/) n++
                }
            }
            fn ~ /^__ace\$fn\$arith\$/ && $2 == "mov" && n == 1 &&
                $3 ~ /^[xw][0-8],$/ { bad = 1 }
            fn ~ /^__ace\$fn\$arith\$/ && $2 ~ /^(add|sub)s$/ && n == 2 { arith = 1 }
            fn ~ /^__ace\$fn\$narrow\$/ && $2 == "cmp" && n == 2 { cmp = 1 }
            fn ~ /^__ace\$fn\$narrow\$/ && $2 ~ /^cbn?z$/ && n == 1 { test = 1 }
            fn ~ /^__ace\$fn\$indexed\$/ && $2 == "add" && $NF == "#3" && n == 1 { idx = 1 }
            fn ~ /^__ace\$fn\$indexed\$/ && $2 == "ldr" && $NF == "#3]" &&
                $(NF - 2) ~ /^[xw](9|1[0-4]|19|2[0-8]),$/ { idx = 1 }
            END { exit bad || !arith || !cmp || !test || !idx }'; then
            echo "FAIL: promoted locals must be read in place as operands"
            fail=1
        fi
        # The state of four nested range loops and the accumulator stays in
        # registers: no load or store in the frame beyond saving registers; a
        # function with any asm statement touches no callee-saved register.
        if ! otool -tv /tmp/ace_emit/control_flow/loop_registers.o | awk '
            /^[^0-9].*:$/ { fn = $0; next }
            fn ~ /^__ace\$fn\$nested\$/ {
                seen = 1
                if ($2 ~ /^(ldu?r|stu?r)$/ && $4 ~ /^\[(x29|sp)/) bad = 1
            }
            fn ~ /^__ace\$fn\$with_asm\$/ {
                for (i = 3; i <= NF; i++) {
                    r = $i; gsub(/[][,]/, "", r)
                    if (r ~ /^x(19|2[0-8])$/) bad = 1
                }
            }
            END { exit bad || !seen }'; then
            echo "FAIL: loop state must stay in registers"
            fail=1
        fi
        # A range that ascends by one from a constant zero over an unsigned
        # type compares its cursor with the end register: no frame access, trip
        # count, division, or direction test. A constant end within 4095 is a
        # cmp immediate.
        if ! otool -tv /tmp/ace_emit/control_flow/range_limits.o | awk '
            /^[^0-9].*:$/ {
                fn = 0
                if ($0 ~ /^__ace\$fn\$lean\$/) fn = 1
                else if ($0 ~ /^__ace\$fn\$fixed\$/) fn = 2
                next
            }
            fn && $2 ~ /^(udiv|mul|cbz|b\.hi|b\.gt)$/ { bad = 1 }
            fn && $2 ~ /^(ldu?r|stu?r)$/ && $4 ~ /^\[(x29|sp)/ { bad = 1 }
            fn == 1 && $2 == "cmp" && $3 ~ /^x[0-9]+,$/ && $4 ~ /^x[0-9]+$/ { reg = 1 }
            fn == 2 && $2 == "cmp" && $3 ~ /^x[0-9]+,$/ && $4 == "#0x4" { imm = 1 }
            END { exit bad || !reg || !imm }'; then
            echo "FAIL: ascending step-one ranges must compare the cursor with the end"
            fail=1
        fi
        # A value && branches on its left compare's flags; only the last
        # compare materializes a bool.
        if ! otool -tv /tmp/ace_emit/basics/logical_values.o | awk '
            /^[^0-9].*:$/ { fn = $0 ~ /^__ace\$fn\$has\$/; next }
            fn && $2 ~ /^(cbz|cbnz)$/ { bad = 1 }
            fn && $2 == "cset" { sets++ }
            fn && $2 == "b.hs" { branch = 1 }
            END { exit bad || sets != 1 || !branch }'; then
            echo "FAIL: a value && must branch on its left compare"
            fail=1
        fi
        # Call-free functions save no callee-saved register, and the ten hot
        # float locals of fpressure stay in registers: no float load or store
        # through an address.
        if ! otool -tv /tmp/ace_emit/basics/register_locals.o | awk '
            /^[^0-9].*:$/ { fn = $0; next }
            fn ~ /^__ace\$fn\$fpressure\$/ {
                seen = 1
                if ($2 ~ /^(ldur|stur|ldr|str|ldp|stp)$/ && $3 ~ /^[sd]/ && $0 ~ /\[/) bad = 1
            }
            {
                for (i = 3; i <= NF; i++) {
                    r = $i; gsub(/[][,]/, "", r)
                    if (r !~ /^([sd]([89]|1[0-5])|x(19|2[0-8]))$/) continue
                    if (fn ~ /^__ace\$fn\$(id8|idf|fpressure)\$/) bad = 1
                }
            }
            END { exit bad || !seen }'; then
            echo "FAIL: call-free float locals must stay in caller-saved registers"
            fail=1
        fi
        # A global integer const with a compile-time initializer is an
        # immediate: masked() is one `and` with a bitmask immediate and reads
        # no memory for MASK.
        if ! otool -tv /tmp/ace_emit/basics/const_immediate.o | awk '
            /^[^0-9].*:$/ { fn = $0; next }
            fn !~ /^__ace\$fn\$masked\$/ { next }
            $2 == "adrp" { bad = 1 }
            $2 == "and" && $NF == "#0x7fffffff" { saw = 1 }
            END { exit bad || !saw }'; then
            echo "FAIL: global integer consts must be folded to immediates"
            fail=1
        fi
        # A struct literal or tuple returned in registers computes each leaf
        # straight into its result register: no frame-temp address (`sub x0,
        # x29, ...`) is formed to reload from, and the second and third leaves
        # end in x1 / d2.
        if ! otool -tv /tmp/ace_emit/basics/return_registers.o | awk '
            /^[^0-9].*:$/ { fn = $0; next }
            fn ~ /^__ace\$fn\$(small_step|pair_step|vec_add|vec2_scale|divmod)\$/ &&
                $2 == "sub" && $3 == "x0," && $4 == "x29," { bad = 1 }
            fn ~ /^__ace\$fn\$small_step\$/ && $2 == "and" && $3 == "x0," { small = 1 }
            fn ~ /^__ace\$fn\$pair_step\$/ && $2 == "and" && $3 == "x1," { pair = 1 }
            fn ~ /^__ace\$fn\$vec_add\$/ && $2 == "fadd" && $3 == "d2," { vec = 1 }
            fn ~ /^__ace\$fn\$vec2_scale\$/ && $2 == "fmul" && $3 == "s1," { vec2 = 1 }
            fn ~ /^__ace\$fn\$divmod\$/ && $2 == "msub" && $3 == "x1," { divmod = 1 }
            END { exit bad || !small || !pair || !vec || !vec2 || !divmod }'; then
            echo "FAIL: register-returned literals and tuples must skip the frame temp"
            fail=1
        fi
        # A register-returned call assigned to a local or bound to typed
        # binders reaches its destination without a staged temp: after the
        # call no frame address is formed and no result register is reloaded
        # from the frame.
        if ! otool -tv /tmp/ace_emit/basics/call_result_sink.o | awk '
            /^_main:/ { exit }
            /^[^0-9].*:$/ { fn = $0; after = 0; next }
            fn ~ /^__ace\$fn\$sink_/ && $2 == "bl" && $3 !~ /fail/ {
                after = 1
                for (k = split("small pair vec padded divmod", s, " "); k; k--) if (fn ~ ("sink_" s[k]) && !seen[s[k]]++) nseen++
                next
            }
            after && (($2 == "sub" && $4 == "x29,") || ($2 == "add" && $4 == "sp,")) { bad = 1 }
            after && $2 ~ /^ldu?r$/ && $3 ~ /^[xd][0-7],$/ && $4 ~ /^\[(x29|sp)/ { bad = 1 }
            END { exit bad || nseen != 5 }'; then
            echo "FAIL: register-returned call results must be stored straight into the destination"
            fail=1
        fi
        # A by-value aggregate argument that is a local goes straight to its
        # argument registers: before the call no frame address is formed for
        # a staged copy.
        if ! otool -tv /tmp/ace_emit/basics/call_frame_args.o | awk '
            /^[^0-9].*:$/ { fn = $0; done = 0; next }
            fn ~ /^__ace\$fn\$use_(pair|vec)\$/ && !done {
                if ($2 == "bl" && $3 !~ /fail/) {
                    done = 1
                    if (fn ~ /use_pair/) pair = 1
                    else vec = 1
                    next
                }
                if (($2 == "sub" && $4 == "x29,") || ($2 == "add" && $4 == "sp,")) bad = 1
            }
            END { exit bad || !pair || !vec }'; then
            echo "FAIL: frame-resident aggregate arguments must load straight from their slots"
            fail=1
        fi
        # A call-free function keeps every value in caller-saved registers
        # from x9 up, and values whose live ranges do not overlap share one:
        # straight()'s four take only x9 and x10 and it saves no callee-saved
        # register (the IR backend compiles it).
        if ! otool -tv /tmp/ace_emit/basics/register_locals.o | awk '
            /^[^0-9].*:$/ { fn = ($0 ~ /^__ace\$fn\$straight\$/); next }
            fn {
                for (i = 3; i <= NF; i++) {
                    r = $i; gsub(/[][,]/, "", r)
                    if (r == "x9") low = 1
                    if (r == "x10") high = 1
                    if (r ~ /^x(1[1-4]|19|2[0-8])$/) bad = 1
                }
            }
            END { exit bad || !low || !high }'; then
            echo "FAIL: call-free values must share x9 up by live range"
            fail=1
        fi
        # A small struct local popped from a slice loads each field straight
        # into its register at the field's own width, and a whole store back
        # writes each field: no copy through x16 and no narrow slot reload.
        if ! otool -tv /tmp/ace_emit/basics/struct_registers.o | awk '
            /^[^0-9].*:$/ { fn = ($0 ~ /^__ace\$fn\$frames\$/); next }
            fn && $2 ~ /^(ldr|str)$/ && $3 == "x16," { bad = 1 }
            fn && $2 ~ /^(ldur|stur)$/ && $3 ~ /^w/ { bad = 1 }
            fn && $2 == "ldr" && $3 ~ /^w/ && $5 == "#0x4]" { narrow = 1 }
            fn && $2 == "ldr" && $3 ~ /^x/ && $5 == "#0x8]" { wide = 1 }
            fn && $2 == "str" && $3 ~ /^w/ && $5 == "#0x4]" { store = 1 }
            END { exit bad || !narrow || !wide || !store }'; then
            echo "FAIL: a small struct local must load and store field by field"
            fail=1
        fi
        # A literal assigned to an element is stored field by field straight
        # into it: no temp built and copied through x16.
        if ! otool -tv /tmp/ace_emit/basics/struct_registers.o | awk '
            /^[^0-9].*:$/ { fn = ($0 ~ /^__ace\$fn\$push\$/); next }
            fn && $2 ~ /^(ldr|str)$/ && $3 == "x16," { bad = 1 }
            fn && $2 == "str" && $3 ~ /^w/ && $5 == "#0x4]" { narrow = 1 }
            fn && $2 == "str" && $3 ~ /^x/ && $5 == "#0x8]" { wide = 1 }
            END { exit bad || !narrow || !wide }'; then
            echo "FAIL: a struct literal must store straight into its destination"
            fail=1
        fi
        # Passed to and assigned from a call returning an HFA, a Vec3 local
        # stays in registers: inside the loop (its head to the backward
        # branch) no float goes through memory, and after the call the result
        # moves from d0 into a callee-saved d register by fmov.
        if ! otool -tv /tmp/ace_emit/basics/struct_registers.o | awk '
            /^[^0-9].*:$/ { fn = ($0 ~ /^__ace\$fn\$hfa_loop\$/); next }
            function hex(h,   v, i) {
                sub(/^0x/, "", h)
                v = 0
                for (i = 1; i <= length(h); i++) v = v * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
                return v
            }
            fn { n++; addr[n] = hex($1); op[n] = $2; a3[n] = $3; a4[n] = $4 }
            END {
                # the loop back edge, an unconditional jump (a top-tested
                # loop) or the rotated bottom tests conditional branch
                for (i = 1; i <= n; i++)
                    if (op[i] ~ /^(b|b\.[a-z]+)$/ && a3[i] ~ /^0x/ && hex(a3[i]) < addr[i]) { head = hex(a3[i]); tail = addr[i] }
                for (i = 1; i <= n; i++) {
                    if (addr[i] < head || addr[i] > tail) continue
                    if (op[i] ~ /^(ldur|stur|ldr|str|ldp|stp)$/ && a3[i] ~ /^d/) bad = 1
                    if (op[i] == "bl") called = 1
                    if (called && op[i] == "fmov" && a3[i] ~ /^d([89]|1[0-5]),$/ && a4[i] == "d0") back = 1
                }
                exit bad || !head || !back
            }'; then
            echo "FAIL: an HFA struct local must stay in registers across calls"
            fail=1
        fi
        # A call-free function takes caller-saved registers before x19-x28. In
        # one with a function-value call, what lives across the call is in
        # callee-saved registers: after it, no caller-saved register but the
        # result x0 is read before it is written.
        if ! otool -tv /tmp/ace_emit/basics/leaf_registers.o | awk '
            /^[^0-9].*:$/ {
                fn = ($0 ~ /^__ace\$fn\$hot\$/) ? 1 : ($0 ~ /^__ace\$fn\$with_call\$/) ? 2 : 0
                next
            }
            fn == 1 {
                for (i = 3; i <= NF; i++) {
                    r = $i; gsub(/[][,]/, "", r)
                    if (r == "x9") low = 1
                    if (r == "x19") spill = 1
                }
            }
            fn == 2 && called {
                first = ($2 ~ /^(stu?r|stp|cmp|cmn|tst|cbn?z|blr|ret)$/) ? 3 : 4
                for (i = first; i <= NF; i++) {
                    r = $i; gsub(/[][,!]/, "", r); sub(/^w/, "x", r)
                    if (r ~ /^x([1-9]|1[0-5])$/ && !(r in written)) bad = 1
                }
                if (first == 4) { r = $3; gsub(/,/, "", r); sub(/^w/, "x", r); written[r] = 1 }
            }
            fn == 2 && $2 == "blr" { called = 1 }
            END { exit bad || !low || !spill || !called }'; then
            echo "FAIL: values live across a call must stay in callee-saved registers"
            fail=1
        fi
        # Small constants are immediates: the digit scan compares against #0x30
        # and counts with an immediate add on its own register, and dest()
        # compares with and adds its small constants as immediates.
        if ! otool -tv /tmp/ace_emit/basics/immediate_operands.o | awk '
            /^[^0-9].*:$/ {
                fn = ($0 ~ /^__ace\$fn\$digits\$/) ? 1 : ($0 ~ /^__ace\$fn\$dest\$/) ? 2 : 0
                next
            }
            fn == 1 && $2 == "cmp" && $3 ~ /^w/ && $4 == "#0x30" { cmp = 1 }
            fn == 1 && $2 ~ /^adds?$/ && $3 == $4 && $5 == "#0x1" { inc = 1 }
            fn == 2 && $2 ~ /^(cmp|cmn)$/ && $4 ~ /^#/ { dcmp = 1 }
            fn == 2 && $2 ~ /^adds?$/ && $5 == "#0x1" { dinc = 1 }
            END { exit !cmp || !inc || !dcmp || !dinc }'; then
            echo "FAIL: small constants must be immediates"
            fail=1
        fi
        # Offsets fold into the access: deep() loads the nested field at the
        # summed offset, and global_field() keeps each relocated adrp/add pair
        # intact ahead of the field access.
        if ! otool -tv /tmp/ace_emit/basics/peephole.o | awk '
            /^[^0-9].*:$/ {
                fn = 0
                if ($0 ~ /^__ace\$fn\$deep\$/) fn = 2
                else if ($0 ~ /^__ace\$fn\$global_field\$/) fn = 3
                last = ""; next
            }
            fn == 2 && $2 == "ldr" && $5 == "#0x30]" { summed = 1 }
            fn == 3 && reloc {
                if ($2 != "add" || $3 != reloc || $4 != reloc || $5 != "#0x0") bad = 1
                pairs++
            }
            fn == 3 && $2 == "ldr" && $5 == "#0x30]" { field = 1 }
            fn { reloc = ($2 == "adrp") ? $3 : "" }
            END { exit bad || !summed || pairs < 2 || !field }'; then
            echo "FAIL: field offsets must fold into accesses, never into a relocated instruction"
            fail=1
        fi
        # A by-reference parameter's pointer lives in a register: the hot
        # Counter.run loop loads the receiver's fields through it and never
        # reloads a frame slot.
        if ! otool -tv /tmp/ace_emit/basics/byref_registers.o | awk '
            /^[^0-9].*:$/ { fn = ($0 ~ /^__ace\$fn\$Counter\$run\$/); next }
            function hex(h,   v, i) {
                sub(/^0x/, "", h)
                v = 0
                for (i = 1; i <= length(h); i++) v = v * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
                return v
            }
            fn {
                n++; addr[n] = hex($1); op[n] = $2; a4[n] = $4
                target[n] = op[n] ~ /^cbn?z$/ ? $4 : $3
            }
            END {
                # the loop back edge, an unconditional jump (a top-tested
                # loop) or the rotated bottom tests branch (cbz/cbnz or a
                # conditional branch)
                for (i = 1; i <= n; i++)
                    if (op[i] ~ /^(b|b\.[a-z]+|cbn?z)$/ && target[i] ~ /^0x/ && hex(target[i]) < addr[i])
                        { head = hex(target[i]); tail = addr[i] }
                for (i = 1; i <= n; i++) {
                    if (addr[i] < head || addr[i] > tail || op[i] !~ /^ldu?r$/) continue
                    if (a4[i] ~ /^\[(x29|sp)/) bad = 1
                    else if (a4[i] ~ /^\[x[0-9]+[],]/) direct++
                }
                exit bad || !head || direct < 2
            }'; then
            echo "FAIL: by-reference parameter pointers must stay in registers"
            fail=1
        fi
    fi
    # -o mode drives the same pipeline through the cc link step
    "$ACE" --debug -o /tmp/ace_emit/direct tests/emit/basics/main.ace && /tmp/ace_emit/direct
    got=$?
    if [ "$got" -ne 42 ]; then
        echo "FAIL: -o mode exited $got, want 42"
        fail=1
    fi
    # --report writes whole-compilation statistics to stderr. welcome imports
    # fmt, so the source-file count also verifies imported-module accounting.
    report=$(cd /tmp/ace_emit/basics && "$ACE" --debug --report -c welcome.ace 2>&1 >/dev/null)
    if ! printf '%s\n' "$report" | grep -q '^Ace compile report:$'; then
        echo "FAIL: --report heading missing"
        fail=1
    fi
    if ! printf '%s\n' "$report" | grep -q '^--------------------$'; then
        echo "FAIL: --report separator missing"
        fail=1
    fi
    if ! printf '%s\n' "$report" | grep -q 'source files:.*11$'; then
        echo "FAIL: --report should count root + fmt/target + sys/target + decimal/bigint/ryu/table/formatting/mem sources"
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
    if [ -n "$DWARFDUMP" ]; then
        dwarf=$("$DWARFDUMP" --debug-info --debug-line /tmp/ace_emit/basics/main.o 2>/tmp/ace_test_errs)
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
        multidwarf=$("$DWARFDUMP" --debug-line /tmp/ace_emit/basics/welcome.o 2>/tmp/ace_test_errs)
        if [ $? -ne 0 ] ||
           ! printf '%s\n' "$multidwarf" | grep -q '/std/fmt/mod.ace' ||
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
# Runs in the background with its own scratch files; its log is
# printed at the end in section order.
(
T=/tmp/ace_sections/modules
mkdir -p "$T"
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
        rm -f $T/run
        out=$({ "$ACE" --debug -o "/tmp/ace_modules/$name" "$f" && "/tmp/ace_modules/$name"; } 2>$T/run)
        got=$?
        if [ "$got" -ne "$exp" ]; then
            echo "FAIL: $f exited $got, want $exp"
            cat $T/run
            fail=1
        fi
        expout=$(sed -n 's/^.*expect-output:[[:space:]]*//p' "$f")
        if [ -n "$expout" ] && [ "$out" != "$expout" ]; then
            echo "FAIL: $f output '$out', want '$expout'"
            fail=1
        fi
    done
    cycle=$("$ACE" tests/modules/cycle/main.ace 2>&1)
    if ! printf '%s\n' "$cycle" | grep -Eq 'import cycle: [^ ]*/a\.ace -> [^ ]*/b\.ace -> [^ ]*/c\.ace -> [^ ]*/a\.ace$'; then
        echo "FAIL: import cycle should list only the loop, in import order"
        printf '%s\n' "$cycle"
        fail=1
    fi
    # multi-file DWARF across a local module, a stdlib module, and the root
    if [ -n "$DWARFDUMP" ]; then
        moddwarf=$("$DWARFDUMP" --debug-line /tmp/ace_modules/diamond.o 2>$T/errs)
        if [ $? -ne 0 ] ||
           ! printf '%s\n' "$moddwarf" | grep -q '/shared.ace' ||
           ! printf '%s\n' "$moddwarf" | grep -q '/main.ace'; then
            echo "FAIL: module and root sources should both appear in DWARF"
            cat $T/errs
            fail=1
        fi
    fi
fi
echo "$fail" >"$T/fail"
) >/tmp/ace_sections/modules.log 2>&1 &

# std/ is found beside the real compiler binary, not beside argv[0]: run the
# compiler by bare name through a PATH symlink from an unrelated directory.
rm -rf /tmp/ace_std_lookup
mkdir -p /tmp/ace_std_lookup/bin /tmp/ace_std_lookup/work
ln -s "$ACE" /tmp/ace_std_lookup/bin/ace
printf 'import fmt\n\nmain(): int {\n    fmt.print("std found\\n")\n    return 0\n}\n' >/tmp/ace_std_lookup/work/main.ace
std_out=$(cd /tmp/ace_std_lookup/work && PATH="/tmp/ace_std_lookup/bin:$PATH" ace -o main main.ace 2>&1 && ./main 2>&1)
if [ "$std_out" != "std found" ]; then
    echo "FAIL: std lookup through a PATH symlink: $std_out"
    fail=1
fi

# driver fixtures: external link inputs and options through the -o pipeline.
# The cc launch is an argument vector with no shell, so paths with spaces and
# shell metacharacters link exactly as written; -L/-l and -framework cover
# archives and Apple frameworks; validation and linker failures are checked.
# Runs in the background with its own scratch files; its log is
# printed at the end in section order.
(
T=/tmp/ace_sections/driver
mkdir -p "$T"
if [ -d tests/driver ]; then
    mkdir -p "/tmp/ace_driver/lib dir" "/tmp/ace_driver/space dir"
    cc -c tests/driver/helper.c -o /tmp/ace_driver/helper_raw.o
    cc -c tests/driver/abi.c -o /tmp/ace_driver/abi_c.o
    ar rcs "/tmp/ace_driver/lib dir/libhelper.a" /tmp/ace_driver/helper_raw.o
    cp /tmp/ace_driver/helper_raw.o '/tmp/ace_driver/evil;$(echo pwn)`id`.o'
    cp /tmp/ace_driver/helper_raw.o "/tmp/ace_driver/space dir/help lib.o"

    out=$("$ACE" --debug -o /tmp/ace_driver/app_obj tests/driver/main_obj.ace \
          /tmp/ace_driver/helper_raw.o && /tmp/ace_driver/app_obj)
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: linking an extra .o exited $got, want 7"
        fail=1
    fi
    out=$("$ACE" --debug -o /tmp/ace_driver/app_lib tests/driver/main_obj.ace \
          -L "/tmp/ace_driver/lib dir" -l helper && /tmp/ace_driver/app_lib)
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: -L/-l archive link exited $got, want 7"
        fail=1
    fi
    # Apple frameworks exist on a Darwin host only
    if [ "$(uname -s)" = Darwin ]; then
        out=$("$ACE" --debug -o /tmp/ace_driver/app_fw tests/driver/main_fw.ace \
              -framework CoreFoundation && /tmp/ace_driver/app_fw)
        got=$?
        if [ "$got" -ne 0 ]; then
            echo "FAIL: -framework link exited $got, want 0"
            fail=1
        fi
    fi
    # spaces and shell metacharacters must travel as plain argument bytes
    out=$("$ACE" --debug -o "/tmp/ace_driver/space dir/my app" tests/driver/main_obj.ace \
          "/tmp/ace_driver/space dir/help lib.o" && "/tmp/ace_driver/space dir/my app")
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: paths with spaces exited $got, want 7"
        fail=1
    fi
    out=$("$ACE" --debug -o /tmp/ace_driver/app_evil tests/driver/main_obj.ace \
          '/tmp/ace_driver/evil;$(echo pwn)`id`.o' && /tmp/ace_driver/app_evil)
    got=$?
    if [ "$got" -ne 7 ]; then
        echo "FAIL: shell metacharacters in names exited $got, want 7"
        fail=1
    fi
    # -o FILE.ace compiles that file in place: executable and object take the stem
    printf 'main(): int {\n    return 5\n}\n' > /tmp/ace_driver/stem_test.ace
    if ! "$ACE" --debug -o /tmp/ace_driver/stem_test.ace >/dev/null 2>$T/errs; then
        echo "FAIL: -o FILE.ace in-place compile failed"
        cat $T/errs
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
    if ! "$ACE" --debug -o /tmp/ace_driver/app_abi tests/driver/abi.ace \
         /tmp/ace_driver/abi_c.o >/dev/null 2>$T/errs; then
        echo "FAIL: C ABI oracle did not compile/link"
        cat $T/errs
        fail=1
    else
        /tmp/ace_driver/app_abi
        got=$?
        if [ "$got" -ne 0 ]; then
            echo "FAIL: bidirectional C ABI oracle exited $got, want 0"
            fail=1
        fi
    fi

    # C global variables (C Interop #6): from an object, and from a shared
    # library, which only the GOT reaches; debug and release
    mkdir -p /tmp/ace_driver/globals
    cc -c tests/driver/globals.c -o /tmp/ace_driver/globals_c.o
    cc -shared -fPIC tests/driver/globals.c -o /tmp/ace_driver/globals/libglobals.so
    cp /tmp/ace_driver/globals/libglobals.so /tmp/ace_driver/globals/libglobals.dylib
    for mode in --debug --release; do
        for input in object shared; do
            if [ "$input" = object ]; then set -- /tmp/ace_driver/globals_c.o
            else set -- -L /tmp/ace_driver/globals -l globals; fi
            if ! "$ACE" $mode -o /tmp/ace_driver/app_globals tests/driver/globals.ace "$@" \
                    >/dev/null 2>$T/errs; then
                echo "FAIL: C globals ($input, $mode) did not compile/link"
                cat $T/errs
                fail=1
                continue
            fi
            LD_LIBRARY_PATH=/tmp/ace_driver/globals DYLD_LIBRARY_PATH=/tmp/ace_driver/globals \
                /tmp/ace_driver/app_globals
            got=$?
            if [ "$got" -ne 0 ]; then
                echo "FAIL: C globals ($input, $mode) exited $got, want 0"
                fail=1
            fi
        done
    done

    driver_diag() {
        want=$1; shift
        if "$ACE" "$@" >/dev/null 2>$T/errs; then
            echo "FAIL: expected driver rejection: $*"
            fail=1
        elif ! grep -qF "$want" $T/errs; then
            echo "FAIL: expected diagnostic '$want' from: $*"
            cat $T/errs
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
    if "$ACE" --debug -o /tmp/ace_driver/app_bad tests/driver/main_obj.ace \
       -l ace_missing_lib_zz >/dev/null 2>$T/errs; then
        echo "FAIL: missing library should fail to link"
        fail=1
    elif ! grep -qF "ace: linker exited with status" $T/errs; then
        echo "FAIL: linker failure should report the exit status"
        cat $T/errs
        fail=1
    fi
fi
echo "$fail" >"$T/fail"
) >/tmp/ace_sections/driver.log 2>&1 &

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
if ! "$ACE" --tokens tests/lexer/numbers_ok.ace | grep -qF "${tab}string${tab}"'"tab\there \"quoted\" backslash\\ hex\x7f nul\0"'; then
    echo "FAIL: --tokens should print a string token escaped as its literal"
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
uninitialized_spec=$("$ACE" tests/sema/uninitialized/specialization_err.ace 2>&1)
for expect in \
    "specialization_err.ace:7:12: error: 'x' was not initialized before use" \
    "specialization_err.ace:11:12: error: 'unused' was not initialized before use"; do
    if ! printf '%s\n' "$uninitialized_spec" | grep -qF "$expect"; then
        echo "FAIL: specialization-body definite-initialization diagnostic: $expect"
        fail=1
    fi
done
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

# package fixtures: one directory per case. The root ace.pkg passed to
# --pkg-graph/--pkg-check sits directly at the case root, or at "$case/app"
# when its dependencies must sit outside it; dependencies live under the
# case's pkg/<name> and are found by the nearest-pkg/ walk-up. A `// expect-ok:
# name1,name2,...` comment names the expected dependency-first package-name
# order from --pkg-graph; `// expect-err: substring` names diagnostic text a
# failing graph-resolution case must report instead. `// expect-check-ok` and
# `// expect-check-err: substring` are the --pkg-check equivalents, covering
# cross-package import resolution (module.c) rather than graph resolution
# alone. A case may use either pair, or both. `// target: NAME` resolves the
# graph for NAME rather than the host (qualified sections, Package System #16).
if [ -d tests/package ]; then
    for case_dir in tests/package/*/; do
        if [ -f "${case_dir}ace.pkg" ]; then dir=${case_dir%/}
        elif [ -f "${case_dir}app/ace.pkg" ]; then dir="${case_dir}app"
        else continue
        fi
        f="$dir/ace.pkg"
        ok=$(sed -n 's/^\/\/ *expect-ok:[[:space:]]*//p' "$f")
        err=$(sed -n 's/^\/\/ *expect-err:[[:space:]]*//p' "$f")
        check_ok=$(sed -n 's/^\/\/ *expect-check-ok[[:space:]]*.*/yes/p' "$f")
        check_err=$(sed -n 's/^\/\/ *expect-check-err:[[:space:]]*//p' "$f")
        tgt=$(sed -n 's/^\/\/ *target:[[:space:]]*//p' "$f")
        if [ -n "$ok" ] || [ -n "$err" ]; then
            out=$("$ACE" --pkg-graph="$dir" ${tgt:+--target=$tgt} 2>/tmp/ace_test_errs)
            got=$?
            if [ -n "$ok" ]; then
                if [ "$got" -ne 0 ]; then
                    echo "FAIL (should resolve clean): $dir"
                    cat /tmp/ace_test_errs
                    fail=1
                else
                    names=$(printf '%s\n' "$out" | cut -f1 | paste -sd, -)
                    if [ "$names" != "$ok" ]; then
                        echo "FAIL: $dir resolved order '$names', want '$ok'"
                        fail=1
                    fi
                fi
            else
                if [ "$got" -eq 0 ]; then
                    echo "FAIL (should report errors): $dir"
                    fail=1
                elif ! grep -qF "$err" /tmp/ace_test_errs; then
                    echo "FAIL: $dir missing diagnostic '$err'"
                    cat /tmp/ace_test_errs
                    fail=1
                fi
            fi
        fi
        if [ -n "$check_ok" ] || [ -n "$check_err" ]; then
            "$ACE" --pkg-check="$dir" >/dev/null 2>/tmp/ace_test_errs
            got=$?
            if [ -n "$check_ok" ]; then
                if [ "$got" -ne 0 ]; then
                    echo "FAIL (should check clean): $dir"
                    cat /tmp/ace_test_errs
                    fail=1
                fi
            else
                if [ "$got" -eq 0 ]; then
                    echo "FAIL (should report errors): $dir"
                    fail=1
                elif ! grep -qF "$check_err" /tmp/ace_test_errs; then
                    echo "FAIL: $dir missing diagnostic '$check_err'"
                    cat /tmp/ace_test_errs
                    fail=1
                fi
            fi
        fi
        if [ -z "$ok" ] && [ -z "$err" ] && [ -z "$check_ok" ] && [ -z "$check_err" ]; then
            echo "FAIL: $f has no 'expect-ok'/'expect-err'/'expect-check-ok'/'expect-check-err' comment"
            fail=1
        fi
    done
fi

# `ace pkg` real subcommand fixtures: one directory per case, exercising the
# CLI surface itself (cwd-based ace.pkg discovery, build/run linking, check)
# rather than the --pkg-graph/--pkg-check debug hooks above. The root
# ace.pkg sits directly at the case root, or at "$case/app" with its
# dependencies under "$case/pkg/<name>", matching the tests/package convention. Markers live as
# `//`-comments in ace.pkg: `// expect-build-exit: N` builds an app case and
# runs it (via both `pkg build` + the binary, and `pkg run` directly),
# expecting exit code N, passing the words of `// run-args: ...` to the binary
# and after `pkg run --debug --`, also run from the repository root through
# `--dir` with a relative compiler path; `// expect-build-ok` builds a lib case, expecting
# `pkg build` to succeed with no linked binary in build/; `// expect-build-err:
# substring` expects `pkg build` on an app case to fail with that diagnostic
# (e.g. a native link-input conflict, Package System #14 — checking alone
# never reaches the link step); `// expect-run-err: substring` expects
# `pkg run` on a lib case to fail with that diagnostic; `// expect-check-ok`
# expects `pkg check` to succeed from a nested working directory (src/)
# rather than the package root. `// pkg-add-args: NAME VERSION` runs
# `ace pkg add NAME VERSION` before any of the above, so a case can prove the
# added dependency actually builds/links; `// expect-add-err: NAME VERSION
# substring` and `// expect-remove-err: NAME substring` run `pkg add`/`pkg
# remove` expecting failure with that diagnostic substring and an unchanged
# manifest (spec/queue.md Decision Queue, "Package system v1": failure leaves the
# manifest byte-for-byte unchanged); `// expect-remove-ok: NAME` runs `pkg
# remove NAME` expecting success and the entry gone from ace.pkg; `//
# expect-list-has: substring` (repeatable) runs `pkg list` expecting success
# and every listed substring present in its output. `// expect-host: OS`
# skips the case unless `uname -s` prints OS (e.g. Darwin for a framework).
if [ -d tests/package_cli ]; then
    for case_dir in tests/package_cli/*/; do
        if [ -f "${case_dir}ace.pkg" ]; then dir=${case_dir%/}
        elif [ -f "${case_dir}app/ace.pkg" ]; then dir="${case_dir}app"
        else continue
        fi
        f="$dir/ace.pkg"
        host=$(sed -n 's/^\/\/ *expect-host:[[:space:]]*//p' "$f")
        [ -n "$host" ] && [ "$host" != "$(uname -s)" ] && continue
        name=$(sed -n 's/^name: *//p' "$f")
        build_exit=$(sed -n 's/^\/\/ *expect-build-exit:[[:space:]]*//p' "$f")
        run_args=$(sed -n 's/^\/\/ *run-args:[[:space:]]*//p' "$f")
        build_ok=$(sed -n 's/^\/\/ *expect-build-ok[[:space:]]*.*/yes/p' "$f")
        build_err=$(sed -n 's/^\/\/ *expect-build-err:[[:space:]]*//p' "$f")
        run_err=$(sed -n 's/^\/\/ *expect-run-err:[[:space:]]*//p' "$f")
        check_ok=$(sed -n 's/^\/\/ *expect-check-ok[[:space:]]*.*/yes/p' "$f")
        pkg_add_args=$(sed -n 's/^\/\/ *pkg-add-args:[[:space:]]*//p' "$f")
        add_err=$(sed -n 's/^\/\/ *expect-add-err:[[:space:]]*//p' "$f")
        remove_ok=$(sed -n 's/^\/\/ *expect-remove-ok:[[:space:]]*//p' "$f")
        remove_err=$(sed -n 's/^\/\/ *expect-remove-err:[[:space:]]*//p' "$f")
        list_has=$(sed -n 's/^\/\/ *expect-list-has:[[:space:]]*//p' "$f")

        if [ -n "$pkg_add_args" ]; then
            add_name=$(printf '%s\n' "$pkg_add_args" | awk '{print $1}')
            add_version=$(printf '%s\n' "$pkg_add_args" | awk '{print $2}')
            if ! (cd "$dir" && "$ACE" pkg add "$add_name" "$add_version") >/tmp/ace_test_errs 2>&1; then
                echo "FAIL (pkg add should succeed): $dir"
                cat /tmp/ace_test_errs
                fail=1
            fi
        fi
        if [ -n "$add_err" ]; then
            add_err_name=$(printf '%s\n' "$add_err" | awk '{print $1}')
            add_err_version=$(printf '%s\n' "$add_err" | awk '{print $2}')
            add_err_msg=$(printf '%s\n' "$add_err" | cut -d' ' -f3-)
            before=$(cat "$f")
            add_out=$(cd "$dir" && "$ACE" pkg add "$add_err_name" "$add_err_version" 2>&1)
            got=$?
            after=$(cat "$f")
            if [ "$got" -eq 0 ]; then
                echo "FAIL (pkg add should fail): $dir"
                fail=1
            elif ! printf '%s\n' "$add_out" | grep -qF "$add_err_msg"; then
                echo "FAIL: $dir missing 'pkg add' diagnostic '$add_err_msg'"
                printf '%s\n' "$add_out"
                fail=1
            elif [ "$before" != "$after" ]; then
                echo "FAIL: $dir 'pkg add' should leave the manifest unchanged on failure"
                fail=1
            fi
        fi
        if [ -n "$remove_ok" ]; then
            if ! (cd "$dir" && "$ACE" pkg remove "$remove_ok") >/tmp/ace_test_errs 2>&1; then
                echo "FAIL (pkg remove should succeed): $dir"
                cat /tmp/ace_test_errs
                fail=1
            elif grep -q "^${remove_ok}:" "$f"; then
                echo "FAIL: $dir manifest still contains removed dependency '$remove_ok'"
                fail=1
            fi
        fi
        if [ -n "$remove_err" ]; then
            rm_err_name=$(printf '%s\n' "$remove_err" | awk '{print $1}')
            rm_err_msg=$(printf '%s\n' "$remove_err" | cut -d' ' -f2-)
            before=$(cat "$f")
            rm_out=$(cd "$dir" && "$ACE" pkg remove "$rm_err_name" 2>&1)
            got=$?
            after=$(cat "$f")
            if [ "$got" -eq 0 ]; then
                echo "FAIL (pkg remove should fail): $dir"
                fail=1
            elif ! printf '%s\n' "$rm_out" | grep -qF "$rm_err_msg"; then
                echo "FAIL: $dir missing 'pkg remove' diagnostic '$rm_err_msg'"
                printf '%s\n' "$rm_out"
                fail=1
            elif [ "$before" != "$after" ]; then
                echo "FAIL: $dir 'pkg remove' should leave the manifest unchanged on failure"
                fail=1
            fi
        fi
        if [ -n "$list_has" ]; then
            list_out=$(cd "$dir" && "$ACE" pkg list 2>&1)
            got=$?
            if [ "$got" -ne 0 ]; then
                echo "FAIL (pkg list should succeed): $dir"
                printf '%s\n' "$list_out"
                fail=1
            else
                while IFS= read -r want; do
                    [ -z "$want" ] && continue
                    if ! printf '%s\n' "$list_out" | grep -qF "$want"; then
                        echo "FAIL: $dir 'pkg list' missing '$want'"
                        printf '%s\n' "$list_out"
                        fail=1
                    fi
                done <<EOF
$list_has
EOF
            fi
        fi

        rm -rf "$dir/build"
        if [ -n "$build_exit" ]; then
            if ! (cd "$dir" && "$ACE" pkg build --debug) >/dev/null 2>/tmp/ace_test_errs; then
                echo "FAIL (pkg build should succeed): $dir"
                cat /tmp/ace_test_errs
                fail=1
            else
                "$dir/build/$name" $run_args
                got=$?
                if [ "$got" -ne "$build_exit" ]; then
                    echo "FAIL: $dir/build/$name exited $got, want $build_exit"
                    fail=1
                fi
                (cd "$dir" && "$ACE" pkg run --debug -- $run_args) >/dev/null 2>/tmp/ace_test_errs
                got=$?
                if [ "$got" -ne "$build_exit" ]; then
                    echo "FAIL: $dir 'pkg run' exited $got, want $build_exit"
                    cat /tmp/ace_test_errs
                    fail=1
                fi
                # A relative compiler path proves std/ is still found after the chdir.
                case_abs="$PWD/$dir"
                (cd "$(dirname "$ACE")" && "./$(basename "$ACE")" pkg run --debug --dir="$case_abs" -- $run_args) \
                    >/dev/null 2>/tmp/ace_test_errs
                got=$?
                if [ "$got" -ne "$build_exit" ]; then
                    echo "FAIL: $dir 'pkg run --dir' exited $got, want $build_exit"
                    cat /tmp/ace_test_errs
                    fail=1
                fi
            fi
        fi
        if [ -n "$build_ok" ]; then
            if ! (cd "$dir" && "$ACE" pkg build --debug) >/dev/null 2>/tmp/ace_test_errs; then
                echo "FAIL (pkg build should succeed): $dir"
                cat /tmp/ace_test_errs
                fail=1
            elif [ ! -d "$dir/build" ] || [ -e "$dir/build/$name" ]; then
                echo "FAIL: $dir lib build should create an empty build/ with no linked binary"
                fail=1
            fi
        fi
        if [ -n "$build_err" ]; then
            build_out=$(cd "$dir" && "$ACE" pkg build --debug 2>&1)
            got=$?
            if [ "$got" -eq 0 ]; then
                echo "FAIL (pkg build should fail): $dir"
                fail=1
            elif ! printf '%s\n' "$build_out" | grep -qF "$build_err"; then
                echo "FAIL: $dir missing 'pkg build' diagnostic '$build_err'"
                printf '%s\n' "$build_out"
                fail=1
            fi
        fi
        if [ -n "$run_err" ]; then
            run_out=$(cd "$dir" && "$ACE" pkg run --debug 2>&1)
            got=$?
            if [ "$got" -eq 0 ]; then
                echo "FAIL (pkg run on a lib should fail): $dir"
                fail=1
            elif ! printf '%s\n' "$run_out" | grep -qF "$run_err"; then
                echo "FAIL: $dir missing 'pkg run' diagnostic '$run_err'"
                printf '%s\n' "$run_out"
                fail=1
            fi
        fi
        if [ -n "$check_ok" ]; then
            if ! (cd "$dir/src" && "$ACE" pkg check) >/dev/null 2>/tmp/ace_test_errs; then
                echo "FAIL (pkg check from a nested directory should succeed): $dir"
                cat /tmp/ace_test_errs
                fail=1
            fi
        fi
        rm -rf "$dir/build"
        # pkg-add-args mutates the checked-in manifest in place (unlike
        # build/, ace.pkg is a tracked source file); restore it so repeated
        # runs stay idempotent rather than hitting "duplicate dependency" on
        # the second run.
        if [ -n "$pkg_add_args" ]; then
            (cd "$dir" && "$ACE" pkg remove "$add_name") >/dev/null 2>&1
        fi
        if [ -z "$build_exit" ] && [ -z "$build_ok" ] && [ -z "$build_err" ] && [ -z "$run_err" ] &&
           [ -z "$check_ok" ] && [ -z "$add_err" ] && [ -z "$remove_ok" ] && [ -z "$remove_err" ] &&
           [ -z "$list_has" ]; then
            echo "FAIL: $f has no expect-*/pkg-add-args marker comment"
            fail=1
        fi
    done
fi

# IR lowering: --ir prints the root file's functions as tests/ir/NAME.ir
# (debug) and NAME.release.ir (release) expect. Emit fixtures build in debug,
# so every one that checks must also lower and verify in release.
for f in tests/ir/*.ace; do
    for mode in debug release; do
        want=${f%.ace}.ir
        [ "$mode" = release ] && want=${f%.ace}.release.ir
        if ! "$ACE" --$mode --ir $(target_of "$f") "$f" >/tmp/ace_ir_out 2>&1 || ! cmp -s /tmp/ace_ir_out "$want"; then
            echo "FAIL: --$mode --ir $f differs from $want"
            fail=1
        fi
    done
done
for f in $(find tests/emit -name '*.ace' | sort); do
    "$ACE" "$f" >/dev/null 2>&1 || continue # a companion file, not a root
    if ! "$ACE" --release --ir "$f" >/dev/null 2>/tmp/ace_test_errs; then
        echo "FAIL: --release --ir $f: $(head -1 /tmp/ace_test_errs)"
        fail=1
    fi
done

wait
for name in checked modules driver; do
    cat "/tmp/ace_sections/$name.log"
    [ "$(cat "/tmp/ace_sections/$name/fail" 2>/dev/null)" = 0 ] || fail=1
done

if [ $fail -eq 0 ]; then
    echo "\n--------------------"
    echo "all tests passed"
fi
exit $fail
