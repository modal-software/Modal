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
#include <stdlib.h>
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
        uint8_t ref; // the slot holds the address of the value (an array parameter); LOCAL_FIELDS
    } locals[MAX_LOCALS];
    int nlocals;
    NodeId rest_of, rest; // the statement being lowered in a block, and the ones after it
    int has_self;   // slot 0 holds self (a reference)
    int struct_abi; // a parameter or the result is a struct value: never inlined
    int fails;      // the function says `, error`: its error is the slot after its value
    NodeId handled_call;  // `n, e := f()`: the call whose error the binding takes
    NodeId guard_call;    // and the call that is a boundary (a panic below comes back to it)
    MirSlot handled_error; // and that error, once the call is lowered
    int loops;      // the function calls itself (a loop): larger bodies are inlined into it
    int for_compiler; // lowered for the compiler to run (comptime.c): descriptors are its own
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

// A module-level struct variable has a global slot per scalar field (its leaves, nested
// structs and arrays flattened, in declaration order; fields with no bytes have none): the same
// global slots every backend has, so a struct needs no address of its own. `&g` is not given.
static uint32_t global_leaves(TypeId t)
{
    if (type_kind(t) == T_ARRAY)
    {
        return type_len(t) * global_leaves(type_ref(t));
    }
    if (type_kind(t) != T_STRUCT)
    {
        return 1;
    }
    SymId st = type_ref(t);
    Unit *home = &cx.units[SYM(st)->unit];
    uint32_t n = 0;
    for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind == AST_FIELD_DECL && !layout_member_gone(st, m))
        {
            n += global_leaves(sema_type_of(home, NODE(home, m)->a, st));
        }
    }
    return n;
}

// The first leaf of field `field` inside struct type t.
static uint32_t global_leaf_of(TypeId t, SymId field)
{
    SymId st = type_ref(t);
    Unit *home = &cx.units[SYM(st)->unit];
    uint32_t n = 0;
    const Token *want = TOK(home, SYM(field)->tok);
    for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind != AST_FIELD_DECL || layout_member_gone(st, m))
        {
            continue;
        }
        if (NODE(home, m)->tok == SYM(field)->tok || TOK(home, NODE(home, m)->tok) == want)
        {
            return n;
        }
        n += global_leaves(sema_type_of(home, NODE(home, m)->a, st));
    }
    return n;
}

static uint32_t global_index(SymId s);

// The leaves an expression names inside a module-level struct variable (`g`, `g.a`, `g.a.x`,
// `util.g.x`): its variable and the first leaf. 0 if it is not one.
static int global_path(NodeId id, SymId *var, uint32_t *leaf)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    SymId s = u->refs ? u->refs[id] : 0;
    NodeId init;
    if (s && is_global(s, &init) && type_kind(sema_value_type(&cx.units[SYM(s)->unit], s)) == T_STRUCT)
    {
        *var = s;
        *leaf = 0;
        return 1;
    }
    if (n->kind == AST_SELECTOR && s && SYM(s)->kind == SYM_FIELD && global_path(n->a, var, leaf))
    {
        *leaf += global_leaf_of(u->types[n->a], s);
        return 1;
    }
    return 0;
}

// A struct of type t at address src + off, into the leaves of a global from `leaf` on.
static void global_store_fields(uint32_t leaf, MirSlot src, uint32_t off, TypeId t);
static void global_load_fields(uint32_t leaf, MirSlot dst, uint32_t off, TypeId t);

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
        TypeId t = sema_value_type(&cx.units[SYM(s)->unit], s);
        global_of[s] = nglobals + 1;
        nglobals += type_kind(t) == T_STRUCT ? global_leaves(t) : 1; // a struct: a slot per field
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
// A function that loops (it calls itself) inlines bodies up to this size: the call it saves is
// paid on every turn there, and its code is written once. Elsewhere the larger limit grew code
// (structs 3,016 -> 7,228 B) and the compiler's time (big emit +19%) for nothing measurable.
#define INLINE_LOOP_MAX 32

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
    if (f->n > INLINE_LOOP_MAX || L.errors || L.struct_abi)
    {
        return;
    }
    for (size_t i = 0; i < f->n; i++)
    {
        uint8_t op = f->code[i].op;
        if (op == MIR_CALL || op == MIR_ADDR || op == MIR_ZERO || (op == MIR_RET && f->code[f->n - 1].op != MIR_RET))
        {
            return; // (a return before the end, each arm's, becomes a jump to the end when spliced)
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
            k->strings[i] = (MirString){bytes, f->strings[i].len, f->strings[i].panic};
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
// Does the code from k on return at once (labels passed over, jumps followed)? A call there is the
// function's last act: in an arm that returns, `call; L: ret` and `call; jump L ... L: ret` too.
static int returns_at(const MirFunc *f, size_t k)
{
    for (int hops = 0; hops < 8 && k < f->n; hops++)
    {
        while (k < f->n && f->code[k].op == MIR_LABEL)
        {
            k++;
        }
        if (k >= f->n)
        {
            return 0;
        }
        if (f->code[k].op == MIR_RET)
        {
            return 1;
        }
        if (f->code[k].op != MIR_JUMP)
        {
            return 0;
        }
        uint32_t target = f->code[k].imm;
        size_t at = f->n;
        for (size_t j = 0; j < f->n; j++)
        {
            if (f->code[j].op == MIR_LABEL && f->code[j].imm == target)
            {
                at = j;
                break;
            }
        }
        k = at;
    }
    return 0;
}

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
        if (r < f->ret_slots || !returns_at(f, i + 1 + r))
        {
            continue;
        }
        uint32_t params = mir_param_slots((SymId)in.imm);
        MirInst *t = &f->code[to - 1];
        t->sub = MIR_CALL_TAIL;
        t->b = params; // the slots to move from the window to the frame's start
        if (f->code[i + 1 + r].op == MIR_RET)
        {
            i += 1 + r; // the copies of the result and the return are the callee's now
        }
        else
        {
            i += r; // the copies are the callee's; the labels after stay (other paths reach them)
        }
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
            h->f.strings[i] = (MirString){bytes, f->strings[i].len, f->strings[i].panic};
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
    case MIR_GADDR:
    case MIR_DESC:
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
    case MIR_GADDR:
    case MIR_DESC:
        return in->d == s;
    default:
        return 0;
    }
}

// Splices a kept body in place of a call. Its slots become window slots, except that a
// parameter is read straight from the caller's argument slot until the body first writes it
// (only in straight-line bodies: with a branch, the parameter is copied to its window slot).
// Returns the slot holding the result.
static void place_label(uint32_t label);

static MirSlot splice(const Inlinable *k, const MirSlot *args, int nargs)
{
    MirFunc *f = L.f;
    int branches = body_has_labels(k);
    uint32_t labels = f->nlabels;
    uint32_t done = labels + k->nlabels; // where a return inside the body goes
    f->nlabels += k->nlabels + 1;
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
        if (in.op == MIR_RET) // an arm's return: on to the end of the body
        {
            inst(MIR_JUMP)->imm = done;
            continue;
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
    int returns = 0;
    for (uint32_t i = 0; i < k->n; i++)
    {
        returns |= k->code[i].op == MIR_RET;
    }
    if (returns)
    {
        place_label(done);
        return SLOT_WIN | 0; // every return left its value there
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

static int is_array(TypeId t)
{
    return type_kind(t) == T_ARRAY;
}

// The frame slots a value of type t takes (an array local: its elements, packed).
static uint32_t slots_of(TypeId t)
{
    if (!is_struct(t) && !is_array(t))
    {
        return 1;
    }
    Layout l = type_layout(t);
    return l.size ? (l.size + 7) / 8 : 1;
}

// The slots a parameter or an argument of type t takes: an array goes by its address.
static uint32_t param_slots(TypeId t)
{
    return is_array(t) ? 1 : slots_of(t);
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
    case MIR_GADDR:
    case MIR_DESC:
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

// A struct local whose every use is `p.field`: each field in a slot of its own (in declaration
// order), so it lives in registers. It is never in memory, so nothing takes its address.
#define LOCAL_FIELDS 2
// A number or bool local whose address was taken (`&n`): its slot is memory from then on (the
// register allocator leaves addressed slots alone), and a write through the reference stores
// only its width, so every later read loads it in its width.
#define LOCAL_BOXED 3

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
    L.locals[L.nlocals].ref = 0;
    L.nlocals++;
    for (uint32_t j = 0; j < k; j++)
    {
        if (slot + j < MAX_TRACKED)
        {
            L.is_local[slot + j] = 1;
        }
    }
}

static int find_local_ref(NameId name, MirSlot *slot, int *ref)
{
    for (int i = L.nlocals - 1; i >= 0; i--)
    {
        if (L.locals[i].name == name)
        {
            *slot = L.locals[i].slot;
            *ref = L.locals[i].ref;
            return 1;
        }
    }
    return 0;
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
    case T_ERROR:
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
    if (L.f && sema_known(L.f->sym, id, out))
    {
        return 1; // computed while compiling for this specialization (a load's offset)
    }
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
        if (n->kind == AST_SELECTOR && sema_typeof_field(u, id, out))
        {
            return 1; // `typeof(point).size`: known while compiling
        }
        if (n->kind == AST_IDENT && sema_bound_value(u, id, out))
        {
            return 1; // a value parameter of a specialization: known while compiling
        }
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
static MirSlot element_addr(NodeId id, uint32_t *off);
static MirSlot lower_field_base(NodeId base, uint32_t tok, int *ok);
static MirSlot array_addr(NodeId arr, uint32_t tok);
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
    case AST_FIELD_INIT:
        return contains_call(n->a);
    case AST_STRUCT_LIT:
        for (NodeId f = n->a; f; f = NEXT(L.u, f))
        {
            if (contains_call(f))
            {
                return 1;
            }
        }
        return 0;
    default:
        return 0;
    }
}

// The value of `id` where a value of type `want` is expected: converted between integer types,
// and a struct given where a reference is wanted passes its reference.
static MirSlot lower_match(NodeId id, TypeId want);
static void lower_stmt(NodeId id);

static MirSlot string_slot(const uint8_t *bytes, size_t len);
static MirInst *bini(uint8_t op, int var, MirSlot d, MirSlot a, uint64_t imm);

// ---- type descriptors (std's type, std/type.modal) ----------------------------------------------
//
// A descriptor is std's `type` (id, size, align, fields, members) followed by its member table
// (offset, of) per field. They hold addresses (members, of), and machine code has no data that
// holds an address at load time, so they live in module-level slots, written by a routine the
// compiler makes when it knows them all (mir_next_helper), which the program's entry runs before
// main (mir_entry_routine). A use is then the descriptor's address (MIR_GADDR). For the compiler
// to run (comptime), MIR_DESC asks the interpreter for its own copy instead.
#define MAX_DESCS 1024
static struct
{
    TypeId t;
    uint32_t base, fields;
} descs[MAX_DESCS];
static int ndescs;
static SymId desc_init;  // the routine that writes them
static SymId desc_start; // the entry when there are descriptors: desc_init, then main
static int desc_written, start_written;

static uint32_t desc_alloc(TypeId t)
{
    for (int i = 0; i < ndescs; i++)
    {
        if (descs[i].t == t)
        {
            return descs[i].base;
        }
    }
    if (ndescs == MAX_DESCS)
    {
        return 0;
    }
    uint64_t d[4];
    sema_type_desc(t, d);
    int me = ndescs++;
    descs[me].t = t;
    descs[me].fields = (uint32_t)d[3];
    descs[me].base = nglobals;
    nglobals += 5 + 2 * (uint32_t)d[3];
    if (type_kind(t) == T_STRUCT) // the types of its fields have descriptors too
    {
        SymId st = type_ref(t);
        Unit *home = &cx.units[SYM(st)->unit];
        for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
        {
            if (NODE(home, m)->kind == AST_FIELD_DECL)
            {
                desc_alloc(sema_type_of(home, NODE(home, m)->a, st));
            }
        }
    }
    return descs[me].base;
}

// The address of t's descriptor, plus `slot` words (5: its member table).
static MirSlot desc_address(TypeId t, uint32_t slot)
{
    MirSlot d = temp();
    if (sema_capturing() || L.for_compiler)
    {
        MirInst *i = inst(MIR_DESC);
        i->d = d;
        i->imm = t;
        if (slot)
        {
            bini(MB_ADD, MV_U64, d, d, 8ull * slot);
        }
        return d;
    }
    uint32_t base = desc_alloc(t);
    if (!desc_init)
    {
        desc_init = symbol_new(&cx.units[0], SYM_METHOD, 0, 0, 0, 0);
        desc_start = symbol_new(&cx.units[0], SYM_METHOD, 0, 0, 0, 0);
    }
    MirInst *a = inst(MIR_GADDR);
    a->d = d;
    a->imm = base + slot;
    return d;
}

// A module-level slot = a constant (the constant made first: the store reads it).
static void gstore_const(uint32_t slot, uint64_t value)
{
    MirSlot v = emit_const(value);
    MirInst *g = inst(MIR_GSTORE);
    g->a = v;
    g->imm = slot;
}

// The routine that writes every descriptor (the entry runs it before main).
static void lower_desc_init(void)
{
    for (int i = 0; i < ndescs; i++)
    {
        uint64_t d[4];
        sema_type_desc(descs[i].t, d);
        uint32_t base = descs[i].base;
        for (int w = 0; w < 4; w++)
        {
            gstore_const(base + (uint32_t)w, d[w]);
        }
        if (descs[i].fields)
        {
            MirSlot members = temp();
            MirInst *a = inst(MIR_GADDR);
            a->d = members;
            a->imm = base + 5;
            MirInst *g = inst(MIR_GSTORE);
            g->a = members;
            g->imm = base + 4;
        }
        else
        {
            gstore_const(base + 4, 0);
        }
        TypeId t = descs[i].t;
        if (type_kind(t) != T_STRUCT)
        {
            continue;
        }
        SymId st = type_ref(t);
        Unit *home = &cx.units[SYM(st)->unit];
        uint32_t k = 0;
        for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
        {
            if (NODE(home, m)->kind != AST_FIELD_DECL)
            {
                continue;
            }
            gstore_const(base + 5 + 2 * k, layout_member_offset(st, m));
            MirSlot of = temp();
            MirInst *a = inst(MIR_GADDR);
            a->d = of;
            a->imm = desc_alloc(sema_type_of(home, NODE(home, m)->a, st));
            MirInst *s2 = inst(MIR_GSTORE);
            s2->a = of;
            s2->imm = base + 6 + 2 * k;
            k++;
        }
    }
}

SymId mir_entry_routine(void)
{
    return desc_start;
}

// The address of a type's descriptor (std's type: id, size, align, fields), read-only data
// next to the code: a string's bytes, past its length word.
static MirSlot type_descriptor(TypeId t)
{
    return desc_address(t, 0);
}

// typeof(a) of an *any: the address of its descriptor.
static MirSlot any_descriptor(NodeId any)
{
    MirSlot p = lower_expr(any);
    MirInst *ld = inst(MIR_LOAD);
    ld->var = MV_U64;
    ld->d = temp();
    ld->a = p;
    ld->imm = 8;
    return ld->d;
}

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
    if (sema_is_any_ref(want) && type_kind(have) == T_PTR && !sema_is_any_ref(have))
    {
        // a reference into an *any: the pair (address, type) in a block of this frame
        MirSlot at = lower_expr(id);
        uint32_t k = L.perm;
        L.perm += 2;
        into(at, SLOT_PERM | k);
        into(type_descriptor(type_ref(have)), SLOT_PERM | (k + 1));
        MirInst *a = inst(MIR_ADDR);
        a->d = temp();
        a->a = SLOT_PERM | k;
        a->imm = 2;
        return a->d;
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
    f->strings[f->nstrings] = (MirString){bytes, n, 0};
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
    f->strings[f->nstrings] = (MirString){bytes, len, 0};
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
static void miss_if(uint8_t op, int v, MirSlot slot, uint64_t k, uint32_t miss);
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
    if (desc_init && !desc_written) // the descriptors, now that every use has asked for its own
    {
        desc_written = 1;
        region_end(REGION_MIR);
        region_set_end(REGION_MIR, cx.regions[REGION_MIR].base);
        L.u = &cx.units[0];
        L.f = out;
        L.errors = 0;
        L.top = L.max_top = 0;
        L.perm = 0;
        L.nlocals = 0;
        L.ret = T_VOID;
        L.has_self = 0;
        L.struct_abi = 1;
        *out = (MirFunc){.u = L.u, .sym = desc_init, .label = "descriptors", .param_slots = 0};
        lower_desc_init();
        inst(MIR_RET);
        finish_frame(out);
        return 1;
    }
    SymId main_fn = desc_start ? scope_find(cx.units[0].scope, "main", 4) : 0;
    if (desc_start && !start_written && main_fn && SYM(main_fn)->kind == SYM_METHOD)
    {
        // the program's entry: the descriptors, then main, whose result is this one's
        start_written = 1;
        region_end(REGION_MIR);
        region_set_end(REGION_MIR, cx.regions[REGION_MIR].base);
        L.u = &cx.units[0];
        L.f = out;
        L.errors = 0;
        L.top = L.max_top = 0;
        L.perm = 0;
        L.nlocals = 0;
        L.has_self = 0;
        L.struct_abi = 1;
        TypeId ret = sema_fn_ret(&cx.units[0], main_fn);
        L.ret = ret;
        *out = (MirFunc){.u = L.u, .sym = desc_start, .label = "start", .param_slots = 0,
                         .returns = ret != T_VOID, .ret_slots = ret != T_VOID};
        inst(MIR_CALL)->imm = desc_init;
        inst(MIR_CALL)->imm = main_fn; // its result is in its slot 0, which is this one's
        inst(MIR_RET);
        finish_frame(out);
        return 1;
    }
    nsink_helpers = 0; // all handed over: the next program starts afresh
    ndescs = 0;
    desc_written = 0;
    start_written = 0;
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
    f->strings[f->nstrings] = (MirString){bytes, npending, 0};
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
    if (type_kind(type) == T_FUNC) // a function value: its name and signature (std.md §13)
    {
        char text[512];
        size_t len = sema_fn_text(u, type_ref(type), text, sizeof(text));
        add_pending(text, len);
        return 1;
    }
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

// print(e): a compare per error the program can name, each writing its name; one it cannot
// name (an id from elsewhere) is `error N`. The names are in the program only because it prints
// an error (G4: no name reaches a binary unless the program writes it).
static void write_text(const char *text)
{
    MirSlot str = string_slot((const uint8_t *)text, strlen(text));
    if (sink_put)
    {
        sink_value(SINK_STR, str);
        return;
    }
    MirInst *w = inst(MIR_WRITE);
    w->sub = MW_STR;
    w->a = str;
}

static void write_error(MirSlot e)
{
    static uint64_t ids[512];
    uint32_t n = sema_errors_list(ids, 512);
    uint32_t end = new_label();
    for (uint32_t i = 0; i <= n; i++)
    {
        uint64_t id = i ? ids[i - 1] : 0;
        char name[80];
        if (id)
        {
            sema_error_name(id, name, sizeof(name));
        }
        else
        {
            snprintf(name, sizeof(name), "nil");
        }
        uint32_t next = new_label();
        miss_if(MB_NE, MV_U64, e, id, next);
        write_text(name);
        inst(MIR_JUMP)->imm = end;
        place_label(next);
    }
    write_text("error ");
    if (sink_put)
    {
        sink_value(SINK_UINT, e);
    }
    else
    {
        MirInst *w = inst(MIR_WRITE);
        w->sub = MW_UINT;
        w->a = e;
    }
    place_label(end);
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
        if (t == T_ERROR) // print(e): its name (nil, bad_input, panic.division), or `error N`
        {
            flush_pending();
            write_error(s);
            L.top = top;
            continue;
        }
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

// A field with bytes (std.md §10: one nothing reads, or a constant, has none).
static int field_stored(SymId field)
{
    return layout_field_use(field, NULL) == FIELD_KEPT;
}

// A read of a field that has no bytes: its constant (0 for one nothing reads, which no code does).
static MirSlot field_constant(SymId field)
{
    uint64_t v = 0;
    layout_field_use(field, &v);
    TypeId t = sema_value_type(&cx.units[SYM(field)->unit], field);
    int var = int_variant(t);
    return emit_const(var >= 0 ? mir_normalize(v, var) : v);
}

static TypeId field_type(SymId field)
{
    return sema_value_type(&cx.units[SYM(field)->unit], field);
}

// The struct st at src + soff, copied to dst + doff (both addresses), one field at a time in its
// width: a struct held in another sits at any offset its alignment allows, so it is not whole
// slots. A field with no bytes is not copied; padding is not either.
static void copy_fields(MirSlot dst, uint32_t doff, MirSlot src, uint32_t soff, SymId st)
{
    Unit *home = &cx.units[SYM(st)->unit];
    for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind != AST_FIELD_DECL || layout_member_gone(st, m))
        {
            continue;
        }
        TypeId ft = sema_type_of(home, NODE(home, m)->a, st);
        uint32_t off = layout_member_offset(st, m);
        if (is_struct(ft))
        {
            copy_fields(dst, doff + off, src, soff + off, type_ref(ft));
            continue;
        }
        uint32_t n = is_array(ft) ? type_len(ft) : 1, w = is_array(ft) ? type_layout(type_ref(ft)).size : 0;
        TypeId et = is_array(ft) ? type_ref(ft) : ft;
        for (uint32_t e = 0; e < n; e++)
        {
            MirSlot v = temp();
            MirInst *ld = inst(MIR_LOAD);
            ld->var = (uint8_t)mem_variant(et);
            ld->d = v;
            ld->a = src;
            ld->imm = soff + off + e * w;
            MirInst *stv = inst(MIR_STORE);
            stv->var = (uint8_t)mem_variant(et);
            stv->a = dst;
            stv->b = v;
            stv->imm = doff + off + e * w;
        }
    }
}

// The leaves of a global struct (from `leaf`) to and from a struct at an address, in each field's
// width: a struct variable of the module is read or written whole this way.
static void global_fields(uint32_t leaf, MirSlot mem, uint32_t off, TypeId t, int store)
{
    if (type_kind(t) == T_ARRAY || type_kind(t) != T_STRUCT)
    {
        uint32_t n = type_kind(t) == T_ARRAY ? type_len(t) : 1;
        TypeId et = type_kind(t) == T_ARRAY ? type_ref(t) : t;
        uint32_t w = type_layout(et).size;
        for (uint32_t e = 0; e < n; e++)
        {
            MirSlot v = temp();
            if (store) // memory -> global
            {
                MirInst *ld = inst(MIR_LOAD);
                ld->var = (uint8_t)mem_variant(et);
                ld->d = v;
                ld->a = mem;
                ld->imm = off + e * w;
                MirInst *g = inst(MIR_GSTORE);
                g->a = v;
                g->imm = leaf + e;
            }
            else
            {
                MirInst *g = inst(MIR_GLOAD);
                g->d = v;
                g->imm = leaf + e;
                MirInst *st = inst(MIR_STORE);
                st->var = (uint8_t)mem_variant(et);
                st->a = mem;
                st->b = v;
                st->imm = off + e * w;
            }
        }
        return;
    }
    SymId st = type_ref(t);
    Unit *home = &cx.units[SYM(st)->unit];
    for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind != AST_FIELD_DECL || layout_member_gone(st, m))
        {
            continue;
        }
        TypeId ft = sema_type_of(home, NODE(home, m)->a, st);
        global_fields(leaf, mem, off + layout_member_offset(st, m), ft, store);
        leaf += global_leaves(ft);
    }
}

static void global_store_fields(uint32_t leaf, MirSlot src, uint32_t off, TypeId t)
{
    global_fields(leaf, src, off, t, 1);
}

static void global_load_fields(uint32_t leaf, MirSlot dst, uint32_t off, TypeId t)
{
    global_fields(leaf, dst, off, t, 0);
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
// `point{ x, y: e }` into the struct's slots at `block`: at zero when the fields leave padding,
// then each member stored at its offset, in its width.
static void struct_literal_at(NodeId id, MirSlot block)
{
    Unit *u = L.u;
    TypeId t = u->types[id];
    uint32_t k = slots_of(t), bytes = 0;
    for (NodeId f = NODE(u, id)->a; f; f = NEXT(u, f))
    {
        SymId member = u->refs[f];
        bytes += member && SYM(member)->kind == SYM_FIELD && field_stored(member) ? type_layout(field_type(member)).size
                                                                                  : 0;
    }
    if (bytes != 8 * k) // padding: every byte of the value is defined
    {
        MirInst *z = inst(MIR_ZERO);
        z->a = block;
        z->imm = k;
    }
    MirInst *a = inst(MIR_ADDR);
    MirSlot addr = temp();
    a->d = addr;
    a->a = block;
    a->imm = k;
    for (NodeId f = NODE(u, id)->a; f; f = NEXT(u, f))
    {
        SymId member = u->refs[f];
        if (!member || SYM(member)->kind != SYM_FIELD)
        {
            continue; // the checker reported it
        }
        if (is_struct(field_type(member))) // a struct held in this one: its fields, at its offset
        {
            MirSlot from = lower_struct_addr(NODE(u, f)->a);
            copy_fields(addr, (uint32_t)field_offset(member), from, 0, type_ref(field_type(member)));
            continue;
        }
        MirSlot value = lower_as(NODE(u, f)->a, field_type(member));
        if (!field_stored(member))
        {
            continue; // computed (it may call something), but it has nowhere to go
        }
        MirInst *st = inst(MIR_STORE);
        st->var = (uint8_t)mem_variant(field_type(member));
        st->a = addr;
        st->b = value;
        st->imm = field_offset(member);
    }
}

// The same, in a block of its own: a member's value may read where the result goes.
static MirSlot struct_literal_block(NodeId id)
{
    MirSlot block = temp_n(slots_of(L.u->types[id]));
    struct_literal_at(id, block);
    return block;
}

// typeof(X) as a value into the k slots at dst: the descriptor's constants, or an *any's copied
// from its descriptor. 0 if id is not a typeof.
static int typeof_into(NodeId id, MirSlot dst, uint32_t k)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    uint64_t desc[4];
    TypeId named = sema_named_type(u, id);
    int a_type = named != T_NONE && sema_std_type() && u->types[id] == type_make(T_STRUCT, sema_std_type());
    if (a_type)
    {
        sema_type_desc(named, desc); // `describe(point)`: the type's descriptor
    }
    if (a_type || sema_typeof_desc(u, id, desc))
    {
        for (uint32_t j = 0; j < k && j < 4; j++)
        {
            into(emit_const(desc[j]), dst + j);
        }
        if (k > 4) // members: the descriptor's table (0 for a type with no fields)
        {
            TypeId t = a_type ? named : u->types[n->b];
            if (type_kind(t) == T_TYPE)
            {
                t = type_make(T_STRUCT, type_ref(t));
            }
            into(desc[3] ? desc_address(t, 5) : emit_const(0), dst + 4);
        }
        return 1;
    }
    if (n->kind != AST_CALL || sema_primitive(u, id) != PRIM_TYPEID || !n->b)
    {
        return 0;
    }
    MirSlot from = any_descriptor(n->b);
    for (uint32_t j = k; j-- > 0;)
    {
        MirInst *i = inst(MIR_LOAD);
        i->var = MV_U64;
        i->d = dst + j;
        i->a = from;
        i->imm = 8 * j;
    }
    return 1;
}

static void lower_struct_into(NodeId id, MirSlot dst)
{
    if (NODE(L.u, id)->kind == AST_INDEX) // an element of an array of structs: copied out, field by field
    {
        TypeId et = L.u->types[id];
        MirSlot src = lower_struct_addr(id);
        MirInst *a = inst(MIR_ADDR);
        a->d = temp();
        a->a = dst;
        a->imm = slots_of(et);
        copy_fields(a->d, 0, src, 0, type_ref(et));
        return;
    }
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    TypeId t = u->types[id];
    uint32_t k = slots_of(t);
    MirSlot base;
    if (typeof_into(id, dst, k))
    {
        return;
    }
    if (n->kind == AST_CALL && sema_primitive(u, id) == PRIM_LOAD && n->b) // load(S, p, offset): a struct
    {
        NodeId p = NEXT(u, n->b), off = NEXT(u, p);
        MirSlot at = lower_expr(p);
        uint64_t c;
        uint32_t imm = 0;
        if (const_value(off, &c) && c < 0x7fffffffull)
        {
            imm = (uint32_t)c;
        }
        else
        {
            MirSlot o = lower_as(off, T_U64);
            MirSlot sum = temp();
            MirInst *add = inst(MIR_BIN);
            add->sub = MB_ADD;
            add->var = MV_U64;
            add->d = sum;
            add->a = at;
            add->b = o;
            at = sum;
        }
        MirInst *z = inst(MIR_ZERO);
        z->a = dst;
        z->imm = k;
        MirInst *a = inst(MIR_ADDR);
        a->d = temp();
        a->a = dst;
        a->imm = k;
        copy_fields(a->d, 0, at, imm, type_ref(t));
        return;
    }
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
    if (n->kind == AST_STRUCT_LIT)
    {
        copy_slots(dst, struct_literal_block(id), k);
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
    SymId gvar;
    uint32_t leaf;
    if ((n->kind == AST_IDENT || n->kind == AST_SELECTOR) && global_path(id, &gvar, &leaf))
    {
        // a module-level struct variable (or a struct in one): its slots, field by field
        MirInst *z = inst(MIR_ZERO);
        z->a = dst;
        z->imm = k;
        MirInst *a = inst(MIR_ADDR);
        a->d = temp();
        a->a = dst;
        a->imm = k;
        global_load_fields(global_index(gvar) + leaf, a->d, 0, t);
        return;
    }
    if (n->kind == AST_UNARY_OP && TOK(u, n->tok)->kind == TOK_STAR) // `*p`: the struct p points to
    {
        MirSlot from = lower_expr(n->a);
        for (uint32_t j = k; j-- > 0;) // last slot first, as for self (dst may hold p)
        {
            MirInst *i = inst(MIR_LOAD);
            i->var = MV_U64;
            i->sub = struct_is_registers(t) ? MIR_MEM_DEVICE : 0;
            i->d = dst + j;
            i->a = from;
            i->imm = 8 * j;
        }
        return;
    }
    SymId held = n->kind == AST_SELECTOR ? u->refs[id] : 0;
    if (held && SYM(held)->kind == SYM_FIELD) // `b.corner`: the struct held in b, copied out
    {
        int ok;
        MirSlot outer = lower_field_base(n->a, n->tok, &ok);
        if (!ok)
        {
            return;
        }
        MirInst *z = inst(MIR_ZERO);
        z->a = dst;
        z->imm = k;
        MirInst *a = inst(MIR_ADDR);
        a->d = temp();
        a->a = dst;
        a->imm = k;
        copy_fields(a->d, 0, outer, (uint32_t)field_offset(held), type_ref(t));
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
    if (n->kind == AST_INDEX) // `people[i]`: the element where it is (a write through it reaches the array)
    {
        uint32_t off;
        MirSlot at = element_addr(id, &off);
        if (!off)
        {
            return at;
        }
        MirSlot moved = temp();
        bini(MB_ADD, MV_U64, moved, at, off);
        return moved;
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


// The end of a call whose arguments are computed: inlined, or copied into the window and called.
static void error_up(MirSlot err);
static int raises_nothing(SymId fn);
static NodeId arena_argument(NodeId call);
static void copy_fields(MirSlot dst, uint32_t doff, MirSlot src, uint32_t soff, SymId st);
static uint32_t struct_slots(SymId s);

static MirSlot finish_call(NodeId id, SymId fn, const Inlinable *inline_body, MirSlot *args, const uint32_t *argk,
                           const uint32_t *argw, int nargs)
{
    Unit *u = L.u;
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
    MirInst *call = inst(MIR_CALL);
    call->imm = fn;
    if (id && id == L.guard_call) // `n, e := f()`: a boundary, a panic below comes back here
    {
        TypeId rt = sema_fn_ret(u, fn);
        uint32_t k = rt == T_VOID ? 0 : slots_of(rt);
        if (k > 127)
        {
            unsupported(NODE(u, id)->tok, "a boundary on a call whose value takes more than 127 slots");
        }
        call->sub = MIR_CALL_GUARD;
        call->var = (uint8_t)(k << 1 | (sema_fn_fails(fn) ? 1u : 0u)); // (no slot field: passes rename those)
        MirSlot val = k > 1 ? temp_n(k) : temp(), err = temp();
        if (k > 1)
        {
            copy_slots(val, SLOT_WIN | 0, k); // a struct value: its slots
        }
        else if (k)
        {
            into(SLOT_WIN | 0, val);
        }
        into(SLOT_WIN | k, err);
        L.handled_error = err;
        return val;
    }
    if (sema_fn_fails(fn)) // the value, then its error in the next slot (guards.md §5)
    {
        TypeId rt = sema_fn_ret(u, fn);
        uint32_t k = rt == T_VOID ? 0 : slots_of(rt);
        MirSlot val = k > 1 ? temp_n(k) : temp(), err = temp();
        if (k > 1)
        {
            copy_slots(val, SLOT_WIN | 0, k); // a struct value: its slots
        }
        else if (k)
        {
            into(SLOT_WIN | 0, val);
        }
        into(SLOT_WIN | k, err);
        if (id && id == L.handled_call)
        {
            L.handled_error = err; // `n, e := f()`: the binding takes it
        }
        else
        {
            error_up(err); // `n := f()`: an error goes up
        }
        return val;
    }
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

// A bound call's arguments (`a := f(x)`, then `a()`), as f's window takes them: the receiver,
// then each parameter (a struct: its slots). Returns the slots they take in all.
static uint32_t bound_args(SymId fn, uint32_t *argk, uint32_t *argw, int *nargs)
{
    Unit *u = L.u;
    uint32_t win = 0;
    int n = 0;
    if (SYM(fn)->owner)
    {
        argk[n] = 1;
        argw[n++] = win++;
    }
    const Unit *home = &cx.units[SYM(fn)->unit];
    for (NodeId p = sema_fn_params(u, fn); p && n < 256; p = NEXT(home, p))
    {
        TypeId pt = sema_param_type(u, fn, p);
        uint32_t k = is_struct(pt) ? slots_of(pt) : 1;
        argk[n] = k;
        argw[n++] = win;
        win += k;
    }
    *nargs = n;
    return win;
}

// `a := f(x)` that the body calls as `a()`: the arguments now, in a block of the frame; no call.
static void lower_bound_args(NodeId define, uint32_t top)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, define);
    const AstNode *call = NODE(u, n->a);
    const AstNode *callee = NODE(u, call->a);
    SymId fn = u->refs[n->a];
    uint32_t argk[257], argw[257];
    int na;
    uint32_t k = bound_args(fn, argk, argw, &na);
    MirSlot block = temp_n(k ? k : 1);
    int i = 0;
    if (SYM(fn)->owner)
    {
        MirSlot r = callee->kind == AST_SELECTOR
                        ? lower_as(callee->a, type_make(T_PTR, type_make(T_STRUCT, SYM(fn)->owner)))
                        : 0; // a bare member name, through implicit self
        into(r, block);
        i = 1;
    }
    const Unit *home = &cx.units[SYM(fn)->unit];
    NodeId param = sema_fn_params(u, fn);
    for (NodeId a = call->b; a && param && i < na; a = NEXT(u, a), param = NEXT(home, param), i++)
    {
        TypeId pt = sema_param_type(u, fn, param);
        if (is_struct(pt))
        {
            lower_struct_into(a, block + argw[i]);
        }
        else
        {
            into(lower_as(a, pt), block + argw[i]);
        }
    }
    push_local_n(token_name(TOK(u, n->tok)), block, k ? k : 1);
    L.top = top + (k ? k : 1);
}

// `a()`: f with the arguments its binding computed (copied: an inlined body may write them).
static MirSlot lower_bound_call(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    SymId fn = u->refs[id];
    MirSlot block;
    if (!find_local(token_name(TOK(u, NODE(u, n->a)->tok)), &block))
    {
        unsupported(n->tok, "this bound call");
        return temp();
    }
    if (cx.audit)
    {
        audit_call(L.f->sym, fn);
    }
    const Inlinable *inline_body = inlinable_for(fn);
    if (inline_body && inline_body->n > INLINE_MAX && !L.loops)
    {
        inline_body = NULL;
    }
    MirSlot args[257];
    uint32_t argk[257], argw[257];
    int na;
    bound_args(fn, argk, argw, &na);
    for (int i = 0; i < na; i++)
    {
        args[i] = block + argw[i];
        if (inline_body)
        {
            MirSlot c = argk[i] == 1 ? temp() : temp_n(argk[i]);
            if (argk[i] == 1)
            {
                into(args[i], c);
            }
            else
            {
                copy_slots(c, args[i], argk[i]);
            }
            args[i] = c;
        }
    }
    return finish_call(id, fn, inline_body, args, argk, argw, na);
}

// What a call to fn passes after its arguments, and under which name the caller has each: a
// literal's captures (the caller's own name, or, in a specialization made for that literal, the
// name it keeps them under), then those of the literals a specialization fn was made for.
typedef struct
{
    NameId name;
    TypeId type;
} CaptureFrom;

static uint32_t captures_passed(SymId fn, Capture *caps, CaptureFrom *out, uint32_t cap)
{
    uint32_t n = 0;
    SymId mine[16]; // the literals the function being lowered was made for
    uint32_t nm = sema_spec_functions(L.f->sym, mine, 16);
    SymId lits[17];
    uint32_t nl = 0;
    lits[nl++] = fn;
    SymId given[16];
    uint32_t ng = sema_spec_functions(fn, given, 16);
    for (uint32_t g = 0; g < ng && nl < 17; g++)
    {
        lits[nl++] = given[g];
    }
    for (uint32_t l = 0; l < nl; l++)
    {
        int kept = 0; // this caller keeps the literal's captures under its own names
        for (uint32_t m = 0; m < nm && !kept; m++)
        {
            kept = mine[m] == lits[l];
        }
        uint32_t c = sema_captures(lits[l], caps, 64);
        for (uint32_t i = 0; i < c && n < cap; i++)
        {
            out[n].name = kept ? sema_capture_name(lits[l], caps[i].name) : caps[i].name;
            out[n++].type = caps[i].type;
        }
    }
    return n;
}

static MirSlot lower_call(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    const AstNode *callee = NODE(u, n->a);
    NodeId arglist = n->b;
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
    if (sema_primitive(u, id) == PRIM_TYPEID) // a typeof the struct paths did not take: in a block
    {
        uint32_t k = slots_of(u->types[id]);
        MirSlot block = temp_n(k);
        typeof_into(id, block, k);
        return block;
    }
    if (sema_primitive(u, id) == PRIM_LOAD && n->b) // load(T, p, offset): the T at p + offset
    {
        TypeId t = u->types[id];
        NodeId p = NEXT(u, n->b), off = NEXT(u, p);
        if (is_struct(t)) // a struct: into a block of its own, field by field
        {
            uint32_t k = slots_of(t);
            MirSlot block = temp_n(k);
            lower_struct_into(id, block);
            return block;
        }
        MirSlot at = lower_expr(p);
        uint64_t c;
        uint64_t imm = 0;
        if (const_value(off, &c) && c < 0x7fffffffull)
        {
            imm = c;
        }
        else
        {
            MirSlot o = lower_as(off, T_U64);
            MirSlot sum = temp();
            MirInst *add = inst(MIR_BIN);
            add->sub = MB_ADD;
            add->var = MV_U64;
            add->d = sum;
            add->a = at;
            add->b = o;
            at = sum;
        }
        MirInst *ld = inst(MIR_LOAD);
        ld->var = (uint8_t)mem_variant(t);
        ld->d = temp();
        ld->a = at;
        ld->imm = imm;
        return ld->d;
    }
    if (sema_primitive(u, id) == PRIM_UNANY && n->b) // point(a): a's address, when a holds a point
    {
        MirSlot p = lower_expr(n->b);
        MirInst *dl = inst(MIR_LOAD); // the descriptor's address, then its id
        dl->var = MV_U64;
        dl->d = temp();
        dl->a = p;
        dl->imm = 8;
        MirSlot desc = dl->d;
        MirSlot have = temp();
        MirInst *ld = inst(MIR_LOAD);
        ld->var = MV_U64;
        ld->d = have;
        ld->a = desc;
        ld->imm = 0;
        MirSlot want = emit_const(type_make(T_STRUCT, u->refs[id])); // before the compare reads it
        MirSlot same = temp();
        MirInst *eq = inst(MIR_BIN);
        eq->sub = MB_EQ;
        eq->var = MV_U64;
        eq->d = same;
        eq->a = have;
        eq->b = want;
        uint32_t ok = new_label();
        MirInst *j = inst(MIR_JNZ);
        j->a = same;
        j->imm = ok;
        int line, col;
        source_pos(u->src, TSTART(TOK(u, n->tok)), &line, &col);
        char place[540], shown[540], text[600];
        int pl = snprintf(place, sizeof(place), "%s:%d:%d", u->path, line, col);
        trap_place(place, pl < 0 ? 0 : (size_t)pl >= sizeof(place) ? sizeof(place) - 1 : (size_t)pl, shown,
                   sizeof(shown));
        int tl = snprintf(text, sizeof(text), "%s: the *any holds another type\n", shown); // no type names
        MirSlot msg = string_slot((const uint8_t *)text, (size_t)(tl < 0 ? 0 : tl));
        MirInst *tr = inst(MIR_TRAP);
        tr->a = msg;
        tr->imm = PANIC_ANYTYPE;
        L.f->strings[L.f->nstrings - 1].panic = (uint8_t)tr->imm; // (its message: made just before)
        place_label(ok);
        MirInst *at = inst(MIR_LOAD);
        at->var = MV_U64;
        at->d = temp();
        at->a = p;
        at->imm = 0;
        return at->d;
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
        MirInst *tr = inst(MIR_TRAP);
        tr->a = last;
        int al;
        const char *an = symbol_name(L.f->sym, &al); // std's assert stops with panic.assert
        tr->imm = al == 6 && memcmp(an, "assert", 6) == 0 && strstr(cx.units[SYM(L.f->sym)->unit].path, "std/")
                      ? PANIC_ASSERT
                      : PANIC_RAISED;
        L.f->strings[L.f->nstrings - 1].panic = (uint8_t)tr->imm; // (its message: made just before)
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
    if (sema_primitive(u, id) == PRIM_BOUND)
    {
        return lower_bound_call(id);
    }
    if (!fn || SYM(fn)->kind != SYM_METHOD)
    {
        unsupported(callee->tok, "calling something that is not a Modal function");
        return temp();
    }
    if (cx.audit)
    {
        audit_call(L.f->sym, fn); // inlined or not: what the caller reaches
    }

    // Each argument is computed straight into its window slot when nothing after it calls
    // anything (a call would reuse the window); otherwise into a temporary, copied at the end.
    // A call that is inlined keeps its arguments where they are computed.
    const Inlinable *inline_body = inlinable_for(fn);
    if (inline_body && ((inline_body->n > INLINE_MAX && !L.loops) || id == L.guard_call))
    {
        inline_body = NULL; // larger than a call outside a loop is worth; or a boundary
    }
    MirSlot args[256];
    uint32_t argk[256], argw[256]; // slots each argument takes, and where in the window
    uint32_t win = 0;
    int nargs = 0;
    NodeId later_args = arglist;
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
    for (NodeId a = arglist; a && param; a = NEXT(u, a), param = NEXT(home, param))
    {
        if (nargs == 256)
        {
            unsupported(NODE(u, a)->tok, "a call with more than 256 arguments");
            break;
        }
        TypeId pt = sema_param_type(u, fn, param);
        if (type_kind(pt) == T_FUNC)
        {
            continue; // the function given: the specialization calls it by name, nothing is passed
        }
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
        MirSlot r = is_array(pt) ? array_addr(a, NODE(u, a)->tok) : lower_as(a, pt); // an array: its address
        args[nargs] = calls_after || inline_body ? r : into(r, SLOT_WIN | win);
        argk[nargs] = 1;
        win++;
        nargs++;
    }
    {
        // a literal's captures, after the arguments: the caller's locals of those names (in a
        // specialization made for that literal, its own copies); then, for a specialization made
        // for literals that read their callers' locals, those
        Capture caps[64];
        CaptureFrom from_caps[96];
        uint32_t nc = captures_passed(fn, caps, from_caps, 96);
        for (uint32_t i = 0; i < nc && nargs < 256; i++)
        {
            MirSlot from;
            int ref;
            if (!find_local_ref(from_caps[i].name, &from, &ref) || ref)
            {
                unsupported(callee->tok, "a literal that reads a local this call cannot see");
                break;
            }
            uint32_t ck = slots_of(from_caps[i].type);
            if (ck > 1) // a struct: its slots, copied
            {
                MirSlot b = inline_body ? temp_n(ck) : (SLOT_WIN | win);
                copy_slots(b, from, ck);
                args[nargs] = b;
                argk[nargs] = ck;
                argw[nargs] = win;
                win += ck;
                nargs++;
                continue;
            }
            MirSlot r = temp();
            into(from, r);
            args[nargs] = inline_body ? r : into(r, SLOT_WIN | win);
            argk[nargs] = 1;
            argw[nargs] = win++;
            nargs++;
        }
    }
    return finish_call(id, fn, inline_body, args, argk, argw, nargs);
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
    char text[600], place[540];
    int p = snprintf(place, sizeof(place), "%s:%d:%d", u->path, line, col);
    char shown[540];
    trap_place(place, p < 0 ? 0 : (size_t)p >= sizeof(place) ? sizeof(place) - 1 : (size_t)p, shown, sizeof(shown));
    int n = snprintf(text, sizeof(text), "%s: division by zero\n", shown);
    n = n < 0 ? 0 : n >= (int)sizeof(text) ? (int)sizeof(text) - 1 : n;
    uint32_t ok = new_label();
    MirInst *j = inst(MIR_JNZ);
    j->a = b;
    j->imm = ok;
    MirSlot m = string_slot((const uint8_t *)text, (size_t)n);
    MirInst *tr = inst(MIR_TRAP);
    tr->a = m;
    tr->imm = PANIC_DIVISION;
    L.f->strings[L.f->nstrings - 1].panic = (uint8_t)tr->imm; // (its message: made just before)
    place_label(ok);
}

// ---- arrays (std.md §9) ---------------------------------------------------------------------------
//
// An array local is a block of slots holding its elements packed, as a struct holds its fields; a
// parameter holds its address. a[i] is a load or a store of the element's width at base + i *
// size. An index is checked against the length unless the checker proved it (a literal, or a
// local an arm knows is in range: INDEX_SAFE); out of range, the program stops with the place.

static MirSlot array_addr(NodeId arr, uint32_t tok)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, arr);
    if (n->kind == AST_PAREN_GROUP)
    {
        return array_addr(n->a, tok);
    }
    MirSlot base;
    int ref;
    if (type_kind(u->types[arr]) == T_PTR) // `p: *[N]T`: the address it holds
    {
        return lower_expr(arr);
    }
    if (n->kind == AST_IDENT && !u->refs[arr] && find_local_ref(token_name(TOK(u, n->tok)), &base, &ref))
    {
        if (ref)
        {
            return base;
        }
        MirInst *a = inst(MIR_ADDR);
        a->d = temp();
        a->a = base;
        a->imm = slots_of(u->types[arr]);
        return a->d;
    }
    unsupported(tok, "indexing an array that is not a local or a parameter");
    return temp();
}

// An array local from `src` (`c := a`, `b: [4]u8 = a`): its own copy, a slot at a time (array
// storage is always whole slots).
static void copy_array_into(NodeId src, MirSlot block, uint32_t k)
{
    MirSlot from = array_addr(src, NODE(L.u, src)->tok);
    for (uint32_t j = 0; j < k; j++)
    {
        MirInst *ld = inst(MIR_LOAD);
        ld->var = MV_U64;
        ld->d = block + j;
        ld->a = from;
        ld->imm = 8 * j;
    }
}

static void guard_index(MirSlot i, uint64_t len, uint32_t tok)
{
    Unit *u = L.u;
    int line, col;
    source_pos(u->src, TSTART(TOK(u, tok)), &line, &col);
    char text[600], place[540], shown[540];
    int p = snprintf(place, sizeof(place), "%s:%d:%d", u->path, line, col);
    trap_place(place, p < 0 ? 0 : (size_t)p >= sizeof(place) ? sizeof(place) - 1 : (size_t)p, shown, sizeof(shown));
    int n = snprintf(text, sizeof(text), "%s: index out of range (0 to %llu)\n", shown,
                     (unsigned long long)(len - 1));
    n = n < 0 ? 0 : n >= (int)sizeof(text) ? (int)sizeof(text) - 1 : n;
    uint32_t ok = new_label();
    MirSlot t = temp();
    if (len < 0x10000)
    {
        bini(MB_LT, MV_U64, t, i, len);
    }
    else
    {
        MirSlot n = emit_const(len); // before the compare that reads it
        MirInst *c = inst(MIR_BIN);
        c->sub = MB_LT;
        c->var = MV_U64;
        c->d = t;
        c->a = i;
        c->b = n;
    }
    MirInst *j = inst(MIR_JNZ);
    j->a = t;
    j->imm = ok;
    MirSlot msg = string_slot((const uint8_t *)text, (size_t)n);
    MirInst *tr = inst(MIR_TRAP);
    tr->a = msg;
    tr->imm = PANIC_INDEX;
    L.f->strings[L.f->nstrings - 1].panic = (uint8_t)tr->imm; // (its message: made just before)
    place_label(ok);
}

// The address of a[i], and a constant offset to add to it (a literal index folds there).
static MirSlot element_addr(NodeId id, uint32_t *off)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    TypeId at = u->types[n->a];
    *off = 0;
    if (at == T_STR) // a []u8: its length, then its bytes
    {
        MirSlot s = lower_expr(n->a);
        MirSlot i = lower_as(n->b, T_U64);
        MirSlot len = temp();
        MirInst *ld = inst(MIR_LOAD);
        ld->var = MV_U64;
        ld->d = len;
        ld->a = s;
        uint32_t ok = new_label();
        MirSlot t = temp();
        MirInst *c = inst(MIR_BIN);
        c->sub = MB_LT;
        c->var = MV_U64;
        c->d = t;
        c->a = i;
        c->b = len;
        MirInst *j = inst(MIR_JNZ);
        j->a = t;
        j->imm = ok;
        int line, col;
        source_pos(u->src, TSTART(TOK(u, n->tok)), &line, &col);
        char text[600], place[540], shown[540];
        int p = snprintf(place, sizeof(place), "%s:%d:%d", u->path, line, col);
        trap_place(place, p < 0 ? 0 : (size_t)p >= sizeof(place) ? sizeof(place) - 1 : (size_t)p, shown,
                   sizeof(shown));
        int k = snprintf(text, sizeof(text), "%s: index out of range (past the string's end)\n", shown);
        k = k < 0 ? 0 : k >= (int)sizeof(text) ? (int)sizeof(text) - 1 : k;
        MirSlot msg = string_slot((const uint8_t *)text, (size_t)k);
        MirInst *tr = inst(MIR_TRAP);
        tr->a = msg;
        tr->imm = PANIC_INDEX;
        L.f->strings[L.f->nstrings - 1].panic = (uint8_t)tr->imm; // (its message: made just before)
        place_label(ok);
        MirSlot addr = temp();
        MirInst *add = inst(MIR_BIN);
        add->sub = MB_ADD;
        add->var = MV_U64;
        add->d = addr;
        add->a = s;
        add->b = i;
        *off = 8;
        return addr;
    }
    MirSlot base = array_addr(n->a, n->tok);
    at = type_kind(at) == T_PTR ? type_ref(at) : at;
    uint32_t size = type_layout(type_ref(at)).size;
    uint64_t len = type_len(at);
    const AstNode *ix = NODE(u, n->b);
    uint64_t k;
    if (ix->kind == AST_NUMBER_LIT &&
        number_value(TSTART(TOK(u, ix->tok)), TOK(u, ix->tok)->len, &k, NULL) && k < len)
    {
        *off = (uint32_t)(k * size); // checked by the checker: in range
        return base;
    }
    MirSlot i = lower_as(n->b, T_U64);
    if (!(n->aux & INDEX_SAFE))
    {
        guard_index(i, len, n->tok);
    }
    MirSlot o = i;
    if (size != 1)
    {
        o = temp();
        bini(MB_MUL, MV_U64, o, i, size);
    }
    MirSlot addr = temp();
    MirInst *add = inst(MIR_BIN);
    add->sub = MB_ADD;
    add->var = MV_U64;
    add->d = addr;
    add->a = base;
    add->b = o;
    return addr;
}

// Every use of `name` from `id` on (a list when `list`) is `name.field`, read or written: the
// local never goes anywhere whole, so its fields can be locals of their own. Unknown shapes say no.
static int fields_only(NodeId id, NameId name, int list);
static int fields_only_node(NodeId id, NameId name)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    switch (n->kind)
    {
    case AST_IDENT:
        return token_name(TOK(u, n->tok)) != name;
    case AST_SELECTOR:
        if (NODE(u, n->a)->kind == AST_IDENT && token_name(TOK(u, NODE(u, n->a)->tok)) == name)
        {
            SymId m = u->refs[id];
            return m && SYM(m)->kind == SYM_FIELD && !is_struct(field_type(m));
        }
        return fields_only_node(n->a, name);
    case AST_NUMBER_LIT:
    case AST_STRING_LIT:
    case AST_SITE:
        return 1;
    case AST_PATTERN:
        return (!n->a || fields_only_node(n->a, name)) && (!n->b || fields_only_node(n->b, name));
    case AST_BLOCK:
    case AST_STRUCT_LIT:
        return fields_only(n->a, name, 1);
    case AST_CALL:
        return fields_only_node(n->a, name) && fields_only(n->b, name, 1);
    case AST_MATCH:
        return fields_only(n->a, name, 1) && fields_only(n->b, name, 1);
    case AST_ARM:
        return fields_only(n->a, name, 1) && (!n->b || fields_only_node(n->b, name));
    case AST_IF:
    {
        const AstNode *br = NODE(u, n->b);
        return fields_only_node(n->a, name) && (!br->a || fields_only_node(br->a, name)) &&
               (!br->b || fields_only_node(br->b, name));
    }
    case AST_FIELD_INIT:
    case AST_RETURN:
    case AST_UNARY_OP:
    case AST_PAREN_GROUP:
    case AST_DEFINE:
        return !n->a || fields_only_node(n->a, name);
    case AST_FIELD_DECL:
        return !(n->flags & AST_F_PLACED) && (!n->b || fields_only_node(n->b, name)) &&
               token_name(TOK(u, n->tok)) != name; // a new `name` here: give up
    case AST_ASSIGN:
    case AST_BIN_OP:
    case AST_INDEX:
        return fields_only_node(n->a, name) && fields_only_node(n->b, name);
    default:
        return 0; // a function literal (it may capture), or a shape this walk does not know
    }
}

static int fields_only(NodeId id, NameId name, int list)
{
    for (; id; id = list ? NEXT(L.u, id) : 0)
    {
        if (!fields_only_node(id, name))
        {
            return 0;
        }
    }
    return 1;
}

// Where field `member` sits among its struct's fields (declaration order), the slot it takes
// in a LOCAL_FIELDS local.
static uint32_t field_index(SymId member)
{
    const Unit *home = &cx.units[SYM(member)->unit];
    uint32_t i = 0;
    for (NodeId m = NODE(home, SYM(SYM(member)->owner)->decl)->a; m; m = NEXT(home, m))
    {
        if (m == SYM(member)->decl)
        {
            return i;
        }
        i += NODE(home, m)->kind == AST_FIELD_DECL;
    }
    return i;
}

static uint32_t field_count(TypeId t)
{
    SymId s = type_ref(t);
    const Unit *home = &cx.units[SYM(s)->unit];
    uint32_t i = 0;
    for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
    {
        i += NODE(home, m)->kind == AST_FIELD_DECL;
    }
    return i;
}

// `p := point{ x, y }` (or `p: point = ...`), statement `id`, when every later use is `p.field`:
// each field in a slot of its own. 0 when the local must be a block.
static int lower_fields_local(NodeId id, NodeId lit, uint32_t top)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    NameId name = token_name(TOK(u, n->tok));
    if (L.rest_of != id || !fields_only(L.rest, name, 1) || !fields_only_node(lit, name))
    {
        return 0;
    }
    for (NodeId f = NODE(u, lit)->a; f; f = NEXT(u, f))
    {
        if (!u->refs[f] || is_struct(field_type(u->refs[f])))
        {
            return 0;
        }
    }
    uint32_t k = field_count(u->types[lit]);
    MirSlot values[64];
    uint32_t at[64];
    int nv = 0;
    for (NodeId f = NODE(u, lit)->a; f && nv < 64; f = NEXT(u, f), nv++) // every value, then the slots
    {
        values[nv] = lower_as(NODE(u, f)->a, field_type(u->refs[f]));
        at[nv] = field_index(u->refs[f]);
    }
    MirSlot slot = temp_n(k);
    for (int i = 0; i < nv; i++)
    {
        into(values[i], slot + at[i]);
    }
    push_local_n(name, slot, k);
    L.locals[L.nlocals - 1].ref = LOCAL_FIELDS;
    L.top = top + k;
    return 1;
}

// `p.x` of a LOCAL_FIELDS local: the slot x lives in, or (MirSlot)-1.
static MirSlot fields_local_slot(NodeId selector)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, selector);
    const AstNode *b = NODE(u, n->a);
    MirSlot slot;
    int ref;
    SymId m = u->refs[selector];
    if (b->kind != AST_IDENT || u->refs[n->a] || !m || SYM(m)->kind != SYM_FIELD ||
        !find_local_ref(token_name(TOK(u, b->tok)), &slot, &ref) || ref != LOCAL_FIELDS)
    {
        return (MirSlot)-1;
    }
    return slot + field_index(m);
}

static MirSlot lower_field_base(NodeId base, uint32_t tok, int *ok)
{
    Unit *u = L.u;
    *ok = 1;
    if (is_self_ident(base) && L.has_self)
    {
        return 0;
    }
    const AstNode *b = NODE(u, base);
    if (b->kind == AST_CALL && sema_primitive(u, base) == PRIM_TYPEID && b->b && sema_is_any_ref(u->types[b->b]))
    {
        return any_descriptor(b->b); // `typeof(a).id`: read in place
    }
    SymId inner = b->kind == AST_SELECTOR ? u->refs[base] : 0;
    if (inner && SYM(inner)->kind == SYM_FIELD && is_struct(field_type(inner)))
    {
        // `b.corner.x`: corner's address is b's plus its offset, so a write reaches b itself
        MirSlot outer = lower_field_base(b->a, tok, ok);
        if (!*ok)
        {
            return outer;
        }
        MirSlot at = temp();
        bini(MB_ADD, MV_U64, at, outer, field_offset(inner));
        return at;
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
    case AST_IF: // the condition, a branch to `other` when false, `then`, a jump over `other`
    {
        const AstNode *br = NODE(u, n->b);
        uint32_t other = new_label(), end = br->b ? new_label() : other;
        uint32_t top = L.top;
        MirSlot cond = lower_as(n->a, T_BOOL);
        MirInst *j = inst(MIR_JZ);
        j->a = cond;
        j->imm = other;
        L.top = top;
        lower_stmt(br->a);
        if (br->b)
        {
            if (!L.f->n || !MIR_NO_FALLTHROUGH(&L.f->code[L.f->n - 1]))
            {
                inst(MIR_JUMP)->imm = end;
            }
            place_label(other);
            lower_stmt(br->b);
        }
        place_label(end);
        return temp();
    }
    case AST_NUMBER_LIT:
        unsupported(n->tok, "a fractional number");
        return temp();
    case AST_STRING_LIT:
        return lower_string(t);
    case AST_SITE:
    {
        char text[600], shown[600];
        int k = sema_site_text(u, id, text, sizeof(text));
        size_t n = trap_place(text, (size_t)k, shown, sizeof(shown)); // `trap N` in a binary
        return string_slot((const uint8_t *)shown, n);
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
            if (!field_stored(s))
            {
                return field_constant(s);
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
        int ref = 0;
        if (find_local_ref(token_name(t), &slot, &ref))
        {
            if (ref == LOCAL_BOXED)
            {
                MirInst *a = inst(MIR_ADDR);
                a->d = temp();
                a->a = slot;
                a->imm = 1;
                MirInst *ld = inst(MIR_LOAD);
                ld->var = (uint8_t)mem_variant(type);
                ld->d = temp();
                ld->a = a->d;
                return ld->d;
            }
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
        if (t->kind == TOK_AMP && is_array(u->types[n->a])) // &a: the array's address
        {
            return array_addr(n->a, n->tok);
        }
        if (t->kind == TOK_AMP && NODE(u, n->a)->kind == AST_IDENT && !u->refs[n->a] &&
            !is_struct(u->types[n->a]) && !is_array(u->types[n->a]) && !is_self_ident(n->a))
        {
            // &n of a number local: its slot, which is memory from now on (LOCAL_BOXED)
            NameId name = token_name(TOK(u, NODE(u, n->a)->tok));
            for (int i = L.nlocals - 1; i >= 0; i--)
            {
                if (L.locals[i].name == name)
                {
                    if (L.locals[i].ref == 0)
                    {
                        L.locals[i].ref = LOCAL_BOXED;
                    }
                    if (L.locals[i].ref != LOCAL_BOXED)
                    {
                        break;
                    }
                    MirInst *a = inst(MIR_ADDR);
                    a->d = temp();
                    a->a = L.locals[i].slot;
                    a->imm = 1;
                    return a->d;
                }
            }
        }
        SymId gref;
        uint32_t gleaf;
        if (t->kind == TOK_AMP && global_path(n->a, &gref, &gleaf))
        {
            unsupported(n->tok, "a reference to a module-level struct variable (its fields live in "
                                "slots of their own)");
            return temp();
        }
        if (t->kind == TOK_AMP && is_struct(u->types[n->a]) &&
            ((NODE(u, n->a)->kind == AST_IDENT && !u->refs[n->a]) || NODE(u, n->a)->kind == AST_STRUCT_LIT))
            // &p of a struct local: its own block, so writes through it reach p. &T{ ... }: a
            // block of the frame (lower_struct_addr), which lives as long as the function
        {
            return lower_struct_addr(n->a);
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
        SymId gvar;
        uint32_t leaf;
        if (member && SYM(member)->kind == SYM_FIELD && !is_struct(field_type(member)) &&
            global_path(id, &gvar, &leaf)) // `g.a.x` of a module-level struct variable: its slot
        {
            if (!field_stored(member))
            {
                return field_constant(member);
            }
            MirInst *i = inst(MIR_GLOAD);
            i->d = temp();
            i->imm = global_index(gvar) + leaf;
            return i->d;
        }
        MirSlot own = fields_local_slot(id);
        if (own != (MirSlot)-1)
        {
            return own; // a field of a LOCAL_FIELDS local: its own slot
        }
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
        if (!field_stored(member))
        {
            if (contains_call(n->a))
            {
                lower_expr(n->a); // `make().x`: the call still happens
            }
            return field_constant(member); // the value is never in memory
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
        if (target->kind == AST_INDEX && is_struct(u->types[n->a])) // people[i] = person{ ... }: field by field
        {
            MirSlot value = lower_struct_addr(n->b); // computed first: it may read the array
            uint32_t off;
            MirSlot addr = element_addr(n->a, &off);
            copy_fields(addr, off, value, 0, type_ref(u->types[n->a]));
            return temp();
        }
        if (target->kind == AST_INDEX) // a[i] = v: the element, in its width
        {
            MirSlot value = lower_as(n->b, u->types[n->a]);
            uint32_t off;
            MirSlot addr = element_addr(n->a, &off);
            MirInst *st = inst(MIR_STORE);
            st->var = (uint8_t)mem_variant(u->types[n->a]);
            st->a = addr;
            st->b = value;
            st->imm = off;
            return value;
        }
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
        SymId gvar;
        uint32_t leaf;
        if ((target->kind == AST_IDENT || target->kind == AST_SELECTOR) && global_path(n->a, &gvar, &leaf))
        {
            TypeId tt = u->types[n->a];
            if (is_struct(tt)) // `g = p`, `g.a = q`: every field of the value into its slot
            {
                MirSlot from = lower_struct_addr(n->b);
                global_store_fields(global_index(gvar) + leaf, from, 0, tt);
                return temp();
            }
            MirSlot value = lower_as(n->b, tt);
            if (s && SYM(s)->kind == SYM_FIELD && !field_stored(s))
            {
                return value;
            }
            MirInst *g = inst(MIR_GSTORE);
            g->a = value;
            g->imm = global_index(gvar) + leaf;
            return value;
        }
        if ((target->kind == AST_IDENT || target->kind == AST_SELECTOR) && is_global(s, &tinit))
        {
            MirSlot value = lower_as(n->b, u->types[n->a]);
            MirInst *i = inst(MIR_GSTORE);
            i->a = value;
            i->imm = global_index(s);
            return value;
        }
        MirSlot own = target->kind == AST_SELECTOR ? fields_local_slot(n->a) : (MirSlot)-1;
        if (own != (MirSlot)-1)
        {
            return into(lower_as(n->b, u->types[n->a]), own);
        }
        if (target->kind == AST_SELECTOR || (target->kind == AST_IDENT && s && SYM(s)->kind == SYM_FIELD))
        {
            MirSlot base = 0;
            int ok = 1;
            if (target->kind == AST_SELECTOR)
            {
                base = lower_field_base(target->a, target->tok, &ok);
            }
            if (s && SYM(s)->kind == SYM_FIELD && is_struct(field_type(s))) // `b.corner = g`
            {
                if (!ok || target->kind != AST_SELECTOR)
                {
                    unsupported(target->tok, "assigning to this");
                    return temp();
                }
                MirSlot from = lower_struct_addr(n->b);
                copy_fields(base, (uint32_t)field_offset(s), from, 0, type_ref(field_type(s)));
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
            if (!field_stored(s))
            {
                return value; // nothing reads it, or it holds this constant already
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
    {
        uint32_t off;
        MirSlot addr = element_addr(id, &off);
        MirInst *ld = inst(MIR_LOAD);
        ld->var = (uint8_t)mem_variant(u->types[id]);
        ld->d = temp();
        ld->a = addr;
        ld->imm = off;
        return ld->d;
    }
    default:
        unsupported(n->tok, "this expression");
        return temp();
    }
}

// ---- statements and functions -----------------------------------------------------------------

static MirSlot lower_match_as(NodeId id, TypeId want, int returned);

// An error not handled here: when it is not nil, this function returns it (its value at zero).
static void error_up(MirSlot err)
{
    if (!L.fails && L.ret != T_ERROR)
    {
        // nowhere to go up to: a module-level value, which the compiler computes; an error there
        // stops it (`'b' cannot be computed by the compiler: ... gives an error`)
        uint32_t ok = new_label();
        MirInst *j = inst(MIR_JZ);
        j->a = err;
        j->imm = ok;
        const char *text = "gets an error from a call, and nothing here handles it (at module level there is no `, e`)\n";
        MirSlot msg = string_slot((const uint8_t *)text, strlen(text));
        MirInst *tr = inst(MIR_TRAP);
        tr->a = msg;
        tr->imm = PANIC_UNHANDLED;
        L.f->strings[L.f->nstrings - 1].panic = (uint8_t)tr->imm;
        place_label(ok);
        return;
    }
    uint32_t ok = new_label();
    MirInst *j = inst(MIR_JZ);
    j->a = err;
    j->imm = ok;
    uint32_t k = L.fails && L.ret != T_VOID ? slots_of(L.ret) : 0;
    if (k)
    {
        MirInst *z = inst(MIR_ZERO);
        z->a = 0;
        z->imm = k;
    }
    into(err, k);
    inst(MIR_RET);
    place_label(ok);
}

static void lower_return(NodeId value)
{
    const AstNode *v = value ? NODE(L.u, value) : NULL;
    if (L.fails) // `return x` gives no error; `return bad_input` an error and the value at zero
    {
        uint32_t k = L.ret == T_VOID ? 0 : slots_of(L.ret);
        if (value && L.u->types[value] == T_ERROR && L.ret != T_ERROR)
        {
            MirSlot err = temp();
            into(lower_as(value, T_ERROR), err);
            if (k)
            {
                MirInst *z = inst(MIR_ZERO);
                z->a = 0;
                z->imm = k;
            }
            into(err, k);
        }
        else
        {
            if (value && is_struct(L.ret))
            {
                lower_struct_into(value, 0); // a struct value: its slots, then no error
            }
            else if (value)
            {
                into(lower_as(value, L.ret), 0);
            }
            into(emit_const(0), k);
        }
        inst(MIR_RET);
        return;
    }
    if (v && v->kind == AST_MATCH && v->a && L.ret != T_VOID && !is_struct(L.ret))
    {
        // `return if n { 0: acc, _: s(n - 1, acc + n) }`: each arm returns its own value, so a call
        // there is a tail call (a jump), as in a body of arms.
        lower_match_as(value, L.ret, 1);
        return;
    }
    const AstNode *lit = v && v->kind == AST_UNARY_OP && TOK(L.u, v->tok)->kind == TOK_AMP ? NODE(L.u, v->a) : NULL;
    if (lit && lit->kind == AST_STRUCT_LIT && L.has_self)
    {
        // `return &point{ x, y }` in a `self: point` method: built in self's storage (the
        // caller's fresh value, at zero), and self's reference, already in slot 0, is the result.
        // Every value is computed before the first store: a value may read self.
        NodeId lid = v->a;
        Unit *u = L.u;
        MirSlot values[64];
        SymId members[64];
        int nv = 0;
        for (NodeId f = NODE(u, lid)->a; f; f = NEXT(u, f))
        {
            SymId member = u->refs[f];
            if (!member || SYM(member)->kind != SYM_FIELD || nv == 64)
            {
                continue;
            }
            values[nv] = is_struct(field_type(member)) ? lower_struct_addr(NODE(u, f)->a) // copied below
                                                       : lower_as(NODE(u, f)->a, field_type(member));
            members[nv++] = member; // a store through self changes no local
        }
        for (int i = 0; i < nv; i++)
        {
            if (!field_stored(members[i]))
            {
                continue;
            }
            if (is_struct(field_type(members[i])))
            {
                copy_fields(0, (uint32_t)field_offset(members[i]), values[i], 0, type_ref(field_type(members[i])));
                continue;
            }
            MirInst *st = inst(MIR_STORE);
            st->var = (uint8_t)mem_variant(field_type(members[i]));
            st->a = 0;
            st->b = values[i];
            st->imm = field_offset(members[i]);
        }
        inst(MIR_RET);
        return;
    }
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
        NodeId rest_of = L.rest_of, rest = L.rest;
        for (NodeId s = n->a; s; s = NEXT(u, s))
        {
            L.rest_of = s;
            L.rest = NEXT(u, s);
            lower_stmt(s);
        }
        L.rest_of = rest_of;
        L.rest = rest;
        L.nlocals = locals;
        return;
    }
    case AST_DEFINE:
    {
        if (n->b) // `n, e := f()`: the value and the error (guards.md §5)
        {
            TypeId vt = u->types[id];
            if (is_array(vt))
            {
                unsupported(n->tok, "an array value with its error");
                return;
            }
            uint32_t vk = is_struct(vt) ? slots_of(vt) : 1;
            MirSlot vs = vk > 1 ? temp_n(vk) : temp(), es = temp();
            NodeId saved = L.handled_call, saved_guard = L.guard_call;
            L.handled_call = n->a;
            L.guard_call = NODE(u, n->a)->kind == AST_CALL && !raises_nothing(u->refs[n->a]) ? n->a : 0;
            L.handled_error = 0;
            // the arena, as it is at the call: given back on an error (DEFINE_ROLLBACK, the checker's)
            NodeId arena = (n->aux & DEFINE_ROLLBACK) ? arena_argument(n->a) : 0;
            MirSlot arena_at = 0, snapshot = 0;
            SymId arena_st = 0;
            if (arena)
            {
                arena_st = type_ref(type_ref(u->types[arena]));
                arena_at = temp();
                into(lower_expr(arena), arena_at);
                uint32_t k = L.perm;
                L.perm += struct_slots(arena_st);
                MirInst *ad = inst(MIR_ADDR);
                ad->d = temp();
                ad->a = SLOT_PERM | k;
                ad->imm = struct_slots(arena_st);
                snapshot = ad->d;
                copy_fields(snapshot, 0, arena_at, 0, arena_st);
            }
            MirSlot v = lower_expr(n->a);
            L.handled_call = saved;
            L.guard_call = saved_guard;
            if (vk > 1)
            {
                copy_slots(vs, v, vk); // a struct value: its slots
            }
            else if (vt != T_VOID && vt != T_NONE)
            {
                into(v, vs);
            }
            into(L.handled_error ? L.handled_error : emit_const(0), es); // a call that cannot fail: nil
            if (arena)
            {
                uint32_t kept = new_label();
                MirInst *j = inst(MIR_JZ);
                j->a = es;
                j->imm = kept;
                copy_fields(arena_at, 0, snapshot, 0, arena_st); // an error: the arena as it was
                place_label(kept);
            }
            push_local_n(token_name(TOK(u, n->tok)), vs, vk);
            push_local(token_name(TOK(u, n->b)), es);
            if (n->aux & DEFINE_UNREAD) // no line reads e: an error that reaches it stops the program
            {
                uint32_t ok = new_label();
                MirInst *j = inst(MIR_JZ);
                j->a = es;
                j->imm = ok;
                int line, col;
                source_pos(u->src, TSTART(TOK(u, n->b)), &line, &col);
                char place[540], shown[540], text[640];
                int pl = snprintf(place, sizeof(place), "%s:%d:%d", u->path, line, col);
                trap_place(place, pl < 0 ? 0 : (size_t)pl >= sizeof(place) ? sizeof(place) - 1 : (size_t)pl, shown,
                           sizeof(shown));
                const Token *et = TOK(u, n->b);
                int tl = snprintf(text, sizeof(text), "%s: panic.unhandled: the error in '%.*s' is not handled\n",
                                  shown, (int)et->len, TSTART(et));
                MirSlot msg = string_slot((const uint8_t *)text, (size_t)(tl < 0 ? 0 : tl));
                MirInst *tr = inst(MIR_TRAP);
                tr->a = msg;
                tr->imm = PANIC_UNHANDLED;
                L.f->strings[L.f->nstrings - 1].panic = (uint8_t)tr->imm; // (its message: made just before)
                place_label(ok);
            }
            L.top = top + vk + 1;
            return;
        }
        if (NODE(u, n->a)->kind == AST_FN_LIT)
        {
            return; // generated as a function of its own; calls to the local name it directly
        }
        if (type_kind(u->types[id]) == T_FUNC) // `f := double`: calls name it; `a := f(x)`: bound
        {
            if (NODE(u, n->a)->kind == AST_CALL)
            {
                lower_bound_args(id, top);
            }
            return;
        }
        if (is_array(u->types[id])) // `c := a`: a copy of a's elements
        {
            uint32_t k = slots_of(u->types[id]);
            MirSlot slot = temp_n(k);
            copy_array_into(n->a, slot, k);
            push_local_n(token_name(TOK(u, n->tok)), slot, k);
            L.top = top + k;
            return;
        }
        if (is_struct(u->types[id])) // a struct value: its own block of slots
        {
            if (NODE(u, n->a)->kind == AST_STRUCT_LIT && lower_fields_local(id, n->a, top))
            {
                return; // its fields in slots of their own
            }
            uint32_t k = slots_of(u->types[id]);
            MirSlot slot = temp_n(k);
            if (NODE(u, n->a)->kind == AST_STRUCT_LIT) // a new local: nothing reads it yet
            {
                struct_literal_at(n->a, slot);
            }
            else
            {
                lower_struct_into(n->a, slot);
            }
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
        if (is_array(t)) // its elements start at zero, or are a copy of its value's
        {
            uint32_t k = slots_of(t);
            MirSlot slot = temp_n(k);
            if (n->b)
            {
                copy_array_into(n->b, slot, k);
            }
            else
            {
                MirInst *z = inst(MIR_ZERO);
                z->a = slot;
                z->imm = k;
            }
            push_local_n(token_name(TOK(u, n->tok)), slot, k);
            L.top = top + k;
            return;
        }
        if (is_struct(t))
        {
            if (n->b && NODE(u, n->b)->kind == AST_STRUCT_LIT && lower_fields_local(id, n->b, top))
            {
                return; // its fields in slots of their own
            }
            uint32_t k = slots_of(t);
            MirSlot slot = temp_n(k);
            if (n->b)
            {
                if (NODE(u, n->b)->kind == AST_STRUCT_LIT) // a new local: nothing reads it yet
                {
                    struct_literal_at(n->b, slot);
                }
                else
                {
                    lower_struct_into(n->b, slot);
                }
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
    {
        MirSlot r = lower_expr(id);
        if (n->kind == AST_CALL && u->types[id] == T_ERROR && u->refs[id] && !sema_fn_fails(u->refs[id]))
        {
            error_up(r); // `run()` alone: its error goes up
        }
        L.top = top;
        return;
    }
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

// The leaves of a struct global from `leaf` on, from the bytes at off of the value the compiler
// computed (0 without one: a struct variable with no initializer starts at zero).
static uint32_t global_init_leaves(uint32_t leaf, const ComptimeAggregate *c, uint32_t off, TypeId t)
{
    if (type_kind(t) == T_STRUCT)
    {
        SymId st = type_ref(t);
        Unit *home = &cx.units[SYM(st)->unit];
        for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
        {
            if (NODE(home, m)->kind == AST_FIELD_DECL && !layout_member_gone(st, m))
            {
                leaf = global_init_leaves(leaf, c, off + layout_member_offset(st, m),
                                          sema_type_of(home, NODE(home, m)->a, st));
            }
        }
        return leaf;
    }
    uint32_t n = type_kind(t) == T_ARRAY ? type_len(t) : 1;
    TypeId et = type_kind(t) == T_ARRAY ? type_ref(t) : t;
    uint32_t w = type_layout(et).size;
    for (uint32_t e = 0; e < n; e++, leaf++)
    {
        uint32_t at = off + e * w;
        MirSlot k;
        if (et == T_STR && c && at / 8 < c->n && c->str[at / 8])
        {
            k = string_slot(c->str[at / 8], c->len[at / 8]);
        }
        else
        {
            uint64_t v = 0;
            if (c && at + w <= c->n * 8)
            {
                memcpy(&v, (const uint8_t *)c->slots + at, w > 8 ? 8 : w);
            }
            int var = int_variant(et);
            k = emit_const(var >= 0 ? mir_normalize(v, var) : v);
        }
        MirInst *st = inst(MIR_GSTORE);
        st->a = k;
        st->imm = leaf;
        L.top = k;
    }
    return leaf;
}

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
        TypeId gt = sema_value_type((Unit *)home, s);
        if (is_struct(gt)) // a struct variable: each leaf from the value the compiler computed
        {
            global_init_leaves(global_index(s), text, 0, gt);
            continue;
        }
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

// A name pattern known only at run time: on to `miss` unless the value equals it.
static void miss_unless_equal(MirSlot slot, NodeId value, TypeId t, uint32_t miss)
{
    MirSlot top = L.top;
    MirSlot v = lower_as(value, t);
    MirSlot r = temp();
    MirInst *i = inst(MIR_BIN);
    i->sub = MB_NE;
    i->var = (uint8_t)int_variant(t);
    i->d = r;
    i->a = slot;
    i->b = v;
    MirInst *j = inst(MIR_JNZ);
    j->a = r;
    j->imm = miss;
    L.top = top;
}

// ---- arms (docs/std.md §8) ------------------------------------------------------------------------
//
// Each arm: its tests go on to the next arm when the value is not one it takes, a name binds the
// value's slot, then the result. On the parameters (a function body of arms) each result is the
// return; on a value, it goes to the match's slot (a value) or is a statement, and the arm goes to
// the end. The last arm has no test when the arms above leave it only its values (MATCH_LAST_FREE).
static MirSlot lower_match(NodeId id, TypeId want)
{
    return lower_match_as(id, want, 0);
}

// ---- dispatch by search (std.md §8: arms are disjoint, so their order is free) ---------------------
//
// One integer value and at least TREE_MIN arms whose patterns the compiler knows: the values are
// laid out as sorted segments (gaps going to `_`, or to the end), and a binary search over them
// picks the arm in log2 compares instead of one or two per arm.
#define TREE_MIN 4
#define TREE_MAX 64

typedef struct
{
    int64_t lo;       // ordering key of the first value (signed order; unsigned values biased)
    uint64_t lo_bits; // the first value, as the variant holds it
    uint32_t target;  // the arm's label
} Segment;

static void tree_emit(const Segment *s, int a, int b, int v, MirSlot slot)
{
    if (b - a == 1)
    {
        inst(MIR_JUMP)->imm = s[a].target;
        return;
    }
    int mid = (a + b) / 2;
    if (b - mid == 1) // the right side is one arm: branch straight to it
    {
        miss_if(MB_GE, v, slot, s[mid].lo_bits, s[mid].target);
        tree_emit(s, a, mid, v, slot);
        return;
    }
    uint32_t right = new_label();
    miss_if(MB_GE, v, slot, s[mid].lo_bits, right);
    tree_emit(s, a, mid, v, slot);
    place_label(right);
    tree_emit(s, mid, b, v, slot);
}

// MODAL_TREE=0 tests the arms one after another instead, to compare (the fuzzer runs both).
static int tree_mode(void)
{
    static int mode = -1;
    if (mode < 0)
    {
        const char *e = getenv("MODAL_TREE");
        mode = e && e[0] == '0' ? 0 : 1;
    }
    return mode;
}

typedef struct
{
    Segment seg[2 * TREE_MAX + 1];
    int n, variant;
} TreePlan;

// The first arm keeps its own test, in place, as written: the order says which arm to try first
// (a hot arm written first costs one compare and no jump), and the search covers the others. Its
// values never reach the search, so they go to whichever target is next to them.
static int match_tree(const AstNode *m, int ns, TypeId t, uint32_t miss, uint32_t *entry, TreePlan *plan)
{
    Unit *u = L.u;
    int v = int_variant(t);
    if (ns != 1 || v < 0 || t == T_BOOL || !tree_mode())
    {
        return 0;
    }
    int is_signed = v >= MV_I8, width = 8 << (is_signed ? v - MV_I8 : v - MV_U8);
    uint64_t bias = is_signed ? 0 : 1ull << 63;
    struct
    {
        int64_t lo, hi;
        uint64_t lo_bits;
        int arm;
    } in[TREE_MAX];
    int n = 0, i = 0, any = -1;
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm), i++)
    {
        NodeId pat = NODE(u, arm)->a;
        Pattern pt;
        if (i >= TREE_MAX || NEXT(u, pat))
        {
            return 0;
        }
        if (i == 0) // tested in place; checked like the others, so the shape is known
        {
            if (!(NODE(u, pat)->kind == AST_PATTERN && (NODE(u, pat)->aux & PAT_ANY)) &&
                (!sema_arm_pattern(u, pat, t, &pt) || pt.kind == PATTERN_TEXT))
            {
                return 0;
            }
            continue;
        }
        if (NODE(u, pat)->kind == AST_PATTERN && (NODE(u, pat)->aux & PAT_ANY))
        {
            any = i;
            continue;
        }
        if (!sema_arm_pattern(u, pat, t, &pt) || pt.kind == PATTERN_TEXT)
        {
            return 0; // a value known only at run time keeps its place: tested in order
        }
        uint64_t lo = mir_normalize(pt.lo, v), hi = pt.kind == PATTERN_RANGE ? mir_normalize(pt.hi, v) : lo;
        in[n].lo = (int64_t)(lo ^ bias);
        in[n].hi = (int64_t)(hi ^ bias);
        in[n].lo_bits = lo;
        in[n].arm = i;
        n++;
    }
    if (n + 1 < TREE_MIN)
    {
        return 0;
    }
    for (int k = 1; k < n; k++) // sorted by their first value (they are disjoint)
    {
        for (int j = k; j > 0 && in[j].lo < in[j - 1].lo; j--)
        {
            __typeof__(in[0]) x = in[j];
            in[j] = in[j - 1];
            in[j - 1] = x;
        }
    }
    for (int k = 0; k <= i; k++)
    {
        entry[k] = new_label();
    }
    uint32_t rest = any >= 0 ? entry[any] : miss;
    uint64_t umax = width == 64 ? ~0ull : (1ull << width) - 1;
    int64_t min = is_signed ? -(int64_t)(1ull << (width - 1)) : (int64_t)(0 ^ bias);
    int64_t max = is_signed ? (int64_t)((1ull << (width - 1)) - 1) : (int64_t)(umax ^ bias);
    Segment seg[2 * TREE_MAX + 1];
    int ns2 = 0, done = 0;
    int64_t at = min; // the first value not laid out yet
    for (int k = 0; k < n && !done; k++)
    {
        if (in[k].lo > at) // a gap before this arm's values: `_`'s, or nobody's
        {
            seg[ns2++] = (Segment){at, 0, rest};
        }
        seg[ns2++] = (Segment){in[k].lo, 0, entry[in[k].arm]};
        if (in[k].hi == max)
        {
            done = 1;
        }
        else
        {
            at = in[k].hi + 1;
        }
    }
    if (!done)
    {
        seg[ns2++] = (Segment){at, 0, rest};
    }
    // Neighbours with the same target are one segment.
    int w = 0;
    for (int k = 0; k < ns2; k++)
    {
        if (w && seg[w - 1].target == seg[k].target)
        {
            continue;
        }
        seg[w++] = seg[k];
    }
    // The values as the variant holds them (a signed key is already sign-extended).
    for (int k = 0; k < w; k++)
    {
        seg[k].lo_bits = is_signed ? (uint64_t)seg[k].lo : (uint64_t)seg[k].lo ^ bias;
    }
    memcpy(plan->seg, seg, (size_t)w * sizeof(Segment));
    plan->n = w;
    plan->variant = v;
    return 1;
}

// returned: the match is a `return`'s value; each arm returns (and the function ends there).
// The `arena` context a call takes: its argument (an identifier named like its struct), or 0.
static NodeId arena_argument(NodeId call)
{
    Unit *u = L.u;
    for (NodeId a = NODE(u, call)->b; a; a = NEXT(u, a))
    {
        TypeId at = u->types[a];
        const Token *t = TOK(u, NODE(u, a)->tok);
        if (NODE(u, a)->kind == AST_IDENT && type_kind(at) == T_PTR && type_kind(type_ref(at)) == T_STRUCT &&
            t->len == 5 && memcmp(TSTART(t), "arena", 5) == 0)
        {
            return a;
        }
    }
    return 0;
}

// A call whose function, and every one it reaches, was checked, and which can neither fail nor
// panic: `n, e := f()` on it needs no boundary (e is nil).
static int raises_nothing(SymId fn)
{
    uint8_t panics;
    const uint64_t *ids;
    uint32_t n;
    int any;
    return fn && !sema_fn_fails(fn) && sema_error_set(fn, &panics, &ids, &n, &any) == 1 && !panics && !n && !any;
}

// A stage on a call that can fail (MATCH_ERRORS): its value and its error, each arm testing one
// side (sema_error_arm). What no arm takes goes on: an error up (or to `n, e :=`), a value as
// the stage's value.
static MirSlot lower_error_match(NodeId id, TypeId want)
{
    Unit *u = L.u;
    const AstNode *m = NODE(u, id);
    NodeId subject = m->a;
    TypeId vt = u->types[subject];
    int handled = L.handled_call == id; // `n, e := parse(s).(...)`
    NodeId saved = L.handled_call;
    L.handled_call = subject;
    L.handled_error = 0;
    MirSlot v0 = lower_expr(subject);
    MirSlot err0 = L.handled_error ? L.handled_error : emit_const(0);
    L.handled_call = saved;
    MirSlot val = temp(), err = temp();
    into(v0, val);
    into(err0, err);
    int value = !(m->aux & MATCH_STATEMENT) && want != T_VOID && want != T_NONE;
    MirSlot r = value ? temp() : 0;
    uint32_t end = new_label(), top = L.top;
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm))
    {
        const AstNode *a = NODE(u, arm);
        uint32_t next = new_label();
        uint64_t eid;
        int cls = sema_error_arm(u, a->a, vt, &eid);
        if (cls == EARM_ERROR || cls == EARM_VALUE_TYPE || cls == EARM_VALUE)
        {
            MirInst *j = inst(cls == EARM_ERROR ? MIR_JZ : MIR_JNZ); // an error / no error
            j->a = err;
            j->imm = next;
        }
        if (cls == EARM_NAME)
        {
            miss_if(MB_NE, MV_U64, err, eid, next);
        }
        if (cls == EARM_VALUE)
        {
            Pattern pt;
            NodeId name;
            if (sema_arm_pattern(u, a->a, vt, &pt))
            {
                pattern_test(&pt, vt, val, next);
            }
            else if ((name = sema_arm_name(u, a->a, vt)))
            {
                miss_unless_equal(val, name, vt, next);
            }
        }
        NodeId res = a->b;
        SymId given = NODE(u, res)->kind == AST_IDENT && u->refs[res] && SYM(u->refs[res])->kind == SYM_METHOD
                          ? u->refs[res]
                          : 0;
        if (given) // `u64: double`: the function is given the value
        {
            into(val, SLOT_WIN | 0);
            inst(MIR_CALL)->imm = given;
            if (value)
            {
                into(SLOT_WIN | 0, r);
            }
        }
        else if (value)
        {
            into(lower_as(res, want), r);
        }
        else
        {
            lower_stmt(res);
        }
        L.top = top;
        inst(MIR_JUMP)->imm = end;
        place_label(next);
    }
    if (!handled)
    {
        error_up(err); // an error no arm took
    }
    if (value)
    {
        into(val, r); // a value no arm took goes on
    }
    place_label(end);
    if (handled)
    {
        L.handled_error = err;
    }
    return value ? r : temp();
}

static MirSlot lower_match_as(NodeId id, TypeId want, int returned)
{
    if (NODE(L.u, id)->aux & MATCH_ERRORS)
    {
        return lower_error_match(id, want);
    }
    Unit *u = L.u;
    const AstNode *m = NODE(u, id);
    MirSlot subj[16];
    TypeId types[16];
    int ns = 0;
    if (m->a)
    {
        for (NodeId e = m->a; e && ns < 16; e = NEXT(u, e), ns++)
        {
            types[ns] = u->types[e] == T_INT_LIT ? T_I64 : u->types[e]; // (as the checker matched it)
            MirSlot v = lower_as(e, types[ns]);
            subj[ns] = temp();
            into(v, subj[ns]);
        }
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
            slot += param_slots(pt);
        }
    }
    int value = m->a && !returned && want != T_VOID && want != T_NONE;
    if (value && slots_of(want) != 1)
    {
        unsupported(m->tok, "a match whose value is a struct");
        return temp();
    }
    MirSlot r = value ? temp() : 0;
    uint32_t end = new_label(), top = L.top;
    uint32_t entry[TREE_MAX];
    static TreePlan plan;
    NodeId only = sema_taken_arm(L.f->sym, id); // decided while compiling: that arm, untested
    // a value stage that takes some values only: the others go on as its value (std.md §14)
    int pass = value && (m->aux & MATCH_PASS) && !only;
    uint32_t miss = pass ? new_label() : end;
    int tree = only ? 0 : match_tree(m, ns, types[0], miss, entry, &plan);
    int index = 0;
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm), index++)
    {
        if (only && arm != only)
        {
            continue;
        }
        const AstNode *a = NODE(u, arm);
        int last = !NEXT(u, arm) || only;
        int searched = tree && index > 0; // reached through the search: no test of its own
        uint32_t next = searched ? 0 : last ? miss : new_label(); // (the last arm tests only when values are left)
        int locals = L.nlocals;
        int k = 0;
        if (searched)
        {
            place_label(entry[index]);
        }
        for (NodeId pat = a->a; pat && k < ns && !searched && !only; pat = NEXT(u, pat), k++)
        {
            Pattern pt; // (`_` is no test: sema_arm_pattern says 0)
            NodeId name;
            if (last && (m->aux & MATCH_LAST_FREE))
            {
                continue;
            }
            if (sema_arm_pattern(u, pat, types[k], &pt))
            {
                pattern_test(&pt, types[k], subj[k], next);
            }
            else if ((name = sema_arm_name(u, pat, types[k])))
            {
                miss_unless_equal(subj[k], name, types[k], next);
            }
        }
        NodeId res = a->b;
        int kind = NODE(u, res)->kind;
        int statement = kind == AST_BLOCK || kind == AST_RETURN || kind == AST_ASSIGN || kind == AST_FIELD_DECL ||
                        kind == AST_IF;
        if (!m->a || returned) // on the parameters, or `return if ...`: the result is the return
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
        if (!last && !searched)
        {
            place_label(next);
            if (tree) // after the first arm: the search over the others
            {
                tree_emit(plan.seg, 0, plan.n, plan.variant, subj[0]);
            }
        }
    }
    if (pass)
    {
        inst(MIR_JUMP)->imm = end;
        place_label(miss);
        into(subj[0], r);
    }
    place_label(end);
    if (returned)
    {
        inst(MIR_RET); // (reached by no arm when they take every value)
    }
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
    L.fails = sema_fn_fails(fn);
    L.struct_abi = is_struct(L.ret) || L.fails; // (a result with its error: never inlined, for now)
    L.loops = (SYM(fn)->flags & SYM_F_LOOPS) != 0; // it calls itself (the checker saw it)
    L.for_compiler = 0;
    L.handled_call = 0;
    L.guard_call = 0;
    *out = (MirFunc){.u = u, .sym = fn, .returns = L.ret != T_VOID || L.fails,
                     .ret_slots = (L.ret == T_VOID ? 0 : slots_of(L.ret)) + (uint32_t)L.fails};
    if (!mir_lowerable(u, fn, 0))
    {
        unsupported(SYM(fn)->tok, "a module-level function that takes `self`");
        return 0;
    }

    if (L.has_self)
    {
        temp(); // slot 0: self
    }
    // Parameters; a pattern has a slot but no name.
    uint32_t miss = 0;
    SymId next_clause = sema_next_clause(u, fn);
    int patterned = 0; // a clause with patterns and no clause after it: its miss returns (nil only)
    for (NodeId p = sema_fn_params(u, fn); p && !next_clause; p = NEXT(u, p))
    {
        Pattern pat;
        patterned |= sema_pattern_of(u, p, SYM(fn)->owner, &pat, 0);
    }
    if (next_clause || patterned)
    {
        miss = new_label();
    }
    // Parameters take consecutive slots (the caller's window puts them there), before the
    // tests need any temporary.
    MirSlot first_param = L.top;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p))
    {
        TypeId pt = sema_param_type(u, fn, p);
        if (type_kind(pt) == T_FUNC)
        {
            continue; // `f: function(...)`: known while compiling, no slot (its calls name the function)
        }
        uint32_t k = param_slots(pt);
        L.struct_abi |= is_struct(pt);
        MirSlot slot = temp_n(k);
        uint64_t value;
        if (!sema_pattern(u, p, SYM(fn)->owner, &value, 0))
        {
            push_local_n(token_name(TOK(u, NODE(u, p)->tok)), slot, k);
            L.locals[L.nlocals - 1].ref = (uint8_t)is_array(pt); // an array parameter: its address
        }
    }
    {
        Capture caps[64]; // what it reads of the function around it: parameters after its own
        uint32_t nc = sema_captures(fn, caps, 64);
        for (uint32_t i = 0; i < nc; i++)
        {
            uint32_t ck = slots_of(caps[i].type); // a struct: its slots
            push_local_n(caps[i].name, ck > 1 ? temp_n(ck) : temp(), ck);
        }
        SymId given[16]; // a specialization for literals that read their callers' locals: those, too
        uint32_t ng = sema_spec_functions(fn, given, 16);
        for (uint32_t g = 0; g < ng; g++)
        {
            uint32_t gc = sema_captures(given[g], caps, 64);
            for (uint32_t i = 0; i < gc; i++)
            {
                uint32_t ck = slots_of(caps[i].type);
                push_local_n(sema_capture_name(given[g], caps[i].name), ck > 1 ? temp_n(ck) : temp(), ck);
            }
        }
    }
    uint32_t params_end = L.top;
    out->param_slots = params_end;
    MirSlot next_slot = first_param;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p))
    {
        MirSlot slot = next_slot;
        if (type_kind(sema_param_type(u, fn, p)) == T_FUNC)
        {
            continue;
        }
        next_slot += param_slots(sema_param_type(u, fn, p));
        Pattern pat;
        if (!sema_pattern_of(u, p, SYM(fn)->owner, &pat, 0))
        {
            continue;
        }
        // The clause's test: an argument its pattern does not take sends the call on.
        pattern_test(&pat, sema_param_type(u, fn, p), slot, miss);
    }

    // A mutable array parameter (`[4]u8 a`) is the function's own copy: its elements are copied
    // in, so writing them changes nothing of the caller's (an immutable one is read in place).
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p))
    {
        TypeId pt = sema_param_type(u, fn, p);
        MirSlot from;
        int ref;
        NameId name = token_name(TOK(u, NODE(u, p)->tok));
        if (!is_array(pt) || !(NODE(u, p)->flags & AST_F_MUT) || !find_local_ref(name, &from, &ref))
        {
            continue;
        }
        uint32_t k = slots_of(pt);
        MirSlot block = temp_n(k);
        for (uint32_t j = 0; j < k; j++)
        {
            MirInst *ld = inst(MIR_LOAD);
            ld->var = MV_U64;
            ld->d = block + j;
            ld->a = from;
            ld->imm = 8 * j;
        }
        push_local_n(name, block, k);
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
    else if (patterned) // no clause takes these values: the call does nothing
    {
        place_label(miss);
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
    Capture caps[64];
    uint32_t k = SYM(fn)->owner != 0;
    uint32_t ncap = sema_captures(fn, caps, 64); // a literal's captures, after its parameters
    for (uint32_t i = 0; i < ncap; i++)
    {
        k += slots_of(caps[i].type);
    }
    SymId given[16];
    uint32_t ng = sema_spec_functions(fn, given, 16);
    for (uint32_t g = 0; g < ng; g++)
    {
        uint32_t gc = sema_captures(given[g], caps, 64);
        for (uint32_t i = 0; i < gc; i++)
        {
            k += slots_of(caps[i].type);
        }
    }
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p))
    {
        TypeId pt = sema_param_type(u, fn, p);
        k += type_kind(pt) == T_FUNC ? 0 : slots_of(pt); // a function given while compiling: no slot
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
    ndescs = 0;
    desc_init = desc_start = 0;
    desc_written = start_written = 0;
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
    L.loops = 0;
    L.for_compiler = 1;
    L.fails = 0; // (an error with nowhere to go stops the compiler: error_up)
    L.handled_call = L.guard_call = 0;
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
        "trap",  "jump", "gaddr", "desc",
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
