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
#define CHUNK_SZ 64
#define TRACING 0
/* Each OS thread now multiplexes NSTREAMS independent pipelines instead of
 * relying on the kernel to time-slice extra pthreads. This buys instruction-
 * level parallelism (stream B has independent work ready while stream A
 * stalls on a cache miss) without paying for kernel context switches or
 * cross-thread cache eviction. See POOL_CAPACITY below for why the slab size
 * shrinks accordingly. */
#define NSTREAMS 8
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
    if (TRACING == 1)
    {
        printf("ptr:   %p\n", ptr);
        printf("align: %zu\n", align);
    }
    return (void *)((uintptr_t)ptr & ~(align - 1));
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
    p->cursor = (Chunk *)align_backward(slab, CHUNK_SZ);
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
        fprintf(stderr, "fatal: pool ran out of memory\n[Process exited %d]\n", EXIT_FAILURE);
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
} Token;
static const char *kind_name(TokenKind k)
{
    static const char *names[TOK__COUNT] = {
        "EOF", "(",     ")",       "{",      "}",        "op",      "ID",   "NUM",
        "STR", "'fun'", "'const'", "'test'", "'assert'", "'write'", "':='",
    };
    return names[k];
}
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
static int kind_in(TokenKind k, const TokenKind *set, size_t n)
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

static __thread WorkerState *t_ws = NULL;

static inline WorkerState *get_t_ws(void)
{
    static __thread WorkerState storage = {0};
    return &storage;
};

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

static uint64_t eval_iter(Rsl *r, Node *root);
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
static Obj *scope_lookup(Scope *s, const char *name, size_t len)
{
    for (Scope *sc = s; sc; sc = sc->parent)
    {
        for (size_t i = 0; i < sc->objs.size; i++)
        {
            Obj *o = &sc->objs.data[i];
            if (o->len == len && memcmp(o->name, name, len) == 0)
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
        .len = strlen(ident->text),
        .name_tok = idx,
        .val = val,
    };
    r->resolved.data[idx] = o;
    if (o->len == 1 && o->name[0] == '_')
    {
        return;
    }
    for (size_t i = 0; i < r->cur->objs.size; i++)
    {
        Obj *alt = &r->cur->objs.data[i];
        if (alt->len == o->len && memcmp(alt->name, o->name, o->len) == 0)
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

static void selfcheck(const char *level, const Production *t, size_t n)
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
            .d.num = strtoll(p->previous->text, NULL, 10),
        };
        return mvec_push_nodeptr(values, n);
    }
    if (match(p, TOK_IDENTIFIER))
    {
        const Token *tok = p->previous;
        Obj *o = scope_lookup(r->cur, tok->text, strlen(tok->text));
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
static int dispatch(Parser *p, Rsl *r, const Production *t, size_t n);
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
    Node *init = parse_expr(p, r);
    if (!init)
    {
        return 0;
    }
    long long val = (init->kind == ND_NUM) ? init->d.num : 0;
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
    return dispatch(p, r, T_BLOCK, T_BLOCK_N);
}
static int p_stmt(Parser *p, Rsl *r)
{
    if (match(p, TOK_ASSERT))
    {
        Node *e = parse_expr(p, r);
        if (!e)
        {
            return 0;
        }
        uint64_t v = eval_iter(r, e);
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
        if (!dispatch(p, r, T_DECL, T_DECL_N))
        {
            r->cur = saved;
            return 0;
        }
    }
    r->cur = saved;
    return 1;
}
static int dispatch(Parser *p, Rsl *r, const Production *t, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (kind_in(p->current->kind, t[i].first, t[i].first_len))
        {
            if (r->cx->verbose)
            {
                printf("  [%d:%d] %-8s => %s\n", p->current->line, p->current->col,
                       kind_name(p->current->kind), t[i].name);
            }
            return t[i].fn(p, r);
        }
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
    /* NOTE: WorkerState/t_ws/get_t_ws() below were leftover scaffolding —
     * ws->resolved was never populated anywhere in this file, so a prior
     * `if (!ws->resolved.data) return 1;` guard here was firing on every
     * single call, unconditionally, before parsing/resolving/evaluating
     * ever ran. The perf loop never checked this function's return value,
     * so that early-return was invisible: the old benchmark was timing the
     * stub path (alloc the resolved array, bail), not the real pipeline.
     * Removed the guard; get_t_ws() is left in place in case it's mid-wiring
     * into the real resolver elsewhere in your codebase. */
    r.resolved = mvec_init_objref(cx->pool, ntokens + 1);

    for (size_t i = 0; i <= ntokens; i++)
    {
        if (!mvec_push_objref(&r.resolved, NULL))
        {
            return 1;
        }
    }

    r.cur = scope_push(cx->pool, NULL);
    if (!r.cur)
    {
        return 1;
    }

    while (p.current->kind != TOK_EOF && !p.had_error)
    {
        if (!dispatch(&p, &r, T_DECL, T_DECL_N))
        {
            break;
        }
    }
    return p.had_error || r.had_error;
}

/* ---------------------------------------------------------------------
 * Per-OS-thread interleaved streams.
 *
 * Instead of spawning ncpu * NSTREAMS pthreads (which reintroduces the
 * exact cache-thrashing/oversubscription problem this refactor fixes),
 * each OS thread is pinned to one physical core and internally round-robins
 * across NSTREAMS independent (pool, Cx) pairs. Each stream's pool is its
 * own small slab; NSTREAMS of them are sized to still fit comfortably in
 * that core's L2, so switching between streams inside the same core stays
 * cheap (no kernel involvement, no eviction of a *different core's* cache)
 * while giving the out-of-order engine independent work to chew on when one
 * stream stalls on a cache-line fill.
 * --------------------------------------------------------------------- */

/* IMPORTANT CORRECTION: this was originally shrunk to 512 chunks on the
 * (wrong) assumption that a single pipeline run's peak footprint would
 * scale down with pool "budget." It doesn't — a single run_pipeline_cx()
 * call over the `happy` token stream has a fixed peak working set (Scopes,
 * Objs, Nodes, _mvec_* temporaries) that needs the full 4096 chunks
 * (256 KiB) your original single-worker design used; capacities of 1024 or
 * 2048 chunks fail with "pool ran out of memory". That means NSTREAMS
 * streams together need NSTREAMS * 256 KiB = 2 MiB here, which is likely
 * BIGGER than one core's L2 (commonly 256 KiB-1 MiB). So the "stays
 * cache-resident across streams" claim needs to be verified with
 * `perf stat -e cache-misses`, not assumed — the real benefit of
 * interleaving here may be limited to hiding memory latency (independent
 * work in flight while one stream stalls), not full cache residency. If you
 * want true cache-fit, you'd need to shrink the compiled program itself
 * (fewer tokens/nodes per run), not just the pool budget. */
#define POOL_CAPACITY 4096

typedef struct
{
    LFPool *pool;
    Cx cx;
} Stream;

typedef struct
{
    int core_id;
    const Token *stream_tokens;
    size_t ntokens;
    int iters_per_stream; /* iterations to run on EACH of the NSTREAMS streams */
    uint64_t my_ns;
    int ok;
} BenchTask;

static int pin_to_core(int core_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

static void *bench_worker(void *arg)
{
    BenchTask *t = (BenchTask *)arg;

    if (!pin_to_core(t->core_id))
    {
        fprintf(stderr, "warn: could not pin thread to core %d\n", t->core_id);
    }

    Stream streams[NSTREAMS];
    for (int s = 0; s < NSTREAMS; s++)
    {
        if (!pool.allocator(&streams[s].pool, POOL_CAPACITY))
        {
            fprintf(stderr, "fatal: stream pool init failed (core %d, stream %d)\n", t->core_id, s);
            t->ok = 0;
            return NULL;
        }
        streams[s].cx = (Cx){
            .pool = streams[s].pool,
            .source = NULL,
            .verbose = 0,
        };
    }

    int had_err = 0;
    uint64_t s0 = now_ns();

    /* Round-robin across streams at a fixed granularity of "one pipeline
     * run" instead of running one stream to exhaustion before touching the
     * next — this is what keeps independent work in flight for the CPU to
     * overlap. */
    for (int i = 0; i < t->iters_per_stream; i++)
    {
        for (int s = 0; s < NSTREAMS; s++)
        {
            pool.reset(streams[s].pool);
            t_cx = &streams[s].cx;
            had_err |= run_pipeline_cx(&streams[s].cx, t->stream_tokens, t->ntokens);
        }
    }

    t->my_ns = now_ns() - s0;
    t->ok = !had_err;

    for (int s = 0; s < NSTREAMS; s++)
    {
        pool.drop(streams[s].pool);
    }
    return NULL;
}

int main(void)
{
    get_t_ws();
    static const Token happy[] = {
        {TOK_CONST, "const", 1, 1},     {TOK_IDENTIFIER, "x", 1, 7}, {TOK_DEFINE, ":=", 1, 9},
        {TOK_NUMBER, "1", 1, 12},       {TOK_OPERATOR, "+", 1, 14},  {TOK_NUMBER, "2", 1, 16},
        {TOK_OPERATOR, "*", 1, 18},     {TOK_NUMBER, "3", 1, 20},    {TOK_TEST, "\"m\"", 2, 1},
        {TOK_STRING, "\"m\"", 2, 6},    {TOK_LBRACE, "{", 2, 10},    {TOK_ASSERT, "assert", 3, 3},
        {TOK_IDENTIFIER, "x", 3, 10},   {TOK_OPERATOR, "<", 3, 12},  {TOK_NUMBER, "10", 3, 14},
        {TOK_RBRACE, "}", 4, 1},        {TOK_CONST, "const", 5, 1},  {TOK_IDENTIFIER, "_", 5, 7},
        {TOK_DEFINE, ":=", 5, 9},       {TOK_NUMBER, "4", 5, 12},    {TOK_WRITE, "write", 6, 1},
        {TOK_STRING, "\"done\"", 6, 7}, {TOK_EOF, "", 6, 13},
    };
    const size_t n_happy = sizeof(happy) / sizeof(happy[0]);

    /* One OS thread per physical core. No `<< 3` here: that shift was what
     * spawned 8x more pthreads than cores, forcing the kernel to preempt and
     * evict cache between unrelated workers. NSTREAMS gives us the "8x" back
     * as cooperative, same-core multiplexing instead. */
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    int K = ncpu > 1 ? (int)ncpu : 1;

    /* Single "warm-up" pool for the correctness demo runs below (happy path
     * + intentional redeclaration failure), unrelated to the pinned bench
     * workers. */
    LFPool *demo_pool;
    if (!pool.allocator(&demo_pool, 4096))
    {
        fprintf(stderr, "fatal: pool init failed\n");
        return 1;
    }
    Cx demo_cx = {.pool = demo_pool, .source = NULL, .verbose = 0};
    t_cx = &demo_cx;

    uint64_t t0 = now_ns();
    selfcheck("decl", T_DECL, T_DECL_N);
    selfcheck("block", T_BLOCK, T_BLOCK_N);
    printf("selfcheck: %llu ns\n\n", (unsigned long long)(now_ns() - t0));
    printf("== pipeline (fluxo feliz) ==\n");
    t0 = now_ns();

    int err = run_pipeline_cx(&demo_cx, happy, n_happy);

    static const Token redecl[] = {
        {TOK_CONST, "const", 1, 1}, {TOK_IDENTIFIER, "y", 1, 7}, {TOK_DEFINE, ":=", 1, 9},
        {TOK_NUMBER, "1", 1, 12},   {TOK_CONST, "const", 2, 1},  {TOK_IDENTIFIER, "y", 2, 7},
        {TOK_DEFINE, ":=", 2, 9},   {TOK_NUMBER, "2", 2, 12},    {TOK_EOF, "", 2, 14},
    };

    printf("\npipeline: %llu ns → %s\n\n", (unsigned long long)(now_ns() - t0),
           err ? "FALHOU" : "aceito");
    printf("== pipeline (redeclaração proposital) ==\n");
    err = run_pipeline_cx(&demo_cx, redecl, sizeof(redecl) / sizeof(redecl[0]));
    printf("\nresultado: %s (esperado: FALHOU)\n\n", err ? "FALHOU" : "aceito");
    pool.drop(demo_pool);

    /* Parallel bench: K pinned OS threads, each multiplexing NSTREAMS
     * interleaved pipelines. Total pipeline runs stays N, same as before:
     * N == K * NSTREAMS * iters_per_stream (as close as integer division
     * allows). */
    const int N = 100000000;
    int total_slots = K * NSTREAMS;
    int base_iters = N / total_slots;
    int leftover = N % total_slots; /* absorbed into thread 0 for simplicity */

    pthread_t *threads = malloc((size_t)K * sizeof(pthread_t));
    if (!threads)
    {
        return 1;
    }
    BenchTask *tasks = malloc((size_t)K * sizeof(BenchTask));
    if (!tasks)
    {
        return 1;
    }

    t0 = now_ns();

    for (int w = 0; w < K; w++)
    {
        int extra_iters = 0;
        if (leftover > 0)
        {
            /* Distribute the remainder in whole per-stream iterations so
             * every stream in a task still runs the same iters_per_stream;
             * simplest correct approach is to just fold all leftover into
             * thread 0's iters_per_stream (rounded up). */
            extra_iters = (w == 0) ? (leftover + NSTREAMS - 1) / NSTREAMS : 0;
        }
        tasks[w] = (BenchTask){
            .core_id = w % (int)ncpu,
            .stream_tokens = happy,
            .ntokens = n_happy,
            .iters_per_stream = base_iters + extra_iters,
            .my_ns = 0,
            .ok = 0,
        };
        pthread_create(&threads[w], NULL, bench_worker, &tasks[w]);
    }
    for (int w = 0; w < K; w++)
    {
        pthread_join(threads[w], NULL);
    }
    uint64_t dt = now_ns() - t0;

    /* Per-worker report. */
    printf("== per-worker (core-pinned, %d streams each) ==\n", NSTREAMS);
    uint64_t total_runs = 0;
    for (int w = 0; w < K; w++)
    {
        uint64_t runs = (uint64_t)tasks[w].iters_per_stream * NSTREAMS;
        total_runs += runs;
        printf("core %d: %llu pipeline runs in %llu ns → %llu ns/op (%s)\n", tasks[w].core_id,
               (unsigned long long)runs, (unsigned long long)tasks[w].my_ns,
               (unsigned long long)(tasks[w].my_ns / runs), tasks[w].ok ? "ok" : "ERROR");
    }
    printf("== aggregate ==\n");
    printf("bench: %llu pipelines × %d cores × %d streams/core in %llu ns → %llu ns/op (parallel "
           "wall)\n",
           (unsigned long long)total_runs, K, NSTREAMS, (unsigned long long)dt,
           (unsigned long long)(dt / total_runs));

    free(threads);
    free(tasks);
    return 0;
}
