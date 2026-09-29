// elf_object.c — the ELF64 x86-64 object writer (src/elf.c) before any
// backend fills it: a hand-encoded main exercises each relocation form.
//
// Built by tests/run.sh on a linux-x64 host with the compiler's sources, it
// writes an object (argv[1]) whose main prints through an absolute pointer
// (R_X86_64_64), calls puts (R_X86_64_PLT32), stores to .bss and loads .data
// (R_X86_64_PC32), and carries DWARF line rows; linked by cc, it prints
// "elf object" and exits 42.
#include "../src/backend.h"

#include <stdio.h>

static void text(Obj* o, const uint8_t* p, size_t n) { buf_write(obj_text(o), p, n); }

// A 4-byte field at the text cursor relocated against sym, its addend stored.
static void rel32(Obj* o, ESym* sym, uint32_t type, int32_t addend) {
    Sec* t = &o->secs[SEC_TEXT];
    reloc_add(t, (uint32_t)t->data.len, sym, 1, 2, 1, type);
    buf_put32(&t->data, (uint32_t)addend);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    enum { PC32 = 2, PLT32 = 4 };
    Diag d = { "tests/elf_object.c", 0 };
    Obj o;
    obj_init(&o, &d, EMIT_DEBUG);
    Sec* s = o.secs;

    ESym* msg = sym_define(&o.syms, s, "msg", 3, 0, &s[SEC_CONST]);
    buf_write(&s[SEC_CONST].data, "elf object", 11);
    ESym* ptr = sym_define(&o.syms, s, "ptr", 3, 0, &s[SEC_DATA]);
    reloc_add(&s[SEC_DATA], (uint32_t)s[SEC_DATA].data.len, msg, 0, 3, 0, 0);
    buf_put64(&s[SEC_DATA].data, 0);
    ESym* val = sym_define(&o.syms, s, "val", 3, 0, &s[SEC_DATA]);
    buf_put32(&s[SEC_DATA].data, 40);
    ESym* bss = sym_define(&o.syms, s, "bss", 3, 0, &s[SEC_BSS]);
    s[SEC_BSS].size += 4;
    ESym* puts_ = sym_intern(&o.syms, "puts", 4);

    sym_define(&o.syms, s, "main", 4, 1, &s[SEC_TEXT]);
    obj_line(&o, 0, 1, 1, d.path);
    text(&o, (const uint8_t[]){ 0x48, 0x83, 0xEC, 0x08 }, 4); // sub rsp, 8
    text(&o, (const uint8_t[]){ 0x48, 0x8B, 0x3D }, 3);       // mov rdi, [rip + ptr]
    rel32(&o, ptr, PC32, -4);
    obj_line(&o, (uint32_t)obj_text(&o)->len, 2, 1, d.path);
    text(&o, (const uint8_t[]){ 0xE8 }, 1);                   // call puts
    rel32(&o, puts_, PLT32, -4);
    text(&o, (const uint8_t[]){ 0xC7, 0x05 }, 2);             // mov dword [rip + bss], 2
    rel32(&o, bss, PC32, -8);
    text(&o, (const uint8_t[]){ 2, 0, 0, 0 }, 4);
    text(&o, (const uint8_t[]){ 0x8B, 0x05 }, 2);             // mov eax, [rip + val]
    rel32(&o, val, PC32, -4);
    text(&o, (const uint8_t[]){ 0x03, 0x05 }, 2);             // add eax, [rip + bss]
    rel32(&o, bss, PC32, -4);
    text(&o, (const uint8_t[]){ 0x48, 0x83, 0xC4, 0x08, 0xC3 }, 5); // add rsp, 8; ret
    obj_debug_fn(&o, 0, (uint32_t)obj_text(&o)->len, "main", 4);

    ESym* anchor = sym_intern(&o.syms, "__ace$text", 10);
    obj_debug_info(&o, anchor);
    obj_debug_line(&o, anchor);
    return elf_write(&o, argv[1]) ? 0 : 1;
}
