// elf.h (v2): writing ELF executables for the native backend (see elf.c).
#ifndef MODAL_V2_ELF_H
#define MODAL_V2_ELF_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum
{
    ELF_ARM64,
    ELF_X86_64,
} ElfCpu;

// Where the code is placed in the file and in memory: what a pc-relative fix-up needs.
#define ELF_BASE 0x400000u
#define ELF_CODE_OFFSET 0xb0u // after the header and two program headers

// `code` is position-independent machine code with no imports; `entry` the offset of the
// first instruction to run (the program's _start). One segment, read and execute.
int elf_write_executable(FILE *out, ElfCpu cpu, const uint8_t *code, size_t len, size_t entry);
// Bare metal: the code alone is loaded, at `base` (the memory map's flash), so its first byte
// is the flash's first byte; the headers stay in the file for the loader.
int elf_write_image(FILE *out, ElfCpu cpu, const uint8_t *code, size_t len, size_t entry,
                    uint64_t base);

#endif // MODAL_V2_ELF_H
