#!/bin/sh
# Differential check of the IR backend. Generates random programs of scalar
# functions (every integer type, bool, casts, arithmetic, bitwise, shift and
# comparison operators, comparison chains, && and ||, if/else, general for
# loops, labeled break/continue and blocks, early returns, and calls between
# them), builds each with two compilers in debug and release, and requires
# identical output and exit status. main prints each function's result for
# several arguments; a debug arithmetic trap ends the run the same way in
# both. Run a change against a build of HEAD:
#   tests/ir-diff.sh <baseline-ace> <candidate-ace> [count] [seed]
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 <baseline-ace> <candidate-ace> [count] [seed]" >&2
    exit 2
fi
old=$1
new=$2
count=${3:-200}
seed=${4:-1}
for ace in "$old" "$new"; do
    if [ ! -f "$ace" ] || [ ! -x "$ace" ]; then
        echo "$0: not an executable compiler: $ace" >&2
        exit 2
    fi
done
dir=$(mktemp -d "${TMPDIR:-/tmp}/ace-ir-diff.XXXXXX")
trap 'rm -rf "$dir"' EXIT HUP INT TERM

generator='
function pick(n) { return int(rand() * n) }
function chance(p) { return rand() < p }

function indent(n,   s, i) {
    s = ""
    for (i = 0; i < n; i++) s = s "    "
    return s
}

function signed(t) { return t ~ /^i/ || t == "int" }
function bits(t) {
    if (t == "i8" || t == "u8") return 8
    if (t == "i16" || t == "u16") return 16
    if (t == "i32" || t == "u32") return 32
    return 64
}

function literal(t,   m) {
    if (t == "bool") return chance(0.5) ? "true" : "false"
    m = bits(t) == 8 ? 20 : bits(t) == 16 ? 3000 : bits(t) == 32 ? 100000 : 5000000
    if (chance(0.3)) m = 10
    if (signed(t)) return pick(2 * m + 1) - m
    return pick(2 * m + 1)
}

# a variable of type t in scope, or ""
function var_of(t,   i, n, c) {
    n = 0
    for (i = 0; i < nvars; i++) if (vtype[i] == t) c[n++] = vname[i]
    return n ? c[pick(n)] : ""
}

function any_var(   i) {
    if (!nvars) return ""
    i = pick(nvars)
    return vname[i] SUBSEP vtype[i]
}

function leaf(t, depth,   v, r, s) {
    r = rand()
    v = var_of(t)
    if (v != "" && r < 0.55) return v
    if (t != "bool" && r < 0.8) {
        s = any_var()
        if (s != "") {
            split(s, part, SUBSEP)
            if (part[2] != "bool" && part[2] != t) return t "(" part[1] ")"
        }
    }
    if (t != "bool" && nfuncs > 0 && depth > 0 && r < 0.9) return t "(" call_expr() ")"
    return t == "bool" ? literal(t) : t "(" literal(t) ")"
}

function call_expr(   j, k, s) {
    j = pick(nfuncs)
    s = "f" j "("
    for (k = 0; k < nparams[j]; k++) s = s (k ? ", " : "") expr(ptype[j, k], 0)
    return s ")"
}

function compare(depth,   t, n, s, i, ops) {
    t = types[2 + pick(ntypes - 1)] # not bool
    split("< <= > >= == !=", ops, " ")
    if (chance(0.2)) { # a comparison chain
        n = 3
        s = expr(t, depth - 1)
        for (i = 1; i < n; i++) s = s " " ops[1 + pick(4)] " " expr(t, depth - 1)
        return s
    }
    return expr(t, depth - 1) " " ops[1 + pick(6)] " " expr(t, depth - 1)
}

function boolean(depth,   r) {
    r = rand()
    if (depth <= 0 || r < 0.2) return leaf("bool", depth)
    if (r < 0.6) return "(" compare(depth) ")"
    if (r < 0.75) return "!(" boolean(depth - 1) ")"
    return "(" boolean(depth - 1) (chance(0.5) ? " && " : " || ") boolean(depth - 1) ")"
}

function mask(t) { return bits(t) == 8 ? 31 : bits(t) == 16 ? 4095 : bits(t) == 32 ? 16777215 : 1099511627775 }
function small(t) { return bits(t) == 8 ? 7 : bits(t) == 16 ? 1023 : bits(t) == 32 ? 1048575 : 68719476735 }

function expr(t, depth,   r, a, b, op, m) {
    if (t == "bool") return boolean(depth)
    r = rand()
    if (depth <= 0 || r < 0.3) return leaf(t, depth)
    a = expr(t, depth - 1)
    b = expr(t, depth - 1)
    if (r < 0.45 && chance(0.8)) { # masked operands: debug checks rarely fire
        m = mask(t)
        if (chance(0.5)) return "((" a " & " m ") + (" b " & " m "))"
        if (signed(t)) return "((" a " & " m ") - (" b " & " m "))"
        return "(((" a " & " m ") | " (m + 1) ") - (" b " & " m "))"
    }
    if (r < 0.45) return "(" a (chance(0.5) ? " + " : " - ") b ")"
    if (r < 0.55 && chance(0.8)) return "((" a " & " small(t) ") * " pick(10) ")"
    if (r < 0.55) return "(" a " * " (chance(0.5) ? pick(10) : b) ")"
    if (r < 0.62) { # a divisor made nonzero, mostly
        op = chance(0.5) ? " / " : " % "
        return "(" a op (chance(0.9) ? "(" b " | 1)" : b) ")"
    }
    if (r < 0.72) {
        op = chance(0.5) ? " << " : " >> "
        return "(" a op (chance(0.9) ? "(" b " & " (bits(t) - 1) ")" : b) ")"
    }
    if (r < 0.85) {
        op = substr("&|^", 1 + pick(3), 1)
        return "(" a " " op " " b ")"
    }
    if (r < 0.92 && signed(t)) return "-(" a ")"
    return "~(" a ")"
}

function declare(name, t) {
    vname[nvars] = name
    vtype[nvars] = t
    nvars++
}

function block(depth, pad, loops,   i, n, saved) {
    saved = nvars
    n = 1 + pick(4)
    for (i = 0; i < n; i++) stmt(depth, pad, loops)
    nvars = saved
}

function assignable(   i, n, c) {
    n = 0
    for (i = 0; i < nvars; i++) if (!vfixed[vname[i]]) c[n++] = i
    return n ? c[pick(n)] : -1
}

function stmt(depth, pad, loops,   p, r, t, name, i, lab, n, open, ops) {
    p = indent(pad)
    r = rand()
    if (depth <= 0 || r < 0.3) {
        t = types[1 + pick(ntypes)]
        name = "v" nlocals++
        print p name ": " t " = " expr(t, 2)
        declare(name, t)
        return
    }
    if (r < 0.5) {
        i = assignable()
        if (i < 0) return
        t = vtype[i]
        if (t == "bool" || chance(0.5)) { print p vname[i] " = " expr(t, 2); return }
        split("+= -= *= /= %= &= |= ^= <<= >>=", ops, " ")
        n = 1 + pick(10)
        if (n == 3) { print p vname[i] " *= " pick(4); return }
        if (n == 4 || n == 5) { print p vname[i] " " ops[n] " (" expr(t, 1) " | 1)"; return }
        if (n >= 9) { print p vname[i] " " ops[n] " (" expr(t, 1) " & " (bits(t) - 1) ")"; return }
        if (n <= 2 && chance(0.8)) { print p vname[i] " " ops[n] " (" expr(t, 1) " & 3)"; return }
        print p vname[i] " " ops[n] " " expr(t, 1)
        return
    }
    if (r < 0.55) {
        print p "if " boolean(1) " { return int(" expr(types[2 + pick(ntypes - 1)], 1) ") }"
        return
    }
    if (r < 0.62 && loops != "") {
        n = split(loops, open, " ")
        lab = open[1 + pick(n)]
        print p "if " boolean(1) " { " (chance(0.5) || lab ~ /^b/ ? "break" : "continue") " " lab " }"
        return
    }
    if (r < 0.75) {
        print p "if " boolean(2) " {"
        block(depth - 1, pad + 1, loops)
        if (chance(0.6)) {
            print p "} else {"
            block(depth - 1, pad + 1, loops)
        }
        print p "}"
        return
    }
    if (r < 0.92) {
        t = types[2 + pick(ntypes - 1)]
        name = "k" nlocals++
        lab = "l" nlabels++
        print p lab ": for " name ": " t " = 0; " name " < " (1 + pick(4)) "; " name " += 1 {"
        declare(name, t)
        vfixed[name] = 1
        block(depth - 1, pad + 1, loops == "" ? lab : loops " " lab)
        nvars--
        print p "}"
        return
    }
    lab = "b" nlabels++
    print p lab ": {"
    block(depth - 1, pad + 1, loops == "" ? lab : loops " " lab)
    print p "}"
}

BEGIN {
    srand(seed * 1000003 + n)
    ntypes = split("bool i8 i16 i32 i64 u8 u16 u32 u64 usize int", types, " ")
    # bool first: types[2..] are the integers
    print "import fmt\n"
    nf = 2 + pick(4)
    for (f = 0; f < nf; f++) {
        nvars = 0
        nlocals = 0
        nlabels = 0
        np = 1 + pick(4)
        s = "f" f "("
        for (k = 0; k < np; k++) {
            t = types[1 + pick(ntypes)]
            ptype[f, k] = t
            s = s (k ? ", " : "") "p" k ": " t
            declare("p" k, t)
            vfixed["p" k] = 1
        }
        print s "): int {"
        block(3, 1, "")
        print "    return int(" expr(types[2 + pick(ntypes - 1)], 2) ")"
        print "}\n"
        nparams[f] = np
        nfuncs = f + 1
        delete vfixed
    }
    print "main(): int {"
    for (f = 0; f < nf; f++) {
        for (c = 0; c < 3; c++) {
            s = "f" f "("
            for (k = 0; k < nparams[f]; k++) s = s (k ? ", " : "") (ptype[f, k] == "bool" ? literal("bool") : ptype[f, k] "(" literal(ptype[f, k]) ")")
            print "    fmt.printf(\"%d\\n\", " s "))"
        }
    }
    print "    return 0"
    print "}"
}'

n=0
lowered=0
funcs=0
trapped=0
mismatched=0
while [ "$n" -lt "$count" ]; do
    awk -v seed="$seed" -v n="$n" "$generator" > "$dir/case.ace"
    if ! "$old" "$dir/case.ace" > "$dir/check" 2>&1; then
        echo "FAIL: program $n does not check under $old:" >&2
        head -3 "$dir/check" >&2
        cp "$dir/case.ace" "${TMPDIR:-/tmp}/ace-ir-diff-bad.ace"
        exit 1
    fi
    "$new" --ir "$dir/case.ace" > "$dir/ir"
    funcs=$((funcs + $(grep -c '^f[0-9]*(\|^; f[0-9]*:' "$dir/ir" || true)))
    lowered=$((lowered + $(grep -c '^f[0-9]*(' "$dir/ir" || true)))
    same=1
    for mode in debug release; do
        for side in old new; do
            eval ace=\$$side
            rc=0
            "$ace" --$mode -o "$dir/$side" "$dir/case.ace" > "$dir/$side.out" 2>&1 || rc=$?
            if [ "$rc" -eq 0 ]; then
                "$dir/$side" > "$dir/$side.out" 2>&1 || rc=$?
            else
                rc=compile$rc
            fi
            echo "exit $rc" >> "$dir/$side.out"
        done
        if [ "$mode" = debug ] && grep -q 'arithmetic violation' "$dir/old.out"; then
            trapped=$((trapped + 1))
        fi
        if ! cmp -s "$dir/old.out" "$dir/new.out"; then
            same=0
            echo "mismatch: program $n, $mode" >&2
        fi
    done
    if [ "$same" -eq 0 ]; then
        mismatched=$((mismatched + 1))
        if [ "$mismatched" -eq 1 ]; then
            keep=$(mktemp -d "${TMPDIR:-/tmp}/ace-ir-diff-mismatch.XXXXXX")
        fi
        cp "$dir/case.ace" "$keep/case_$n.ace"
    fi
    n=$((n + 1))
done

echo "programs $count (seed $seed), functions lowered $lowered of $funcs, debug traps $trapped, mismatches $mismatched"
if [ "$mismatched" -ne 0 ]; then
    echo "mismatching programs kept in $keep" >&2
    exit 1
fi
