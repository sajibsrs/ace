#!/bin/sh
# Differential check of the IR backend. Generates random programs of scalar
# functions (every integer type, bool, casts, arithmetic, bitwise, shift and
# comparison operators, comparison chains, && and ||, if/else, general for
# loops, labeled break/continue and blocks, early returns, and calls between
# them) and memory (globals, a struct global and its mut method, mut
# parameters, pointers to variables, function values, and range loops with
# steps in both directions), and floats (f32 and f64 locals, parameters past
# the eight argument registers, results, globals, arithmetic, NaN-producing
# division, comparisons, and conversions both ways), and records (random
# structs: floating aggregates, words shared by several fields, ones past 16
# bytes, nested; as locals, parameters, results, literals, fields, and
# copies) and (T, Error) results destructured, and strings (literals, null,
# .len, null checks, []u8 views; as locals, parameters, results, and fields),
# and calls to generic templates and a written specialization, directly and
# as values, and indexing and slicing (reads, writes, ^=, &, every slice form;
# indices and bounds mostly in range, at times one past either end) of a
# 22-byte mut []u8, a string, and a [22]u8 struct field, each a window
# between two guard bytes (and in-range reads of a local copy of the array), so a
# debug bounds trap ends the run the same way in both and release reads or
# writes a guard. Some scalar-result functions are inline, so their calls
# expand in place, with every parameter form. Scalar switches (literal,
# variable, and computed patterns, several to a case, fallthrough, and
# default) sit among the statements, and some functions defer notes of their
# parameters, and tagged unions (one nested, one of over 16 bytes) are built,
# passed, returned, copied, compared, and switched on (with a binder and a
# default), and held in a struct member and an array, and calls with a noalias slice
# parameter compare their slice windows, and for-in loops read and change them. A labeled block restarts, a computed
# global constant is read, and an Error's name and message are looked up. Some functions bind an
# address-taken local in an asm statement (a host register), read from its home. Each
# program is built with two
# compilers in debug and release, and requires identical output and exit
# status. main prints each function's result for several arguments (a float
# as its bits); a debug arithmetic or bounds trap ends the run the same way
# in both.
# Run a change against a build of HEAD:
#   tests/ir-diff.sh <baseline-ace> <candidate-ace> [count] [seed]
set -eu
asm_reg=x9
[ "$(uname -s)-$(uname -m)" = Linux-x86_64 ] && asm_reg=r9

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


function fleaf(t, depth,   v, r, s) {
    r = rand()
    v = var_of(t)
    if (v != "" && r < 0.45) return v
    if (r < 0.65) {
        s = any_var()
        if (s != "") {
            split(s, part, SUBSEP)
            if (part[2] != "bool") return t "(" part[1] ")"
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

# a call of a random function with a scalar result, converted to t
function call_conv(t,   j, i, n, c) {
    n = 0
    for (i = 0; i < nfuncs; i++) if (!isrec(fret[i]) && fret[i] !~ /^\(/ && fret[i] != "string") c[n++] = i
    if (!n) return isfloat(t) ? fliteral(t) : t "(" literal(t) ")"
    j = c[pick(n)]
    return t "(f" j "(" args_of(j, 0) "))"
}

function isrec(t) { return t ~ /^R/ }
function recno(t) { return substr(t, 2) + 0 }

# a value of record type t: a variable, a literal, or a call
function rexpr(t, depth,   r, v, s, i, j, n, c) {
    r = rand()
    v = var_of(t)
    if (v != "" && r < 0.5) return v
    if (depth > 0 && r < 0.75) {
        n = 0
        for (i = 0; i < nfuncs; i++) if (fret[i] == t) c[n++] = i
        if (n) {
            j = c[pick(n)]
            return "f" j "(" args_of(j, 0) ")"
        }
    }
    s = t " {"
    for (i = 0; i < nfld[recno(t)]; i++) s = s (i ? ", " : " ") "f" i " = " anyexpr(ftype[recno(t), i], depth - 1)
    return s " }"
}

function anyexpr(t, depth) { return isrec(t) ? rexpr(t, depth) : t == "string" ? sexpr(depth) : expr(t, depth) }

# a string: a variable or field, a literal, null, a call, a []u8 view and back
function sexpr(depth,   r, v, i, j, n, c) {
    r = rand()
    v = var_of("string")
    if (v != "" && r < 0.35) return v
    if (r < 0.45) {
        v = field_of("string")
        if (v != "") return v
    }
    if (depth > 0 && r < 0.6) {
        n = 0
        for (i = 0; i < nfuncs; i++) if (fret[i] == "string") c[n++] = i
        if (n) {
            j = c[pick(n)]
            return "f" j "(" args_of(j, 0) ")"
        }
    }
    if (depth > 0 && r < 0.7) return "gpick[string](" sexpr(depth - 1) ", " sexpr(depth - 1) ", " boolean(0) ")"
    if (depth > 0 && r < 0.75) return "string([]u8(" (v != "" ? v : strs[1 + pick(nstrs)]) "))"
    if (r < 0.82) return "null"
    return strs[1 + pick(nstrs)]
}

# a scalar field of type t of a record variable in scope, or ""
function field_of(t,   i, k, n, c) {
    n = 0
    for (i = 0; i < nvars; i++) {
        if (!isrec(vtype[i])) continue
        for (k = 0; k < nfld[recno(vtype[i])]; k++) if (ftype[recno(vtype[i]), k] == t) c[n++] = vname[i] ".f" k
    }
    return n ? c[pick(n)] : ""
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

# a scalar variable and its type, or ""
function any_var(   i, n, c) {
    n = 0
    for (i = 0; i < nvars; i++) if (!isrec(vtype[i]) && vtype[i] != "string") c[n++] = i
    if (!n) return ""
    i = c[pick(n)]
    return vname[i] SUBSEP vtype[i]
}

# an index into a 22-element window at a random integer type: usually in
# range, at times one past the end or -1
function index_of(depth,   r, e, t) {
    r = rand()
    e = "u8(" expr(types[2 + pick(ntypes - 1)], depth) ")"
    if (r < 0.05) return "(i8(" e " % 2) - 1)"
    t = types[2 + pick(ntypes - 1)]
    return t "(" e " % " (r < 0.2 ? 23 : 22) ")"
}

# a window (the mut []u8 gv or the string gt) sliced from lo, a u8 local
# below 12: every form, lo at times -1 and hi at times one past the end
# a writable 22-byte window: the mut []u8 gv, or the array ga.a
function window() { return chance(0.5) ? "gv" : "ga.a" }

function slice_of(lo, depth,   w, r, t, hi) {
    r = rand()
    w = r < 0.35 ? "gv" : r < 0.7 ? "gt" : "ga.a"
    r = rand()
    if (r < 0.1) return w "[..]"
    if (r < 0.2) return w "[.." lo "]"
    t = types[2 + pick(ntypes - 1)]
    if (r < 0.3) return w "[" t "(" lo ")..]"
    if (r < 0.35) return w "[(i8(" lo " % 2) - 1).." lo "]"
    hi = "(" lo " + u8(" expr(types[2 + pick(ntypes - 1)], depth) ") % " (chance(0.15) ? 13 : 12) ")"
    return w "[" t "(" lo ").." t "(" hi ")]"
}

# a for-in loop over a window (the mut []u8 gv, the string gt, or the array
# ga.a, or a slice of one): each element copied to a binder, with or without
# the index, or (over a writable one) aliased and changed; it sums into a new
# local, and at times skips or leaves early
function forin_stmt(pad,   p, acc, w, r, c, i, k) {
    p = indent(pad)
    acc = "v" nlocals++
    print p acc ": int = 0"
    declare(acc, "int")
    c = "e" nlocals++
    i = "e" nlocals++
    k = pick(200)
    r = rand()
    if (r < 0.3) {
        print p "for " c ": u8 in mut " window() " {"
        print p "    " c " ^= u8(" k ")"
        print p "    " acc " += int(" c ")"
        print p "}"
        return
    }
    w = r < 0.45 ? "gt" : r < 0.6 ? "gv" : r < 0.75 ? "ga.a" : slice_of("u8(" pick(12) ")", 1)
    if (chance(0.5)) {
        print p "for " i ": usize, " c ": u8 in " w " {"
        print p "    " acc " += int(" c ") * int(" i " + 1)"
    } else {
        print p "for " c ": u8 in " w " {"
        print p "    " acc " += int(" c ")"
    }
    if (chance(0.3)) print p "    if int(" c ") > " k " { continue }"
    if (chance(0.3)) print p "    if int(" c ") == " k " { break }"
    print p "    " acc " += 1"
    print p "}"
}

function seq_stmt(pad,   p, r, name) {
    p = indent(pad)
    if (chance(0.35)) { forin_stmt(pad); return }
    r = rand()
    if (r < 0.3) { print p window() "[" index_of(1) "] = u8(" expr("u8", 1) ")"; return }
    if (r < 0.45) { print p window() "[" index_of(1) "] ^= " expr("u8", 1); return }
    name = "v" nlocals++
    if (r < 0.5) { # a local copy of the array, indexed in range
        print p name ": [22]u8 = ga.a"
        print p "v" nlocals ": u8 = " name "[u8(" expr(types[2 + pick(ntypes - 1)], 1) ") % 22]"
        declare("v" nlocals++, "u8")
        return
    }
    if (r < 0.55) {
        print p name ": u8 = *(&" (chance(0.3) ? "gt" : window()) "[" index_of(1) "])"
        declare(name, "u8")
        return
    }
    print p name ": u8 = u8(" expr(types[2 + pick(ntypes - 1)], 1) ") % 12"
    declare(name, "u8")
    print p "v" nlocals ": string = " slice_of(name, 1)
    declare("v" nlocals++, "string")
}

function leaf(t, depth,   v, r, s) {
    if (t == "u8" && depth > 0 && chance(0.15)) return (chance(0.3) ? "gt" : window()) "[" index_of(depth - 1) "]"
    r = rand()
    v = var_of(t)
    if (v != "" && r < 0.55) return v
    if (r < 0.62) {
        s = field_of(t)
        if (s != "") return s
    }
    if (t != "bool" && r < 0.8) {
        s = any_var()
        if (s != "") {
            split(s, part, SUBSEP)
            if (part[2] != "bool" && part[2] != t) return t "(" part[1] ")"
        }
    }
    if (t != "bool" && r < 0.84) {
        s = var_of("string")
        if (s == "") s = field_of("string")
        if (s != "" && chance(0.5)) return t "(" s ".len)"
    }
    if (t != "bool" && depth > 0 && r < 0.87) {
        if (chance(0.5)) return "gmax[" t "](" expr(t, depth - 1) ", " expr(t, depth - 1) ")"
        return "gpick[" t "](" expr(t, depth - 1) ", " expr(t, depth - 1) ", " boolean(depth - 1) ")"
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
        else if (isrec(t)) s = s (k ? ", " : "") rexpr(t, 0)
        else if (t == "string") s = s (k ? ", " : "") (lit ? strs[1 + pick(nstrs)] : sexpr(0))
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
    if (r < 0.45) { # a generic stamp or specialization as a value
        t = types[2 + pick(ntypes - 1)]
        name = "h" nlocals++
        print p name ": (" t ", " t ", bool): " t " = gpick[" t "]"
        print p "v" nlocals ": " t " = " name "(" expr(t, 1) ", " expr(t, 1) ", " boolean(1) ")"
        declare("v" nlocals++, t)
        return
    }
    if (r < 0.55) { # a call through a function value
        j = pick(nfuncs + 1) - 1
        if (j < 0 || fmut[j] || fret[j] ~ /^\(/) return
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

function boolean(depth,   r, a) {
    r = rand()
    if (r < 0.08) {
        a = var_of("string")
        if (a != "") return "(" a (chance(0.5) ? " == null" : " != null") ")"
    }
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

function assignable(   i, n, c) { # a scalar one
    n = 0
    for (i = 0; i < nvars; i++) if (!vfixed[vname[i]] && !isrec(vtype[i]) && vtype[i] != "string") c[n++] = i
    return n ? c[pick(n)] : -1
}

# a record local, a field or whole-record assignment, or a destructured call
function record_stmt(pad,   p, r, t, i, k, j, n, c, name) {
    p = indent(pad)
    r = rand()
    if (r < 0.4) {
        t = "R" pick(nrec)
        name = "v" nlocals++
        print p name ": " t " = " rexpr(t, 2)
        declare(name, t)
        return
    }
    n = 0
    for (i = 0; i < nvars; i++) if (isrec(vtype[i]) && !vfixed[vname[i]]) c[n++] = i
    if (r < 0.8 && n) {
        i = c[pick(n)]
        t = vtype[i]
        if (chance(0.3)) { print p vname[i] " = " rexpr(t, 1); return }
        k = pick(nfld[recno(t)])
        print p vname[i] ".f" k " = " anyexpr(ftype[recno(t), k], 1)
        return
    }
    n = 0
    for (j = 0; j < nfuncs; j++) if (fret[j] ~ /^\(/) c[n++] = j
    if (!n) return
    j = c[pick(n)]
    t = substr(fret[j], 2, index(fret[j], ",") - 2)
    name = "v" nlocals++
    print p name ": " t ", e" nlocals ": Error = f" j "(" args_of(j, 0) ")"
    declare(name, t)
    print p "v" nlocals + 1 ": i32 = e" nlocals ".code"
    declare("v" nlocals + 1, "i32")
    print p "v" nlocals + 2 ": bool = e" nlocals " != null"
    declare("v" nlocals + 2, "bool")
    nlocals += 3
}

# a switch on a fresh scalar local: each case has one to three patterns (a
# literal, a variable, an expression, at times the scrutinee itself, so some
# cases always match), a case that is not last may fall through into the next,
# and a default is optional; a break or continue in a case names an enclosing
# loop or block
function switch_stmt(depth, pad, loops,   p, q, t, name, ncases, hasdef, i, j, k, pats, r) {
    p = indent(pad)
    q = indent(pad + 1)
    t = anytype()
    name = "v" nlocals++
    print p name ": " t " = " expr(t, 2)
    declare(name, t)
    print p "switch " name " {"
    ncases = 1 + pick(4)
    hasdef = chance(0.6)
    for (i = 0; i < ncases; i++) {
        k = chance(0.7) ? 1 : 2 + pick(2)
        pats = ""
        for (j = 0; j < k; j++) {
            r = rand()
            pats = pats (j ? ", " : "") (r < 0.4 ? literal(t) : r < 0.6 ? (var_of(t) != "" ? var_of(t) : literal(t)) : r < 0.9 ? expr(t, 1) : name)
        }
        print q pats " {"
        block(depth - 1, pad + 2, loops)
        if ((i < ncases - 1 || hasdef) && chance(0.25)) print indent(pad + 2) "fallthrough"
        print q "}"
    }
    if (hasdef) {
        print q "default {"
        block(depth - 1, pad + 2, loops)
        print q "}"
    }
    print p "}"
}

function stmt(depth, pad, loops,   p, r, t, name, i, lab, n, open, ops) {
    p = indent(pad)
    if (depth > 0 && chance(0.18)) { memory_stmt(depth, pad, loops); return }
    if (depth > 0 && chance(0.1)) { switch_stmt(depth, pad, loops); return }
    if (chance(0.12)) { seq_stmt(pad); return }
    if (chance(0.15)) { record_stmt(pad); return }
    r = rand()
    if (r < 0.08) {
        name = "v" nlocals++
        print p name ": string = " sexpr(2)
        declare(name, "string")
        if (chance(0.3)) print p name " = " sexpr(1)
        return
    }
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
    if (isrec(rett)) return rexpr(rett, depth)
    if (rett == "string") return sexpr(depth)
    if (rett ~ /^\(/) {
        t = substr(rett, 2, index(rett, ",") - 2)
        return expr(t, depth) ", " (chance(0.5) ? "null" : "Fail(" (pick(5) - 2) ")")
    }
    t = chance(0.5) ? (chance(0.5) ? "f32" : "f64") : types[2 + pick(ntypes - 1)]
    if (t == rett) return expr(t, depth)
    return rett "(" expr(t, depth) ")"
}

# every scalar of a record value e of type t
function show_rec(e, t,   i, s) {
    s = ""
    for (i = 0; i < nfld[recno(t)]; i++) {
        if (isrec(ftype[recno(t), i])) s = s show_rec(e ".f" i, ftype[recno(t), i])
        else s = s show(e ".f" i, ftype[recno(t), i]) "\n"
    }
    return s
}

# fmt.printf of value e of type t: a float as its exact bits
function show(e, t) {
    if (t == "bool") return "    if " e " { fmt.printf(\"1\\n\") } else { fmt.printf(\"0\\n\") }"
    if (t == "string") return "    fmt.printf(\"[%s] %d\\n\", " e ", int(" e ".len))\n" show("(" e " == null)", "bool")
    if (isfloat(t)) return "    fmt.printf(\"%u\\n\", uint(bitcast(" (t == "f32" ? "u32" : "u64") ", " e ")))"
    return "    fmt.printf(\"%d\\n\", int(" e "))"
}

BEGIN {
    srand(seed * 1000003 + n)
    ntypes = split("bool i8 i16 i32 i64 u8 u16 u32 u64 usize int", types, " ")
    # bool first: types[2..] are the integers
    print "import fmt\n"
    for (k = 1; k <= ntypes; k++) print "g_" types[k] ": " types[k] " = " literal(types[k])
    print "g_f32: f32 = " fliteral("f32") "\ng_f64: f64 = " fliteral("f64")
    print "\nS: struct {\n    a: i64,\n    b: u8,\n}\n"
    print "gs: S = S { a = " literal("i64") ", b = " literal("u8") " }\n"
    print "S.bump(mut self, n: i64): int {\n    self.a += n & 7\n    self.b = u8(self.a & 63)\n    return int(self.b)\n}\n"
    print "UE: union { none, a: u8, b: i64, c: f32 }\n"
    print "UW: union { none, w: i32, o: UE }\n"
    print "mk(k: i64, v: i64): UE {\n    if k == 0 { return UE.none }\n    if k == 1 { return UE.a(u8(v)) }\n    if k == 2 { return UE.b(v) }\n    return UE.c(f32(v))\n}\n"
    print "mkw(k: i64, v: i64): UW {\n    if k == 0 { return UW.none }\n    if k == 1 { return UW.w(i32(v)) }\n    return UW.o(mk(k, v))\n}\n"
    print "bit(b: bool): int {\n    if b { return 1 }\n    return 0\n}\n"
    print "eqe(a: UE, b: UE): int {\n    return bit(a == b)\n}\n"
    print "nee(a: UE, b: UE): int {\n    return bit(a != b)\n}\n"
    print "eqw(a: UW, b: UW): int {\n    return bit(a == b)\n}\n"
    print "new(a: UW, b: UW): int {\n    return bit(a != b)\n}\n"
    print "swap(e: UE, w: UW, mut o: UE): UW {\n    o = e\n    return w\n}\n"
    print "UB: struct {\n    k: i64,\n    e: UE,\n    t: u8,\n}\n"
    print "eval(e: UE): i64 {\n    switch e {\n    UE.none { return 1 }\n    UE.a(x: u8) { return i64(x) }\n    UE.b(x: i64) { return x * 3 }\n    default { return 9 }\n    }\n    return 0\n}\n"
    print "evw(w: UW): i64 {\n    switch w {\n    UW.none { return 5 }\n    UW.w(x: i32) { return i64(x) }\n    UW.o(x: UE) { return eval(x) + 7 }\n    }\n    return 0\n}\n"
    print "mkb(k: i64, v: i64): UB {\n    return UB { k = k, e = mk(k, v), t = u8(v) }\n}\n"
    print "sumb(b: UB): i64 {\n    return b.k + eval(b.e) + i64(b.t)\n}\n"
    print "echob(b: UB): UB {\n    return b\n}\n"
    print "nblit(noalias d: mut []u8, s: []u8): int {\n    if d.len > 0 && s.len > 0 { return int(d[0]) + int(s[0]) }\n    return int(d.len) + int(s.len)\n}\n"
    print "GK: const int = 3 * 5 + 1\n"
    print "DomErr: error {\n    3: three = \"the third\",\n    9: nine = \"nine\",\n}\n"
    print "derr(k: i64): Error {\n    if k == 0 { return null }\n    if k == 1 { return DomErr(3) }\n    if k == 2 { return DomErr(9) }\n    return DomErr(i32(k))\n}\n"
    print "dlen(e: Error): int {\n    return int(e.name.len) * 100 + int(e.message.len)\n}\n"
    print "again(n: i64): i64 {\n    t: i64 = 0\n    k: i64 = 0\n    rep: {\n        k += 1\n        t += k * GK\n        if k < n { continue rep }\n    }\n    return t\n}\n"
    print "table() {\n    for i: i64 in 0 .. 4 {\n        for j: i64 in 0 .. 4 {"
    print "            fmt.printf(\"%d\", eqe(mk(i, " pick(3) "), mk(j, " pick(3) ")))"
    print "            fmt.printf(\"%d\", nee(mk(i, 1), mk(j, 2)))"
    print "            fmt.printf(\"%d\", eqw(mkw(i, " pick(3) "), mkw(j, " pick(3) ")))"
    print "            fmt.printf(\"%d \", new(mkw(i, 2), mkw(j, 2)))"
    print "            o: UE = mk(0, 0)\n            w: UW = swap(mk(i, 1), mkw(j, 2), mut o)"
    print "            fmt.printf(\"%d\", eqe(o, mk(i, 1)))\n            fmt.printf(\"%d \", eqw(w, mkw(j, 2)))"
    print "            fmt.printf(\"%d %d \", eval(mk(i, " pick(9) ")), evw(mkw(j, " pick(9) ")))"
    print "            bb: UB = echob(mkb(i, " pick(9) "))\n            fmt.printf(\"%d \", sumb(bb))"
    print "            bb.e = mk(j, 2)\n            fmt.printf(\"%d \", sumb(bb))"
    print "            arr: [2]UE = [mk(i, 1), mk(j, 2)]\n            fmt.printf(\"%d\\n\", eval(arr[0]) + eval(arr[j & 1]))\n        }\n    }\n}\n"
    print "gd: i64\n"
    print "dnote(x: i64) {\n    gd = gd * 3 % 1000003 + x % 1000\n}\n"
    print "pin(mut n: i64) {}\n"
    print "Fail: error\n"
    print "gbuf: [24]u8\ngv: mut []u8\ngt: string\n"
    print "GA: struct {\n    lo: u8,\n    a: [22]u8,\n    hi: u8,\n}\n\nga: GA\n"
    nstrs = split("\"\" \"a\" \"hello\" \"ace-ir\" \"0123456789abcdef01234\"", strs, " ")
    print "gmax[T](a: T, b: T): T {\n    if a > b { return a }\n    return b\n}\n"
    print "gpick[T](a: T, b: T, c: bool): T {\n    if c { return a }\n    return b\n}\n"
    print "gpick[i64](a: i64, b: i64, c: bool): i64 {\n    if c { return b }\n    return a\n}\n"
    nrec = 1 + pick(4)
    for (r = 0; r < nrec; r++) { # a floating aggregate, small scalars sharing words, or anything
        form = rand()
        nfld[r] = form < 0.3 ? 1 + pick(4) : 1 + pick(5)
        s = "R" r ": struct {\n"
        ft = chance(0.5) ? "f32" : "f64"
        for (k = 0; k < nfld[r]; k++) {
            if (form < 0.3) t = ft
            else if (form < 0.55) t = narrows[1 + pick(split("i8 u8 bool u16 i32 u32", narrows, " "))]
            else t = chance(0.15) ? "string" : anytype()
            if (r > 0 && chance(0.15)) t = "R" pick(r)
            ftype[r, k] = t
            s = s "    f" k ": " t ",\n"
        }
        print s "}\n"
    }
    nf = 2 + pick(4)
    for (f = 0; f < nf; f++) {
        nvars = 0
        nlocals = 0
        nlabels = 0
        np = chance(0.15) ? 9 + pick(4) : 1 + pick(4) # past the argument registers, at times
        r = rand()
        rett = r < 0.1 ? "string" : r < 0.2 ? "R" pick(nrec) : r < 0.35 ? "(" types[2 + pick(ntypes - 1)] ", Error)" : r < 0.55 ? (chance(0.5) ? "f32" : "f64") : "int"
        fret[f] = rett
        finl = chance(0.5) && (rett == "int" || isfloat(rett))
        s = (finl ? "inline " : "") "f" f "("
        for (k = 0; k < np; k++) {
            t = chance(0.3) ? "R" pick(nrec) : chance(0.15) ? "string" : anytype()
            ptype[f, k] = t
            pmut[f, k] = !isrec(t) && t != "string" && chance(0.2)
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
        if (!finl && chance(0.3)) print "    ap: i64 = 0\n    pin(mut ap)\n    asm \"nop\" { in: " asm_reg " = ap }" # an address-taken binding, read from its home; an inline function cannot hold asm
        if (!finl && chance(0.3)) { # deferred notes: the value a parameter has when deferred, then a constant
            for (k = 0; k < np; k++)
                if (ptype[f, k] ~ /^(i8|i16|i32|i64|u8|u16|u32|u64|usize|int)$/ && (pmut[f, k] || chance(0.3))) {
                    print "    defer dnote(i64(p" k "))"
                    break
                }
            if (chance(0.5)) print "    defer dnote(" pick(50) ")"
        }
        block(3, 1, "")
        print "    return " result(2)
        print "}\n"
        nparams[f] = np
        nfuncs = f + 1
        delete vfixed
    }
    nvars = 0
    print "main(): int {"
    print "    gbuf[0] = 35\n    gbuf[23] = 36\n    gv = mut gbuf[1..23]"
    print "    ga.lo = 37\n    ga.hi = 38\n    for k: usize in 0 .. 22 { ga.a[k] = u8(k * 3) }"
    print "    gt = \"#abcdefghijklmnopqrstuv$\"[1..23]"
    for (f = 0; f < nf; f++) {
        for (c = 0; c < 3; c++) {
            if (isrec(fret[f])) {
                print "    r" f "_" c ": " fret[f] " = f" f "(" args_of(f, 1) ")"
                printf "%s", show_rec("r" f "_" c, fret[f])
            } else if (fret[f] ~ /^\(/) {
                t = substr(fret[f], 2, index(fret[f], ",") - 2)
                print "    t" f "_" c ": " t ", e" f "_" c ": Error = f" f "(" args_of(f, 1) ")"
                print show("t" f "_" c, t)
                print show("e" f "_" c ".code", "i32")
            } else {
                print show("f" f "(" args_of(f, 1) ")", fret[f])
            }
        }
    }
    for (k = 0; k < 3; k++) { # two nonempty windows of gbuf: disjoint, or a debug contract trap in both (the function only reads, so a release overlap is harmless)
        lo = 1 + pick(20); hi = lo + 1 + pick(23 - lo); lo2 = 1 + pick(20); hi2 = lo2 + 1 + pick(23 - lo2)
        print "    fmt.printf(\"%d\\n\", nblit(mut gbuf[" lo ".." hi "], gbuf[" lo2 ".." hi2 "]))"
    }
    print "    for k: i64 in 0 .. 5 {\n        fmt.printf(\"%d %d\\n\", dlen(derr(k)), int(again(k)))\n        if derr(k).domain == typeid(DomErr) { fmt.printf(\"dom\\n\") }\n    }"
    print "    table()"
    print "    fmt.printf(\"%d\\n\", gd)"
    print "    fmt.printf(\"%d %d\\n\", int(gs.a), int(gs.b))"
    print "    for k: usize in 0 .. 24 { fmt.printf(\"%d \", int(gbuf[k])) }"
    print "    fmt.printf(\"%d %d \", int(ga.lo), int(ga.hi))\n    for k: usize in 0 .. 22 { fmt.printf(\"%d \", int(ga.a[k])) }"
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
    awk -v seed="$seed" -v n="$n" -v asm_reg="$asm_reg" "$generator" > "$dir/case.ace"
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
        if [ "$mode" = debug ] && grep -Eq '(arithmetic|bounds) violation' "$dir/old.out"; then
            trapped=$((trapped + 1))
        fi
        if ! cmp -s "$dir/old.out" "$dir/new.out"; then
            same=0
            echo "mismatch: program $n, $mode" >&2
            cp "$dir/old.out" "$dir/old_$mode.out"
            cp "$dir/new.out" "$dir/new_$mode.out"
        fi
    done
    if [ "$same" -eq 0 ]; then
        mismatched=$((mismatched + 1))
        if [ "$mismatched" -eq 1 ]; then
            keep=$(mktemp -d "${TMPDIR:-/tmp}/ace-ir-diff-mismatch.XXXXXX")
        fi
        cp "$dir/case.ace" "$keep/case_$n.ace"
        for out in "$dir"/old_*.out "$dir"/new_*.out; do
            [ -f "$out" ] && mv "$out" "$keep/case_${n}_${out##*/}"
        done
    fi
    n=$((n + 1))
done

echo "programs $count (seed $seed), functions lowered $lowered of $funcs, debug traps $trapped, mismatches $mismatched"
if [ "$mismatched" -ne 0 ]; then
    echo "mismatching programs kept in $keep" >&2
    exit 1
fi
