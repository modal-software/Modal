// mir/opt.c (v2): the MIR optimizer. One function at a time, after its frame is final and
// before it is kept for inlining, so an inlined body is already optimized and its constants then
// meet the caller's (p.md B2).
//
// The passes, each conservative (when in doubt, a value is unknown and an instruction stays):
//   fold    constants through copies, arithmetic (the same `compute` the lowering folds with, so
//           the same wrap as the stencils), and memory (a store then a load of the same place and
//           width, nothing in between); a branch on a known value; a divisor guard that cannot
//           fire. Knowledge is per straight-line block: a label forgets everything.
//   output  a number or bool known now is written as its text; neighbouring constant text, spaces
//           and newlines become one string (one runtime call).
//   stores  a store overwritten before anything could read it; stores into a frame struct whose
//           address is only ever stored through (nothing reads it back).
//   fields  a load or store through the address of this function's own struct, at a whole slot:
//           the slot itself (scalar replacement). When no other use of the address is left, it
//           goes (dce), and the struct's slots are plain values again (registers, folding).
//   dce     an instruction without effects whose result nobody reads.
// Slots whose address is taken are never tracked: a store through a pointer can change them.
#include "v2/compiler/mir/mir.h"
#include <string.h>

typedef struct
{
    MirFunc *f;
    uint32_t nslots;
    uint8_t *addressed; // per slot: covered by some ADDR
    uint8_t *known;     // per slot: value known in this block
    uint64_t *value;
} Opt;

// The memory facts of the current block: (base slot, offset, width) holds a known constant.
#define MAX_FACTS 16
typedef struct
{
    MirSlot base;
    uint64_t off;
    uint8_t var;
    uint64_t value;
} Fact;
static Fact facts[MAX_FACTS];
static int nfacts;

static int produces(uint8_t op)
{
    switch (op)
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
    case MIR_GADDR:
    case MIR_DESC:
        return 1;
    default:
        return 0;
    }
}

// Does `in` read slot s? (CALL reads its window and anything addressed; RET the result.)
static int reads(const Opt *o, const MirInst *in, MirSlot s)
{
    switch (in->op)
    {
    case MIR_BIN:
    case MIR_STORE:
        return in->a == s || in->b == s;
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
        return in->a == s;
    case MIR_WRITE:
        return in->sub != MW_SPACE && in->sub != MW_NL && in->a == s;
    case MIR_CALL:
        return s >= o->f->frame_size || (s < o->nslots && o->addressed[s]);
    case MIR_RET:
        return s < o->f->ret_slots;
    case MIR_MOVE:
        return s >= in->a && s < in->a + in->imm;
    case MIR_LOADN:
        return in->a == s;
    default:
        return 0;
    }
}

// Does `in` overwrite slot s entirely?
static int kills(const MirInst *in, MirSlot s)
{
    if (in->op == MIR_ZERO)
    {
        return s >= in->a && s < in->a + in->imm;
    }
    if (in->op == MIR_LOADN)
    {
        return s >= in->d && s < in->d + in->imm && s != in->a;
    }
    if (in->op == MIR_MOVE) // (a move onto itself in part: the slots it reads stay live first)
    {
        return s >= in->d && s < in->d + in->imm && !(s >= in->a && s < in->a + in->imm);
    }
    return produces(in->op) && in->d == s;
}

// Is the value in slot s (written by instruction i) read before it is overwritten? A branch, a
// label or the end of a long scan counts as a read: only straight-line code is followed.
static int live_after(const Opt *o, size_t i, MirSlot s)
{
    const MirFunc *f = o->f;
    if (s >= o->nslots || o->addressed[s]) // (a window slot is read by a CALL: see reads)
    {
        return 1;
    }
    for (size_t k = i + 1, steps = 0; k < f->n && steps < 4096; k++, steps++)
    {
        const MirInst *in = &f->code[k];
        if (reads(o, in, s))
        {
            return 1;
        }
        if (in->op == MIR_LABEL || MIR_BRANCHES(in))
        {
            return 1;
        }
        if (kills(in, s))
        {
            return 0;
        }
        if (MIR_ENDS(in))
        {
            return in->op == MIR_RET ? s < f->ret_slots : 0;
        }
    }
    return f->n > i + 4096; // the end of the function: dead, unless the scan gave up
}

static void forget(Opt *o, MirSlot s)
{
    if (s < o->nslots)
    {
        o->known[s] = 0;
    }
    for (int k = 0; k < nfacts; k++)
    {
        if (facts[k].base == s) // the pointer changed: what it pointed to is another place
        {
            facts[k--] = facts[--nfacts];
        }
    }
    if (s < o->nslots && o->addressed[s])
    {
        nfacts = 0; // memory seen through a pointer changed under it
    }
}

static void learn(Opt *o, MirSlot s, uint64_t v)
{
    forget(o, s);
    if (s < o->nslots && !o->addressed[s])
    {
        o->known[s] = 1;
        o->value[s] = v;
    }
}

static int is_known(const Opt *o, MirSlot s, uint64_t *v)
{
    if (s < o->nslots && o->known[s])
    {
        *v = o->value[s];
        return 1;
    }
    return 0;
}

static void forget_block(Opt *o)
{
    memset(o->known, 0, o->nslots);
    nfacts = 0;
}

static void delete_inst(MirInst *in)
{
    in->op = MIR__COUNT; // removed by compact()
}

static void compact(MirFunc *f)
{
    size_t out = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        if (f->code[i].op != MIR__COUNT)
        {
            f->code[out++] = f->code[i];
        }
    }
    f->n = out;
}

static uint32_t add_string(MirFunc *f, const uint8_t *bytes, size_t len)
{
    uint8_t *copy = (uint8_t *)region_push(REGION_MIR, (len + 7) & ~(size_t)7);
    memcpy(copy, bytes, len);
    if (f->nstrings == f->strings_cap)
    {
        f->strings = (MirString *)region_grow_array(REGION_MIR, f->strings, sizeof(MirString),
                                                    &f->strings_cap, f->nstrings + 1);
    }
    f->strings[f->nstrings] = (MirString){copy, len, 0};
    return (uint32_t)f->nstrings++;
}

// The text a write of a known value prints (what rt_write_int / uint / bool produce).
static size_t value_text(uint8_t sub, uint64_t v, char *buf)
{
    if (sub == MW_BOOL)
    {
        return (size_t)snprintf(buf, 24, "%s", v ? "true" : "false");
    }
    if (sub == MW_INT)
    {
        return (size_t)snprintf(buf, 24, "%lld", (long long)(int64_t)v);
    }
    return (size_t)snprintf(buf, 24, "%llu", (unsigned long long)v);
}

// ---- fold ------------------------------------------------------------------------------------

static int fold(Opt *o)
{
    MirFunc *f = o->f;
    int changed = 0;
    forget_block(o);
    for (size_t i = 0; i < f->n; i++)
    {
        MirInst *in = &f->code[i];
        uint64_t a, b, r;
        switch (in->op)
        {
        case MIR_CONST:
            learn(o, in->d, in->imm);
            break;
        case MIR_COPY:
            if (is_known(o, in->a, &a))
            {
                *in = (MirInst){.op = MIR_CONST, .d = in->d, .imm = a};
                changed = 1;
                learn(o, in->d, a);
            }
            else
            {
                forget(o, in->d);
            }
            break;
        case MIR_BIN:
        case MIR_BINI:
        {
            int ka = is_known(o, in->a, &a);
            int kb = in->op == MIR_BINI ? (b = in->imm, 1) : is_known(o, in->b, &b);
            if (ka && kb && mir_compute(in->sub, in->var, a, b, &r))
            {
                *in = (MirInst){.op = MIR_CONST, .d = in->d, .imm = r};
                changed = 1;
                learn(o, in->d, r);
            }
            else if (in->op == MIR_BIN && kb && b < 0x10000 && mir_normalize(b, in->var) == b &&
                     !(in->sub == MB_DIV && (b == 0 || mir_normalize(b, in->var) == 0)))
            {
                in->op = MIR_BINI; // the constant goes into the instruction
                in->imm = b;
                in->b = 0;
                changed = 1;
                forget(o, in->d);
            }
            else
            {
                forget(o, in->d);
            }
            break;
        }
        case MIR_NEG:
        case MIR_NOT:
        case MIR_NORM:
            if (is_known(o, in->a, &a))
            {
                r = in->op == MIR_NEG    ? mir_normalize(0 - a, in->var)
                    : in->op == MIR_NOT  ? (uint64_t)(a == 0)
                                         : mir_normalize(a, in->var);
                *in = (MirInst){.op = MIR_CONST, .d = in->d, .imm = r};
                changed = 1;
                learn(o, in->d, r);
            }
            else
            {
                forget(o, in->d);
            }
            break;
        case MIR_LOAD:
        {
            if (in->sub == MIR_MEM_DEVICE) // a register: read every time, as written
            {
                nfacts = 0;
                forget(o, in->d);
                break;
            }
            int hit = -1;
            for (int k = 0; k < nfacts && hit < 0; k++)
            {
                hit = facts[k].base == in->a && facts[k].off == in->imm && facts[k].var == in->var ? k : -1;
            }
            if (hit >= 0)
            {
                r = mir_normalize(facts[hit].value, in->var);
                *in = (MirInst){.op = MIR_CONST, .d = in->d, .imm = r};
                changed = 1;
                learn(o, in->d, r);
            }
            else
            {
                forget(o, in->d);
            }
            break;
        }
        case MIR_STORE:
            nfacts = 0; // it may overlap anything seen through another pointer
            for (MirSlot s = 0; s < o->nslots; s++)
            {
                if (o->addressed[s])
                {
                    o->known[s] = 0;
                }
            }
            if (in->sub != MIR_MEM_DEVICE && is_known(o, in->b, &b))
            {
                facts[0] = (Fact){in->a, in->imm, in->var, mir_normalize(b, in->var)};
                nfacts = 1;
            }
            break;
        case MIR_ZERO:
            for (uint64_t k = 0; k < in->imm; k++)
            {
                forget(o, (MirSlot)(in->a + k));
            }
            break;
        case MIR_MOVE:
        case MIR_LOADN:
            nfacts = 0; // it may write slots some address points into
            for (uint64_t k = 0; k < in->imm; k++)
            {
                forget(o, (MirSlot)(in->d + k));
            }
            break;
        case MIR_ADDR:
        case MIR_STR:
        case MIR_GLOAD:
    case MIR_GADDR:
    case MIR_DESC:
            forget(o, in->d);
            break;
        case MIR_CALL:
        {
            // Every argument known: the compiler runs the callee, and the call is its result
            // (p.md B5; sema/comptime.c says when it does not).
            SymId callee = (SymId)in->imm;
            uint32_t nargs = callee != f->sym && SYM(callee)->kind == SYM_METHOD ? mir_param_slots(callee) : 9;
            uint64_t args[8], result;
            uint32_t nresult = 0;
            int all = nargs <= 8 && in->sub != MIR_CALL_GUARD; // (a boundary gives its error slot too)
            for (uint32_t k = 0; k < nargs && all; k++)
            {
                all = is_known(o, f->frame_size + (MirSlot)k, &args[k]);
            }
            // (the callee is lowered for the compiler only now, when the fold can happen)
            int folded = all && comptime_fold_call(callee, args, nargs, &result, &nresult);
            forget_block(o); // the callee writes its window, and memory
            if (folded)
            {
                if (nresult)
                {
                    *in = (MirInst){.op = MIR_CONST, .d = f->frame_size, .imm = result};
                    learn(o, f->frame_size, result);
                }
                else
                {
                    delete_inst(in); // it does nothing the program can see
                }
                changed = 1;
            }
            break;
        }
        case MIR_LABEL:
            forget_block(o);
            break;
        case MIR_JZ:
        case MIR_JNZ:
            if (is_known(o, in->a, &a))
            {
                int taken = in->op == MIR_JZ ? a == 0 : a != 0;
                if (!taken)
                {
                    delete_inst(in);
                    changed = 1;
                }
                else
                {
                    // Taken: the code from here to its label is reached only from here (no
                    // label in between), so it is dead. A divisor guard ends this way.
                    size_t k = i + 1;
                    while (k < f->n && f->code[k].op != MIR_LABEL)
                    {
                        k++;
                    }
                    if (k < f->n && f->code[k].imm == in->imm)
                    {
                        for (size_t j = i; j < k; j++)
                        {
                            delete_inst(&f->code[j]);
                        }
                        changed = 1;
                        i = k - 1; // the label is next: it forgets the block
                    }
                }
            }
            break;
        case MIR_WRITE:
            if ((in->sub == MW_INT || in->sub == MW_UINT || in->sub == MW_BOOL) &&
                is_known(o, in->a, &a) && !live_after(o, i, in->a))
            {
                char text[24];
                size_t len = value_text(in->sub, a, text);
                in->sub = MW_STR;
                in->imm = add_string(f, (const uint8_t *)text, len);
                in->op = MIR_STR + MIR__COUNT; // marked: becomes STR + WRITE.str (see expand)
                changed = 1;
            }
            break;
        default:
            break;
        }
    }
    return changed;
}

// A write of a known value was marked by fold: it becomes `str s` + `write.str s` in the slot it
// read, which is dead after the write (it was only loaded to be printed), or it stays a value
// write if that slot is still needed.
static void expand_writes(Opt *o)
{
    MirFunc *f = o->f;
    size_t marked = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        marked += f->code[i].op == MIR_STR + MIR__COUNT;
    }
    if (!marked)
    {
        return;
    }
    size_t need = f->n + marked;
    if (need > f->cap)
    {
        f->code = (MirInst *)region_grow_array(REGION_MIR, f->code, sizeof(MirInst), &f->cap, need);
    }
    // From the end, so each instruction moves once.
    size_t to = need;
    for (size_t i = f->n; i-- > 0;)
    {
        MirInst in = f->code[i];
        if (in.op != MIR_STR + MIR__COUNT)
        {
            f->code[--to] = in;
            continue;
        }
        f->code[--to] = (MirInst){.op = MIR_WRITE, .sub = MW_STR, .a = in.a};
        f->code[--to] = (MirInst){.op = MIR_STR, .d = in.a, .imm = in.imm};
    }
    f->n = need;
}

// ---- output -----------------------------------------------------------------------------------

// Runs of constant output: `str s = A; write.str s`, `write.space`, `write.nl`, one after the
// other, become one string. Every slot of the run is dead after it (checked).
static int merge_output(Opt *o)
{
    MirFunc *f = o->f;
    int changed = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        size_t j = i, parts = 0, len = 0;
        while (j < f->n)
        {
            const MirInst *in = &f->code[j];
            if (in->op == MIR_WRITE && (in->sub == MW_SPACE || in->sub == MW_NL))
            {
                len += 1;
                j++;
            }
            else if (in->op == MIR_STR && j + 1 < f->n && f->code[j + 1].op == MIR_WRITE &&
                     f->code[j + 1].sub == MW_STR && f->code[j + 1].a == in->d &&
                     !live_after(o, j + 1, in->d))
            {
                len += f->strings[in->imm].len;
                j += 2;
            }
            else
            {
                break;
            }
            parts++;
        }
        if (parts < 2 || !len)
        {
            continue;
        }
        // The slot of the first string of the run carries the merged one.
        MirSlot slot = 0;
        int have_slot = 0;
        uint8_t *text = (uint8_t *)region_push(REGION_MIR, (len + 7) & ~(size_t)7);
        size_t at = 0;
        for (size_t k = i; k < j;)
        {
            const MirInst *in = &f->code[k];
            if (in->op == MIR_WRITE)
            {
                text[at++] = in->sub == MW_SPACE ? ' ' : '\n';
                k++;
            }
            else
            {
                memcpy(text + at, f->strings[in->imm].bytes, f->strings[in->imm].len);
                at += f->strings[in->imm].len;
                if (!have_slot)
                {
                    slot = in->d;
                    have_slot = 1;
                }
                k += 2;
            }
        }
        if (!have_slot)
        {
            continue; // only spaces and newlines: no slot to carry a string
        }
        uint32_t id = add_string(f, text, len);
        f->code[i] = (MirInst){.op = MIR_STR, .d = slot, .imm = id};
        f->code[i + 1] = (MirInst){.op = MIR_WRITE, .sub = MW_STR, .a = slot};
        for (size_t k = i + 2; k < j; k++)
        {
            delete_inst(&f->code[k]);
        }
        changed = 1;
        i = j - 1;
    }
    compact(f);
    return changed;
}

// ---- stores -----------------------------------------------------------------------------------

static int dead_stores(Opt *o)
{
    MirFunc *f = o->f;
    int changed = 0;
    // A store overwritten (same place, same width) before anything could read memory.
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *st = &f->code[i];
        if (st->op != MIR_STORE || st->sub == MIR_MEM_DEVICE)
        {
            continue;
        }
        for (size_t k = i + 1; k < f->n; k++)
        {
            const MirInst *in = &f->code[k];
            if (in->op == MIR_STORE && in->sub != MIR_MEM_DEVICE && in->a == st->a &&
                in->imm == st->imm && in->var == st->var)
            {
                delete_inst(&f->code[i]);
                changed = 1;
                break;
            }
            if (in->op == MIR_LOAD || in->op == MIR_STORE || in->op == MIR_CALL || in->op == MIR_MOVE || in->op == MIR_LOADN ||
                in->op == MIR_LABEL || MIR_BRANCHES(in) || in->op == MIR_RET ||
                in->op == MIR_TRAP || in->op == MIR_WRITE || kills(in, st->a) ||
                (st->a < o->nslots && o->addressed[st->a]))
            {
                break;
            }
        }
    }
    // Stores into a frame struct nobody reads: its address (one ADDR, written once) is only ever
    // the base of stores, and its slots are read by nothing.
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *ad = &f->code[i];
        if (ad->op != MIR_ADDR || ad->d >= f->frame_size || ad->d < f->ret_slots ||
            ad->a + ad->imm > f->frame_size)
        {
            continue;
        }
        MirSlot p = ad->d, lo = ad->a, hi = (MirSlot)(ad->a + ad->imm);
        int ok = 1, stores = 0;
        for (size_t k = 0; k < f->n && ok; k++)
        {
            const MirInst *in = &f->code[k];
            if (k == i)
            {
                continue;
            }
            if (in->op == MIR_ADDR && in->a < hi && in->a + in->imm > lo)
            {
                ok = 0; // another pointer into it
            }
            if (produces(in->op) && in->d == p)
            {
                ok = 0; // p is reused for something else
            }
            for (MirSlot s = lo; s < hi && ok; s++)
            {
                ok = !(reads(o, in, s) && in->op != MIR_CALL) && !(in->op == MIR_CALL && s >= f->frame_size);
            }
            if (ok && reads(o, in, p))
            {
                if (in->op == MIR_STORE && in->a == p && in->b != p && in->sub != MIR_MEM_DEVICE)
                {
                    stores++;
                }
                else
                {
                    ok = 0;
                }
            }
        }
        if (ok && stores)
        {
            for (size_t k = 0; k < f->n; k++)
            {
                if (f->code[k].op == MIR_STORE && f->code[k].a == p)
                {
                    delete_inst(&f->code[k]);
                }
            }
            changed = 1;
        }
    }
    compact(f);
    return changed;
}

// ---- fields ----------------------------------------------------------------------------------
//
// `addr p = s1 (3 slots)` then `load .i64 [p + 8]` reads slot s2: a copy. A narrower field at
// the start of a slot is that slot extended from its width (little-endian on every target), so
// `load .u8 [p + 0]` is `norm.u8 s1`. A whole-slot store is a copy into the slot; a narrower one
// stays (MIR has no bit insertion). Known per block, as the address may be another at a label.
static int frame_fields(Opt *o)
{
    MirFunc *f = o->f;
    int changed = 0, any = 0;
    for (size_t i = 0; i < f->n && !any; i++)
    {
        any = f->code[i].op == MIR_ADDR;
    }
    if (!any)
    {
        return 0;
    }
    MirSlot *base = (MirSlot *)region_push(REGION_MIR, o->nslots * sizeof(MirSlot) * 2);
    MirSlot *count = base + o->nslots; // slots of the struct p points into; 0: p is no such address
    memset(count, 0, o->nslots * sizeof(MirSlot));
    for (size_t i = 0; i < f->n; i++)
    {
        MirInst *in = &f->code[i];
        if (in->op == MIR_LABEL)
        {
            memset(count, 0, o->nslots * sizeof(MirSlot));
            continue;
        }
        if ((in->op == MIR_LOAD || in->op == MIR_STORE) && in->sub != MIR_MEM_DEVICE && in->a < o->nslots &&
            count[in->a] && in->imm % 8 == 0 && in->imm / 8 < count[in->a])
        {
            MirSlot slot = base[in->a] + (MirSlot)(in->imm / 8);
            int whole = in->var == MV_U64 || in->var == MV_I64;
            if (in->op == MIR_LOAD)
            {
                *in = whole ? (MirInst){.op = MIR_COPY, .d = in->d, .a = slot}
                            : (MirInst){.op = MIR_NORM, .var = in->var, .d = in->d, .a = slot};
                changed = 1;
            }
            else if (whole && in->b != in->a)
            {
                *in = (MirInst){.op = MIR_COPY, .d = slot, .a = in->b};
                changed = 1;
            }
        }
        if (produces(in->op) && in->d < o->nslots)
        {
            count[in->d] = 0;
            if (in->op == MIR_ADDR && in->d < f->frame_size && in->a + in->imm <= f->frame_size)
            {
                base[in->d] = in->a;
                count[in->d] = (MirSlot)in->imm;
            }
        }
        else if (in->op == MIR_ZERO || in->op == MIR_MOVE || in->op == MIR_LOADN)
        {
            MirSlot to = in->op == MIR_ZERO ? in->a : in->d;
            for (uint64_t k = 0; k < in->imm && to + k < o->nslots; k++)
            {
                count[to + k] = 0;
            }
        }
    }
    return changed;
}

// ---- dce -------------------------------------------------------------------------------------

static void mark_addressed(Opt *o)
{
    memset(o->addressed, 0, o->nslots);
    const MirFunc *f = o->f;
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        if (in->op == MIR_ADDR)
        {
            for (uint64_t k = 0; k < in->imm && in->a + k < o->nslots; k++)
            {
                o->addressed[in->a + k] = 1;
            }
        }
    }
}

// What live_after says of every slot at once, kept from the end of the function backwards: one
// sweep, so dce costs n, not a forward scan per instruction (n² in a long block). A slot's value
// holds when its stamp is the current epoch; otherwise it is `base`, which a label, a branch or
// an end sets for every slot at once.
typedef struct
{
    const Opt *o;
    uint32_t *stamp;
    uint8_t *val;
    uint32_t epoch;
    uint8_t base;
} Live;

static int live_get(const Live *l, MirSlot s)
{
    if (s >= l->o->nslots || l->o->addressed[s])
    {
        return 1;
    }
    return l->stamp[s] == l->epoch ? l->val[s] : l->base;
}

static void live_set(Live *l, MirSlot s, uint8_t v)
{
    if (s < l->o->nslots)
    {
        l->stamp[s] = l->epoch;
        l->val[s] = v;
    }
}

static void live_range(Live *l, uint64_t from, uint64_t n, uint8_t v)
{
    for (uint64_t k = 0; k < n && from + k < l->o->nslots; k++)
    {
        live_set(l, (MirSlot)(from + k), v);
    }
}

static void live_all(Live *l, uint8_t v)
{
    l->epoch++;
    l->base = v;
}

// The slots `in` reads become live (as reads() says).
static void live_reads(Live *l, const MirInst *in)
{
    const MirFunc *f = l->o->f;
    switch (in->op)
    {
    case MIR_BIN:
    case MIR_STORE:
        live_set(l, in->a, 1);
        live_set(l, in->b, 1);
        break;
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
    case MIR_LOADN:
        live_set(l, in->a, 1);
        break;
    case MIR_WRITE:
        if (in->sub != MW_SPACE && in->sub != MW_NL)
        {
            live_set(l, in->a, 1);
        }
        break;
    case MIR_CALL: // its window (addressed slots are always live)
        if (f->frame_size < l->o->nslots)
        {
            live_range(l, f->frame_size, l->o->nslots - f->frame_size, 1);
        }
        break;
    case MIR_RET:
        live_range(l, 0, f->ret_slots, 1);
        break;
    case MIR_MOVE:
        live_range(l, in->a, in->imm, 1);
        break;
    default:
        break;
    }
}

// From liveness after `in` to liveness before it, in live_after's order: a read, then a label
// or a branch, then an overwrite, then an end.
static void live_step(Live *l, const MirInst *in)
{
    if (in->op == MIR_LABEL || MIR_BRANCHES(in))
    {
        live_all(l, 1);
        return;
    }
    if (MIR_ENDS(in))
    {
        live_all(l, 0); // (a RET's result slots: its reads, below)
        live_reads(l, in);
        return;
    }
    if (in->op == MIR_ZERO)
    {
        live_range(l, in->a, in->imm, 0);
    }
    else if (in->op == MIR_LOADN || in->op == MIR_MOVE)
    {
        live_range(l, in->d, in->imm, 0);
    }
    else if (produces(in->op))
    {
        live_set(l, in->d, 0);
    }
    live_reads(l, in); // (LOADN's a and MOVE's own slots: read, so live again)
}

static int dce(Opt *o)
{
    MirFunc *f = o->f;
    int changed = 0;
    // (a region leaving its shared page starts again elsewhere: then nothing is given back)
    char *base = cx.regions[REGION_MIR].base;
    size_t mark = cx.regions[REGION_MIR].used;
    Live l = {o, (uint32_t *)region_push(REGION_MIR, ((size_t)o->nslots * 4 + 7) & ~(size_t)7),
              (uint8_t *)region_push(REGION_MIR, (o->nslots + 7) & ~(size_t)7), 1, 0};
    memset(l.stamp, 0, (size_t)o->nslots * 4); // nothing stamped: every slot is `base`, dead at the end
    for (size_t i = f->n; i-- > 0;)
    {
        MirInst *in = &f->code[i];
        if (produces(in->op) && !live_get(&l, in->d))
        {
            delete_inst(in); // (removed: it reads nothing, so what it read may die too)
            changed = 1;
            continue;
        }
        if (in->op == MIR_ZERO)
        {
            int any = 0;
            for (uint64_t k = 0; k < in->imm && !any; k++)
            {
                any = live_get(&l, (MirSlot)(in->a + k));
            }
            if (!any)
            {
                delete_inst(in);
                changed = 1;
                continue;
            }
        }
        live_step(&l, in);
    }
    if (cx.regions[REGION_MIR].base == base)
    {
        region_set_end(REGION_MIR, base + mark);
    }
    return changed;
}

// ---- bounds ----------------------------------------------------------------------------------
//
// An index guard (`t = lt.u64 i, N; jnz t, ok; ...; trap; ok:`) that passed says i < N from `ok`
// on, while i is not written: `ok` is reached only from it (the other way traps). A later guard of
// the same i against N or more is then true (`xs[i]` read, `out[i]` written: one check), and fold
// takes its trap away.

// The constant in slot s at instruction k, if the block writes it with one before k.
static int const_before(const MirFunc *f, size_t k, MirSlot s, uint64_t *v)
{
    while (k-- > 0)
    {
        const MirInst *in = &f->code[k];
        if (in->op == MIR_LABEL || MIR_BRANCHES(in) || in->op == MIR_CALL)
        {
            return 0;
        }
        if (kills(in, s))
        {
            if (in->op != MIR_CONST)
            {
                return 0;
            }
            *v = in->imm;
            return 1;
        }
    }
    return 0;
}

// Is instruction k `t = lt.u64 i, N` with N known? (Its i and N.)
static int index_compare(const MirFunc *f, size_t k, MirSlot *i, uint64_t *n)
{
    const MirInst *in = &f->code[k];
    if ((in->op != MIR_BIN && in->op != MIR_BINI) || in->sub != MB_LT || in->var != MV_U64 || in->d == in->a)
    {
        return 0;
    }
    *i = in->a;
    if (in->op == MIR_BINI)
    {
        *n = in->imm;
        return 1;
    }
    return in->b != in->d && const_before(f, k, in->b, n);
}

static int known_bounds(Opt *o)
{
    MirFunc *f = o->f;
    int changed = 0;
    struct
    {
        MirSlot i;
        uint64_t n;
    } held[MAX_FACTS];
    int nheld = 0;
    uint32_t pending_label = UINT32_MAX; // the `ok` of the guard just passed, and what it proves
    MirSlot pending_i = 0;
    uint64_t pending_n = 0;
    for (size_t k = 0; k < f->n; k++)
    {
        MirInst *in = &f->code[k];
        if (in->op == MIR_LABEL)
        {
            int keep = in->imm == pending_label && k > 0 && MIR_ENDS(&f->code[k - 1]);
            int branches = 0; // to this label: only the guard's
            for (size_t j = 0; j < f->n && keep; j++)
            {
                branches += MIR_BRANCHES(&f->code[j]) && f->code[j].imm == in->imm;
            }
            keep = keep && branches == 1;
            if (!keep)
            {
                nheld = 0;
            }
            else if (nheld < MAX_FACTS)
            {
                held[nheld].i = pending_i;
                held[nheld++].n = pending_n;
            }
            pending_label = UINT32_MAX;
            continue;
        }
        MirSlot i;
        uint64_t n;
        if (index_compare(f, k, &i, &n))
        {
            for (int h = 0; h < nheld; h++)
            {
                if (held[h].i == i && held[h].n <= n)
                {
                    *in = (MirInst){.op = MIR_CONST, .d = in->d, .imm = 1};
                    changed = 1;
                    break;
                }
            }
            if (in->op != MIR_CONST && k + 1 < f->n && f->code[k + 1].op == MIR_JNZ && f->code[k + 1].a == in->d)
            {
                pending_label = (uint32_t)f->code[k + 1].imm;
                pending_i = i;
                pending_n = n;
            }
        }
        // What this instruction writes is no longer known to be in range.
        if (in->op == MIR_CALL || in->op == MIR_STORE)
        {
            for (int h = 0; h < nheld; h++)
            {
                if (in->op == MIR_CALL || (held[h].i < o->nslots && o->addressed[held[h].i]) ||
                    held[h].i >= f->frame_size)
                {
                    held[h--] = held[--nheld];
                }
            }
        }
        for (int h = 0; h < nheld; h++)
        {
            if (kills(in, held[h].i) || ((in->op == MIR_MOVE || in->op == MIR_LOADN) && held[h].i >= in->d && held[h].i < in->d + in->imm))
            {
                held[h--] = held[--nheld];
            }
        }
    }
    return changed;
}

// ---- the pass ---------------------------------------------------------------------------------

void mir_optimize(MirFunc *f)
{
    // The slots it may touch: the frame, the call window, and what instructions name.
    uint32_t nslots = f->frame_size + 1;
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        MirSlot top = in->d > in->a ? in->d : in->a;
        top = in->b > top ? in->b : top;
        if (in->op == MIR_ZERO || in->op == MIR_ADDR)
        {
            top = (MirSlot)(in->a + in->imm);
        }
        if (in->op == MIR_MOVE)
        {
            top = (MirSlot)((in->d > in->a ? in->d : in->a) + in->imm);
        }
        if (in->op == MIR_LOADN)
        {
            top = (MirSlot)(in->d + in->imm) > top ? (MirSlot)(in->d + in->imm) : top;
        }
        nslots = top + 1 > nslots ? top + 1 : nslots;
    }
    if (nslots > (1u << 16) || f->n > (1u << 20))
    {
        return; // too large to be worth it here
    }
    Opt o = {f, nslots, NULL, NULL, NULL};
    o.addressed = (uint8_t *)region_push(REGION_MIR, (nslots + 7) & ~(size_t)7);
    o.known = (uint8_t *)region_push(REGION_MIR, (nslots + 7) & ~(size_t)7);
    o.value = (uint64_t *)region_push(REGION_MIR, nslots * sizeof(uint64_t));
    for (int round = 0; round < 8; round++)
    {
        int changed = frame_fields(&o);
        mark_addressed(&o);
        changed |= known_bounds(&o);
        changed |= fold(&o);
        compact(f);
        expand_writes(&o);
        mark_addressed(&o);
        changed |= merge_output(&o);
        changed |= dead_stores(&o);
        mark_addressed(&o);
        changed |= dce(&o);
        compact(f);
        if (!changed)
        {
            break;
        }
    }
}
