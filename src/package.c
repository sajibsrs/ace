// package.c — ace.pkg manifest parsing and local-path dependency-graph
// resolution (Package System v1, SPEC.md Decision Queue). A manifest is a
// small line-based key: value format, plus one [dependencies] section of
// name: relative/path entries. Dependency paths are resolved from the
// declaring ace.pkg and may traverse upward (packages are ordinary sibling
// directories, not nested inside one another); the root's own root file must
// stay within its package directory. No codegen or import-surface wiring
// happens here.
#include "ace.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { const char* alias; const char* path; int line; } RawDep;

typedef struct {
    const char* name; int name_line;
    const char* version; int version_line;
    const char* ace; int ace_line;
    const char* kind; int kind_line;
    const char* root; int root_line;
    const char* description; int description_line;
    RawDep* deps; int ndeps, cdeps;
} RawManifest;

typedef struct NameEntry NameEntry;
struct NameEntry { const char* name; const char* dir; NameEntry* next; };

typedef struct {
    Diag*       d;
    const char* compiler_path;
    Package*    loading; // in-progress stack (cycle detection)
    Package*    loaded;  // completed, newest-first
    NameEntry*  names;   // declared package name -> its canonical directory
    int         have_ace_version;
    int         ace_major, ace_minor; // the root's, shared by the whole graph
} PackageLoader;

static void package_err(PackageLoader* L, const char* manifest, int line, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", manifest, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    L->d->errors++;
}

static const char* package_strndup(const char* s, size_t len) {
    char* out = arena_alloc(len + 1);
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

static const char* trim(const char* s, size_t len, size_t* out_len) {
    while (len && (s[0] == ' ' || s[0] == '\t')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) len--;
    *out_len = len;
    return s;
}

static int is_ident(const char* s, size_t len) {
    if (!len) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 0; i < len; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

int package_valid_name(const char* s, size_t len) {
    if (!is_ident(s, len)) return 0;
    for (size_t i = 0; i < len; i++)
        if (isupper((unsigned char)s[i])) return 0;
    return 1;
}

static int parse_uint_component(const char* s, size_t len, int* out) {
    if (!len) return 0;
    long v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
        if (v > 0x7fffffff) return 0;
    }
    *out = (int)v;
    return 1;
}

// Exactly "major.minor.patch" — three nonnegative integers, two dots.
static int parse_version(const char* s, size_t len, int* a, int* b, int* c) {
    const char* p1 = memchr(s, '.', len);
    if (!p1) return 0;
    size_t off1 = (size_t)(p1 - s);
    const char* p2 = memchr(p1 + 1, '.', len - off1 - 1);
    if (!p2) return 0;
    size_t off2 = (size_t)(p2 - s);
    if (memchr(p2 + 1, '.', len - off2 - 1)) return 0;
    return parse_uint_component(s, off1, a) &&
           parse_uint_component(p1 + 1, off2 - off1 - 1, b) &&
           parse_uint_component(p2 + 1, len - off2 - 1, c);
}

// Exactly "major.minor" — two nonnegative integers, one dot.
static int parse_ace_version(const char* s, size_t len, int* a, int* b) {
    const char* p1 = memchr(s, '.', len);
    if (!p1) return 0;
    size_t off1 = (size_t)(p1 - s);
    if (memchr(p1 + 1, '.', len - off1 - 1)) return 0;
    return parse_uint_component(s, off1, a) && parse_uint_component(p1 + 1, len - off1 - 1, b);
}

// A relative path with no ".." component, ending in ".ace" — the shape a
// package's own root file must have (it cannot leave the package directory).
static int path_is_local_ace(const char* s, size_t len) {
    if (!len || s[0] == '/') return 0;
    if (len < 4 || memcmp(s + len - 4, ".ace", 4) != 0) return 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '.' && i + 1 < len && s[i + 1] == '.' &&
            (i == 0 || s[i - 1] == '/') && (i + 2 == len || s[i + 2] == '/'))
            return 0;
    }
    return 1;
}

// A relative directory path — dependency paths may traverse upward (".."),
// since packages are ordinary sibling directories on disk.
static int path_is_relative(const char* s, size_t len) {
    return len > 0 && s[0] != '/';
}

static void add_raw_dep(RawManifest* rm, const char* alias, const char* path, int line) {
    if (rm->ndeps == rm->cdeps) {
        rm->cdeps = rm->cdeps ? rm->cdeps * 2 : 8;
        rm->deps = realloc(rm->deps, (size_t)rm->cdeps * sizeof(RawDep));
        if (!rm->deps) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
    }
    rm->deps[rm->ndeps].alias = alias;
    rm->deps[rm->ndeps].path = path;
    rm->deps[rm->ndeps].line = line;
    rm->ndeps++;
}

// Parse one ace.pkg's text into its raw (unvalidated) fields and dependency
// entries. Diagnoses malformed lines but keeps scanning so one pass reports
// every problem in the file.
static int parse_manifest(PackageLoader* L, const char* manifest_path, RawManifest* rm) {
    Source s;
    if (source_load(&s, manifest_path) != 0) { L->d->errors++; return 0; }
    memset(rm, 0, sizeof *rm);
    int line = 0, in_deps = 0, ok = 1;
    const char* p = s.buf;
    const char* end = s.buf + s.len;
    while (p < end) {
        line++;
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        const char* eol = nl ? nl : end;
        size_t raw_len = (size_t)(eol - p);
        size_t tlen;
        const char* t = trim(p, raw_len, &tlen);
        p = nl ? nl + 1 : end;
        if (tlen == 0) continue;
        if (tlen >= 2 && t[0] == '/' && t[1] == '/') continue; // comment
        if (tlen == 14 && memcmp(t, "[dependencies]", 14) == 0) { in_deps = 1; continue; }
        if (t[0] == '[') {
            package_err(L, manifest_path, line, "unknown section '%.*s'", (int)tlen, t);
            ok = 0;
            continue;
        }
        const char* colon = memchr(t, ':', tlen);
        if (!colon) {
            package_err(L, manifest_path, line, "expected 'key: value'");
            ok = 0;
            continue;
        }
        size_t klen = (size_t)(colon - t);
        size_t kt;
        const char* key = trim(t, klen, &kt);
        size_t vraw = tlen - klen - 1;
        size_t vlen;
        const char* val = trim(colon + 1, vraw, &vlen);
        if (!kt || !vlen) {
            package_err(L, manifest_path, line, "expected 'key: value'");
            ok = 0;
            continue;
        }
        if (in_deps) {
            for (int i = 0; i < rm->ndeps; i++)
                if (strlen(rm->deps[i].alias) == kt && memcmp(rm->deps[i].alias, key, kt) == 0) {
                    package_err(L, manifest_path, line, "duplicate dependency '%.*s'", (int)kt, key);
                    ok = 0;
                }
            add_raw_dep(rm, package_strndup(key, kt), package_strndup(val, vlen), line);
            continue;
        }
#define FIELD(lit, slot, lslot)                                                \
        if (text_is(key, (uint32_t)kt, lit)) {                                \
            if (rm->slot) { package_err(L, manifest_path, line,                   \
                "duplicate field '" lit "'"); ok = 0; }                       \
            else { rm->slot = package_strndup(val, vlen); rm->lslot = line; }      \
            continue;                                                         \
        }
        FIELD("name", name, name_line)
        FIELD("version", version, version_line)
        FIELD("ace", ace, ace_line)
        FIELD("kind", kind, kind_line)
        FIELD("root", root, root_line)
        FIELD("description", description, description_line)
#undef FIELD
        package_err(L, manifest_path, line, "unknown field '%.*s'", (int)kt, key);
        ok = 0;
    }
    return ok;
}

static int on_stack(PackageLoader* L, const char* canon) {
    for (Package* p = L->loading; p; p = p->next)
        if (strcmp(p->dir, canon) == 0) return 1;
    return 0;
}

static Package* already_loaded(PackageLoader* L, const char* canon) {
    for (Package* p = L->loaded; p; p = p->next)
        if (strcmp(p->dir, canon) == 0) return p;
    return NULL;
}

static void report_cycle(PackageLoader* L, const char* manifest, int line, const char* canon) {
    size_t cap = strlen(canon) + 64;
    for (Package* p = L->loading; p; p = p->next) cap += strlen(p->dir) + 4;
    char* chain = malloc(cap);
    size_t off = 0;
    off += (size_t)snprintf(chain + off, cap - off, "%s", canon);
    for (Package* p = L->loading; p; p = p->next)
        off += (size_t)snprintf(chain + off, cap - off, " -> %s", p->dir);
    package_err(L, manifest, line, "package dependency cycle: %s", chain);
    free(chain);
}

static int is_std_module_name(const char* compiler_path, const char* name) {
    char dir[4096];
    compiler_dir(dir, sizeof dir, compiler_path);
    char flat[4608], directory[4608];
    if (strcmp(dir, ".") == 0) {
        snprintf(flat, sizeof flat, "std/%s.ace", name);
        snprintf(directory, sizeof directory, "std/%s/mod.ace", name);
    } else {
        snprintf(flat, sizeof flat, "%s/std/%s.ace", dir, name);
        snprintf(directory, sizeof directory, "%s/std/%s/mod.ace", dir, name);
    }
    return canonicalize(flat) != NULL || canonicalize(directory) != NULL;
}

static Package* package_load(PackageLoader* L, const char* dir_arg, int is_root,
                         const char* dep_manifest, int dep_line) {
    const char* canon = canonicalize(dir_arg);
    if (!canon) {
        package_err(L, dep_manifest ? dep_manifest : dir_arg, dep_line,
                "cannot find package directory '%s'", dir_arg);
        return NULL;
    }
    struct stat st;
    if (stat(canon, &st) != 0 || !S_ISDIR(st.st_mode)) {
        package_err(L, dep_manifest ? dep_manifest : dir_arg, dep_line,
                "'%s' is not a directory", canon);
        return NULL;
    }
    if (on_stack(L, canon)) {
        report_cycle(L, dep_manifest ? dep_manifest : canon, dep_line, canon);
        return NULL;
    }
    Package* existing = already_loaded(L, canon);
    if (existing) return existing;

    const char* manifest = join_path(canon, "ace.pkg");
    struct stat mst;
    if (stat(manifest, &mst) != 0) {
        package_err(L, dep_manifest ? dep_manifest : canon, dep_line,
                "no ace.pkg at '%s'", canon);
        return NULL;
    }

    RawManifest rm;
    int parse_ok = parse_manifest(L, manifest, &rm);
    if (!parse_ok) { free(rm.deps); return NULL; }

    int ok = 1;
    if (!rm.name) { package_err(L, manifest, 1, "missing required field 'name'"); ok = 0; }
    if (!rm.version) { package_err(L, manifest, 1, "missing required field 'version'"); ok = 0; }
    if (!rm.ace) { package_err(L, manifest, 1, "missing required field 'ace'"); ok = 0; }
    if (!rm.kind) { package_err(L, manifest, 1, "missing required field 'kind'"); ok = 0; }
    if (!rm.root) { package_err(L, manifest, 1, "missing required field 'root'"); ok = 0; }
    if (!ok) { free(rm.deps); return NULL; }

    if (!package_valid_name(rm.name, strlen(rm.name))) {
        package_err(L, manifest, rm.name_line,
                "package name '%s' must be a lowercase identifier", rm.name);
        ok = 0;
    }
    int vmaj = 0, vmin = 0, vpat = 0;
    if (!parse_version(rm.version, strlen(rm.version), &vmaj, &vmin, &vpat)) {
        package_err(L, manifest, rm.version_line,
                "version '%s' must be exactly major.minor.patch", rm.version);
        ok = 0;
    }
    int amaj = 0, amin = 0;
    if (!parse_ace_version(rm.ace, strlen(rm.ace), &amaj, &amin)) {
        package_err(L, manifest, rm.ace_line,
                "ace version '%s' must be exactly major.minor", rm.ace);
        ok = 0;
    } else {
        if (!L->have_ace_version) {
            L->have_ace_version = 1;
            L->ace_major = amaj;
            L->ace_minor = amin;
        } else if (amaj != L->ace_major || amin != L->ace_minor) {
            package_err(L, manifest, rm.ace_line,
                    "ace version %d.%d does not match the graph's %d.%d",
                    amaj, amin, L->ace_major, L->ace_minor);
            ok = 0;
        }
    }
    int is_app = 0;
    if (text_is(rm.kind, (uint32_t)strlen(rm.kind), "app")) is_app = 1;
    else if (text_is(rm.kind, (uint32_t)strlen(rm.kind), "lib")) is_app = 0;
    else {
        package_err(L, manifest, rm.kind_line, "kind '%s' must be 'app' or 'lib'", rm.kind);
        ok = 0;
    }
    if (!is_root && is_app) {
        package_err(L, manifest, rm.kind_line,
                "dependency '%s' is kind 'app'; only a lib package can be a dependency",
                rm.name);
        ok = 0;
    }
    if (!path_is_local_ace(rm.root, strlen(rm.root))) {
        package_err(L, manifest, rm.root_line,
                "root '%s' must be a relative path within the package, ending in '.ace'",
                rm.root);
        ok = 0;
    }
    const char* root_canon = NULL;
    if (ok) {
        root_canon = canonicalize(join_path(canon, rm.root));
        if (!root_canon || !(path_beneath(root_canon, canon) || strcmp(root_canon, canon) == 0)) {
            package_err(L, manifest, rm.root_line, "root '%s' does not exist", rm.root);
            ok = 0;
        }
    }
    if (ok && is_std_module_name(L->compiler_path, rm.name)) {
        package_err(L, manifest, rm.name_line,
                "package name '%s' collides with a standard-library module name", rm.name);
        ok = 0;
    }
    if (ok) {
        NameEntry* found = NULL;
        for (NameEntry* e = L->names; e; e = e->next)
            if (text_is(e->name, (uint32_t)strlen(e->name), rm.name)) { found = e; break; }
        if (found && strcmp(found->dir, canon) != 0) {
            package_err(L, manifest, rm.name_line,
                    "package name '%s' resolves to two different sources: %s and %s",
                    rm.name, found->dir, canon);
            ok = 0;
        } else if (!found) {
            NameEntry* e = (NameEntry*)arena_alloc(sizeof(NameEntry));
            e->name = arena_string(rm.name);
            e->dir = canon;
            e->next = L->names;
            L->names = e;
        }
    }
    for (int i = 0; i < rm.ndeps; i++) {
        if (!is_ident(rm.deps[i].alias, strlen(rm.deps[i].alias))) {
            package_err(L, manifest, rm.deps[i].line,
                    "dependency alias '%s' must be an identifier", rm.deps[i].alias);
            ok = 0;
        }
        if (!path_is_relative(rm.deps[i].path, strlen(rm.deps[i].path))) {
            package_err(L, manifest, rm.deps[i].line,
                    "dependency path '%s' must be a relative path", rm.deps[i].path);
            ok = 0;
        }
    }
    if (!ok) { free(rm.deps); return NULL; }

    Package* package = (Package*)arena_alloc(sizeof(Package));
    memset(package, 0, sizeof *package);
    package->dir = canon;
    package->name = arena_string(rm.name);
    package->ver_major = vmaj; package->ver_minor = vmin; package->ver_patch = vpat;
    package->ace_major = amaj; package->ace_minor = amin;
    package->is_app = is_app;
    package->root_file = root_canon;
    package->description = rm.description ? arena_string(rm.description) : NULL;
    package->next = L->loading; // push (cycle-detection stack)
    L->loading = package;

    PackageDep* deps = NULL;
    for (int i = 0; i < rm.ndeps; i++) {
        const char* target = join_path(canon, rm.deps[i].path);
        Package* dep = package_load(L, target, 0, manifest, rm.deps[i].line);
        if (!dep) continue;
        if (strcmp(dep->name, rm.deps[i].alias) != 0) {
            package_err(L, manifest, rm.deps[i].line,
                    "dependency alias '%s' does not match package name '%s' declared at '%s'",
                    rm.deps[i].alias, dep->name, dep->dir);
            continue;
        }
        PackageDep* pd = (PackageDep*)arena_alloc(sizeof(PackageDep));
        pd->name = arena_string(rm.deps[i].alias);
        pd->package = dep;
        pd->next = deps;
        deps = pd;
    }
    package->deps = deps;
    free(rm.deps);

    L->loading = package->next; // pop
    package->next = L->loaded;  // completed: dependencies already precede it
    L->loaded = package;
    return package;
}

Package* package_resolve(const char* package_dir, Diag* d, const char* compiler_path,
                         Package*** order, int* norder) {
    PackageLoader L = {0};
    L.d = d;
    L.compiler_path = compiler_path;
    Package* root = package_load(&L, package_dir, 1, NULL, 0);
    if (!root || d->errors > 0) {
        *order = NULL;
        *norder = 0;
        return NULL;
    }
    int n = 0;
    for (Package* p = L.loaded; p; p = p->next) n++;
    Package** arr = malloc((size_t)n * sizeof(Package*));
    if (!arr) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
    for (int i = n - 1; i >= 0; i--) {
        arr[i] = L.loaded;
        L.loaded = L.loaded->next;
    }
    *order = arr;
    *norder = n;
    return root;
}
