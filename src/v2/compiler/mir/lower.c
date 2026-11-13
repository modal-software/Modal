// lower.c (v2): checked tree -> MIR, one function at a time.
//
// Semantics are those of the C backend's output, exactly: an operation of a sized type is done
// in that type; a literal keeps its exact value; an operation on literals only is done in C's
// own types; / and comparisons are done in C's common type of the (promoted) operands; `and` and
// `or` short-circuit. The machine-code backends were tested against the C backend on random
// programs with this lowering (see docs/context.md, 13.2).
//
// Done here, once for every target (the cheap half of what an optimizing JIT does):
//   - constant folding: an expression of literals becomes one constant
//   - immediate operands: `x + 1` is one instruction, not a constant and an addition
//   - results are computed where they are needed: into the local a definition names, into the
//     return slot, into a call's argument slot, instead of into a temporary and then copied
//   - the frame layout is final before any code is generated, so a backend never backpatches
//     frame offsets
#include "v2/compiler/mir/mir.h"
#include <string.h>

#define MAX_LOCALS 512
#define MAX_TRACKED 8192 // slots whose "is a local" bit is kept; beyond, results are copied

#define SLOT_PERM 0x80000000u // constructor values: perm_base + k
#define SLOT_WIN 0x40000000u  // the call window: frame_size + i
#define SLOT_KIND (SLOT_PERM | SLOT_WIN)

typedef struct
{
    Unit *u;
    MirFunc *f;
    int errors;
    uint32_t top, max_top, perm;
    struct
    {
        NameId name;
        MirSlot slot;
    } locals[MAX_LOCALS];
    int nlocals;
    int has_self;   // slot 0 holds self (a reference)
    int struct_abi; // a parameter or the result is a struct value: never inlined
    TypeId ret;
    uint8_t is_local[MAX_TRACKED];
} Lower;

static Lower L;

// ---- module-level variables -------------------------------------------------------------------
//
// A module-level variable (`u8 hits`, `u64 total = 1000`) has no bytes in the binary: it lives
// at the bottom of the value stack, reached through the runtime context, and main starts by
// writing the initial values of all of them.

static uint32_t *global_of; // per symbol: index + 1, or 0
static size_t global_cap;
static uint32_t nglobals;

uint32_t mir_global_count(void)
{
    return nglobals;
}

// Is this symbol a module-level variable (and not a constant that folds)? Its initializer, if
// any, goes to *init (a node of its own file).
static int is_global(SymId s, NodeId *init)
{
    if (!s || SYM(s)->kind != SYM_CONST || SYM(s)->owner)
    {
        return 0;
    }
    const Unit *home = &cx.units[SYM(s)->unit];
    const AstNode *decl = NODE(home, SYM(s)->decl);
    if (decl->kind != AST_FIELD_DECL || !(decl->flags & AST_F_MUT))
    {
        return 0;
    }
    *init = decl->b;
    return 1;
}

static uint32_t global_index(SymId s)
{
    if (global_cap < cx.syms.cap)
    {
        size_t cap = cx.syms.cap;
        uint32_t *grown = (uint32_t *)arena_alloc(cap * sizeof(uint32_t));
        memset(grown, 0, cap * sizeof(uint32_t));
        if (global_cap)
        {
            memcpy(grown, global_of, global_cap * sizeof(uint32_t));
        }
        global_of = grown;
        global_cap = cap;
    }
    if (!global_of[s])
    {
        global_of[s] = ++nglobals;
    }
    return global_of[s] - 1;
}

// ---- inlining ---------------------------------------------------------------------------------
//
// A small function (a few instructions, no call, one return at its end) is kept, and a call to
// it made later is replaced by its body (the JIT's inlining, done ahead of time). The body works
// on the call window, which is exactly the frame a real call would give it, so the arguments
// are already where it reads them and its result is where the caller takes it.
#define INLINE_MAX 16

typedef struct
{
    MirInst *code;
    uint32_t n;
    MirString *strings;
    uint32_t nstrings;
    uint32_t nlabels;
} Inlinable;

static Inlinable **inlinable; // per symbol; NULL: not small, or not lowered yet
static size_t inlinable_cap;

static void keep_if_small(const MirFunc *f)
{
    if (f->n > INLINE_MAX || L.errors || L.struct_abi)
    {
        return;
    }
    for (size_t i = 0; i < f->n; i++)
    {
        uint8_t op = f->code[i].op;
        if (op == MIR_CALL || op == MIR_ADDR || op == MIR_ZERO || (op == MIR_RET && i + 1 != f->n))
        {
            return;
        }
    }
    if (inlinable_cap < cx.syms.size)
    {
        size_t cap = inlinable_cap;
        Inlinable **grown = (Inlinable **)arena_alloc(cx.syms.cap * sizeof(Inlinable *));
        memset(grown, 0, cx.syms.cap * sizeof(Inlinable *));
        if (cap)
        {
            memcpy(grown, inlinable, cap * sizeof(Inlinable *));
        }
        inlinable = grown;
        inlinable_cap = cx.syms.cap;
    }
    // One allocation of the exact size: the header, then the instructions (no final return).
    Inlinable *k = (Inlinable *)arena_alloc(sizeof(Inlinable) + (f->n - 1) * sizeof(MirInst));
    k->n = (uint32_t)f->n - 1;
    k->code = (MirInst *)(k + 1);
    memcpy(k->code, f->code, k->n * sizeof(MirInst));
    k->nstrings = (uint32_t)f->nstrings;
    k->strings = NULL;
    if (f->nstrings)
    {
        k->strings = (MirString *)arena_alloc(f->nstrings * sizeof(MirString));
        for (size_t i = 0; i < f->nstrings; i++)
        {
            uint8_t *bytes = (uint8_t *)arena_alloc(f->strings[i].len + 1);
            memcpy(bytes, f->strings[i].bytes, f->strings[i].len);
            k->strings[i] = (MirString){bytes, f->strings[i].len};
        }
    }
    k->nlabels = f->nlabels;
    inlinable[f->sym] = k;
}

static MirInst *inst(uint8_t op);
static void self_loops(MirFunc *f);

// ---- tail calls -------------------------------------------------------------------------------
//
// `return g(...)` as the last thing a function does, `call g; copy s0.. = window; ret`, becomes
// one MIR_CALL_TAIL: g runs in this function's frame and returns to this function's caller, so
// a recursion through tail calls (every loop of Modal: a clause that calls its function again,
// a clause falling through to the next) runs in constant stack, in every backend.
// Not when the function takes the address of one of its slots (a struct it built, a local
// passed by reference): g's frame takes this one's place, and the address would point into it.
static void mark_tail_calls(MirFunc *f)
{
    for (size_t i = 0; i < f->n; i++)
    {
        if (f->code[i].op == MIR_ADDR)
        {
            return;
        }
    }
    size_t to = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        MirInst in = f->code[i];
        f->code[to++] = in;
        if (in.op != MIR_CALL || in.sub || !SYM(in.imm)->decl || i + 1 + f->ret_slots >= f->n)
        {
            continue;
        }
        uint32_t r = 0;
        while (r < f->ret_slots && f->code[i + 1 + r].op == MIR_COPY && f->code[i + 1 + r].d == r &&
               f->code[i + 1 + r].a == f->frame_size + r)
        {
            r++;
        }
        if (r < f->ret_slots || f->code[i + 1 + r].op != MIR_RET)
        {
            continue;
        }
        uint32_t params = mir_param_slots((SymId)in.imm);
        MirInst *t = &f->code[to - 1];
        t->sub = MIR_CALL_TAIL;
        t->b = params; // the slots to move from the window to the frame's start
        i += 1 + r;    // the copies of the result and the return are the callee's now
    }
    f->n = to;
    self_loops(f);
}

// A tail call of the function to itself is a loop: the arguments move from the window to the
// parameters, and the code goes on at a label before the first instruction. No frame is made
// again and nothing is saved again: in registers, a loop like any other (the native backends'
// allocator sees the branch back). The new code is in the region of the function being lowered.
static void self_loops(MirFunc *f)
{
    size_t extra = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        if (MIR_IS_TAIL(&f->code[i]) && f->code[i].imm == f->sym)
        {
            extra += 1 + f->code[i].b;
        }
    }
    if (!extra)
    {
        return;
    }
    MirInst *code = (MirInst *)region_push(REGION_MIR, (f->n + extra) * sizeof(MirInst));
    uint32_t top = f->nlabels++;
    size_t n = 0;
    code[n++] = (MirInst){.op = MIR_LABEL, .imm = top};
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        if (!(MIR_IS_TAIL(in) && in->imm == f->sym))
        {
            code[n++] = *in;
            continue;
        }
        for (uint32_t j = 0; j < in->b; j++) // ascending: the window is above the parameters
        {
            code[n++] = (MirInst){.op = MIR_COPY, .d = j, .a = f->frame_size + j};
        }
        code[n++] = (MirInst){.op = MIR_JUMP, .imm = top};
    }
    f->code = code;
    f->n = f->cap = n;
}

// ---- pattern clauses as one function (the native backends) --------------------------------------
//
// Each clause is lowered as its own function, and one whose patterns do not match ends with a
// tail call to the next clause, with the same arguments. A loop in Modal is a clause calling its
// function again, the first clause: through two functions on every turn. So a clause with a next
// one is held (copied out of the region) until the next is lowered, then the two become one
// function: the first's code up to its miss label, then the next's. In that function the
// recursion is a call to itself, which self_loops makes a loop. The next clause is still
// generated on its own as well (nothing calls it any more; machine code drops it).
typedef struct
{
    MirFunc f;     // its code and strings are in the arena
    SymId next;    // the clause it waits for
    size_t miss;   // where its miss path starts: the copies to the window, then the tail call
} HeldClause;

#define MAX_HELD 64
static HeldClause held[MAX_HELD];
static int nheld;

// Is f's last act `copy window <- parameters; tail next` (the miss path mir_lower writes)?
// Returns the index of the first copy, or 0.
static size_t miss_path(const MirFunc *f, SymId next)
{
    uint32_t k = f->param_slots;
    if (f->n < (size_t)k + 2 || !MIR_IS_TAIL(&f->code[f->n - 1]) || f->code[f->n - 1].imm != next ||
        f->code[f->n - 1].b != k)
    {
        return 0;
    }
    size_t at = f->n - 1 - k;
    for (uint32_t j = 0; j < k; j++)
    {
        const MirInst *c = &f->code[at + j];
        if (c->op != MIR_COPY || c->d != f->frame_size + j || c->a != j)
        {
            return 0;
        }
    }
    return f->code[at - 1].op == MIR_LABEL ? at : 0;
}

int mir_clause_hold(const MirFunc *f)
{
    SymId next = f->label ? 0 : sema_next_clause(f->u, f->sym);
    size_t at = next ? miss_path(f, next) : 0;
    if (!at || nheld == MAX_HELD)
    {
        return 0;
    }
    HeldClause *h = &held[nheld++];
    h->f = *f;
    h->f.code = (MirInst *)arena_alloc(f->n * sizeof(MirInst));
    memcpy(h->f.code, f->code, f->n * sizeof(MirInst));
    h->f.cap = f->n;
    h->f.strings = NULL;
    h->f.strings_cap = f->nstrings;
    if (f->nstrings)
    {
        h->f.strings = (MirString *)arena_alloc(f->nstrings * sizeof(MirString));
        for (size_t i = 0; i < f->nstrings; i++)
        {
            uint8_t *bytes = (uint8_t *)arena_alloc(f->strings[i].len + 1);
            memcpy(bytes, f->strings[i].bytes, f->strings[i].len);
            h->f.strings[i] = (MirString){bytes, f->strings[i].len};
        }
    }
    h->next = next;
    h->miss = at;
    return 1;
}

// The slots of one function placed in a frame of `frame` slots: its window moves up with it.
static MirSlot reframe(MirSlot s, uint32_t from, uint32_t to)
{
    return s >= from ? s - from + to : s;
}

static void reframe_inst(MirInst *in, uint32_t from, uint32_t to)
{
    switch (in->op)
    {
    case MIR_CONST:
    case MIR_STR:
    case MIR_GLOAD:
        in->d = reframe(in->d, from, to);
        break;
    case MIR_BIN:
        in->d = reframe(in->d, from, to);
        in->a = reframe(in->a, from, to);
        in->b = reframe(in->b, from, to);
        break;
    case MIR_STORE:
        in->a = reframe(in->a, from, to);
        in->b = reframe(in->b, from, to);
        break;
    case MIR_COPY:
    case MIR_ADDR:
    case MIR_LOAD:
    case MIR_BINI:
    case MIR_NEG:
    case MIR_NOT:
    case MIR_NORM:
        in->d = reframe(in->d, from, to);
        in->a = reframe(in->a, from, to);
        break;
    case MIR_ZERO:
    case MIR_JZ:
    case MIR_JNZ:
    case MIR_GSTORE:
    case MIR_TRAP:
        in->a = reframe(in->a, from, to);
        break;
    case MIR_WRITE:
        if (in->sub != MW_SPACE && in->sub != MW_NL)
        {
            in->a = reframe(in->a, from, to);
        }
        break;
    default: // LABEL, CALL, RET, JUMP: no slot
        break;
    }
}

int mir_clause_merge(const MirFunc *g, MirFunc *out)
{
    int i = 0;
    while (i < nheld && held[i].next != g->sym)
    {
        i++;
    }
    if (i == nheld || g->param_slots != held[i].f.param_slots || g->ret_slots != held[i].f.ret_slots)
    {
        return 0;
    }
    HeldClause h = held[i];
    held[i] = held[--nheld];
    const MirFunc *f = &h.f;
    uint32_t frame = f->frame_size > g->frame_size ? f->frame_size : g->frame_size;
    size_t n = h.miss + g->n;
    MirInst *code = (MirInst *)region_push(REGION_MIR, n * sizeof(MirInst));
    for (size_t k = 0; k < h.miss; k++)
    {
        code[k] = f->code[k];
        reframe_inst(&code[k], f->frame_size, frame);
    }
    for (size_t k = 0; k < g->n; k++)
    {
        MirInst in = g->code[k];
        reframe_inst(&in, g->frame_size, frame);
        if (in.op == MIR_LABEL || MIR_BRANCHES(&in))
        {
            in.imm += f->nlabels;
        }
        else if (in.op == MIR_STR)
        {
            in.imm += f->nstrings;
        }
        code[h.miss + k] = in;
    }
    MirString *strings = NULL;
    size_t nstrings = f->nstrings + g->nstrings;
    if (nstrings)
    {
        strings = (MirString *)region_push(REGION_MIR, nstrings * sizeof(MirString));
        memcpy(strings, f->strings, f->nstrings * sizeof(MirString));
        memcpy(strings + f->nstrings, g->strings, g->nstrings * sizeof(MirString));
    }
    *out = *f;
    out->code = code;
    out->n = out->cap = n;
    out->strings = strings;
    out->nstrings = out->strings_cap = nstrings;
    out->nlabels = f->nlabels + g->nlabels;
    out->frame_size = frame;
    self_loops(out);
    return 1;
}

int mir_clause_release(MirFunc *out)
{
    if (!nheld)
    {
        return 0;
    }
    *out = held[--nheld].f;
    return 1;
}

// What cannot be lowered is not reported here: the function is left out, and the reason is
// kept. It becomes an error only if the program reaches the function (see MirFunc.fail_*), so
// a library may hold code the backend cannot translate yet as long as it is not used.
static void unsupported(uint32_t tok, const char *what)
{
    if (!L.errors)
    {
        L.f->fail_tok = tok;
        L.f->fail_what = what;
    }
    L.errors++;
}

static MirSlot into(MirSlot r, MirSlot dst);
static MirSlot temp(void);
static int is_temp(MirSlot s);

static const Inlinable *inlinable_for(SymId fn)
{
    return fn < inlinable_cap && inlinable[fn] ? inlinable[fn] : NULL;
}

static int body_has_labels(const Inlinable *k)
{
    for (uint32_t i = 0; i < k->n; i++)
    {
        if (k->code[i].op == MIR_LABEL)
        {
            return 1;
        }
    }
    return 0;
}

static int inst_writes(const MirInst *in, MirSlot s)
{
    switch (in->op)
    {
    case MIR_CONST:
    case MIR_COPY:
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

// Splices a kept body in place of a call. Its slots become window slots, except that a
// parameter is read straight from the caller's argument slot until the body first writes it
// (only in straight-line bodies: with a branch, the parameter is copied to its window slot).
// Returns the slot holding the result.
static MirSlot splice(const Inlinable *k, const MirSlot *args, int nargs)
{
    MirFunc *f = L.f;
    int branches = body_has_labels(k);
    uint32_t labels = f->nlabels;
    f->nlabels += k->nlabels;
    size_t strings = f->nstrings;
    for (uint32_t i = 0; i < k->nstrings; i++)
    {
        if (f->nstrings == f->strings_cap)
        {
            f->strings = (MirString *)region_grow_array(REGION_MIR, f->strings, sizeof(MirString),
                                                        &f->strings_cap, f->nstrings + 1);
        }
        f->strings[f->nstrings++] = k->strings[i];
    }

    uint8_t written[256] = {0}; // parameters the body has written so far
    for (int p = 0; p < nargs && p < 256; p++)
    {
        int ever = 0;
        for (uint32_t i = 0; i < k->n; i++)
        {
            ever |= inst_writes(&k->code[i], (MirSlot)p);
        }
        if (branches && ever)
        {
            into(args[p], SLOT_WIN | (uint32_t)p); // the body sees a copy it may change
            written[p] = 1;
        }
    }

    for (uint32_t i = 0; i < k->n; i++)
    {
        MirInst in = k->code[i];
        MirSlot ops[2] = {in.a, in.b};
        for (int o = 0; o < 2; o++)
        {
            MirSlot s = ops[o];
            ops[o] = s < (MirSlot)nargs && s < 256 && !written[s] ? args[s] : (SLOT_WIN | s);
        }
        MirInst *out = inst(in.op);
        *out = in;
        out->a = ops[0];
        out->b = ops[1];
        out->d = SLOT_WIN | in.d;
        if (in.d < 256 && (int)in.d < nargs && inst_writes(&in, in.d))
        {
            written[in.d] = 1;
        }
        if (in.op == MIR_LABEL || MIR_BRANCHES(&in))
        {
            out->imm = in.imm + labels;
        }
        else if (in.op == MIR_STR)
        {
            out->imm = in.imm + strings;
        }
    }
    // A result computed by the body's last instruction goes to a temporary: the caller can then
    // aim it straight at its destination.
    MirInst *last = f->n ? &f->code[f->n - 1] : NULL;
    if (k->n && last->d == (SLOT_WIN | 0) && inst_writes(&k->code[k->n - 1], 0))
    {
        last->d = temp();
        return last->d;
    }
    return nargs > 0 && !written[0] ? args[0] : (SLOT_WIN | 0);
}

// ---- building ---------------------------------------------------------------------------------

static MirInst *inst(uint8_t op)
{
    MirFunc *f = L.f;
    if (f->n == f->cap)
    {
        f->code = (MirInst *)region_grow_array(REGION_MIR, f->code, sizeof(MirInst), &f->cap,
                                               f->n + 1);
    }
    MirInst *i = &f->code[f->n++];
    *i = (MirInst){.op = op};
    return i;
}

static MirSlot temp(void)
{
    MirSlot s = L.top++;
    if (L.top > L.max_top)
    {
        L.max_top = L.top;
    }
    return s;
}

// k consecutive slots: a struct value.
static MirSlot temp_n(uint32_t k)
{
    MirSlot s = L.top;
    L.top += k;
    if (L.top > L.max_top)
    {
        L.max_top = L.top;
    }
    return s;
}

static int is_struct(TypeId t)
{
    return type_kind(t) == T_STRUCT;
}

// The frame slots a value of type t takes.
static uint32_t slots_of(TypeId t)
{
    if (!is_struct(t))
    {
        return 1;
    }
    Layout l = type_layout(t);
    return l.size ? (l.size + 7) / 8 : 1;
}

// How a value of type t is read from and written to memory.
static int int_variant(TypeId t);
static int mem_variant(TypeId t)
{
    int v = int_variant(t);
    return v >= 0 ? v : MV_U64; // a pointer or a string
}

static int is_temp(MirSlot s)
{
    return !(s & SLOT_KIND) && s < MAX_TRACKED && !L.is_local[s] && !(L.has_self && s == 0);
}

static int produces_value(uint8_t op)
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
        return 1;
    default:
        return 0;
    }
}

// Puts the value in slot `r` into `dst`. When `r` is a temporary that the last instruction just
// computed, that instruction computes into `dst` instead, and nothing is copied.
static MirSlot into(MirSlot r, MirSlot dst)
{
    if (r == dst)
    {
        return dst;
    }
    MirFunc *f = L.f;
    if (f->n && is_temp(r) && f->code[f->n - 1].d == r && produces_value(f->code[f->n - 1].op))
    {
        f->code[f->n - 1].d = dst;
        return dst;
    }
    MirInst *i = inst(MIR_COPY);
    i->d = dst;
    i->a = r;
    return dst;
}

static MirSlot emit_const(uint64_t v)
{
    MirInst *i = inst(MIR_CONST);
    i->d = temp();
    i->imm = v;
    return i->d;
}

static uint32_t new_label(void)
{
    return L.f->nlabels++;
}

static void place_label(uint32_t label)
{
    inst(MIR_LABEL)->imm = label;
}

static void push_local_n(NameId name, MirSlot slot, uint32_t k);
static void push_local(NameId name, MirSlot slot)
{
    push_local_n(name, slot, 1);
}

static void push_local_n(NameId name, MirSlot slot, uint32_t k)
{
    if (L.nlocals == MAX_LOCALS)
    {
        fprintf(stderr, "fatal: too many locals in one function\n");
        exit(2);
    }
    L.locals[L.nlocals].name = name;
    L.locals[L.nlocals].slot = slot;
    L.nlocals++;
    for (uint32_t j = 0; j < k; j++)
    {
        if (slot + j < MAX_TRACKED)
        {
            L.is_local[slot + j] = 1;
        }
    }
}

static int find_local(NameId name, MirSlot *slot)
{
    for (int i = L.nlocals - 1; i >= 0; i--)
    {
        if (L.locals[i].name == name)
        {
            *slot = L.locals[i].slot;
            return 1;
        }
    }
    return 0;
}

// ---- variants: the C types of the C backend's output ------------------------------------------

static int int_variant(TypeId t)
{
    switch (t)
    {
    case T_U8:
    case T_BOOL:
        return MV_U8;
    case T_U16:
        return MV_U16;
    case T_U32:
        return MV_U32;
    case T_U64:
        return MV_U64;
    case T_I8:
        return MV_I8;
    case T_I16:
        return MV_I16;
    case T_I32:
    case T_INT:
    case T_INT_LIT:
        return MV_I32;
    case T_I64:
        return MV_I64;
    default:
        return -1;
    }
}

uint64_t mir_normalize(uint64_t v, int variant)
{
    switch (variant)
    {
    case MV_U8:
        return (uint8_t)v;
    case MV_U16:
        return (uint16_t)v;
    case MV_U32:
        return (uint32_t)v;
    case MV_I8:
        return (uint64_t)(int64_t)(int8_t)v;
    case MV_I16:
        return (uint64_t)(int64_t)(int16_t)v;
    case MV_I32:
        return (uint64_t)(int64_t)(int32_t)v;
    default:
        return v;
    }
}

static int is_signed(int v)
{
    return v >= MV_I8;
}

// C's integer promotions: everything narrower than int is done in int.
static int promoted(int v)
{
    return v == MV_U8 || v == MV_U16 || v == MV_I8 || v == MV_I16 ? MV_I32 : v;
}

// C's usual arithmetic conversions on two promoted types (int, unsigned, long, unsigned long).
static int common_variant(int a, int b)
{
    if (a < 0 || b < 0)
    {
        return -1;
    }
    if (a == b)
    {
        return a;
    }
    int rank_a = a == MV_I64 || a == MV_U64 ? 2 : 1;
    int rank_b = b == MV_I64 || b == MV_U64 ? 2 : 1;
    int unsigned_a = a == MV_U32 || a == MV_U64, unsigned_b = b == MV_U32 || b == MV_U64;
    if (unsigned_a == unsigned_b)
    {
        return rank_a >= rank_b ? a : b;
    }
    int u = unsigned_a ? a : b, s = unsigned_a ? b : a;
    int rank_u = unsigned_a ? rank_a : rank_b, rank_s = unsigned_a ? rank_b : rank_a;
    return rank_u >= rank_s ? u : s; // a long holds every unsigned int
}

static uint64_t literal_value(const Token *t)
{
    uint64_t v = 0;
    number_value(TSTART(t), t->len, &v, NULL);
    return v;
}

// The C type a value has in the C backend's output. Operations of a sized type are cast to it;
// literals are written as they are, and operations on literals only are not cast, so there C's
// own rules decide (a literal is the first of int, long that holds it; unsigned long beyond).
static int c_variant(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    if (u->types[id] != T_INT_LIT)
    {
        return int_variant(u->types[id]);
    }
    uint64_t bits;
    int neg;
    switch (n->kind)
    {
    case AST_NUMBER_LIT:
    {
        uint64_t v = literal_value(TOK(u, n->tok));
        return v <= 0x7fffffffu ? MV_I32 : v <= 0x7fffffffffffffffull ? MV_I64 : MV_U64;
    }
    case AST_IDENT:
    case AST_SELECTOR:
        // an unsized constant: the variant a literal of its value has (-7 is an int, as `-7` is)
        if (sema_unsized_value(u, id, &bits, &neg))
        {
            return neg ? ((int64_t)bits >= -0x80000000ll ? MV_I32 : MV_I64)
                       : (bits <= 0x7fffffffu ? MV_I32 : bits <= 0x7fffffffffffffffull ? MV_I64 : MV_U64);
        }
        return MV_I32;
    case AST_PAREN_GROUP:
        return c_variant(n->a);
    case AST_UNARY_OP:
        return promoted(c_variant(n->a));
    case AST_BIN_OP:
        return common_variant(promoted(c_variant(n->a)), promoted(c_variant(n->b)));
    default:
        return MV_I32;
    }
}

static int operand_variant(NodeId id)
{
    int v = c_variant(id);
    return v < 0 ? -1 : promoted(v);
}

// ---- binary operations ------------------------------------------------------------------------

static const struct
{
    const char *text;
    uint8_t op;
    uint8_t arith; // + - * wrap in the result type
    uint8_t compare;
} binops[] = {
    {"+", MB_ADD, 1, 0}, {"-", MB_SUB, 1, 0}, {"*", MB_MUL, 1, 0}, {"/", MB_DIV, 0, 0},
    {"==", MB_EQ, 0, 1}, {"!=", MB_NE, 0, 1}, {"<", MB_LT, 0, 1},  {"<=", MB_LE, 0, 1},
    {">", MB_GT, 0, 1},  {">=", MB_GE, 0, 1},
};

typedef struct
{
    int index;  // in binops, -1: not an operator we lower
    int var;    // the variant the operation is done in
    int result; // the variant the result is converted to after a division, -1: none
} BinPlan;

static BinPlan plan_binop(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    BinPlan p = {-1, -1, -1};
    for (size_t i = 0; i < sizeof(binops) / sizeof(binops[0]); i++)
    {
        // (strncmp stops at the table text's end, so text[len] is in bounds when it matched)
        if (strncmp(binops[i].text, TSTART(t), t->len) == 0 && binops[i].text[t->len] == '\0')
        {
            p.index = (int)i;
            break;
        }
    }
    if (p.index < 0)
    {
        return p;
    }
    TypeId type = u->types[id];
    if (binops[p.index].arith && type != T_INT_LIT)
    {
        p.var = int_variant(type);
    }
    else
    {
        p.var = common_variant(operand_variant(n->a), operand_variant(n->b));
    }
    int want = int_variant(type);
    if (!binops[p.index].arith && !binops[p.index].compare && want >= 0 && want != p.var &&
        type != T_INT_LIT)
    {
        p.result = want; // a division done in a wider type, back to its own
    }
    return p;
}

// a <op> b in variant v, as the generated code computes it. Returns 0 if it must not be folded
// (a division by zero, or one that overflows: they are left for the program to do).
static int compute(int op, int v, uint64_t a, uint64_t b, uint64_t *out)
{
    a = mir_normalize(a, v);
    b = mir_normalize(b, v);
    int s = is_signed(v);
    switch (op)
    {
    case MB_ADD:
        *out = mir_normalize(a + b, v);
        return 1;
    case MB_SUB:
        *out = mir_normalize(a - b, v);
        return 1;
    case MB_MUL:
        *out = mir_normalize(a * b, v);
        return 1;
    case MB_DIV:
        if (b == 0 || (s && (int64_t)b == -1))
        {
            return 0;
        }
        *out = mir_normalize(s ? (uint64_t)((int64_t)a / (int64_t)b) : a / b, v);
        return 1;
    case MB_EQ:
        *out = a == b;
        return 1;
    case MB_NE:
        *out = a != b;
        return 1;
    case MB_LT:
        *out = s ? (int64_t)a < (int64_t)b : a < b;
        return 1;
    case MB_LE:
        *out = s ? (int64_t)a <= (int64_t)b : a <= b;
        return 1;
    case MB_GT:
        *out = s ? (int64_t)a > (int64_t)b : a > b;
        return 1;
    default:
        *out = s ? (int64_t)a >= (int64_t)b : a >= b;
        return 1;
    }
}

int mir_compute(int op, int v, uint64_t a, uint64_t b, uint64_t *out)
{
    return compute(op, v, a, b, out);
}

// The value of an expression made of literals only, as the generated code would compute it.
static int const_value(NodeId id, uint64_t *out)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    switch (n->kind)
    {
    case AST_NUMBER_LIT:
    {
        const Token *t = TOK(u, n->tok);
        if (memchr(TSTART(t), '.', t->len))
        {
            return 0;
        }
        uint64_t v = literal_value(t);
        int variant = u->types[id] == T_INT_LIT ? -1 : int_variant(u->types[id]);
        *out = variant >= 0 ? mir_normalize(v, variant) : v;
        return 1;
    }
    case AST_PAREN_GROUP:
        return const_value(n->a, out);
    case AST_CALL:
    {
        int v = u->types[id] == T_INT_LIT ? -1 : int_variant(u->types[id]);
        if (sema_primitive(u, id) == PRIM_CONVERT && v >= 0 && n->b && const_value(n->b, out))
        {
            *out = mir_normalize(*out, v); // u8(300) is 44, as the program computes it
            return 1;
        }
        if (!sema_sizeof(u, id, out))
        {
            return 0;
        }
        *out = v >= 0 ? mir_normalize(*out, v) : *out; // unsized, as a literal is
        return 1;
    }
    case AST_SELECTOR: // `util.limit`: a constant of another module
    case AST_IDENT:
    {
        // A module-level constant with a literal initializer is a constant of its own type.
        // It may live in another file: its declaration is read there.
        SymId s = u->refs[id];
        if (!s || SYM(s)->kind != SYM_CONST)
        {
            return 0;
        }
        const Unit *home = &cx.units[SYM(s)->unit];
        const AstNode *decl = NODE(home, SYM(s)->decl);
        NodeId init = decl->kind == AST_FIELD_DECL ? decl->b : decl->a;
        int v = int_variant(u->types[id]);
        uint64_t computed;
        int neg;
        if (sema_unsized_value(u, id, &computed, &neg))
        {
            *out = computed; // unsized: what it meets converts it, as for a literal
            return 1;
        }
        if (v >= 0 && !(decl->kind == AST_FIELD_DECL && (decl->flags & AST_F_MUT)) &&
            sema_comptime_value(s, &computed)) // computed by the compiler (docs/comptime.md)
        {
            *out = mir_normalize(computed, v);
            return 1;
        }
        if (init && NODE(home, init)->kind == AST_IDENT && v >= 0 &&
            !(decl->kind == AST_FIELD_DECL && (decl->flags & AST_F_MUT)))
        {
            const Token *b = TOK(home, NODE(home, init)->tok); // `has_output := true`
            if ((b->len == 4 && memcmp(TSTART(b), "true", 4) == 0) || (b->len == 5 && memcmp(TSTART(b), "false", 5) == 0))
            {
                *out = b->len == 4;
                return 1;
            }
        }
        if (!init || NODE(home, init)->kind != AST_NUMBER_LIT || v < 0 ||
            (decl->kind == AST_FIELD_DECL && (decl->flags & AST_F_MUT)))
        {
            return 0;
        }
        const Token *lit = TOK(home, NODE(home, init)->tok);
        if (memchr(TSTART(lit), '.', lit->len))
        {
            return 0;
        }
        *out = mir_normalize(literal_value(lit), v);
        return 1;
    }
    case AST_UNARY_OP:
    {
        uint64_t a;
        int v = u->types[id] == T_INT_LIT ? c_variant(id) : int_variant(u->types[id]);
        if (TOK(u, n->tok)->kind != TOK_MINUS || v < 0 || !const_value(n->a, &a))
        {
            return 0;
        }
        *out = mir_normalize(0 - a, v);
        return 1;
    }
    case AST_BIN_OP:
    {
        uint64_t a, b;
        BinPlan p = plan_binop(id);
        if (p.index < 0 || p.var < 0 || !const_value(n->a, &a) || !const_value(n->b, &b) ||
            !compute(binops[p.index].op, p.var, a, b, out))
        {
            return 0;
        }
        if (p.result >= 0)
        {
            *out = mir_normalize(*out, p.result);
        }
        return 1;
    }
    default:
        return 0;
    }
}

// ---- expressions -------------------------------------------------------------------------------

static MirSlot lower_expr(NodeId id);
static MirSlot lower_call(NodeId id);
static MirSlot lower_struct_addr(NodeId id);
static void lower_struct_into(NodeId id, MirSlot dst);

static int is_self_ident(NodeId id)
{
    const AstNode *n = NODE(L.u, id);
    const Token *t = TOK(L.u, n->tok);
    return n->kind == AST_IDENT && is_self_token(TSTART(t), (int)t->len);
}

// Does evaluating this expression call anything? (A call reuses the window.)
static int contains_call(NodeId id)
{
    if (!id)
    {
        return 0;
    }
    const AstNode *n = NODE(L.u, id);
    if (n->kind == AST_CALL)
    {
        return 1;
    }
    switch (n->kind)
    {
    case AST_BIN_OP:
    case AST_ASSIGN:
        return contains_call(n->a) || contains_call(n->b);
    case AST_UNARY_OP:
    case AST_PAREN_GROUP:
    case AST_SELECTOR:
        return contains_call(n->a);
    default:
        return 0;
    }
}

// The value of `id` where a value of type `want` is expected: converted between integer types,
// and a struct given where a reference is wanted passes its reference.
static MirSlot lower_match(NodeId id, TypeId want);

static MirSlot lower_as(NodeId id, TypeId want)
{
    Unit *u = L.u;
    TypeId have = u->types[id];
    if (NODE(u, id)->kind == AST_MATCH && have != T_VOID) // each arm's value made in `want`
    {
        return lower_match(id, want);
    }
    if (type_kind(want) == T_PTR && type_kind(have) == T_STRUCT)
    {
        return lower_struct_addr(id);
    }
    int wv = int_variant(want), hv = c_variant(id);
    uint64_t k;
    if (wv >= 0 && want != T_BOOL && const_value(id, &k))
    {
        return emit_const(mir_normalize(k, wv)); // converted at compile time
    }
    MirSlot slot = lower_expr(id);
    if (want != have && wv >= 0 && hv >= 0 && want != T_BOOL && wv != hv)
    {
        MirInst *i = inst(MIR_NORM);
        i->var = (uint8_t)wv;
        i->d = temp();
        i->a = slot;
        return i->d;
    }
    return slot;
}

// A string literal's bytes, its escapes decoded (as lower_string does), into out; at most cap.
static size_t decode_string(const Token *t, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = 1; i + 1 < t->len && n < cap; i++)
    {
        char ch = TSTART(t)[i];
        if (ch == '\\' && i + 2 < t->len)
        {
            char e = TSTART(t)[++i];
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e;
        }
        out[n++] = (uint8_t)ch;
    }
    return n;
}

static MirSlot lower_string(const Token *t)
{
    MirFunc *f = L.f;
    // rounded to 8 bytes: the arrays that follow in this region stay aligned
    uint8_t *bytes = (uint8_t *)region_push(REGION_MIR, (t->len + 8 + 7) & ~(size_t)7);
    size_t n = 0;
    for (uint32_t i = 1; i + 1 < t->len; i++)
    {
        char ch = TSTART(t)[i];
        if (ch == '\\' && i + 2 < t->len)
        {
            char e = TSTART(t)[++i];
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e;
        }
        bytes[n++] = (uint8_t)ch;
    }
    if (f->nstrings == f->strings_cap)
    {
        f->strings = (MirString *)region_grow_array(REGION_MIR, f->strings, sizeof(MirString),
                                                    &f->strings_cap, f->nstrings + 1);
    }
    f->strings[f->nstrings] = (MirString){bytes, n};
    MirInst *i = inst(MIR_STR);
    i->d = temp();
    i->imm = f->nstrings++;
    return i->d;
}

// Output text known at compile time (literal strings, constant numbers and booleans, the
// separators of print) is gathered and written as one string: one runtime call, not several.
static int contains_call(NodeId id);
static uint8_t pending[4096];
static size_t npending;

// ---- output through the program's `out` -------------------------------------------------------
//
// A call marked AST_F_SINK sends its bytes to `out.put(b)`, one call per byte, in order
// (sema/check.c, route_output). The loops are written here in MIR: a string is its length and
// bytes; a number is turned into digits in a 24-byte block, last digit first.
static MirSlot sink_slot; // the reference to the `out`
static SymId sink_put;    // its put method: 0 when output goes to the system

static MirInst *bini(uint8_t op, int var, MirSlot d, MirSlot a, uint64_t imm)
{
    MirInst *i = inst(MIR_BINI);
    i->sub = op;
    i->var = (uint8_t)var;
    i->d = d;
    i->a = a;
    i->imm = imm;
    return i;
}

static void sink_byte(MirSlot b)
{
    MirInst *c = inst(MIR_COPY);
    c->d = SLOT_WIN | 0;
    c->a = sink_slot;
    c = inst(MIR_COPY);
    c->d = SLOT_WIN | 1;
    c->a = b;
    inst(MIR_CALL)->imm = sink_put;
}

// The n bytes at p (both temporaries, used up).
static void sink_bytes(MirSlot p, MirSlot n)
{
    uint32_t top = new_label(), end = new_label();
    MirInst *j = inst(MIR_JZ);
    j->a = n;
    j->imm = end;
    place_label(top);
    MirSlot b = temp();
    MirInst *ld = inst(MIR_LOAD);
    ld->var = MV_U8;
    ld->d = b;
    ld->a = p;
    sink_byte(b);
    bini(MB_ADD, MV_U64, p, p, 1);
    bini(MB_SUB, MV_U64, n, n, 1);
    j = inst(MIR_JNZ);
    j->a = n;
    j->imm = top;
    place_label(end);
}

static void sink_string(MirSlot s)
{
    MirSlot n = temp(), p = temp();
    MirInst *ld = inst(MIR_LOAD);
    ld->var = MV_U64;
    ld->d = n;
    ld->a = s;
    bini(MB_ADD, MV_U64, p, s, 8);
    sink_bytes(p, n);
}

static void sink_number(MirSlot v, int is_signed)
{
    MirSlot u = temp();
    MirInst *c = inst(MIR_COPY);
    c->d = u;
    c->a = v;
    if (is_signed)
    {
        uint32_t positive = new_label();
        MirSlot neg = temp();
        bini(MB_LT, MV_I64, neg, u, 0);
        MirInst *j = inst(MIR_JZ);
        j->a = neg;
        j->imm = positive;
        MirInst *k = inst(MIR_CONST);
        k->d = neg;
        k->imm = '-';
        sink_byte(neg);
        MirInst *ng = inst(MIR_NEG);
        ng->var = MV_I64;
        ng->d = u;
        ng->a = u; // as u64 from here: the most negative value is fine too
        place_label(positive);
    }
    // 24 bytes in the constructor area, kept for the frame: the digits, last first.
    MirSlot block = SLOT_PERM | L.perm;
    L.perm += 3;
    MirSlot end = temp(), p = temp(), q = temp(), t = temp();
    MirInst *a = inst(MIR_ADDR);
    a->d = end;
    a->a = block;
    a->imm = 3;
    bini(MB_ADD, MV_U64, end, end, 24);
    c = inst(MIR_COPY);
    c->d = p;
    c->a = end;
    uint32_t top = new_label();
    place_label(top);
    bini(MB_DIV, MV_U64, q, u, 10);
    bini(MB_MUL, MV_U64, t, q, 10);
    MirInst *sub = inst(MIR_BIN);
    sub->sub = MB_SUB;
    sub->var = MV_U64;
    sub->d = t;
    sub->a = u;
    sub->b = t;
    bini(MB_ADD, MV_U64, t, t, '0');
    bini(MB_SUB, MV_U64, p, p, 1);
    MirInst *st = inst(MIR_STORE);
    st->var = MV_U8;
    st->a = p;
    st->b = t;
    c = inst(MIR_COPY);
    c->d = u;
    c->a = q;
    MirInst *j = inst(MIR_JNZ);
    j->a = u;
    j->imm = top;
    MirSlot n = temp();
    MirInst *len = inst(MIR_BIN);
    len->sub = MB_SUB;
    len->var = MV_U64;
    len->d = n;
    len->a = end;
    len->b = p;
    sink_bytes(p, n);
}

static MirSlot string_slot(const uint8_t *bytes, size_t len);

static void sink_bool(MirSlot v)
{
    MirSlot s = string_slot((const uint8_t *)"false", 5);
    uint32_t done = new_label();
    MirInst *j = inst(MIR_JZ);
    j->a = v;
    j->imm = done;
    MirSlot t = string_slot((const uint8_t *)"true", 4);
    MirInst *c = inst(MIR_COPY);
    c->d = s;
    c->a = t;
    place_label(done);
    sink_string(s);
}

// A string known now, as a slot holding its address.
static MirSlot string_slot(const uint8_t *src, size_t len)
{
    MirFunc *f = L.f;
    uint8_t *bytes = (uint8_t *)region_push(REGION_MIR, (len + 7) & ~(size_t)7);
    memcpy(bytes, src, len);
    if (f->nstrings == f->strings_cap)
    {
        f->strings = (MirString *)region_grow_array(REGION_MIR, f->strings, sizeof(MirString),
                                                    &f->strings_cap, f->nstrings + 1);
    }
    f->strings[f->nstrings] = (MirString){bytes, len};
    MirInst *i = inst(MIR_STR);
    i->d = temp();
    i->imm = f->nstrings++;
    return i->d;
}

// ---- shared output routines -------------------------------------------------------------------
//
// The loops above are written once per program, not at every call: a routine per `put` method
// and kind of value (a string, an unsigned or signed number, a bool), a function the compiler
// makes (frame: the `out` in slot 0, the value in slot 1). A call to it is three instructions.
enum
{
    SINK_STR,
    SINK_UINT,
    SINK_INT,
    SINK_BOOL,
};
static const char *const sink_labels[] = {"out.string", "out.unsigned", "out.signed", "out.bool"};

#define MAX_SINK_HELPERS 256
typedef struct
{
    SymId put, helper;
    uint8_t kind, lowered;
} SinkHelper;
static SinkHelper sink_helpers[MAX_SINK_HELPERS];
static int nsink_helpers;

static SymId sink_helper(int kind)
{
    for (int i = 0; i < nsink_helpers; i++)
    {
        if (sink_helpers[i].put == sink_put && sink_helpers[i].kind == kind)
        {
            return sink_helpers[i].helper;
        }
    }
    if (nsink_helpers == MAX_SINK_HELPERS)
    {
        return 0; // written inline instead
    }
    Unit *home = &cx.units[SYM(sink_put)->unit];
    SymId h = symbol_new(home, SYM_METHOD, 0, SYM(sink_put)->tok, 0, 0);
    sink_helpers[nsink_helpers++] = (SinkHelper){sink_put, h, (uint8_t)kind, 0};
    return h;
}

static void sink_string(MirSlot s);
static void sink_number(MirSlot v, int is_signed);
static void sink_bool(MirSlot v);

// out.<kind>(value) through the shared routine.
static void sink_value(int kind, MirSlot v)
{
    SymId h = sink_helper(kind);
    if (!h)
    {
        kind == SINK_STR ? sink_string(v) : kind == SINK_BOOL ? sink_bool(v) : sink_number(v, kind == SINK_INT);
        return;
    }
    MirInst *c = inst(MIR_COPY);
    c->d = SLOT_WIN | 0;
    c->a = sink_slot;
    c = inst(MIR_COPY);
    c->d = SLOT_WIN | 1;
    c->a = v;
    inst(MIR_CALL)->imm = h;
}

static void finish_frame(MirFunc *out);

int mir_next_helper(MirFunc *out)
{
    for (int i = 0; i < nsink_helpers; i++)
    {
        if (sink_helpers[i].lowered)
        {
            continue;
        }
        sink_helpers[i].lowered = 1;
        region_end(REGION_MIR);
        region_set_end(REGION_MIR, cx.regions[REGION_MIR].base);
        SymId put = sink_helpers[i].put;
        L.u = &cx.units[SYM(put)->unit];
        L.f = out;
        L.errors = 0;
        L.top = L.max_top = 2; // slot 0: the `out`, slot 1: the value
        L.perm = 0;
        L.nlocals = 0;
        L.ret = T_VOID;
        L.has_self = 0;
        L.struct_abi = 1; // never inlined: it is the point of having it once
        *out = (MirFunc){.u = L.u, .sym = sink_helpers[i].helper, .label = sink_labels[sink_helpers[i].kind],
                         .param_slots = 2};
        sink_slot = 0;
        sink_put = put;
        int kind = sink_helpers[i].kind;
        kind == SINK_STR ? sink_string(1) : kind == SINK_BOOL ? sink_bool(1) : sink_number(1, kind == SINK_INT);
        sink_put = 0;
        inst(MIR_RET);
        finish_frame(out);
        return 1;
    }
    nsink_helpers = 0; // all handed over: the next program starts afresh
    return 0;
}

static void flush_pending(void)
{
    if (!npending)
    {
        return;
    }
    if (sink_put)
    {
        uint32_t top = L.top;
        sink_value(SINK_STR, string_slot(pending, npending));
        L.top = top;
        npending = 0;
        return;
    }
    MirFunc *f = L.f;
    uint8_t *bytes = (uint8_t *)region_push(REGION_MIR, (npending + 7) & ~(size_t)7);
    memcpy(bytes, pending, npending);
    if (f->nstrings == f->strings_cap)
    {
        f->strings = (MirString *)region_grow_array(REGION_MIR, f->strings, sizeof(MirString),
                                                    &f->strings_cap, f->nstrings + 1);
    }
    f->strings[f->nstrings] = (MirString){bytes, npending};
    MirSlot d = temp();
    MirInst *i = inst(MIR_STR);
    i->d = d;
    i->imm = f->nstrings++;
    MirInst *w = inst(MIR_WRITE);
    w->sub = MW_STR;
    w->a = d;
    npending = 0;
}

static void add_pending(const char *p, size_t n)
{
    while (n)
    {
        if (npending == sizeof(pending))
        {
            flush_pending();
        }
        size_t k = sizeof(pending) - npending < n ? sizeof(pending) - npending : n;
        memcpy(pending + npending, p, k);
        npending += k;
        p += k;
        n -= k;
    }
}

// Is this output argument known now? Then its text is added to the pending output.
static int constant_text(NodeId a)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, a);
    const Token *t = TOK(u, n->tok);
    TypeId type = u->types[a];
    if (n->kind == AST_STRING_LIT)
    {
        for (uint32_t i = 1; i + 1 < t->len; i++)
        {
            char ch = TSTART(t)[i];
            if (ch == '\\' && i + 2 < t->len)
            {
                char e = TSTART(t)[++i];
                ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e;
            }
            add_pending(&ch, 1);
        }
        return 1;
    }
    if (type == T_BOOL && n->kind == AST_IDENT && !u->refs[a] &&
        ((t->len == 4 && memcmp(TSTART(t), "true", 4) == 0) ||
         (t->len == 5 && memcmp(TSTART(t), "false", 5) == 0)))
    {
        add_pending(TSTART(t), t->len);
        return 1;
    }
    uint64_t v;
    if (type_is_int(type) && const_value(a, &v))
    {
        // printed as the generated code would: unsigned for u64, signed otherwise
        int variant = type == T_INT_LIT ? c_variant(a) : int_variant(type);
        char digits[24];
        int k = 24;
        int negative = type != T_U64 && variant != MV_U64 && (int64_t)v < 0;
        uint64_t m = negative ? 0 - v : v;
        do
        {
            digits[--k] = (char)('0' + m % 10);
            m /= 10;
        } while (m);
        if (negative)
        {
            digits[--k] = '-';
        }
        add_pending(digits + k, (size_t)(24 - k));
        return 1;
    }
    return 0;
}

static MirSlot lower_output(NodeId args, int newline, SymId put)
{
    Unit *u = L.u;
    npending = 0;
    sink_put = 0;
    if (put) // the first argument is the `out` the bytes go to
    {
        MirSlot r = lower_as(args, type_make(T_PTR, type_make(T_STRUCT, SYM(put)->owner)));
        sink_slot = temp();
        into(r, sink_slot);
        sink_put = put;
        args = NEXT(u, args);
    }
    for (NodeId a = args; a; a = NEXT(u, a))
    {
        if (newline && a != args)
        {
            add_pending(" ", 1);
        }
        if (constant_text(a))
        {
            continue;
        }
        TypeId t = u->types[a];
        uint32_t top = L.top;
        if (contains_call(a))
        {
            flush_pending(); // the call may write too: the text before it goes out first
        }
        MirSlot s = lower_expr(a);
        uint8_t sub;
        if (t == T_STR)
        {
            sub = MW_STR;
        }
        else if (t == T_BOOL)
        {
            sub = MW_BOOL;
        }
        else if (t == T_U64)
        {
            sub = MW_UINT;
        }
        else if (type_is_int(t))
        {
            sub = MW_INT;
        }
        else
        {
            unsupported(NODE(u, a)->tok, "printing a value of this type");
            L.top = top;
            continue;
        }
        flush_pending();
        if (sink_put)
        {
            sink_value(sub == MW_STR    ? SINK_STR
                       : sub == MW_BOOL ? SINK_BOOL
                       : sub == MW_INT  ? SINK_INT
                                        : SINK_UINT,
                       s);
            L.top = top;
            continue;
        }
        MirInst *i = inst(MIR_WRITE);
        i->a = s;
        i->sub = sub;
        L.top = top;
    }
    if (newline)
    {
        add_pending("\n", 1);
    }
    flush_pending();
    sink_put = 0;
    return temp();
}

static uint32_t struct_slots(SymId s)
{
    return slots_of(type_make(T_STRUCT, s));
}

// Fields are where C puts them (sema/layout.c), read and written in their own width.
// A field placed with `@`: a device register. Its loads and stores are marked MIR_MEM_DEVICE.
static int field_is_register(SymId field)
{
    const Unit *home = &cx.units[SYM(field)->unit];
    return SYM(field)->decl && (NODE(home, SYM(field)->decl)->flags & AST_F_PLACED) != 0;
}

static int struct_is_registers(TypeId t)
{
    if (type_kind(t) != T_STRUCT)
    {
        return 0;
    }
    SymId s = type_ref(t);
    const Unit *home = &cx.units[SYM(s)->unit];
    for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind == AST_FIELD_DECL && (NODE(home, m)->flags & AST_F_PLACED))
        {
            return 1;
        }
    }
    return 0;
}

static uint64_t field_offset(SymId field)
{
    return field_offset_of(field);
}

static TypeId field_type(SymId field)
{
    return sema_value_type(&cx.units[SYM(field)->unit], field);
}

// k slots from src to dst, in the order that is safe when the blocks overlap.
static void copy_slots(MirSlot dst, MirSlot src, uint32_t k)
{
    if (dst == src)
    {
        return;
    }
    for (uint32_t i = 0; i < k; i++)
    {
        uint32_t j = dst > src ? k - 1 - i : i;
        MirInst *c = inst(MIR_COPY);
        c->d = dst + j;
        c->a = src + j;
    }
}

// A string or struct value the compiler computed (docs/comptime.md): its numbers as constants,
// its strings as the program's own strings.
static MirSlot computed_string(const ComptimeAggregate *c)
{
    return string_slot(c->str[0], c->len[0]);
}

static void computed_struct_into(const ComptimeAggregate *c, MirSlot dst)
{
    for (uint32_t j = 0; j < c->n; j++)
    {
        into(c->str[j] ? string_slot(c->str[j], c->len[j]) : emit_const(c->slots[j]), dst + j);
    }
}

// The struct value `id` into the k slots at dst.
static void lower_struct_into(NodeId id, MirSlot dst)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    TypeId t = u->types[id];
    uint32_t k = slots_of(t);
    MirSlot base;
    if (!type_layout(t).size)
    {
        unsupported(n->tok, "a struct with a field that has no layout yet");
        return;
    }
    if (n->kind == AST_PAREN_GROUP)
    {
        lower_struct_into(n->a, dst);
        return;
    }
    if (is_self_ident(id) && L.has_self)
    {
        // Read through the reference in slot 0, last slot first: when the value goes to the
        // result (dst = 0), the reference is overwritten only by the last read.
        for (uint32_t j = k; j-- > 0;)
        {
            MirInst *i = inst(MIR_LOAD);
            i->var = MV_U64;
            i->sub = struct_is_registers(t) ? MIR_MEM_DEVICE : 0;
            i->d = dst + j;
            i->a = 0;
            i->imm = 8 * j;
        }
        return;
    }
    if (n->kind == AST_IDENT && !u->refs[id] && find_local(token_name(TOK(u, n->tok)), &base))
    {
        copy_slots(dst, base, k);
        return;
    }
    if (n->kind == AST_CALL)
    {
        copy_slots(dst, lower_call(id), k); // the result is in the window: taken at once
        return;
    }
    const ComptimeAggregate *computed = (n->kind == AST_IDENT || n->kind == AST_SELECTOR) && u->refs[id]
                                            ? sema_comptime_aggregate(u->refs[id])
                                            : NULL;
    if (computed && computed->n == k)
    {
        computed_struct_into(computed, dst); // a module-level struct the compiler computed
        return;
    }
    unsupported(n->tok, "this struct value");
}

// The address of a struct value: self, a local's block, or a fresh block in the constructor
// area (a method may keep the reference it is given, as `new` does, so it outlives the
// statement).
static MirSlot lower_struct_addr(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    if (is_self_ident(id) && L.has_self)
    {
        return 0;
    }
    if (n->kind == AST_PAREN_GROUP)
    {
        return lower_struct_addr(n->a);
    }
    uint32_t k = slots_of(u->types[id]);
    MirSlot base;
    if (!(n->kind == AST_IDENT && !u->refs[id] && find_local(token_name(TOK(u, n->tok)), &base)))
    {
        base = SLOT_PERM | L.perm;
        L.perm += k;
        lower_struct_into(id, base);
    }
    MirInst *a = inst(MIR_ADDR);
    a->d = temp();
    a->a = base;
    a->imm = k;
    return a->d;
}


static MirSlot lower_call(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    const AstNode *callee = NODE(u, n->a);
    SymId fn = u->refs[id];

    SymId put = (n->flags & AST_F_SINK) ? u->refs[id] : 0;
    if (callee->kind == AST_IDENT && TOK(u, callee->tok)->kind == TOK_WRITE)
    {
        return lower_output(n->b, 0, put);
    }
    uint64_t size;
    if (sema_sizeof(u, id, &size))
    {
        return emit_const(size); // a constant of the target's layout
    }
    if (sema_primitive(u, id) == PRIM_CONVERT && n->b)
    {
        return lower_as(n->b, u->types[id]); // the width changes: wraps, as a C cast
    }
    if (sema_primitive(u, id) == PRIM_TRAP)
    {
        // Literal text is joined into one message; anything else (a parameter, a `site`) is
        // written as a part, then the trap ends with what is left.
        char text[600];
        size_t len = 0;
        for (NodeId a = n->b; a; a = NEXT(u, a))
        {
            const AstNode *an = NODE(u, a);
            if (an->kind == AST_STRING_LIT)
            {
                len += decode_string(TOK(u, an->tok), (uint8_t *)text + len, sizeof(text) - len);
                continue;
            }
            if (len)
            {
                MirSlot joined = string_slot((const uint8_t *)text, len); // before the trap reads it
                MirInst *p = inst(MIR_TRAP);
                p->sub = MIR_TRAP_PART;
                p->a = joined;
                len = 0;
            }
            MirSlot part = lower_expr(a);
            MirInst *p = inst(MIR_TRAP);
            p->sub = MIR_TRAP_PART;
            p->a = part;
        }
        MirSlot last = string_slot((const uint8_t *)text, len);
        inst(MIR_TRAP)->a = last;
        return temp();
    }
    if (callee->kind == AST_IDENT && sema_is_std_print(u, u->refs[n->a]))
    {
        return lower_output(n->b, 1, put);
    }
    if (callee->kind == AST_IDENT && sema_std_member(u, u->refs[n->a]) == STD_SYS_PUT)
    {
        MirSlot byte = lower_as(n->b, T_U8);
        MirInst *w = inst(MIR_WRITE);
        w->sub = MW_BYTE;
        w->a = byte;
        return temp();
    }
    if (!fn || SYM(fn)->kind != SYM_METHOD)
    {
        unsupported(callee->tok, "calling something that is not a Modal function");
        return temp();
    }

    // Each argument is computed straight into its window slot when nothing after it calls
    // anything (a call would reuse the window); otherwise into a temporary, copied at the end.
    // A call that is inlined keeps its arguments where they are computed.
    const Inlinable *inline_body = inlinable_for(fn);
    MirSlot args[256];
    uint32_t argk[256], argw[256]; // slots each argument takes, and where in the window
    uint32_t win = 0;
    int nargs = 0;
    NodeId later_args = n->b;
    if (SYM(fn)->owner) // a method: the receiver comes first
    {
        MirSlot r;
        if (callee->kind == AST_SELECTOR)
        {
            TypeId base = u->types[callee->a];
            if (type_kind(base) == T_TYPE) // T.m(...): a fresh zero value, kept for the frame
            {
                uint32_t k = L.perm;
                L.perm += struct_slots(type_ref(base));
                MirInst *z = inst(MIR_ZERO);
                z->a = SLOT_PERM | k;
                z->imm = struct_slots(type_ref(base));
                MirInst *a = inst(MIR_ADDR);
                a->d = temp();
                a->a = SLOT_PERM | k;
                a->imm = struct_slots(type_ref(base));
                r = a->d;
            }
            else
            {
                r = lower_as(callee->a, type_make(T_PTR, type_make(T_STRUCT, SYM(fn)->owner)));
            }
        }
        else
        {
            r = 0; // a bare member name, through implicit self
        }
        int calls_after = 0;
        for (NodeId a = later_args; a; a = NEXT(u, a))
        {
            calls_after |= contains_call(a);
        }
        argk[nargs] = 1;
        argw[nargs] = win++;
        args[nargs++] = calls_after || inline_body ? r : into(r, SLOT_WIN | 0);
    }
    const Unit *home = &cx.units[SYM(fn)->unit]; // where the parameters are declared
    NodeId param = sema_fn_params(u, fn);
    for (NodeId a = n->b; a && param; a = NEXT(u, a), param = NEXT(home, param))
    {
        if (nargs == 256)
        {
            unsupported(NODE(u, a)->tok, "a call with more than 256 arguments");
            break;
        }
        TypeId pt = sema_param_type(u, fn, param);
        int calls_after = 0;
        for (NodeId b = NEXT(u, a); b; b = NEXT(u, b))
        {
            calls_after |= contains_call(b);
        }
        argw[nargs] = win;
        if (is_struct(pt)) // a struct value: its slots, copied
        {
            uint32_t k = slots_of(pt);
            MirSlot b = calls_after || inline_body ? temp_n(k) : (SLOT_WIN | win);
            lower_struct_into(a, b);
            args[nargs] = b;
            argk[nargs] = k;
            win += k;
            nargs++;
            continue;
        }
        MirSlot r = lower_as(a, pt);
        args[nargs] = calls_after || inline_body ? r : into(r, SLOT_WIN | win);
        argk[nargs] = 1;
        win++;
        nargs++;
    }
    if (inline_body)
    {
        MirSlot result = splice(inline_body, args, nargs);
        if (sema_fn_ret(u, fn) == T_VOID)
        {
            return temp();
        }
        if (is_temp(result))
        {
            return result;
        }
        MirInst *r = inst(MIR_COPY);
        r->d = temp();
        r->a = result;
        return r->d;
    }
    for (int i = 0; i < nargs; i++)
    {
        if (argk[i] == 1)
        {
            into(args[i], SLOT_WIN | argw[i]);
        }
        else
        {
            copy_slots(SLOT_WIN | argw[i], args[i], argk[i]);
        }
    }
    inst(MIR_CALL)->imm = fn;
    if (sema_fn_ret(u, fn) == T_VOID)
    {
        return temp();
    }
    if (is_struct(sema_fn_ret(u, fn)))
    {
        return SLOT_WIN | 0; // a struct result: the caller copies its slots before any call
    }
    // The result is in the callee's slot 0. It is copied out at once: the next call reuses the
    // window. When this copy is the last instruction, its user can take the value straight.
    MirInst *r = inst(MIR_COPY);
    r->d = temp();
    r->a = SLOT_WIN | 0;
    return r->d;
}

static void guard_divisor(MirSlot b, uint32_t tok);

static MirSlot lower_binop(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    TypeId ta = u->types[n->a], tb = u->types[n->b];

    if (t->kind == TOK_AND || t->kind == TOK_OR) // short-circuit, as C's && and ||
    {
        MirSlot d = temp();
        into(lower_expr(n->a), d);
        uint32_t end = new_label();
        MirInst *j = inst(t->kind == TOK_AND ? MIR_JZ : MIR_JNZ);
        j->a = d;
        j->imm = end;
        MirSlot b = lower_expr(n->b);
        MirInst *c = inst(MIR_COPY);
        c->d = d;
        c->a = b;
        place_label(end);
        return d;
    }
    if (ta == T_STR || tb == T_STR)
    {
        unsupported(n->tok, "an operator on strings");
        return temp();
    }

    BinPlan p = plan_binop(id);
    if (p.index < 0)
    {
        unsupported(n->tok, "this operator");
        return temp();
    }
    if (p.var < 0)
    {
        unsupported(n->tok, "an operator on values of this type");
        return temp();
    }

    uint8_t op = binops[p.index].op;
    NodeId left = n->a, right = n->b;
    uint64_t k;
    // A constant on the left of a commutative or mirrored operator goes to the right.
    if (const_value(left, &k) && !const_value(right, &k) && op != MB_SUB && op != MB_DIV)
    {
        static const uint8_t mirror[MB__COUNT] = {
            [MB_ADD] = MB_ADD, [MB_MUL] = MB_MUL, [MB_EQ] = MB_EQ, [MB_NE] = MB_NE,
            [MB_LT] = MB_GT,   [MB_LE] = MB_GE,   [MB_GT] = MB_LT, [MB_GE] = MB_LE,
        };
        op = mirror[op];
        left = n->b;
        right = n->a;
    }
    MirSlot a = lower_expr(left);
    MirInst *i;
    if (const_value(right, &k) && mir_normalize(k, p.var) < 0x10000 &&
        !(op == MB_DIV && mir_normalize(k, p.var) == 0))
    {
        i = inst(MIR_BINI);
        i->imm = mir_normalize(k, p.var);
    }
    else
    {
        MirSlot b = lower_expr(right);
        // no guard for a known divisor that is not 0, or an arm's binding whose test excludes 0
        int nonzero = NODE(u, right)->kind == AST_IDENT && (NODE(u, right)->aux & IDENT_NONZERO);
        if (op == MB_DIV && !nonzero && !(const_value(right, &k) && mir_normalize(k, p.var) != 0))
        {
            guard_divisor(b, n->tok);
        }
        i = inst(MIR_BIN);
        i->b = b;
    }
    MirSlot d = temp();
    i->sub = op;
    i->var = (uint8_t)p.var;
    i->a = a;
    i->d = d;
    if (p.result >= 0)
    {
        MirInst *c = inst(MIR_NORM); // (moves the array: `i` is not used after this)
        c->var = (uint8_t)p.result;
        c->a = d;
        c->d = d;
    }
    return d;
}

// A divisor that may be zero: the program stops there, with the place, instead of doing what
// the CPU does (0 on arm64, a fault on x86-64, undefined in C). The C backend says the same.
static void guard_divisor(MirSlot b, uint32_t tok)
{
    Unit *u = L.u;
    int line, col;
    source_pos(u->src, TSTART(TOK(u, tok)), &line, &col);
    char text[600];
    int n = snprintf(text, sizeof(text), "%s:%d:%d: division by zero\n", u->path, line, col);
    n = n < 0 ? 0 : n >= (int)sizeof(text) ? (int)sizeof(text) - 1 : n;
    uint32_t ok = new_label();
    MirInst *j = inst(MIR_JNZ);
    j->a = b;
    j->imm = ok;
    MirSlot m = string_slot((const uint8_t *)text, (size_t)n);
    inst(MIR_TRAP)->a = m;
    place_label(ok);
}

static MirSlot lower_field_base(NodeId base, uint32_t tok, int *ok)
{
    Unit *u = L.u;
    *ok = 1;
    if (is_self_ident(base) && L.has_self)
    {
        return 0;
    }
    if (type_kind(u->types[base]) == T_PTR)
    {
        return lower_expr(base);
    }
    if (is_struct(u->types[base]))
    {
        return lower_struct_addr(base); // a struct value: through its address
    }
    unsupported(tok, "a field of this value");
    *ok = 0;
    return temp();
}

static MirSlot lower_expr(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    TypeId type = u->types[id];
    uint64_t k;

    if (const_value(id, &k))
    {
        return emit_const(k);
    }
    switch (n->kind)
    {
    case AST_MATCH:
        return lower_match(id, type == T_INT_LIT ? T_INT : type);
    case AST_NUMBER_LIT:
        unsupported(n->tok, "a fractional number");
        return temp();
    case AST_STRING_LIT:
        return lower_string(t);
    case AST_SITE:
    {
        char text[600];
        int k = sema_site_text(u, id, text, sizeof(text));
        return string_slot((const uint8_t *)text, (size_t)k);
    }
    case AST_IDENT:
    {
        SymId s = u->refs[id];
        MirSlot slot;
        if (is_self_ident(id) && L.has_self)
        {
            return 0;
        }
        if (s && SYM(s)->kind == SYM_FIELD) // a bare member name, through implicit self
        {
            if (is_struct(field_type(s)))
            {
                unsupported(n->tok, "a struct inside a struct");
                return temp();
            }
            MirInst *i = inst(MIR_LOAD);
            i->var = (uint8_t)mem_variant(field_type(s));
            i->sub = field_is_register(s) ? MIR_MEM_DEVICE : 0;
            i->d = temp();
            i->a = 0;
            i->imm = field_offset(s);
            return i->d;
        }
        NodeId ginit;
        if (is_global(s, &ginit))
        {
            uint64_t computed;
            if (ginit && NODE(&cx.units[SYM(s)->unit], ginit)->kind != AST_NUMBER_LIT &&
                !sema_comptime_value(s, &computed) && !sema_comptime_aggregate(s))
            {
                unsupported(n->tok, "a module-level variable whose initializer is not a literal");
                return temp();
            }
            MirInst *i = inst(MIR_GLOAD);
            i->d = temp();
            i->imm = global_index(s);
            return i->d;
        }
        if (s && SYM(s)->kind == SYM_CONST && sema_device_address(s))
        {
            return emit_const(sema_device_address(s)); // a device: the address of its registers
        }
        if (s && SYM(s)->kind == SYM_CONST)
        {
            // A module-level constant: its initializer, if it is a literal.
            // (A number one was folded by const_value: here is what is left.)
            const Unit *home = &cx.units[SYM(s)->unit];
            const AstNode *decl = NODE(home, SYM(s)->decl);
            NodeId init = decl->kind == AST_FIELD_DECL ? decl->b : decl->a;
            if (!init || (decl->kind == AST_FIELD_DECL && (decl->flags & AST_F_MUT)))
            {
                unsupported(n->tok, "a module-level variable");
                return temp();
            }
            const ComptimeAggregate *computed = sema_comptime_aggregate(s);
            if (computed && type == T_STR)
            {
                return computed_string(computed);
            }
            if (NODE(home, init)->kind != AST_STRING_LIT)
            {
                unsupported(n->tok, "a module-level constant that is not a literal");
                return temp();
            }
            return lower_string(TOK(home, NODE(home, init)->tok));
        }
        if (s)
        {
            unsupported(n->tok, "using a function, a type or an import as a value");
            return temp();
        }
        if (type == T_BOOL && t->len == 4 && memcmp(TSTART(t), "true", 4) == 0)
        {
            return emit_const(1);
        }
        if (type == T_BOOL && t->len == 5 && memcmp(TSTART(t), "false", 5) == 0)
        {
            return emit_const(0);
        }
        if (type == T_NIL)
        {
            return emit_const(0);
        }
        if (find_local(token_name(t), &slot))
        {
            return slot;
        }
        if (type_kind(type) == T_FUNC)
        {
            unsupported(n->tok, "a function used as a value");
        }
        else if (SYM(L.f->sym)->flags & SYM_F_LITERAL)
        {
            unsupported(n->tok, "a function literal that uses a local of the function around it "
                                "(a closure)");
        }
        else
        {
            unsupported(n->tok, "this name");
        }
        return temp();
    }
    case AST_PAREN_GROUP:
        return lower_expr(n->a);
    case AST_UNARY_OP:
    {
        if (t->kind == TOK_AMP && is_self_ident(n->a) && L.has_self)
        {
            return 0; // &self: the reference itself
        }
        MirSlot a = lower_expr(n->a);
        int v = type == T_INT_LIT ? c_variant(id) : int_variant(type);
        MirInst *i;
        if (t->kind == TOK_BANG)
        {
            i = inst(MIR_NOT);
        }
        else if (t->kind == TOK_MINUS && v >= 0)
        {
            i = inst(MIR_NEG);
            i->var = (uint8_t)v;
        }
        else
        {
            unsupported(n->tok, "this unary operator");
            return temp();
        }
        i->a = a;
        i->d = temp();
        return i->d;
    }
    case AST_BIN_OP:
        return lower_binop(id);
    case AST_SELECTOR:
    {
        SymId member = u->refs[id];
        int ok;
        NodeId sinit;
        if (is_global(member, &sinit)) // a module-level variable of another module
        {
            uint64_t computed;
            if (sinit && NODE(&cx.units[SYM(member)->unit], sinit)->kind != AST_NUMBER_LIT &&
                !sema_comptime_value(member, &computed) && !sema_comptime_aggregate(member))
            {
                unsupported(n->tok, "a module-level variable whose initializer is not a literal");
                return temp();
            }
            MirInst *i = inst(MIR_GLOAD);
            i->d = temp();
            i->imm = global_index(member);
            return i->d;
        }
        if (member && SYM(member)->kind == SYM_CONST && sema_device_address(member))
        {
            return emit_const(sema_device_address(member)); // a device of another module
        }
        if (member && SYM(member)->kind == SYM_CONST) // a string constant of another module
        {
            const Unit *home = &cx.units[SYM(member)->unit];
            const AstNode *decl = NODE(home, SYM(member)->decl);
            NodeId init = decl->kind == AST_FIELD_DECL ? decl->b : decl->a;
            if (init && NODE(home, init)->kind == AST_STRING_LIT)
            {
                return lower_string(TOK(home, NODE(home, init)->tok));
            }
            const ComptimeAggregate *computed = sema_comptime_aggregate(member);
            if (computed && u->types[id] == T_STR)
            {
                return computed_string(computed);
            }
            unsupported(n->tok, "a module-level constant that is not a literal");
            return temp();
        }
        if (!member || SYM(member)->kind != SYM_FIELD)
        {
            unsupported(n->tok, "using a method as a value");
            return temp();
        }
        if (is_struct(field_type(member)))
        {
            unsupported(n->tok, "a struct inside a struct");
            return temp();
        }
        MirSlot base = lower_field_base(n->a, n->tok, &ok);
        if (!ok)
        {
            return base;
        }
        MirInst *i = inst(MIR_LOAD);
        i->var = (uint8_t)mem_variant(field_type(member));
        i->sub = field_is_register(member) ? MIR_MEM_DEVICE : 0;
        i->d = temp();
        i->a = base;
        i->imm = field_offset(member);
        return i->d;
    }
    case AST_CALL:
        return lower_call(id);
    case AST_ASSIGN:
    {
        const AstNode *target = NODE(u, n->a);
        SymId s = u->refs[n->a];
        MirSlot slot;
        if (target->kind == AST_IDENT && !s && find_local(token_name(TOK(u, target->tok)), &slot))
        {
            if (is_struct(u->types[n->a]))
            {
                lower_struct_into(n->b, slot);
                return temp();
            }
            return into(lower_as(n->b, u->types[n->a]), slot);
        }
        NodeId tinit;
        if ((target->kind == AST_IDENT || target->kind == AST_SELECTOR) && is_global(s, &tinit))
        {
            MirSlot value = lower_as(n->b, u->types[n->a]);
            MirInst *i = inst(MIR_GSTORE);
            i->a = value;
            i->imm = global_index(s);
            return value;
        }
        if (target->kind == AST_SELECTOR || (target->kind == AST_IDENT && s && SYM(s)->kind == SYM_FIELD))
        {
            MirSlot base = 0;
            int ok = 1;
            if (target->kind == AST_SELECTOR)
            {
                base = lower_field_base(target->a, target->tok, &ok);
            }
            if (s && SYM(s)->kind == SYM_FIELD && is_struct(field_type(s)))
            {
                unsupported(target->tok, "a struct inside a struct");
                return temp();
            }
            MirSlot value = lower_as(n->b, u->types[n->a]);
            if (!ok || !s || SYM(s)->kind != SYM_FIELD)
            {
                if (ok)
                {
                    unsupported(target->tok, "assigning to this");
                }
                return value;
            }
            MirInst *i = inst(MIR_STORE);
            i->var = (uint8_t)mem_variant(field_type(s));
            i->sub = field_is_register(s) ? MIR_MEM_DEVICE : 0;
            i->a = base;
            i->imm = field_offset(s);
            i->b = value;
            return value;
        }
        unsupported(target->tok, "assigning to this");
        return temp();
    }
    case AST_FN_LIT:
        unsupported(n->tok, "a function literal used as a value");
        return temp();
    case AST_INDEX:
        unsupported(n->tok, "indexing");
        return temp();
    default:
        unsupported(n->tok, "this expression");
        return temp();
    }
}

// ---- statements and functions -----------------------------------------------------------------

static void lower_return(NodeId value)
{
    if (value && is_struct(L.ret))
    {
        lower_struct_into(value, 0);
    }
    else if (value)
    {
        into(lower_as(value, L.ret), 0);
    }
    inst(MIR_RET);
}

static void lower_stmt(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    uint32_t top = L.top;

    switch (n->kind)
    {
    case AST_BLOCK:
    {
        int locals = L.nlocals;
        for (NodeId s = n->a; s; s = NEXT(u, s))
        {
            lower_stmt(s);
        }
        L.nlocals = locals;
        return;
    }
    case AST_DEFINE:
    {
        if (NODE(u, n->a)->kind == AST_FN_LIT)
        {
            return; // generated as a function of its own; calls to the local name it directly
        }
        if (is_struct(u->types[id])) // a struct value: its own block of slots
        {
            uint32_t k = slots_of(u->types[id]);
            MirSlot slot = temp_n(k);
            lower_struct_into(n->a, slot);
            push_local_n(token_name(TOK(u, n->tok)), slot, k);
            L.top = top + k;
            return;
        }
        MirSlot slot = temp(); // the local lives where the statement's temporaries began
        into(lower_as(n->a, u->types[id]), slot);
        push_local(token_name(TOK(u, n->tok)), slot);
        L.top = top + 1;
        return;
    }
    case AST_FIELD_DECL: // a local with its type: its value converted to it, or zero
    {
        TypeId t = u->types[id];
        if (is_struct(t))
        {
            uint32_t k = slots_of(t);
            MirSlot slot = temp_n(k);
            if (n->b)
            {
                lower_struct_into(n->b, slot);
            }
            else
            {
                for (uint32_t i = 0; i < k; i++)
                {
                    into(emit_const(0), slot + i);
                }
            }
            push_local_n(token_name(TOK(u, n->tok)), slot, k);
            L.top = top + k;
            return;
        }
        MirSlot slot = temp();
        into(n->b ? lower_as(n->b, t) : emit_const(0), slot);
        push_local(token_name(TOK(u, n->tok)), slot);
        L.top = top + 1;
        return;
    }
    case AST_RETURN:
        lower_return(n->a);
        L.top = top;
        return;
    default:
        lower_expr(id);
        L.top = top;
        return;
    }
}

static const AstNode *fn_node(const Unit *u, SymId fn)
{
    const AstNode *n = NODE(u, SYM(fn)->decl);
    return n->kind == AST_DEFINE ? NODE(u, n->a) : n;
}

int mir_lowerable(Unit *u, SymId fn, int report)
{
    const AstNode *n = fn_node(u, fn);
    (void)report;
    return SYM(fn)->owner || !(n->flags & AST_F_SELF);
}

static MirSlot emit_const(uint64_t v);

// main's prologue: every module-level variable of the program gets its initial value (a literal,
// or 0). An initializer that is not a literal is reported where the variable is used.
static void init_globals(void)
{
    for (SymId s = 1; s < cx.syms.size; s++)
    {
        NodeId init;
        if (!is_global(s, &init))
        {
            continue;
        }
        const Unit *home = &cx.units[SYM(s)->unit];
        uint64_t value = 0;
        uint64_t computed;
        const ComptimeAggregate *text = init ? sema_comptime_aggregate(s) : NULL;
        if (text && text->n == 1 && text->str[0]) // a string the compiler computed
        {
            MirSlot k = string_slot(text->str[0], text->len[0]);
            MirInst *st = inst(MIR_GSTORE);
            st->a = k;
            st->imm = global_index(s);
            L.top = k;
            continue;
        }
        if (init && sema_comptime_value(s, &computed))
        {
            int v = int_variant(sema_value_type((Unit *)home, s));
            value = v >= 0 ? mir_normalize(computed, v) : computed;
        }
        else if (init)
        {
            const AstNode *n = NODE(home, init);
            const Token *t = TOK(home, n->tok);
            if (n->kind != AST_NUMBER_LIT || memchr(TSTART(t), '.', t->len))
            {
                continue; // not a literal: not given a place, so a use of it is reported
            }
            int v = int_variant(sema_value_type((Unit *)home, s));
            value = v >= 0 ? mir_normalize(literal_value(t), v) : literal_value(t);
        }
        MirSlot k = emit_const(value);
        MirInst *st = inst(MIR_GSTORE);
        st->a = k;
        st->imm = global_index(s);
        L.top = k; // the temporary is free again
    }
}

// A clause's test: go to `miss` when `slot <op> k` (in variant v).
static void miss_if(uint8_t op, int v, MirSlot slot, uint64_t k, uint32_t miss)
{
    MirSlot t = temp();
    if (k < 0x10000)
    {
        bini(op, v, t, slot, k);
    }
    else
    {
        MirSlot c = emit_const(k);
        MirInst *i = inst(MIR_BIN);
        i->sub = op;
        i->var = (uint8_t)v;
        i->d = t;
        i->a = slot;
        i->b = c;
    }
    MirInst *j = inst(MIR_JNZ);
    j->a = t;
    j->imm = miss;
    L.top = t; // the test's temporary is free again
}

// A string pattern: the length, then the bytes, eight at a time while eight are left (a string
// is its length, then its bytes; every target is little-endian), then one at a time.
static void miss_unless_text(MirSlot s, uint32_t tok, uint32_t miss)
{
    uint8_t text[4096];
    size_t len = sema_pattern_text(L.u, tok, text, sizeof(text));
    if (len > sizeof(text))
    {
        unsupported(tok, "a string pattern longer than 4096 bytes");
        return;
    }
    uint32_t top = L.top;
    MirSlot w = temp();
    MirInst *ld = inst(MIR_LOAD);
    ld->var = MV_U64;
    ld->d = w;
    ld->a = s;
    ld->imm = 0;
    miss_if(MB_NE, MV_U64, w, len, miss);
    for (size_t at = 0; at < len;)
    {
        int wide = len - at >= 8;
        uint64_t k = 0;
        for (int b = 0; b < (wide ? 8 : 1); b++)
        {
            k |= (uint64_t)text[at + (size_t)b] << (8 * b);
        }
        ld = inst(MIR_LOAD);
        ld->var = wide ? MV_U64 : MV_U8;
        ld->d = w;
        ld->a = s;
        ld->imm = 8 + at;
        miss_if(MB_NE, wide ? MV_U64 : MV_U8, w, k, miss);
        at += wide ? 8 : 1;
    }
    L.top = top;
}

// Go to `miss` unless the value in `slot` (of type t) is one the pattern takes.
static void pattern_test(const Pattern *pat, TypeId t, MirSlot slot, uint32_t miss)
{
    if (pat->kind == PATTERN_TEXT)
    {
        miss_unless_text(slot, pat->tok, miss);
        return;
    }
    int v = int_variant(t);
    if (pat->kind == PATTERN_VALUE)
    {
        miss_if(MB_NE, v, slot, mir_normalize(pat->lo, v), miss);
        return;
    }
    int is_signed = v >= MV_I8;
    uint64_t lo = mir_normalize(pat->lo, v), hi = mir_normalize(pat->hi, v);
    uint64_t min = is_signed ? mir_normalize(1ull << ((8u << (v - MV_I8)) - 1), v) : 0;
    uint64_t max = mir_normalize(is_signed ? (1ull << ((8u << (v - MV_I8)) - 1)) - 1
                                           : ~0ull >> (64 - (8u << (v - MV_U8))), v);
    if (lo != min)
    {
        miss_if(MB_LT, v, slot, lo, miss);
    }
    if (hi != max)
    {
        miss_if(MB_GT, v, slot, hi, miss);
    }
}

// ---- arms (docs/std.md §8) ------------------------------------------------------------------------
//
// Each arm: its tests go on to the next arm when the value is not one it takes, a name binds the
// value's slot, then the result. On the parameters (a function body of arms) each result is the
// return; on a value, it goes to the match's slot (a value) or is a statement, and the arm goes to
// the end. The last arm has no test when the arms above leave it only its values (MATCH_LAST_FREE).
static MirSlot lower_match(NodeId id, TypeId want)
{
    Unit *u = L.u;
    const AstNode *m = NODE(u, id);
    MirSlot subj[16];
    TypeId types[16];
    int ns = 0;
    if (m->a)
    {
        types[0] = u->types[m->a];
        MirSlot v = lower_as(m->a, types[0]);
        subj[0] = temp();
        into(v, subj[0]);
        ns = 1;
    }
    else
    {
        SymId fn = L.f->sym;
        MirSlot slot = SYM(fn)->owner != 0;
        for (NodeId p = sema_fn_params(u, fn); p && ns < 16; p = NEXT(u, p))
        {
            TypeId pt = sema_param_type(u, fn, p);
            const AstNode *pn = NODE(u, p);
            const Token *name = TOK(u, pn->tok);
            if (!(pn->flags & AST_F_CONTEXT) && !is_self_token(TSTART(name), (int)name->len))
            {
                types[ns] = pt;
                subj[ns++] = slot;
            }
            slot += slots_of(pt);
        }
    }
    int value = m->a && want != T_VOID && want != T_NONE;
    if (value && slots_of(want) != 1)
    {
        unsupported(m->tok, "a match whose value is a struct");
        return temp();
    }
    MirSlot r = value ? temp() : 0;
    uint32_t end = new_label(), top = L.top;
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm))
    {
        const AstNode *a = NODE(u, arm);
        int last = !NEXT(u, arm);
        uint32_t next = last ? 0 : new_label();
        int locals = L.nlocals;
        int k = 0;
        for (NodeId pat = a->a; pat && k < ns; pat = NEXT(u, pat), k++)
        {
            const AstNode *pn = NODE(u, pat);
            if (pn->kind == AST_IDENT)
            {
                Pattern pt;
                if (pn->a && !(last && (m->aux & MATCH_LAST_FREE)) && sema_arm_pattern(u, pn->a, types[k], &pt))
                {
                    pattern_test(&pt, types[k], subj[k], next); // `d 1..9`: the test, then the name
                }
                MirSlot at = subj[k];
                if (pn->b) // `u64 n`: a copy of its own, which the arm may change
                {
                    at = temp();
                    into(subj[k], at);
                }
                push_local(token_name(TOK(u, pn->tok)), at);
                continue;
            }
            Pattern pt;
            if (!(last && (m->aux & MATCH_LAST_FREE)) && sema_arm_pattern(u, pat, types[k], &pt))
            {
                pattern_test(&pt, types[k], subj[k], next);
            }
        }
        NodeId res = a->b;
        int kind = NODE(u, res)->kind;
        int statement = kind == AST_BLOCK || kind == AST_RETURN || kind == AST_ASSIGN || kind == AST_FIELD_DECL;
        if (!m->a) // on the parameters: the result is the return
        {
            if (!statement && L.ret != T_VOID)
            {
                lower_return(res);
            }
            else
            {
                lower_stmt(res);
                if (!L.f->n || L.f->code[L.f->n - 1].op != MIR_RET)
                {
                    inst(MIR_RET);
                }
            }
        }
        else
        {
            if (value)
            {
                into(lower_as(res, want), r);
            }
            else
            {
                lower_stmt(res);
            }
            if (!last)
            {
                inst(MIR_JUMP)->imm = end;
            }
        }
        L.nlocals = locals;
        L.top = top;
        if (next)
        {
            place_label(next);
        }
    }
    place_label(end);
    return value ? r : temp();
}

static void region_start(void);

int mir_lower(Unit *u, SymId fn, MirFunc *out)
{
    region_start(); // the previous function is done
    const AstNode *n = fn_node(u, fn);

    L.u = u;
    L.f = out;
    L.errors = 0;
    L.top = L.max_top = L.perm = 0;
    L.nlocals = 0;
    L.ret = sema_fn_ret(u, fn);
    L.has_self = SYM(fn)->owner != 0;
    L.struct_abi = is_struct(L.ret);
    *out = (MirFunc){.u = u, .sym = fn, .returns = L.ret != T_VOID,
                     .ret_slots = L.ret == T_VOID ? 0 : slots_of(L.ret)};
    if (!mir_lowerable(u, fn, 0))
    {
        unsupported(SYM(fn)->tok, "a module-level function that takes `self`");
        return 0;
    }
    if (SYM(fn)->flags & SYM_F_CAPTURES)
    {
        unsupported(SYM(fn)->tok,
                    "a function literal that uses a local of the function around it (a closure)");
        return 0;
    }

    if (L.has_self)
    {
        temp(); // slot 0: self
    }
    // Parameters; a pattern has a slot but no name.
    uint32_t miss = 0;
    SymId next_clause = sema_next_clause(u, fn);
    if (next_clause)
    {
        miss = new_label();
    }
    // Parameters take consecutive slots (the caller's window puts them there), before the
    // tests need any temporary.
    MirSlot first_param = L.top;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p))
    {
        TypeId pt = sema_param_type(u, fn, p);
        uint32_t k = slots_of(pt);
        L.struct_abi |= is_struct(pt);
        MirSlot slot = temp_n(k);
        uint64_t value;
        if (!sema_pattern(u, p, SYM(fn)->owner, &value, 0))
        {
            push_local_n(token_name(TOK(u, NODE(u, p)->tok)), slot, k);
        }
    }
    uint32_t params_end = L.top;
    out->param_slots = params_end;
    MirSlot next_slot = first_param;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p))
    {
        MirSlot slot = next_slot;
        next_slot += slots_of(sema_param_type(u, fn, p));
        Pattern pat;
        if (!sema_pattern_of(u, p, SYM(fn)->owner, &pat, 0))
        {
            continue;
        }
        // The clause's test: an argument its pattern does not take sends the call on.
        pattern_test(&pat, sema_param_type(u, fn, p), slot, miss);
    }

    if (u->id == 0 && SYM(fn)->owner == 0 && scope_find(u->scope, "main", 4) == fn)
    {
        init_globals();
    }
    if (n->flags & AST_F_ONELINE)
    {
        lower_return(fn_body(u, (NodeId)(n - u->nodes.data)));
    }
    else
    {
        for (NodeId s = NODE(u, fn_body(u, (NodeId)(n - u->nodes.data)))->a; s; s = NEXT(u, s))
        {
            lower_stmt(s);
        }
        if (!out->n || out->code[out->n - 1].op != MIR_RET)
        {
            inst(MIR_RET);
        }
    }
    if (next_clause)
    {
        // The patterns did not match: the same arguments go to the next clause.
        place_label(miss);
        for (MirSlot i = 0; i < params_end; i++)
        {
            MirInst *c = inst(MIR_COPY);
            c->d = SLOT_WIN | i;
            c->a = i;
        }
        inst(MIR_CALL)->imm = next_clause;
        copy_slots(0, SLOT_WIN | 0, out->ret_slots);
        inst(MIR_RET);
    }

    finish_frame(out);
    if (L.errors == 0)
    {
        mir_optimize(out);
    }
    keep_if_small(out); // a small body, kept to be spliced into later callers
    if (L.errors == 0)
    {
        mark_tail_calls(out); // after keeping: a spliced body must return to its caller
    }
    return L.errors == 0;
}

// The frame is complete: constructor values and the window get their places. It holds at
// least the result, which is written over the first slots on return.
static void finish_frame(MirFunc *out)
{
    if (L.max_top < out->ret_slots)
    {
        L.max_top = out->ret_slots;
    }
    out->frame_size = L.max_top + L.perm;
    memset(L.is_local, 0, L.max_top < MAX_TRACKED ? L.max_top : MAX_TRACKED); // for the next
    for (size_t i = 0; i < out->n; i++)
    {
        MirSlot *slots[3] = {&out->code[i].d, &out->code[i].a, &out->code[i].b};
        for (int k = 0; k < 3; k++)
        {
            if (*slots[k] & SLOT_PERM)
            {
                *slots[k] = L.max_top + (*slots[k] & ~SLOT_KIND);
            }
            else if (*slots[k] & SLOT_WIN)
            {
                *slots[k] = out->frame_size + (*slots[k] & ~SLOT_KIND);
            }
        }
    }
}

// ---- compile time ---------------------------------------------------------------------------------

// Lowering inside a lowering: the optimizer of one function asks the compiler to run a callee
// (sema/comptime.c), which may lower it now. The function being optimized keeps its MIR (the
// nested lowerings start after it in the region) and the lowering state comes back after.
static const void *nest_base; // where a nested lowering starts (NULL: not nested)
static Lower saved_L;

int mir_nest_begin(void)
{
    if (nest_base)
    {
        return 0; // one level only
    }
    nest_base = region_end(REGION_MIR);
    saved_L = L;
    return 1;
}

// The region may move to a reservation of its own while a nested lowering grows it (what was
// handed out before stays where it is): the nested start is then the new region's end.
static int in_mir_region(const void *p)
{
    const Region *r = &cx.regions[REGION_MIR];
    return (const char *)p >= r->base && (const char *)p <= r->base + r->used;
}

void mir_nest_end(void)
{
    L = saved_L;
    if (in_mir_region(nest_base))
    {
        region_set_end(REGION_MIR, nest_base);
    }
    nest_base = NULL;
}

static void region_start(void)
{
    region_end(REGION_MIR); // reserves it on first use
    if (nest_base && !in_mir_region(nest_base))
    {
        nest_base = region_end(REGION_MIR);
    }
    region_set_end(REGION_MIR, nest_base ? nest_base : cx.regions[REGION_MIR].base);
}

// The slots a call to fn fills: self, then each parameter (a struct takes several). From the
// signature alone, so the optimizer can ask without lowering fn.
uint32_t mir_param_slots(SymId fn)
{
    Unit *u = &cx.units[SYM(fn)->unit];
    uint32_t k = SYM(fn)->owner != 0;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p))
    {
        k += slots_of(sema_param_type(u, fn, p));
    }
    return k;
}

// A fresh compilation in the same process (--repeat; the compiler lowers while checking now):
// nothing kept from the last one.
void mir_reset(void)
{
    inlinable = NULL;
    inlinable_cap = 0;
    global_of = NULL;
    global_cap = 0;
    nglobals = 0;
    nsink_helpers = 0;
    nest_base = NULL;
    nheld = 0;
}

// A module-level initializer as a function of no parameters that returns it (in slot 0), for the
// compiler to run (sema/comptime.c). The function `fn` it is lowered for names it in --mir.
int mir_lower_expr(Unit *u, NodeId expr, TypeId type, SymId fn, MirFunc *out)
{
    region_start();
    L.u = u;
    L.f = out;
    L.errors = 0;
    L.top = L.max_top = L.perm = 0;
    L.nlocals = 0;
    L.ret = type;
    L.has_self = 0;
    L.struct_abi = 1; // never kept for inlining
    *out = (MirFunc){.u = u, .sym = fn, .returns = 1, .ret_slots = slots_of(type), .label = "(compile time)"};
    if (type == T_VOID) // a statement (`assert(...)`, a call): run for what it checks
    {
        out->ret_slots = 0;
        out->returns = 0;
        lower_expr(expr);
        inst(MIR_RET);
        finish_frame(out);
        return L.errors == 0;
    }
    L.top = out->ret_slots; // from slot 0: the result
    if (is_struct(type))
    {
        lower_struct_into(expr, 0);
    }
    else
    {
        into(lower_as(expr, type), 0);
    }
    inst(MIR_RET);
    finish_frame(out);
    return L.errors == 0;
}

// ---- --mir ----------------------------------------------------------------------------------------

void mir_print(const MirFunc *f, FILE *out)
{
    static const char *const ops[MIR__COUNT] = {
        "const", "copy", "addr", "zero", "load", "store", "bin", "bini", "neg",
        "not",   "norm", "label", "jz",  "jnz",  "call",  "ret", "str",  "write", "gload", "gstore",
        "trap",  "jump",
    };
    static const char *const bins[MB__COUNT] = {"add", "sub", "mul", "div", "eq",
                                                "ne",  "lt",  "le",  "gt",  "ge"};
    static const char *const vars[MV__COUNT] = {"u8", "u16", "u32", "u64",
                                                "i8", "i16", "i32", "i64"};
    static const char *const writes[] = {"str", "int", "uint", "bool", "space", "nl", "byte"};
    int len;
    const char *name = f->label ? f->label : symbol_name(f->sym, &len);
    if (f->label)
    {
        len = (int)strlen(f->label);
    }
    fprintf(out, "fn %.*s  frame %u\n", len, name, f->frame_size);
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        switch (in->op)
        {
        case MIR_BIN:
            fprintf(out, "    s%u = %s.%s s%u, s%u\n", in->d, bins[in->sub], vars[in->var], in->a, in->b);
            break;
        case MIR_BINI:
            fprintf(out, "    s%u = %s.%s s%u, %llu\n", in->d, bins[in->sub], vars[in->var], in->a,
                    (unsigned long long)in->imm);
            break;
        case MIR_LABEL:
            fprintf(out, "  L%llu:\n", (unsigned long long)in->imm);
            break;
        case MIR_WRITE:
            fprintf(out, "    write.%s s%u\n", writes[in->sub], in->a);
            break;
        case MIR_CALL:
        {
            int l;
            const char *callee = symbol_name((SymId)in->imm, &l);
            if (in->sub == MIR_CALL_TAIL)
            {
                fprintf(out, "    tail %.*s (%u slots)\n", l, callee, in->b);
                break;
            }
            fprintf(out, "    call %.*s\n", l, callee);
            break;
        }
        default:
            fprintf(out, "    %s d=s%u a=s%u b=s%u imm=%llu%s%s\n", ops[in->op], in->d, in->a, in->b,
                    (unsigned long long)in->imm,
                    in->op == MIR_NEG || in->op == MIR_NORM || in->op == MIR_LOAD || in->op == MIR_STORE
                        ? " ."
                        : "",
                    in->op == MIR_NEG || in->op == MIR_NORM || in->op == MIR_LOAD || in->op == MIR_STORE
                        ? vars[in->var]
                        : "");
            break;
        }
    }
}
