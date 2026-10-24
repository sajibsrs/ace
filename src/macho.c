// macho.c — the Mach-O arm64 object shared by every backend: sections,
// symbols, relocations, fixups, the DWARF line table, and the file image.
//
// Object format facts (pinned against clang reference objects, otool dumps):
//   - one LC_SEGMENT_64 with an EMPTY segname holds all sections; each
//     section carries its real (segname, sectname) pair (__TEXT/__DATA/__DWARF)
//   - segment vmaddr=0, fileoff = first section's file offset; section addrs
//     are continuous from 0; maxprot=initprot=7 (RWX in objects — the linker
//     decides the final image)
//   - symbol order in LC_SYMTAB: locals (n_type 0x0e = N_SECT), then extdefs
//     (0x0f = N_SECT|N_EXT), then undefs (0x01 = N_EXT); strtab starts with
//     the empty string (index 0)
//   - MH flags = 0 — no SUBSECTIONS_VIA_SYMBOLS: internal bl's are patched
//     in-place, never relocated (spec/compiler.md Compiler #5)
//   - one LC_BUILD_VERSION (platform 1 = macOS, minos 11.0), LC_SYMTAB,
//     LC_DYSYMTAB
//
// Relocations (r_word1 = symbolnum(24)|pcrel<<24|length<<25|extern<<27|type<<28;
// BRANCH26=2, PAGE21=3, PAGEOFF12=4 (length 2), UNSIGNED=0 (length 3)):
//   - adrp/add address pairs against local symbols (PAGE21 pcrel=1 / PAGEOFF12
//     pcrel=0), 8-byte absolute words via UNSIGNED (vtable slots, string
//     pointers in data), extern branches via BRANCH26 (extern=1). Entries are
//     kept structured and serialized once symbol indices are known at write.

#include "backend.h"

#include <stdio.h>
#include <unistd.h>

void oom(void) {
    fprintf(stderr, "ace: out of memory\n");
    exit(1);
}

// --- byte buffer encodings ------------------------------------------------------------

static void buf_uleb(Buf* b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) byte |= 0x80;
        buf_put8(b, byte);
    } while (v);
}

static void buf_sleb(Buf* b, int64_t v) {
    int more;
    do {
        uint8_t byte = (uint8_t)(v & 0x7f);
        int sign = byte & 0x40;
        v >>= 7;
        more = !((v == 0 && !sign) || (v == -1 && sign));
        if (more) byte |= 0x80;
        buf_put8(b, byte);
    } while (more);
}

static void buf_putname(Buf* b, const char* name) { // 16-byte padded field
    size_t n = strlen(name);
    if (n > 16) n = 16;
    buf_write(b, name, n);
    for (size_t i = n; i < 16; i++) buf_put8(b, 0);
}

// --- sections & symbols ---------------------------------------------------------------

void reloc_add(Sec* s, uint32_t off, ESym* sym, int pcrel, int len, int ext, uint32_t type) {
    if (s->nrl == s->crl) {
        s->crl = s->crl ? s->crl * 2 : 8;
        s->rl = realloc(s->rl, (size_t)s->crl * sizeof(Rel));
        if (!s->rl) oom();
    }
    s->rl[s->nrl++] = (Rel){ off, sym, (uint8_t)!!pcrel, (uint8_t)len, (uint8_t)!!ext, (uint8_t)type };
}

static uint32_t sym_hash(const char* p, uint32_t len) {
    uint32_t h = 2166136261u; // FNV-1a
    for (uint32_t i = 0; i < len; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h;
}

static void sym_slot_put(SymTab* t, ESym* s) {
    size_t i = s->hash & (t->slot_cap - 1);
    while (t->slots[i]) i = (i + 1) & (t->slot_cap - 1);
    t->slots[i] = s;
}

ESym* sym_intern(SymTab* t, const char* p, uint32_t len) {
    uint32_t h = sym_hash(p, len);
    if (t->slot_cap) {
        for (size_t i = h & (t->slot_cap - 1); t->slots[i];
             i = (i + 1) & (t->slot_cap - 1)) {
            ESym* s = t->slots[i];
            if (s->hash == h && s->len == len && memcmp(s->name, p, len) == 0)
                return s;
        }
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->v = realloc(t->v, t->cap * sizeof(ESym*));
        if (!t->v) oom();
    }
    ESym* s = malloc(sizeof(ESym));
    if (!s) oom();
    t->v[t->n++] = s;
    s->name = malloc((size_t)len + 1);
    if (!s->name) oom();
    memcpy(s->name, p, len);
    s->name[len] = 0;
    s->hash = h;
    s->len = len;
    s->strx = s->ord = 0;
    s->type = 0;
    s->sect = 0;
    s->value = 0;
    if (t->n * 2 > t->slot_cap) { // load factor <= 1/2
        size_t cap = t->slot_cap ? t->slot_cap * 2 : 64;
        free(t->slots);
        t->slots = calloc(cap, sizeof(ESym*));
        if (!t->slots) oom();
        t->slot_cap = cap;
        for (size_t i = 0; i < t->n; i++) sym_slot_put(t, t->v[i]);
    } else {
        sym_slot_put(t, s);
    }
    return s;
}

ESym* sym_define(SymTab* t, Sec* secs, const char* p, uint32_t len, int ext, Sec* in) {
    ESym* s = sym_intern(t, p, len);
    if (s->type) return s; // redefinition is a sema error; keep the first
    s->type = N_SECT_ | (ext ? N_EXT_ : 0);
    s->sect = (uint8_t)(1 + (int)(in - secs));
    s->value = sec_zerofill(in) ? in->size : in->data.len;
    return s;
}

// --- the object under construction -----------------------------------------------------

void obj_init(Obj* o, Diag* d, EmitMode mode) {
    static const Sec PROTO[NSECS_THREAD] = {
        [SEC_TEXT]       = { .sect = "__text",       .seg = "__TEXT",
                             .flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, .align = 6 },
        [SEC_CONST]      = { .sect = "__const",      .seg = "__DATA_CONST",
                             .align = 4 },
        [SEC_DATA]       = { .sect = "__data",       .seg = "__DATA",
                             .align = 4 },
        [SEC_BSS]        = { .sect = "__bss",        .seg = "__DATA",
                             .flags = S_ZEROFILL, .align = 4 },
        [SEC_DEBUG_ABBREV] = { .sect = "__debug_abbrev", .seg = "__DWARF",
                               .flags = S_ATTR_DEBUG },
        [SEC_DEBUG_INFO] = { .sect = "__debug_info", .seg = "__DWARF",
                             .flags = S_ATTR_DEBUG },
        [SEC_DEBUG_LINE] = { .sect = "__debug_line", .seg = "__DWARF",
                             .flags = S_ATTR_DEBUG },
        [SEC_THREAD_DATA] = { .sect = "__thread_data", .seg = "__DATA",
                              .flags = S_THREAD_LOCAL_REGULAR, .align = 4 },
        [SEC_THREAD_VARS] = { .sect = "__thread_vars", .seg = "__DATA",
                              .flags = S_THREAD_LOCAL_VARIABLES, .align = 3 },
        [SEC_THREAD_BSS]  = { .sect = "__thread_bss", .seg = "__DATA",
                              .flags = S_THREAD_LOCAL_ZEROFILL, .align = 4 },
    };
    memset(o, 0, sizeof *o);
    o->d = d;
    o->mode = mode;
    for (int i = 0; i < NSECS_THREAD; i++) {
        o->secs[i].sect = PROTO[i].sect;
        o->secs[i].seg = PROTO[i].seg;
        o->secs[i].flags = PROTO[i].flags;
        o->secs[i].align = PROTO[i].align;
    }
    sym_define(&o->syms, o->secs, "__ace$text", 10, 0, &o->secs[SEC_TEXT]);
}

void obj_fixup(Obj* o, uint8_t kind, uint32_t site, ESym* target) {
    if (o->nfix == o->capfix) {
        o->capfix = o->capfix ? o->capfix * 2 : 16;
        o->fix = realloc(o->fix, o->capfix * sizeof(Fixup));
        if (!o->fix) oom();
    }
    o->fix[o->nfix++] = (Fixup){ kind, site, target, 0 };
}

void obj_line(Obj* o, uint32_t addr, int line, int col, const char* path) {
    if (o->nlines && o->lines[o->nlines - 1].addr == addr) {
        o->lines[o->nlines - 1] = (LineRow){ addr, line, col, path };
        return;
    }
    if (o->nlines == o->clines) {
        o->clines = o->clines ? o->clines * 2 : 64;
        o->lines = realloc(o->lines, (size_t)o->clines * sizeof(LineRow));
        if (!o->lines) oom();
    }
    o->lines[o->nlines++] = (LineRow){ addr, line, col, path };
}

void obj_debug_fn(Obj* o, uint32_t addr, uint32_t size, const char* name, uint32_t nlen) {
    if (o->ndebug_fns == o->cdebug_fns) {
        o->cdebug_fns = o->cdebug_fns ? o->cdebug_fns * 2 : 16;
        o->debug_fns = realloc(o->debug_fns, (size_t)o->cdebug_fns * sizeof(DebugFn));
        if (!o->debug_fns) oom();
    }
    o->debug_fns[o->ndebug_fns++] = (DebugFn){ addr, size, name, nlen };
}

// --- fixups ---------------------------------------------------------------------------

void obj_patch_fixups(Obj* o) {
    Buf* t = obj_text(o);
    for (size_t i = 0; i < o->nfix; i++) {
        Fixup* f = &o->fix[i];
        if (f->kind == FK_SUBSP) {
            uint32_t frame = (f->frame + 15u) & ~15u;
            if (frame <= 0xFFFu) {
                buf_patch32(t, f->site, 0xD10003FFu | (frame << 10));
                continue;
            }
            uint32_t pages = (frame + 0xFFFu) >> 12;
            if (pages > 0xFFFu) {
                fprintf(stderr, "ace: function stack frame exceeds AArch64 immediate range\n");
                o->d->errors++;
                continue;
            }
            buf_patch32(t, f->site, 0xD14003FFu | (pages << 10));
            continue;
        }
        if (!f->target->type || f->target->sect != 1 + SEC_TEXT) {
            fprintf(stderr, "ace: internal call to '%s' is not a text symbol\n", f->target->name);
            o->d->errors++;
            continue;
        }
        int64_t delta = (int64_t)f->target->value - (int64_t)f->site;
        if (delta < -(1 << 27) || delta >= (1 << 27) || (delta & 3)) {
            fprintf(stderr, "ace: call to '%s' out of branch range\n", f->target->name);
            o->d->errors++;
            continue;
        }
        buf_patch32(t, f->site, 0x94000000u | (uint32_t)((delta >> 2) & 0x03FFFFFFu));
    }
}

// --- DWARF line table -------------------------------------------------------------

static void source_path(const char* input, char* out, size_t cap) {
    if (input[0] == '/') {
        snprintf(out, cap, "%s", input);
        return;
    }
    char cwd[3072];
    if (getcwd(cwd, sizeof cwd)) snprintf(out, cap, "%s/%s", cwd, input);
    else snprintf(out, cap, "%s", input);
}

void obj_debug_info(Obj* o, ESym* text_anchor) {
    if (!o->nlines) return;
    Buf* ab = &o->secs[SEC_DEBUG_ABBREV].data;
    // Abbrev 1: compile unit. Function children let dsymutil select the text
    // ranges while deliberately omitting types, variables, and frame data.
    buf_uleb(ab, 1);
    buf_uleb(ab, 0x11); // DW_TAG_compile_unit
    buf_put8(ab, 1);    // DW_CHILDREN_yes
    buf_uleb(ab, 0x25); buf_uleb(ab, 0x08); // DW_AT_producer, DW_FORM_string
    buf_uleb(ab, 0x13); buf_uleb(ab, 0x05); // DW_AT_language, DW_FORM_data2
    buf_uleb(ab, 0x03); buf_uleb(ab, 0x08); // DW_AT_name, DW_FORM_string
    buf_uleb(ab, 0x10); buf_uleb(ab, 0x17); // DW_AT_stmt_list, DW_FORM_sec_offset
    buf_uleb(ab, 0x1b); buf_uleb(ab, 0x08); // DW_AT_comp_dir, DW_FORM_string
    buf_uleb(ab, 0x11); buf_uleb(ab, 0x01); // DW_AT_low_pc, DW_FORM_addr
    buf_uleb(ab, 0x12); buf_uleb(ab, 0x06); // DW_AT_high_pc, DW_FORM_data4
    buf_put8(ab, 0); buf_put8(ab, 0);
    buf_uleb(ab, 2);
    buf_uleb(ab, 0x2e); // DW_TAG_subprogram
    buf_put8(ab, 0);    // DW_CHILDREN_no
    buf_uleb(ab, 0x11); buf_uleb(ab, 0x01); // DW_AT_low_pc, DW_FORM_addr
    buf_uleb(ab, 0x12); buf_uleb(ab, 0x06); // DW_AT_high_pc, DW_FORM_data4
    buf_uleb(ab, 0x03); buf_uleb(ab, 0x08); // DW_AT_name, DW_FORM_string
    buf_put8(ab, 0); buf_put8(ab, 0);
    buf_put8(ab, 0);    // abbreviation table terminator

    char path[4096], cwd[3072];
    source_path(o->d->path, path, sizeof path);
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    Buf* in = &o->secs[SEC_DEBUG_INFO].data;
    size_t unit = in->len;
    buf_put32(in, 0);   // unit_length
    buf_put16(in, 4);   // DWARF v4
    buf_put32(in, 0);   // abbrev offset
    buf_put8(in, 8);    // address size
    buf_uleb(in, 1);    // abbrev code
    buf_write(in, "Ace compiler", sizeof "Ace compiler");
    buf_put16(in, 0x8000); // DW_LANG_lo_user: Ace has no assigned DWARF code
    buf_write(in, path, strlen(path) + 1);
    buf_put32(in, 0);   // stmt_list offset
    buf_write(in, cwd, strlen(cwd) + 1);
    uint32_t low_pc = (uint32_t)in->len;
    buf_put64(in, 0);
    reloc_add(&o->secs[SEC_DEBUG_INFO], low_pc, text_anchor, 0, 3, 0, RM_UNSIGNED);
    buf_put32(in, (uint32_t)obj_text(o)->len); // high_pc is a size in DWARF v4
    for (int i = 0; i < o->ndebug_fns; i++) {
        DebugFn* f = &o->debug_fns[i];
        buf_uleb(in, 2);
        uint32_t fn_pc = (uint32_t)in->len;
        buf_put64(in, f->addr); // relocation addend within __text
        reloc_add(&o->secs[SEC_DEBUG_INFO], fn_pc, text_anchor, 0, 3, 0, RM_UNSIGNED);
        buf_put32(in, f->size);
        buf_write(in, f->name, f->nlen);
        buf_put8(in, 0);
    }
    buf_put8(in, 0);    // end compile-unit children
    buf_patch32(in, unit, (uint32_t)(in->len - unit - 4));
}

void obj_debug_line(Obj* o, ESym* text_anchor) {
    if (!o->nlines) return;
    Buf* b = &o->secs[SEC_DEBUG_LINE].data;

    const char** files = malloc((size_t)o->nlines * sizeof(char*));
    if (!files) oom();
    int nfiles = 0;
    for (int i = 0; i < o->nlines; i++) {
        int seen = 0;
        for (int j = 0; j < nfiles; j++) if (strcmp(files[j], o->lines[i].path) == 0) { seen = 1; break; }
        if (!seen) files[nfiles++] = o->lines[i].path;
    }

    size_t unit = b->len;
    buf_put32(b, 0);             // unit_length, patched below
    buf_put16(b, 4);             // DWARF v4
    size_t header_len = b->len;
    buf_put32(b, 0);             // header_length, patched below
    size_t header = b->len;
    buf_put8(b, 1);              // minimum_instruction_length
    buf_put8(b, 1);              // maximum_operations_per_instruction
    buf_put8(b, 1);              // default_is_stmt
    buf_put8(b, (uint8_t)-5);    // line_base
    buf_put8(b, 14);             // line_range
    buf_put8(b, 13);             // opcode_base
    static const uint8_t oplen[12] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    buf_write(b, oplen, sizeof oplen);
    buf_put8(b, 0);              // include_directories terminator
    for (int i = 0; i < nfiles; i++) {
        char path[4096];
        source_path(files[i], path, sizeof path);
        buf_write(b, path, strlen(path) + 1);
        buf_uleb(b, 0);          // directory index (full filename)
        buf_uleb(b, 0);          // modification time
        buf_uleb(b, 0);          // file size
    }
    buf_put8(b, 0);              // file_names terminator
    buf_patch32(b, header_len, (uint32_t)(b->len - header));

    // DW_LNE_set_address. The object relocation supplies __text's final base.
    buf_put8(b, 0);
    buf_uleb(b, 9);
    buf_put8(b, 2);
    uint32_t address_off = (uint32_t)b->len;
    buf_put64(b, 0);
    reloc_add(&o->secs[SEC_DEBUG_LINE], address_off, text_anchor, 0, 3, 0, RM_UNSIGNED);

    uint32_t addr = 0;
    int line = 1, col = 0, file = 1;
    for (int i = 0; i < o->nlines; i++) {
        LineRow* r = &o->lines[i];
        int want_file = 1;
        for (int j = 0; j < nfiles; j++) if (strcmp(files[j], r->path) == 0) { want_file = j + 1; break; }
        if (r->addr > addr) {
            buf_put8(b, 2);      // DW_LNS_advance_pc
            buf_uleb(b, r->addr - addr);
            addr = r->addr;
        }
        if (want_file != file) {
            buf_put8(b, 4);      // DW_LNS_set_file
            buf_uleb(b, (uint64_t)want_file);
            file = want_file;
        }
        if (r->line != line) {
            buf_put8(b, 3);      // DW_LNS_advance_line
            buf_sleb(b, (int64_t)r->line - line);
            line = r->line;
        }
        if (r->col != col) {
            buf_put8(b, 5);      // DW_LNS_set_column
            buf_uleb(b, (uint64_t)(r->col > 0 ? r->col : 0));
            col = r->col;
        }
        buf_put8(b, 1);          // DW_LNS_copy
    }
    uint32_t end = (uint32_t)obj_text(o)->len;
    if (end > addr) {
        buf_put8(b, 2);
        buf_uleb(b, end - addr);
    }
    buf_put8(b, 0);
    buf_uleb(b, 1);
    buf_put8(b, 1);              // DW_LNE_end_sequence
    buf_patch32(b, unit, (uint32_t)(b->len - unit - 4));
    free(files);
}


// --- Mach-O object writing ----------------------------------------------------------

size_t obj_write(Obj* o, const char* path) {
    // The thread-local sections follow the others, so an object without a
    // thread-local writes exactly the first NSECS.
    int nsecs = o->secs[SEC_THREAD_VARS].data.len ? NSECS_THREAD : NSECS;
    // Section size = data length for file-backed sections (bss is reserved).
    for (int i = 0; i < nsecs; i++) if (!sec_zerofill(&o->secs[i])) o->secs[i].size = o->secs[i].data.len;

    uint32_t segcmd = 72 + 80 * (uint32_t)nsecs;
    uint32_t cmdend = 32 + segcmd + 24 + 24 + 80; // header + 4 load commands
    uint32_t max_section_align = 1;
    for (int i = 0; i < nsecs; i++) {
        uint32_t a = 1u << o->secs[i].align;
        if (max_section_align < a) max_section_align = a;
    }
    uint32_t base = (cmdend + max_section_align - 1) & ~(max_section_align - 1);

    // Layout: addrs continuous from 0; file offsets track (bss takes no bytes).
    uint64_t addr = 0;
    uint32_t off = base;
    for (int i = 0; i < nsecs; i++) {
        Sec* s = &o->secs[i];
        uint32_t a = 1u << s->align;
        addr = (addr + a - 1) & ~(uint64_t)(a - 1);
        off = (off + a - 1) & ~(a - 1);
        s->addr = addr;
        s->offset = off;
        addr += s->size;
        if (!sec_zerofill(s)) off += (uint32_t)s->size;
    }
    uint64_t vmsize = addr;
    uint32_t filesize = off - base;

    // Symbol order (pinned): locals, extdefs, undefs. Assign strtab indices
    // and table ordinals; relocation entries serialize against the ordinals.
    Buf str;
    memset(&str, 0, sizeof str);
    buf_put8(&str, 0); // strx 0: the empty string
    ESym** order = malloc((o->syms.n ? o->syms.n : 1) * sizeof(ESym*));
    if (!order) oom();
    size_t norder = 0, nloc = 0, next_ = 0, nund = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < o->syms.n; i++) {
            ESym* s = o->syms.v[i];
            int want = pass == 0 ? (s->type && !(s->type & N_EXT_))
                    : pass == 1 ? (s->type && (s->type & N_EXT_))
                                : !s->type;
            if (!want) continue;
            s->strx = (uint32_t)str.len;
            s->ord = (uint32_t)norder;
            buf_write(&str, s->name, strlen(s->name) + 1);
            order[norder++] = s;
            if (pass == 0) nloc++;
            else if (pass == 1) next_++;
            else nund++;
        }
    }

    // Serialize structured relocations now that symbol ordinals are known.
    Buf relocs[NSECS_THREAD];
    memset(relocs, 0, sizeof relocs);
    for (int i = 0; i < nsecs; i++) {
        for (int j = 0; j < o->secs[i].nrl; j++) {
            Rel* r = &o->secs[i].rl[j];
            buf_put32(&relocs[i], r->off);
            uint32_t target = r->ext ? r->sym->ord : r->sym->sect;
            buf_put32(&relocs[i], (target & 0xFFFFFFu) |
                      ((uint32_t)r->pcrel << 24) | ((uint32_t)r->len << 25) |
                      ((uint32_t)r->ext << 27) | ((uint32_t)r->type << 28));
        }
    }

    // Relocation data follows all section data.
    uint32_t reloff = off;
    for (int i = 0; i < nsecs; i++) {
        o->secs[i].reloff = reloff;
        reloff += (uint32_t)relocs[i].len;
    }

    // Assemble the file image.
    Buf img;
    memset(&img, 0, sizeof img);

    // mach_header_64
    buf_put32(&img, 0xFEEDFACFu);             // MH_MAGIC_64
    buf_put32(&img, 0x0100000Cu);             // CPU_TYPE_ARM64
    buf_put32(&img, 0);                       // CPU_SUBTYPE_ARM64_ALL
    buf_put32(&img, 1);                       // MH_OBJECT
    buf_put32(&img, 4);                       // ncmds
    buf_put32(&img, segcmd + 24 + 24 + 80);   // sizeofcmds
    buf_put32(&img, 0);                       // flags (pinned: no bit set)
    buf_put32(&img, 0);                       // reserved

    // LC_SEGMENT_64 — one nameless segment holding all sections
    buf_put32(&img, 0x19);
    buf_put32(&img, segcmd);
    buf_putname(&img, "");                    // segname: empty (pinned)
    buf_put64(&img, 0);                       // vmaddr
    buf_put64(&img, vmsize);
    buf_put64(&img, base);                    // fileoff
    buf_put64(&img, filesize);
    buf_put32(&img, 7);                       // maxprot RWX
    buf_put32(&img, 7);                       // initprot RWX
    buf_put32(&img, (uint32_t)nsecs);
    buf_put32(&img, 0);                       // flags
    for (int i = 0; i < nsecs; i++) {
        Sec* s = &o->secs[i];
        buf_putname(&img, s->sect);
        buf_putname(&img, s->seg);
        buf_put64(&img, s->addr);
        buf_put64(&img, s->size);
        buf_put32(&img, s->offset);
        buf_put32(&img, s->align);
        buf_put32(&img, relocs[i].len ? s->reloff : 0);
        buf_put32(&img, (uint32_t)(relocs[i].len / 8));
        buf_put32(&img, s->flags);
        buf_put32(&img, 0);                   // reserved1
        buf_put32(&img, 0);                   // reserved2
        buf_put32(&img, 0);                   // reserved3
    }

    // LC_BUILD_VERSION
    buf_put32(&img, 0x32);
    buf_put32(&img, 24);
    buf_put32(&img, 1);                       // PLATFORM_MACOS
    buf_put32(&img, 0x000B0000u);             // minos 11.0
    buf_put32(&img, 0x000B0000u);             // sdk 11.0
    buf_put32(&img, 0);                       // ntools

    // LC_SYMTAB — symoff/stroff patched once known
    size_t symcmd = img.len;
    buf_put32(&img, 0x02);
    buf_put32(&img, 24);
    buf_put32(&img, 0);                       // symoff (patch)
    buf_put32(&img, (uint32_t)o->syms.n);
    buf_put32(&img, 0);                       // stroff (patch)
    buf_put32(&img, (uint32_t)str.len);

    // LC_DYSYMTAB
    buf_put32(&img, 0x0B);
    buf_put32(&img, 80);
    buf_put32(&img, 0); buf_put32(&img, (uint32_t)nloc);
    buf_put32(&img, (uint32_t)nloc); buf_put32(&img, (uint32_t)next_);
    buf_put32(&img, (uint32_t)(nloc + next_)); buf_put32(&img, (uint32_t)nund);
    for (int i = 0; i < 12; i++) buf_put32(&img, 0); // remaining dysym fields

    // Section data (offsets pinned above), then relocations.
    while (img.len < base) buf_put8(&img, 0);
    for (int i = 0; i < nsecs; i++) {
        Sec* s = &o->secs[i];
        while (img.len < s->offset) buf_put8(&img, 0);
        if (sec_zerofill(s)) continue;
        buf_write(&img, s->data.d, s->data.len);
    }
    for (int i = 0; i < nsecs; i++) buf_write(&img, relocs[i].d, relocs[i].len);

    // Symbol table (in `order`), then the string table.
    uint32_t symoff = (uint32_t)((img.len + 7) & ~(size_t)7);
    while (img.len < symoff) buf_put8(&img, 0);
    for (size_t i = 0; i < norder; i++) {
        ESym* s = order[i];
        buf_put32(&img, s->strx);
        buf_put8(&img, s->type ? s->type : N_EXT_); // undefined: N_EXT only
        buf_put8(&img, s->sect);
        buf_put16(&img, 0);                         // n_desc
        uint64_t value = s->value;
        if (s->sect > 0 && s->sect <= nsecs) value += o->secs[s->sect - 1].addr;
        buf_put64(&img, value);
    }
    uint32_t stroff = (uint32_t)img.len;
    buf_write(&img, str.d, str.len);

    buf_patch32(&img, symcmd + 8, symoff);
    buf_patch32(&img, symcmd + 16, stroff);

    size_t object_size = img.len;
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "ace: cannot write '%s'\n", path);
        o->d->errors++;
    } else {
        fwrite(img.d, 1, img.len, fp);
        fclose(fp);
    }
    free(str.d);
    free(img.d);
    free(order);
    for (int i = 0; i < nsecs; i++) free(relocs[i].d);
    return o->d->errors ? 0 : object_size;
}
