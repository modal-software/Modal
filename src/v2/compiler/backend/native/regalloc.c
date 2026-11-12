// regalloc.c (v2): register allocation for the native backend, by linear scan over live ranges.
//
// The MIR names frame slots. Most of them are temporaries and locals that nothing else can see:
// their address is never taken (MIR_ADDR), they are not a call window (the callee's frame) and no
// block of slots is cleared over them (MIR_ZERO). Their values can live in machine registers. The
// registers are the ones the C calling convention preserves (arm64 x19-x28, x86-64 rbx rbp
// r12-r15) for values a call out crosses: every stencil, runtime function and C function the code
// calls keeps them, and each Modal function saves the ones it uses. Values no call out crosses get
// scratch registers instead, with nothing to save.
//
//   1. liveness by blocks (labels start them, branches, returns and traps end them), iterated to
//      a fixed point: forward branches settle in one pass, the loops of output routines in two
//   2. webs: the lowering reuses a temporary slot for unrelated values (every string address of
//      a print goes through one slot). Each value that dies inside its block gets a name of its
//      own (RA_VIRTUAL | k), so its range is only its own; a value live out of its block keeps
//      the slot's name, which is what the next blocks read. A value that gets no register uses
//      its slot's place in the frame, which no other value of that slot needs at the same time
//   3. liveness again over the names; each name's range is the hull of the points where it is
//      live or named, in code order (a loop's values are live over all of it)
//   4. Poletto & Sarkar's linear scan: ranges by start; when no register is free, the range that
//      ends last stays in the frame
// Slots live at entry (parameters) are loaded first. The result's slots are never split: the
// return reads them by their slot.
#include "v2/compiler/backend/native/regalloc.h"
#include <string.h>

// The operands an instruction reads (up to 2, and the result's slots for a return) and writes.
static int inst_reads(const MirInst *in, MirSlot out[2], int *all_ret)
{
    *all_ret = 0;
    switch (in->op)
    {
    case MIR_BIN:
    case MIR_STORE:
        out[0] = in->a;
        out[1] = in->b;
        return 2;
    case MIR_COPY:
    case MIR_BINI:
    case MIR_NEG:
    case MIR_NOT:
    case MIR_NORM:
    case MIR_LOAD:
    case MIR_JZ:
    case MIR_JNZ:
    case MIR_GSTORE:
    case MIR_TRAP:
        out[0] = in->a;
        return 1;
    case MIR_WRITE:
        if (in->sub == MW_SPACE || in->sub == MW_NL)
        {
            return 0;
        }
        out[0] = in->a;
        return 1;
    case MIR_RET:
        *all_ret = 1;
        return 0;
    default:
        return 0;
    }
}

static int inst_writes(const MirInst *in)
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
        return 1;
    default:
        return 0;
    }
}

// The operand fields an instruction reads, to rename them.
static void renamed_reads(MirInst *in, MirSlot (*name)(void *, MirSlot), void *ctx)
{
    MirSlot r[2];
    int all_ret;
    int n = inst_reads(in, r, &all_ret);
    if (n >= 1)
    {
        in->a = name(ctx, in->a);
    }
    if (n == 2)
    {
        in->b = name(ctx, in->b);
    }
}

typedef struct
{
    uint32_t start, end; // instruction indices, inclusive
    uint32_t name;       // index in RegAlloc.reg
    MirSlot as;          // the name as the code writes it
} Range;

static int by_start(const void *x, const void *y)
{
    const Range *a = (const Range *)x, *b = (const Range *)y;
    if (a->start != b->start)
    {
        return a->start < b->start ? -1 : 1;
    }
    return a->name < b->name ? -1 : a->name > b->name;
}

#define BIT(set, i) ((set)[(i) >> 6] >> ((i) & 63) & 1)
#define SETBIT(set, i) ((set)[(i) >> 6] |= 1ull << ((i) & 63))

// The control flow of the function (the same for its code before and after renaming).
typedef struct
{
    const MirFunc *f;
    uint32_t nb;
    uint32_t *first, *last, *label_block;
} Blocks;

// Liveness of the names of `code`: dense[name] is its bit (UINT32_MAX: not tracked).
typedef struct
{
    const MirInst *code;
    const RegAlloc *ra;
    const uint32_t *dense;
    uint32_t nd;
    size_t words;
    uint64_t *in, *out;
} Live;

static int bit_of(const Live *lv, MirSlot s, uint32_t *k)
{
    uint32_t n = ra_index(lv->ra, s);
    if (n >= lv->ra->nnames || lv->dense[n] == UINT32_MAX)
    {
        return 0;
    }
    *k = lv->dense[n];
    return 1;
}

static int liveness(const Blocks *bl, Live *lv)
{
    const MirFunc *f = bl->f;
    uint32_t nb = bl->nb;
    size_t words = lv->words = (lv->nd + 63) / 64;
    size_t bytes = nb * words * 8;
    uint64_t *use = (uint64_t *)arena_alloc(bytes + 8), *def = (uint64_t *)arena_alloc(bytes + 8);
    lv->in = (uint64_t *)arena_alloc(bytes + 8);
    lv->out = (uint64_t *)arena_alloc(bytes + 8);
    memset(use, 0, bytes);
    memset(def, 0, bytes);
    memset(lv->in, 0, bytes);
    memset(lv->out, 0, bytes);
    for (uint32_t b = 0; b < nb; b++)
    {
        uint64_t *u = use + b * words, *d = def + b * words;
        for (uint32_t i = bl->first[b]; i <= bl->last[b]; i++)
        {
            const MirInst *in = &lv->code[i];
            MirSlot r[2];
            int all_ret;
            uint32_t k;
            int nr = inst_reads(in, r, &all_ret);
            for (int j = 0; j < nr; j++)
            {
                if (bit_of(lv, r[j], &k) && !BIT(d, k))
                {
                    SETBIT(u, k);
                }
            }
            for (MirSlot s = 0; all_ret && s < f->ret_slots; s++)
            {
                if (bit_of(lv, s, &k) && !BIT(d, k))
                {
                    SETBIT(u, k);
                }
            }
            if (inst_writes(in) && bit_of(lv, in->d, &k))
            {
                SETBIT(d, k);
            }
        }
    }
    // Backwards to a fixed point: out(b) = union of in(successors); in(b) = use | (out & ~def).
    for (int changed = 1, rounds = 0; changed; rounds++)
    {
        changed = 0;
        if (rounds > 64)
        {
            return 0; // not settling: leave the function in the frame
        }
        for (uint32_t b = nb; b-- > 0;)
        {
            const MirInst *end = &lv->code[bl->last[b]];
            uint64_t *o = lv->out + b * words;
            uint32_t succ[2];
            int ns = 0;
            if (end->op != MIR_RET && end->op != MIR_TRAP && b + 1 < nb)
            {
                succ[ns++] = b + 1;
            }
            if ((end->op == MIR_JZ || end->op == MIR_JNZ) && end->imm < f->nlabels)
            {
                succ[ns++] = bl->label_block[end->imm];
            }
            for (int k = 0; k < ns; k++)
            {
                const uint64_t *si = lv->in + succ[k] * words;
                for (size_t w = 0; w < words; w++)
                {
                    o[w] |= si[w];
                }
            }
            uint64_t *ib = lv->in + b * words;
            const uint64_t *u = use + b * words, *d = def + b * words;
            for (size_t w = 0; w < words; w++)
            {
                uint64_t v = u[w] | (o[w] & ~d[w]);
                if (v != ib[w])
                {
                    ib[w] = v;
                    changed = 1;
                }
            }
        }
    }
    return 1;
}

// Renaming state: the name each slot's current value has in this block.
typedef struct
{
    MirSlot *cur;
    uint32_t *stamp;
    uint32_t block;
} Rename;

static MirSlot current_name(void *ctx, MirSlot s)
{
    Rename *rn = (Rename *)ctx;
    return rn->stamp[s] == rn->block ? rn->cur[s] : s;
}

int regalloc(const MirFunc *f, int preserved, int scratch, const uint8_t *clobber, RegAlloc *out)
{
    int nregs = preserved + scratch;
    memset(out, 0, sizeof(*out));
    if (f->n == 0 || f->n > 50000)
    {
        return 0;
    }
    // The slots named, and which may live in a register.
    uint32_t nslots = f->ret_slots;
    int calls = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        uint32_t top = in->d > in->a ? in->d : in->a;
        top = in->b > top ? in->b : top;
        if (in->op == MIR_ZERO || in->op == MIR_ADDR)
        {
            top = in->a + (uint32_t)in->imm > top ? in->a + (uint32_t)in->imm - 1 : top;
        }
        nslots = top + 1 > nslots ? top + 1 : nslots;
        calls |= in->op == MIR_CALL;
    }
    if (nslots >= RA_VIRTUAL)
    {
        return 0;
    }
    uint8_t *ok = (uint8_t *)arena_alloc(nslots + 1);
    for (uint32_t s = 0; s < nslots; s++)
    {
        ok[s] = s < 4096 && (!calls || s < f->frame_size); // past 4096 no short offset reaches it
    }
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        if (in->op == MIR_ZERO || in->op == MIR_ADDR)
        {
            for (uint64_t k = 0; k < in->imm && in->a + k < nslots; k++)
            {
                ok[in->a + k] = 0;
            }
        }
    }
    out->norig = out->nnames = nslots;
    out->code = (MirInst *)f->code;
    out->live_in = (uint8_t *)arena_alloc(nslots + 1);
    out->home = (uint8_t *)arena_alloc(nslots + 1);
    out->fused = (uint8_t *)arena_alloc(f->n);
    memset(out->live_in, 0, nslots);
    memset(out->fused, 0, f->n);
    for (uint32_t s = 0; s < nslots; s++)
    {
        out->home[s] = !ok[s];
    }
    out->reg = (uint8_t *)arena_alloc(nslots + 1);
    memset(out->reg, RA_NONE, nslots);
    uint32_t *dense = (uint32_t *)arena_alloc((nslots + 1) * sizeof(uint32_t));
    uint32_t nd = 0;
    for (uint32_t s = 0; s < nslots; s++)
    {
        dense[s] = ok[s] ? nd++ : UINT32_MAX;
    }
    if (nd == 0 || nregs == 0)
    {
        memset(out->home, 1, nslots);
        return 1;
    }

    // Blocks.
    Blocks bl = {f, 0, NULL, NULL, NULL};
    bl.label_block = (uint32_t *)arena_alloc((f->nlabels + 1) * sizeof(uint32_t));
    uint32_t *block_of = (uint32_t *)arena_alloc(f->n * sizeof(uint32_t));
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        int leader = i == 0 || in->op == MIR_LABEL;
        if (i > 0)
        {
            uint8_t p = f->code[i - 1].op;
            leader |= p == MIR_JZ || p == MIR_JNZ || p == MIR_RET || p == MIR_TRAP;
        }
        bl.nb += leader;
        block_of[i] = bl.nb - 1;
        if (in->op == MIR_LABEL && in->imm < f->nlabels)
        {
            bl.label_block[in->imm] = bl.nb - 1;
        }
    }
    bl.first = (uint32_t *)arena_alloc(bl.nb * sizeof(uint32_t));
    bl.last = (uint32_t *)arena_alloc(bl.nb * sizeof(uint32_t));
    for (size_t i = f->n; i-- > 0;)
    {
        bl.first[block_of[i]] = (uint32_t)i;
    }
    for (size_t i = 0; i < f->n; i++)
    {
        bl.last[block_of[i]] = (uint32_t)i;
    }

    // 1. Liveness of the slots.
    Live slots = {f->code, out, dense, nd, 0, NULL, NULL};
    if (!liveness(&bl, &slots))
    {
        return 0;
    }

    // 2. Webs: a value that dies in its block gets a name of its own. Only a slot written more
    // than once has more than one value; when there is none, the names are the slots.
    uint8_t *writes = (uint8_t *)arena_alloc(nslots + 1);
    memset(writes, 0, nslots);
    int reused = 0;
    for (size_t i = 0; i < f->n && !reused; i++)
    {
        const MirInst *in = &f->code[i];
        if (inst_writes(in) && in->d < nslots && ok[in->d] && in->d >= f->ret_slots)
        {
            reused = writes[in->d]++ > 0;
        }
    }
    uint32_t nvirt = 0;
    MirInst *code = (MirInst *)f->code;
    Live names = slots;
    uint8_t *reg = out->reg;
    if (!reused)
    {
        goto ranges;
    }
    code = (MirInst *)arena_alloc(f->n * sizeof(MirInst));
    memcpy(code, f->code, f->n * sizeof(MirInst));
    uint32_t *orig = (uint32_t *)arena_alloc(f->n * sizeof(uint32_t) + 4);
    uint8_t *last_def = (uint8_t *)arena_alloc(f->n);
    memset(last_def, 0, f->n);
    Rename rn = {(MirSlot *)arena_alloc(nslots * sizeof(MirSlot)), (uint32_t *)arena_alloc(nslots * sizeof(uint32_t)), 0};
    memset(rn.stamp, 0, nslots * sizeof(uint32_t));
    for (uint32_t b = 0; b < bl.nb; b++)
    {
        rn.block = b + 1;
        for (uint32_t i = bl.last[b] + 1; i-- > bl.first[b];) // the last write of each slot here
        {
            const MirInst *in = &f->code[i];
            if (inst_writes(in) && in->d < nslots && ok[in->d] && rn.stamp[in->d] != rn.block)
            {
                rn.stamp[in->d] = rn.block;
                last_def[i] = 1;
            }
        }
        // stamps of this block meant "seen" above; from here they mean "renamed in this block"
        rn.block = (b + 1) | 0x80000000u;
        for (uint32_t i = bl.first[b]; i <= bl.last[b]; i++)
        {
            MirInst *in = &code[i];
            renamed_reads(in, current_name, &rn);
            MirSlot w = f->code[i].d;
            if (!inst_writes(in) || w >= nslots || !ok[w] || w < f->ret_slots)
            {
                continue;
            }
            const uint64_t *live_out = slots.out + b * slots.words;
            if (last_def[i] && BIT(live_out, dense[w]))
            {
                rn.stamp[w] = 0; // the value the next blocks read: the slot's own name
                in->d = w;
                continue;
            }
            orig[nvirt] = w;
            in->d = RA_VIRTUAL | nvirt++;
            rn.cur[w] = in->d;
            rn.stamp[w] = rn.block;
        }
    }
    out->code = code;
    out->orig = orig;
    out->nnames = nslots + nvirt;
    reg = (uint8_t *)arena_alloc(out->nnames + 1);
    memset(reg, RA_NONE, out->nnames);
    out->reg = reg;
    uint32_t *dense2 = (uint32_t *)arena_alloc((out->nnames + 1) * sizeof(uint32_t));
    memcpy(dense2, dense, nslots * sizeof(uint32_t));
    for (uint32_t k = 0; k < nvirt; k++)
    {
        dense2[nslots + k] = nd + k;
    }

    // 3. Liveness of the names, and their ranges.
    names = (Live){code, out, dense2, nd + nvirt, 0, NULL, NULL};
    if (!liveness(&bl, &names))
    {
        return 0;
    }
ranges:;
    uint32_t nn = nd + nvirt;
    Range *rg = (Range *)arena_alloc(nn * sizeof(Range));
    uint32_t *named = (uint32_t *)arena_alloc(nn * sizeof(uint32_t)); // instructions naming it
    for (uint32_t s = 0; s < nslots; s++)
    {
        if (ok[s])
        {
            rg[dense[s]] = (Range){UINT32_MAX, 0, s, s};
        }
    }
    for (uint32_t k = 0; k < nvirt; k++)
    {
        rg[nd + k] = (Range){UINT32_MAX, 0, nslots + k, RA_VIRTUAL | k};
    }
    memset(named, 0, nn * sizeof(uint32_t));
#define TOUCH(k, i)                                                                                \
    do                                                                                             \
    {                                                                                              \
        rg[k].start = (i) < rg[k].start ? (i) : rg[k].start;                                       \
        rg[k].end = (i) > rg[k].end ? (i) : rg[k].end;                                             \
    } while (0)
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &code[i];
        MirSlot r[2];
        int all_ret;
        uint32_t k;
        int nr = inst_reads(in, r, &all_ret);
        for (int j = 0; j < nr; j++)
        {
            if (bit_of(&names, r[j], &k))
            {
                TOUCH(k, (uint32_t)i);
                named[k]++;
            }
        }
        for (MirSlot s = 0; all_ret && s < f->ret_slots; s++)
        {
            if (bit_of(&names, s, &k))
            {
                TOUCH(k, (uint32_t)i);
            }
        }
        if (inst_writes(in) && bit_of(&names, in->d, &k))
        {
            TOUCH(k, (uint32_t)i);
            named[k]++;
        }
    }
    size_t words = names.words;
    for (uint32_t b = 0; b < bl.nb; b++)
    {
        const uint64_t *ib = names.in + b * words, *o = names.out + b * words;
        for (size_t w = 0; w < words; w++)
        {
            for (uint64_t bits = ib[w] | o[w]; bits; bits &= bits - 1)
            {
                uint32_t k = (uint32_t)(w * 64 + (size_t)__builtin_ctzll(bits));
                if (BIT(ib, k))
                {
                    TOUCH(k, bl.first[b]);
                }
                if (BIT(o, k))
                {
                    TOUCH(k, bl.last[b]);
                }
            }
        }
    }
    uint8_t *entry = (uint8_t *)arena_alloc(nn + 1); // live at entry: loaded first
    memset(entry, 0, nn);
    for (size_t w = 0; w < words; w++)
    {
        for (uint64_t bits = names.in[w]; bits; bits &= bits - 1)
        {
            uint32_t k = (uint32_t)(w * 64 + (size_t)__builtin_ctzll(bits));
            entry[k] = 1;
            rg[k].start = 0;
            if (rg[k].name < nslots)
            {
                out->live_in[rg[k].name] = 1;
            }
        }
    }
#undef TOUCH

    // Comparisons that only feed the next branch: no register.
    for (uint32_t k = 0; k < nn; k++)
    {
        Range *r = &rg[k];
        if (r->start == UINT32_MAX || r->end != r->start + 1 || entry[k])
        {
            continue;
        }
        const MirInst *cmp = &code[r->start], *br = &code[r->end];
        if ((cmp->op == MIR_BIN || cmp->op == MIR_BINI) && cmp->sub >= MB_EQ && cmp->d == r->as &&
            (br->op == MIR_JZ || br->op == MIR_JNZ) && br->a == r->as && cmp->a != r->as &&
            (cmp->op == MIR_BINI || cmp->b != r->as))
        {
            out->fused[r->start] = 1;
            r->start = UINT32_MAX;
        }
    }

    // Clobbers before each instruction, to ask whether one is inside a range.
    uint32_t *before = (uint32_t *)arena_alloc((f->n + 1) * sizeof(uint32_t));
    before[0] = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        before[i + 1] = before[i] + (clobber && clobber[i]);
    }

    // 4. Linear scan. A name no instruction names (a parameter only passed back as the result)
    // stays where it is: a register would only load it and store it back.
    Range *sorted = (Range *)arena_alloc(nn * sizeof(Range) + sizeof(Range));
    uint32_t nr = 0;
    for (uint32_t k = 0; k < nn; k++)
    {
        if (rg[k].start != UINT32_MAX && named[k])
        {
            sorted[nr++] = rg[k];
        }
    }
    if (nr > 32)
    {
        qsort(sorted, nr, sizeof(Range), by_start);
    }
    else
    {
        for (uint32_t i = 1; i < nr; i++) // insertion sort: a few ranges
        {
            Range r = sorted[i];
            uint32_t j = i;
            for (; j > 0 && by_start(&sorted[j - 1], &r) > 0; j--)
            {
                sorted[j] = sorted[j - 1];
            }
            sorted[j] = r;
        }
    }
    Range *active[32];
    int nactive = 0;
    uint32_t free_mask = nregs >= 32 ? 0xffffffffu : (1u << nregs) - 1;
    uint32_t keep_mask = preserved >= 32 ? 0xffffffffu : (1u << preserved) - 1; // the preserved class
    int used = 0;
    for (uint32_t k = 0; k < nr; k++)
    {
        Range *cur = &sorted[k];
        for (int a = 0; a < nactive;) // expire the ranges that ended before this one starts
        {
            if (active[a]->end < cur->start)
            {
                free_mask |= 1u << reg[active[a]->name];
                active[a] = active[--nactive];
            }
            else
            {
                a++;
            }
        }
        // A clobber strictly inside the range (one at its last read happens after the read;
        // one at its first write, before it). A value live at entry is there before index 0.
        int at_entry = cur->name < nslots && out->live_in[cur->name];
        uint32_t from = at_entry ? 0 : cur->start + 1;
        int crosses = cur->end > from && before[cur->end] - before[from] > 0;
        uint32_t can = crosses ? free_mask & keep_mask : free_mask;
        if (!crosses && (can & ~keep_mask))
        {
            can &= ~keep_mask; // a scratch register first: nothing to save
        }
        if (can)
        {
            int r = __builtin_ctz(can);
            free_mask &= ~(1u << r);
            reg[cur->name] = (uint8_t)r;
            active[nactive++] = cur;
            used = r < preserved && r + 1 > used ? r + 1 : used;
            continue;
        }
        if (!crosses)
        {
            continue; // the scratch and preserved registers are taken: the frame
        }
        int far = -1; // spill the range in a preserved register that ends last
        for (int a = 0; a < nactive; a++)
        {
            if (reg[active[a]->name] < preserved && (far < 0 || active[a]->end > active[far]->end))
            {
                far = a;
            }
        }
        if (far >= 0 && active[far]->end > cur->end)
        {
            reg[cur->name] = reg[active[far]->name];
            reg[active[far]->name] = RA_NONE;
            active[far] = cur;
        }
    }
    out->used = used;

    // A slot keeps its place in the frame when one of its values is named and has no register.
    for (uint32_t k = 0; k < nn; k++)
    {
        const Range *r = &rg[k];
        if (named[k] && r->start != UINT32_MAX && reg[r->name] == RA_NONE) // (a fused one has no value)
        {
            out->home[r->name < nslots ? r->name : orig[r->name - nslots]] = 1;
        }
    }
    return 1;
}
