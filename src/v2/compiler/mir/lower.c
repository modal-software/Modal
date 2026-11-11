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
    int has_self; // slot 0 holds self (a reference)
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
    if (f->n > INLINE_MAX || L.errors)
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
    Inlinable *k = (Inlinable *)arena_alloc(sizeof(Inlinable));
    k->n = (uint32_t)f->n - 1; // without the final return
    k->code = (MirInst *)arena_alloc(f->n * sizeof(MirInst));
    memcpy(k->code, f->code, f->n * sizeof(MirInst));
    k->nstrings = (uint32_t)f->nstrings;
    k->strings = (MirString *)arena_alloc((f->nstrings + 1) * sizeof(MirString));
    for (size_t i = 0; i < f->nstrings; i++)
    {
        uint8_t *bytes = (uint8_t *)arena_alloc(f->strings[i].len + 1);
        memcpy(bytes, f->strings[i].bytes, f->strings[i].len);
        k->strings[i] = (MirString){bytes, f->strings[i].len};
    }
    k->nlabels = f->nlabels;
    inlinable[f->sym] = k;
}

static MirInst *inst(uint8_t op);

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
        if (in.op == MIR_LABEL || in.op == MIR_JZ || in.op == MIR_JNZ)
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

static void push_local(NameId name, MirSlot slot)
{
    if (L.nlocals == MAX_LOCALS)
    {
        fprintf(stderr, "fatal: too many locals in one function\n");
        exit(2);
    }
    L.locals[L.nlocals].name = name;
    L.locals[L.nlocals].slot = slot;
    L.nlocals++;
    if (slot < MAX_TRACKED)
    {
        L.is_local[slot] = 1;
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
    for (uint32_t i = 0; i < t->len; i++)
    {
        v = v * 10 + (uint64_t)(TSTART(t)[i] - '0');
    }
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
    switch (n->kind)
    {
    case AST_NUMBER_LIT:
    {
        uint64_t v = literal_value(TOK(u, n->tok));
        return v <= 0x7fffffffu ? MV_I32 : v <= 0x7fffffffffffffffull ? MV_I64 : MV_U64;
    }
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
        if (strlen(binops[i].text) == t->len && memcmp(binops[i].text, TSTART(t), t->len) == 0)
        {
            p.index = (int)i;
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
static MirSlot lower_as(NodeId id, TypeId want)
{
    Unit *u = L.u;
    TypeId have = u->types[id];
    if (type_kind(want) == T_PTR && type_kind(have) == T_STRUCT)
    {
        if (is_self_ident(id) && L.has_self)
        {
            return 0;
        }
        unsupported(NODE(u, id)->tok, "a struct value that is not `self`");
        return temp();
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

static void flush_pending(void)
{
    if (!npending)
    {
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

static MirSlot lower_output(NodeId args, int newline)
{
    Unit *u = L.u;
    npending = 0;
    for (NodeId a = args; a; a = NODE(u, a)->next)
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
    return temp();
}

static uint32_t struct_slots(SymId s)
{
    const Unit *u = &cx.units[SYM(s)->unit];
    uint32_t n = 0;
    for (NodeId m = NODE(u, SYM(s)->decl)->a; m; m = NODE(u, m)->next)
    {
        n += NODE(u, m)->kind == AST_FIELD_DECL;
    }
    return n ? n : 1;
}

// Fields are 8-byte slots in declaration order.
static uint64_t field_offset(SymId field)
{
    SymId owner = SYM(field)->owner;
    const Unit *u = &cx.units[SYM(owner)->unit];
    uint64_t index = 0;
    for (NodeId m = NODE(u, SYM(owner)->decl)->a; m; m = NODE(u, m)->next)
    {
        if (NODE(u, m)->kind == AST_FIELD_DECL)
        {
            if (m == SYM(field)->decl)
            {
                return index * 8;
            }
            index++;
        }
    }
    return 0;
}

static MirSlot lower_call(NodeId id)
{
    Unit *u = L.u;
    const AstNode *n = NODE(u, id);
    const AstNode *callee = NODE(u, n->a);
    SymId fn = u->refs[id];

    if (callee->kind == AST_IDENT && TOK(u, callee->tok)->kind == TOK_WRITE)
    {
        return lower_output(n->b, 0);
    }
    if (callee->kind == AST_IDENT && sema_is_std_print(u, u->refs[n->a]))
    {
        return lower_output(n->b, 1);
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
        for (NodeId a = later_args; a; a = NODE(u, a)->next)
        {
            calls_after |= contains_call(a);
        }
        args[nargs++] = calls_after || inline_body ? r : into(r, SLOT_WIN | 0);
    }
    const Unit *home = &cx.units[SYM(fn)->unit]; // where the parameters are declared
    NodeId param = sema_fn_params(u, fn);
    for (NodeId a = n->b; a && param; a = NODE(u, a)->next, param = NODE(home, param)->next)
    {
        if (nargs == 256)
        {
            unsupported(NODE(u, a)->tok, "a call with more than 256 arguments");
            break;
        }
        MirSlot r = lower_as(a, sema_param_type(u, fn, param));
        int calls_after = 0;
        for (NodeId b = NODE(u, a)->next; b; b = NODE(u, b)->next)
        {
            calls_after |= contains_call(b);
        }
        args[nargs] = calls_after || inline_body ? r : into(r, SLOT_WIN | (uint32_t)nargs);
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
        into(args[i], SLOT_WIN | (uint32_t)i);
    }
    inst(MIR_CALL)->imm = fn;
    if (sema_fn_ret(u, fn) == T_VOID)
    {
        return temp();
    }
    // The result is in the callee's slot 0. It is copied out at once: the next call reuses the
    // window. When this copy is the last instruction, its user can take the value straight.
    MirInst *r = inst(MIR_COPY);
    r->d = temp();
    r->a = SLOT_WIN | 0;
    return r->d;
}

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
    unsupported(tok, "a field of a struct value that is not behind a reference");
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
    case AST_NUMBER_LIT:
        unsupported(n->tok, "a fractional number");
        return temp();
    case AST_STRING_LIT:
        return lower_string(t);
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
            MirInst *i = inst(MIR_LOAD);
            i->d = temp();
            i->a = 0;
            i->imm = field_offset(s);
            return i->d;
        }
        NodeId ginit;
        if (is_global(s, &ginit))
        {
            if (ginit && NODE(&cx.units[SYM(s)->unit], ginit)->kind != AST_NUMBER_LIT)
            {
                unsupported(n->tok, "a module-level variable whose initializer is not a literal");
                return temp();
            }
            MirInst *i = inst(MIR_GLOAD);
            i->d = temp();
            i->imm = global_index(s);
            return i->d;
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
            if (sinit && NODE(&cx.units[SYM(member)->unit], sinit)->kind != AST_NUMBER_LIT)
            {
                unsupported(n->tok, "a module-level variable whose initializer is not a literal");
                return temp();
            }
            MirInst *i = inst(MIR_GLOAD);
            i->d = temp();
            i->imm = global_index(member);
            return i->d;
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
            unsupported(n->tok, "a module-level constant that is not a literal");
            return temp();
        }
        if (!member || SYM(member)->kind != SYM_FIELD)
        {
            unsupported(n->tok, "using a method as a value");
            return temp();
        }
        MirSlot base = lower_field_base(n->a, n->tok, &ok);
        if (!ok)
        {
            return base;
        }
        MirInst *i = inst(MIR_LOAD);
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
    if (value)
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
        for (NodeId s = n->a; s; s = NODE(u, s)->next)
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
        MirSlot slot = temp(); // the local lives where the statement's temporaries began
        into(lower_as(n->a, u->types[id]), slot);
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

static int count_params(Unit *u, SymId fn)
{
    int n = 0;
    for (NodeId p = sema_fn_params(u, fn); p; p = NODE(u, p)->next)
    {
        n++;
    }
    return n;
}

int mir_lowerable(Unit *u, SymId fn, int report)
{
    const AstNode *n = fn_node(u, fn);
    (void)report;
    return SYM(fn)->owner || !(n->flags & AST_F_SELF);
}

static int count_params(Unit *u, SymId fn);
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
        if (init)
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

int mir_lower(Unit *u, SymId fn, MirFunc *out)
{
    region_end(REGION_MIR); // reserves it on first use
    region_set_end(REGION_MIR, cx.regions[REGION_MIR].base); // the previous function is done
    const AstNode *n = fn_node(u, fn);

    L.u = u;
    L.f = out;
    L.errors = 0;
    L.top = L.max_top = L.perm = 0;
    L.nlocals = 0;
    L.ret = sema_fn_ret(u, fn);
    L.has_self = SYM(fn)->owner != 0;
    *out = (MirFunc){.u = u, .sym = fn, .returns = L.ret != T_VOID};
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
    for (NodeId p = sema_fn_params(u, fn); p; p = NODE(u, p)->next)
    {
        MirSlot slot = temp();
        uint64_t value;
        if (!sema_pattern(u, p, SYM(fn)->owner, &value, 0))
        {
            push_local(token_name(TOK(u, NODE(u, p)->tok)), slot);
        }
    }
    MirSlot slot = first_param;
    for (NodeId p = sema_fn_params(u, fn); p; p = NODE(u, p)->next, slot++)
    {
        uint64_t value;
        if (!sema_pattern(u, p, SYM(fn)->owner, &value, 0))
        {
            continue;
        }
        // The clause's test: an argument that differs from its pattern sends the call on.
        int v = int_variant(sema_param_type(u, fn, p));
        uint64_t k = mir_normalize(value, v);
        MirSlot t = temp();
        if (k < 0x10000)
        {
            MirInst *i = inst(MIR_BINI);
            i->sub = MB_NE;
            i->var = (uint8_t)v;
            i->d = t;
            i->a = slot;
            i->imm = k;
        }
        else
        {
            MirSlot c = emit_const(k);
            MirInst *i = inst(MIR_BIN);
            i->sub = MB_NE;
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
    uint32_t params_end = first_param + (uint32_t)count_params(u, fn);

    if (u->id == 0 && SYM(fn)->owner == 0 && scope_find(u->scope, "main", 4) == fn)
    {
        init_globals();
    }
    if (n->flags & AST_F_ONELINE)
    {
        lower_return(n->c);
    }
    else
    {
        for (NodeId s = NODE(u, n->c)->a; s; s = NODE(u, s)->next)
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
        if (out->returns)
        {
            MirInst *r = inst(MIR_COPY);
            r->d = 0;
            r->a = SLOT_WIN | 0;
        }
        inst(MIR_RET);
    }

    // The frame is complete: constructor values and the window get their places.
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
    keep_if_small(out); // a small body, kept to be spliced into later callers
    return L.errors == 0;
}

// ---- --mir ----------------------------------------------------------------------------------------

void mir_print(const MirFunc *f, FILE *out)
{
    static const char *const ops[MIR__COUNT] = {
        "const", "copy", "addr", "zero", "load", "store", "bin", "bini", "neg",
        "not",   "norm", "label", "jz",  "jnz",  "call",  "ret", "str",  "write", "gload", "gstore",
    };
    static const char *const bins[MB__COUNT] = {"add", "sub", "mul", "div", "eq",
                                                "ne",  "lt",  "le",  "gt",  "ge"};
    static const char *const vars[MV__COUNT] = {"u8", "u16", "u32", "u64",
                                                "i8", "i16", "i32", "i64"};
    static const char *const writes[] = {"str", "int", "uint", "bool", "space", "nl"};
    int len;
    const char *name = symbol_name(f->sym, &len);
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
            fprintf(out, "    call %.*s\n", l, callee);
            break;
        }
        default:
            fprintf(out, "    %s d=s%u a=s%u b=s%u imm=%llu%s%s\n", ops[in->op], in->d, in->a, in->b,
                    (unsigned long long)in->imm, in->op == MIR_NEG || in->op == MIR_NORM ? " ." : "",
                    in->op == MIR_NEG || in->op == MIR_NORM ? vars[in->var] : "");
            break;
        }
    }
}
