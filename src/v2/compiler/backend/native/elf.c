// elf.c (v2): a static ELF executable, written directly (no linker, no libc).
//
// The smallest file the kernel runs: the ELF header, one PT_LOAD segment that maps the whole
// file read+execute, and PT_GNU_STACK so the stack is not executable. The program needs no
// data segment: the runtime context and the value stack live on the stack, and string bytes
// are placed in the code (backend/native/emit.c). A hello world is a few hundred bytes.
//
// Layout (offsets in the file; the file is mapped at ELF_BASE):
//
//   0x00  Elf64_Ehdr (64 bytes)
//   0x40  Elf64_Phdr PT_LOAD      (56 bytes)
//   0x78  Elf64_Phdr PT_GNU_STACK (56 bytes)
//   0xb0  code
//
// No section headers: the kernel does not read them (tools show the segments).
#include "v2/compiler/backend/native/elf.h"
#include <string.h>

#define EM_X86_64 62
#define EM_AARCH64 183
#define PT_LOAD 1
#define PT_GNU_STACK 0x6474e551u
#define PF_X 1
#define PF_W 2
#define PF_R 4

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void phdr(uint8_t *p, uint32_t type, uint32_t flags, uint64_t off, uint64_t addr,
                 uint64_t size, uint64_t align)
{
    put32(p + 0, type);
    put32(p + 4, flags);
    put64(p + 8, off);
    put64(p + 16, addr);  // p_vaddr
    put64(p + 24, addr);  // p_paddr
    put64(p + 32, size);  // p_filesz
    put64(p + 40, size);  // p_memsz
    put64(p + 48, align);
}

static int write_elf(FILE *out, ElfCpu cpu, const uint8_t *code, size_t len, size_t entry,
                     uint64_t base, int bare);

int elf_write_executable(FILE *out, ElfCpu cpu, const uint8_t *code, size_t len, size_t entry)
{
    return write_elf(out, cpu, code, len, entry, ELF_BASE, 0);
}

int elf_write_image(FILE *out, ElfCpu cpu, const uint8_t *code, size_t len, size_t entry,
                    uint64_t base)
{
    return write_elf(out, cpu, code, len, entry, base, 1);
}

static int write_elf(FILE *out, ElfCpu cpu, const uint8_t *code, size_t len, size_t entry,
                     uint64_t base, int bare)
{
    // Hosted: the whole file is mapped from offset 0, so the code is at base + 0xb0, 64 KB
    // aligned (valid with 4, 16 and 64 KB pages). Bare: only the code, at base itself; a
    // loader copies bytes, so 16 is enough (address and offset agree modulo it).
    uint64_t code_at = bare ? base : base + ELF_CODE_OFFSET;
    uint8_t h[ELF_CODE_OFFSET];
    memset(h, 0, sizeof(h));
    // e_ident: magic, 64-bit, little-endian, version 1, System V ABI
    memcpy(h, "\x7f" "ELF", 4);
    h[4] = 2;
    h[5] = 1;
    h[6] = 1;
    put16(h + 16, 2); // ET_EXEC
    put16(h + 18, cpu == ELF_ARM64 ? EM_AARCH64 : EM_X86_64);
    put32(h + 20, 1);
    put64(h + 24, code_at + entry); // e_entry
    put64(h + 32, 0x40);                               // e_phoff
    put64(h + 40, 0);                                  // e_shoff: no sections
    put32(h + 48, 0);                                  // e_flags
    put16(h + 52, 64);                                 // e_ehsize
    put16(h + 54, 56);                                 // e_phentsize
    put16(h + 56, 2);                                  // e_phnum
    put16(h + 58, 64);                                 // e_shentsize
    put16(h + 60, 0);                                  // e_shnum
    put16(h + 62, 0);                                  // e_shstrndx
    if (bare)
    {
        phdr(h + 0x40, PT_LOAD, PF_R | PF_X, ELF_CODE_OFFSET, base, len, 16);
    }
    else
    {
        phdr(h + 0x40, PT_LOAD, PF_R | PF_X, 0, base, ELF_CODE_OFFSET + len, 0x10000);
    }
    phdr(h + 0x78, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 16);

    if (fwrite(h, 1, sizeof(h), out) != sizeof(h) || fwrite(code, 1, len, out) != len)
    {
        return 1;
    }
    return 0;
}
