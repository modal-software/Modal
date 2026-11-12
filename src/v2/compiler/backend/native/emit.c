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
#include "v2/compiler/backend/native/elf.h"
#include "v2/compiler/backend/native/macho.h"
#include "v2/compiler/backend/native/regalloc.h"
#include "v2/compiler/backend/native/run.h"
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
        uint32_t frame; // value-stack slots of its frame (MirFunc.frame_size): callees start there
        uint32_t uses;  // slots it touches itself, inlined bodies in the window included
        uint32_t saved; // bytes of registers it saves on the machine stack (generated with registers)
        uint32_t call_frame; // machine-stack bytes each of its calls takes
        uint64_t out;        // most bytes it writes to the system output itself (OUT_UNKNOWN)
    } *order;
    size_t norder, order_cap;
    MachoImportCall *externs; // calls to the C library
    size_t nexterns, externs_cap;

    Fixup *rtcalls; // calls into the runtime, which is placed (only what is used) at the end
    size_t nrtcalls, rtcalls_cap;

    Fixup *jumps; // branches of the function being emitted
    size_t njumps, jumps_cap;
    size_t labels[MAX_LABELS];

    // `main` is a constant program: after the optimizer its MIR only writes constant text and
    // returns a constant. Then the whole program is that text and that status (see const_entry).
    int const_main;
    uint8_t *const_text;
    size_t const_len;
    uint64_t const_status;

    uint32_t regs_functions; // functions generated with registers (--stats)
    uint32_t folded;         // functions dropped as identical to another (shake)
    SymId main_sym;          // the entry's main (found once)
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

static void push_fixup(RegionId r, Fixup **list, size_t *n, size_t *cap, Fixup f)
{
    if (*n == *cap)
    {
        *list = (Fixup *)region_grow_array(r, *list, sizeof(Fixup), cap, *n + 1);
    }
    (*list)[(*n)++] = f;
}

static void push_extern(size_t at, uint32_t sym)
{
    if (nat.nexterns == nat.externs_cap)
    {
        nat.externs = (MachoImportCall *)region_grow_array(REGION_EXTERNS, nat.externs, sizeof(MachoImportCall),
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
            push_fixup(REGION_JUMPS, &nat.jumps, &nat.njumps, &nat.jumps_cap, (Fixup){at, r->kind, r->addend, label});
        }
        else if (r->target == ST_CALLEE)
        {
            push_fixup(REGION_CALLS, &nat.calls, &nat.ncalls, &nat.calls_cap, (Fixup){at, r->kind, r->addend, callee});
        }
        else if (r->target >= ST_EXTERN)
        {
            push_extern(at, (uint32_t)(r->target - ST_EXTERN));
        }
        else
        {
            push_fixup(REGION_RTCALLS, &nat.rtcalls, &nat.nrtcalls, &nat.rtcalls_cap,
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
static const int st_load[MV__COUNT] = VARIANT_ROW(load);
static const int st_store[MV__COUNT] = {ST_store_8, ST_store_16, ST_store_32, ST_store_64,
                                        ST_store_8, ST_store_16, ST_store_32, ST_store_64};
static const int st_neg[MV__COUNT] = VARIANT_ROW(neg);
static const int st_write[] = {
    [MW_STR] = ST_write_str,   [MW_INT] = ST_write_int,       [MW_UINT] = ST_write_uint,
    [MW_BOOL] = ST_write_bool, [MW_SPACE] = ST_write_space,   [MW_NL] = ST_write_nl,
    [MW_BYTE] = ST_write_byte,
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
        nat.order = (struct FnRange *)region_grow_array(REGION_FNS, nat.order, sizeof(struct FnRange), &nat.order_cap,
                                                nat.norder + 1);
    }
    nat.order[nat.norder++] = (struct FnRange){sym, nat.len, nat.ncalls, 0, 0, 0, 0, 0};
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

// Identical-code folding (gold's and lld's --icf): two kept functions whose machine code is the
// same byte for byte, with the same calls, runtime calls and C calls at the same places, are one
// function. The later one is dropped and every call to it goes to the first. Returns, per
// function, the one it became (SIZE_MAX: itself or dropped).
typedef struct
{
    const Fixup *calls;
    size_t ncalls;
    size_t rt_lo, rt_hi;   // its runtime calls in nat.rtcalls
    size_t ext_lo, ext_hi; // its C calls in nat.externs
} FnFixups;

static int same_function(size_t i, size_t j, const FnFixups *fx)
{
    const struct FnRange *a = &nat.order[i], *b = &nat.order[j];
    size_t na = (i + 1 < nat.norder ? nat.order[i + 1].at : nat.len) - a->at;
    size_t nb = (j + 1 < nat.norder ? nat.order[j + 1].at : nat.len) - b->at;
    if (na != nb || fx[i].ncalls != fx[j].ncalls || fx[i].rt_hi - fx[i].rt_lo != fx[j].rt_hi - fx[j].rt_lo ||
        fx[i].ext_hi - fx[i].ext_lo != fx[j].ext_hi - fx[j].ext_lo || memcmp(nat.code + a->at, nat.code + b->at, na) != 0)
    {
        return 0;
    }
    for (size_t k = 0; k < fx[i].ncalls; k++)
    {
        const Fixup *x = &fx[i].calls[k], *y = &fx[j].calls[k];
        if (x->at - a->at != y->at - b->at || x->ref != y->ref || x->reloc != y->reloc || x->addend != y->addend)
        {
            return 0;
        }
    }
    for (size_t k = 0; k < fx[i].rt_hi - fx[i].rt_lo; k++)
    {
        const Fixup *x = &nat.rtcalls[fx[i].rt_lo + k], *y = &nat.rtcalls[fx[j].rt_lo + k];
        if (x->at - a->at != y->at - b->at || x->ref != y->ref)
        {
            return 0;
        }
    }
    for (size_t k = 0; k < fx[i].ext_hi - fx[i].ext_lo; k++)
    {
        const MachoImportCall *x = &nat.externs[fx[i].ext_lo + k], *y = &nat.externs[fx[j].ext_lo + k];
        if (x->at - a->at != y->at - b->at || x->sym != y->sym)
        {
            return 0;
        }
    }
    return 1;
}

static size_t *fold_identical(uint8_t *keep)
{
    size_t nf = nat.norder;
    size_t *alias = (size_t *)arena_alloc(nf * sizeof(size_t));
    FnFixups *fx = (FnFixups *)arena_alloc(nf * sizeof(FnFixups));
    size_t nbuckets = 1;
    while (nbuckets < nf * 2)
    {
        nbuckets <<= 1;
    }
    size_t *bucket = (size_t *)arena_alloc(nbuckets * sizeof(size_t));
    for (size_t b = 0; b < nbuckets; b++)
    {
        bucket[b] = SIZE_MAX;
    }
    size_t rt = 0, ext = 0;
    for (size_t i = 0; i < nf; i++)
    {
        alias[i] = SIZE_MAX;
        size_t at = nat.order[i].at, end = i + 1 < nf ? nat.order[i + 1].at : nat.len;
        size_t c_end = i + 1 < nf ? nat.order[i + 1].first_call : nat.ncalls;
        fx[i].calls = nat.calls ? nat.calls + nat.order[i].first_call : NULL;
        fx[i].ncalls = c_end - nat.order[i].first_call;
        while (rt < nat.nrtcalls && nat.rtcalls[rt].at < at)
        {
            rt++;
        }
        fx[i].rt_lo = rt;
        while (rt < nat.nrtcalls && nat.rtcalls[rt].at < end)
        {
            rt++;
        }
        fx[i].rt_hi = rt;
        while (ext < nat.nexterns && nat.externs[ext].at < at)
        {
            ext++;
        }
        fx[i].ext_lo = ext;
        while (ext < nat.nexterns && nat.externs[ext].at < end)
        {
            ext++;
        }
        fx[i].ext_hi = ext;
        if (!keep[i] || i + 1 == nf || !nat.order[i].sym)
        {
            continue; // the entry stays itself
        }
        uint64_t h = 1469598103934665603ull ^ (end - at);
        for (size_t k = at; k < end; k++)
        {
            h = (h ^ nat.code[k]) * 1099511628211ull;
        }
        for (size_t k = 0; k < fx[i].ncalls; k++)
        {
            h = (h ^ fx[i].calls[k].ref) * 1099511628211ull;
        }
        size_t b = (size_t)(h ^ (h >> 29)) & (nbuckets - 1);
        for (; bucket[b] != SIZE_MAX; b = (b + 1) & (nbuckets - 1))
        {
            if (same_function(bucket[b], i, fx))
            {
                break;
            }
        }
        if (bucket[b] == SIZE_MAX)
        {
            bucket[b] = i;
            continue;
        }
        alias[i] = bucket[b];
        keep[i] = 0;
        nat.folded++;
    }
    return alias;
}

// Dead-code elimination: only the functions the entry reaches, through calls, stay. They are
// moved down over the ones dropped; branches inside a function are relative, so they move with it.
static void shake(void)
{
    ArenaMark mark = arena_mark(); // its tables are its own
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

    size_t *alias = fold_identical(keep);

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
            nat.fn_at[nat.order[i].sym] = keep[i] ? start[i] : alias[i] != SIZE_MAX ? start[alias[i]] : NOT_EMITTED;
        }
    }
    nat.entry_at = start[nf - 1];
    nat.len = to;
    arena_release(mark);
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

_Static_assert(ST_trap_a == ST_trap + 1 && ST_write_byte_a == ST_write_byte + 1 &&
                   ST_write_str_a == ST_write_str + 1 && ST_write_uint_a == ST_write_uint + 1 &&
                   ST_write_bool_a == ST_write_bool + 1,
               "the _a forms do not follow their stencils");
_Static_assert(ST_add_u8_a == ST_add_u8 + V_A && ST_add_u8_ar == ST_add_u8 + V_AR &&
                   ST_add_u8_r == ST_add_u8 + V_R && ST_norm_i64_r == ST_norm_i64 + V_R &&
                   ST_load_u8_r == ST_load_u8 + V_R && ST_copy_ar == ST_copy + V_AR &&
                   ST_not_a == ST_not + V_A && ST_write_int_a == ST_write_int + 1 &&
                   ST_jz_a == ST_jz + 1 && ST_store_8_a == ST_store_8 + 1 &&
                   ST_store_64_a == ST_store_64 + 1,
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
    case MIR_TRAP:
        return 1;
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

// The slots of the function being emitted that hold its result, and those whose address is
// taken somewhere (struct values read and written through a pointer): the accumulator never
// stands in for one of those.
static uint32_t ret_slots, addressed_lo, addressed_hi;

// Loops: a branch back to a label before it. Within one, a value is read again on the next
// pass, before the place it was written, so the accumulator may stand in for it only if no
// other instruction of the loop reads it. (MIR from output sinks has loops; source has none.)
#define MAX_LOOPS 256
static struct
{
    uint32_t lo, hi; // MIR indices: the label and the branch back to it
} loops[MAX_LOOPS];
static uint32_t nloops;
static int loops_overflow; // more loops than recorded: no accumulator at all in this function
static uint32_t label_index[MAX_LABELS];

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
    case MIR_TRAP:
    case MIR_GSTORE:
        return in->a == s;
    case MIR_CALL:
        return s >= frame_size; // the callee reads its frame
    case MIR_RET:
        return s < ret_slots; // the result
    case MIR_ADDR:
        return s >= in->a && s < in->a + in->imm; // its address escapes
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
    if (k + 1 >= f->n || !can_produce_acc(in) || in->d >= f->frame_size ||
        (in->d >= addressed_lo && in->d < addressed_hi))
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
    if (loops_overflow)
    {
        return 0;
    }
    for (uint32_t l = 0; l < nloops; l++)
    {
        if (k < loops[l].lo || k > loops[l].hi)
        {
            continue;
        }
        for (uint32_t j = loops[l].lo; j <= loops[l].hi; j++)
        {
            if (j != k + 1 && reads(&f->code[j], in->d, f->frame_size))
            {
                return 0;
            }
        }
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

static uint32_t frame_bytes(const Stencil *s); // memory budgets, below

// ---- registers --------------------------------------------------------------------------------
//
// A function whose slots got registers (regalloc.c) is generated instruction by instruction here:
// each operation on registers or frame slots is encoded directly (one to four machine
// instructions), and the stencils stay for what calls out: calls, output, traps, and division on
// x86-64 (its idiv needs rax and rdx and faults on INT64_MIN / -1). A slot that got no register is
// read and written in the frame at a short offset (`ldr x9, [x0, #s*8]`, `mov rax, [rdi + s*8]`).
// The registers are those the C calling convention preserves, so every stencil and runtime call
// keeps them; the function saves the ones it uses on entry and restores them at each return.
// Scratch registers: arm64 x9-x11 (stencils pass only x0-x2 between them); x86-64 rax, rcx.
// A value stays in its canonical form (the slot rule: zero- or sign-extended to 64 bits), so
// arithmetic is done in 64 bits and the result is extended from its width again.

static const uint8_t a64_regs[] = {19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
static const uint8_t x64_regs[] = {3, 14, 15, 5, 12, 13}; // rbx r14 r15 rbp r12 r13
// Registers a call out does not keep, for values no call out crosses: nothing to save.
static const uint8_t a64_scratch[] = {3, 4, 5, 6, 7, 8, 12, 13, 14, 15};
static const uint8_t x64_scratch[] = {8, 9, 10, 11}; // r8-r11

static RegAlloc ra;
static int ra_saved; // registers the prologue saves (an even count: the stack stays aligned)
// A leaf function that calls the runtime itself (output, traps) keeps f and c in the first two
// preserved registers (arm64 x19 x20, x86-64 rbx r14), so a runtime call is a plain call with
// nothing to save around it; its values use the preserved registers after those (ra_base = 2).
// ra_lr: arm64 keeps the link register on the stack for the whole function (it calls out).
static int ra_base, ra_lr;


// The frame without the slots that live only in a register: ra_shift[s] is how many of those
// are below slot s, which moves down by as much (blocks of slots never contain one, so they stay
// whole; the call window moves down with the rest). NULL: the frame is as the MIR says.
static uint32_t *ra_shift;
static uint32_t ra_removed;

static uint32_t H(MirSlot s)
{
    MirSlot slot = s & RA_VIRTUAL ? ra_slot(&ra, s) : s; // a value split from a slot: the slot's place
    if (!ra_shift)
    {
        return slot;
    }
    return slot - (slot < ra.norig ? ra_shift[slot] : ra_removed);
}

static int regs_count(void)
{
    return nat.arch->cpu == CPU_ARM64 ? (int)sizeof(a64_regs) : (int)sizeof(x64_regs);
}

static int scratch_count(void)
{
    return nat.arch->cpu == CPU_ARM64 ? (int)sizeof(a64_scratch) : (int)sizeof(x64_scratch);
}

// The machine register of slot s, or -1 when it is in the frame.
static int R(MirSlot s)
{
    uint32_t n = ra_index(&ra, s);
    if (n >= ra.nnames || ra.reg[n] == RA_NONE)
    {
        return -1;
    }
    int r = ra.reg[n], keep = regs_count() - ra_base;
    if (nat.arch->cpu == CPU_ARM64)
    {
        return r < keep ? a64_regs[ra_base + r] : a64_scratch[r - keep];
    }
    return r < keep ? x64_regs[ra_base + r] : x64_scratch[r - keep];
}

// Does instruction `in` run a stencil or call out, which may change the scratch registers?
static int clobbers(const MirInst *in)
{
    int x86 = nat.arch->cpu == CPU_X86_64;
    switch (in->op)
    {
    case MIR_CALL:
    case MIR_WRITE:
        return 1;
    case MIR_ZERO:
        return in->imm > 16;
    case MIR_BIN:
    case MIR_BINI:
        return x86 && in->sub == MB_DIV;
    case MIR_LOAD:
    case MIR_STORE:
        return x86 && in->imm > 0x7fffffffull;
    case MIR_GLOAD:
    case MIR_GSTORE:
        return x86 && in->imm > 0x0fffffffull;
    default:
        return 0; // a trap does not come back
    }
}

static void push_jump(size_t at, uint8_t reloc, int8_t addend, uint32_t label)
{
    push_fixup(REGION_JUMPS, &nat.jumps, &nat.njumps, &nat.jumps_cap, (Fixup){at, reloc, addend, label});
}

// ---- arm64 ----

static void a64_mov(int d, int n)
{
    if (d != n)
    {
        put32(0xaa0003e0u | (uint32_t)n << 16 | (uint32_t)d); // orr xd, xzr, xn
    }
}

static void a64_imm(int d, uint64_t v)
{
    if (v == 0)
    {
        put32(0xd2800000u | (uint32_t)d); // movz xd, #0
        return;
    }
    int zeros = 0, ones = 0;
    for (int hw = 0; hw < 4; hw++)
    {
        uint32_t part = (uint32_t)(v >> (16 * hw)) & 0xffffu;
        zeros += part == 0;
        ones += part == 0xffffu;
    }
    int inverted = ones > zeros; // movn, then movk for the pieces that are not all ones
    int first = 1;
    for (int hw = 0; hw < 4; hw++)
    {
        uint32_t part = (uint32_t)(v >> (16 * hw)) & 0xffffu;
        if (part == (inverted ? 0xffffu : 0u) && !(hw == 3 && first))
        {
            continue;
        }
        if (first)
        {
            uint32_t imm = inverted ? (~part & 0xffffu) : part;
            put32((inverted ? 0x92800000u : 0xd2800000u) | (uint32_t)hw << 21 | imm << 5 | (uint32_t)d);
            first = 0;
        }
        else
        {
            put32(0xf2800000u | (uint32_t)hw << 21 | part << 5 | (uint32_t)d); // movk
        }
    }
}

static void a64_ldr_slot(int t, MirSlot s)
{
    put32(0xf9400000u | H(s) << 10 | (uint32_t)t); // ldr xt, [x0, #s*8]
}

static void a64_str_slot(int t, MirSlot s)
{
    put32(0xf9000000u | H(s) << 10 | (uint32_t)t); // str xt, [x0, #s*8]
}

// The register holding slot s: its own, or `scratch` loaded from the frame.
static int a64_src(MirSlot s, int scratch)
{
    int r = R(s);
    if (r >= 0)
    {
        return r;
    }
    a64_ldr_slot(scratch, s);
    return scratch;
}

static int a64_dst(MirSlot s, int scratch)
{
    int r = R(s);
    return r >= 0 ? r : scratch;
}

static void a64_fin(MirSlot s, int r)
{
    if (R(s) < 0)
    {
        a64_str_slot(r, s);
    }
}

// xd = xn extended from the width of `var` (ubfm / sbfm xd, xn, #0, #bits - 1).
static void a64_norm(int d, int n, int var)
{
    static const uint8_t top[MV__COUNT] = {7, 15, 31, 63, 7, 15, 31, 63};
    if (top[var] == 63)
    {
        a64_mov(d, n);
        return;
    }
    put32((var >= MV_I8 ? 0x93400000u : 0xd3400000u) | (uint32_t)top[var] << 10 | (uint32_t)n << 5 | (uint32_t)d);
}

// cset xd for comparison `sub` in `var` (signed or unsigned conditions).
static uint32_t a64_cond(int sub, int var);

static void a64_cset(int d, int sub, int var)
{
    put32(0x9a9f07e0u | (a64_cond(sub, var) ^ 1u) << 12 | (uint32_t)d); // csinc xd, xzr, xzr, !cond
}

static int is_compare(int sub)
{
    return sub >= MB_EQ;
}

static uint32_t a64_cond(int sub, int var)
{
    static const uint8_t sgn[MB__COUNT] = {[MB_EQ] = 0, [MB_NE] = 1, [MB_LT] = 11, [MB_LE] = 13, [MB_GT] = 12, [MB_GE] = 10};
    static const uint8_t uns[MB__COUNT] = {[MB_EQ] = 0, [MB_NE] = 1, [MB_LT] = 3, [MB_LE] = 9, [MB_GT] = 8, [MB_GE] = 2};
    return var >= MV_I8 ? sgn[sub] : uns[sub];
}

// The flags of comparison `in` (cmp with a register, or an immediate when it fits).
static void a64_compare(const MirInst *in)
{
    int lhs = a64_src(in->a, 9);
    if (in->op == MIR_BINI)
    {
        uint64_t v = mir_normalize(in->imm, in->var);
        int64_t sv = (int64_t)v;
        if (v < 4096 || (sv < 0 && sv > -4096))
        {
            uint32_t k = (uint32_t)(sv >= 0 ? sv : -sv);
            put32((sv >= 0 ? 0xf100001fu : 0xb100001fu) | k << 10 | (uint32_t)lhs << 5); // cmp / cmn
            return;
        }
        a64_imm(10, v);
        put32(0xeb00001fu | 10u << 16 | (uint32_t)lhs << 5);
        return;
    }
    int rhs = a64_src(in->b, 10);
    put32(0xeb00001fu | (uint32_t)rhs << 16 | (uint32_t)lhs << 5); // cmp xn, xm
}

// A comparison and the branch that is its only reader: cmp and b.cond (cbz / cbnz against 0).
static void a64_compare_branch(const MirInst *cmp, const MirInst *br)
{
    int when_true = br->op == MIR_JNZ;
    if (cmp->op == MIR_BINI && mir_normalize(cmp->imm, cmp->var) == 0 && (cmp->sub == MB_EQ || cmp->sub == MB_NE))
    {
        int r = a64_src(cmp->a, 9);
        int nonzero = (cmp->sub == MB_NE) == when_true;
        push_jump(nat.len, SR_BRANCH19, 0, (uint32_t)br->imm);
        put32((nonzero ? 0xb5000000u : 0xb4000000u) | (uint32_t)r); // cbnz / cbz
        return;
    }
    a64_compare(cmp);
    uint32_t cond = a64_cond(cmp->sub, cmp->var) ^ (when_true ? 0u : 1u);
    push_jump(nat.len, SR_BRANCH19, 0, (uint32_t)br->imm);
    put32(0x54000000u | cond); // b.cond
}

static void a64_compare(const MirInst *in);

static void a64_bin(const MirInst *in)
{
    if (is_compare(in->sub))
    {
        a64_compare(in);
        int rd = a64_dst(in->d, 9);
        a64_cset(rd, in->sub, in->var);
        a64_fin(in->d, rd);
        return;
    }
    int ra_ = a64_src(in->a, 9);
    int rd = a64_dst(in->d, 9);
    int rb;
    if (in->op == MIR_BINI)
    {
        uint64_t v = mir_normalize(in->imm, in->var);
        int64_t sv = (int64_t)v;
        if ((in->sub == MB_ADD || in->sub == MB_SUB) && (v < 4096 || (sv < 0 && sv > -4096)))
        {
            int add = (in->sub == MB_ADD) == (sv >= 0);
            uint32_t k = (uint32_t)(sv >= 0 ? sv : -sv);
            put32((add ? 0x91000000u : 0xd1000000u) | k << 10 | (uint32_t)ra_ << 5 | (uint32_t)rd);
            a64_norm(rd, rd, in->var);
            a64_fin(in->d, rd);
            return;
        }
        if (is_compare(in->sub) && (v < 4096 || (sv < 0 && sv > -4096)))
        {
            uint32_t k = (uint32_t)(sv >= 0 ? sv : -sv);
            put32((sv >= 0 ? 0xf100001fu : 0xb100001fu) | k << 10 | (uint32_t)ra_ << 5); // cmp / cmn
            a64_cset(rd, in->sub, in->var);
            a64_fin(in->d, rd);
            return;
        }
        a64_imm(10, v);
        rb = 10;
    }
    else
    {
        rb = a64_src(in->b, 10);
    }
    uint32_t mnr = (uint32_t)rb << 16 | (uint32_t)ra_ << 5 | (uint32_t)rd;
    switch (in->sub)
    {
    case MB_ADD:
        put32(0x8b000000u | mnr);
        break;
    case MB_SUB:
        put32(0xcb000000u | mnr);
        break;
    case MB_MUL:
        put32(0x9b007c00u | mnr); // madd xd, xn, xm, xzr
        break;
    case MB_DIV:
        // The divisor is not zero (MIR_TRAP came first). INT_MIN / -1 wraps in 64 bits (arm64
        // does not trap), and the narrower types are extended back, as the stencils do.
        put32((in->var >= MV_I8 ? 0x9ac00c00u : 0x9ac00800u) | mnr);
        break;
    default:
        put32(0xeb00001fu | (uint32_t)rb << 16 | (uint32_t)ra_ << 5); // cmp xn, xm
        a64_cset(rd, in->sub, in->var);
        a64_fin(in->d, rd);
        return;
    }
    a64_norm(rd, rd, in->var);
    a64_fin(in->d, rd);
}

// The load / store instruction of a width (unsigned offset form), and its size.
static uint32_t a64_mem_op(int var, int store, uint32_t *size)
{
    static const uint32_t ld[MV__COUNT] = {0x39400000u, 0x79400000u, 0xb9400000u, 0xf9400000u,
                                           0x39800000u, 0x79800000u, 0xb9800000u, 0xf9400000u};
    static const uint32_t st[4] = {0x39000000u, 0x79000000u, 0xb9000000u, 0xf9000000u};
    *size = 1u << (var & 3);
    return store ? st[var & 3] : ld[var];
}

static void a64_mem(const MirInst *in, int store)
{
    int base = a64_src(in->a, 9);
    int t = store ? a64_src(in->b, 10) : a64_dst(in->d, 10);
    uint32_t size;
    uint32_t op = a64_mem_op(in->var, store, &size);
    if (in->imm % size == 0 && in->imm / size < 4096)
    {
        put32(op | (uint32_t)(in->imm / size) << 10 | (uint32_t)base << 5 | (uint32_t)t);
    }
    else
    {
        a64_imm(11, in->imm);
        put32(0x8b000000u | 11u << 16 | (uint32_t)base << 5 | 11u); // add x11, base, x11
        put32(op | 11u << 5 | (uint32_t)t);
    }
    if (!store)
    {
        a64_fin(in->d, t);
    }
}

static void a64_global(const MirInst *in, int store)
{
    put32(0xf9400000u | 2u << 10 | 1u << 5 | 9u); // ldr x9, [x1, #16]: c->globals
    int t = store ? a64_src(in->a, 10) : a64_dst(in->d, 10);
    if (in->imm < 4096)
    {
        put32((store ? 0xf9000000u : 0xf9400000u) | (uint32_t)in->imm << 10 | 9u << 5 | (uint32_t)t);
    }
    else
    {
        a64_imm(11, in->imm);
        put32((store ? 0xf8207800u : 0xf8607800u) | 11u << 16 | 9u << 5 | (uint32_t)t); // [x9, x11, lsl 3]
    }
    if (!store)
    {
        a64_fin(in->d, t);
    }
}

// ---- x86-64 ----

static void xb(uint8_t b)
{
    put_bytes(&b, 1);
}

// An instruction with a register operand `reg` and an r/m operand: register `rm`, or [rm + disp]
// when `mem`. `byte`: an 8-bit register operand (spl..dil need a REX prefix).
static void x_op(int w, int p66, const char *op, int nop, int reg, int rm, int mem, int32_t disp, int byte)
{
    if (p66)
    {
        xb(0x66);
    }
    uint8_t rex = (uint8_t)(0x40 | (w ? 8 : 0) | (reg & 8 ? 4 : 0) | (rm & 8 ? 1 : 0));
    if (rex != 0x40 || (byte && reg >= 4 && reg < 8))
    {
        xb(rex);
    }
    for (int i = 0; i < nop; i++)
    {
        xb((uint8_t)op[i]);
    }
    int r = reg & 7, low = rm & 7;
    if (!mem)
    {
        xb((uint8_t)(0xc0 | r << 3 | low));
        return;
    }
    if (disp == 0 && low != 5)
    {
        xb((uint8_t)(r << 3 | low));
        if (low == 4)
        {
            xb(0x24);
        }
    }
    else if (disp >= -128 && disp <= 127)
    {
        xb((uint8_t)(0x40 | r << 3 | low));
        if (low == 4)
        {
            xb(0x24);
        }
        xb((uint8_t)disp);
    }
    else
    {
        xb((uint8_t)(0x80 | r << 3 | low));
        if (low == 4)
        {
            xb(0x24);
        }
        put32((uint32_t)disp);
    }
}

enum
{
    XAX = 0,
    XCX = 1,
    XDX = 2,
    XSI = 6,
    XDI = 7,
};

// op reg, slot (the slot's register or its place in the frame, [rdi + s*8]).
static void x_slot(int w, const char *op, int nop, int reg, MirSlot s)
{
    int r = R(s);
    if (r >= 0)
    {
        x_op(w, 0, op, nop, reg, r, 0, 0, 0);
    }
    else
    {
        x_op(w, 0, op, nop, reg, XDI, 1, (int32_t)(H(s) * 8), 0);
    }
}

static void x_load(int reg, MirSlot s)
{
    if (R(s) != reg)
    {
        x_slot(1, "\x8b", 1, reg, s); // mov reg, slot
    }
}

static void x_store(int reg, MirSlot s)
{
    int r = R(s);
    if (r == reg)
    {
        return;
    }
    if (r >= 0)
    {
        x_op(1, 0, "\x89", 1, reg, r, 0, 0, 0); // mov r, reg
    }
    else
    {
        x_op(1, 0, "\x89", 1, reg, XDI, 1, (int32_t)(H(s) * 8), 0); // mov [rdi + s*8], reg
    }
}

// The register holding slot s: its own, or `scratch` loaded from the frame.
static int x_src(MirSlot s, int scratch)
{
    int r = R(s);
    if (r >= 0)
    {
        return r;
    }
    x_load(scratch, s);
    return scratch;
}

static void x_norm(int var)
{
    switch (var)
    {
    case MV_U8:
        put_bytes("\x0f\xb6\xc0", 3); // movzx eax, al
        break;
    case MV_U16:
        put_bytes("\x0f\xb7\xc0", 3); // movzx eax, ax
        break;
    case MV_U32:
        put_bytes("\x89\xc0", 2); // mov eax, eax
        break;
    case MV_I8:
        put_bytes("\x48\x0f\xbe\xc0", 4); // movsx rax, al
        break;
    case MV_I16:
        put_bytes("\x48\x0f\xbf\xc0", 4); // movsx rax, ax
        break;
    case MV_I32:
        put_bytes("\x48\x63\xc0", 3); // movsxd rax, eax
        break;
    default:
        break;
    }
}

static void x_imm(int reg, uint64_t v)
{
    if (v == 0)
    {
        x_op(0, 0, "\x31", 1, reg, reg, 0, 0, 0); // xor r32, r32
    }
    else if (v <= 0xffffffffull)
    {
        if (reg & 8)
        {
            xb(0x41);
        }
        xb((uint8_t)(0xb8 | (reg & 7))); // mov r32, imm32
        put32((uint32_t)v);
    }
    else if ((int64_t)v >= INT32_MIN && (int64_t)v <= INT32_MAX)
    {
        x_op(1, 0, "\xc7", 1, 0, reg, 0, 0, 0); // mov r64, simm32
        put32((uint32_t)v);
    }
    else
    {
        xb((uint8_t)(0x48 | (reg & 8 ? 1 : 0)));
        xb((uint8_t)(0xb8 | (reg & 7))); // movabs
        put_bytes(&v, 8);
    }
}

static uint8_t x_cond(int sub, int var)
{
    static const uint8_t sgn[MB__COUNT] = {[MB_EQ] = 4, [MB_NE] = 5, [MB_LT] = 0xc, [MB_LE] = 0xe, [MB_GT] = 0xf, [MB_GE] = 0xd};
    static const uint8_t uns[MB__COUNT] = {[MB_EQ] = 4, [MB_NE] = 5, [MB_LT] = 2, [MB_LE] = 6, [MB_GT] = 7, [MB_GE] = 3};
    return var >= MV_I8 ? sgn[sub] : uns[sub];
}

static void x_setcc(int sub, int var)
{
    xb(0x0f);
    xb((uint8_t)(0x90 | x_cond(sub, var))); // setcc al
    xb(0xc0);
    put_bytes("\x0f\xb6\xc0", 3); // movzx eax, al
}

static void x_load(int reg, MirSlot s);
static void x_imm(int reg, uint64_t v);
static void x_slot(int w, const char *op, int nop, int reg, MirSlot s);

// The flags of comparison `in`: cmp lhs, rhs (an immediate when it fits 32 bits).
static void x_compare(const MirInst *in)
{
    int lhs = R(in->a);
    if (lhs < 0)
    {
        x_load(XAX, in->a);
        lhs = XAX;
    }
    if (in->op == MIR_BINI)
    {
        int64_t sv = (int64_t)mir_normalize(in->imm, in->var);
        if (sv >= INT32_MIN && sv <= INT32_MAX)
        {
            int small = sv >= -128 && sv <= 127;
            x_op(1, 0, small ? "\x83" : "\x81", 1, 7, lhs, 0, 0, 0); // cmp lhs, imm
            if (small)
            {
                xb((uint8_t)sv);
            }
            else
            {
                put32((uint32_t)sv);
            }
            return;
        }
        x_imm(XCX, (uint64_t)sv);
        x_op(1, 0, "\x3b", 1, lhs, XCX, 0, 0, 0);
        return;
    }
    x_slot(1, "\x3b", 1, lhs, in->b); // cmp lhs, b
}

static void x_compare_branch(const MirInst *cmp, const MirInst *br)
{
    int when_true = br->op == MIR_JNZ;
    uint8_t cc;
    if (cmp->op == MIR_BINI && mir_normalize(cmp->imm, cmp->var) == 0 && (cmp->sub == MB_EQ || cmp->sub == MB_NE) &&
        R(cmp->a) >= 0)
    {
        x_op(1, 0, "\x85", 1, R(cmp->a), R(cmp->a), 0, 0, 0); // test r, r
        cc = cmp->sub == MB_NE ? 5 : 4;
    }
    else
    {
        x_compare(cmp);
        cc = x_cond(cmp->sub, cmp->var);
    }
    cc ^= when_true ? 0 : 1;
    xb(0x0f);
    xb((uint8_t)(0x80 | cc)); // jcc rel32
    push_jump(nat.len, SR_REL32, -4, (uint32_t)br->imm);
    put32(0);
}

// Spills the register operands an instruction reads to their frame places (before a stencil),
// and reloads the register of the slot it wrote (after it).
static void spill_reads(const MirInst *in)
{
    MirSlot r[2] = {in->a, in->b};
    int n = in->op == MIR_BIN || in->op == MIR_STORE ? 2 : 1;
    for (int k = 0; k < n; k++)
    {
        int reg = R(r[k]);
        if (reg >= 0)
        {
            if (nat.arch->cpu == CPU_ARM64)
            {
                a64_str_slot(reg, r[k]);
            }
            else
            {
                x_op(1, 0, "\x89", 1, reg, XDI, 1, (int32_t)(H(r[k]) * 8), 0);
            }
        }
    }
}

static void reload_write(MirSlot d)
{
    int reg = R(d);
    if (reg >= 0)
    {
        if (nat.arch->cpu == CPU_ARM64)
        {
            a64_ldr_slot(reg, d);
        }
        else
        {
            x_op(1, 0, "\x8b", 1, reg, XDI, 1, (int32_t)(H(d) * 8), 0);
        }
    }
}

static void x_bin(const MirInst *in)
{
    if (in->sub == MB_DIV)
    {
        spill_reads(in);
        if (in->op == MIR_BIN)
        {
            emit(st_bin[MB_DIV][in->var], H(in->d), H(in->a), H(in->b), 0, 0);
        }
        else
        {
            emit(st_bini[MB_DIV][in->var], H(in->d), H(in->a), in->imm, 0, 0);
        }
        reload_write(in->d);
        return;
    }
    if (is_compare(in->sub))
    {
        x_compare(in);
        x_setcc(in->sub, in->var);
        x_store(XAX, in->d);
        return;
    }
    x_load(XAX, in->a);
    if (in->op == MIR_BINI)
    {
        uint64_t v = mir_normalize(in->imm, in->var);
        int64_t sv = (int64_t)v;
        static const uint8_t ext[MB__COUNT] = {[MB_ADD] = 0, [MB_SUB] = 5, [MB_EQ] = 7, [MB_NE] = 7, [MB_LT] = 7,
                                               [MB_LE] = 7, [MB_GT] = 7, [MB_GE] = 7};
        if (sv >= INT32_MIN && sv <= INT32_MAX)
        {
            int small = sv >= -128 && sv <= 127;
            if (in->sub == MB_MUL)
            {
                x_op(1, 0, small ? "\x6b" : "\x69", 1, XAX, XAX, 0, 0, 0); // imul rax, rax, imm
            }
            else
            {
                x_op(1, 0, small ? "\x83" : "\x81", 1, ext[in->sub], XAX, 0, 0, 0); // add/sub/cmp rax, imm
            }
            if (small)
            {
                xb((uint8_t)sv);
            }
            else
            {
                put32((uint32_t)sv);
            }
        }
        else
        {
            x_imm(XCX, v);
            static const char *const ops[MB__COUNT] = {[MB_ADD] = "\x03", [MB_SUB] = "\x2b", [MB_MUL] = "\x0f\xaf",
                                                       [MB_EQ] = "\x3b", [MB_NE] = "\x3b", [MB_LT] = "\x3b",
                                                       [MB_LE] = "\x3b", [MB_GT] = "\x3b", [MB_GE] = "\x3b"};
            x_op(1, 0, ops[in->sub], in->sub == MB_MUL ? 2 : 1, XAX, XCX, 0, 0, 0);
        }
    }
    else
    {
        switch (in->sub)
        {
        case MB_ADD:
            x_slot(1, "\x03", 1, XAX, in->b);
            break;
        case MB_SUB:
            x_slot(1, "\x2b", 1, XAX, in->b);
            break;
        case MB_MUL:
            x_slot(1, "\x0f\xaf", 2, XAX, in->b);
            break;
        default:
            x_slot(1, "\x3b", 1, XAX, in->b); // cmp rax, b
            break;
        }
    }
    if (is_compare(in->sub))
    {
        x_setcc(in->sub, in->var);
    }
    else
    {
        x_norm(in->var);
    }
    x_store(XAX, in->d);
}

static void x_mem(const MirInst *in, int store)
{
    int base = x_src(in->a, XCX);
    int32_t disp = (int32_t)in->imm;
    if (!store)
    {
        int t = R(in->d) >= 0 ? R(in->d) : XAX;
        switch (in->var)
        {
        case MV_U8:
            x_op(0, 0, "\x0f\xb6", 2, t, base, 1, disp, 0);
            break;
        case MV_U16:
            x_op(0, 0, "\x0f\xb7", 2, t, base, 1, disp, 0);
            break;
        case MV_U32:
            x_op(0, 0, "\x8b", 1, t, base, 1, disp, 0);
            break;
        case MV_I8:
            x_op(1, 0, "\x0f\xbe", 2, t, base, 1, disp, 0);
            break;
        case MV_I16:
            x_op(1, 0, "\x0f\xbf", 2, t, base, 1, disp, 0);
            break;
        case MV_I32:
            x_op(1, 0, "\x63", 1, t, base, 1, disp, 0);
            break;
        default:
            x_op(1, 0, "\x8b", 1, t, base, 1, disp, 0);
            break;
        }
        x_store(t, in->d);
        return;
    }
    int v = x_src(in->b, XAX);
    switch (in->var & 3)
    {
    case 0:
        x_op(0, 0, "\x88", 1, v, base, 1, disp, 1);
        break;
    case 1:
        x_op(0, 1, "\x89", 1, v, base, 1, disp, 0);
        break;
    case 2:
        x_op(0, 0, "\x89", 1, v, base, 1, disp, 0);
        break;
    default:
        x_op(1, 0, "\x89", 1, v, base, 1, disp, 0);
        break;
    }
}

// ---- both ----

// arm64: the registers the prologue stores, in pairs: the preserved ones in use, then the link
// register when the function calls out (with a spare preserved register to make the pair).
static uint8_t a64_save[12];
static int a64_nsave;

static void a64_plan_save(void)
{
    int n = ra_base + ra.used;
    a64_nsave = 0;
    for (int i = 0; i < n; i++)
    {
        a64_save[a64_nsave++] = a64_regs[i];
    }
    if (ra_lr)
    {
        a64_save[a64_nsave++] = 30;
    }
    if (a64_nsave & 1)
    {
        a64_save[a64_nsave++] = n < (int)sizeof(a64_regs) ? a64_regs[n] : 29;
    }
}

static void regs_prologue(void)
{
    if (nat.arch->cpu == CPU_ARM64)
    {
        for (int i = 0; i < a64_nsave; i += 2)
        {
            put32(0xa9bf03e0u | (uint32_t)a64_save[i + 1] << 10 | a64_save[i]); // stp, [sp, #-16]!
        }
    }
    for (int i = 0; i < ra_saved && nat.arch->cpu != CPU_ARM64; i++)
    {
        if (x64_regs[i] & 8)
        {
            xb(0x41);
        }
        xb((uint8_t)(0x50 | (x64_regs[i] & 7))); // push
    }
    if (ra_base)
    {
        if (nat.arch->cpu == CPU_ARM64)
        {
            a64_mov(19, 0); // f
            a64_mov(20, 1); // c
        }
        else
        {
            put_bytes("\x48\x89\xfb", 3); // mov rbx, rdi
            put_bytes("\x49\x89\xf6", 3); // mov r14, rsi
        }
    }
    for (MirSlot s = 0; s < ra.norig; s++)
    {
        if (ra.live_in[s] && R(s) >= 0)
        {
            reload_write(s);
        }
    }
}

// A runtime function called with c and a value (output, a trap): arm64 `mov x1, value;
// mov x0, x20; bl`, x86-64 the same in rsi / rdi with the stack aligned for a C call; then f and
// c are back in their registers for the code after it.
static void regs_runtime_call(MirSlot value, uint32_t rt, int returns)
{
    if (nat.arch->cpu == CPU_ARM64)
    {
        int r = R(value);
        if (r >= 0)
        {
            a64_mov(1, r);
        }
        else
        {
            a64_ldr_slot(1, value);
        }
        a64_mov(0, 20);
        push_fixup(REGION_RTCALLS, &nat.rtcalls, &nat.nrtcalls, &nat.rtcalls_cap, (Fixup){nat.len, SR_BRANCH26, 0, rt});
        put32(0x94000000u); // bl
        if (returns)
        {
            a64_mov(0, 19);
            a64_mov(1, 20);
        }
        return;
    }
    x_load(XSI, value);
    put_bytes("\x4c\x89\xf7", 3); // mov rdi, r14
    xb(0x50);                       // push rax: the stack aligned for the call
    xb(0xe8);
    push_fixup(REGION_RTCALLS, &nat.rtcalls, &nat.nrtcalls, &nat.rtcalls_cap, (Fixup){nat.len, SR_REL32, -4, rt});
    put32(0);
    if (returns)
    {
        xb(0x59);                       // pop rcx
        put_bytes("\x48\x89\xdf", 3); // mov rdi, rbx
        put_bytes("\x4c\x89\xf6", 3); // mov rsi, r14
    }
}

static void regs_epilogue(const MirFunc *f)
{
    for (MirSlot s = 0; s < f->ret_slots; s++)
    {
        spill_reads(&(MirInst){.op = MIR_COPY, .a = s});
    }
    for (int i = a64_nsave - 2; i >= 0 && nat.arch->cpu == CPU_ARM64; i -= 2)
    {
        put32(0xa8c103e0u | (uint32_t)a64_save[i + 1] << 10 | a64_save[i]); // ldp, [sp], #16
    }
    for (int i = ra_saved - 2; i >= 0 && nat.arch->cpu != CPU_ARM64; i -= 2)
    {
        {
            for (int k = i + 1; k >= i; k--)
            {
                if (x64_regs[k] & 8)
                {
                    xb(0x41);
                }
                xb((uint8_t)(0x58 | (x64_regs[k] & 7))); // pop
            }
        }
    }
    if (nat.arch->cpu == CPU_ARM64)
    {
        put32(0xd65f03c0u); // ret
    }
    else
    {
        xb(0xc3);
    }
}

// A string: its length and bytes in the code, after the instruction that takes its address.
static void regs_string(const MirInst *in, const MirString *str)
{
    size_t data = 8 + str->len;
    size_t padded = (data + 3) & ~(size_t)3;
    int r = R(in->d);
    if (nat.arch->cpu == CPU_ARM64)
    {
        int t = r >= 0 ? r : 9;
        uint32_t off = r >= 0 ? 8 : 12;
        put32(0x10000000u | (off >> 2) << 5 | (uint32_t)t); // adr t, data
        a64_fin(in->d, t);
        put32(0x14000000u | (uint32_t)((4 + padded) / 4)); // b over the data
    }
    else
    {
        int t = r >= 0 ? r : XAX;
        xb((uint8_t)(0x48 | (t & 8 ? 4 : 0)));
        xb(0x8d);
        xb((uint8_t)(0x05 | (t & 7) << 3)); // lea t, [rip + disp]
        size_t disp_at = nat.len;
        put32(0);
        x_store(t, in->d);
        xb(0xe9); // jmp over the data
        put32((uint32_t)padded);
        set32(disp_at, (uint32_t)(nat.len - (disp_at + 4)));
    }
    uint64_t len = str->len;
    put_bytes(&len, 8);
    put_bytes(str->bytes, str->len);
    static const uint8_t zero[4] = {0};
    put_bytes(zero, padded - data);
}

// A call: the callee's frame is this one's window, so f moves up by the frame for the call and
// back after it (the callee returns with f and c as it got them: every stencil passes them on).
// arm64 keeps the link register on the stack; x86-64 keeps the stack aligned for the callee.
static void regs_call(const MirFunc *f, SymId callee)
{
    uint32_t off = H(f->frame_size) * 8;
    if (nat.arch->cpu == CPU_ARM64)
    {
        if (!ra_lr)
        {
            put32(0xf81f0ffeu); // str x30, [sp, #-16]!
        }
        if (off < 4096)
        {
            put32(0x91000000u | off << 10); // add x0, x0, #off
        }
        else
        {
            a64_imm(16, off);
            put32(0x8b100000u); // add x0, x0, x16
        }
        push_fixup(REGION_CALLS, &nat.calls, &nat.ncalls, &nat.calls_cap, (Fixup){nat.len, SR_BRANCH26, 0, callee});
        put32(0x94000000u); // bl callee
        if (off < 4096)
        {
            put32(0xd1000000u | off << 10); // sub x0, x0, #off
        }
        else
        {
            a64_imm(16, off);
            put32(0xcb100000u); // sub x0, x0, x16
        }
        if (!ra_lr)
        {
            put32(0xf84107feu); // ldr x30, [sp], #16
        }
        return;
    }
    xb(0x50); // push rax: the callee starts with the stack as a call leaves it
    int small = off <= 127;
    put_bytes(small ? "\x48\x83\xc7" : "\x48\x81\xc7", 3); // add rdi, off
    if (small)
    {
        xb((uint8_t)off);
    }
    else
    {
        put32(off);
    }
    xb(0xe8); // call callee
    push_fixup(REGION_CALLS, &nat.calls, &nat.ncalls, &nat.calls_cap, (Fixup){nat.len, SR_REL32, -4, callee});
    put32(0);
    put_bytes(small ? "\x48\x83\xef" : "\x48\x81\xef", 3); // sub rdi, off
    if (small)
    {
        xb((uint8_t)off);
    }
    else
    {
        put32(off);
    }
    xb(0x59); // pop rcx
}

// An instruction whose operand is read by a stencil from acc (its _a form) when it is in a
// register: rdx / x2 is acc.
static void to_acc_reg(int reg)
{
    if (nat.arch->cpu == CPU_ARM64)
    {
        a64_mov(2, reg);
    }
    else
    {
        x_op(1, 0, "\x89", 1, reg, XDX, 0, 0, 0); // mov rdx, reg
    }
}

// Which slots keep their place in the frame: those in no register, the parameters (the caller
// writes them there) and the result (the caller reads it there), and on x86-64 the operands of
// an instruction done by a stencil from the frame (division). The others are dropped from it.
static void plan_homes(const MirFunc *f)
{
    ra_shift = NULL;
    ra_removed = 0;
    if (f->param_slots == UINT32_MAX)
    {
        return;
    }
    uint8_t *need = (uint8_t *)arena_alloc(ra.norig + 1);
    memset(need, 0, ra.norig + 1);
    for (size_t k = 0; k < f->n && nat.arch->cpu == CPU_X86_64; k++)
    {
        const MirInst *in = &ra.code[k];
        if (clobbers(in) && in->op != MIR_CALL && in->op != MIR_WRITE && in->op != MIR_ZERO)
        {
            MirSlot s[3] = {in->d, in->a, in->b};
            for (int i = 0; i < 3; i++)
            {
                MirSlot slot = ra_slot(&ra, s[i]);
                if (slot < ra.norig)
                {
                    need[slot] = 1;
                }
            }
        }
    }
    uint32_t keep = f->param_slots > f->ret_slots ? f->param_slots : f->ret_slots;
    ra_shift = (uint32_t *)arena_alloc((ra.norig + 1) * sizeof(uint32_t));
    for (MirSlot s = 0; s < ra.norig; s++)
    {
        ra_shift[s] = ra_removed;
        if (s >= keep && !ra.home[s] && !need[s] && !ra.live_in[s])
        {
            ra_removed++;
        }
    }
    ra_shift[ra.norig] = ra_removed;
}

static void emit_regs(const MirFunc *f)
{
    nat.njumps = 0;
    for (uint32_t i = 0; i < f->nlabels; i++)
    {
        nat.labels[i] = NOT_EMITTED;
    }
    ra_saved = (ra_base + ra.used + 1) & ~1;
    a64_plan_save();
    plan_homes(f);
    regs_prologue();
    int arm = nat.arch->cpu == CPU_ARM64;
    for (size_t k = 0; k < f->n; k++)
    {
        const MirInst *in = &ra.code[k];
        if (ra.fused[k])
        {
            if (arm)
            {
                a64_compare_branch(in, &ra.code[k + 1]);
            }
            else
            {
                x_compare_branch(in, &ra.code[k + 1]);
            }
            k++;
            continue;
        }
        switch (in->op)
        {
        case MIR_CONST:
        {
            uint64_t v = in->imm;
            if (arm)
            {
                if (v == 0 && R(in->d) < 0)
                {
                    a64_str_slot(31, in->d); // str xzr
                    break;
                }
                int t = a64_dst(in->d, 9);
                a64_imm(t, v);
                a64_fin(in->d, t);
            }
            else if (R(in->d) >= 0)
            {
                x_imm(R(in->d), v);
            }
            else if ((int64_t)v >= INT32_MIN && (int64_t)v <= INT32_MAX)
            {
                x_op(1, 0, "\xc7", 1, 0, XDI, 1, (int32_t)(H(in->d) * 8), 0); // mov qword [rdi + d], simm32
                put32((uint32_t)v);
            }
            else
            {
                x_imm(XAX, v);
                x_store(XAX, in->d);
            }
            break;
        }
        case MIR_COPY:
            if (in->d == in->a)
            {
                break;
            }
            if (arm)
            {
                int rd = R(in->d);
                if (rd >= 0)
                {
                    int s = a64_src(in->a, rd);
                    a64_mov(rd, s);
                }
                else
                {
                    a64_str_slot(a64_src(in->a, 9), in->d);
                }
            }
            else
            {
                int rd = R(in->d);
                if (rd >= 0)
                {
                    x_load(rd, in->a);
                }
                else
                {
                    x_store(x_src(in->a, XAX), in->d);
                }
            }
            break;
        case MIR_ADDR:
            if (arm)
            {
                int t = a64_dst(in->d, 9);
                uint32_t off = H(in->a) * 8;
                if (off < 4096)
                {
                    put32(0x91000000u | off << 10 | (uint32_t)t); // add t, x0, #off
                }
                else
                {
                    a64_imm(10, off);
                    put32(0x8b0a0000u | (uint32_t)t); // add t, x0, x10
                }
                a64_fin(in->d, t);
            }
            else
            {
                int t = R(in->d) >= 0 ? R(in->d) : XAX;
                x_op(1, 0, "\x8d", 1, t, XDI, 1, (int32_t)(H(in->a) * 8), 0); // lea t, [rdi + a*8]
                x_store(t, in->d);
            }
            break;
        case MIR_ZERO:
            if (in->imm > 16)
            {
                emit(ST_zero, H(in->a), in->imm, 0, 0, 0);
                break;
            }
            if (!arm)
            {
                put_bytes("\x31\xc0", 2); // xor eax, eax
            }
            for (uint64_t i = 0; i < in->imm; i++)
            {
                if (arm)
                {
                    a64_str_slot(31, in->a + (MirSlot)i);
                }
                else
                {
                    x_store(XAX, in->a + (MirSlot)i);
                }
            }
            break;
        case MIR_LOAD:
        case MIR_STORE:
            if (arm)
            {
                a64_mem(in, in->op == MIR_STORE);
            }
            else if (in->imm > 0x7fffffffull)
            {
                spill_reads(in);
                if (in->op == MIR_LOAD)
                {
                    emit(st_load[in->var], H(in->d), H(in->a), in->imm, 0, 0);
                    reload_write(in->d);
                }
                else
                {
                    emit(st_store[in->var], H(in->a), in->imm, H(in->b), 0, 0);
                }
            }
            else
            {
                x_mem(in, in->op == MIR_STORE);
            }
            break;
        case MIR_BIN:
        case MIR_BINI:
            if (arm)
            {
                a64_bin(in);
            }
            else
            {
                x_bin(in);
            }
            break;
        case MIR_NEG:
        case MIR_NOT:
        case MIR_NORM:
            if (arm)
            {
                int s = a64_src(in->a, 9);
                int t = a64_dst(in->d, 9);
                if (in->op == MIR_NEG)
                {
                    put32(0xcb0003e0u | (uint32_t)s << 16 | (uint32_t)t); // neg
                    a64_norm(t, t, in->var);
                }
                else if (in->op == MIR_NOT)
                {
                    put32(0xf100001fu | (uint32_t)s << 5); // cmp s, #0
                    a64_cset(t, MB_EQ, MV_U64);
                }
                else
                {
                    a64_norm(t, s, in->var);
                }
                a64_fin(in->d, t);
            }
            else
            {
                x_load(XAX, in->a);
                if (in->op == MIR_NEG)
                {
                    put_bytes("\x48\xf7\xd8", 3); // neg rax
                    x_norm(in->var);
                }
                else if (in->op == MIR_NOT)
                {
                    put_bytes("\x48\x85\xc0", 3); // test rax, rax
                    x_setcc(MB_EQ, MV_U64);
                }
                else
                {
                    x_norm(in->var);
                }
                x_store(XAX, in->d);
            }
            break;
        case MIR_LABEL:
            nat.labels[in->imm] = nat.len;
            break;
        case MIR_JZ:
        case MIR_JNZ:
            if (arm)
            {
                int r = a64_src(in->a, 9);
                push_jump(nat.len, SR_BRANCH19, 0, (uint32_t)in->imm);
                put32((in->op == MIR_JZ ? 0xb4000000u : 0xb5000000u) | (uint32_t)r); // cbz / cbnz
            }
            else
            {
                int r = R(in->a);
                if (r >= 0)
                {
                    x_op(1, 0, "\x85", 1, r, r, 0, 0, 0); // test r, r
                }
                else
                {
                    x_op(1, 0, "\x83", 1, 7, XDI, 1, (int32_t)(H(in->a) * 8), 0); // cmp qword [rdi + a], 0
                    xb(0);
                }
                xb(0x0f);
                xb(in->op == MIR_JZ ? 0x84 : 0x85);
                push_jump(nat.len, SR_REL32, -4, (uint32_t)in->imm);
                put32(0);
            }
            break;
        case MIR_CALL:
            regs_call(f, (SymId)in->imm);
            break;
        case MIR_RET:
            regs_epilogue(f);
            break;
        case MIR_STR:
            regs_string(in, &f->strings[in->imm]);
            break;
        case MIR_WRITE:
        case MIR_TRAP:
        {
            int st = in->op == MIR_TRAP ? ST_trap : st_write[in->sub];
            int operand = in->op == MIR_TRAP || (in->sub != MW_SPACE && in->sub != MW_NL);
            if (ra_base && (in->op == MIR_TRAP || in->sub != MW_BYTE) && operand)
            {
                static const uint8_t rt_of[] = {[MW_STR] = RT_write_str, [MW_INT] = RT_write_int,
                                                [MW_UINT] = RT_write_uint, [MW_BOOL] = RT_write_bool};
                regs_runtime_call(in->a, in->op == MIR_TRAP ? RT_trap : rt_of[in->sub], in->op != MIR_TRAP);
            }
            else if (operand && R(in->a) >= 0)
            {
                to_acc_reg(R(in->a));
                emit(st + 1, in->a, 0, 0, 0, 0); // the _a form
            }
            else
            {
                emit(st, H(in->a), 0, 0, 0, 0);
            }
            break;
        }
        case MIR_GLOAD:
        case MIR_GSTORE:
            if (arm)
            {
                a64_global(in, in->op == MIR_GSTORE);
            }
            else if (in->imm > 0x0fffffffull)
            {
                spill_reads(in);
                emit(in->op == MIR_GLOAD ? ST_gload : ST_gstore, in->op == MIR_GLOAD ? H(in->d) : in->imm,
                     in->op == MIR_GLOAD ? in->imm : H(in->a), 0, 0, 0);
                reload_write(in->op == MIR_GLOAD ? in->d : (MirSlot)-1);
            }
            else
            {
                x_op(1, 0, "\x8b", 1, XCX, XSI, 1, 16, 0); // mov rcx, [rsi + 16]: c->globals
                if (in->op == MIR_GLOAD)
                {
                    int t = R(in->d) >= 0 ? R(in->d) : XAX;
                    x_op(1, 0, "\x8b", 1, t, XCX, 1, (int32_t)(in->imm * 8), 0);
                    x_store(t, in->d);
                }
                else
                {
                    x_op(1, 0, "\x89", 1, x_src(in->a, XAX), XCX, 1, (int32_t)(in->imm * 8), 0);
                }
            }
            break;
        default:
            break;
        }
    }
    for (size_t i = 0; i < nat.njumps; i++)
    {
        const Fixup *j = &nat.jumps[i];
        fill_branch(j->at, j->reloc, j->addend, nat.labels[j->ref]);
    }
}

static void emit_stencils(const MirFunc *f)
{
    nat.njumps = 0;
    for (uint32_t i = 0; i < f->nlabels; i++)
    {
        nat.labels[i] = NOT_EMITTED;
    }

    ret_slots = f->ret_slots;
    addressed_lo = UINT32_MAX;
    addressed_hi = 0;
    nloops = 0;
    loops_overflow = 0;
    for (uint32_t i = 0; i < f->nlabels; i++)
    {
        label_index[i] = UINT32_MAX;
    }
    for (size_t k = 0; k < f->n; k++)
    {
        const MirInst *in = &f->code[k];
        if (in->op == MIR_LABEL)
        {
            label_index[in->imm] = (uint32_t)k;
        }
        else if ((in->op == MIR_JZ || in->op == MIR_JNZ) && label_index[in->imm] != UINT32_MAX)
        {
            if (nloops < MAX_LOOPS)
            {
                loops[nloops].lo = label_index[in->imm];
                loops[nloops++].hi = (uint32_t)k;
            }
            else
            {
                loops_overflow = 1;
            }
        }
        if (in->op == MIR_ADDR)
        {
            addressed_lo = in->a < addressed_lo ? in->a : addressed_lo;
            addressed_hi = in->a + in->imm > addressed_hi ? in->a + (uint32_t)in->imm : addressed_hi;
        }
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
            emit(st_load[in->var] + v, in->d, in->a, in->imm, 0, 0);
            break;
        case MIR_STORE:
            emit(st_store[in->var] + (in_acc ? 1 : 0), in->a, in->imm, in->b, 0, 0);
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
        case MIR_TRAP:
            emit(in_acc ? ST_trap_a : ST_trap, in->a, 0, 0, 0, 0);
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

// ---- the output bound -------------------------------------------------------------------------
//
// The most bytes a function writes to the system output itself: each write's largest text (a
// string: its length, taken from the `str` that gave the slot its address in the same block; a
// number: 20 characters, as -9223372036854775808; a bool: 5). With the call graph (stack_need)
// this bounds what the whole program writes; when that fits the buffer `output_buffer` asks for,
// the buffer is exactly that (one flush at the end either way). Unknown when a string comes from
// elsewhere (a parameter) or when the function loops.
#define OUT_UNKNOWN UINT64_MAX

static uint64_t out_bound(const MirFunc *f)
{
    uint64_t total = 0;
    int loops = 0, any = 0;
    for (size_t k = 0; k < f->n && !any; k++)
    {
        any = f->code[k].op == MIR_WRITE;
    }
    if (!any)
    {
        return 0; // (a loop without output writes nothing)
    }
    ArenaMark mark = arena_mark();
    uint8_t *placed = (uint8_t *)arena_alloc(f->nlabels + 1);
    memset(placed, 0, f->nlabels + 1);
    for (size_t k = 0; k < f->n; k++)
    {
        const MirInst *in = &f->code[k];
        if (in->op == MIR_LABEL && in->imm < f->nlabels)
        {
            placed[in->imm] = 1;
        }
        if ((in->op == MIR_JZ || in->op == MIR_JNZ) && in->imm < f->nlabels && placed[in->imm])
        {
            loops = 1; // a branch back
        }
        if (in->op != MIR_WRITE)
        {
            continue;
        }
        switch (in->sub)
        {
        case MW_STR:
        {
            uint64_t len = OUT_UNKNOWN;
            for (size_t j = k; j-- > 0;)
            {
                const MirInst *p = &f->code[j];
                if (p->op == MIR_LABEL || p->op == MIR_JZ || p->op == MIR_JNZ || p->op == MIR_CALL)
                {
                    break;
                }
                if (writes(p, in->a))
                {
                    len = p->op == MIR_STR ? f->strings[p->imm].len : OUT_UNKNOWN;
                    break;
                }
            }
            if (len == OUT_UNKNOWN)
            {
                arena_release(mark);
                return OUT_UNKNOWN;
            }
            total += len;
            break;
        }
        case MW_INT:
        case MW_UINT:
            total += 20;
            break;
        case MW_BOOL:
            total += 5;
            break;
        default:
            total += 1;
            break;
        }
    }
    arena_release(mark);
    return loops && total ? OUT_UNKNOWN : total;
}

// Registers or not. Every function gets them (measured over the examples and the benchmark: the
// stencil form was shorter only for bodies that are a lone `ret`, and those now get no register
// either). MODAL_REGS=0 turns them off, to compare: the fuzzer runs every way against the C backend.
static int regs_mode(void)
{
    static int mode = -1;
    if (mode < 0)
    {
        const char *e = getenv("MODAL_REGS");
        mode = e && e[0] == '0' ? 0 : 1;
    }
    return mode;
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
    nat.order[nat.norder - 1].frame = f->frame_size;
    uint32_t uses = f->frame_size > f->ret_slots ? f->frame_size : f->ret_slots;
    for (size_t k = 0; k < f->n; k++)
    {
        const MirInst *in = &f->code[k];
        uint32_t top = in->d > in->a ? in->d : in->a;
        top = in->b > top ? in->b : top;
        if (in->op == MIR_ZERO || in->op == MIR_ADDR)
        {
            top = in->a + (uint32_t)in->imm > top ? in->a + (uint32_t)in->imm - 1 : top;
        }
        uses = top + 1 > uses ? top + 1 : uses;
    }
    nat.order[nat.norder - 1].uses = uses;
    nat.order[nat.norder - 1].out = out_bound(f);

    ArenaMark mark = arena_mark();
    nat.order[nat.norder - 1].call_frame = frame_bytes(&nat.arch->stencils[ST_call]);
    uint8_t *clobber = NULL;
    int runtime = 0, modal = 0; // calls the runtime with a value; calls a Modal function
    if (regs_mode() && uses <= 4096)
    {
        clobber = (uint8_t *)arena_alloc(f->n + 1);
        for (size_t k = 0; k < f->n; k++)
        {
            const MirInst *in = &f->code[k];
            clobber[k] = (uint8_t)clobbers(in);
            modal |= in->op == MIR_CALL;
            runtime |= in->op == MIR_TRAP || (in->op == MIR_WRITE && in->sub != MW_SPACE && in->sub != MW_NL &&
                                              in->sub != MW_BYTE);
        }
    }
    ra_shift = NULL;
    ra_removed = 0;
    // Only in a function that calls no other Modal function: there the two registers cost what
    // the write stencil's own frame did, at the same moment; in one that calls others they would
    // stay saved under every nested call (RAM along the deepest chain).
    ra_base = clobber && runtime && !modal ? 2 : 0;
    ra_lr = clobber && nat.arch->cpu == CPU_ARM64 && (runtime || modal);
    if (!clobber || !regalloc(f, regs_count() - ra_base, scratch_count(), clobber, &ra))
    {
        emit_stencils(f);
        arena_release(mark);
        return;
    }
    emit_regs(f);
    nat.order[nat.norder - 1].saved = (uint32_t)(nat.arch->cpu == CPU_ARM64 ? a64_nsave : ra_saved) * 8;
    nat.order[nat.norder - 1].frame = H(f->frame_size); // the frame without its register-only slots
    nat.order[nat.norder - 1].uses = uses - ra_removed;
    ra_shift = NULL;
    // regs_call: the link register / the alignment push (arm64 ra_lr: in `saved`, below)
    nat.order[nat.norder - 1].call_frame = ra_lr ? 0 : 16;
    nat.regs_functions++;
    arena_release(mark);
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

// Is `f` the entry's main, and is it only `str s; write.str s` pairs, maybe `const s0 = v`, and
// a final `ret`? Then its output and status are known now.
static void note_const_main(const MirFunc *f)
{
    if (!nat.main_sym)
    {
        nat.main_sym = scope_find(cx.units[0].scope, "main", 4);
    }
    if (f->label || f->sym != nat.main_sym || SYM(f->sym)->unit != 0 || SYM(f->sym)->owner)
    {
        return;
    }
    nat.const_main = 0;
    size_t len = 0;
    uint64_t status = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        if (in->op == MIR_STR && i + 1 < f->n && f->code[i + 1].op == MIR_WRITE &&
            f->code[i + 1].sub == MW_STR && f->code[i + 1].a == in->d)
        {
            len += f->strings[in->imm].len;
            i++;
        }
        else if (in->op == MIR_CONST && in->d == 0 && f->returns && f->ret_slots == 1)
        {
            status = in->imm;
        }
        else if (!(in->op == MIR_RET && i + 1 == f->n))
        {
            return;
        }
    }
    uint8_t *text = (uint8_t *)arena_alloc(len ? len : 1);
    size_t at = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        if (f->code[i].op == MIR_STR)
        {
            const MirString *str = &f->strings[f->code[i].imm];
            memcpy(text + at, str->bytes, str->len);
            at += str->len;
        }
    }
    nat.const_main = 1;
    nat.const_text = text;
    nat.const_len = len;
    nat.const_status = status;
}

static void native_function(const BackendJob *job, const MirFunc *f)
{
    (void)job;
    note_const_main(f);
    emit_mir(f);
}

// The entry of a constant program: write(1, text, len), then return the status. No value stack,
// no runtime, no buffer: main is not called (the shake drops it).
static size_t const_entry(void)
{
    uint32_t write_sym = 0;
    while (nat.arch->externs[write_sym] && strcmp(nat.arch->externs[write_sym], "write") != 0)
    {
        write_sym++;
    }
    size_t start = nat.len;
    size_t len = nat.const_len;
    uint32_t status = (uint32_t)(nat.const_status & 0xff);
    if (nat.arch->cpu == CPU_ARM64)
    {
        int wide = len >= 0x10000;
        size_t adr_at = start + 8;
        size_t text_at = start + 4 * (9 + wide);
        int64_t off = (int64_t)(text_at - adr_at);
        put32(0xa9bf7bfdu); // stp x29, x30, [sp, #-16]!
        put32(0x910003fdu); // mov x29, sp
        put32(0x10000001u | (((uint32_t)off & 3u) << 29) | ((((uint32_t)off >> 2) & 0x7ffffu) << 5)); // adr x1, text
        put32(0xd2800020u); // movz x0, #1
        put32(0xd2800002u | (((uint32_t)len & 0xffffu) << 5)); // movz x2, #len
        if (wide)
        {
            put32(0xf2a00002u | ((((uint32_t)len >> 16) & 0xffffu) << 5)); // movk x2, #len >> 16, lsl 16
        }
        push_extern(nat.len, write_sym);
        put32(0x94000000u);                       // bl write
        put32(0x52800000u | (status << 5));       // movz w0, #status
        put32(0xa8c17bfdu);                       // ldp x29, x30, [sp], #16
        put32(0xd65f03c0u);                       // ret
    }
    else
    {
        size_t lea_end = start + 1 + 7;
        size_t text_at = start + 1 + 7 + 5 + 5 + 5 + 5 + 1 + 1;
        put_bytes("\x55", 1); // push rbp (the stack aligned for the call)
        put_bytes("\x48\x8d\x35", 3); // lea rsi, [rip + text]
        put32((uint32_t)(int32_t)((int64_t)text_at - (int64_t)lea_end));
        put_bytes("\xbf\x01\x00\x00\x00", 5); // mov edi, 1
        put_bytes("\xba", 1); // mov edx, len
        put32((uint32_t)len);
        put_bytes("\xe8", 1); // call write
        push_extern(nat.len, write_sym);
        put32(0);
        put_bytes("\xb8", 1); // mov eax, status
        put32(status);
        put_bytes("\x5d\xc3", 2); // pop rbp; ret
    }
    put_bytes(nat.const_text, len);
    return start;
}

// ---- memory budgets ----------------------------------------------------------------------------------
//
// Every byte a program uses at run time is on the native stack, and all of it is known here:
//   - the value stack: the deepest chain of frames, from the call graph (a callee's frame
//     starts where its caller's ends, so a chain needs the sum of their frames);
//   - one machine frame per nested Modal call (the call stencil's);
//   - the runtime context and its output buffer, in the entry's frame;
//   - the runtime functions at the bottom of the deepest chain.
// So the value stack is sized exactly instead of a fixed 512 KB, and a program over the budgets
// its mod.modal sets (max_code, max_ram) is an error now, not a crash on the device.
// A program that calls itself has no known depth: it gets `stack` bytes (mod.modal, 512 KB if
// not set), and the report names a function of the cycle.

typedef struct
{
    uint64_t slots;   // value-stack slots of the deepest chain, from main
    uint32_t levels;  // nested Modal calls on it
    uint64_t machine; // machine-stack bytes of the deepest chain: call stencils and saved registers
    uint64_t out;     // most bytes the program writes to the system output (OUT_UNKNOWN)
    SymId recursive;  // a function that calls itself (through others), or 0
} StackNeed;

static StackNeed stack_need(SymId main_sym)
{
    StackNeed r = {.out = OUT_UNKNOWN};
    uint64_t call_frame = frame_bytes(&nat.arch->stencils[ST_call]);
    size_t nf = nat.norder;
    size_t *index_of = (size_t *)arena_alloc(cx.syms.cap * sizeof(size_t));
    uint64_t *need = (uint64_t *)arena_alloc(nf * sizeof(uint64_t) + 1);
    uint32_t *levels = (uint32_t *)arena_alloc(nf * sizeof(uint32_t) + 1);
    uint64_t *machine = (uint64_t *)arena_alloc(nf * sizeof(uint64_t) + 1);
    uint64_t *out = (uint64_t *)arena_alloc(nf * sizeof(uint64_t) + 1);
    uint8_t *state = (uint8_t *)arena_alloc(nf + 1); // 0: not seen, 1: on the path, 2: done
    size_t *path = (size_t *)arena_alloc(nf * sizeof(size_t) + 1);
    size_t *next = (size_t *)arena_alloc(nf * sizeof(size_t) + 1);
    memset(state, 0, nf);
    for (size_t i = 0; i < cx.syms.cap; i++)
    {
        index_of[i] = SIZE_MAX;
    }
    for (size_t i = 0; i < nf; i++)
    {
        if (nat.order[i].sym)
        {
            index_of[nat.order[i].sym] = i;
        }
    }
    if (main_sym >= cx.syms.cap || index_of[main_sym] == SIZE_MAX)
    {
        return r;
    }
    size_t sp = 0;
    path[sp] = index_of[main_sym];
    next[sp] = nat.order[path[sp]].first_call;
    state[path[sp]] = 1;
    sp++;
    while (sp)
    {
        size_t f = path[sp - 1];
        size_t end = f + 1 < nf ? nat.order[f + 1].first_call : nat.ncalls;
        if (next[sp - 1] < end)
        {
            SymId callee = nat.calls[next[sp - 1]++].ref;
            size_t g = callee < cx.syms.cap ? index_of[callee] : SIZE_MAX;
            if (g == SIZE_MAX || nat.fn_at[callee] == NOT_EMITTED)
            {
                continue; // not generated: reported when the calls are filled
            }
            if (state[g] == 1)
            {
                r.recursive = r.recursive ? r.recursive : callee;
                continue;
            }
            if (state[g] == 0)
            {
                state[g] = 1;
                path[sp] = g;
                next[sp] = nat.order[g].first_call;
                sp++;
            }
            continue;
        }
        // every callee is done: this function's need is its frame and the deepest callee's
        uint64_t deepest = 0, mdeep = 0, written = nat.order[f].out;
        uint32_t lv = 0;
        int calls = 0;
        for (size_t c = nat.order[f].first_call; c < end; c++)
        {
            SymId callee = nat.calls[c].ref;
            size_t g = callee < cx.syms.cap ? index_of[callee] : SIZE_MAX;
            if (g == SIZE_MAX || state[g] != 2)
            {
                continue;
            }
            calls = 1;
            deepest = need[g] > deepest ? need[g] : deepest;
            lv = levels[g] > lv ? levels[g] : lv;
            written = written == OUT_UNKNOWN || out[g] == OUT_UNKNOWN || out[g] > (1u << 30) ? OUT_UNKNOWN
                                                                                             : written + out[g];
            uint64_t cf = nat.order[f].call_frame ? nat.order[f].call_frame : call_frame;
            mdeep = cf + machine[g] > mdeep ? cf + machine[g] : mdeep;
        }
        machine[f] = nat.order[f].saved + mdeep;
        out[f] = written;
        need[f] = nat.order[f].frame + deepest;
        need[f] = nat.order[f].uses > need[f] ? nat.order[f].uses : need[f];
        levels[f] = calls ? lv + 1 : 0;
        state[f] = 2;
        sp--;
    }
    r.slots = need[index_of[main_sym]];
    r.levels = levels[index_of[main_sym]];
    r.machine = machine[index_of[main_sym]];
    r.out = r.recursive ? OUT_UNKNOWN : out[index_of[main_sym]];
    return r;
}

// Bytes of native stack a stencil takes while it runs, read from its prologue: arm64
// `stp x29, x30, [sp, #-N]!` and `sub sp, sp, #M`; x86-64 its pushes, `sub rsp, imm` and the
// return address. Variable-size parts (the entry's arrays) are added by the caller.
static uint32_t frame_bytes(const Stencil *s)
{
    uint32_t total = 0;
    if (nat.arch->cpu == CPU_ARM64)
    {
        for (uint32_t i = 0; i + 4 <= s->size && i < 48; i += 4)
        {
            uint32_t w;
            memcpy(&w, s->code + i, 4);
            if ((w & 0xffc003e0u) == 0xa98003e0u) // stp xA, xB, [sp, #imm]!
            {
                int32_t imm = (int32_t)((w >> 15) & 0x7f);
                imm = imm >= 64 ? imm - 128 : imm;
                total += (uint32_t)(-imm * 8);
            }
            else if ((w & 0xff8003ffu) == 0xd10003ffu) // sub sp, sp, #imm{, lsl #12}
            {
                total += ((w >> 10) & 0xfff) << ((w >> 22) & 1 ? 12 : 0);
            }
        }
        return total;
    }
    total = 8; // the return address
    for (uint32_t i = 0; i < s->size && i < 32;)
    {
        const uint8_t *c = s->code + i;
        if (c[0] >= 0x50 && c[0] <= 0x57)
        {
            total += 8;
            i += 1;
        }
        else if (c[0] == 0x41 && i + 1 < s->size && c[1] >= 0x50 && c[1] <= 0x57)
        {
            total += 8;
            i += 2;
        }
        else if (c[0] == 0x48 && i + 3 < s->size && c[1] == 0x83 && c[2] == 0xec)
        {
            total += c[3];
            i += 4;
        }
        else if (c[0] == 0x48 && i + 6 < s->size && c[1] == 0x81 && c[2] == 0xec)
        {
            uint32_t imm;
            memcpy(&imm, c + 3, 4);
            total += imm;
            i += 7;
        }
        else
        {
            i++; // past the prologue's register moves
        }
    }
    return total;
}

// The machine stack runtime function i takes, with the deepest of the functions it calls.
static uint64_t runtime_depth(uint32_t i)
{
    const Stencil *s = &nat.arch->runtime[i];
    uint64_t deepest = 0;
    for (uint32_t r = 0; r < s->nrelocs; r++)
    {
        uint16_t t = s->relocs[r].target;
        uint64_t d = t >= ST_EXTERN ? 64 : t >= ST_RUNTIME ? runtime_depth(t - ST_RUNTIME) : 0; // 64: the system call
        deepest = d > deepest ? d : deepest;
    }
    return frame_bytes(s) + deepest;
}

static uint64_t round16(uint64_t v)
{
    return (v + 15) & ~(uint64_t)15;
}

static size_t memory_report(Unit *u, StackNeed sn, uint64_t slots, uint64_t outbuf, int entry_st,
                            uint32_t at)
{
    uint64_t entry = frame_bytes(&nat.arch->stencils[entry_st]) + round16(slots * 8) +
                     round16(outbuf);
    // Each nested call: the call stencil's frame and the registers the callee saves (main's too).
    uint64_t calls = sn.machine;
    // The runtime: the deepest chain of its functions from a stencil that calls one (its own
    // frame first), down to the system call.
    uint64_t runtime = 0;
    for (size_t i = 0; i < nat.nrtcalls; i++)
    {
        uint64_t d = frame_bytes(&nat.arch->stencils[ST_write_int]) + runtime_depth(nat.rtcalls[i].ref);
        runtime = d > runtime ? d : runtime;
    }
    uint64_t ram = entry + calls + runtime;
    uint64_t code = nat.len;
    // On bare metal the memory map is the budget unless mod.modal asks for less.
    int bare = target_get()->os == TOS_NONE;
    uint64_t max_code =
        domain_config_int(u->domain, "max_code", bare ? domain_config_int(u->domain, "flash_size", 0) : 0);
    uint64_t max_ram =
        domain_config_int(u->domain, "max_ram", bare ? domain_config_int(u->domain, "ram_size", 0) : 0);
    int len = 0;
    const char *rec = sn.recursive ? symbol_name(sn.recursive, &len) : "";
    if (cx.stats)
    {
        printf("memory    code %llu B, ram %llu B: value stack %llu B (%llu slots, %s), output "
               "%llu B, %u nested calls %llu B, runtime %llu B, entry %llu B\n",
               (unsigned long long)code, (unsigned long long)ram,
               (unsigned long long)(slots * 8), (unsigned long long)slots,
               sn.recursive ? "recursive: from `stack`" : "exact", (unsigned long long)outbuf,
               sn.levels, (unsigned long long)calls, (unsigned long long)runtime,
               (unsigned long long)(entry - round16(slots * 8) - round16(outbuf)));
        if (sn.recursive)
        {
            printf("          '%.*s' calls itself: its depth is not known when compiling\n", len, rec);
        }
        printf("functions %u with registers, %u folded into an identical one, %u calls computed by the compiler\n",
               nat.regs_functions, nat.folded, comptime_folded());
    }
    size_t errors = 0;
    if (max_code && code > max_code)
    {
        diag_at(u, at, "error", "the program's code is %llu bytes, over max_code = %llu (mod.modal)",
                (unsigned long long)code, (unsigned long long)max_code);
        errors++;
    }
    if (max_ram && ram > max_ram)
    {
        diag_at(u, at, "error",
                "the program needs %llu bytes of memory, over max_ram = %llu (mod.modal): value "
                "stack %llu, output buffer %llu, %u nested calls %llu, runtime %llu, entry %llu%s%.*s%s",
                (unsigned long long)ram, (unsigned long long)max_ram,
                (unsigned long long)(slots * 8), (unsigned long long)outbuf, sn.levels,
                (unsigned long long)calls, (unsigned long long)runtime,
                (unsigned long long)(entry - round16(slots * 8) - round16(outbuf)),
                sn.recursive ? ". The value stack is `stack` from mod.modal, because '" : "", len,
                rec, sn.recursive ? "' calls itself: set `stack` in mod.modal to the bytes its calls may take" : "");
        errors++;
    }
    return errors;
}

// Linux: no C library. The one system service the runtime uses, write(2), is three
// instructions placed after the runtime, and _start calls the C-style entry and exits with
// what it returned. Returns the offset of _start, or (size_t)-1 if the program calls a C
// function this system layer does not provide.
static size_t linux_system_layer(Unit *u)
{
    int arm = nat.arch->cpu == CPU_ARM64;
    while (nat.len & 3)
    {
        put_bytes("", 1);
    }
    size_t write_at = nat.len;
    if (arm)
    {
        put32(0xd2800808u); // movz x8, #64 (write)
        put32(0xd4000001u); // svc #0
        put32(0xd65f03c0u); // ret
    }
    else
    {
        put_bytes("\xb8\x01\x00\x00\x00" // mov eax, 1 (write)
                  "\x0f\x05"                 // syscall
                  "\xc3",                     // ret
                  8);
    }
    size_t exit_at = NOT_EMITTED; // only if a trap can end the program
    for (size_t i = 0; i < nat.nexterns; i++)
    {
        const char *name = nat.arch->externs[nat.externs[i].sym];
        if (strcmp(name, "exit") == 0 && exit_at == NOT_EMITTED)
        {
            while (nat.len & 3)
            {
                put_bytes("", 1);
            }
            exit_at = nat.len;
            if (arm)
            {
                put32(0xd2800bc8u); // movz x8, #94 (exit_group), the status already in w0
                put32(0xd4000001u); // svc #0
            }
            else
            {
                put_bytes("\xb8\xe7\x00\x00\x00" // mov eax, 231 (exit_group), status in edi
                          "\x0f\x05",                // syscall
                          7);
            }
        }
    }
    for (size_t i = 0; i < nat.nexterns; i++)
    {
        const MachoImportCall *c = &nat.externs[i];
        const char *name = nat.arch->externs[c->sym];
        size_t target = strcmp(name, "write") == 0 ? write_at
                        : strcmp(name, "exit") == 0 ? exit_at
                                                    : NOT_EMITTED;
        if (target == NOT_EMITTED)
        {
            diag_at(u, 0, "error", "backend native: '%s' is not available on Linux", name);
            return (size_t)-1;
        }
        if (arm)
        {
            fill_branch(c->at, SR_BRANCH26, 0, target);
        }
        else
        {
            set32(c->at, (uint32_t)(int32_t)((int64_t)target - (int64_t)(c->at + 4)));
        }
    }
    while (nat.len & 3)
    {
        put_bytes("", 1);
    }
    size_t start = nat.len;
    if (arm)
    {
        put32(0x94000000u | ((uint32_t)((int64_t)(nat.entry_at - start) / 4) & 0x03ffffffu)); // bl entry
        put32(0xd2800bc8u); // movz x8, #94 (exit_group), the status already in w0
        put32(0xd4000001u); // svc #0
    }
    else
    {
        put_bytes("\xe8", 1); // call entry
        put32((uint32_t)(int32_t)((int64_t)nat.entry_at - (int64_t)(start + 5)));
        put_bytes("\x89\xc7"             // mov edi, eax
                  "\xb8\xe7\x00\x00\x00" // mov eax, 231 (exit_group)
                  "\x0f\x05",             // syscall
                  9);
    }
    return start;
}

// Bare metal: no system. write(2) becomes `return 0` (nothing can reach the system output:
// the checker requires an `out` on a target without one, so the runtime only ever flushes an
// empty buffer), and _start puts the stack at the top of RAM, calls the entry and then waits
// for interrupts forever: there is no process to exit, and main's result stays in x0 / eax.
typedef struct
{
    uint64_t flash_origin, flash_size, ram_origin, ram_size;
} MemoryMap;

static size_t bare_system_layer(const MemoryMap *map)
{
    int arm = nat.arch->cpu == CPU_ARM64;
    while (nat.len & 3)
    {
        put_bytes("", 1);
    }
    size_t write_at = nat.len;
    if (arm)
    {
        put32(0xd2800000u); // movz x0, #0
        put32(0xd65f03c0u); // ret
    }
    else
    {
        put_bytes("\x31\xc0\xc3", 3); // xor eax, eax; ret
    }
    // exit (a trap): there is no process to end, so the board halts, the status in x0 / edi.
    size_t exit_at = NOT_EMITTED;
    for (size_t i = 0; i < nat.nexterns && exit_at == NOT_EMITTED; i++)
    {
        if (strcmp(nat.arch->externs[nat.externs[i].sym], "exit") == 0)
        {
            while (nat.len & 3)
            {
                put_bytes("", 1);
            }
            exit_at = nat.len;
            if (arm)
            {
                put32(0xd503207fu); // wfi
                put32(0x17ffffffu); // b (back to the wfi)
            }
            else
            {
                put_bytes("\x89\xf8"   // mov eax, edi (the status), so the halt below is
                          "\x89\xc7"   // _start's own: mov edi, eax; hlt; jmp (to the hlt)
                          "\xf4"
                          "\xeb\xfd",
                          7);
            }
        }
    }
    for (size_t i = 0; i < nat.nexterns; i++)
    {
        const MachoImportCall *c = &nat.externs[i];
        size_t target = strcmp(nat.arch->externs[c->sym], "exit") == 0 ? exit_at : write_at;
        if (arm)
        {
            fill_branch(c->at, SR_BRANCH26, 0, target);
        }
        else
        {
            set32(c->at, (uint32_t)(int32_t)((int64_t)target - (int64_t)(c->at + 4)));
        }
    }
    while (nat.len & 15)
    {
        put_bytes("", 1);
    }
    size_t start = nat.len;
    uint64_t top = (map->ram_origin + map->ram_size) & ~(uint64_t)15;
    if (arm)
    {
        for (int k = 0; k < 4; k++) // movz/movk x9, top
        {
            put32((k ? 0xf2800009u : 0xd2800009u) | ((uint32_t)k << 21) |
                  ((uint32_t)((top >> (16 * k)) & 0xffff) << 5));
        }
        put32(0x9100013fu); // mov sp, x9
        put32(0x94000000u | ((uint32_t)((int64_t)(nat.entry_at - nat.len) / 4) & 0x03ffffffu)); // bl entry
        put32(0xd503207fu); // wfi
        put32(0x17ffffffu); // b (back to the wfi)
    }
    else
    {
        put_bytes("\x48\xbc", 2); // movabs rsp, top
        put_bytes(&top, 8);
        put_bytes("\xe8", 1); // call entry
        put32((uint32_t)(int32_t)((int64_t)nat.entry_at - (int64_t)(nat.len + 4)));
        put_bytes("\x89\xc7"   // mov edi, eax (main's result, for a debugger)
                  "\xf4"       // hlt
                  "\xeb\xfd", // jmp (back to the hlt)
                  5);
    }
    return start;
}

static int memory_map(Unit *u, MemoryMap *map, uint32_t at)
{
    static const char *const keys[] = {"flash_origin", "flash_size", "ram_origin", "ram_size"};
    uint64_t *v[] = {&map->flash_origin, &map->flash_size, &map->ram_origin, &map->ram_size};
    int missing = 0;
    for (int i = 0; i < 4; i++)
    {
        *v[i] = domain_config_int(u->domain, keys[i], UINT64_MAX);
        missing |= *v[i] == UINT64_MAX;
    }
    if (missing)
    {
        diag_at(u, at, "error",
                "target %s runs without a system: its mod.modal gives the memory map "
                "(flash_origin, flash_size, ram_origin, ram_size)",
                target_get()->name);
        return 0;
    }
    if (map->flash_origin % 16 || map->ram_origin % 16 || !map->ram_size || !map->flash_size)
    {
        diag_at(u, at, "error",
                "the memory map needs flash and RAM at multiples of 16, with sizes other than 0");
        return 0;
    }
    return 1;
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
    // The value stack and the output buffer, sized for this program (see memory budgets).
    StackNeed sn = stack_need(entry);
    uint64_t slots = mir_global_count() + (sn.slots ? sn.slots : 1);
    if (sn.recursive)
    {
        slots = mir_global_count() + domain_config_int(u->domain, "stack", 512 * 1024) / 8;
    }
    int writes_out = 0; // a stencil writes to the system output (a trap only flushes what they wrote)
    for (size_t i = 0; i < nat.nrtcalls && !writes_out; i++)
    {
        writes_out = nat.rtcalls[i].ref != RT_trap;
    }
    // Output that is only constant text, a few times, with no recursion: each string goes straight
    // to the system. No buffer (RAM), no rt_put / rt_flush (code); the same bytes, in the same order.
    int direct = writes_out && !sn.recursive;
    size_t sites = 0;
    for (size_t i = 0; i < nat.nrtcalls && direct; i++)
    {
        direct = nat.rtcalls[i].ref == RT_write_str || nat.rtcalls[i].ref == RT_trap;
        sites += nat.rtcalls[i].ref == RT_write_str;
    }
    direct = direct && sites <= 16;
    if (direct)
    {
        for (size_t i = 0; i < nat.nrtcalls; i++)
        {
            if (nat.rtcalls[i].ref == RT_write_str)
            {
                nat.rtcalls[i].ref = RT_write_direct;
            }
        }
        writes_out = 0;
    }
    uint64_t outbuf = writes_out ? domain_config_int(u->domain, "output_buffer", 4096) : direct ? 0 : 16;
    if (writes_out && sn.out != OUT_UNKNOWN && sn.out < outbuf)
    {
        outbuf = (sn.out + 7) & ~(uint64_t)7; // all of the output fits: one flush at the end, as before
    }
    outbuf = outbuf || direct ? outbuf : 16;
    int void_main = sema_fn_ret(u, entry) == T_VOID;
    int entry_st = direct ? (void_main ? ST_entry_void_direct : ST_entry_direct)
                          : void_main ? ST_entry_void : ST_entry;
    begin_range(0);
    int constant = nat.const_main && mir_global_count() == 0 && nat.const_len < 0xffffffffull;
    if (constant)
    {
        // The whole program is its text and its status: nothing else is reached, so the shake
        // drops main and every runtime function, and no value stack or buffer is needed.
        entry_st = ST_entry_void_direct; // (for the memory report: an upper bound of its frame)
        outbuf = 0;
        slots = 0;
        sn.machine = 0; // main is not called
        nat.entry_at = const_entry();
    }
    else
    {
        nat.entry_at = emit(entry_st, mir_global_count(), slots, outbuf, entry, 0);
    }
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

    size_t n = job->path ? strlen(job->path) : 0;
    int object = n > 2 && strcmp(job->path + n - 2, ".o") == 0;
    if (target_get()->os == TOS_NONE)
    {
        MemoryMap map;
        if (object)
        {
            diag_at(u, 0, "error", "backend native: object files are written for macOS only");
            return 1;
        }
        if (!memory_map(u, &map, SYM(entry)->tok))
        {
            return 1;
        }
        size_t start = bare_system_layer(&map);
        if (memory_report(u, sn, slots, outbuf, entry_st, SYM(entry)->tok))
        {
            return 1;
        }
        if (elf_write_image(job->out, nat.arch->cpu == CPU_ARM64 ? ELF_ARM64 : ELF_X86_64,
                            nat.code, nat.len, start, map.flash_origin))
        {
            diag_at(u, 0, "error", "backend native: cannot write %s", job->path);
            return 1;
        }
        return 0;
    }
    if (target_get()->os != TOS_LINUX && memory_report(u, sn, slots, outbuf, entry_st, SYM(entry)->tok))
    {
        return 1;
    }

    // An executable, written and signed here; or an object file when asked for one (x.o).
    if (target_get()->os == TOS_LINUX)
    {
        if (object)
        {
            diag_at(u, 0, "error", "backend native: object files are written for macOS only");
            return 1;
        }
        size_t start = linux_system_layer(u);
        if (start == (size_t)-1 || memory_report(u, sn, slots, outbuf, entry_st, SYM(entry)->tok))
        {
            return 1;
        }
        if (elf_write_executable(job->out, nat.arch->cpu == CPU_ARM64 ? ELF_ARM64 : ELF_X86_64,
                                 nat.code, nat.len, start))
        {
            diag_at(u, 0, "error", "backend native: cannot write %s", job->path);
            return 1;
        }
        return 0;
    }
    if (!job->out)
    {
        return 0; // `--run` in memory: the code stays in nat for native_run
    }
    const char *base = job->path ? strrchr(job->path, '/') : NULL;
    const char *ident = base ? base + 1 : job->path ? job->path : "a.out";
    MachoCpu cpu = nat.arch->cpu == CPU_ARM64 ? MACHO_ARM64 : MACHO_X86_64;
    // Only the C functions the program calls are imported (a program without a division has no
    // `exit`): every import costs a stub, a GOT slot and a bind.
    const char *imports[9];
    uint32_t index[8] = {0}, nused = 0;
    for (uint32_t k = 0; nat.arch->externs[k] && k < 8; k++)
    {
        int called = 0;
        for (size_t i = 0; i < nat.nexterns && !called; i++)
        {
            called = nat.externs[i].sym == k;
        }
        if (called)
        {
            index[k] = nused;
            imports[nused++] = nat.arch->externs[k];
        }
    }
    imports[nused] = NULL;
    for (size_t i = 0; i < nat.nexterns; i++)
    {
        nat.externs[i].sym = index[nat.externs[i].sym];
    }
    int failed = object ? macho_write_object(job->out, cpu, nat.code, nat.len, nat.entry_at,
                                             nat.externs, nat.nexterns, imports)
                        : macho_write_executable(job->out, cpu, nat.code, nat.len, nat.entry_at,
                                                 nat.externs, nat.nexterns, imports, ident);
    if (failed)
    {
        diag_at(u, 0, "error", "backend native: cannot write %s", job->path);
        return 1;
    }
    return 0;
}

// `--run`: a macOS program for this CPU runs in the compiler's process (run.c).
static int native_can_run(void)
{
    const MachineTarget *t = target_get();
    return t->os == TOS_MACOS && run_host_can(t->cpu == TCPU_X86_64 ? MACHO_X86_64 : MACHO_ARM64);
}

static int native_run(const BackendJob *job)
{
    (void)job;
    MachoCpu cpu = nat.arch->cpu == CPU_ARM64 ? MACHO_ARM64 : MACHO_X86_64;
    return run_in_memory(cpu, nat.code, nat.len, nat.entry_at, nat.externs, nat.nexterns,
                         nat.arch->externs);
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
    b->can_run = native_can_run;
    b->run = native_run;
}
