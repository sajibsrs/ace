// elf.c — the ELF64 x86-64 relocatable object for target linux-x64: the same
// Obj the backends fill (sections, symbols, relocations, DWARF; macho.c),
// written in ELF form instead of Mach-O.
//
// Pinned conventions:
//   - sections in Obj order after the null section: .text .rodata .data .bss
//     .debug_abbrev .debug_info .debug_line, then a .rela section per
//     relocated section, .note.GNU-stack (non-executable stack), .symtab,
//     .strtab, .shstrtab. An empty debug section is still written.
//   - Rel.type is an R_X86_64_* type, except type 0 (RM_UNSIGNED from the
//     shared DWARF writers), which is an absolute address of Rel.len
//     (3: R_X86_64_64, 2: R_X86_64_32).
//   - A relocation always names its symbol, local or not (ELF has no section-
//     number form); Rel.ext only mirrors Mach-O. Its addend is the value the
//     backend stored in the field (4 or 8 bytes by Rel.len), which the writer
//     moves into the .rela entry and clears, so the result is S + A (- P).
//   - symbols: null, then locals in Obj order, then defined globals, then
//     undefined globals; st_value is the offset within its section. A defined
//     symbol in .text is STT_FUNC (the __ace$text anchor aside), one
//     elsewhere STT_OBJECT.

#include "backend.h"

#include <stdio.h>

enum { // ELF section indices: SEC_x -> 1 + x, then these
    SH_RELA0 = 1 + NSECS,
};

enum {
    SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8,
    SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4, SHF_INFO_LINK = 0x40,
    R_X86_64_64 = 1, R_X86_64_32 = 10,
};

static const struct { const char* name; uint32_t type, flags; } ELF_SECS[NSECS] = {
    [SEC_TEXT]         = { ".text",         SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR },
    [SEC_CONST]        = { ".rodata",       SHT_PROGBITS, SHF_ALLOC },
    [SEC_DATA]         = { ".data",         SHT_PROGBITS, SHF_ALLOC | SHF_WRITE },
    [SEC_BSS]          = { ".bss",          SHT_NOBITS,   SHF_ALLOC | SHF_WRITE },
    [SEC_DEBUG_ABBREV] = { ".debug_abbrev", SHT_PROGBITS, 0 },
    [SEC_DEBUG_INFO]   = { ".debug_info",   SHT_PROGBITS, 0 },
    [SEC_DEBUG_LINE]   = { ".debug_line",   SHT_PROGBITS, 0 },
};

typedef struct {
    uint32_t name, type;
    uint64_t flags, off, size;
    uint32_t link, info;
    uint64_t align, entsize;
} Shdr;

static void put_shdr(Buf* b, const Shdr* h) {
    buf_put32(b, h->name);
    buf_put32(b, h->type);
    buf_put64(b, h->flags);
    buf_put64(b, 0);            // sh_addr
    buf_put64(b, h->off);
    buf_put64(b, h->size);
    buf_put32(b, h->link);
    buf_put32(b, h->info);
    buf_put64(b, h->align);
    buf_put64(b, h->entsize);
}

static uint32_t str_add(Buf* b, const char* s) {
    uint32_t at = (uint32_t)b->len;
    buf_write(b, s, strlen(s) + 1);
    return at;
}

static void pad_to(Buf* b, uint64_t align) {
    while (b->len % align) buf_put8(b, 0);
}

size_t elf_write(Obj* o, const char* path) {
    for (int i = 0; i < NSECS; i++) if (i != SEC_BSS) o->secs[i].size = o->secs[i].data.len;

    // Symbols: locals, defined globals, undefined; ord is the .symtab index.
    Buf str;
    memset(&str, 0, sizeof str);
    buf_put8(&str, 0);
    ESym** order = malloc((o->syms.n ? o->syms.n : 1) * sizeof(ESym*));
    if (!order) oom();
    size_t norder = 0, nloc = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < o->syms.n; i++) {
            ESym* s = o->syms.v[i];
            int want = pass == 0 ? (s->type && !(s->type & N_EXT_))
                    : pass == 1 ? (s->type && (s->type & N_EXT_))
                                : !s->type;
            if (!want) continue;
            s->strx = str_add(&str, s->name);
            s->ord = (uint32_t)(norder + 1);
            order[norder++] = s;
            if (pass == 0) nloc++;
        }
    }

    // Relocations, their stored addends moved into the entries.
    Buf rela[NSECS];
    memset(rela, 0, sizeof rela);
    for (int i = 0; i < NSECS; i++) {
        Sec* s = &o->secs[i];
        for (int j = 0; j < s->nrl; j++) {
            Rel* r = &s->rl[j];
            uint32_t type = r->type ? r->type : r->len == 3 ? R_X86_64_64 : R_X86_64_32;
            int64_t addend;
            uint8_t* at = s->data.d + r->off;
            if (r->len == 3) {
                uint64_t v = 0;
                for (int k = 7; k >= 0; k--) v = v << 8 | at[k];
                addend = (int64_t)v;
                memset(at, 0, 8);
            } else {
                uint32_t v = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
                addend = (int32_t)v;
                memset(at, 0, 4);
            }
            buf_put64(&rela[i], r->off);
            buf_put64(&rela[i], (uint64_t)r->sym->ord << 32 | type);
            buf_put64(&rela[i], (uint64_t)addend);
        }
    }

    // Section header names.
    Buf shs;
    memset(&shs, 0, sizeof shs);
    buf_put8(&shs, 0);
    uint32_t sec_name[NSECS], rela_name[NSECS];
    for (int i = 0; i < NSECS; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, ".rela%s", ELF_SECS[i].name);
        rela_name[i] = str_add(&shs, nm);
        sec_name[i] = rela_name[i] + 5; // ".rela.text" ends in ".text"
    }
    uint32_t stack_name = str_add(&shs, ".note.GNU-stack");
    uint32_t symtab_name = str_add(&shs, ".symtab");
    uint32_t strtab_name = str_add(&shs, ".strtab");
    uint32_t shstrtab_name = str_add(&shs, ".shstrtab");

    // The file image: header, section contents, then section headers.
    Buf img;
    memset(&img, 0, sizeof img);
    for (int i = 0; i < 64; i++) buf_put8(&img, 0); // ELF header, filled below

    Shdr sh[1 + 2 * NSECS + 4];
    memset(sh, 0, sizeof sh);
    int nsh = 1;
    for (int i = 0; i < NSECS; i++) {
        Sec* s = &o->secs[i];
        uint64_t a = (uint64_t)1 << s->align;
        pad_to(&img, a);
        sh[nsh] = (Shdr){ sec_name[i], ELF_SECS[i].type, ELF_SECS[i].flags, img.len, s->size, 0, 0, a, 0 };
        if (i != SEC_BSS) buf_write(&img, s->data.d, s->data.len);
        nsh++;
    }
    int symtab_ix = nsh + 1; // after the stack note
    for (int i = 0; i < NSECS; i++) {
        if (!rela[i].len) continue;
        pad_to(&img, 8);
        sh[nsh++] = (Shdr){ rela_name[i], SHT_RELA, SHF_INFO_LINK, img.len, rela[i].len, 0, (uint32_t)(1 + i), 8, 24 };
        buf_write(&img, rela[i].d, rela[i].len);
        symtab_ix++;
    }
    sh[nsh++] = (Shdr){ stack_name, SHT_PROGBITS, 0, img.len, 0, 0, 0, 1, 0 };
    for (int i = 1; i < nsh; i++) if (sh[i].type == SHT_RELA) sh[i].link = (uint32_t)symtab_ix;

    // .symtab
    pad_to(&img, 8);
    uint64_t symoff = img.len;
    for (int i = 0; i < 24; i++) buf_put8(&img, 0); // null symbol
    for (size_t i = 0; i < norder; i++) {
        ESym* s = order[i];
        int defined = s->type != 0;
        uint8_t bind = (!defined || (s->type & N_EXT_)) ? 1 : 0; // STB_GLOBAL : STB_LOCAL
        // NOTYPE, FUNC, OBJECT; the __ace$text anchor is no function, so a
        // symbolizer names the function at offset 0 instead of it
        uint8_t kind = !defined || strcmp(s->name, "__ace$text") == 0 ? 0 : s->sect == 1 + SEC_TEXT ? 2 : 1;
        buf_put32(&img, s->strx);
        buf_put8(&img, (uint8_t)(bind << 4 | kind));
        buf_put8(&img, 0);                           // st_other: default visibility
        buf_put16(&img, defined ? s->sect : 0);      // SEC_x + 1 is its ELF index
        buf_put64(&img, defined ? s->value : 0);
        buf_put64(&img, 0);                          // st_size
    }
    sh[nsh++] = (Shdr){ symtab_name, SHT_SYMTAB, 0, symoff, img.len - symoff,
                        (uint32_t)(symtab_ix + 1), (uint32_t)(1 + nloc), 8, 24 };
    uint64_t stroff = img.len;
    buf_write(&img, str.d, str.len);
    sh[nsh++] = (Shdr){ strtab_name, SHT_STRTAB, 0, stroff, str.len, 0, 0, 1, 0 };
    uint64_t shstroff = img.len;
    buf_write(&img, shs.d, shs.len);
    sh[nsh++] = (Shdr){ shstrtab_name, SHT_STRTAB, 0, shstroff, shs.len, 0, 0, 1, 0 };

    pad_to(&img, 8);
    uint64_t shoff = img.len;
    for (int i = 0; i < nsh; i++) put_shdr(&img, &sh[i]);

    // Elf64_Ehdr
    static const uint8_t ident[16] = { 0x7f, 'E', 'L', 'F', 2, 1, 1, 0 }; // 64-bit, LSB, v1, SysV
    memcpy(img.d, ident, 16);
    Buf h = { img.d + 16, 0, 48 };
    buf_put16(&h, 1);                   // ET_REL
    buf_put16(&h, 62);                  // EM_X86_64
    buf_put32(&h, 1);                   // EV_CURRENT
    buf_put64(&h, 0);                   // e_entry
    buf_put64(&h, 0);                   // e_phoff
    buf_put64(&h, shoff);
    buf_put32(&h, 0);                   // e_flags
    buf_put16(&h, 64);                  // e_ehsize
    buf_put16(&h, 0);                   // e_phentsize
    buf_put16(&h, 0);                   // e_phnum
    buf_put16(&h, 64);                  // e_shentsize
    buf_put16(&h, (uint16_t)nsh);
    buf_put16(&h, (uint16_t)(nsh - 1)); // e_shstrndx: .shstrtab is last

    size_t object_size = img.len;
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "ace: cannot write '%s'\n", path);
        o->d->errors++;
    } else {
        fwrite(img.d, 1, img.len, fp);
        fclose(fp);
    }
    free(img.d);
    free(str.d);
    free(shs.d);
    free(order);
    for (int i = 0; i < NSECS; i++) free(rela[i].d);
    return o->d->errors ? 0 : object_size;
}
