// module.c — general import resolution for the merged compilation unit.
//
// Modules are files. The loader recursively loads what each file imports —
// bare names from the stdlib beside the compiler, local files by a path
// relative to the importing source — canonicalizes identity by realpath so
// every file loads exactly once through any number of imports or aliases,
// and diagnoses cycles with the full chain. Each loaded module's surface is
// lowered into one private module-prefixed namespace (`name$member`; `$`
// cannot appear in source identifiers): declaration names and their in-module
// references are renamed together, qualified references in the importing file
// are rewritten to the prefixed names after a `pub` check, and extern
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
    Node* file = parse(toks, ntok, &d);
    if (d.errors) { L->d->errors += d.errors; return NULL; }
    return file;
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

static void collect_members(Module* m) {
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        switch (n->kind) {
        case N_FUNC:
            if (!(n->flags & F_METHOD))
                add_member(m, n->str, n->len, 0, (n->flags & F_PUB) != 0);
            break;
        case N_VAR:
            add_member(m, n->kids[0]->str, n->kids[0]->len, 0,
                       (n->flags & F_PUB) != 0);
            break;
        case N_STRUCT: case N_CONTRACT: case N_ENUM:
        case N_ALIAS: case N_DISTINCT:
            add_member(m, n->str, n->len, 1, (n->flags & F_PUB) != 0);
            break;
        case N_EXTERN: // C bindings are module-private
            for (int j = 0; j < n->nkids; j++)
                add_member(m, n->kids[j]->str, n->kids[j]->len, 0, 0);
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

// Unique `name$` prefix per module; `$` never occurs in source identifiers,
// so prefixed names cannot collide with root declarations, and distinct
// modules can never share a prefixed member name.
static const char* assign_prefix(Loader* L, const char* canon) {
    const char* base = strrchr(canon, '/');
    base = base ? base + 1 : canon;
    size_t blen = strlen(base);
    if (blen > 4 && strcmp(base + blen - 4, ".ace") == 0) blen -= 4;
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
// Bare and ident-aliased imports come from the stdlib beside the compiler;
// string paths are local and relative to the importing source file.
static const char* import_target(Loader* L, Node* imp, const char* src_path) {
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
    // bare `import fmt` or alias `import f: fmt` — stdlib only
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
    char path[4608];
    if (strcmp(dir, ".") == 0) snprintf(path, sizeof path, "std/%s.ace", stdname);
    else snprintf(path, sizeof path, "%s/std/%s.ace", dir, stdname);
    if (!canonicalize(path)) {
        import_err(L->d, imp, "standard library module '%s' not found (expected %s)",
                   stdname, path);
        return NULL;
    }
    return arena_string(path);
}

static void bind(Loader* L, Node* imp, Module* mod, Binding** binds) {
    for (Binding* b = *binds; b; b = b->next) {
        if (b->len == imp->len && memcmp(b->name, imp->str, imp->len) == 0) {
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
    if (module_on_stack(L, canon)) {
        report_cycle(L, imp, canon);
        return NULL;
    }
    for (Module* m = L->mods; m; m = m->next)
        if (strcmp(m->canon, canon) == 0) return m; // loaded once, aliased freely
    Node* file = load_module(L, canon);
    if (!file) return NULL;
    Module* m = module_new(L, canon, file);
    collect_members(m); // original names + pub-ness, before any renaming
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
// Externs and exported functions keep their linker spelling in Node.cname.
static void rename_decls(Module* m, Renames* rs) {
    for (int i = 0; i < m->file->nkids; i++) {
        Node* n = m->file->kids[i];
        Node* named = n;
        if (n->kind == N_VAR) named = n->kids[0];
        if (n->kind == N_EXTERN) {
            for (int j = 0; j < n->nkids; j++) {
                Node* f = n->kids[j];
                renames_add(rs, f->str, f->len, prefixed(m, f->str, f->len));
                f->cname = arena_stringn(f->str, f->len);
                f->str = rs->v[rs->n - 1].to;
                f->len = (uint32_t)strlen(f->str);
            }
            continue;
        }
        if (n->kind == N_FUNC && (n->flags & F_METHOD)) continue;
        if (n->kind != N_FUNC && n->kind != N_VAR &&
            n->kind != N_STRUCT && n->kind != N_CONTRACT && n->kind != N_ENUM &&
            n->kind != N_ALIAS && n->kind != N_DISTINCT)
            continue;
        if ((n->flags & F_EXPO) && !n->cname) // `export` keeps its C name
            n->cname = arena_stringn(named->str, named->len);
        renames_add(rs, named->str, named->len, prefixed(m, named->str, named->len));
        named->str = rs->v[rs->n - 1].to;
        named->len = (uint32_t)strlen(named->str);
    }
}

static void rename_refs(Node* n, Renames* rs) {
    if (!n) return;
    if (n->kind == N_IDENT) {
        for (int i = 0; i < rs->n; i++) {
            if (rs->v[i].flen == n->len && memcmp(rs->v[i].from, n->str, n->len) == 0) {
                n->str = rs->v[i].to;
                n->len = (uint32_t)strlen(rs->v[i].to);
                break;
            }
        }
        return; // leaf: no children hold references
    }
    if (n->kind == N_NAMED) { // rename the type name; generic args still walk
        for (int i = 0; i < rs->n; i++) {
            if (rs->v[i].flen == n->len && memcmp(rs->v[i].from, n->str, n->len) == 0) {
                n->str = rs->v[i].to;
                n->len = (uint32_t)strlen(rs->v[i].to);
                break;
            }
        }
    }
    if (n->kind == N_ENUM) return; // enum member names are type-scoped, keep them
    rename_refs(n->a, rs);
    rename_refs(n->b, rs);
    rename_refs(n->c, rs);
    rename_refs(n->d, rs);
    for (int i = 0; i < n->nkids; i++) rename_refs(n->kids[i], rs);
}

// --- per-file resolution ----------------------------------------------------------------

static int resolve_file(Loader* L, Node* file, const char* src_path, int is_root) {
    Binding* binds = NULL;
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        if (n->kind != N_IMPORT) break; // imports precede declarations (parser)
        Module* mod = ensure_module(L, n, src_path);
        if (!mod) continue;
        bind(L, n, mod, &binds);
    }
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
                    n->kind == N_ENUM || n->kind == N_ALIAS ||
                    n->kind == N_DISTINCT || n->kind == N_EXTERN) {
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

    if (!is_root) {
        Module* self = L->loading; // the module being resolved is stack top
        Renames rs = {0};
        rename_decls(self, &rs);
        for (int i = 0; i < file->nkids; i++)
            rename_refs(file->kids[i], &rs);
        free(rs.v);
        // drop the module's import nodes: the bindings never enter the
        // merged namespace
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
                    CompileReport* report) {
    Loader L = {0};
    L.d = d;
    L.compiler_path = compiler_path;
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

    // merge: root imports, then every module's declarations in dependency
    // order, then the root declarations
    int nroot_imports = 0, nroot_decls = 0, nmod_decls = 0;
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind == N_IMPORT) nroot_imports++;
        else nroot_decls++;
    for (int i = 0; i < nmods; i++) nmod_decls += order[i]->file->nkids;

    int total = nroot_imports + nmod_decls + nroot_decls;
    Node** kids = (Node**)arena_alloc((size_t)total * sizeof(Node*));
    int at = 0;
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind == N_IMPORT) kids[at++] = file->kids[i];
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
