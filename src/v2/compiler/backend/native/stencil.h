// stencil.h (v2): what the native backend knows about a stencil (see stencils.c, stencilgen.c).
#ifndef MODAL_V2_STENCIL_H
#define MODAL_V2_STENCIL_H

#include <stddef.h>
#include <stdint.h>

typedef enum
{
    SR_MOVW_G0, // arm64 movz/movk: one 16-bit piece of a 64-bit immediate
    SR_MOVW_G1,
    SR_MOVW_G2,
    SR_MOVW_G3,
    SR_BRANCH26, // arm64 b / bl
    SR_BRANCH19, // arm64 b.cond / cbz / cbnz
    SR_BRANCH14, // arm64 tbz / tbnz
    SR_ABS32,    // x86-64: a 32-bit immediate, zero-extended
    SR_ABS32S,   // x86-64: a 32-bit immediate, sign-extended
    SR_REL32,    // x86-64: a 32-bit pc-relative field (calls and jumps), with its addend
} StencilRelocKind;

enum
{
    ST_HOLE_A,
    ST_HOLE_B,
    ST_HOLE_C,
    ST_CONTINUE,
    ST_JUMP,
    ST_CALLEE,
    ST_RUNTIME = 16,
    ST_EXTERN = 128,
};

typedef struct
{
    uint32_t offset;
    uint8_t kind;
    int8_t addend;
    uint16_t target;
} StencilReloc;

typedef struct
{
    const uint8_t *code;
    uint32_t size;
    uint32_t nrelocs;
    const StencilReloc *relocs;
    // The compact form (arm64): valid when every hole value fits in 16 bits, each hole being one
    // movz instead of a movz and three movk. Elsewhere the same as the full form.
    const uint8_t *ccode;
    uint32_t csize;
    uint32_t ncrelocs;
    const StencilReloc *crelocs;
} Stencil;

typedef enum
{
    CPU_ARM64,
    CPU_X86_64,
} NativeCpu;

typedef struct
{
    const char *name;
    NativeCpu cpu;
    const Stencil *stencils; // by ST_* id
    const Stencil *runtime;  // by RT_* id
    const char *const *externs;
    uint64_t set_hash; // STENCIL_SET_HASH of its header: the ids must be the same everywhere
} NativeArch;

extern const NativeArch native_arm64;
extern const NativeArch native_x86_64;

#endif // MODAL_V2_STENCIL_H
