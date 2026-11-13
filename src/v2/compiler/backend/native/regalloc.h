// regalloc.h (v2): which values of a function live in machine registers (native backend).
#ifndef MODAL_V2_REGALLOC_H
#define MODAL_V2_REGALLOC_H

#include "v2/compiler/mir/mir.h"

#define RA_NONE 0xff // the value stays in the frame
// A value split from a slot (see regalloc.c): named RA_VIRTUAL | k in RegAlloc.code. Its place in
// the frame, when it gets no register, is its slot's (RegAlloc.orig[k]).
#define RA_VIRTUAL 0x20000000u

typedef struct
{
    MirInst *code;     // the function with its slots split into values: what is generated
    uint8_t *reg;      // per name (a slot, or norig + k for value k): a register number, or RA_NONE
    uint8_t *live_in;  // per slot: its value at entry is read (a parameter): loaded first
    uint8_t *fused;    // per instruction: a comparison whose only reader is the branch after it
    uint8_t *home;     // per slot: some value of it stays in the frame, so the slot keeps its place
    uint32_t *orig;    // per split value: the slot it came from
    uint32_t norig;    // slots (0 .. norig - 1)
    uint32_t nnames;   // slots and split values
    int used;          // preserved registers used: 0 .. used - 1 (saved and restored)
} RegAlloc;

// The index of name `s` (a slot or RA_VIRTUAL | k) in RegAlloc.reg.
static inline uint32_t ra_index(const RegAlloc *ra, MirSlot s)
{
    return s & RA_VIRTUAL ? ra->norig + (s & ~RA_VIRTUAL) : s;
}

// The frame slot of name `s`.
static inline MirSlot ra_slot(const RegAlloc *ra, MirSlot s)
{
    return s & RA_VIRTUAL ? ra->orig[s & ~RA_VIRTUAL] : s;
}

// Register allocation by linear scan over live ranges (Poletto & Sarkar), after splitting each
// slot into its values (webs): the lowering reuses a temporary slot for unrelated values, and one
// range for all of them would cross every call. A slot whose address is never taken, that is not
// part of a call window and is below 4096 is split and allocated; the others stay in the frame.
// Two classes: registers 0 .. preserved - 1 survive the instructions marked in `clobber` (calls
// out); registers preserved .. preserved + scratch - 1 do not, and are given to ranges no such
// instruction crosses (nothing to save). A comparison read only by the branch right after it gets
// no register at all (`fused`): the two become one compare-and-branch.
// Returns 0 when the function is not allocated (too large to analyse).
int regalloc(const MirFunc *f, int preserved, int scratch, const uint8_t *clobber, RegAlloc *out);

#endif // MODAL_V2_REGALLOC_H
