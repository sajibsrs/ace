#!/bin/sh
# Differential check of the IR backend. Generates random programs of scalar
# functions (every integer type, bool, casts, arithmetic, bitwise, shift and
# comparison operators, comparison chains, && and ||, if/else, general for
# loops, labeled break/continue and blocks, early returns, and calls between
# them) and memory (globals, a struct global and its mut method, mut
# parameters, pointers to variables, function values, and range loops with
# steps in both directions), and floats (f32 and f64 locals, parameters past
# the eight argument registers, results, globals, arithmetic, NaN-producing
# division, comparisons, and conversions both ways), builds each with two
# compilers in debug and release, and requires identical output and exit
# status. main prints each function's result for several arguments (a float
# as its bits); a debug arithmetic trap ends the run the same way in both.
# Run a change against a build of HEAD:
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

function isfloat(t) { return t == "f32" || t == "f64" }

# any value type: mostly integers and bool, sometimes a float
function anytype() { return chance(0.25) ? (chance(0.5) ? "f32" : "f64") : types[1 + pick(ntypes)] }

function fliteral(t,   n) {
    n = split("0.0 -0.0 0.5 1.0 -1.5 0.1 3.0 100.0 65536.25 -2.5e-3 1.0e10 1.0e300", flits, " ")
    return t "(" flits[1 + pick(t == "f32" ? n - 1 : n)] ")"
}

# expression e of type et converted to t; a float reaches a narrow integer
# through i64, since the AST emitter leaves a narrow fcvtzs result unwrapped
function conv(t, e, et) {
    if (isfloat(et) && !isfloat(t) && bits(t) < 32) return t "(i64(" e "))"
    return t "(" e ")"
}

function fleaf(t, depth,   v, r, s) {
    r = rand()
    v = var_of(t)
    if (v != "" && r < 0.45) return v
    if (r < 0.65) {
        s = any_var()
        if (s != "") {
            split(s, part, SUBSEP)
            if (part[2] != "bool") return conv(t, part[1], part[2])
        }
    }
    if (v != "" && r < 0.72) return "((" v " - " v ") / (" v " - " v "))" # NaN, or 0 / 0 of an infinity
    if (nfuncs > 0 && depth > 0 && r < 0.82) return call_conv(t)
    return fliteral(t)
}

function fexpr(t, depth,   r, a, b) {
    r = rand()
    if (depth <= 0 || r < 0.3) return fleaf(t, depth)
    a = fexpr(t, depth - 1)
    b = fexpr(t, depth - 1)
    if (r < 0.75) return "(" a " " substr("+-*/", 1 + pick(4), 1) " " b ")"
    if (r < 0.85) return "-(" a ")"
    return t "(" fexpr(t == "f32" ? "f64" : "f32", depth - 1) ")"
}

# a call of a random function, converted to t
function call_conv(t,   j) {
    j = pick(nfuncs)
    return conv(t, "f" j "(" args_of(j, 0) ")", fret[j])
}

function literal(t,   m) {
    if (isfloat(t)) return fliteral(t)
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
            if (part[2] != "bool" && part[2] != t) return conv(t, part[1], part[2])
        }
    }
    if (t != "bool" && nfuncs > 0 && depth > 0 && r < 0.9) return call_conv(t)
    if (t != "bool" && depth > 0 && r < 0.93) return t "(mut gs.bump(" expr("i64", 0) "))"
    return t == "bool" ? literal(t) : t "(" literal(t) ")"
}

# the arguments of a call to fj: a mut parameter takes a variable
function args_of(j, lit,   k, s, t) {
    s = ""
    for (k = 0; k < nparams[j]; k++) {
        t = ptype[j, k]
        if (pmut[j, k]) s = s (k ? ", " : "") "mut " lvalue(t)
        else if (lit) s = s (k ? ", " : "") (t == "bool" || isfloat(t) ? literal(t) : t "(" literal(t) ")")
        else s = s (k ? ", " : "") expr(t, 0)
    }
    return s
}

# an assignable variable of type t (not through a pointer), else its global
function lvalue(t,   i, n, c) {
    n = 0
    for (i = 0; i < nvars; i++) if (vtype[i] == t && !vfixed[vname[i]] && vname[i] !~ /^\*/) c[n++] = vname[i]
    return n ? c[pick(n)] : "g_" t
}

# a bound of a range loop over t: small, so the loop stays short
function bound(t) {
    if (chance(0.3)) return "(" expr(t, 1) " & 7)"
    return t "(" (signed(t) ? pick(13) - 4 : pick(9)) ")"
}

function memory_stmt(depth, pad, loops,   p, r, t, i, name, lab, j, k, s) {
    p = indent(pad)
    r = rand()
    if (r < 0.35) { # a pointer to a variable, written and read through
        i = assignable()
        if (i < 0 || vname[i] ~ /^\*/) return
        t = vtype[i]
        name = "q" nlocals++
        print p name ": mut *" t " = mut &" vname[i]
        if (t == "bool" || isfloat(t)) print p "*" name " = " expr(t, 1)
        else print p "*" name " += (" expr(t, 1) " & 3)"
        declare("*" name, t)
        return
    }
    if (r < 0.55) { # a call through a function value
        j = pick(nfuncs + 1) - 1
        if (j < 0 || fmut[j]) return
        name = "h" nlocals++
        s = ""
        for (k = 0; k < nparams[j]; k++) s = s (k ? ", " : "") ptype[j, k]
        print p name ": (" s "): " fret[j] " = f" j
        print p "v" nlocals ": " fret[j] " = " name "(" args_of(j, 0) ")"
        declare("v" nlocals++, fret[j])
        return
    }
    t = types[2 + pick(ntypes - 1)]
    name = "k" nlocals++
    lab = "l" nlabels++
    print p lab ": for " name ": " t " in " bound(t) " .. " bound(t) (chance(0.3) ? " step " (1 + pick(3)) : "") " {"
    declare(name, t)
    vfixed[name] = 1
    block(depth - 1, pad + 1, loops == "" ? lab : loops " " lab)
    nvars--
    print p "}"
}

function compare(depth,   t, n, s, i, ops) {
    t = chance(0.3) ? (chance(0.5) ? "f32" : "f64") : types[2 + pick(ntypes - 1)] # not bool
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
    if (isfloat(t)) return fexpr(t, depth)
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
    if (depth > 0 && chance(0.18)) { memory_stmt(depth, pad, loops); return }
    r = rand()
    if (depth <= 0 || r < 0.3) {
        t = anytype()
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
        if (isfloat(t)) { print p vname[i] " " substr("+-*/", 1 + pick(4), 1) "= " expr(t, 1); return }
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
        print p "if " boolean(1) " { return " result(1) " }"
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

# a return value of the current function
function result(depth,   t) {
    t = chance(0.5) ? (chance(0.5) ? "f32" : "f64") : types[2 + pick(ntypes - 1)]
    if (t == rett) return expr(t, depth)
    return conv(rett, expr(t, depth), t)
}

# fmt.printf of value e of type t: a float as its exact bits
function show(e, t) {
    if (isfloat(t)) return "    fmt.printf(\"%u\\n\", uint(bits.cast(" e ")))"
    return "    fmt.printf(\"%d\\n\", int(" e "))"
}

BEGIN {
    srand(seed * 1000003 + n)
    ntypes = split("bool i8 i16 i32 i64 u8 u16 u32 u64 usize int", types, " ")
    # bool first: types[2..] are the integers
    print "import bits\nimport fmt\n"
    for (k = 1; k <= ntypes; k++) print "g_" types[k] ": " types[k] " = " literal(types[k])
    print "g_f32: f32 = " fliteral("f32") "\ng_f64: f64 = " fliteral("f64")
    print "\nS: struct {\n    a: i64,\n    b: u8,\n}\n"
    print "gs: S = S { a = " literal("i64") ", b = " literal("u8") " }\n"
    print "mut S.bump(n: i64): int {\n    self.a += n & 7\n    self.b = u8(self.a & 63)\n    return int(self.b)\n}\n"
    nf = 2 + pick(4)
    for (f = 0; f < nf; f++) {
        nvars = 0
        nlocals = 0
        nlabels = 0
        np = chance(0.15) ? 9 + pick(4) : 1 + pick(4) # past the argument registers, at times
        rett = chance(0.3) ? (chance(0.5) ? "f32" : "f64") : "int"
        fret[f] = rett
        s = "f" f "("
        for (k = 0; k < np; k++) {
            t = anytype()
            ptype[f, k] = t
            pmut[f, k] = chance(0.2)
            if (pmut[f, k]) fmut[f] = 1
            s = s (k ? ", " : "") (pmut[f, k] ? "mut " : "") "p" k ": " t
            declare("p" k, t)
            if (!pmut[f, k]) vfixed["p" k] = 1
        }
        for (k = 1; k <= ntypes; k++) if (chance(0.25)) declare("g_" types[k], types[k])
        if (chance(0.3)) declare("g_f32", "f32")
        if (chance(0.3)) declare("g_f64", "f64")
        if (chance(0.3)) declare("gs.a", "i64")
        if (chance(0.3)) declare("gs.b", "u8")
        print s "): " rett " {"
        block(3, 1, "")
        print "    return " result(2)
        print "}\n"
        nparams[f] = np
        nfuncs = f + 1
        delete vfixed
    }
    nvars = 0
    print "main(): int {"
    for (f = 0; f < nf; f++) {
        for (c = 0; c < 3; c++) {
            print show("f" f "(" args_of(f, 1) ")", fret[f])
        }
    }
    print "    fmt.printf(\"%d %d\\n\", int(gs.a), int(gs.b))"
    for (k = 2; k <= ntypes; k++) print "    fmt.printf(\"%d\\n\", int(g_" types[k] "))"
    print show("g_f32", "f32")
    print show("g_f64", "f64")
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
