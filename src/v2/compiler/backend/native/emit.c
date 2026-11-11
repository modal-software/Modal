// backend/native/emit.c (v2): the native backends. Machine code by copy-and-patch, from the MIR.
//
//   --backend native   this machine's CPU     --backend arm64     --backend x86_64
//
// Every MIR instruction is one stencil: machine code compiled by clang from stencils.c when
// Modal was built, with holes. Generating a function is copying its stencils one after another
// and filling their holes (slot numbers, constants, branch targets), with the JIT techniques
// that make that fast and good, applied ahead of time:
//
//   - a stencil is chosen by the exact variant the MIR gives (u8 + wraps, i32 / divides signed),
//     so there are no guards and nothing is speculated
//   - the jump at the end of each stencil to the next one is dropped: they fall through
//   - arm64: a stencil whose holes all fit in 16 bits is copied in its compact form, one movz
//     per hole instead of a movz and three movk (built by stencilgen)
//   - a call to a function not emitted yet gets a hole and a fix-up, filled when it is (TCC's
//     backpatching; safe, because every declaration is known before any body)
//
// The output is a signed Mach-O executable written by macho.c, or an object file for `-o x.o`.
// Strings live in the code, right after the instruction that takes their address, so the code
// holds no absolute address and needs no relocation when loaded.
#include "v2/compiler/backend/backend.h"
#include "v2/compiler/backend/native/macho.h"
#include "v2/compiler/backend/native/stencil.h"
#include "v2/compiler/mir/mir.h"
#include <stdlib.h>
#include <string.h>

#define STENCIL_IDS_ONLY // the ids (the same for every CPU); the tables are in arch_*.c
#include "v2/compiler/backend/native/stencils_arm64.h"

typedef struct
{
    size_t at;     // the field to fill
    uint8_t reloc; // StencilRelocKind
    int8_t addend;
    uint32_t ref; // a label or a function symbol
} Fixup;

#define MAX_LABELS 4096
#define NOT_EMITTED ((size_t)-1)

typedef struct
{
    const NativeArch *arch;
    Unit *u;
    int errors;

    uint8_t *code;
    size_t len, cap;
    size_t runtime_at[RT__COUNT];
    size_t entry_at;

    size_t *fn_at; // per symbol: where its code starts, NOT_EMITTED until then
    size_t fn_at_cap; // symbols can be added while generating (function literals)
    Fixup *calls; // every call to a Modal function, in the order emitted; filled at the end
    size_t ncalls, calls_cap;
    // The functions in the order emitted (the entry last): where each starts, and its first
    // call in `calls`. Only those reachable from the entry are kept (see shake).
    struct FnRange
    {
        SymId sym;
        size_t at;
        size_t first_call;
    } *order;
    size_t norder, order_cap;
    MachoImportCall *externs; // calls to the C library
    size_t nexterns, externs_cap;

    Fixup *rtcalls; // calls into the runtime, which is placed (only what is used) at the end
    size_t nrtcalls, rtcalls_cap;

    Fixup *jumps; // branches of the function being emitted
    size_t njumps, jumps_cap;
    size_t labels[MAX_LABELS];
} Native;

static Native nat;

static void fatal(const char *what)
{
    fprintf(stderr, "fatal: backend %s: %s\n", nat.arch->name, what);
    exit(2);
}

// The per-symbol table covers every symbol, including those added since (function literals).
static void fn_at_fit(void)
{
    if (nat.fn_at_cap >= cx.syms.cap)
    {
        return;
    }
    size_t *grown = (size_t *)arena_alloc(cx.syms.cap * sizeof(size_t));
    for (size_t i = 0; i < cx.syms.cap; i++)
    {
        grown[i] = i < nat.fn_at_cap ? nat.fn_at[i] : NOT_EMITTED;
    }
    nat.fn_at = grown;
    nat.fn_at_cap = cx.syms.cap;
}

// Lists grow in the arena (aligned doubling); the code has its own region and grows in place.
static void *grow_list(void *data, size_t elem, size_t *cap, size_t need)
{
    while (*cap < need)
    {
        data = arena_grow(data, elem, cap);
    }
    return data;
}

static void put_bytes(const void *p, size_t n)
{
    if (nat.len + n > nat.cap)
    {
        nat.code = (uint8_t *)region_grow_array(REGION_CODE, nat.code, 1, &nat.cap, nat.len + n);
    }
    memcpy(nat.code + nat.len, p, n);
    nat.len += n;
}

static void put32(uint32_t w)
{
    put_bytes(&w, 4);
}

static uint32_t get32(size_t at)
{
    uint32_t w;
    memcpy(&w, nat.code + at, 4);
    return w;
}

static void set32(size_t at, uint32_t w)
{
    memcpy(nat.code + at, &w, 4);
}

// ---- filling holes --------------------------------------------------------------------------------

// A value hole: an immediate in the instruction at `at`.
static void fill_value(size_t at, uint8_t reloc, uint64_t value)
{
    switch (reloc)
    {
    case SR_MOVW_G0:
    case SR_MOVW_G1:
    case SR_MOVW_G2:
    case SR_MOVW_G3:
    {
        uint32_t imm = (uint32_t)(value >> (16 * (reloc - SR_MOVW_G0))) & 0xffffu;
        set32(at, (get32(at) & ~(0xffffu << 5)) | (imm << 5));
        return;
    }
    case SR_ABS32:
        if (value > 0xffffffffull)
        {
            fatal("a value does not fit a 32-bit hole");
        }
        set32(at, (uint32_t)value);
        return;
    case SR_ABS32S:
        if (value > 0x7fffffffull)
        {
            fatal("a value does not fit a signed 32-bit hole");
        }
        set32(at, (uint32_t)value);
        return;
    default:
        fatal("a value in a branch hole");
    }
}

// A branch or call hole at `at`, aimed at `target`.
static void fill_branch(size_t at, uint8_t reloc, int8_t addend, size_t target)
{
    if (reloc == SR_REL32)
    {
        int64_t rel = (int64_t)target + addend - (int64_t)at;
        if (rel > INT32_MAX || rel < INT32_MIN)
        {
            fatal("a branch is out of range");
        }
        set32(at, (uint32_t)(int32_t)rel);
        return;
    }
    int64_t delta = ((int64_t)target - (int64_t)at) / 4;
    uint32_t w = get32(at);
    int64_t limit = reloc == SR_BRANCH26 ? (1 << 25) : reloc == SR_BRANCH19 ? (1 << 18) : (1 << 13);
    if (delta >= limit || delta < -limit)
    {
        fatal("a branch is out of range");
    }
    if (reloc == SR_BRANCH26)
    {
        w = (w & 0xfc000000u) | ((uint32_t)delta & 0x03ffffffu);
    }
    else if (reloc == SR_BRANCH19)
    {
        w = (w & ~(0x7ffffu << 5)) | (((uint32_t)delta & 0x7ffffu) << 5);
    }
    else
    {
        w = (w & ~(0x3fffu << 5)) | (((uint32_t)delta & 0x3fffu) << 5);
    }
    set32(at, w);
}

static void push_fixup(Fixup **list, size_t *n, size_t *cap, Fixup f)
{
    if (*n == *cap)
    {
        *list = (Fixup *)grow_list(*list, sizeof(Fixup), cap, *n + 1);
    }
    (*list)[(*n)++] = f;
}

static void push_extern(size_t at, uint32_t sym)
{
    if (nat.nexterns == nat.externs_cap)
    {
        nat.externs = (MachoImportCall *)grow_list(nat.externs, sizeof(MachoImportCall),
                                                   &nat.externs_cap, nat.nexterns + 1);
    }
    nat.externs[nat.nexterns++] = (MachoImportCall){at, sym};
}

// Is the stencil's last instruction its jump to the next stencil? It is then left out.
static size_t without_final_jump(const Stencil *s)
{
    for (uint32_t i = 0; i < s->nrelocs; i++)
    {
        const StencilReloc *r = &s->relocs[i];
        if (r->target != ST_CONTINUE || r->offset != s->size - 4)
        {
            continue;
        }
        if (r->kind == SR_BRANCH26)
        {
            return s->size - 4; // arm64: b
        }
        if (r->kind == SR_REL32 && s->size >= 5 && s->code[s->size - 5] == 0xe9)
        {
            return s->size - 5; // x86-64: jmp rel32
        }
    }
    return s->size;
}

// Copies stencil `id` to the end of the code and fills its holes with a, b, c. `callee` is the
// function a call stencil calls, `label` where a branch stencil jumps.
static size_t emit(int id, uint64_t a, uint64_t b, uint64_t c, SymId callee, uint32_t label)
{
    const Stencil *full = &nat.arch->stencils[id];
    const Stencil compact = {full->ccode, full->csize, full->ncrelocs, full->crelocs, 0, 0, 0, 0};
    const Stencil *s = a < 0x10000 && b < 0x10000 && c < 0x10000 ? &compact : full;
    size_t size = without_final_jump(s);
    size_t start = nat.len;
    put_bytes(s->code, size);

    const uint64_t holes[3] = {a, b, c};
    for (uint32_t i = 0; i < s->nrelocs; i++)
    {
        const StencilReloc *r = &s->relocs[i];
        if (r->offset >= size)
        {
            continue; // the dropped jump
        }
        size_t at = start + r->offset;
        if (r->target <= ST_HOLE_C)
        {
            fill_value(at, r->kind, holes[r->target]);
        }
        else if (r->target == ST_CONTINUE)
        {
            fill_branch(at, r->kind, r->addend, start + size);
        }
        else if (r->target == ST_JUMP)
        {
            push_fixup(&nat.jumps, &nat.njumps, &nat.jumps_cap, (Fixup){at, r->kind, r->addend, label});
        }
        else if (r->target == ST_CALLEE)
        {
            push_fixup(&nat.calls, &nat.ncalls, &nat.calls_cap, (Fixup){at, r->kind, r->addend, callee});
        }
        else if (r->target >= ST_EXTERN)
        {
            push_extern(at, (uint32_t)(r->target - ST_EXTERN));
        }
        else
        {
            push_fixup(&nat.rtcalls, &nat.nrtcalls, &nat.rtcalls_cap,
                       (Fixup){at, r->kind, r->addend, (uint32_t)(r->target - ST_RUNTIME)});
        }
    }
    return start;
}

// ---- MIR -> stencils ------------------------------------------------------------------------------

#define VARIANT_ROW(op)                                                                            \
    {ST_##op##_u8, ST_##op##_u16, ST_##op##_u32, ST_##op##_u64,                                    \
     ST_##op##_i8, ST_##op##_i16, ST_##op##_i32, ST_##op##_i64}

static const int st_bin[MB__COUNT][MV__COUNT] = {
    [MB_ADD] = VARIANT_ROW(add), [MB_SUB] = VARIANT_ROW(sub), [MB_MUL] = VARIANT_ROW(mul),
    [MB_DIV] = VARIANT_ROW(div), [MB_EQ] = VARIANT_ROW(eq),   [MB_NE] = VARIANT_ROW(ne),
    [MB_LT] = VARIANT_ROW(lt),   [MB_LE] = VARIANT_ROW(le),   [MB_GT] = VARIANT_ROW(gt),
    [MB_GE] = VARIANT_ROW(ge),
};
static const int st_bini[MB__COUNT][MV__COUNT] = {
    [MB_ADD] = VARIANT_ROW(addi), [MB_SUB] = VARIANT_ROW(subi), [MB_MUL] = VARIANT_ROW(muli),
    [MB_DIV] = VARIANT_ROW(divi), [MB_EQ] = VARIANT_ROW(eqi),   [MB_NE] = VARIANT_ROW(nei),
    [MB_LT] = VARIANT_ROW(lti),   [MB_LE] = VARIANT_ROW(lei),   [MB_GT] = VARIANT_ROW(gti),
    [MB_GE] = VARIANT_ROW(gei),
};
static const int st_norm[MV__COUNT] = VARIANT_ROW(norm);
static const int st_neg[MV__COUNT] = VARIANT_ROW(neg);
static const int st_write[] = {
    [MW_STR] = ST_write_str,   [MW_INT] = ST_write_int,       [MW_UINT] = ST_write_uint,
    [MW_BOOL] = ST_write_bool, [MW_SPACE] = ST_write_space,   [MW_NL] = ST_write_nl,
};

// A string: its length and bytes are placed in the code, right after the instructions that
// store their address in slot d; the code jumps over them. No absolute address is written.
static void emit_string(uint32_t d, const MirString *str)
{
    size_t data = 8 + str->len;
    size_t padded = (data + 3) & ~(size_t)3;
    if (nat.arch->cpu == CPU_ARM64)
    {
        if (d >= 4096)
        {
            fatal("a frame is too large");
        }
        put32(0x10000000u | (3u << 5) | 9u);               // adr x9, .+12 (immhi:immlo = 3:0)
        put32(0xf9000000u | (d << 10) | (0u << 5) | 9u);   // str x9, [x0, #d*8]
        put32(0x14000000u | (uint32_t)((4 + padded) / 4)); // b over the data
    }
    else
    {
        if (d >= (1u << 28))
        {
            fatal("a frame is too large");
        }
        static const uint8_t lea[] = {0x48, 0x8d, 0x05}; // lea rax, [rip + 12]
        put_bytes(lea, 3);
        put32(12);
        static const uint8_t mov[] = {0x48, 0x89, 0x87}; // mov [rdi + d*8], rax
        put_bytes(mov, 3);
        put32(d * 8);
        put_bytes("\xe9", 1); // jmp over the data
        put32((uint32_t)padded);
    }
    uint64_t len = str->len;
    put_bytes(&len, 8);
    put_bytes(str->bytes, str->len);
    static const uint8_t zero[4] = {0};
    put_bytes(zero, padded - data);
}

// ---- keeping only what is reached ----------------------------------------------------------------

static void begin_range(SymId sym)
{
    if (nat.norder == nat.order_cap)
    {
        nat.order = (struct FnRange *)grow_list(nat.order, sizeof(struct FnRange), &nat.order_cap,
                                                nat.norder + 1);
    }
    nat.order[nat.norder++] = (struct FnRange){sym, nat.len, nat.ncalls};
}

// The function holding code offset `at`: an index in nat.order (they are in code order).
static size_t range_of(size_t at, size_t from)
{
    while (from + 1 < nat.norder && nat.order[from + 1].at <= at)
    {
        from++;
    }
    return from;
}

// Moves the positions in `at` (a list in code order) with their function; drops those of
// functions that were not kept. Returns the new count.
static size_t remap(void *list, size_t n, size_t stride, const uint8_t *keep, const size_t *start)
{
    size_t out = 0, fi = 0;
    for (size_t i = 0; i < n; i++)
    {
        uint8_t *item = (uint8_t *)list + i * stride;
        size_t at;
        memcpy(&at, item, sizeof(at)); // `at` is the first field of every list's items
        fi = range_of(at, fi);
        if (!keep[fi])
        {
            continue;
        }
        at = at - nat.order[fi].at + start[fi];
        memcpy(item, &at, sizeof(at));
        memmove((uint8_t *)list + out * stride, item, stride);
        out++;
    }
    return out;
}

// Dead-code elimination: only the functions the entry reaches, through calls, stay. They are
// moved down over the ones dropped; branches inside a function are relative, so they move with it.
static void shake(void)
{
    size_t nf = nat.norder;
    uint8_t *keep = (uint8_t *)arena_alloc(nf);
    size_t *start = (size_t *)arena_alloc(nf * sizeof(size_t));
    size_t *stack = (size_t *)arena_alloc(nf * sizeof(size_t));
    size_t *index_of = (size_t *)arena_alloc(cx.syms.cap * sizeof(size_t));
    memset(keep, 0, nf);
    for (size_t i = 0; i < nf; i++)
    {
        if (nat.order[i].sym)
        {
            index_of[nat.order[i].sym] = i;
        }
    }
    size_t sp = 0;
    keep[nf - 1] = 1; // the entry
    stack[sp++] = nf - 1;
    while (sp)
    {
        size_t f = stack[--sp];
        size_t end = f + 1 < nf ? nat.order[f + 1].first_call : nat.ncalls;
        for (size_t c = nat.order[f].first_call; c < end; c++)
        {
            SymId callee = nat.calls[c].ref;
            if (nat.fn_at[callee] == NOT_EMITTED)
            {
                continue; // reported when the calls are filled
            }
            size_t g = index_of[callee];
            if (!keep[g])
            {
                keep[g] = 1;
                stack[sp++] = g;
            }
        }
    }

    size_t to = nat.order[0].at;
    for (size_t i = 0; i < nf; i++)
    {
        size_t end = i + 1 < nf ? nat.order[i + 1].at : nat.len;
        start[i] = to;
        if (keep[i])
        {
            memmove(nat.code + to, nat.code + nat.order[i].at, end - nat.order[i].at);
            to += end - nat.order[i].at;
        }
    }
    nat.ncalls = remap(nat.calls, nat.ncalls, sizeof(Fixup), keep, start);
    nat.nrtcalls = remap(nat.rtcalls, nat.nrtcalls, sizeof(Fixup), keep, start);
    nat.nexterns = remap(nat.externs, nat.nexterns, sizeof(MachoImportCall), keep, start);
    for (size_t i = 0; i < nf; i++)
    {
        if (nat.order[i].sym)
        {
            nat.fn_at[nat.order[i].sym] = keep[i] ? start[i] : NOT_EMITTED;
        }
    }
    nat.entry_at = start[nf - 1];
    nat.len = to;
}

// ---- the accumulator ------------------------------------------------------------------------------
//
// A value whose only reader is the very next instruction does not go through the frame: its
// producer leaves it in `acc` (the _r form of its stencil) and the reader takes it from there
// (the _a form). Variant ids follow the sorted names: name, name_a, name_ar, name_r.
enum
{
    V_MEM = 0, // from and to slots
    V_A = 1,   // first operand from acc
    V_AR = 2,  // from acc, to acc
    V_R = 3,   // to acc
};

_Static_assert(ST_add_u8_a == ST_add_u8 + V_A && ST_add_u8_ar == ST_add_u8 + V_AR &&
                   ST_add_u8_r == ST_add_u8 + V_R && ST_norm_i64_r == ST_norm_i64 + V_R &&
                   ST_load_field_r == ST_load_field + V_R && ST_copy_ar == ST_copy + V_AR &&
                   ST_not_a == ST_not + V_A && ST_write_int_a == ST_write_int + 1 &&
                   ST_jz_a == ST_jz + 1 && ST_store_field_a == ST_store_field + 1,
               "the stencil variants are not where the backend expects them");

// Which operand of `in` can come from acc: 1 = `a`, 2 = `b` (a store's value), 0 = none.
static int acc_operand(const MirInst *in)
{
    switch (in->op)
    {
    case MIR_COPY:
        return in->d != in->a; // a copy onto itself is not emitted at all
    case MIR_BIN:
    case MIR_BINI:
    case MIR_NEG:
    case MIR_NORM:
    case MIR_NOT:
    case MIR_LOAD:
    case MIR_JZ:
    case MIR_JNZ:
        return 1;
    case MIR_WRITE:
        return in->sub != MW_SPACE && in->sub != MW_NL;
    case MIR_STORE:
        return 2;
    case MIR_GSTORE:
        return 1;
    default:
        return 0;
    }
}

static int can_produce_acc(const MirInst *in)
{
    switch (in->op)
    {
    case MIR_CONST:
        return nat.arch->cpu == CPU_ARM64 || in->imm <= 0x7fffffffull;
    case MIR_COPY:
        return in->d != in->a;
    case MIR_LOAD:
    case MIR_BIN:
    case MIR_BINI:
    case MIR_NEG:
    case MIR_NORM:
    case MIR_NOT:
    case MIR_GLOAD:
        return 1;
    default:
        return 0;
    }
}

static int reads(const MirInst *in, MirSlot s, uint32_t frame_size)
{
    switch (in->op)
    {
    case MIR_BIN:
    case MIR_STORE:
        return in->a == s || in->b == s;
    case MIR_COPY:
    case MIR_BINI:
    case MIR_NEG:
    case MIR_NORM:
    case MIR_NOT:
    case MIR_LOAD:
    case MIR_JZ:
    case MIR_JNZ:
    case MIR_WRITE:
    case MIR_GSTORE:
        return in->a == s;
    case MIR_CALL:
        return s >= frame_size; // the callee reads its frame
    case MIR_RET:
        return s == 0; // the result
    case MIR_ADDR:
        return in->a == s; // its address escapes
    default:
        return 0;
    }
}

static int writes(const MirInst *in, MirSlot s)
{
    switch (in->op)
    {
    case MIR_CONST:
    case MIR_COPY:
    case MIR_ADDR:
    case MIR_LOAD:
    case MIR_BIN:
    case MIR_BINI:
    case MIR_NEG:
    case MIR_NOT:
    case MIR_NORM:
    case MIR_STR:
    case MIR_GLOAD:
        return in->d == s;
    default:
        return 0;
    }
}

// Does instruction k leave its value in acc for instruction k + 1?
static int to_acc(const MirFunc *f, size_t k)
{
    const MirInst *in = &f->code[k];
    if (k + 1 >= f->n || !can_produce_acc(in) || in->d >= f->frame_size)
    {
        return 0;
    }
    const MirInst *next = &f->code[k + 1];
    int op = acc_operand(next);
    MirSlot via = op == 1 ? next->a : op == 2 ? next->b : (MirSlot)-1;
    if (via != in->d || (op == 1 && next->op == MIR_BIN && next->b == in->d) ||
        (op == 1 && next->op == MIR_STORE) || (op == 2 && next->a == in->d))
    {
        return 0;
    }
    // Nobody else may read the slot before it is written again. A write only ends the search
    // while the code is straight: past a branch or a label, the write may be jumped over, so any
    // later read keeps the value in its slot.
    int straight = next->op != MIR_JZ && next->op != MIR_JNZ;
    for (size_t j = k + 2, n = 0; j < f->n; j++, n++)
    {
        const MirInst *later = &f->code[j];
        if (n == 1024 || reads(later, in->d, f->frame_size))
        {
            return 0;
        }
        if (later->op == MIR_LABEL || later->op == MIR_JZ || later->op == MIR_JNZ)
        {
            straight = 0;
        }
        if (straight && writes(later, in->d))
        {
            return 1;
        }
    }
    return 1;
}

static void emit_mir(const MirFunc *f)
{
    if (f->nlabels > MAX_LABELS)
    {
        fatal("too many branches in one function");
    }
    fn_at_fit();
    nat.fn_at[f->sym] = nat.len;
    begin_range(f->sym);
    nat.njumps = 0;
    for (uint32_t i = 0; i < f->nlabels; i++)
    {
        nat.labels[i] = NOT_EMITTED;
    }

    int in_acc = 0; // the previous instruction left this one's operand in acc
    for (size_t k = 0; k < f->n; k++)
    {
        const MirInst *in = &f->code[k];
        int out = to_acc(f, k);
        int v = in_acc ? (out ? V_AR : V_A) : (out ? V_R : V_MEM);
        switch (in->op)
        {
        case MIR_CONST:
            // x86-64 holes are 32-bit: a larger constant is built from two halves.
            if (out)
            {
                emit(ST_const_r, 0, in->imm, 0, 0, 0);
            }
            else if (nat.arch->cpu == CPU_X86_64 && in->imm > 0x7fffffffull)
            {
                emit(ST_const64, in->d, in->imm & 0xffffffffu, in->imm >> 32, 0, 0);
            }
            else
            {
                emit(ST_const, in->d, in->imm, 0, 0, 0);
            }
            break;
        case MIR_COPY:
            if (in->d != in->a)
            {
                emit(ST_copy + v, in->d, in->a, 0, 0, 0);
            }
            break;
        case MIR_ADDR:
            emit(ST_addr, in->d, in->a, 0, 0, 0);
            break;
        case MIR_ZERO:
            emit(ST_zero, in->a, in->imm, 0, 0, 0);
            break;
        case MIR_LOAD:
            emit(ST_load_field + v, in->d, in->a, in->imm, 0, 0);
            break;
        case MIR_STORE:
            emit(ST_store_field + (in_acc ? 1 : 0), in->a, in->imm, in->b, 0, 0);
            break;
        case MIR_BIN:
            emit(st_bin[in->sub][in->var] + v, in->d, in->a, in->b, 0, 0);
            break;
        case MIR_BINI:
            emit(st_bini[in->sub][in->var] + v, in->d, in->a, in->imm, 0, 0);
            break;
        case MIR_NEG:
            emit(st_neg[in->var] + v, in->d, in->a, 0, 0, 0);
            break;
        case MIR_NOT:
            emit(ST_not + v, in->d, in->a, 0, 0, 0);
            break;
        case MIR_NORM:
            emit(st_norm[in->var] + v, in->d, in->a, 0, 0, 0);
            break;
        case MIR_LABEL:
            nat.labels[in->imm] = nat.len;
            break;
        case MIR_JZ:
        case MIR_JNZ:
            emit((in->op == MIR_JZ ? ST_jz : ST_jnz) + (in_acc ? 1 : 0), in->a, 0, 0, 0,
                 (uint32_t)in->imm);
            break;
        case MIR_CALL:
            emit(ST_call, f->frame_size, 0, 0, (SymId)in->imm, 0);
            break;
        case MIR_RET:
            emit(ST_ret, 0, 0, 0, 0, 0);
            break;
        case MIR_STR:
            emit_string(in->d, &f->strings[in->imm]);
            break;
        case MIR_WRITE:
            emit(st_write[in->sub] + (in_acc && acc_operand(in) ? 1 : 0), in->a, 0, 0, 0, 0);
            break;
        case MIR_GLOAD:
            emit(out ? ST_gload_r : ST_gload, in->d, in->imm, 0, 0, 0);
            break;
        case MIR_GSTORE:
            emit(in_acc ? ST_gstore_a : ST_gstore, in->imm, in->a, 0, 0, 0);
            break;
        default:
            break;
        }
        in_acc = out;
    }
    for (size_t i = 0; i < nat.njumps; i++)
    {
        const Fixup *j = &nat.jumps[i];
        fill_branch(j->at, j->reloc, j->addend, nat.labels[j->ref]);
    }
}

// ---- the backend --------------------------------------------------------------------------------

static void native_begin(const BackendJob *job, const NativeArch *arch)
{
    Unit *u = job->entry;
    nat = (Native){.arch = arch, .u = u};
    if (arch->set_hash != STENCIL_SET_HASH)
    {
        fatal("its stencils are out of date: run `make stencils`");
    }
    nat.fn_at_cap = 0;
    fn_at_fit();

    (void)u; // every file's functions come; what cannot be generated is reported if used
}

static void begin_arm64(const BackendJob *job)
{
    native_begin(job, &native_arm64);
}

static void begin_x86_64(const BackendJob *job)
{
    native_begin(job, &native_x86_64);
}

static void native_function(const BackendJob *job, const MirFunc *f)
{
    (void)job;
    emit_mir(f);
}

static int native_end(const BackendJob *job)
{
    Unit *u = job->entry;
    if (cx.errors)
    {
        return nat.errors;
    }
    SymId entry = scope_find(u->scope, "main", 4);
    if (!entry || SYM(entry)->kind != SYM_METHOD || sema_fn_params(u, entry) ||
        SYM(entry)->next_overload)
    {
        diag_at(u, entry ? SYM(entry)->tok : 0, "error",
                "backend native: the program needs one `main()` without parameters to start from");
        return nat.errors + 1;
    }
    fn_at_fit();
    begin_range(0);
    nat.entry_at = emit(sema_fn_ret(u, entry) == T_VOID ? ST_entry_void : ST_entry,
                        mir_global_count(), 0, 0, entry, 0);
    if (nat.fn_at[entry] == NOT_EMITTED)
    {
        const CodegenFailure *why = codegen_failure(entry);
        if (why)
        {
            diag_tok(why->unit, &why->at, "error", "backend native: %s is not supported yet (in 'main')",
                     why->what);
        }
        else
        {
            diag_at(u, SYM(entry)->tok, "error", "backend native: `main` was not generated");
        }
        return nat.errors + 1;
    }
    shake();

    // The runtime: only the functions the program reaches, after the program's code.
    uint8_t used[RT__COUNT] = {0};
    for (size_t i = 0; i < nat.nrtcalls; i++)
    {
        used[nat.rtcalls[i].ref] = 1;
    }
    for (int changed = 1; changed;) // and what those call
    {
        changed = 0;
        for (int i = 0; i < RT__COUNT; i++)
        {
            for (uint32_t r = 0; used[i] && r < nat.arch->runtime[i].nrelocs; r++)
            {
                uint16_t t = nat.arch->runtime[i].relocs[r].target;
                if (t >= ST_RUNTIME && t < ST_EXTERN && !used[t - ST_RUNTIME])
                {
                    used[t - ST_RUNTIME] = 1;
                    changed = 1;
                }
            }
        }
    }
    while (nat.len & 3)
    {
        put_bytes("", 1);
    }
    size_t at = nat.len;
    for (int i = 0; i < RT__COUNT; i++)
    {
        nat.runtime_at[i] = NOT_EMITTED;
        if (used[i])
        {
            nat.runtime_at[i] = at;
            at += (nat.arch->runtime[i].size + 3) & ~(size_t)3;
        }
    }
    for (int i = 0; i < RT__COUNT; i++)
    {
        if (!used[i])
        {
            continue;
        }
        const Stencil *s = &nat.arch->runtime[i];
        size_t start = nat.len;
        put_bytes(s->code, s->size);
        for (uint32_t r = 0; r < s->nrelocs; r++)
        {
            const StencilReloc *rel = &s->relocs[r];
            size_t pos = start + rel->offset;
            if (rel->target >= ST_EXTERN)
            {
                push_extern(pos, (uint32_t)(rel->target - ST_EXTERN));
            }
            else if (rel->target >= ST_RUNTIME)
            {
                fill_branch(pos, rel->kind, rel->addend, nat.runtime_at[rel->target - ST_RUNTIME]);
            }
            else
            {
                fatal("a runtime function with a hole");
            }
        }
        while (nat.len & 3)
        {
            put_bytes("", 1);
        }
    }
    for (size_t i = 0; i < nat.nrtcalls; i++)
    {
        const Fixup *c = &nat.rtcalls[i];
        fill_branch(c->at, c->reloc, c->addend, nat.runtime_at[c->ref]);
    }

    // Every call, now that each function has its final place.
    for (size_t i = 0; i < nat.ncalls; i++)
    {
        const Fixup *c = &nat.calls[i];
        if (nat.fn_at[c->ref] == NOT_EMITTED)
        {
            const CodegenFailure *why = codegen_failure(c->ref);
            if (why)
            {
                diag_tok(why->unit, &why->at, "error",
                         "backend native: %s is not supported yet (in '%.*s', which the program calls)",
                         why->what, why->name_len, why->name);
                codegen_failure_clear(c->ref);
            }
            else if (!cx.errors)
            {
                diag_at(u, SYM(c->ref)->tok, "error",
                        "backend native: a function that is called was not generated");
            }
            nat.errors++;
            continue;
        }
        fill_branch(c->at, c->reloc, c->addend, nat.fn_at[c->ref]);
    }
    if (nat.errors)
    {
        return nat.errors;
    }

    // An executable, written and signed here; or an object file when asked for one (x.o).
    size_t n = job->path ? strlen(job->path) : 0;
    int object = n > 2 && strcmp(job->path + n - 2, ".o") == 0;
    const char *base = job->path ? strrchr(job->path, '/') : NULL;
    const char *ident = base ? base + 1 : job->path ? job->path : "a.out";
    MachoCpu cpu = nat.arch->cpu == CPU_ARM64 ? MACHO_ARM64 : MACHO_X86_64;
    int failed = object ? macho_write_object(job->out, cpu, nat.code, nat.len, nat.entry_at,
                                             nat.externs, nat.nexterns, nat.arch->externs)
                        : macho_write_executable(job->out, cpu, nat.code, nat.len, nat.entry_at,
                                                 nat.externs, nat.nexterns, nat.arch->externs, ident);
    if (failed)
    {
        diag_at(u, 0, "error", "backend native: cannot write %s", job->path);
        return 1;
    }
    return 0;
}

// One Init for three names (Go's archInits does the same): the name chooses the CPU.
void backend_native_init(Backend *b)
{
    int x86 = strcmp(b->name, "x86_64") == 0;
#if defined(__x86_64__)
    x86 |= strcmp(b->name, "native") == 0;
#endif
    b->summary = x86 ? "x86-64 machine code by copy-and-patch: a signed executable, no toolchain"
                     : "arm64 machine code by copy-and-patch: a signed executable, no toolchain";
    b->extension = "";
    b->toolchain = NULL;
    b->executable = 1;
    b->begin = x86 ? begin_x86_64 : begin_arm64;
    b->function_mir = native_function;
    b->end = native_end;
}
