// macho.h (v2): writing Mach-O files for the native backend (see macho.c).
#ifndef MODAL_V2_MACHO_H
#define MODAL_V2_MACHO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum
{
    MACHO_ARM64,
    MACHO_X86_64,
} MachoCpu;

typedef struct
{
    // offset in the code of a call to a C library function: the `bl` (arm64) or the call's
    // 32-bit pc-relative field (x86-64)
    size_t at;
    uint32_t sym; // which one: an index in `imports`
} MachoImportCall;

// `code` is position-independent arm64 code; `entry` the offset of `int main(void)` in it;
// `imports` the C library functions it calls, NULL-terminated. `ident` names the signature.
int macho_write_executable(FILE *out, MachoCpu cpu, const uint8_t *code, size_t len, size_t entry,
                           const MachoImportCall *calls, size_t ncalls, const char *const *imports,
                           const char *ident);
int macho_write_object(FILE *out, MachoCpu cpu, const uint8_t *code, size_t len, size_t entry,
                       const MachoImportCall *calls, size_t ncalls, const char *const *imports);

#endif // MODAL_V2_MACHO_H
