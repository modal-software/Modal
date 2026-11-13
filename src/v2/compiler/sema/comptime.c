// sema/comptime.c (v2): the compiler runs code (docs/comptime.md, step 1).
//
// A module-level initializer is a compile-time position: `limit: u64 = fact(10)` is computed
// while compiling, and every backend then sees the number, as if it were written there. The
// engine is an interpreter over the MIR, the same instructions the machine-code backends
// generate from, with the same arithmetic (mir_compute: the wrap of every backend). So a function
// gives the same result in the compiler as in the program.
//
// Compile time is a machine without capabilities: output, a device register, a mutable module
// variable, a trap (a division by zero) stop the evaluation with an error that says which
// function did it. Budgets in mod.modal bound it: `comptime_steps` instructions (1,000,000 by
// default) and `comptime_depth` nested calls (1,000).
//
// A function the evaluation calls is checked again, quietly (it is checked and reported in its
// own turn), and lowered on the spot (sema_lower_for_comptime); its MIR is kept for the rest of
// the compilation, so a function runs as many times as needed for the price of one lowering.
#include "v2/compiler/mir/mir.h"
#include "v2/compiler/sema/types.h"
#include <stdarg.h>
#include <string.h>

typedef struct
{
    MirInst *code;
    size_t n;
    MirString *strings;
    uint32_t nstrings;
    uint32_t frame_size;
    uint32_t uses;   // slots it touches, its call windows included
    uint32_t params; // the slots its caller writes (its parameters, self included)
    uint32_t ret_slots;
    uint32_t *label; // instruction index of each label
    uint8_t state;   // 0 not lowered, 1 lowered, 2 cannot be lowered
    const char *why; // what could not be lowered
} CtFunc;

// The functions the compiler has lowered to run, by symbol: few, so a small open-addressing map
// (a table per symbol would be 64 B for each of a big program's 54,000 functions).
static SymId *fn_keys;
static CtFunc **fn_vals;
static size_t fn_cap, fn_count;
static uint64_t *values; // per symbol: its computed value
static uint8_t *known;   // 1: a number in values; 2: a string or struct in aggregates
static ComptimeAggregate **aggregates;
static size_t values_cap;
static CtFunc *capture_into; // where comptime_capture copies the function it is handed
static SymId capture_sym;    // which one (a function literal inside it is checked first)

static struct Folds *folds;
static uint64_t fold_steps; // spent by the optimizer's folds in this compilation
static uint32_t fold_count; // calls replaced by their result

uint32_t comptime_folded(void)
{
    return fold_count;
}

void comptime_reset(void)
{
    mir_reset();
    folds = NULL;
    fold_steps = 0;
    fold_count = 0;
    fn_keys = NULL;
    fn_vals = NULL;
    fn_cap = fn_count = 0;
    values = NULL;
    known = NULL;
    aggregates = NULL;
    values_cap = 0;
    capture_into = NULL;
}

static CtFunc *ct_func(SymId fn)
{
    if (2 * (fn_count + 1) > fn_cap)
    {
        size_t cap = fn_cap ? fn_cap * 2 : 64;
        SymId *keys = (SymId *)arena_alloc(cap * sizeof(SymId));
        CtFunc **vals = (CtFunc **)arena_alloc(cap * sizeof(CtFunc *));
        memset(keys, 0, cap * sizeof(SymId));
        for (size_t i = 0; i < fn_cap; i++)
        {
            if (fn_keys[i])
            {
                size_t j = fn_keys[i] * 2654435761u & (cap - 1);
                while (keys[j])
                {
                    j = (j + 1) & (cap - 1);
                }
                keys[j] = fn_keys[i];
                vals[j] = fn_vals[i];
            }
        }
        fn_keys = keys;
        fn_vals = vals;
        fn_cap = cap;
    }
    size_t j = fn * 2654435761u & (fn_cap - 1);
    while (fn_keys[j] && fn_keys[j] != fn)
    {
        j = (j + 1) & (fn_cap - 1);
    }
    if (!fn_keys[j])
    {
        fn_keys[j] = fn;
        fn_vals[j] = (CtFunc *)arena_alloc(sizeof(CtFunc));
        memset(fn_vals[j], 0, sizeof(CtFunc));
        fn_count++;
    }
    return fn_vals[j];
}

// The tables of computed values, per symbol: made when the first value is stored.
static void fit(void)
{
    size_t cap = cx.syms.cap;
    if (values_cap < cap)
    {
        uint64_t *v = (uint64_t *)arena_alloc(cap * sizeof(uint64_t));
        uint8_t *k = (uint8_t *)arena_alloc(cap);
        ComptimeAggregate **a = (ComptimeAggregate **)arena_alloc(cap * sizeof(*a));
        memset(k, 0, cap);
        memset(a, 0, cap * sizeof(*a));
        if (values_cap)
        {
            memcpy(v, values, values_cap * sizeof(uint64_t));
            memcpy(k, known, values_cap);
            memcpy(a, aggregates, values_cap * sizeof(*a));
        }
        values = v;
        known = k;
        aggregates = a;
        values_cap = cap;
    }
}

int sema_comptime_value(SymId s, uint64_t *out)
{
    if (s >= values_cap || known[s] != 1)
    {
        return 0;
    }
    *out = values[s];
    return 1;
}

const ComptimeAggregate *sema_comptime_aggregate(SymId s)
{
    return s < values_cap && known[s] == 2 ? aggregates[s] : NULL;
}

// Can a value of type t leave the compiler for the program? Numbers, bools, strings, and
// structs of these can: their bytes are the same in both. A reference cannot (it would point
// into the compiler), nor can a device's registers. The slots holding a string are marked.
static const char *portable(TypeId t, uint32_t off, uint8_t *is_str, uint32_t k)
{
    if (type_is_int(t) || t == T_BOOL)
    {
        return NULL;
    }
    if (t == T_STR)
    {
        if (target_get()->ptr_size != 8 || off % 8 || off / 8 >= k)
        {
            return "a string on a target whose pointers are not 8 bytes";
        }
        is_str[off / 8] = 1;
        return NULL;
    }
    if (type_kind(t) == T_STRUCT)
    {
        SymId st = type_ref(t);
        if (sema_is_register_block(st))
        {
            return "a block of device registers";
        }
        Unit *home = &cx.units[SYM(st)->unit];
        for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
        {
            if (NODE(home, m)->kind != AST_FIELD_DECL)
            {
                continue;
            }
            const char *why = portable(sema_type_of(home, NODE(home, m)->a, st),
                                       off + layout_member_offset(st, m), is_str, k);
            if (why)
            {
                return why;
            }
        }
        return NULL;
    }
    if (type_kind(t) == T_PTR)
    {
        return "a reference (it would point into the compiler)";
    }
    return "a value of this type";
}

// The function lives in REGION_MIR until the next one is lowered: a copy in the arena is kept.
static void keep(CtFunc *to, const MirFunc *f)
{
    to->n = f->n;
    to->code = (MirInst *)arena_alloc(f->n * sizeof(MirInst) + sizeof(MirInst));
    memcpy(to->code, f->code, f->n * sizeof(MirInst));
    to->nstrings = (uint32_t)f->nstrings;
    to->strings = (MirString *)arena_alloc(f->nstrings * sizeof(MirString) + sizeof(MirString));
    for (size_t i = 0; i < f->nstrings; i++)
    {
        uint8_t *b = (uint8_t *)arena_alloc(f->strings[i].len + 8);
        memcpy(b, f->strings[i].bytes, f->strings[i].len);
        to->strings[i] = (MirString){b, f->strings[i].len};
    }
    to->frame_size = f->frame_size;
    to->params = f->param_slots;
    to->ret_slots = f->ret_slots;
    uint32_t uses = f->frame_size > f->ret_slots ? f->frame_size : f->ret_slots;
    to->label = (uint32_t *)arena_alloc((f->nlabels + 1) * sizeof(uint32_t));
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
        if (in->op == MIR_LABEL && in->imm < f->nlabels)
        {
            to->label[in->imm] = (uint32_t)k;
        }
    }
    to->uses = uses;
    to->state = 1;
}

void comptime_capture(Unit *u, SymId fn)
{
    MirFunc f;
    if (!capture_into || fn != capture_sym)
    {
        return; // a function literal inside it: not run here
    }
    CtFunc *to = capture_into;
    capture_into = NULL;
    if (mir_lower(u, fn, &f))
    {
        keep(to, &f);
    }
    else
    {
        to->state = 2;
        to->why = f.fail_what ? f.fail_what : "something that cannot be generated";
    }
}

static const CtFunc *function(SymId fn)
{
    CtFunc *f = ct_func(fn); // (its address stays: each is allocated on its own)
    if (f->state == 0)
    {
        f->state = 2;
        f->why = "a body with an error";
        capture_into = f;
        capture_sym = fn;
        if (!sema_lower_for_comptime(fn))
        {
            f->state = 2;
        }
        capture_into = NULL;
    }
    return f;
}

// ---- the interpreter ---------------------------------------------------------------------------

typedef struct
{
    uint64_t *stack;
    size_t cap;
    uint64_t steps, max_steps;
    uint32_t max_depth;
    uint8_t overflow; // stopped for want of value stack: run again with more
    uint8_t trapped;  // stopped by a trap: its text is in trap_text
    struct CtFrame *frames; // the interpreter's own call stack (no C recursion per Modal call)
    uint32_t frames_cap;
    char trap_text[400];
    int trap_len;
    char why[200];    // the reason it stopped
    struct
    {
        const uint8_t *at;
        size_t n;
    } blocks[64]; // strings made at compile time (their address is a value)
    int nblocks;
} Vm;

static int stop(Vm *vm, SymId fn, const char *what)
{
    int len = 0;
    const char *name = fn ? symbol_name(fn, &len) : "";
    if (!vm->why[0])
    {
        if (fn)
        {
            snprintf(vm->why, sizeof(vm->why), "'%.*s' %s", len, name, what);
        }
        else
        {
            snprintf(vm->why, sizeof(vm->why), "%s", what);
        }
    }
    return 0;
}

// stop() with a formatted reason. Its buffer lives here, not in run(): run recurses once per
// Modal call (comptime_depth deep), so its frame must stay small.
static __attribute__((noinline, format(printf, 3, 4))) int stopf(Vm *vm, SymId fn, const char *fmt, ...)
{
    char what[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);
    return stop(vm, fn, what);
}

// Is [p, p + n) memory the evaluation owns (its value stack, or a string it made)?
static int reachable(const Vm *vm, uint64_t p, uint64_t n)
{
    uint64_t lo = (uint64_t)(uintptr_t)vm->stack, hi = lo + vm->cap * 8;
    if (p >= lo && p + n <= hi && p + n >= p)
    {
        return 1;
    }
    for (int i = 0; i < vm->nblocks; i++)
    {
        uint64_t b = (uint64_t)(uintptr_t)vm->blocks[i].at;
        if (p >= b && p + n <= b + vm->blocks[i].n + 8)
        {
            return 1;
        }
    }
    return 0;
}

// A caller waiting for its callee: the interpreter keeps these itself, so a Modal call is not a
// C call and comptime_depth is not bounded by the compiler's own stack.
struct CtFrame
{
    SymId sym;
    const CtFunc *f;
    size_t fp, k; // its frame, and the call it is at
};

static int run(Vm *vm, SymId sym, const CtFunc *f, size_t fp)
{
    uint32_t depth = 0;
    if (fp + f->uses + 1 > vm->cap)
    {
        vm->overflow = 1;
        return stop(vm, 0, "uses more value stack than the compiler gives it");
    }
    uint64_t *s = vm->stack + fp;
    for (size_t k = 0;; k++)
    {
        if (k >= f->n)
        {
            return stop(vm, sym, "ends without returning");
        }
        const MirInst *in = &f->code[k];
        if (++vm->steps > vm->max_steps)
        {
            return stopf(vm, 0, "takes more steps than comptime_steps = %llu (mod.modal) allows",
                         (unsigned long long)vm->max_steps);
        }
        uint64_t r;
        switch (in->op)
        {
        case MIR_CONST:
            s[in->d] = in->imm;
            break;
        case MIR_COPY:
            s[in->d] = s[in->a];
            break;
        case MIR_ADDR:
            s[in->d] = (uint64_t)(uintptr_t)&s[in->a];
            break;
        case MIR_ZERO:
            memset(&s[in->a], 0, in->imm * 8);
            break;
        case MIR_LOAD:
        case MIR_STORE:
        {
            uint64_t size = 1ull << (in->var & 3), at = s[in->a] + in->imm;
            if (in->sub == MIR_MEM_DEVICE)
            {
                return stop(vm, sym, "uses a device register (it runs only in the program)");
            }
            if (!reachable(vm, at, size))
            {
                return stop(vm, sym, "reads or writes memory the compiler does not have");
            }
            void *p = (void *)(uintptr_t)at;
            if (in->op == MIR_LOAD)
            {
                uint64_t raw = 0;
                memcpy(&raw, p, size); // little-endian on every target, and in the compiler
                s[in->d] = mir_normalize(raw, in->var);
            }
            else
            {
                memcpy(p, &s[in->b], size);
            }
            break;
        }
        case MIR_BIN:
        case MIR_BINI:
        {
            uint64_t a = s[in->a], b = in->op == MIR_BINI ? in->imm : s[in->b];
            if (in->sub == MB_DIV)
            {
                uint64_t nb = mir_normalize(b, in->var);
                if (nb == 0)
                {
                    return stop(vm, sym, "divides by zero");
                }
                if (in->var >= MV_I8 && (int64_t)nb == -1)
                {
                    s[in->d] = mir_normalize(0 - a, in->var); // the smallest value wraps, as in the program
                    break;
                }
            }
            if (!mir_compute(in->sub, in->var, a, b, &r))
            {
                return stop(vm, sym, "computes what the compiler cannot");
            }
            s[in->d] = r;
            break;
        }
        case MIR_NEG:
            s[in->d] = mir_normalize(0 - s[in->a], in->var);
            break;
        case MIR_NOT:
            s[in->d] = s[in->a] == 0;
            break;
        case MIR_NORM:
            s[in->d] = mir_normalize(s[in->a], in->var);
            break;
        case MIR_LABEL:
            break;
        case MIR_JUMP:
            k = f->label[in->imm];
            break;
        case MIR_JZ:
        case MIR_JNZ:
            if ((s[in->a] == 0) == (in->op == MIR_JZ))
            {
                k = f->label[in->imm]; // the label itself: the loop goes on after it
            }
            break;
        case MIR_CALL:
        {
            SymId callee = (SymId)in->imm;
            const CtFunc *g = function(callee);
            if (g->state != 1)
            {
                int len;
                const char *name = symbol_name(callee, &len);
                return stopf(vm, sym, "calls '%.*s', which the compiler cannot run (%s)", len, name, g->why);
            }
            if (in->sub == MIR_CALL_TAIL) // in this frame's place: no deeper
            {
                if (fp + g->uses + 1 > vm->cap)
                {
                    vm->overflow = 1;
                    return stop(vm, 0, "uses more value stack than the compiler gives it");
                }
                memmove(s, s + f->frame_size, in->b * sizeof(uint64_t));
                sym = callee;
                f = g;
                k = (size_t)-1;
                break;
            }
            if (depth + 1 > vm->max_depth)
            {
                return stopf(vm, 0, "calls nest deeper than comptime_depth = %u (mod.modal) allows", vm->max_depth);
            }
            size_t callee_fp = fp + f->frame_size;
            if (callee_fp + g->uses + 1 > vm->cap)
            {
                vm->overflow = 1;
                return stop(vm, 0, "uses more value stack than the compiler gives it");
            }
            if (depth == vm->frames_cap)
            {
                uint32_t cap = vm->frames_cap ? vm->frames_cap * 2 : 64;
                struct CtFrame *grown = (struct CtFrame *)arena_alloc(cap * sizeof(struct CtFrame));
                if (depth)
                {
                    memcpy(grown, vm->frames, depth * sizeof(struct CtFrame));
                }
                vm->frames = grown;
                vm->frames_cap = cap;
            }
            vm->frames[depth++] = (struct CtFrame){sym, f, fp, k};
            sym = callee;
            f = g;
            fp = callee_fp;
            s = vm->stack + fp;
            k = (size_t)-1; // the loop goes on at the callee's first instruction
            break;
        }
        case MIR_RET:
        {
            if (!depth)
            {
                return 1;
            }
            const struct CtFrame *back = &vm->frames[--depth];
            sym = back->sym;
            f = back->f;
            fp = back->fp;
            k = back->k; // the loop goes on after the call
            s = vm->stack + fp;
            break;
        }
        case MIR_STR:
        {
            const MirString *str = &f->strings[in->imm];
            if (vm->nblocks == 64)
            {
                return stop(vm, sym, "makes more strings than the compiler keeps");
            }
            uint8_t *b = (uint8_t *)arena_alloc(str->len + 8);
            uint64_t len = str->len;
            memcpy(b, &len, 8);
            memcpy(b + 8, str->bytes, str->len);
            vm->blocks[vm->nblocks].at = b;
            vm->blocks[vm->nblocks++].n = str->len;
            s[in->d] = (uint64_t)(uintptr_t)b;
            break;
        }
        case MIR_WRITE:
            return stop(vm, sym, "prints (it runs only in the program)");
        case MIR_GLOAD:
        case MIR_GSTORE:
            return stop(vm, sym, "uses a mutable module variable (in the compiler it has only its first value)");
        case MIR_TRAP:
        {
            // The text the program would print: its parts gather until the final trap. A
            // division by zero says so; any other (a failed assert) is given as it is.
            for (int b = 0; b < vm->nblocks; b++)
            {
                if ((uint64_t)(uintptr_t)vm->blocks[b].at == s[in->a])
                {
                    int room = (int)sizeof(vm->trap_text) - 1 - vm->trap_len;
                    int n = (int)vm->blocks[b].n < room ? (int)vm->blocks[b].n : room;
                    memcpy(vm->trap_text + vm->trap_len, vm->blocks[b].at + 8, (size_t)n);
                    vm->trap_len += n;
                    break;
                }
            }
            if (in->sub == MIR_TRAP_PART)
            {
                break;
            }
            vm->trapped = 1;
            int len = vm->trap_len;
            const char *text = vm->trap_text;
            len -= len && text[len - 1] == '\n';
            vm->trap_text[len] = 0;
            vm->trap_len = len;
            if (len >= 16 && memcmp(text + len - 16, "division by zero", 16) == 0)
            {
                return stop(vm, sym, "divides by zero");
            }
            return stopf(vm, sym, "fails at %.*s", len, text);
        }
        default:
            return stop(vm, sym, "does what the compiler cannot run");
        }
    }
}

// ---- module-level initializers --------------------------------------------------------------------

// An initializer the backends already take as it is: a literal, an import, a function.
static int written_out(const Unit *u, NodeId init)
{
    uint8_t k = NODE(u, init)->kind;
    if (k == AST_NUMBER_LIT || k == AST_STRING_LIT || k == AST_FN_LIT)
    {
        return 1;
    }
    if (k == AST_IDENT)
    {
        const Token *t = TOK(u, NODE(u, init)->tok);
        return (t->len == 4 && memcmp(TSTART(t), "true", 4) == 0) ||
               (t->len == 5 && memcmp(TSTART(t), "false", 5) == 0);
    }
    return ast_import_root(u, init) != 0;
}

// The value stack starts small (8 KB) and is given 4 times more when an evaluation needs it,
// up to 512 KB: the evaluation then runs again (it is deterministic). It cannot grow in place,
// since the evaluation keeps addresses into it.
#define VM_FIRST_SLOTS 1024u
#define VM_MAX_SLOTS (1u << 16)

static void vm_ready(Vm *vm, const Unit *u)
{
    if (!vm->stack)
    {
        vm->cap = VM_FIRST_SLOTS;
        vm->stack = (uint64_t *)arena_alloc(vm->cap * 8);
        vm->max_steps = domain_config_int(u->domain, "comptime_steps", 1000000);
        vm->max_depth = (uint32_t)domain_config_int(u->domain, "comptime_depth", 1000);
    }
    vm->steps = 0;
    vm->why[0] = 0;
    vm->nblocks = 0;
    vm->overflow = 0;
    vm->trapped = 0;
    vm->trap_len = 0;
}

// Runs `expr` of `type` in the compiler; its value is in vm->stack from slot 0. 0: it could not
// be run, and *why (or vm->why) says why.
static int evaluate(Vm *vm, Unit *u, NodeId expr, TypeId type, SymId s, const char **why)
{
    MirFunc f;
    *why = NULL;
    if (!mir_lower_expr(u, expr, type, s, &f))
    {
        *why = f.fail_what ? f.fail_what : "its expression";
        return 0;
    }
    if (cx.show_mir)
    {
        mir_print(&f, stdout);
    }
    CtFunc top;
    memset(&top, 0, sizeof(top));
    keep(&top, &f);
    for (;;)
    {
        vm_ready(vm, u);
        if (top.uses + 1 <= vm->cap)
        {
            memset(vm->stack, 0, (top.uses + 1) * 8);
            if (run(vm, 0, &top, 0))
            {
                return 1;
            }
        }
        else
        {
            vm->overflow = 1;
        }
        if (!vm->overflow || vm->cap >= VM_MAX_SLOTS)
        {
            if (vm->overflow && !vm->why[0])
            {
                stop(vm, 0, "uses more value stack than the compiler gives it");
            }
            return 0;
        }
        vm->cap *= 4;
        vm->stack = (uint64_t *)arena_alloc(vm->cap * 8);
    }
}

// A module-level statement: `assert(sizeof(header) == 16)`, or any call (a function run for
// the asserts inside it). The compiler runs it (docs/comptime.md §1). When it traps with its own
// place (std's assert takes a `site`), the trap's text is the error, and a comparison as its first
// argument gets both sides' values. Nothing here knows assert by name.
static void statement(Vm *vm, Unit *u, NodeId d)
{
    const AstNode *n = NODE(u, d);
    uint32_t at = NODE(u, n->a)->tok;
    const char *why;
    TypeId type = u->types[d];
    type = type == T_BOOL || type_is_int(type) ? type : T_VOID;
    if (evaluate(vm, u, d, type == T_INT_LIT ? T_I64 : type, 0, &why))
    {
        return;
    }
    if (why || !vm->trapped)
    {
        diag_at(u, at, "error", "the compiler cannot run this statement: %s%s", why ? why : vm->why,
                why ? " is not supported yet" : "");
        return;
    }
    char here[600];
    int line, col;
    source_pos(u->src, TSTART(TOK(u, at)), &line, &col);
    int k = snprintf(here, sizeof(here), "%s:%d:%d: ", u->path, line, col);
    if (k <= 0 || strncmp(vm->trap_text, here, (size_t)k) != 0)
    {
        diag_at(u, at, "error", "the compiler cannot run this statement: %s", vm->why);
        return;
    }
    char text[400];
    snprintf(text, sizeof(text), "%s", vm->trap_text + k);
    char values[200] = "";
    NodeId cond = n->b;
    const AstNode *c = cond ? NODE(u, cond) : NULL;
    const Token *op = c ? TOK(u, c->tok) : NULL;
    TypeId lt = c && c->kind == AST_BIN_OP ? u->types[c->a] : T_NONE;
    if (c && c->kind == AST_BIN_OP && (type_is_int(lt) || lt == T_BOOL) &&
        (op->kind == TOK_LT || op->kind == TOK_GT || op->kind == TOK_LE || op->kind == TOK_GE ||
         op->kind == TOK_NEQ || (op->len == 2 && memcmp(TSTART(op), "==", 2) == 0)))
    {
        // both sides, as the program would compute them
        uint64_t v[2];
        NodeId side[2] = {c->a, c->b};
        int ok = 1;
        for (int i = 0; i < 2 && ok; i++)
        {
            TypeId t = u->types[side[i]] == T_INT_LIT ? T_I64 : u->types[side[i]];
            ok = evaluate(vm, u, side[i], t, 0, &why);
            v[i] = vm->stack[0];
        }
        int is_signed = lt == T_I8 || lt == T_I16 || lt == T_I32 || lt == T_I64 || lt == T_INT || lt == T_INT_LIT;
        if (ok)
        {
            snprintf(values, sizeof(values), is_signed ? " (%lld %.*s %lld)" : " (%llu %.*s %llu)",
                     (long long)v[0], op->len, TSTART(op), (long long)v[1]);
        }
    }
    diag_at(u, at, "error", "%s%s", text, values);
}

void comptime_unit(Unit *u)
{
    Vm vm = {0};
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        const AstNode *n = NODE(u, d);
        if (n->kind == AST_CALL)
        {
            statement(&vm, u, d);
            continue;
        }
        NodeId init = n->kind == AST_FIELD_DECL ? n->b : n->kind == AST_DEFINE ? n->a : 0;
        if (!init || (n->flags & AST_F_PLACED) || written_out(u, init))
        {
            continue;
        }
        SymId s = scope_find_token(u->scope, TOK(u, n->tok));
        if (!s || (SYM(s)->kind != SYM_CONST && SYM(s)->kind != SYM_FIELD))
        {
            continue;
        }
        TypeId type = sema_value_type(u, s);
        const Token *name = TOK(u, n->tok);
        if (type == T_INT_LIT)
        {
            continue; // unsized: its exact value is the checker's (sema_unsized_value)
        }
        uint32_t k = 1;
        if (type_kind(type) == T_STRUCT)
        {
            uint32_t size = type_layout(type).size;
            if (!size)
            {
                continue; // no layout: the checker reported why
            }
            k = (size + 7) / 8;
        }
        uint8_t *is_str = (uint8_t *)arena_alloc(k);
        memset(is_str, 0, k);
        const char *why = portable(type, 0, is_str, k);
        if (why)
        {
            if (type_kind(type) == T_PTR || type_kind(type) == T_STRUCT)
            {
                diag_at(u, n->tok, "error", "'%.*s' cannot be computed by the compiler: it is %s", name->len,
                        TSTART(name), why);
            }
            continue; // a function, a type, an import: nothing to compute
        }
        if (type_kind(type) == T_STRUCT && n->kind == AST_FIELD_DECL && (n->flags & AST_F_MUT))
        {
            diag_at(u, n->tok, "error",
                    "'%.*s': a module-level variable holding a struct is not supported yet (an immutable one is)",
                    name->len, TSTART(name));
            continue;
        }
        const char *cannot;
        if (!evaluate(&vm, u, init, type, s, &cannot))
        {
            if (cannot)
            {
                diag_at(u, n->tok, "error", "'%.*s' cannot be computed by the compiler: %s is not supported yet",
                        name->len, TSTART(name), cannot);
            }
            else
            {
                diag_at(u, n->tok, "error", "'%.*s' cannot be computed by the compiler: %s", name->len,
                        TSTART(name), vm.why);
            }
            continue;
        }
        fit();
        if (type_kind(type) == T_STRUCT || type == T_STR)
        {
            ComptimeAggregate *agg = (ComptimeAggregate *)arena_alloc(sizeof(*agg));
            uint64_t *slots = (uint64_t *)arena_alloc(k * sizeof(uint64_t));
            const uint8_t **str = (const uint8_t **)arena_alloc(k * sizeof(*str));
            uint32_t *len = (uint32_t *)arena_alloc(k * sizeof(uint32_t));
            int lost = 0;
            for (uint32_t j = 0; j < k; j++)
            {
                slots[j] = vm.stack[j];
                str[j] = NULL;
                len[j] = 0;
                if (!is_str[j])
                {
                    continue;
                }
                int b = 0;
                while (b < vm.nblocks && (uint64_t)(uintptr_t)vm.blocks[b].at != slots[j])
                {
                    b++;
                }
                if (b == vm.nblocks)
                {
                    lost = 1;
                    break;
                }
                str[j] = vm.blocks[b].at + 8;
                len[j] = (uint32_t)vm.blocks[b].n;
                slots[j] = 0; // the program has its own address for it
            }
            if (lost)
            {
                diag_at(u, n->tok, "error", "'%.*s' cannot be computed by the compiler: it holds a string "
                        "that was never made", name->len, TSTART(name));
                continue;
            }
            *agg = (ComptimeAggregate){k, slots, str, len};
            aggregates[s] = agg;
            known[s] = 2;
            continue;
        }
        int v = type == T_BOOL ? MV_U8 : type_kind(type) == T_INT_LIT ? MV_I64 : -1;
        if (v < 0)
        {
            // the variant of a sized integer type: the slot rule's extension of its width
            static const struct
            {
                TypeId t;
                int v;
            } map[] = {{T_U8, MV_U8}, {T_U16, MV_U16}, {T_U32, MV_U32}, {T_U64, MV_U64},
                       {T_I8, MV_I8}, {T_I16, MV_I16}, {T_I32, MV_I32}, {T_I64, MV_I64}, {T_INT, MV_I32}};
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]) && v < 0; i++)
            {
                v = map[i].t == type ? map[i].v : -1;
            }
        }
        values[s] = v >= 0 ? mir_normalize(vm.stack[0], v) : vm.stack[0];
        known[s] = 1;
    }
}

// ---- the optimizer's folds (p.md B5) -------------------------------------------------------------
//
// A call whose arguments are all known constants is run here, while the caller is optimized:
// the call becomes its result. It is the same interpreter as for module-level values, so the
// result is the program's (the differential test checks it on every backend). The fold is an
// optimization, never an error: whatever the interpreter refuses (output, a trap, a mutable
// module variable, a budget) leaves the call where it is. Each fold has a small budget, and a
// compilation has a total, so the compiler's own time stays bounded. Results are kept per
// (function, arguments): a call made again costs a lookup.
#define FOLD_STEPS 20000         // per call
#define FOLD_TOTAL 2000000       // per compilation
#define FOLD_ARGS 8
#define FOLD_SLOTS 64            // the memo of results

typedef struct
{
    SymId fn;
    uint32_t nargs;
    uint64_t args[FOLD_ARGS];
    uint8_t ok, nresult;
    uint64_t result;
} Fold;

struct Folds
{
    Fold slot[FOLD_SLOTS];
    Vm vm;
};

static int foldable_type(TypeId t)
{
    return t == T_VOID || t == T_BOOL || type_is_int(t);
}

// Prepares fn for folding (lowered once, kept): its argument slots, or -1.
int comptime_param_slots(SymId fn)
{
    if (!fn || SYM(fn)->kind != SYM_METHOD || !foldable_type(sema_fn_ret(&cx.units[SYM(fn)->unit], fn)) ||
        cx.units[SYM(fn)->unit].broken || cx.quiet || fold_steps >= FOLD_TOTAL)
    {
        return -1;
    }
    if (ct_func(fn)->state == 0)
    {
        if (!mir_nest_begin())
        {
            return -1; // a fold inside a fold: not here
        }
        function(fn);
        mir_nest_end();
    }
    const CtFunc *g = ct_func(fn);
    return g->state == 1 && g->ret_slots <= 1 && g->params <= FOLD_ARGS ? (int)g->params : -1;
}

int comptime_fold_call(SymId fn, const uint64_t *args, uint32_t nargs, uint64_t *result, uint32_t *nresult)
{
    if (comptime_param_slots(fn) != (int)nargs)
    {
        return 0;
    }
    if (!folds)
    {
        folds = (struct Folds *)arena_alloc(sizeof(*folds));
        memset(folds, 0, sizeof(*folds));
        folds->vm.cap = 1024; // 8 KB of value stack: a fold is small or it is not done
        folds->vm.stack = (uint64_t *)arena_alloc(folds->vm.cap * 8);
    }
    uint64_t h = hash_bytes(args, nargs * sizeof(uint64_t), fn);
    Fold *m = &folds->slot[h % FOLD_SLOTS];
    if (m->fn == fn && m->nargs == nargs && memcmp(m->args, args, nargs * sizeof(uint64_t)) == 0)
    {
        *result = m->result;
        *nresult = m->nresult;
        fold_count += m->ok;
        return m->ok;
    }
    const CtFunc *g = ct_func(fn);
    Vm *vm = &folds->vm;
    vm->steps = 0;
    vm->max_steps = FOLD_STEPS;
    vm->max_depth = 200;
    vm->why[0] = 0;
    vm->nblocks = 0;
    vm->trapped = 0;
    vm->trap_len = 0;
    memset(vm->stack, 0, (g->uses + 1) * 8);
    memcpy(vm->stack, args, nargs * sizeof(uint64_t));
    // the callees it reaches are lowered as it runs: nested, like the function itself
    int nested = mir_nest_begin();
    int ok = nested && run(vm, fn, g, 0);
    if (nested)
    {
        mir_nest_end();
    }
    fold_steps += vm->steps;
    *m = (Fold){.fn = fn, .nargs = nargs, .ok = (uint8_t)ok, .nresult = (uint8_t)g->ret_slots,
                .result = vm->stack[0]};
    memcpy(m->args, args, nargs * sizeof(uint64_t));
    *result = m->result;
    *nresult = m->nresult;
    fold_count += (uint32_t)ok;
    return ok;
}
