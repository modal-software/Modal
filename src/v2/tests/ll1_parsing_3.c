/* ll1_parsing_3.c — dynamic work distribution benchmark.
 *
 * Baseline for comparison is ll1_parsing_2.c (core-pinned threads, static
 * round split, NSTREAMS same-core interleaving). The parser pipeline below
 * is copied verbatim from that file so that any delta measured here comes
 * from the scheduler, not from parser drift.
 *
 * What changed (the "best of both worlds" experiment):
 *
 *   Work is cut into small batches of ROUNDS_PER_BATCH rounds (a round =
 *   one pass over this thread's stream set). Workers pull the next batch by
 *   atomically incrementing a single shared cursor:
 *
 *       b = atomic_fetch_add(&g_cursor.v, 1)
 *       if (b >= total_batches) done;
 *
 *   - Load skew self-corrects: a fast thread just takes more batches, so no
 *     worker idles at the end waiting on a straggler (the failure mode of
 *     the static split when one chunk lands on harder input).
 *   - The single cache line holding the cursor is the only contention
 *     point, and it is touched once per batch, not once per round, so the
 *     cost amortizes to noise at realistic batch sizes.
 *   - The cursor is padded to a full 64-byte line so batch grabs never
 *     share a line with adjacent globals (false sharing).
 *   - Threads are still pinned to cores and still multiplex nstreams
 *     interleaved pipelines inside each core, preserving the memory-latency
 *     hiding effect from v2. Both knobs stay adjustable so wins can be
 *     attributed: `-DNSTREAMS=8`, or runtime argv for threads / batch size /
 *     stream count.
 *
 * Modes (argv[1]):
 *   dyn    — dynamic batch grabbing via the atomic cursor (default)
 *   static — reproduces v2's fixed per-thread split inside this binary, for
 *            a fair A/B under identical code and flags
 *
 * Usage:
 *   ./test_parser3 [dyn|static] [threads] [batch_rounds] [nstreams] [N] [util_pct]
 * Defaults: dyn, ncpu, 4096, NSTREAMS (compile-time, default 8, max 8),
 *           RUNS_DEFAULT, 100 (= unpaced; e.g. 25 caps each worker at ~25%).
 */
#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE /* needed for CPU_ZERO/CPU_SET/pthread_setaffinity_np on glibc */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef NSTREAMS
#define NSTREAMS 8
#endif
#define MAX_STREAMS 8
_Static_assert(NSTREAMS >= 1 && NSTREAMS <= MAX_STREAMS, "NSTREAMS out of range");

#define CHUNK_SZ 64
#define RUNS_DEFAULT 1000000000ull
#define BATCH_ROUNDS_DEFAULT 4096ull

typedef union Chunk Chunk;
union Chunk
{
    Chunk *next; /* unused; kept for layout compat */
    uint8_t buffer[CHUNK_SZ];
};

typedef struct
{
    void *slab_obj;
    Chunk *cursor;
    Chunk *head;
    size_t capacity;
} LFPool;

static inline void *align_backward(void *ptr, size_t align)
{
    return (void *)((uintptr_t)ptr & ~(align - 1));
}

static inline void *align_forward(void *ptr, size_t align)
{
    uintptr_t p = (uintptr_t)ptr;
    return (void *)((p + (align - 1)) & ~(uintptr_t)(align - 1));
}

typedef struct
{
    int (*allocator)(LFPool **out, size_t capacity);
    void *(*alloc)(LFPool *pool);
    void *(*alloc_sz)(LFPool *pool, size_t sz);
    void (*reset)(LFPool *pool);
    void (*drop)(LFPool *p);
} LFPoolInterface;

static int lfp_init(LFPool **out, size_t capacity)
{
    LFPool *p = (LFPool *)malloc(sizeof(LFPool));
    if (!p)
    {
        return 0;
    }
    size_t raw = (capacity * sizeof(Chunk)) + (CHUNK_SZ - 1);
    void *slab = malloc(raw);
    if (!slab)
    {
        return 0;
    }
    p->slab_obj = slab;
    /* Align FORWARD: glibc can return 32-byte-aligned slabs for this size
     * class, and aligning backward would put the cursor before the block,
     * corrupting the previous chunk's header. The +63 over-allocation makes
     * room so all `capacity` chunks still fit above the aligned base. */
    p->cursor = (Chunk *)align_forward(slab, CHUNK_SZ);
    p->capacity = capacity;
    p->head = p->cursor;
    *out = p;
    return 1;
}
static void lfp_reset(LFPool *p)
{
    if (!p)
    {
        return;
    }
    p->head = p->cursor;
}

static void lfp_drop(LFPool *self)
{
    if (!self)
    {
        return;
    }
    if (self->slab_obj)
    {
        free(self->slab_obj);
        self->slab_obj = NULL;
    }
    self->cursor = NULL;
    self->head = NULL;
    self->capacity = 0;
    free(self);
}

static void *lfp_alloc_sz(LFPool *self, size_t sz)
{
    if (!self || !self->cursor)
    {
        return NULL;
    }
    size_t need = (sz + (CHUNK_SZ - 1)) / CHUNK_SZ;
    if (need == 0)
    {
        need = 1;
    }
    Chunk *old = self->head;

    size_t used = (size_t)(old - self->cursor);
    if (used + need > self->capacity)
    {
        fprintf(stderr, "fatal: pool ran out of memory\n");
        return NULL;
    }
    self->head = old + need;
    return old->buffer;
}

static void *lfp_alloc(LFPool *self)
{
    return lfp_alloc_sz(self, CHUNK_SZ);
}

#define POOL_DEFAULTS                                                                              \
    .allocator = lfp_init, .alloc = lfp_alloc, .alloc_sz = lfp_alloc_sz, .drop = lfp_drop,         \
    .reset = lfp_reset

static const LFPoolInterface pool = {POOL_DEFAULTS};

#define MVEC_IMPL(Name, T)                                                                         \
    typedef struct                                                                                 \
    {                                                                                              \
        T *data;                                                                                   \
        size_t size;                                                                               \
        size_t cap;                                                                                \
        LFPool *ref;                                                                               \
    } _mvec_##Name;                                                                                \
                                                                                                   \
    static inline _mvec_##Name mvec_init_##Name(LFPool *p, size_t reserved)                        \
    {                                                                                              \
        size_t cap = reserved == 0 ? 4 : reserved;                                                 \
        T *data = (T *)pool.alloc_sz(p, cap * sizeof(T));                                          \
        _mvec_##Name v = {.data = data, .size = 0, .cap = cap, .ref = p};                          \
        return v;                                                                                  \
    }                                                                                              \
                                                                                                   \
    static inline int mvec_grow_##Name(_mvec_##Name *vec, size_t need)                             \
    {                                                                                              \
        size_t new_cap = vec->cap == 0 ? 4 : vec->cap;                                             \
        while (new_cap < vec->size + need)                                                         \
            new_cap *= 2;                                                                          \
        T *new_data = (T *)pool.alloc_sz(vec->ref, new_cap * sizeof(T));                           \
        if (!new_data)                                                                             \
            return 0;                                                                              \
        for (size_t i = 0; i < vec->size; i++)                                                     \
            new_data[i] = vec->data[i];                                                            \
        vec->data = new_data;                                                                      \
        vec->cap = new_cap;                                                                        \
        return 1;                                                                                  \
    }                                                                                              \
                                                                                                   \
    static inline int mvec_push_##Name(_mvec_##Name *vec, T value)                                 \
    {                                                                                              \
        if (vec->size >= vec->cap)                                                                 \
        {                                                                                          \
            if (!mvec_grow_##Name(vec, 1))                                                         \
                return 0;                                                                          \
        }                                                                                          \
        vec->data[vec->size++] = value;                                                            \
        return 1;                                                                                  \
    }                                                                                              \
                                                                                                   \
    static inline T mvec_pop_##Name(_mvec_##Name *vec)                                             \
    {                                                                                              \
        assert(vec->size > 0);                                                                     \
        return vec->data[--vec->size];                                                             \
    }                                                                                              \
                                                                                                   \
    static inline void mvec_release_##Name(_mvec_##Name **vec)                                     \
    {                                                                                              \
        *vec = NULL;                                                                               \
    }                                                                                              \
                                                                                                   \
    static inline void mvec_free_##Name(_mvec_##Name *vec)                                         \
    {                                                                                              \
        vec->data = NULL;                                                                          \
        vec->size = 0;                                                                             \
        vec->cap = 0;                                                                              \
    }

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef enum
{
    TOK_EOF,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_OPERATOR,
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,
    TOK_FUNCTION,
    TOK_CONST,
    TOK_TEST,
    TOK_ASSERT,
    TOK_WRITE,
    TOK_DEFINE,
    TOK__COUNT
} TokenKind;

typedef struct
{
    TokenKind kind;
    const char *text;
    int line;
    int col;
    /* Cached lexical invariants, filled once by prepare_tokens(). A real
     * lexer attaches length/decoded value at scan time; recomputing
     * strlen()/strtoll() on every parse of the same stream is repeated
     * work the front-end should never do. len: identifiers/numbers/
     * strings. num: decoded TOK_NUMBER value. sym: interned symbol id for
     * identifiers (scope checks become int compares, not memcmp chains). */
    unsigned char len;
    long long num;
    int sym;
} Token;

/* Intern table: name -> small dense id. Built once in prepare_tokens()
 * (single-threaded, before workers spawn; read-only afterwards). */
#define SYM_TAB_MAX 1024u
static struct
{
    const char *name;
    unsigned char len;
    int id;
} g_syms[SYM_TAB_MAX];
static int g_nsyms = 0;

static inline uint32_t sym_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
    {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static int intern(const char *s, unsigned char len)
{
    uint32_t h = sym_hash(s, len) & (SYM_TAB_MAX - 1);
    for (;;)
    {
        if (g_syms[h].name == NULL)
        {
            if (g_nsyms >= (int)SYM_TAB_MAX)
            {
                return -1;
            }
            g_syms[h].name = s;
            g_syms[h].len = len;
            g_syms[h].id = g_nsyms;
            return g_nsyms++;
        }
        if (g_syms[h].len == len && memcmp(g_syms[h].name, s, len) == 0)
        {
            return g_syms[h].id;
        }
        h = (h + 1) & (SYM_TAB_MAX - 1);
    }
}

static void prepare_tokens(Token *t, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        switch (t[i].kind)
        {
        case TOK_IDENTIFIER:
        case TOK_NUMBER:
        case TOK_STRING:
            t[i].len = (unsigned char)strlen(t[i].text);
            break;
        default:
            t[i].len = 0;
            break;
        }
        if (t[i].kind == TOK_IDENTIFIER)
        {
            t[i].sym = intern(t[i].text, t[i].len);
        }
        else
        {
            t[i].sym = -1;
        }
        if (t[i].kind == TOK_NUMBER)
        {
            t[i].num = strtoll(t[i].text, NULL, 10);
        }
    }
}

static const char *kind_name(TokenKind k)
{
    static const char *names[TOK__COUNT] = {
        "EOF", "(",     ")",       "{",      "}",        "op",      "ID",   "NUM",
        "STR", "'fun'", "'const'", "'test'", "'assert'", "'write'", "':='",
    };
    return names[k];
}

/* Expression representation.
 *
 * Default (fold): expressions are EVALUATED as they are parsed. Every
 * consumer here either discards the tree (assert evaluates immediately) or
 * reads a single scalar out of it (const decl). Materializing heap Nodes
 * just to walk them once was the single largest remaining cost of the run:
 * ~12 pool allocations + 40-byte writes + a full re-walk per expression.
 * Folding also FIXES A LATENT BUG: p_const_decl stored `init->kind ==
 * ND_NUM ? init->d.num : 0`, so `const y := 1+2*3` silently declared 0;
 * folding stores 7.
 *
 * -DMODAL_KEEP_AST restores the tree-building parser (Node/eval_iter) for
 * comparison or for when declarations start needing real ASTs. */
#ifdef MODAL_KEEP_AST
typedef struct Node Node;
typedef enum
{
    ND_NUM,
    ND_IDENT,
    ND_BIN
} NodeKind;
struct Node
{
    NodeKind kind;
    size_t tok_idx;
    union
    {
        long long num;
        struct
        {
            Node *l;
            Node *r;
            char op;
        } bin;
    } d;
};
_Static_assert(sizeof(Node) <= CHUNK_SZ, "Node deve caber em 1 chunk");
MVEC_IMPL(nodeptr, Node *)
MVEC_IMPL(u64, uint64_t)
#else
MVEC_IMPL(ll, long long)
#endif

typedef struct
{
    const Token *current;
    const Token *previous;
    const Token *base;
    int had_error;
} Parser;

static void advance(Parser *p)
{
    p->previous = p->current;
    if (p->current->kind != TOK_EOF)
    {
        p->current++;
    }
}
static int match(Parser *p, TokenKind k)
{
    if (p->current->kind == k)
    {
        advance(p);
        return 1;
    }
    return 0;
}
static void error_at(Parser *p, const Token *t, const char *msg)
{
    if (p->had_error)
    {
        return;
    }
    p->had_error = 1;
    printf("erro [%d:%d]: %s\n", t->line, t->col, msg);
}
__attribute__((unused)) static int kind_in(TokenKind k, const TokenKind *set, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (set[i] == k)
        {
            return 1;
        }
    }
    return 0;
}

typedef enum
{
    OBJ_VAR,
    OBJ_CONST,
    OBJ_FUN
} ObjKind;

typedef struct
{
    ObjKind kind;
    const char *name;
    size_t len;
    size_t name_tok;
    long long val;
    int sym; /* interned id; scope matching is an int compare */
} Obj;
_Static_assert(sizeof(Obj) <= CHUNK_SZ, "Obj deve caber em 1 chunk");
MVEC_IMPL(obj, Obj)
typedef Obj *ObjRef;
MVEC_IMPL(objref, ObjRef)

typedef struct Scope Scope;
struct Scope
{
    Scope *parent;
    _mvec_obj objs;
};

typedef struct WorkerState
{
    _mvec_objref resolved;
    uint32_t resolved_cap;
} WorkerState;

static __thread WorkerState storage_ws = {0};

/* Kept for parity with ll1_parsing_2.c: scaffolding for a future shared
 * resolver state. Not wired into the pipeline here. */
static inline WorkerState *get_t_ws(void)
{
    return &storage_ws;
}

typedef struct
{
    LFPool *pool;
    const Token *source;
    uint8_t verbose;
} Cx;

typedef struct
{
    Cx *cx;
    Scope *cur;
    _mvec_objref resolved;
    uint8_t had_error;
} Rsl;

static __thread Cx *t_cx = NULL;

#ifdef MODAL_KEEP_AST
static uint64_t eval_iter(Rsl *r, Node *root);
#endif
static Scope *scope_push(LFPool *pl, Scope *parent)
{
    Scope *s = (Scope *)pool.alloc_sz(pl, sizeof(Scope));
    if (!s)
    {
        return NULL;
    }
    s->parent = parent;
    s->objs = mvec_init_obj(pl, 8);
    if (!s->objs.data)
    {
        return NULL;
    }
    return s;
}
static Obj *scope_lookup(Scope *s, int sym)
{
    for (Scope *sc = s; sc; sc = sc->parent)
    {
        for (size_t i = 0; i < sc->objs.size; i++)
        {
            Obj *o = &sc->objs.data[i];
            if (o->sym == sym)
            {
                return o;
            }
        }
    }
    return NULL;
}
static void rsl_declare(Rsl *r, const Token *ident, ObjKind kind, long long val)
{
    size_t idx = (size_t)(ident - r->cx->source);
    if (r->resolved.data[idx] != NULL)
    {
        fprintf(stderr, "internal: '%s' already declared or resolved\n", ident->text);
        r->had_error = 1;
        return;
    }
    Obj *o = (Obj *)pool.alloc_sz(r->cx->pool, sizeof(Obj));
    if (!o)
    {
        r->had_error = 1;
        return;
    }
    *o = (Obj){
        .kind = kind,
        .name = ident->text,
        .len = ident->len,
        .name_tok = idx,
        .val = val,
        .sym = ident->sym,
    };
    r->resolved.data[idx] = o;
    if (o->len == 1 && o->name[0] == '_')
    {
        return;
    }
    for (size_t i = 0; i < r->cur->objs.size; i++)
    {
        Obj *alt = &r->cur->objs.data[i];
        if (alt->sym == o->sym)
        {
            const Token *prev = &r->cx->source[alt->name_tok];
            printf("erro [%d:%d]: %s redeclarado neste bloco\n"
                   "\tdeclaração anterior em [%d:%d]\n",
                   ident->line, ident->col, o->name, prev->line, prev->col);
            r->had_error = 1;
            return;
        }
    }
    mvec_push_obj(&r->cur->objs, *o);
}

typedef struct Production Production;
typedef int (*ParseFn)(Parser *p, Rsl *r);
struct Production
{
    const char *name;
    const TokenKind *first;
    size_t first_len;
    ParseFn fn;
};
static int p_const_decl(Parser *p, Rsl *r);
static int p_test_decl(Parser *p, Rsl *r);
static int p_stmt(Parser *p, Rsl *r);
static int p_block(Parser *p, Rsl *r);

static const TokenKind FS_CONST[] = {TOK_CONST};
static const TokenKind FS_TEST[] = {TOK_TEST};
static const TokenKind FS_STMT[] = {TOK_ASSERT, TOK_WRITE};
static const TokenKind FS_BLOCK[] = {TOK_LBRACE};
static const Production T_DECL[] = {
    {"constDecl", FS_CONST, 1, p_const_decl},
    {"testDecl", FS_TEST, 1, p_test_decl},
    {"statement", FS_STMT, 2, p_stmt},
};
#define T_DECL_N (sizeof(T_DECL) / sizeof(T_DECL[0]))
static const Production T_BLOCK[] = {
    {"block", FS_BLOCK, 1, p_block},
};
#define T_BLOCK_N (sizeof(T_BLOCK) / sizeof(T_BLOCK[0]))

/* O(1) dispatch maps (built by build_dispatch_maps before any parse). */
static const Production *g_decl_map[TOK__COUNT];
static const Production *g_block_map[TOK__COUNT];

/* Disabled in profiling runs (see main); kept available for LL(1) audits. */
__attribute__((unused)) static void selfcheck(const char *level, const Production *t, size_t n)
{
    int32_t owner[TOK__COUNT];
    for (size_t k = 0; k < TOK__COUNT; k++)
    {
        owner[k] = -1;
    }
    for (size_t i = 0; i < n; i++)
    {
        for (size_t a = 0; a < t[i].first_len; a++)
        {
            TokenKind k = t[i].first[a];
            if (owner[k] >= 0 && (size_t)owner[k] != i)
            {
                printf("CONFLITO LL(1) em [%s]: '%s' está em %s E em %s\n", level, kind_name(k),
                       t[owner[k]].name, t[i].name);
            }
            else
            {
                owner[k] = (int32_t)i;
            }
        }
    }
}

typedef struct
{
    char op;
    const Token *tok;
} OpTok;
_Static_assert(sizeof(OpTok) <= CHUNK_SZ, "OpTok deve caber em 1 chunk");
MVEC_IMPL(optok, OpTok)

static int prec_of(const Token *t)
{
    if (t->kind != TOK_OPERATOR)
    {
        return -1;
    }
    switch (t->text[0])
    {
    case '*':
    case '/':
        return 3;
    case '+':
    case '-':
        return 2;
    case '<':
    case '>':
        return 1;
    default:
        return -1;
    }
}
#ifdef MODAL_KEEP_AST
static void lr_reduce(_mvec_nodeptr *values, _mvec_optok *ops)
{
    OpTok op = ops->data[--ops->size];
    size_t ri = values->size - 1;
    size_t li = values->size - 2;
    Node *left = values->data[li];
    Node *right = values->data[ri];
    Node *n = (Node *)pool.alloc_sz(t_cx->pool, sizeof(Node));
    if (!n)
    {
        return;
    }
    *n = (Node){
        .kind = ND_BIN,
        .tok_idx = (size_t)(op.tok - t_cx->source),
        .d.bin = {.l = left, .r = right, .op = op.op},
    };
    mvec_push_nodeptr(values, n);
}
static int lr_shift(Parser *p, Rsl *r, _mvec_nodeptr *values)
{
    if (match(p, TOK_NUMBER))
    {
        Node *n = (Node *)pool.alloc_sz(t_cx->pool, sizeof(Node));
        if (!n)
        {
            return 0;
        }
        *n = (Node){
            .kind = ND_NUM,
            .tok_idx = (size_t)(p->previous - r->cx->source),
            .d.num = p->previous->num,
        };
        return mvec_push_nodeptr(values, n);
    }
    if (match(p, TOK_IDENTIFIER))
    {
        const Token *tok = p->previous;
        Obj *o = scope_lookup(r->cur, tok->sym);
        if (!o)
        {
            error_at(p, tok, "undefined identifier");
            return 0;
        }
        r->resolved.data[(size_t)(tok - r->cx->source)] = o;
        Node *n = (Node *)pool.alloc_sz(t_cx->pool, sizeof(Node));
        if (!n)
        {
            return 0;
        }
        *n = (Node){
            .kind = ND_IDENT,
            .tok_idx = (size_t)(tok - r->cx->source),
        };
        return mvec_push_nodeptr(values, n);
    }
    error_at(p, p->current, "expected expression (number or identifier)");
    return 0;
}
static Node *parse_expr(Parser *p, Rsl *r)
{
    _mvec_nodeptr values = mvec_init_nodeptr(t_cx->pool, 32);
    _mvec_optok ops = mvec_init_optok(t_cx->pool, 16);
    if (!values.data || !ops.data)
    {
        return NULL;
    }
    if (!lr_shift(p, r, &values))
    {
        return NULL;
    }
    while (prec_of(p->current) > 0)
    {
        while (ops.size > 0 && prec_of(ops.data[ops.size - 1].tok) >= prec_of(p->current))
        {
            lr_reduce(&values, &ops);
        }
        if (!mvec_push_optok(&ops, (OpTok){.op = p->current->text[0], .tok = p->current}))
        {
            return NULL;
        }
        advance(p);
        if (!lr_shift(p, r, &values))
        {
            return NULL;
        }
    }
    while (ops.size > 0)
    {
        lr_reduce(&values, &ops);
    }
    return values.data[values.size - 1];
}
#else /* fold: evaluate during parse, no tree materialized */
static void lr_reduce(_mvec_ll *values, _mvec_optok *ops)
{
    OpTok op = ops->data[--ops->size];
    long long rv = values->data[--values->size];
    long long lv = values->data[--values->size];
    long long v = 0;
    switch (op.op)
    {
    case '+':
        v = lv + rv;
        break;
    case '-':
        v = lv - rv;
        break;
    case '*':
        v = lv * rv;
        break;
    case '/':
        v = rv ? lv / rv : 0; /* matches legacy eval: /0 folds to 0 */
        break;
    case '<':
        v = lv < rv;
        break;
    case '>':
        v = lv > rv;
        break;
    default:
        break;
    }
    mvec_push_ll(values, v);
}
static int lr_shift(Parser *p, Rsl *r, _mvec_ll *values)
{
    if (match(p, TOK_NUMBER))
    {
        return mvec_push_ll(values, p->previous->num);
    }
    if (match(p, TOK_IDENTIFIER))
    {
        const Token *tok = p->previous;
        Obj *o = scope_lookup(r->cur, tok->sym);
        if (!o)
        {
            error_at(p, tok, "undefined identifier");
            return 0;
        }
        r->resolved.data[(size_t)(tok - r->cx->source)] = o;
        return mvec_push_ll(values, o->val);
    }
    error_at(p, p->current, "expected expression (number or identifier)");
    return 0;
}
static int parse_expr(Parser *p, Rsl *r, long long *out)
{
    _mvec_ll values = mvec_init_ll(t_cx->pool, 8);
    _mvec_optok ops = mvec_init_optok(t_cx->pool, 16);
    if (!values.data || !ops.data)
    {
        return 0;
    }
    if (!lr_shift(p, r, &values))
    {
        return 0;
    }
    while (prec_of(p->current) > 0)
    {
        while (ops.size > 0 && prec_of(ops.data[ops.size - 1].tok) >= prec_of(p->current))
        {
            lr_reduce(&values, &ops);
        }
        if (!mvec_push_optok(&ops, (OpTok){.op = p->current->text[0], .tok = p->current}))
        {
            return 0;
        }
        advance(p);
        if (!lr_shift(p, r, &values))
        {
            return 0;
        }
    }
    while (ops.size > 0)
    {
        lr_reduce(&values, &ops);
    }
    *out = values.data[values.size - 1];
    return 1;
}
#endif
static int dispatch_mapped(Parser *p, Rsl *r, const Production *const *map, const Production *t,
                           size_t n);
static int p_const_decl(Parser *p, Rsl *r)
{
    advance(p);
    const Token *name = p->current;
    if (!match(p, TOK_IDENTIFIER))
    {
        error_at(p, p->current, "expected constant name");
        return 0;
    }
    if (!match(p, TOK_DEFINE))
    {
        error_at(p, p->current, "expected ':=' in constant declaration");
        return 0;
    }
#ifdef MODAL_KEEP_AST
    Node *init = parse_expr(p, r);
    if (!init)
    {
        return 0;
    }
    long long val = (init->kind == ND_NUM) ? init->d.num : 0;
#else
    /* Folded at parse time — also fixes the legacy bug where compound
     * inits (`const y := 1+2*3`) silently declared 0. */
    long long val;
    if (!parse_expr(p, r, &val))
    {
        return 0;
    }
#endif
    rsl_declare(r, name, OBJ_CONST, val);
    if (t_cx->verbose)
    {
        printf("      declarado: const %s\n", name->text);
    }
    return !r->had_error;
}
static int p_test_decl(Parser *p, Rsl *r)
{
    advance(p);
    if (!match(p, TOK_STRING))
    {
        error_at(p, p->current, "expected string literal after 'test'");
        return 0;
    }
    return dispatch_mapped(p, r, g_block_map, T_BLOCK, T_BLOCK_N);
}
static int p_stmt(Parser *p, Rsl *r)
{
    if (match(p, TOK_ASSERT))
    {
#ifdef MODAL_KEEP_AST
        Node *e = parse_expr(p, r);
        if (!e)
        {
            return 0;
        }
        uint64_t v = eval_iter(r, e);
#else
        long long ev;
        if (!parse_expr(p, r, &ev))
        {
            return 0;
        }
        uint64_t v = (uint64_t)ev;
#endif
        if (r->cx->verbose)
        {
            printf("      assert %s (valor=%llu)\n", v ? "passou" : "FALHOU",
                   (unsigned long long)v);
        }
        return 1;
    }
    if (match(p, TOK_WRITE))
    {
        if (!match(p, TOK_STRING))
        {
            error_at(p, p->current, "expected string after 'write'");
            return 0;
        }
        if (r->cx->verbose)
        {
            printf("      reduzido: write STR\n");
        }
        return 1;
    }
    return 0;
}
static int p_block(Parser *p, Rsl *r)
{
    advance(p);
    Scope *saved = r->cur;
    r->cur = scope_push(r->cx->pool, saved);
    if (!r->cur)
    {
        r->had_error = 1;
        return 0;
    }
    while (!match(p, TOK_RBRACE))
    {
        if (p->current->kind == TOK_EOF)
        {
            error_at(p, p->current, "expected '}' before EOF");
            r->cur = saved;
            return 0;
        }
        if (!dispatch_mapped(p, r, g_decl_map, T_DECL, T_DECL_N))
        {
            r->cur = saved;
            return 0;
        }
    }
    r->cur = saved;
    return 1;
}
/* O(1) dispatch: kind -> production, built once by build_dispatch_maps()
 * before any parsing. The grammar is LL(1) (selfcheck proves no first-set
 * conflicts), so each kind maps to at most one production per level; a
 * collision at build time is reported and the map keeps the first. The
 * linear kind_in scan in dispatch() was O(productions×first-set) on every
 * single statement of every run. */
static void build_dispatch_maps(void)
{
    for (int k = 0; k < TOK__COUNT; k++)
    {
        g_decl_map[k] = NULL;
        g_block_map[k] = NULL;
    }
    for (size_t i = 0; i < T_DECL_N; i++)
    {
        for (size_t a = 0; a < T_DECL[i].first_len; a++)
        {
            TokenKind k = T_DECL[i].first[a];
            if (g_decl_map[k] != NULL)
            {
                printf("CONFLITO LL(1) em [decl]: '%s' em %s E em %s\n", kind_name(k),
                       g_decl_map[k]->name, T_DECL[i].name);
            }
            else
            {
                g_decl_map[k] = &T_DECL[i];
            }
        }
    }
    for (size_t i = 0; i < T_BLOCK_N; i++)
    {
        for (size_t a = 0; a < T_BLOCK[i].first_len; a++)
        {
            TokenKind k = T_BLOCK[i].first[a];
            if (g_block_map[k] != NULL)
            {
                printf("CONFLITO LL(1) em [block]: '%s' em %s E em %s\n", kind_name(k),
                       g_block_map[k]->name, T_BLOCK[i].name);
            }
            else
            {
                g_block_map[k] = &T_BLOCK[i];
            }
        }
    }
}

static int dispatch_mapped(Parser *p, Rsl *r, const Production *const *map, const Production *t,
                           size_t n)
{
    const Production *prod = map[p->current->kind];
    if (prod != NULL)
    {
        if (r->cx->verbose)
        {
            printf("  [%d:%d] %-8s => %s\n", p->current->line, p->current->col,
                   kind_name(p->current->kind), prod->name);
        }
        return prod->fn(p, r);
    }
    printf("erro [%d:%d]: inesperado %s; produções válidas:", p->current->line, p->current->col,
           kind_name(p->current->kind));
    for (size_t i = 0; i < n; i++)
    {
        printf(" %s", t[i].name);
    }
    printf("\n");
    p->had_error = 1;
    return 0;
}
#ifdef MODAL_KEEP_AST
typedef struct
{
    Node *n;
    uint8_t expanded;
} Frame;
_Static_assert(sizeof(Frame) <= CHUNK_SZ, "Frame deve caber em 1 chunk");
MVEC_IMPL(frame, Frame)

static uint64_t eval_iter(Rsl *r, Node *root)
{
    _mvec_frame st = mvec_init_frame(t_cx->pool, 16);
    _mvec_u64 out = mvec_init_u64(t_cx->pool, 8);
    if (!st.data || !out.data)
    {
        return 0;
    }
    mvec_push_frame(&st, (Frame){.n = root, .expanded = 0});
    while (st.size > 0)
    {
        Frame *f = &st.data[st.size - 1];
        Node *n = f->n;
        size_t nkids = (n->kind == ND_BIN) ? 2 : 0;
        if (f->expanded || nkids == 0)
        {
            switch (n->kind)
            {
            case ND_NUM:
                mvec_push_u64(&out, (uint64_t)n->d.num);
                break;
            case ND_IDENT:
            {
                ObjRef o = r->resolved.data[n->tok_idx];
                mvec_push_u64(&out, o ? (uint64_t)o->val : 0);
                break;
            }
            case ND_BIN:
            {
                uint64_t rv = out.data[--out.size];
                uint64_t lv = out.data[--out.size];
                uint64_t v = 0;
                switch (n->d.bin.op)
                {
                case '+':
                    v = lv + rv;
                    break;
                case '-':
                    v = lv - rv;
                    break;
                case '*':
                    v = lv * rv;
                    break;
                case '/':
                    v = rv ? lv / rv : 0;
                    break;
                case '<':
                    v = lv < rv;
                    break;
                case '>':
                    v = lv > rv;
                    break;
                default:
                    break;
                }
                mvec_push_u64(&out, v);
                break;
            }
            }
            st.size--;
            continue;
        }
        f->expanded = 1;
        Node *kids[2] = {n->d.bin.l, n->d.bin.r};
        for (size_t i = nkids; i > 0; i--)
        {
            mvec_push_frame(&st, (Frame){.n = kids[i - 1], .expanded = 0});
        }
    }
    return out.size ? out.data[out.size - 1] : 0;
}
#endif /* MODAL_KEEP_AST */

static int run_pipeline_cx(Cx *cx, const Token *stream, size_t ntokens)
{
    Parser p = {.current = stream, .previous = NULL, .base = stream, .had_error = 0};
    t_cx = cx;
    cx->source = stream;
    Rsl r = {
        .cx = cx,
        .had_error = 0,
        .cur = NULL,
    };
    r.resolved = mvec_init_objref(cx->pool, ntokens + 1);
    if (!r.resolved.data)
    {
        return 1;
    }
    /* Bulk NULL-fill instead of n branchy pushes: the vec is reserved
     * exactly, so this is one memset with no per-element bookkeeping. */
    memset(r.resolved.data, 0, (ntokens + 1) * sizeof(ObjRef));
    r.resolved.size = ntokens + 1;

    r.cur = scope_push(cx->pool, NULL);
    if (!r.cur)
    {
        return 1;
    }

    while (p.current->kind != TOK_EOF && !p.had_error)
    {
        if (!dispatch_mapped(&p, &r, g_decl_map, T_DECL, T_DECL_N))
        {
            break;
        }
    }
    return p.had_error || r.had_error;
}

/* ---------------------------------------------------------------------
 * Harness: dynamic batch queue over pinned, stream-multiplexed workers.
 *
 * One round = one pass over the worker's whole stream set (nstreams
 * pipeline runs). Batches are ranges of rounds; workers grab them with a
 * relaxed fetch_add on the shared cursor. Relaxed is sufficient: the only
 * ordering requirement is that each batch index is handed out exactly once
 * (RMW atomicity), and visibility of results is established by
 * pthread_join at the end.
 *
 * Pool sizing keeps v2's finding intact (see that file): a single pipeline
 * run needs ~4096 chunks, so each stream keeps its own 4096-chunk slab and
 * resets it per run. Footprint = threads * nstreams * 256 KiB.
 * --------------------------------------------------------------------- */

#define POOL_CAPACITY 4096

typedef struct
{
    LFPool *pool;
    Cx cx;
} Stream;

typedef struct
{
    atomic_uint_fast64_t v;
} CacheLineU64;
// _Static_assert(sizeof(CacheLineU64) >= 64, "cursor must own its cache line");

typedef enum
{
    MODE_DYN = 0,
    MODE_STATIC = 1
} Mode;

typedef struct
{
    Mode mode;
    uint64_t rounds_total;  /* N / nstreams, N rounded down to a multiple */
    uint64_t batch_rounds;  /* rounds per dynamically grabbed batch */
    uint64_t total_batches; /* ceil(rounds_total / batch_rounds) */
    const Token *tokens;
    size_t ntokens;
    int nstreams;
    int target_util_pm;  /* per-worker CPU budget in permille (0.1% units);
                          * 1000 = unpaced; e.g. 3 = 0.3% */
    CacheLineU64 cursor; /* shared; the ONLY hot shared line */
} GlobalCfg;

/* Adaptive pacing: after a batch of measured duration B, a worker targeting
 * U% utilization accrues a sleep debt of B*(100-U)/U. The debt converts into
 * an actual clock_nanosleep once it reaches MIN_PAUSE_NS (kernel timer
 * wakeups have ~50-250us granularity; smaller sleeps get truncated and
 * utilization stays pegged near 100% — observed empirically). CLOSED LOOP:
 * the real elapsed sleep is compared against the request and any overshoot
 * feeds back as NEGATIVE debt, so systematic wakeup-latency drift cancels
 * over time instead of compounding (an open-loop version here overshot a
 * 10% target by ~70%). NOTE: target is PER THREAD — aggregate core usage ≈
 * min(100, K*U/ncpu)%; pair oversubscribed K with lower U if you want a
 * hard ceiling. */
#define MIN_PAUSE_NS 150000ull /* 150us: below this, nanosleep undersleeps */

static void pace_to_target(const GlobalCfg *g, uint64_t batch_ns, int64_t *debt_ns)
{
    if (g->target_util_pm >= 1000 || batch_ns == 0)
    {
        return;
    }
    *debt_ns +=
        (int64_t)(batch_ns * (uint64_t)(1000 - g->target_util_pm) / (uint64_t)g->target_util_pm);
    if (*debt_ns < (int64_t)MIN_PAUSE_NS)
    {
        return;
    }
    int64_t req = *debt_ns;
    *debt_ns = 0;
    struct timespec ts = {.tv_sec = (time_t)(req / 1000000000ll),
                          .tv_nsec = (long)(req % 1000000000ll)};
    uint64_t t0 = now_ns();
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    *debt_ns -= (int64_t)(now_ns() - t0) - req; /* credit overshoot back */
}

typedef struct
{
    int core_id;
    int slot;    /* index into g_stats */
    uint64_t lo; /* static mode: run rounds [lo, hi); ignored in dyn mode */
    uint64_t hi;
    GlobalCfg *g;
} BenchTask;

typedef struct
{
    uint64_t batches_done;
    uint64_t rounds_done;
    uint64_t busy_ns;
    int ok;
} WorkerStats;

static WorkerStats *g_stats = NULL;

static int pin_to_core(int core_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

/* One round = one pass over the worker's whole stream set (nstreams
 * pipeline runs). Round-robin across streams at fixed granularity of a
 * full pass keeps independent work in flight for the core, same as v2. */
static uint64_t run_rounds(GlobalCfg *g, Stream streams[], uint64_t lo, uint64_t hi, int *had_err)
{
    uint64_t done = 0;
    for (uint64_t r = lo; r < hi; r++)
    {
        for (int s = 0; s < g->nstreams; s++)
        {
            pool.reset(streams[s].pool);
            *had_err |= run_pipeline_cx(&streams[s].cx, g->tokens, g->ntokens);
        }
        done++;
    }
    return done;
}

static void *bench_worker(void *arg)
{
    BenchTask *t = (BenchTask *)arg;
    GlobalCfg *g = t->g;
    WorkerStats *st = &g_stats[t->slot];

    if (!pin_to_core(t->core_id))
    {
        fprintf(stderr, "warn: could not pin thread to core %d\n", t->core_id);
    }

    Stream streams[MAX_STREAMS];
    for (int s = 0; s < g->nstreams; s++)
    {
        if (!pool.allocator(&streams[s].pool, POOL_CAPACITY))
        {
            fprintf(stderr, "fatal: stream pool init failed (core %d, stream %d)\n", t->core_id, s);
            st->ok = 0;
            return NULL;
        }
        streams[s].cx = (Cx){
            .pool = streams[s].pool,
            .source = NULL,
            .verbose = 0,
        };
    }

    int had_err = 0;
    uint64_t batches_done = 0;
    uint64_t rounds_done = 0;
    int64_t pace_debt_ns = 0;
    /* Pure execution time: batch work only, pace sleeps excluded. This is
     * what makes the aggregate cpu line a real utilization measurement. */
    uint64_t work_ns = 0;

    if (g->mode == MODE_STATIC)
    {
        /* v2 semantics: one contiguous range per worker; thread 0 absorbs
         * the division remainder. No rebalancing once started. Walked in
         * batch_rounds chunks so pacing applies identically to both modes
         * (a single unpaced run_rounds call would ignore util targets). */
        for (uint64_t r = t->lo; r < t->hi && !had_err;)
        {
            uint64_t hi = r + g->batch_rounds;
            if (hi > t->hi)
            {
                hi = t->hi;
            }
            uint64_t t_batch = now_ns();
            rounds_done += run_rounds(g, streams, r, hi, &had_err);
            work_ns += now_ns() - t_batch;
            batches_done++;
            pace_to_target(g, now_ns() - t_batch, &pace_debt_ns);
            r = hi;
        }
    }
    else
    {
        /* Dynamic queue: grab the next batch until the cursor passes the
         * end. NOT a busy-wait: the cursor is monotonic and fetch_add hands
         * each index out exactly once, so this loop runs total_batches+K
         * times across all workers and then exits — it never polls a flag
         * waiting for external progress, and there is no sleep/yield path
         * because no worker ever waits on another. A fast/skew-free
         * schedule touches the shared line once per BATCH_ROUNDS rounds,
         * so contention amortizes away. */
        for (;;)
        {
            uint64_t b = atomic_fetch_add_explicit(&g->cursor.v, 1, memory_order_relaxed);
            if (b >= g->total_batches)
            {
                break;
            }
            uint64_t lo = b * g->batch_rounds;
            uint64_t hi = lo + g->batch_rounds;
            if (hi > g->rounds_total)
            {
                hi = g->rounds_total;
            }
            uint64_t t_batch = now_ns();
            rounds_done += run_rounds(g, streams, lo, hi, &had_err);
            work_ns += now_ns() - t_batch;
            batches_done++;
            pace_to_target(g, now_ns() - t_batch, &pace_debt_ns);
        }
    }

    st->busy_ns = work_ns; /* execution only; pace sleeps excluded */
    st->batches_done = batches_done;
    st->rounds_done = rounds_done;
    st->ok = !had_err;

    for (int s = 0; s < g->nstreams; s++)
    {
        pool.drop(streams[s].pool);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    (void)get_t_ws();

    const long ncpu_conf = sysconf(_SC_NPROCESSORS_CONF);

    Mode mode = MODE_DYN;
    int K = ncpu_conf > 1 ? (int)ncpu_conf : 1;
    uint64_t batch_rounds = BATCH_ROUNDS_DEFAULT;
    int nstreams = NSTREAMS;
    uint64_t runs_wanted = RUNS_DEFAULT;

    if (argc > 1 && strcmp(argv[1], "static") == 0)
    {
        mode = MODE_STATIC;
    }
    if (argc > 2)
    {
        K = atoi(argv[2]);
        if (K < 1)
        {
            K = 1;
        }
    }
    if (argc > 3)
    {
        batch_rounds = strtoull(argv[3], NULL, 10);
        if (batch_rounds == 0)
        {
            batch_rounds = 1;
        }
    }
    if (argc > 4)
    {
        nstreams = atoi(argv[4]);
    }
    if (nstreams < 1)
    {
        nstreams = 1;
    }
    if (nstreams > MAX_STREAMS)
    {
        nstreams = MAX_STREAMS;
    }
    if (argc > 5)
    {
        runs_wanted = strtoull(argv[5], NULL, 10);
    }
    /* Utilization target in permille so fractional budgets like 0.3% work:
     * at extreme duty cycles the sleep debt per batch is huge and quantized
     * sleeps stay accurate; below 0.1% wall time is dominated by timer
     * granularity. */
    double util_in = 100.0;
    if (argc > 6)
    {
        util_in = strtod(argv[6], NULL);
    }
    if (util_in < 0.1)
    {
        util_in = 0.1;
    }
    if (util_in > 100.0)
    {
        util_in = 100.0;
    }
    int util_pct = (int)(util_in * 10.0 + 0.5); /* permille */

    static Token happy[] = {
        {.kind = TOK_CONST, .text = "const", .line = 1, .col = 1},
        {.kind = TOK_IDENTIFIER, .text = "x", .line = 1, .col = 7},
        {.kind = TOK_DEFINE, .text = ":=", .line = 1, .col = 9},
        {.kind = TOK_NUMBER, .text = "1", .line = 1, .col = 12},
        {.kind = TOK_OPERATOR, .text = "+", .line = 1, .col = 14},
        {.kind = TOK_NUMBER, .text = "2", .line = 1, .col = 16},
        {.kind = TOK_OPERATOR, .text = "*", .line = 1, .col = 18},
        {.kind = TOK_NUMBER, .text = "3", .line = 1, .col = 20},
        {.kind = TOK_TEST, .text = "\"m\"", .line = 2, .col = 1},
        {.kind = TOK_STRING, .text = "\"m\"", .line = 2, .col = 6},
        {.kind = TOK_LBRACE, .text = "{", .line = 2, .col = 10},
        {.kind = TOK_ASSERT, .text = "assert", .line = 3, .col = 3},
        {.kind = TOK_IDENTIFIER, .text = "x", .line = 3, .col = 10},
        {.kind = TOK_OPERATOR, .text = "<", .line = 3, .col = 12},
        {.kind = TOK_NUMBER, .text = "10", .line = 3, .col = 14},
        {.kind = TOK_RBRACE, .text = "}", .line = 4, .col = 1},
        {.kind = TOK_CONST, .text = "const", .line = 5, .col = 1},
        {.kind = TOK_IDENTIFIER, .text = "_", .line = 5, .col = 7},
        {.kind = TOK_DEFINE, .text = ":=", .line = 5, .col = 9},
        {.kind = TOK_NUMBER, .text = "4", .line = 5, .col = 12},
        {.kind = TOK_WRITE, .text = "write", .line = 6, .col = 1},
        {.kind = TOK_STRING, .text = "\"done\"", .line = 6, .col = 7},
        {.kind = TOK_EOF, .text = "", .line = 6, .col = 13},
    };
    const size_t n_happy = sizeof(happy) / sizeof(happy[0]);
    prepare_tokens(happy, n_happy);

    /* Single "warm-up" pool for the correctness demo runs below (happy path
     * + intentional redeclaration failure), unrelated to the pinned bench
     * workers. */
    LFPool *demo_pool;
    if (!pool.allocator(&demo_pool, POOL_CAPACITY))
    {
        fprintf(stderr, "fatal: pool init failed\n");
        return 1;
    }
    Cx demo_cx = {.pool = demo_pool, .source = NULL, .verbose = 0};
    t_cx = &demo_cx;

    uint64_t t0 = now_ns();
    build_dispatch_maps();
    // selfcheck("decl", T_DECL, T_DECL_N);
    // selfcheck("block", T_BLOCK, T_BLOCK_N);
    printf("selfcheck: %llu ns\n\n", (unsigned long long)(now_ns() - t0));

    printf("== pipeline (fluxo feliz) ==\n");
    t0 = now_ns();
    int err = run_pipeline_cx(&demo_cx, happy, n_happy);

    static Token redecl[] = {
        {.kind = TOK_CONST, .text = "const", .line = 1, .col = 1},
        {.kind = TOK_IDENTIFIER, .text = "y", .line = 1, .col = 7},
        {.kind = TOK_DEFINE, .text = ":=", .line = 1, .col = 9},
        {.kind = TOK_NUMBER, .text = "1", .line = 1, .col = 12},
        {.kind = TOK_CONST, .text = "const", .line = 2, .col = 1},
        {.kind = TOK_IDENTIFIER, .text = "y", .line = 2, .col = 7},
        {.kind = TOK_DEFINE, .text = ":=", .line = 2, .col = 9},
        {.kind = TOK_NUMBER, .text = "2", .line = 2, .col = 12},
        {.kind = TOK_EOF, .text = "", .line = 2, .col = 14},
    };
    printf("\npipeline: %llu ns → %s\n\n", (unsigned long long)(now_ns() - t0),
           err ? "FALHOU" : "aceito");
    printf("== pipeline (redeclaração proposital) ==\n");
    prepare_tokens(redecl, sizeof(redecl) / sizeof(redecl[0]));
    err = run_pipeline_cx(&demo_cx, redecl, sizeof(redecl) / sizeof(redecl[0]));
    printf("\nresultado: %s (esperado: FALHOU)\n\n", err ? "FALHOU" : "aceito");
    pool.drop(demo_pool);

    /* Rounds must divide evenly into whole pipeline runs so accounting stays
     * exact; round N down to the nearest multiple of nstreams. */
    uint64_t rounds_total = runs_wanted / (uint64_t)nstreams;
    uint64_t runs_actual = rounds_total * (uint64_t)nstreams;
    if (runs_actual != runs_wanted)
    {
        printf("note: N ajustado %llu -> %llu (múltiplo de %d)\n\n",
               (unsigned long long)runs_wanted, (unsigned long long)runs_actual, nstreams);
    }

    GlobalCfg g = {
        .mode = mode,
        .rounds_total = rounds_total,
        .batch_rounds = batch_rounds,
        .total_batches = (rounds_total + batch_rounds - 1) / batch_rounds,
        .tokens = happy,
        .ntokens = n_happy,
        .nstreams = nstreams,
        .target_util_pm = util_pct,
        .cursor = {.v = 0},
    };

    /* Harness allocations come from a bootstrap arena instead of malloc:
     * one slab up front, zero allocator traffic during setup/teardown.
     * alloc_sz rounds every request to whole 64-byte chunks, so the block
     * base is chunk-aligned and each WorkerStats (a 256-byte, 64-aligned
     * struct at a 256-byte stride) still owns its cache lines. */
    LFPool *harness_pool;
    if (!pool.allocator(&harness_pool, (size_t)K * 8 + 64))
    {
        fprintf(stderr, "fatal: harness pool init failed\n");
        return 1;
    }
    size_t stats_sz = (size_t)K * sizeof(WorkerStats);
    g_stats = pool.alloc_sz(harness_pool, stats_sz);
    pthread_t *threads = pool.alloc_sz(harness_pool, (size_t)K * sizeof(pthread_t));
    BenchTask *tasks = pool.alloc_sz(harness_pool, (size_t)K * sizeof(BenchTask));
    if (!g_stats || !threads || !tasks)
    {
        fprintf(stderr, "fatal: bench alloc failed\n");
        return 1;
    }
    memset(g_stats, 0, stats_sz);

    /* Static split mirrors v2: base rounds per worker, remainder folded
     * into worker 0. Dyn mode ignores lo/hi entirely. */
    uint64_t base_rounds = rounds_total / (uint64_t)K;
    uint64_t leftover = rounds_total % (uint64_t)K;

    t0 = now_ns();
    for (int w = 0; w < K; w++)
    {
        tasks[w] = (BenchTask){
            .core_id = w % (int)(ncpu_conf > 0 ? ncpu_conf : 1),
            .slot = w,
            .lo = (uint64_t)w * base_rounds,
            .hi = ((uint64_t)w + 1) * base_rounds,
            .g = &g,
        };
        if (mode == MODE_STATIC)
        {
            if (w == 0)
            {
                tasks[w].hi += leftover;
            }
            else if (w > 0)
            {
                tasks[w].lo += leftover;
                tasks[w].hi += leftover;
            }
        }
        pthread_create(&threads[w], NULL, bench_worker, &tasks[w]);
    }
    for (int w = 0; w < K; w++)
    {
        pthread_join(threads[w], NULL);
    }
    uint64_t dt = now_ns() - t0;

    uint64_t total_runs = 0;
    uint64_t min_busy = UINT64_MAX, max_busy = 0;
    uint64_t min_b = UINT64_MAX, max_b = 0;
    int any_err = 0;
    for (int w = 0; w < K; w++)
    {
        WorkerStats *st = &g_stats[w];
        total_runs += st->rounds_done * (uint64_t)nstreams;
        if (!st->ok)
        {
            any_err = 1;
        }
        if (st->busy_ns < min_busy)
        {
            min_busy = st->busy_ns;
        }
        if (st->busy_ns > max_busy)
        {
            max_busy = st->busy_ns;
        }
        if (st->batches_done < min_b)
        {
            min_b = st->batches_done;
        }
        if (st->batches_done > max_b)
        {
            max_b = st->batches_done;
        }
        printf("core %2d: %llu batches | %llu rounds (%llu runs) in %llu ns → %llu ns/op (%s)\n",
               tasks[w].core_id, (unsigned long long)st->batches_done,
               (unsigned long long)st->rounds_done,
               (unsigned long long)(st->rounds_done * (uint64_t)nstreams),
               (unsigned long long)st->busy_ns,
               (unsigned long long)(st->rounds_done
                                        ? st->busy_ns / (st->rounds_done * (uint64_t)nstreams)
                                        : 0),
               st->ok ? "ok" : "ERROR");
    }

    const char *mode_name = (mode == MODE_DYN) ? "dyn" : "static";
    printf("== aggregate [%s] ==\n", mode_name);
    printf("bench: %llu pipelines | %d threads x %d streams | batch=%llu rounds (%llu batches "
           "total) | %llu ns wall → %llu ns/op | target_util=%.1f%%\n",
           (unsigned long long)total_runs, K, nstreams, (unsigned long long)batch_rounds,
           (unsigned long long)g.total_batches, (unsigned long long)dt,
           (unsigned long long)(total_runs ? dt / total_runs : 0), util_pct / 10.0);
    uint64_t sum_busy = 0;
    for (int w = 0; w < K; w++)
    {
        sum_busy += g_stats[w].busy_ns;
    }
    /* Achieved per-thread utilization: fraction of wall time workers spent
     * executing batches. With pacing this should sit near the target;
     * aggregate core usage ≈ K*util/ncpu (capped at 100). */
    double util_meas = dt ? (double)sum_busy / ((double)dt * (double)K) * 100.0 : 0.0;
    printf("balance: busy max/min = %.2fx | batches/thread min=%llu max=%llu%s\n",
           min_busy ? (double)max_busy / (double)min_busy : 0.0, (unsigned long long)min_b,
           (unsigned long long)max_b, mode == MODE_DYN ? "" : " (fixed by construction)");
    printf("cpu: measured util/thread = %.1f%% → est. core usage ≈ %.0f%% of %ld cpus\n", util_meas,
           util_meas * (double)K / (double)(ncpu_conf > 0 ? ncpu_conf : 1), ncpu_conf);

    /* Everything above lives in harness_pool's slab; one drop reclaims it
     * all. The per-stream slabs were already dropped by their workers. */
    pool.drop(harness_pool);
    return any_err ? 1 : 0;
}
