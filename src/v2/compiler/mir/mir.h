// mir.h (v2): the mid-level IR. A checked function, lowered for the machine-code backends.
//
// Between the checked tree and a code generator sits one flat array of three-address
// instructions per function (Cranelift and QBE keep the same shape): every operand is a frame
// slot, every operation has the exact integer variant it computes in, and every decision about
// C semantics (promotions, conversions, short-circuit evaluation, constant folding) was taken
// here, once, for every target. A backend only maps instructions to machine code.
//
// The frame of a function, in 8-byte slots:
//
//   [ self, params ][ locals and temporaries ][ constructor values ][ call window ... )
//   0                                          perm_base             frame_size
//
// A call puts its arguments at frame_size + i: that is the callee's frame, and its slot 0
// holds the result when the call returns (register windows, as in Lua).
#ifndef MODAL_V2_MIR_H
#define MODAL_V2_MIR_H

#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>

typedef uint32_t MirSlot;

typedef enum
{
    MIR_CONST,  // d = imm
    MIR_COPY,   // d = a
    MIR_ADDR,   // d = address of slot a
    MIR_ZERO,   // slots a .. a + imm - 1 = 0
    MIR_LOAD,   // d = 8 bytes at (a + imm)
    MIR_STORE,  // 8 bytes at (a + imm) = b
    MIR_BIN,    // d = a <sub> b, in variant `var`
    MIR_BINI,   // d = a <sub> imm, in variant `var` (imm fits in 16 bits)
    MIR_NEG,    // d = -a, in variant `var`
    MIR_NOT,    // d = !a
    MIR_NORM,   // d = a converted to variant `var`
    MIR_LABEL,  // imm = label number
    MIR_JZ,     // if a == 0: go to label imm
    MIR_JNZ,    // if a != 0: go to label imm
    MIR_CALL,   // call function `imm` (a SymId); its frame starts at frame_size
    MIR_RET,    // return (the result, if any, is already in slot 0)
    MIR_STR,    // d = address of string `imm` (an index in MirFunc.strings)
    MIR_WRITE,  // write a, as `sub` (MirWrite)
    MIR_GLOAD,  // d = module-level variable imm
    MIR_GSTORE, // module-level variable imm = a
    MIR__COUNT,
} MirOp;

typedef enum
{
    MB_ADD,
    MB_SUB,
    MB_MUL,
    MB_DIV,
    MB_EQ,
    MB_NE,
    MB_LT,
    MB_LE,
    MB_GT,
    MB_GE,
    MB__COUNT,
} MirBinOp;

typedef enum
{
    MW_STR,
    MW_INT,
    MW_UINT,
    MW_BOOL,
    MW_SPACE, // no operand
    MW_NL,    // no operand
} MirWrite;

// The integer variants: the C types the C backend's output computes in. A slot holds its value
// zero-extended (unsigned) or sign-extended (signed) to 64 bits.
typedef enum
{
    MV_U8,
    MV_U16,
    MV_U32,
    MV_U64,
    MV_I8,
    MV_I16,
    MV_I32,
    MV_I64,
    MV__COUNT,
} MirVariant;

typedef struct
{
    uint8_t op;  // MirOp
    uint8_t sub; // MirBinOp or MirWrite
    uint8_t var; // MirVariant
    MirSlot d, a, b;
    uint64_t imm;
} MirInst;

typedef struct
{
    const uint8_t *bytes;
    size_t len;
} MirString;

typedef struct MirFunc
{
    Unit *u;
    SymId sym;
    MirInst *code;
    size_t n, cap;
    MirString *strings;
    size_t nstrings, strings_cap;
    uint32_t nlabels;
    uint32_t frame_size; // slots before the call window
    uint8_t returns;     // has a result in slot 0
    // Why it could not be lowered (mir_lower returned 0): the token and what is missing.
    uint32_t fail_tok;
    const char *fail_what;
} MirFunc;

// Lowers a checked function whose body is in memory. Returns 0 after reporting what cannot be
// lowered yet. The function lives in its own region until the next call.
int mir_lower(Unit *u, SymId fn, MirFunc *out);

// Can this function be generated at all? (report: say why not)
int mir_lowerable(Unit *u, SymId fn, int report);

// Compile-time arithmetic in a variant, exactly as the generated code computes it.
uint64_t mir_normalize(uint64_t v, int variant);

void mir_print(const MirFunc *f, FILE *out); // --mir

// Module-level variables live at the bottom of the value stack: how many there are (for the
// entry, which puts main's frame after them).
uint32_t mir_global_count(void);

#endif // MODAL_V2_MIR_H
