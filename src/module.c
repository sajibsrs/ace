// module.c — general import resolution for the merged compilation unit.
//
// Modules are files. The loader recursively loads what each file imports —
// bare names from the stdlib beside the compiler (a flat file or a directory
// with module.ace), local files by a path relative to the importing source —
// canonicalizes identity by realpath so
// every file loads exactly once through any number of imports or aliases,
// and diagnoses cycles with the full chain. Each loaded module's surface is
// lowered into one private module-prefixed namespace (`name$member`; `$`
// cannot appear in source identifiers): declaration names and their in-module
// references are renamed together; qualified and selectively imported
// references in the importing file are rewritten to prefixed names after a
// `pub` check, and extern
// declarations keep their C-boundary name in Node.cname for the linker.
// The merged unit is ordered dependencies-first, so the single-pass checker
// and emitter still see every name declared before use. Separate per-module
// objects and incremental rebuilds are deliberately deferred.
#include "ace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- loader state -----------------------------------------------------------------

typedef struct Member Member;
struct Member {
    const char* name;
    uint32_t    len;
    int         is_type; // lives in the type namespace (struct/contract/…)
    int         pub;     // every declaration with this name is pub
    Member*     next;
};

typedef struct Binding Binding;
struct Binding {
    const char* name;
    uint32_t    len;
    struct Module* mod;
    Node*       node; // the import declaration (diagnostics)
    Binding*    next;
};

typedef struct Select Select;
struct Select {
    const char* name;
    uint32_t    len;
    const char* value_name; // target module-private value identity, if any
    const char* type_name;  // target module-private type identity, if any
    Node*       node;       // selected member node (diagnostics)
    Select*     next;
};

typedef struct Module Module;
struct Module {
    const char* canon;  // canonical path = module identity
    const char* prefix; // unique `name$` prefix for private identities
    Node*       file;   // parsed module (imports already resolved + dropped)
    Binding*    binds;  // this module's own import bindings
    Member*     members;
    Module*     next;   // completion order: dependencies before dependents
};

typedef struct {
    Diag*       d;
    const char* compiler_path;
    const char* std_root; // canonical compiler-shipped std directory
    const char* target;
    CompileReport* report;
    Module*     mods;    // completed modules, dependency order
    Module*     loading; // in-progress stack (cycle detection/chain)
    const char** prefixes; int nprefixes, cprefixes;
} Loader;

static void import_err(Diag* d, Node* n, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ",
            n && n->path ? n->path : d->path,
            n ? n->line : 0, n ? n->col : 0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    d->errors++;
}

static const char* arena_string(const char* s) {
    size_t n = strlen(s);
    char* out = arena_alloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

static int text_is(const char* s, uint32_t len, const char* t) {
    return len == (uint32_t)strlen(t) && memcmp(s, t, len) == 0;
}

static void compiler_dir(char* out, size_t cap, const char* argv0) {
    const char* slash = strrchr(argv0, '/');
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - argv0);
    if (n == 0) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, argv0, n);
    out[n] = 0;
}

// Directory part of a source path ("dir/file.ace" → "dir", "file.ace" → ".").
static void path_dir(char* out, size_t cap, const char* path) {
    const char* slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

static const char* join_path(const char* dir, const char* rel) {
    size_t n = strlen(dir) + 1 + strlen(rel) + 1;
    char* buf = arena_alloc(n);
    if (strcmp(dir, ".") == 0) snprintf(buf, n, "%s", rel);
    else snprintf(buf, n, "%s/%s", dir, rel);
    return buf;
}

// Canonical path = module identity. Returns arena storage or NULL.
static const char* canonicalize(const char* path) {
    char buf[4096];
    if (!realpath(path, buf)) return NULL;
    return arena_string(buf);
}

// True only for descendants, with a path-component boundary after dir.
static int path_beneath(const char* path, const char* dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && path[n] == '/';
}

static int is_std_source(Loader* L, const char* canon) {
    return L->std_root && path_beneath(canon, L->std_root);
}

static int is_std_internal(Loader* L, const char* canon) {
    if (!L->std_root) return 0;
    size_t n = strlen(L->std_root);
    return strncmp(canon, L->std_root, n) == 0 &&
           strcmp(canon + n, "/@") != 0 &&
           strncmp(canon + n, "/@/", 3) == 0;
}

static const char* std_display_path(Loader* L, const char* canon) {
    size_t n = strlen(L->std_root);
    size_t tail = strlen(canon + n);
    char* out = arena_alloc(3 + tail + 1);
    memcpy(out, "std", 3);
    memcpy(out + 3, canon + n, tail + 1);
    return out;
}

// --- module loading -----------------------------------------------------------------

static int resolve_file(Loader* L, Node* file, const char* src_path, int is_root);

static Node* load_module(Loader* L, const char* path) {
    const char* stable = arena_string(path);
    Source src;
    if (source_load(&src, stable) != 0) return NULL; // message already printed
    if (L->report) {
        L->report->source_files++;
        L->report->source_bytes += src.len;
    }
    Diag d = { stable, 0 };
    size_t ntok = 0;
    Token* toks = lex(&src, &d, &ntok);
    if (L->report && ntok) L->report->tokens += ntok - 1; // exclude EOF
    if (d.errors) { L->d->errors += d.errors; return NULL; }
    Node* file = parse(toks, ntok, &d, L->target);
    if (d.errors) { L->d->errors += d.errors; return NULL; }
    return file;
}

// Module-level builtin names are checked before prefixing: `m$panic`
// would pass sema's reservation and silently shadow the builtin for this
// module's own calls (rename_refs rewrites them to the declaration).
static void check_reserved_name(Loader* L, Node* at, const char* name, uint32_t len) {
    if (len == 5 && memcmp(name, "panic", 5) == 0)
        import_err(L->d, at,
                   "'panic' is a reserved builtin name — no declaration may take it");
    if (len == 6 && memcmp(name, "assert", 6) == 0)
        import_err(L->d, at,
                   "'assert' is a reserved builtin name — no declaration may take it");
}

// The member surface of a module: name + namespace + pub-ness. Names are
// unique per namespace inside a module (the checker enforces it after the
// merge), so overloads of one name fold into a single member entry whose
// pub-ness is the AND over its declarations — strict start, relax later.
static void add_member(Module* m, const char* name, uint32_t len, int is_type, int pub) {
    for (Member* x = m->members; x; x = x->next) {
        if (x->is_type == is_type && x->len == len && memcmp(x->name, name, len) == 0) {
            if (!pub) x->pub = 0;
            return;
        }
    }
    Member* x = (Member*)arena_alloc(sizeof(Member));
    memset(x, 0, sizeof *x);
    x->name = name;
    x->len = len;
    x->is_type = is_type;
    x->pub = pub;
    x->next = m->members;
    m->members = x;
}

static void collect_members(Loader* L, Module* m) {
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        switch (n->kind) {
        case N_FUNC:
            if (!(n->flags & F_METHOD)) { // methods bind a qualified name
                check_reserved_name(L, n, n->str, n->len);
                add_member(m, n->str, n->len, 0, (n->flags & F_PUB) != 0);
            }
            break;
        case N_VAR:
            check_reserved_name(L, n, n->kids[0]->str, n->kids[0]->len);
            add_member(m, n->kids[0]->str, n->kids[0]->len, 0,
                       (n->flags & F_PUB) != 0);
            break;
        case N_STRUCT: case N_BITS: case N_CONTRACT: case N_ENUM:
        case N_ALIAS: case N_DISTINCT: case N_ERROR_DOMAIN: case N_UNION:
            check_reserved_name(L, n, n->str, n->len);
            add_member(m, n->str, n->len, 1, (n->flags & F_PUB) != 0);
            break;
        case N_EXTERN: // C bindings are module-private
            for (int j = 0; j < n->nkids; j++) {
                check_reserved_name(L, n->kids[j], n->kids[j]->str, n->kids[j]->len);
                add_member(m, n->kids[j]->str, n->kids[j]->len, 0, 0);
            }
            break;
        default:
            break;
        }
    }
}

static Member* find_member(Module* m, const char* name, uint32_t len, int is_type) {
    for (Member* x = m->members; x; x = x->next)
        if (x->is_type == is_type && x->len == len && memcmp(x->name, name, len) == 0)
            return x;
    return NULL;
}

static int is_std_named_module(Loader* L, const char* canon, const char* name) {
    char dir[4096];
    compiler_dir(dir, sizeof dir, L->compiler_path);
    char flat[4608];
    char directory[4608];
    if (strcmp(dir, ".") == 0) {
        snprintf(flat, sizeof flat, "std/%s.ace", name);
        snprintf(directory, sizeof directory, "std/%s/module.ace", name);
    } else {
        snprintf(flat, sizeof flat, "%s/std/%s.ace", dir, name);
        snprintf(directory, sizeof directory, "%s/std/%s/module.ace", dir, name);
    }
    const char* expected = canonicalize(flat);
    if (expected && strcmp(expected, canon) == 0) return 1;
    expected = canonicalize(directory);
    return expected && strcmp(expected, canon) == 0;
}

static int is_std_internal_named_module(Loader* L, const char* canon,
                                        const char* name) {
    char dir[4096];
    compiler_dir(dir, sizeof dir, L->compiler_path);
    char path[4608];
    if (strcmp(dir, ".") == 0)
        snprintf(path, sizeof path, "std/@/%s.ace", name);
    else
        snprintf(path, sizeof path, "%s/std/@/%s.ace", dir, name);
    const char* expected = canonicalize(path);
    return expected && strcmp(expected, canon) == 0;
}

static void mark_internal_load_intrinsic(Loader* L, Module* m) {
    if (!is_std_internal_named_module(L, m->canon, "byte_load")) return;
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        if (n->kind == N_FUNC && (n->flags & F_INTRINSIC) &&
            text_is(n->str, n->len, "unaligned_u64"))
            n->ival = BI_INTERNAL_LOAD_U64;
    }
}

static void mark_bits_intrinsics(Loader* L, Module* m) {
    if (!is_std_named_module(L, m->canon, "bits")) return;
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        if (n->kind != N_FUNC || !(n->flags & F_INTRINSIC)) continue;
        if (text_is(n->str, n->len, "from_float"))
            n->ival = BI_BITS_FROM_FLOAT;
        else if (text_is(n->str, n->len, "to_float"))
            n->ival = BI_BITS_TO_FLOAT;
        else if (text_is(n->str, n->len, "pop_count"))
            n->ival = BI_BITS_POP_COUNT;
        else if (text_is(n->str, n->len, "lead_zeros"))
            n->ival = BI_BITS_LEAD_ZEROS;
        else if (text_is(n->str, n->len, "trail_zeros"))
            n->ival = BI_BITS_TRAIL_ZEROS;
        else if (text_is(n->str, n->len, "byte_swap"))
            n->ival = BI_BITS_BYTE_SWAP;
        else if (text_is(n->str, n->len, "rotate_left"))
            n->ival = BI_BITS_ROTATE_LEFT;
        else if (text_is(n->str, n->len, "rotate_right"))
            n->ival = BI_BITS_ROTATE_RIGHT;
    }
}

static void mark_mem_intrinsics(Loader* L, Module* m) {
    if (!is_std_named_module(L, m->canon, "mem")) return;
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        if (n->kind != N_FUNC || !(n->flags & F_PUB)) continue;
        // The bracket parameter separates the typed generic overloads from
        // the raw byte forms; both stay compiler-backed with no emitted body.
        if (text_is(n->str, n->len, "alloc"))
            n->ival = n->c ? BI_MEM_ALLOC_T : BI_MEM_ALLOC;
        else if (text_is(n->str, n->len, "realloc"))
            n->ival = n->c ? BI_MEM_REALLOC_T : BI_MEM_REALLOC;
        else if (text_is(n->str, n->len, "release"))
            n->ival = n->c ? BI_MEM_RELEASE_T : BI_MEM_RELEASE;
        else if (text_is(n->str, n->len, "copy")) n->ival = BI_MEM_COPY;
        else if (text_is(n->str, n->len, "move")) n->ival = BI_MEM_MOVE;
        else if (text_is(n->str, n->len, "fill")) n->ival = BI_MEM_FILL;
        else if (text_is(n->str, n->len, "zero")) n->ival = BI_MEM_ZERO;
        else if (text_is(n->str, n->len, "compare")) n->ival = BI_MEM_COMPARE;
        else if (text_is(n->str, n->len, "equal")) n->ival = BI_MEM_EQUAL;
    }
}

static void mark_fmt_intrinsics(Loader* L, Module* m) {
    if (!is_std_named_module(L, m->canon, "fmt")) return;
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        if (n->kind == N_FUNC && (n->flags & F_PUB) &&
            text_is(n->str, n->len, "printf"))
            n->ival = BI_FMT_PRINTF;
    }
}

static void mark_sys_intrinsics(Loader* L, Module* m) {
    if (!is_std_named_module(L, m->canon, "sys")) return;
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        if (n->kind != N_FUNC) continue;
        if (text_is(n->str, n->len, "process_argc")) n->ival = BI_SYS_ARGC;
        else if (text_is(n->str, n->len, "process_arg")) n->ival = BI_SYS_ARG;
        else if (text_is(n->str, n->len, "process_envc")) n->ival = BI_SYS_ENVC;
        else if (text_is(n->str, n->len, "process_env")) n->ival = BI_SYS_ENV;
    }
}

// Unique `name$` prefix per module; `$` never occurs in source identifiers,
// so prefixed names cannot collide with root declarations, and distinct
// modules can never share a prefixed member name.
static const char* assign_prefix(Loader* L, const char* canon) {
    const char* slash = strrchr(canon, '/');
    const char* base = slash ? slash + 1 : canon;
    size_t blen = strlen(base);
    if (blen > 4 && strcmp(base + blen - 4, ".ace") == 0) blen -= 4;
    // A directory module's useful identity is its directory name, not the
    // conventional entry filename shared by every directory module.
    if (blen == 6 && memcmp(base, "module", 6) == 0 && slash) {
        const char* parent = slash;
        while (parent > canon && parent[-1] != '/') parent--;
        if (parent < slash) {
            base = parent;
            blen = (size_t)(slash - parent);
        }
    }
    char candidate[512];
    for (int n = 1;; n++) {
        if (n == 1) snprintf(candidate, sizeof candidate, "%.*s$", (int)blen, base);
        else snprintf(candidate, sizeof candidate, "%.*s%d$", (int)blen, base, n);
        int used = 0;
        for (int i = 0; i < L->nprefixes; i++)
            if (strcmp(L->prefixes[i], candidate) == 0) { used = 1; break; }
        if (!used) break;
    }
    const char* out = arena_string(candidate);
    if (L->nprefixes == L->cprefixes) {
        L->cprefixes = L->cprefixes ? L->cprefixes * 2 : 8;
        L->prefixes = realloc(L->prefixes, (size_t)L->cprefixes * sizeof(char*));
        if (!L->prefixes) exit(1);
    }
    L->prefixes[L->nprefixes++] = out;
    return out;
}

static Module* module_new(Loader* L, const char* canon, Node* file) {
    Module* m = (Module*)arena_alloc(sizeof(Module));
    memset(m, 0, sizeof *m);
    m->canon = canon;
    m->file = file;
    m->prefix = assign_prefix(L, canon);
    return m;
}

static int module_on_stack(Loader* L, const char* canon) {
    for (Module* m = L->loading; m; m = m->next)
        if (strcmp(m->canon, canon) == 0) return 1;
    return 0;
}

static void report_cycle(Loader* L, Node* at, const char* canon) {
    size_t cap = strlen(canon) + 64;
    for (Module* m = L->loading; m; m = m->next) cap += strlen(m->canon) + 4;
    char* chain = arena_alloc(cap);
    size_t off = 0;
    off += (size_t)snprintf(chain + off, cap - off, "%s", canon);
    for (Module* m = L->loading; m; m = m->next)
        off += (size_t)snprintf(chain + off, cap - off, " -> %s", m->canon);
    import_err(L->d, at, "import cycle: %s", chain);
}

// Resolve one import declaration to its target path (arena storage).
// Bare, ident-aliased, and selective `from name` imports come from the stdlib
// beside the compiler; @file.ace names compiler-reserved std implementation;
// string paths are local to the importing source file.
static const char* import_target(Loader* L, Node* imp, const char* src_path) {
    if (imp->a && imp->a->op == T_AT) {
        Node* p = imp->a;
        char dir[4096];
        compiler_dir(dir, sizeof dir, L->compiler_path);
        size_t need = strlen(dir) + p->len + 12;
        char* path = arena_alloc(need);
        if (strcmp(dir, ".") == 0)
            snprintf(path, need, "std/@/%.*s.ace", (int)p->len, p->str);
        else
            snprintf(path, need, "%s/std/@/%.*s.ace", dir, (int)p->len, p->str);
        return path;
    }
    if (imp->a) { // import util: "./util.ace" — path form
        Node* p = imp->a;
        char raw[4096];
        if (p->len >= sizeof raw) {
            import_err(L->d, imp, "module path is too long");
            return NULL;
        }
        memcpy(raw, p->str, p->len);
        raw[p->len] = 0;
        if (raw[0] == '/') {
            import_err(L->d, imp, "module path '%s' must be relative", raw);
            return NULL;
        }
        size_t n = strlen(raw);
        if (n < 5 || strcmp(raw + n - 4, ".ace") != 0) {
            import_err(L->d, imp, "module path '%s' must end in .ace", raw);
            return NULL;
        }
        char dir[4096];
        path_dir(dir, sizeof dir, src_path);
        return join_path(dir, raw);
    }
    // bare/alias import, or selective `from mem` — stdlib only
    Node* name = imp->b ? imp->b : imp;
    char stdname[512];
    if (name->len >= sizeof stdname) {
        import_err(L->d, imp, "module name is too long");
        return NULL;
    }
    memcpy(stdname, name->str, name->len);
    stdname[name->len] = 0;
    char dir[4096];
    compiler_dir(dir, sizeof dir, L->compiler_path);
    char flat[4608];
    char directory[4608];
    if (strcmp(dir, ".") == 0) {
        snprintf(flat, sizeof flat, "std/%s.ace", stdname);
        snprintf(directory, sizeof directory, "std/%s/module.ace", stdname);
    } else {
        snprintf(flat, sizeof flat, "%s/std/%s.ace", dir, stdname);
        snprintf(directory, sizeof directory, "%s/std/%s/module.ace", dir, stdname);
    }
    const char* flat_canon = canonicalize(flat);
    const char* directory_canon = canonicalize(directory);
    if (flat_canon && directory_canon) {
        import_err(L->d, imp,
                   "standard library module '%s' is ambiguous (found %s and %s)",
                   stdname, flat, directory);
        return NULL;
    }
    if (directory_canon) return directory_canon;
    if (flat_canon) return flat_canon;
    import_err(L->d, imp, "standard library module '%s' not found (tried %s and %s)",
               stdname, flat, directory);
    return NULL;
}

static void bind(Loader* L, Node* imp, Module* mod, Binding** binds, Select* sels) {
    for (Binding* b = *binds; b; b = b->next) {
        if (b->len == imp->len && memcmp(b->name, imp->str, imp->len) == 0) {
            import_err(L->d, imp, "duplicate import binding '%.*s'",
                       (int)imp->len, imp->str);
            return;
        }
    }
    for (Select* s = sels; s; s = s->next) {
        if (s->value_name && s->len == imp->len &&
            memcmp(s->name, imp->str, imp->len) == 0) {
            import_err(L->d, imp, "duplicate import binding '%.*s'",
                       (int)imp->len, imp->str);
            return;
        }
    }
    Binding* b = (Binding*)arena_alloc(sizeof(Binding));
    memset(b, 0, sizeof *b);
    b->name = imp->str;
    b->len = imp->len;
    b->mod = mod;
    b->node = imp;
    b->next = *binds;
    *binds = b;
}

static const char* member_identity(Module* mod, const char* name, uint32_t len) {
    size_t plen = strlen(mod->prefix);
    char* out = arena_alloc(plen + len + 1);
    memcpy(out, mod->prefix, plen);
    memcpy(out + plen, name, len);
    out[plen + len] = 0;
    return out;
}

static void select_members(Loader* L, Node* imp, Module* mod,
                           Binding* binds, Select** sels) {
    for (int i = 0; i < imp->nkids; i++) {
        Node* name = imp->kids[i];
        Member* vm = find_member(mod, name->str, name->len, 0);
        Member* tm = find_member(mod, name->str, name->len, 1);
        if (!vm && !tm) {
            import_err(L->d, name, "module has no public member '%.*s'",
                       (int)name->len, name->str);
            continue;
        }
        int value_public = vm && vm->pub;
        int type_public = tm && tm->pub;
        if (!value_public && !type_public) {
            import_err(L->d, name, "member '%.*s' is not public",
                       (int)name->len, name->str);
            continue;
        }
        if (!value_public) vm = NULL;
        if (!type_public) tm = NULL;
        int dup_value = 0, dup_type = 0;
        for (Select* s = *sels; s; s = s->next) {
            if (s->len != name->len || memcmp(s->name, name->str, name->len) != 0)
                continue;
            dup_value |= vm && s->value_name;
            dup_type |= tm && s->type_name;
        }
        for (Binding* b = binds; b; b = b->next)
            if (vm && b->len == name->len &&
                memcmp(b->name, name->str, name->len) == 0) dup_value = 1;
        if (dup_value || dup_type) {
            import_err(L->d, name, "duplicate import binding '%.*s'",
                       (int)name->len, name->str);
            continue;
        }
        Select* s = (Select*)arena_alloc(sizeof(Select));
        memset(s, 0, sizeof *s);
        s->name = name->str;
        s->len = name->len;
        if (vm) s->value_name = member_identity(mod, name->str, name->len);
        if (tm) s->type_name = member_identity(mod, name->str, name->len);
        s->node = name;
        s->next = *sels;
        *sels = s;
    }
}

// Load (once) the module an import refers to and return it.
static Module* ensure_module(Loader* L, Node* imp, const char* src_path) {
    const char* target = import_target(L, imp, src_path);
    if (!target) return NULL;
    const char* canon = canonicalize(target);
    if (!canon) {
        import_err(L->d, imp, "cannot load module '%.*s' from %s: no such file",
                   (int)(imp->a ? imp->a->len : imp->len),
                   imp->a ? imp->a->str : imp->str, src_path);
        return NULL;
    }
    const char* importer = canonicalize(src_path);
    if (is_std_internal(L, canon) &&
        (!importer || !is_std_source(L, importer))) {
        import_err(L->d, imp,
                   "compiler-reserved standard-library implementation '%s' may be imported only by standard-library sources",
                   std_display_path(L, canon));
        return NULL;
    }
    if (is_std_internal(L, canon) && (!imp->a || imp->a->op != T_AT)) {
        import_err(L->d, imp,
                   "compiler-reserved standard-library implementation must use @file.ace import syntax");
        return NULL;
    }
    if (module_on_stack(L, canon)) {
        report_cycle(L, imp, canon);
        return NULL;
    }
    for (Module* m = L->mods; m; m = m->next)
        if (strcmp(m->canon, canon) == 0) return m; // loaded once, aliased freely
    Node* file = load_module(L, canon);
    if (!file) return NULL;
    Module* m = module_new(L, canon, file);
    collect_members(L, m); // original names + pub-ness, before any renaming
    mark_mem_intrinsics(L, m);
    mark_fmt_intrinsics(L, m);
    mark_sys_intrinsics(L, m);
    mark_bits_intrinsics(L, m);
    mark_internal_load_intrinsic(L, m);
    m->next = L->loading; // push
    L->loading = m;
    resolve_file(L, file, canon, 0);
    L->loading = m->next; // pop
    m->next = L->mods;    // completed: dependencies are already in L->mods
    L->mods = m;
    // completion order must be dependencies-first; prepend breaks that, so
    // instead append in reverse by walking — simplest: collect and reverse
    // once before the merge (see resolve_imports).
    return m;
}

// --- qualified reference lowering -----------------------------------------------------
// `binding.member` forms are rewritten to the module's private prefixed
// names after a pub check, before semantic checking runs. Shadowing an
// import binding with a local is diagnosed by the checker (unknown
// `name$member`); the loader cannot see scopes, only the surface.

static void qualified_type_err(Loader* L, Node* n) {
    import_err(L->d, n, "qualified type paths deeper than 'module.Type' are not supported");
}

// `math.Vec[int]` in type position: collapse the qualifier chain to the
// prefixed type name, keeping generic arguments.
static void lower_named(Loader* L, Node* t, Binding* binds) {
    if (!t->a) return;
    if (t->a->a) { qualified_type_err(L, t); return; }
    Node* base = t->a; // the qualifier ident (kept as N_NAMED by the parser)
    for (Binding* b = binds; b; b = b->next) {
        if (!text_is(base->str, base->len, b->name)) continue;
        Member* mem = find_member(b->mod, t->str, t->len, 1);
        if (!mem) {
            import_err(L->d, t, "module '%.*s' has no type '%.*s'",
                       (int)b->len, b->name, (int)t->len, t->str);
        } else if (!mem->pub) {
            import_err(L->d, t, "type '%.*s' of module '%.*s' is not public",
                       (int)t->len, t->str, (int)b->len, b->name);
        } else {
            char qn[512];
            snprintf(qn, sizeof qn, "%s%.*s", b->mod->prefix, (int)t->len, t->str);
            t->str = arena_string(qn);
            t->len = (uint32_t)strlen(qn);
            t->a = NULL;
            t->line = base->line;
            t->col = base->col;
        }
        return;
    }
    import_err(L->d, t, "unknown module '%.*s' in qualified type",
               (int)base->len, base->str);
}

static Node* prefixed_ident(Node* from, const char* qname) {
    Node* id = (Node*)arena_alloc(sizeof(Node));
    memset(id, 0, sizeof *id);
    id->kind = (uint8_t)N_IDENT;
    id->line = from->line;
    id->col = from->col;
    id->path = from->path;
    id->str = arena_string(qname);
    id->len = (uint32_t)strlen(qname);
    return id;
}

// `binding.member` in expression position: value member (function, overload
// set, global, type constructor/cast) → plain prefixed
// ident. Also the `binding.Type.member` enum-constant form: the inner
// `binding.Type` becomes the prefixed type ident and the outer member access
// is kept for the checker's enum path.
static void lower_field(Loader* L, Node* e, Binding* binds) {
    if (e->a->kind == N_FIELD_E && e->a->a && e->a->a->kind == N_IDENT) {
        Node* inner = e->a;
        for (Binding* b = binds; b; b = b->next) {
            if (!text_is(inner->a->str, inner->a->len, b->name)) continue;
            Member* mem = find_member(b->mod, inner->str, inner->len, 1);
            if (!mem) {
                import_err(L->d, inner, "module '%.*s' has no type '%.*s'",
                           (int)b->len, b->name, (int)inner->len, inner->str);
            } else if (!mem->pub) {
                import_err(L->d, inner, "type '%.*s' of module '%.*s' is not public",
                           (int)inner->len, inner->str, (int)b->len, b->name);
            } else {
                char qn[512];
                snprintf(qn, sizeof qn, "%s%.*s", b->mod->prefix,
                         (int)inner->len, inner->str);
                e->a = prefixed_ident(inner, qn);
            }
            return;
        }
        return; // not a binding: ordinary nested field access
    }
    if (e->a->kind != N_IDENT) return;
    for (Binding* b = binds; b; b = b->next) {
        if (!text_is(e->a->str, e->a->len, b->name)) continue;
        Member* mem = find_member(b->mod, e->str, e->len, 0);
        if (!mem) mem = find_member(b->mod, e->str, e->len, 1);
        if (!mem) {
            import_err(L->d, e, "module '%.*s' has no member '%.*s'",
                       (int)b->len, b->name, (int)e->len, e->str);
        } else if (!mem->pub) {
            import_err(L->d, e, "member '%.*s' of module '%.*s' is not public",
                       (int)e->len, e->str, (int)b->len, b->name);
        } else {
            char qn[512];
            snprintf(qn, sizeof qn, "%s%.*s", b->mod->prefix, (int)e->len, e->str);
            int line = e->line, col = e->col;
            const char* path = e->path;
            memset(e, 0, sizeof *e);
            e->kind = (uint8_t)N_IDENT;
            e->line = line;
            e->col = col;
            e->path = path;
            e->str = arena_string(qn);
            e->len = (uint32_t)strlen(qn);
        }
        return;
    }
}

static void lower_qualified(Loader* L, Node* n, Binding* binds) {
    if (!n) return;
    switch (n->kind) {
    case N_IMPORT:
        return; // bindings, not references
    case N_NAMED:
        if (n->a) { lower_named(L, n, binds); return; }
        break;
    case N_FIELD_E:
        if (n->a && n->a->kind == N_IDENT) { lower_field(L, n, binds); return; }
        if (n->a && n->a->kind == N_FIELD_E) { lower_field(L, n, binds); return; }
        break;
    default:
        break;
    }
    lower_qualified(L, n->a, binds);
    lower_qualified(L, n->b, binds);
    lower_qualified(L, n->c, binds);
    lower_qualified(L, n->d, binds);
    for (int i = 0; i < n->nkids; i++) lower_qualified(L, n->kids[i], binds);
}

// --- selective import lowering ------------------------------------------------------
// Selected names are lowered with lexical-scope awareness. This is intentionally
// separate from rename_refs: a parameter/local may shadow a selected value, while
// a generic type parameter may shadow a selected type.

typedef struct LocalName LocalName;
struct LocalName {
    const char* name;
    uint32_t len;
    int depth;
    int is_type;
    LocalName* next;
};

typedef struct {
    Select* sels;
    LocalName* locals;
    int depth;
} SelectLower;

static Select* find_select(Select* sels, const char* name, uint32_t len, int is_type) {
    for (Select* s = sels; s; s = s->next)
        if (s->len == len && memcmp(s->name, name, len) == 0 &&
            (is_type ? s->type_name != NULL : s->value_name != NULL))
            return s;
    return NULL;
}

static int local_name(SelectLower* c, const char* name, uint32_t len, int is_type) {
    for (LocalName* n = c->locals; n; n = n->next)
        if (n->is_type == is_type && n->len == len &&
            memcmp(n->name, name, len) == 0) return 1;
    return 0;
}

static void bind_local(SelectLower* c, const char* name, uint32_t len, int is_type) {
    LocalName* n = (LocalName*)arena_alloc(sizeof(LocalName));
    n->name = name;
    n->len = len;
    n->depth = c->depth;
    n->is_type = is_type;
    n->next = c->locals;
    c->locals = n;
}

static void select_scope_push(SelectLower* c) { c->depth++; }

static void select_scope_pop(SelectLower* c) {
    while (c->locals && c->locals->depth == c->depth) c->locals = c->locals->next;
    c->depth--;
}

static void replace_name(Node* n, const char* name) {
    n->str = name;
    n->len = (uint32_t)strlen(name);
}

static void lower_selected_node(Node* n, SelectLower* c);

static void lower_selected_type(Node* t, SelectLower* c) {
    if (!t) return;
    if (t->kind == N_NAMED && !t->a &&
        !local_name(c, t->str, t->len, 1)) {
        Select* s = find_select(c->sels, t->str, t->len, 1);
        if (s) replace_name(t, s->type_name);
    }
    lower_selected_type(t->a, c);
    if (t->kind == N_STATICMAP_T) lower_selected_type(t->b, c);
    for (int i = 0; i < t->nkids; i++) lower_selected_type(t->kids[i], c);
}

static void lower_selected_type_ref(Node* n, SelectLower* c) {
    if (!n) return;
    if (n->kind == N_NAMED) {
        lower_selected_type(n, c);
        return;
    }
    if (n->kind == N_SLICE_T || n->kind == N_ARRAY_T || n->kind == N_PTR ||
        n->kind == N_CONST_T) { // inline operand, e.g. alignof([]pkg.T)
        lower_selected_type_ref(n->a, c);
        return;
    }
    Node* name = n->kind == N_INDEX ? n->a : n;
    if (name && name->kind == N_IDENT &&
        !local_name(c, name->str, name->len, 1)) {
        Select* s = find_select(c->sels, name->str, name->len, 1);
        if (s) replace_name(name, s->type_name);
    }
    if (n->kind == N_INDEX) {
        lower_selected_type_ref(n->b, c);
        for (int i = 0; i < n->nkids; i++) lower_selected_type_ref(n->kids[i], c);
    }
}

static void lower_selected_block(Node* b, SelectLower* c) {
    select_scope_push(c);
    for (int i = 0; i < b->nkids; i++) lower_selected_node(b->kids[i], c);
    select_scope_pop(c);
}

static void lower_selected_func(Node* f, SelectLower* c) {
    select_scope_push(c);
    if (f->c) {
        for (int i = 0; i < f->c->nkids; i++)
            bind_local(c, f->c->kids[i]->str, f->c->kids[i]->len, 1);
    }
    if (f->d) lower_selected_type(f->d, c);
    for (int i = 0; i < f->nkids; i++) lower_selected_type(f->kids[i]->a, c);
    lower_selected_type(f->a, c);
    select_scope_push(c);
    if (f->flags & F_METHOD) bind_local(c, "self", 4, 0);
    for (int i = 0; i < f->nkids; i++)
        bind_local(c, f->kids[i]->str, f->kids[i]->len, 0);
    if (f->b) lower_selected_block(f->b, c);
    select_scope_pop(c);
    select_scope_pop(c);
}

static void lower_selected_node(Node* n, SelectLower* c) {
    if (!n) return;
    switch (n->kind) {
    case N_IMPORT:
        return;
    case N_IDENT:
        if (!local_name(c, n->str, n->len, 0)) {
            Select* s = find_select(c->sels, n->str, n->len, 0);
            if (!s) s = find_select(c->sels, n->str, n->len, 1);
            if (s) replace_name(n, s->value_name ? s->value_name : s->type_name);
        }
        return;
    case N_NAMED:
        lower_selected_type(n, c);
        return;
    case N_FUNC:
        lower_selected_func(n, c);
        return;
    case N_BLOCK:
        lower_selected_block(n, c);
        return;
    case N_VAR:
        for (int i = 0; i < n->nkids; i++) lower_selected_type(n->kids[i]->a, c);
        lower_selected_node(n->b, c); // initializer precedes the new bindings
        for (int i = 0; i < n->nkids; i++)
            bind_local(c, n->kids[i]->str, n->kids[i]->len, 0);
        return;
    case N_FOR:
        if (n->flags & F_FORGEN) {
            // General head: one select scope covers init, cond, post, and
            // body; a head declaration binds through its N_VAR statement.
            select_scope_push(c);
            lower_selected_node(n->a, c);
            lower_selected_node(n->b, c);
            lower_selected_node(n->c, c);
            lower_selected_node(n->d, c);
            select_scope_pop(c);
            return;
        }
        for (int i = 0; i < n->nkids; i++)
            lower_selected_type(n->kids[i]->a, c);
        lower_selected_node(n->b, c);
        select_scope_push(c);
        for (int i = 0; i < n->nkids; i++)
            bind_local(c, n->kids[i]->str, n->kids[i]->len, 0);
        lower_selected_node(n->c, c);
        select_scope_pop(c);
        return;
    case N_STRUCT: case N_BITS: case N_CONTRACT: case N_ALIAS: case N_UNION:
        select_scope_push(c);
        if (n->c)
            for (int i = 0; i < n->c->nkids; i++)
                bind_local(c, n->c->kids[i]->str, n->c->kids[i]->len, 1);
        if (n->kind == N_ALIAS || n->kind == N_BITS) lower_selected_type(n->a, c);
        else if (n->kind == N_UNION) {
            for (int i = 0; i < n->nkids; i++)
                lower_selected_type(n->kids[i]->a, c); // variant payload types
        }
        else for (int i = 0; i < n->nkids; i++) {
            Node* member = n->kids[i];
            for (int j = 0; j < member->nkids; j++)
                lower_selected_type(member->kids[j]->a, c);
            lower_selected_type(member->a, c);
            lower_selected_node(member->c, c);
        }
        select_scope_pop(c);
        return;
    case N_DISTINCT:
        lower_selected_type(n->a, c);
        return;
    case N_ERROR_DOMAIN:
        return;
    case N_ENUM:
        lower_selected_type(n->a, c);
        for (int i = 0; i < n->nkids; i++)
            lower_selected_node(n->kids[i]->a, c);
        return; // member names themselves are declarations, not references
    case N_SIGNING:
        select_scope_push(c);
        if (n->c)
            for (int i = 0; i < n->c->nkids; i++)
                bind_local(c, n->c->kids[i]->str, n->c->kids[i]->len, 1);
        lower_selected_type(n->a, c);
        for (int i = 0; i < n->nkids; i++) lower_selected_type(n->kids[i]->a, c);
        select_scope_pop(c);
        return;
    case N_EXTERN:
        for (int i = 0; i < n->nkids; i++) {
            Node* f = n->kids[i];
            for (int j = 0; j < f->nkids; j++) lower_selected_type(f->kids[j]->a, c);
            lower_selected_type(f->a, c);
        }
        return;
    case N_FIELD_E:
        lower_selected_node(n->a, c); // field spelling is member-local
        return;
    case N_FIELDINIT:
        lower_selected_node(n->a, c); // initializer name is struct-local
        return;
    case N_STRUCTLIT:
        lower_selected_type_ref(n->a, c);
        for (int i = 0; i < n->nkids; i++) lower_selected_node(n->kids[i], c);
        return;
    case N_CALL:
        if (n->a && n->a->kind == N_IDENT &&
            !local_name(c, n->a->str, n->a->len, 0) &&
            !find_select(c->sels, n->a->str, n->a->len, 0) &&
            (text_is(n->a->str, n->a->len, "sizeof") ||
             text_is(n->a->str, n->a->len, "alignof") ||
             text_is(n->a->str, n->a->len, "offsetof") ||
             text_is(n->a->str, n->a->len, "typeid"))) {
            if (n->nkids) lower_selected_type_ref(n->kids[0], c);
            return;
        }
        break;
    case N_ASM:
        for (int i = 0; i < n->nkids; i++) {
            Node* b = n->kids[i];
            int clob = b->len == 8 && memcmp(b->str, "clobbers", 8) == 0;
            if (!clob)
                for (int j = 1; j < b->nkids; j += 2)
                    lower_selected_node(b->kids[j], c);
        }
        return;
    default:
        break;
    }
    lower_selected_node(n->a, c);
    lower_selected_node(n->b, c);
    lower_selected_node(n->c, c);
    lower_selected_node(n->d, c);
    for (int i = 0; i < n->nkids; i++) lower_selected_node(n->kids[i], c);
}

static int decl_namespace(Node* n) {
    if (n->kind == N_FUNC && !(n->flags & F_METHOD)) return 1;
    if (n->kind == N_VAR || n->kind == N_EXTERN) return 1;
    if (n->kind == N_STRUCT || n->kind == N_BITS || n->kind == N_CONTRACT || n->kind == N_ENUM ||
        n->kind == N_ALIAS || n->kind == N_DISTINCT ||
        n->kind == N_ERROR_DOMAIN || n->kind == N_UNION)
        return 2;
    return 0;
}

static void check_select_collisions(Loader* L, Node* file, Binding* binds,
                                    Select* sels) {
    for (Select* s = sels; s; s = s->next) {
        for (Binding* b = binds; b; b = b->next)
            if (s->value_name && b->len == s->len &&
                memcmp(b->name, s->name, s->len) == 0)
                import_err(L->d, s->node,
                           "import binding '%.*s' collides with another import",
                           (int)s->len, s->name);
        for (int i = 0; i < file->nkids; i++) {
            Node* n = file->kids[i];
            int ns = decl_namespace(n);
            if (!ns || n->kind == N_IMPORT) continue;
            if (n->kind == N_EXTERN) {
                if (!s->value_name) continue;
                for (int j = 0; j < n->nkids; j++)
                    if (n->kids[j]->len == s->len &&
                        memcmp(n->kids[j]->str, s->name, s->len) == 0)
                        import_err(L->d, s->node,
                                   "imported name '%.*s' collides with a declaration",
                                   (int)s->len, s->name);
                continue;
            }
            Node* named = n->kind == N_VAR ? n->kids[0] : n;
            int conflicts = (ns == 1 && s->value_name) || (ns == 2 && s->type_name);
            if (conflicts && named->len == s->len &&
                memcmp(named->str, s->name, s->len) == 0)
                import_err(L->d, s->node,
                           "imported name '%.*s' collides with a declaration",
                           (int)s->len, s->name);
        }
    }
}

// --- module-private identities ---------------------------------------------------------

typedef struct {
    const char* from;
    uint32_t    flen;
    const char* to;
} Rename;

typedef struct {
    Rename* v;
    int     n, cap;
} Renames;

static void renames_add(Renames* rs, const char* from, uint32_t flen, const char* to) {
    if (rs->n == rs->cap) {
        rs->cap = rs->cap ? rs->cap * 2 : 16;
        rs->v = realloc(rs->v, (size_t)rs->cap * sizeof(Rename));
        if (!rs->v) exit(1);
    }
    rs->v[rs->n].from = from;
    rs->v[rs->n].flen = flen;
    rs->v[rs->n].to = to;
    rs->n++;
}

static const char* prefixed(Module* m, const char* name, uint32_t len) {
    size_t plen = strlen(m->prefix);
    char* qn = arena_alloc(plen + len + 1);
    memcpy(qn, m->prefix, plen);
    memcpy(qn + plen, name, len);
    qn[plen + len] = 0;
    return qn;
}

static const char* arena_stringn(const char* s, uint32_t len) {
    char* out = arena_alloc((size_t)len + 1);
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

// Collect every module-level declaration name (free functions, globals,
// types, extern C bindings) and rename the declaration nodes. Methods derive
// their private identity from their renamed owner type.
// Externs/exported functions keep their linker spelling in Node.cname; error
// domains keep their unqualified source spelling there for Error.name.
static void rename_decls(Module* m, Renames* rs) {
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        Node* named = n;
        if (n->kind == N_VAR) named = n->kids[0];
        if (n->kind == N_EXTERN) {
            for (int j = 0; j < n->nkids; j++) {
                Node* f = n->kids[j];
                renames_add(rs, f->str, f->len, prefixed(m, f->str, f->len));
                if (!f->cname) // an explicit `= "symbol"` link name wins
                    f->cname = arena_stringn(f->str, f->len);
                f->str = rs->v[rs->n - 1].to;
                f->len = (uint32_t)strlen(f->str);
            }
            continue;
        }
        if (n->kind == N_FUNC && (n->flags & F_METHOD)) continue;
        if (n->kind != N_FUNC && n->kind != N_VAR &&
            n->kind != N_STRUCT && n->kind != N_BITS && n->kind != N_CONTRACT && n->kind != N_ENUM &&
            n->kind != N_ALIAS && n->kind != N_DISTINCT &&
            n->kind != N_ERROR_DOMAIN && n->kind != N_UNION)
            continue;
        if (n->kind == N_ERROR_DOMAIN && !n->cname)
            n->cname = arena_stringn(named->str, named->len);
        if ((n->flags & F_EXPO) && !n->cname) // `export` keeps its C name
            n->cname = arena_stringn(named->str, named->len);
        renames_add(rs, named->str, named->len, prefixed(m, named->str, named->len));
        named->str = rs->v[rs->n - 1].to;
        named->len = (uint32_t)strlen(named->str);
    }
}

static Rename* find_rename(Renames* rs, const char* name, uint32_t len) {
    for (int i = 0; i < rs->n; i++)
        if (rs->v[i].flen == len && memcmp(rs->v[i].from, name, len) == 0)
            return &rs->v[i];
    return NULL;
}

static void rename_type_refs(Node* t, Renames* rs, SelectLower* c) {
    if (!t) return;
    if (t->kind == N_NAMED && !local_name(c, t->str, t->len, 1)) {
        Rename* r = find_rename(rs, t->str, t->len);
        if (r) replace_name(t, r->to);
    }
    rename_type_refs(t->a, rs, c);
    if (t->kind == N_STATICMAP_T) rename_type_refs(t->b, rs, c);
    for (int i = 0; i < t->nkids; i++) rename_type_refs(t->kids[i], rs, c);
}

static void rename_refs(Node* n, Renames* rs, SelectLower* c);

static void rename_block_refs(Node* b, Renames* rs, SelectLower* c) {
    select_scope_push(c);
    for (int i = 0; i < b->nkids; i++) rename_refs(b->kids[i], rs, c);
    select_scope_pop(c);
}

static void rename_func_refs(Node* f, Renames* rs, SelectLower* c) {
    select_scope_push(c);
    if (f->c) {
        for (int i = 0; i < f->c->nkids; i++)
            bind_local(c, f->c->kids[i]->str, f->c->kids[i]->len, 1);
    }
    if (f->d) rename_type_refs(f->d, rs, c);
    for (int i = 0; i < f->nkids; i++) rename_type_refs(f->kids[i]->a, rs, c);
    rename_type_refs(f->a, rs, c);
    select_scope_push(c);
    if (f->flags & F_METHOD) bind_local(c, "self", 4, 0);
    for (int i = 0; i < f->nkids; i++)
        bind_local(c, f->kids[i]->str, f->kids[i]->len, 0);
    if (f->b) rename_block_refs(f->b, rs, c);
    select_scope_pop(c);
    select_scope_pop(c);
}

static void rename_refs(Node* n, Renames* rs, SelectLower* c) {
    if (!n) return;
    switch (n->kind) {
    case N_IDENT:
        if (!local_name(c, n->str, n->len, 0)) {
            Rename* r = find_rename(rs, n->str, n->len);
            if (r) replace_name(n, r->to);
        }
        return; // leaf: no children hold references
    case N_NAMED:
        rename_type_refs(n, rs, c);
        return;
    case N_FUNC:
        rename_func_refs(n, rs, c);
        return;
    case N_BLOCK:
        rename_block_refs(n, rs, c);
        return;
    case N_VAR:
        for (int i = 0; i < n->nkids; i++) rename_type_refs(n->kids[i]->a, rs, c);
        rename_refs(n->b, rs, c); // initializer precedes the new bindings
        for (int i = 0; i < n->nkids; i++)
            bind_local(c, n->kids[i]->str, n->kids[i]->len, 0);
        return;
    case N_FOR:
        if (n->flags & F_FORGEN) {
            select_scope_push(c);
            rename_refs(n->a, rs, c);
            rename_refs(n->b, rs, c);
            rename_refs(n->c, rs, c);
            rename_refs(n->d, rs, c);
            select_scope_pop(c);
            return;
        }
        for (int i = 0; i < n->nkids; i++) rename_type_refs(n->kids[i]->a, rs, c);
        rename_refs(n->b, rs, c);
        select_scope_push(c);
        for (int i = 0; i < n->nkids; i++)
            bind_local(c, n->kids[i]->str, n->kids[i]->len, 0);
        rename_refs(n->c, rs, c);
        select_scope_pop(c);
        return;
    case N_STRUCT: case N_BITS: case N_CONTRACT: case N_ALIAS: case N_UNION:
        select_scope_push(c);
        if (n->c)
            for (int i = 0; i < n->c->nkids; i++)
                bind_local(c, n->c->kids[i]->str, n->c->kids[i]->len, 1);
        if (n->kind == N_ALIAS || n->kind == N_BITS) rename_type_refs(n->a, rs, c);
        else if (n->kind == N_UNION) {
            for (int i = 0; i < n->nkids; i++)
                rename_type_refs(n->kids[i]->a, rs, c);
        } else for (int i = 0; i < n->nkids; i++) {
            Node* member = n->kids[i];
            for (int j = 0; j < member->nkids; j++)
                rename_type_refs(member->kids[j]->a, rs, c);
            rename_type_refs(member->a, rs, c);
        }
        select_scope_pop(c);
        return;
    case N_DISTINCT:
        rename_type_refs(n->a, rs, c);
        return;
    case N_ENUM:
        rename_type_refs(n->a, rs, c);
        for (int i = 0; i < n->nkids; i++)
            rename_refs(n->kids[i]->a, rs, c);
        return;
    case N_ERROR_DOMAIN:
        return;
    case N_FIELD_E:
        rename_refs(n->a, rs, c); // field spelling is member-local
        return;
    default:
        break;
    }
    rename_refs(n->a, rs, c);
    rename_refs(n->b, rs, c);
    rename_refs(n->c, rs, c);
    rename_refs(n->d, rs, c);
    for (int i = 0; i < n->nkids; i++) rename_refs(n->kids[i], rs, c);
}

// --- per-file resolution ----------------------------------------------------------------

static int resolve_file(Loader* L, Node* file, const char* src_path, int is_root) {
    Binding* binds = NULL;
    Select* sels = NULL;
    const char* source_canon = canonicalize(src_path);
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        if (n->extern_component &&
            (is_root || !source_canon || !is_std_source(L, source_canon))) {
            import_err(L->d, n,
                       "@extern is reserved for compiler-shipped standard-library sources");
        }
    }
    if (L->d->errors) return L->d->errors;
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        if (n->kind != N_IMPORT) break; // imports precede declarations (parser)
        Module* mod = ensure_module(L, n, src_path);
        if (!mod) continue;
        if (n->flags & F_SELECT) select_members(L, n, mod, binds, &sels);
        else bind(L, n, mod, &binds, sels);
    }
    if (L->d->errors) return L->d->errors;

    check_select_collisions(L, file, binds, sels);
    if (L->d->errors) return L->d->errors;

    if (!is_root) {
        // A module's bindings live only in its own file; a binding that
        // collides with one of its own declarations would silently lose the
        // import once the binding nodes are dropped at the merge.
        for (Binding* b = binds; b; b = b->next) {
            for (int i = 0; i < file->nkids; i++) {
                Node* n = file->kids[i];
                if (n->kind == N_IMPORT) continue;
                Node* named = n->kind == N_VAR ? n->kids[0] : n;
                if (n->kind == N_FUNC || n->kind == N_VAR ||
                    n->kind == N_STRUCT || n->kind == N_CONTRACT ||
                    n->kind == N_ENUM || n->kind == N_BITS || n->kind == N_ALIAS ||
                    n->kind == N_DISTINCT || n->kind == N_ERROR_DOMAIN ||
                    n->kind == N_EXTERN ||
                    n->kind == N_UNION) {
                    if (n->kind == N_EXTERN) {
                        for (int j = 0; j < n->nkids; j++)
                            if (text_is(n->kids[j]->str, n->kids[j]->len, b->name))
                                import_err(L->d, b->node,
                                           "import binding '%.*s' collides with a declaration in this module",
                                           (int)b->len, b->name);
                        continue;
                    }
                    if (!(n->kind == N_FUNC && (n->flags & F_METHOD)) &&
                        n->kind != N_SIGNING && text_is(named->str, named->len, b->name))
                        import_err(L->d, b->node,
                                   "import binding '%.*s' collides with a declaration in this module",
                                   (int)b->len, b->name);
                }
            }
        }
        if (L->d->errors) return L->d->errors;
    }

    // lower qualified references with this file's bindings
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind != N_IMPORT)
            lower_qualified(L, file->kids[i], binds);
    if (L->d->errors) return L->d->errors;

    for (int i = 0; i < file->nkids; i++) {
        if (file->kids[i]->kind == N_IMPORT) continue;
        SelectLower c = {0};
        c.sels = sels;
        lower_selected_node(file->kids[i], &c);
    }

    if (!is_root) {
        Module* self = L->loading; // the module being resolved is stack top
        Renames rs = {0};
        rename_decls(self, &rs);
        for (int i = 0; i < file->nkids; i++) {
            SelectLower c = {0};
            rename_refs(file->kids[i], &rs, &c);
        }
        free(rs.v);
        // Drop this module's import nodes: qualified bindings and selected
        // aliases have both been lowered to private target identities.
        int ndecls = 0;
        for (int i = 0; i < file->nkids; i++)
            if (file->kids[i]->kind != N_IMPORT) ndecls++;
        Node** kids = (Node**)arena_alloc((size_t)ndecls * sizeof(Node*));
        int at = 0;
        for (int i = 0; i < file->nkids; i++)
            if (file->kids[i]->kind != N_IMPORT) kids[at++] = file->kids[i];
        file->kids = kids;
        file->nkids = ndecls;
        self->binds = binds;
    }
    return L->d->errors;
}

// --- entry -------------------------------------------------------------------------------

int resolve_imports(Node* file, Diag* d, const char* compiler_path,
                    const char* target,
                    CompileReport* report) {
    Loader L = {0};
    L.d = d;
    L.compiler_path = compiler_path;
    char compiler[4096];
    compiler_dir(compiler, sizeof compiler, compiler_path);
    char std_path[4608];
    if (strcmp(compiler, ".") == 0) snprintf(std_path, sizeof std_path, "std");
    else snprintf(std_path, sizeof std_path, "%s/std", compiler);
    L.std_root = canonicalize(std_path);
    L.target = target;
    L.report = report;

    const char* root_canon = canonicalize(d->path);
    if (!root_canon) root_canon = arena_string(d->path);

    // The root module itself is on the cycle-detection stack so a self-import
    // or root-involving cycle reports the full chain.
    Module root = {0};
    root.canon = root_canon;
    root.file = file;
    root.prefix = "";
    L.loading = &root;
    if (resolve_file(&L, file, d->path, 1)) return d->errors;
    L.loading = NULL;

    // collect completed modules; L->mods is newest-first, so reverse into
    // dependency order (dependencies complete before their dependents)
    int nmods = 0;
    for (Module* m = L.mods; m; m = m->next) nmods++;
    Module** order = malloc((size_t)(nmods ? nmods : 1) * sizeof(Module*));
    if (!order) exit(1);
    for (int i = nmods - 1; i >= 0; i--) {
        order[i] = L.mods;
        L.mods = L.mods->next;
    }

    // Merge: root whole-module bindings, then dependency declarations, then
    // root declarations. Selective root imports were already lowered and do
    // not create module bindings for semantic checking.
    int nroot_imports = 0, nroot_decls = 0, nmod_decls = 0;
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind == N_IMPORT) {
            if (!(file->kids[i]->flags & F_SELECT)) nroot_imports++;
        } else nroot_decls++;
    for (int i = 0; i < nmods; i++) nmod_decls += order[i]->file->nkids;

    int total = nroot_imports + nmod_decls + nroot_decls;
    Node** kids = (Node**)arena_alloc((size_t)total * sizeof(Node*));
    int at = 0;
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind == N_IMPORT &&
            !(file->kids[i]->flags & F_SELECT)) kids[at++] = file->kids[i];
    for (int i = 0; i < nmods; i++)
        for (int j = 0; j < order[i]->file->nkids; j++)
            kids[at++] = order[i]->file->kids[j];
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind != N_IMPORT) kids[at++] = file->kids[i];
    file->kids = kids;
    file->nkids = total;
    free(order);
    free(L.prefixes);
    return d->errors;
}
