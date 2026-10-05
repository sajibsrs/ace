// package.c — ace.pkg manifest parsing and dependency-graph resolution
// (spec/modules-packages.md Package System). A manifest is a small line-based key: value
// format, plus one [dependencies] section of name: version entries. Every
// dependency name in a graph, direct or transitive, is found from the root
// package's directory as the nearest <dir>/pkg/<name> (Package System #5);
// a package's own reserved pkg/ is not part of it (#6). No codegen or
// import-surface wiring happens here.
#include "ace.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { const char* alias; const char* version; int line; } RawDep;

// [[link]] record kinds (LinkKind is declared in ace.h beside PackageLink):
// one raw entry per recognized key, in file order across every occurrence;
// record numbers the [[link]] occurrence the key belongs to.
typedef struct { LinkKind kind; const char* value; int line; int record; } RawLink;

typedef struct {
    const char* name; int name_line;
    const char* version; int version_line;
    const char* ace; int ace_line;
    const char* type; int type_line;
    const char* root; int root_line;
    const char* description; int description_line;
    const char* author; int author_line;
    const char* license; int license_line;
    const char* repository; int repository_line;
    RawDep* deps; int ndeps, cdeps;
    RawLink* links; int nlinks, clinks;
} RawManifest;

typedef struct NameEntry NameEntry;
struct NameEntry { const char* name; const char* dir; NameEntry* next; };

typedef struct {
    Diag*       d;
    Package*    loading; // in-progress stack (cycle detection)
    Package*    loaded;  // completed, newest-first
    NameEntry*  names;   // declared package name -> its canonical directory
    const char* root_dir; // canonical directory every dependency resolves from
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
    for (size_t i = 0; i < len; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

int package_valid_name(const char* s, size_t len) {
    if (!is_ident(s, len)) return 0;
    for (size_t i = 0; i < len; i++) if (isupper((unsigned char)s[i])) return 0;
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

// A relative path with no ".." component — must stay within the package
// directory that declares it. Shared by the root file and [[link]]
// object/search paths (Package System #14: "matching the existing
// import-containment rule").
static int path_is_contained(const char* s, size_t len) {
    if (!len || s[0] == '/') return 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '.' && i + 1 < len && s[i + 1] == '.' &&
            (i == 0 || s[i - 1] == '/') && (i + 2 == len || s[i + 2] == '/'))
            return 0;
    }
    return 1;
}

// A contained path ending in ".ace" — the shape a package's own root file
// must have.
static int path_is_local_ace(const char* s, size_t len) {
    if (len < 4 || memcmp(s + len - 4, ".ace", 4) != 0) return 0;
    return path_is_contained(s, len);
}

// A contained path ending in ".o" or ".a" — the shape a [[link]] "object"
// value must have (Package System #14).
static int path_is_local_object(const char* s, size_t len) {
    if (len < 2 || s[len - 2] != '.' || (s[len - 1] != 'o' && s[len - 1] != 'a')) return 0;
    return path_is_contained(s, len);
}

// The library key of [[link]] record `record`, or NULL.
static const char* record_library(const RawManifest* rm, int record) {
    for (int i = 0; i < rm->nlinks; i++)
        if (rm->links[i].record == record && rm->links[i].kind == LINK_LIBRARY) return rm->links[i].value;
    return NULL;
}

// The path key of [[link]] record `record`, or NULL.
static const char* record_path(const RawManifest* rm, int record) {
    for (int i = 0; i < rm->nlinks; i++)
        if (rm->links[i].record == record && rm->links[i].kind == LINK_PATH) return rm->links[i].value;
    return NULL;
}

// The file the target linker would pick for -l<name> in dir, in its own
// per-directory order (Apple ld64: .tbd, .dylib, .a), or NULL. The path is
// kept as written, symlinks unresolved: the binary records the library's own
// install name either way.
static const char* library_file(const char* dir, const char* name) {
    static const char* const suffixes[] = {".tbd", ".dylib", ".a"};
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
        size_t n = strlen(dir) + strlen(name) + 16;
        char* path = arena_alloc(n);
        snprintf(path, n, "%s/lib%s%s", dir, name, suffixes[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return path;
    }
    return NULL;
}

int package_contains(const char* package_dir, const char* canon) {
    if (strcmp(canon, package_dir) != 0 && !path_beneath(canon, package_dir)) return 0;
    size_t n = strlen(package_dir);
    const char* rest = canon + n;
    return !(strncmp(rest, "/pkg", 4) == 0 && (rest[4] == 0 || rest[4] == '/'));
}

// The nearest existing <dir>/pkg/<name>, checking start and then each
// ancestor directory (Package System #5); NULL if none exists.
static const char* find_in_pkg(const char* start, const char* name) {
    size_t n = strlen(start);
    char* dir = arena_alloc(n + 1);
    memcpy(dir, start, n + 1);
    for (;;) {
        size_t need = strlen(dir) + strlen(name) + 7;
        char* candidate = arena_alloc(need);
        snprintf(candidate, need, "%s/pkg/%s", strcmp(dir, "/") == 0 ? "" : dir, name);
        struct stat st;
        if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) return candidate;
        char* slash = strrchr(dir, '/');
        if (!slash || strcmp(dir, "/") == 0) return NULL;
        if (slash == dir) slash[1] = 0;
        else slash[0] = 0;
    }
}

static void add_raw_dep(RawManifest* rm, const char* alias, const char* version, int line) {
    if (rm->ndeps == rm->cdeps) {
        rm->cdeps = rm->cdeps ? rm->cdeps * 2 : 8;
        rm->deps = realloc(rm->deps, (size_t)rm->cdeps * sizeof(RawDep));
        if (!rm->deps) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
    }
    rm->deps[rm->ndeps].alias = alias;
    rm->deps[rm->ndeps].version = version;
    rm->deps[rm->ndeps].line = line;
    rm->ndeps++;
}

static void add_raw_link(RawManifest* rm, LinkKind kind, const char* value, int line, int record) {
    if (rm->nlinks == rm->clinks) {
        rm->clinks = rm->clinks ? rm->clinks * 2 : 8;
        rm->links = realloc(rm->links, (size_t)rm->clinks * sizeof(RawLink));
        if (!rm->links) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
    }
    rm->links[rm->nlinks].kind = kind;
    rm->links[rm->nlinks].value = value;
    rm->links[rm->nlinks].line = line;
    rm->links[rm->nlinks].record = record;
    rm->nlinks++;
}

// Parse one ace.pkg's text into its raw (unvalidated) fields and dependency
// entries. Diagnoses malformed lines but keeps scanning so one pass reports
// every problem in the file.
static int parse_manifest(PackageLoader* L, const char* manifest_path, RawManifest* rm) {
    Source s;
    if (source_load(&s, manifest_path) != 0) { L->d->errors++; return 0; }
    memset(rm, 0, sizeof *rm);
    int line = 0, in_deps = 0, in_link = 0, link_seen = 0, link_record = 0, ok = 1;
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
        if (tlen == 14 && memcmp(t, "[dependencies]", 14) == 0) { in_deps = 1; in_link = 0; continue; }
        if (tlen == 8 && memcmp(t, "[[link]]", 8) == 0) { in_link = 1; in_deps = 0; link_seen = 0; link_record++; continue; }
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
        if (!kt) {
            package_err(L, manifest_path, line, "expected 'key: value'");
            ok = 0;
            continue;
        }
        if (!vlen) {
            // description/author are optional single-line metadata: an empty
            // value (the field present with nothing after the colon, as
            // `ace pkg init` writes it) means absent, not an error.
            if (!in_deps && (text_is(key, (uint32_t)kt, "description") || text_is(key, (uint32_t)kt, "author") ||
                             text_is(key, (uint32_t)kt, "license") || text_is(key, (uint32_t)kt, "repository")))
                continue;
            package_err(L, manifest_path, line, "expected 'key: value'");
            ok = 0;
            continue;
        }
        if (in_link) {
            LinkKind kind;
            if (text_is(key, (uint32_t)kt, "object")) kind = LINK_OBJECT;
            else if (text_is(key, (uint32_t)kt, "search")) kind = LINK_SEARCH;
            else if (text_is(key, (uint32_t)kt, "library")) kind = LINK_LIBRARY;
            else if (text_is(key, (uint32_t)kt, "framework")) kind = LINK_FRAMEWORK;
            else if (text_is(key, (uint32_t)kt, "path")) kind = LINK_PATH;
            else {
                package_err(L, manifest_path, line, "unknown [[link]] field '%.*s'", (int)kt, key);
                ok = 0;
                continue;
            }
            if (link_seen & (1 << kind)) {
                package_err(L, manifest_path, line, "duplicate '%.*s' in this [[link]] record", (int)kt, key);
                ok = 0;
                continue;
            }
            link_seen |= (1 << kind);
            add_raw_link(rm, kind, package_strndup(val, vlen), line, link_record);
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
        FIELD("type", type, type_line)
        FIELD("root", root, root_line)
        FIELD("description", description, description_line)
        FIELD("author", author, author_line)
        FIELD("license", license, license_line)
        FIELD("repository", repository, repository_line)
#undef FIELD
        package_err(L, manifest_path, line, "unknown field '%.*s'", (int)kt, key);
        ok = 0;
    }
    return ok;
}

static int on_stack(PackageLoader* L, const char* canon) {
    for (Package* p = L->loading; p; p = p->next) if (strcmp(p->dir, canon) == 0) return 1;
    return 0;
}

static Package* already_loaded(PackageLoader* L, const char* canon) {
    for (Package* p = L->loaded; p; p = p->next) if (strcmp(p->dir, canon) == 0) return p;
    return NULL;
}

static void report_cycle(PackageLoader* L, const char* manifest, int line, const char* canon) {
    size_t cap = strlen(canon) + 64;
    for (Package* p = L->loading; p; p = p->next) cap += strlen(p->dir) + 4;
    char* chain = malloc(cap);
    size_t off = 0;
    off += (size_t)snprintf(chain + off, cap - off, "%s", canon);
    for (Package* p = L->loading; p; p = p->next) off += (size_t)snprintf(chain + off, cap - off, " -> %s", p->dir);
    package_err(L, manifest, line, "package dependency cycle: %s", chain);
    free(chain);
}

static int is_std_module_name(const char* name) {
    char dir[4096];
    compiler_dir(dir, sizeof dir);
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
    if (canon && is_root) L->root_dir = canon;
    if (!canon) {
        package_err(L, dep_manifest ? dep_manifest : dir_arg, dep_line, "cannot find package directory '%s'", dir_arg);
        return NULL;
    }
    struct stat st;
    if (stat(canon, &st) != 0 || !S_ISDIR(st.st_mode)) {
        package_err(L, dep_manifest ? dep_manifest : dir_arg, dep_line, "'%s' is not a directory", canon);
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
        package_err(L, dep_manifest ? dep_manifest : canon, dep_line, "no ace.pkg at '%s'", canon);
        return NULL;
    }

    RawManifest rm;
    int parse_ok = parse_manifest(L, manifest, &rm);
    if (!parse_ok) { free(rm.deps); free(rm.links); return NULL; }

    int ok = 1;
    if (!rm.name) { package_err(L, manifest, 1, "missing required field 'name'"); ok = 0; }
    if (!rm.version) { package_err(L, manifest, 1, "missing required field 'version'"); ok = 0; }
    if (!rm.ace) { package_err(L, manifest, 1, "missing required field 'ace'"); ok = 0; }
    if (!rm.type) { package_err(L, manifest, 1, "missing required field 'type'"); ok = 0; }
    if (!rm.root) { package_err(L, manifest, 1, "missing required field 'root'"); ok = 0; }
    if (!ok) { free(rm.deps); free(rm.links); return NULL; }

    if (!package_valid_name(rm.name, strlen(rm.name))) {
        package_err(L, manifest, rm.name_line, "package name '%s' must be a lowercase identifier", rm.name);
        ok = 0;
    }
    int vmaj = 0, vmin = 0, vpat = 0;
    if (!parse_version(rm.version, strlen(rm.version), &vmaj, &vmin, &vpat)) {
        package_err(L, manifest, rm.version_line, "version '%s' must be exactly major.minor.patch", rm.version);
        ok = 0;
    }
    int amaj = 0, amin = 0;
    if (!parse_ace_version(rm.ace, strlen(rm.ace), &amaj, &amin)) {
        package_err(L, manifest, rm.ace_line, "ace version '%s' must be exactly major.minor", rm.ace);
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
    int is_exe = 0;
    if (text_is(rm.type, (uint32_t)strlen(rm.type), "exe")) is_exe = 1;
    else if (text_is(rm.type, (uint32_t)strlen(rm.type), "lib")) is_exe = 0;
    else {
        package_err(L, manifest, rm.type_line, "type '%s' must be 'exe' or 'lib'", rm.type);
        ok = 0;
    }
    if (!is_root && is_exe) {
        package_err(L, manifest, rm.type_line,
                "dependency '%s' is type 'exe'; only a lib package can be a dependency",
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
        } else if (!package_contains(canon, root_canon)) {
            package_err(L, manifest, rm.root_line, "root '%s' is inside the reserved pkg/ directory", rm.root);
            ok = 0;
        }
    }
    if (ok && is_std_module_name(rm.name)) {
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
            package_err(L, manifest, rm.deps[i].line, "dependency alias '%s' must be an identifier", rm.deps[i].alias);
            ok = 0;
        }
        int dmaj, dmin, dpat;
        if (!parse_version(rm.deps[i].version, strlen(rm.deps[i].version), &dmaj, &dmin, &dpat)) {
            package_err(L, manifest, rm.deps[i].line, "dependency '%s' version '%s' must be exactly major.minor.patch",
                        rm.deps[i].alias, rm.deps[i].version);
            ok = 0;
        }
    }
    for (int i = 0; i < rm.nlinks; i++) {
        RawLink* rl = &rm.links[i];
        size_t vlen = strlen(rl->value);
        if (rl->kind == LINK_OBJECT && !path_is_local_object(rl->value, vlen)) {
            package_err(L, manifest, rl->line,
                    "[[link]] object '%s' must be a relative path within the package, "
                    "ending in '.o' or '.a'", rl->value);
            ok = 0;
        } else if (rl->kind == LINK_SEARCH && !path_is_contained(rl->value, vlen)) {
            package_err(L, manifest, rl->line,
                    "[[link]] search '%s' must be a relative path within the package",
                    rl->value);
            ok = 0;
        } else if (rl->kind == LINK_PATH) {
            if (rl->value[0] != '/') {
                package_err(L, manifest, rl->line, "[[link]] path '%s' must be an absolute directory", rl->value);
                ok = 0;
            } else if (!record_library(&rm, rl->record)) {
                package_err(L, manifest, rl->line, "[[link]] path needs a library in the same record");
                ok = 0;
            }
        }
    }
    if (!ok) { free(rm.deps); free(rm.links); return NULL; }

    Package* package = (Package*)arena_alloc(sizeof(Package));
    memset(package, 0, sizeof *package);
    package->dir = canon;
    package->name = arena_string(rm.name);
    package->ver_major = vmaj; package->ver_minor = vmin; package->ver_patch = vpat;
    package->ace_major = amaj; package->ace_minor = amin;
    package->is_exe = is_exe;
    package->root_file = root_canon;
    package->description = rm.description ? arena_string(rm.description) : NULL;
    package->author = rm.author ? arena_string(rm.author) : NULL;
    package->license = rm.license ? arena_string(rm.license) : NULL;
    package->repository = rm.repository ? arena_string(rm.repository) : NULL;
    package->next = L->loading; // push (cycle-detection stack)
    L->loading = package;

    PackageDep* deps = NULL;
    for (int i = 0; i < rm.ndeps; i++) {
        RawDep* rd = &rm.deps[i];
        const char* target = find_in_pkg(L->root_dir, rd->alias);
        if (!target) {
            package_err(L, manifest, rd->line, "dependency '%s' %s not found: no pkg/%s in '%s' or any parent directory",
                        rd->alias, rd->version, rd->alias, L->root_dir);
            continue;
        }
        Package* dep = package_load(L, target, 0, manifest, rd->line);
        if (!dep) continue;
        if (strcmp(dep->name, rd->alias) != 0) {
            package_err(L, manifest, rd->line,
                    "dependency alias '%s' does not match package name '%s' declared at '%s'",
                    rd->alias, dep->name, dep->dir);
            continue;
        }
        int dmaj = 0, dmin = 0, dpat = 0;
        parse_version(rd->version, strlen(rd->version), &dmaj, &dmin, &dpat);
        if (dep->ver_major != dmaj || dep->ver_minor != dmin || dep->ver_patch != dpat) {
            package_err(L, manifest, rd->line, "dependency '%s' requires version %s, but '%s' declares %d.%d.%d",
                        rd->alias, rd->version, dep->dir, dep->ver_major, dep->ver_minor, dep->ver_patch);
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

    PackageLink* links = NULL;
    PackageLink* links_tail = NULL;
    for (int i = 0; i < rm.nlinks; i++) {
        RawLink* rl = &rm.links[i];
        const char* value = rl->value;
        if (rl->kind == LINK_OBJECT || rl->kind == LINK_SEARCH) {
            const char* resolved = canonicalize(join_path(canon, value));
            if (!resolved || !(path_beneath(resolved, canon) || strcmp(resolved, canon) == 0)) {
                package_err(L, manifest, rl->line, "[[link]] path '%s' does not exist", value);
                continue;
            }
            if (!package_contains(canon, resolved)) {
                package_err(L, manifest, rl->line, "[[link]] path '%s' is inside the reserved pkg/ directory", value);
                continue;
            }
            value = resolved;
        }
        if (rl->kind == LINK_PATH) continue; // consumed by its record's library
        // The root's path is binding, so a missing file is an error; a
        // dependency's is a hint that falls back to the linker's search.
        const char* dir = NULL;
        const char* file = NULL;
        if (rl->kind == LINK_LIBRARY) {
            dir = record_path(&rm, rl->record);
            if (dir && !(file = library_file(dir, value)) && is_root) {
                package_err(L, manifest, rl->line,
                        "[[link]] library '%s' not found in '%s' (tried lib%s.tbd, lib%s.dylib, lib%s.a)",
                        value, dir, value, value, value);
                continue;
            }
        }
        PackageLink* pl = (PackageLink*)arena_alloc(sizeof(PackageLink));
        pl->kind = rl->kind;
        pl->value = arena_string(value);
        pl->path = dir ? arena_string(dir) : NULL;
        pl->file = file;
        pl->line = rl->line;
        pl->next = NULL;
        if (links_tail) links_tail->next = pl; else links = pl;
        links_tail = pl;
    }
    package->links = links;
    free(rm.links);

    L->loading = package->next; // pop
    package->next = L->loaded;  // completed: dependencies already precede it
    L->loaded = package;
    return package;
}

Package* package_resolve(const char* package_dir, Diag* d, Package*** order, int* norder) {
    PackageLoader L = {0};
    L.d = d;
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

// The file to link for library `name`: the root's path when it gives one,
// else the dependencies' one agreed path hint, else NULL (plain -l<name>).
// Two dependencies hinting different paths conflict, whether or not either
// exists here, so the result never depends on the host; the root settles it.
static const char* library_choice(Package** order, int norder, const char* name, Diag* d) {
    Package* root = order[norder - 1];
    for (const PackageLink* pl = root->links; pl; pl = pl->next)
        if (pl->kind == LINK_LIBRARY && pl->path && strcmp(pl->value, name) == 0) return pl->file;
    const PackageLink* hint = NULL;
    const Package* hint_pkg = NULL;
    for (int i = 0; i < norder - 1; i++)
        for (const PackageLink* pl = order[i]->links; pl; pl = pl->next) {
            if (pl->kind != LINK_LIBRARY || !pl->path || strcmp(pl->value, name) != 0) continue;
            if (!hint) { hint = pl; hint_pkg = order[i]; continue; }
            if (strcmp(hint->path, pl->path) != 0) {
                fprintf(stderr,
                        "%s:%d: error: [[link]] library '%s' path '%s' conflicts with %s:%d's '%s'; "
                        "the root package can choose one with its own path\n",
                        join_path(order[i]->dir, "ace.pkg"), pl->line, name, pl->path,
                        join_path(hint_pkg->dir, "ace.pkg"), hint->line, hint->path);
                d->errors++;
                return NULL;
            }
        }
    return hint ? hint->file : NULL;
}

char** package_collect_links(Package** order, int norder, Diag* d, int* nargs) {
    typedef struct {
        LinkKind    kind;
        const char* identity; // basename for OBJECT, else the value itself
        const char* value;
        const char* manifest;
        int         line;
    } Seen;
    Seen* seen = NULL; int nseen = 0, cseen = 0;
    char** args = NULL; int nargv = 0, cargv = 0;
#define PUSH_ARG(s) do {                                                      \
        if (nargv == cargv) {                                                 \
            cargv = cargv ? cargv * 2 : 8;                                    \
            args = realloc(args, (size_t)cargv * sizeof(char*));              \
            if (!args) { fprintf(stderr, "ace: out of memory\n"); exit(1); }  \
        }                                                                     \
        args[nargv++] = (char*)(s);                                          \
    } while (0)

    // A library's chosen file (library_choice) replaces -l<name> at the
    // library's first position in the graph.
    for (int i = 0; i < norder; i++) {
        Package* pkg = order[i];
        const char* manifest = join_path(pkg->dir, "ace.pkg");
        for (PackageLink* pl = pkg->links; pl; pl = pl->next) {
            const char* identity = pl->value;
            if (pl->kind == LINK_OBJECT) {
                const char* slash = strrchr(pl->value, '/');
                identity = slash ? slash + 1 : pl->value;
            }
            Seen* dup = NULL;
            for (int k = 0; k < nseen; k++)
                if (seen[k].kind == pl->kind && strcmp(seen[k].identity, identity) == 0) {
                    dup = &seen[k];
                    break;
                }
            if (dup) {
                if (strcmp(dup->value, pl->value) != 0) {
                    fprintf(stderr,
                            "%s:%d: error: native link input '%s' conflicts with %s:%d's "
                            "differing declaration ('%s' vs '%s')\n",
                            manifest, pl->line, identity, dup->manifest, dup->line,
                            pl->value, dup->value);
                    d->errors++;
                }
                continue; // exact duplicate or reported conflict: never emitted twice
            }
            if (nseen == cseen) {
                cseen = cseen ? cseen * 2 : 8;
                seen = realloc(seen, (size_t)cseen * sizeof(Seen));
                if (!seen) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
            }
            seen[nseen].kind = pl->kind;
            seen[nseen].identity = identity;
            seen[nseen].value = pl->value;
            seen[nseen].manifest = manifest;
            seen[nseen].line = pl->line;
            nseen++;
            switch (pl->kind) {
            case LINK_OBJECT: PUSH_ARG(pl->value); break;
            case LINK_SEARCH: PUSH_ARG("-L"); PUSH_ARG(pl->value); break;
            case LINK_LIBRARY: {
                const char* file = library_choice(order, norder, pl->value, d);
                if (file) PUSH_ARG(file);
                else { PUSH_ARG("-l"); PUSH_ARG(pl->value); }
                break;
            }
            case LINK_FRAMEWORK: PUSH_ARG("-framework"); PUSH_ARG(pl->value); break;
            case LINK_PATH: break; // never a PackageLink
            }
        }
    }
#undef PUSH_ARG
    free(seen);
    if (d->errors > 0) {
        free(args);
        *nargs = 0;
        return NULL;
    }
    *nargs = nargv;
    return args;
}

// --- `ace pkg add`/`remove` manifest rewriting (spec/queue.md Decision Queue,
// "Package system v1") -------------------------------------------------------
// Both functions are line-oriented text surgery only, mirroring
// parse_manifest's own comment/section/key scan so they agree with it on
// where an entry begins and ends; neither validates the graph. The caller
// re-resolves afterward and restores the original bytes on any error.

static int overwrite_file(const char* path, const char* data, size_t len) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "ace: cannot write '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "ace: cannot write '%s': %s\n", path, strerror(errno));
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

int package_manifest_add_dep(const char* manifest_path, const char* name, const char* version) {
    Source s;
    if (source_load(&s, manifest_path) != 0) return 1;

    char entry[512];
    int entry_len = snprintf(entry, sizeof entry, "%s: %s\n", name, version);

    // Find the end of the first "[dependencies]" line; the new entry is
    // inserted immediately after it, regardless of what else the section
    // already holds.
    const char* p = s.buf;
    const char* end = s.buf + s.len;
    const char* insert_at = NULL;
    while (p < end) {
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        const char* eol = nl ? nl : end;
        size_t tlen;
        const char* t = trim(p, (size_t)(eol - p), &tlen);
        const char* next = nl ? nl + 1 : end;
        if (tlen == 14 && memcmp(t, "[dependencies]", 14) == 0) { insert_at = next; break; }
        p = next;
    }

    char* out;
    size_t out_len;
    if (insert_at) {
        size_t head = (size_t)(insert_at - s.buf);
        out_len = s.len + (size_t)entry_len;
        out = malloc(out_len + 1);
        if (!out) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
        memcpy(out, s.buf, head);
        memcpy(out + head, entry, (size_t)entry_len);
        memcpy(out + head + (size_t)entry_len, s.buf + head, s.len - head);
    } else {
        // No existing section: append one, preceded by exactly one blank
        // line — collapsing any trailing blank lines already in the file so
        // repeated add/remove cycles stay idempotent rather than piling up.
        size_t content_end = s.len;
        while (content_end > 0 && s.buf[content_end - 1] == '\n') content_end--;
        static const char header[] = "[dependencies]\n";
        size_t sep_len = content_end > 0 ? 2 : 0; // blank line before the header, unless file is empty
        out_len = content_end + sep_len + (sizeof header - 1) + (size_t)entry_len;
        out = malloc(out_len + 1);
        if (!out) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
        size_t w = 0;
        memcpy(out, s.buf, content_end); w += content_end;
        memcpy(out + w, "\n\n", sep_len); w += sep_len;
        memcpy(out + w, header, sizeof header - 1); w += sizeof header - 1;
        memcpy(out + w, entry, (size_t)entry_len); w += (size_t)entry_len;
    }

    int rc = overwrite_file(manifest_path, out, out_len);
    free(out);
    free(s.buf);
    return rc;
}

int package_manifest_remove_dep(const char* manifest_path, const char* name, int* found) {
    Source s;
    if (source_load(&s, manifest_path) != 0) return 1;
    size_t nlen = strlen(name);

    const char* p = s.buf;
    const char* end = s.buf + s.len;
    int in_deps = 0;
    const char* header_line = NULL, *header_line_end = NULL;
    const char* entry_line = NULL, *entry_line_end = NULL;
    int other_deps = 0;

    while (p < end) {
        const char* line_start = p;
        const char* nl = memchr(p, '\n', (size_t)(end - p));
        const char* eol = nl ? nl : end;
        size_t tlen;
        const char* t = trim(p, (size_t)(eol - p), &tlen);
        const char* next = nl ? nl + 1 : end;
        p = next;
        if (tlen == 0) continue;
        if (tlen >= 2 && t[0] == '/' && t[1] == '/') continue;
        if (tlen == 14 && memcmp(t, "[dependencies]", 14) == 0) {
            in_deps = 1;
            header_line = line_start;
            header_line_end = next;
            continue;
        }
        if (t[0] == '[') { in_deps = 0; continue; }
        if (!in_deps) continue;
        const char* colon = memchr(t, ':', tlen);
        if (!colon) continue;
        size_t klen;
        const char* key = trim(t, (size_t)(colon - t), &klen);
        if (!entry_line && klen == nlen && memcmp(key, name, nlen) == 0) {
            entry_line = line_start;
            entry_line_end = next;
        } else {
            other_deps = 1;
        }
    }

    if (!entry_line) {
        *found = 0;
        free(s.buf);
        return 0;
    }
    *found = 1;

    int drop_header = header_line && !other_deps;
    // A blank line immediately precedes the header (two consecutive '\n's
    // right before it) exactly when `add` inserted this section itself;
    // swallow it too so the round trip stays idempotent.
    const char* del_start = header_line;
    if (drop_header && del_start - s.buf >= 2 && del_start[-1] == '\n' && del_start[-2] == '\n') del_start -= 1;

    char* out = malloc(s.len + 1);
    if (!out) { fprintf(stderr, "ace: out of memory\n"); exit(1); }
    size_t w = 0;
    if (drop_header) {
        memcpy(out + w, s.buf, (size_t)(del_start - s.buf)); w += (size_t)(del_start - s.buf);
        memcpy(out + w, header_line_end, (size_t)(entry_line - header_line_end));
        w += (size_t)(entry_line - header_line_end);
    } else {
        memcpy(out + w, s.buf, (size_t)(entry_line - s.buf)); w += (size_t)(entry_line - s.buf);
    }
    memcpy(out + w, entry_line_end, (size_t)(end - entry_line_end));
    w += (size_t)(end - entry_line_end);

    int rc = overwrite_file(manifest_path, out, w);
    free(out);
    free(s.buf);
    return rc;
}
