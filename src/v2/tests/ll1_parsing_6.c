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
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define MODAL_X86 1
#endif

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

/* P4.5: 16-byte token, packed by construction (doc §4 "store LENGTH in
 * 1 byte", shrink toward 16B):
 *   num  i64 — decoded TOK_NUMBER value at scan time (0 otherwise); no
 *             strtoll re-decode at parse time
 *   off  u32 — byte offset into the mapped source; text = src + off.
 *             Zero-copy strings: nothing to relocate, no pointers stored.
 *   sym  i16 — interned id for identifiers (-1 otherwise); scope checks
 *             stay int compares. SYM_TAB_MAX=1024 fits trivially.
 *   kind u8  — TokenKind; TOK__COUNT=15 fits one byte
 *   len  u8  — token length; 255 escapes to an overflow list for
 *             pathological tokens (doc §4's escape hatch)
 * line/col are GONE from the struct: derivable on error from off by
 * counting newlines (cold path), never needed on the hot path.
 * Cache math: 48B -> 16B = 4 tokens per 64B line; the demo run writes
 * ~23*48=1104B of tokens today, ~368B after this lands. */
typedef struct
{
    int64_t num;
    uint32_t off;
    int16_t sym;
    uint8_t kind;
    uint8_t len;
} Tok16;
_Static_assert(sizeof(Tok16) == 16, "Tok16 must pack to exactly 16 bytes");
#define TOK16_LEN_ESC 255u

/* Escape valve for tokens >= 255B (long strings/identifiers): real length
 * lives here as (token index, length) pairs, appended during scan. The
 * demo corpus never hits it; gates exercise it with a synthetic source.
 * Thread-local because worker threads scan concurrently. Linear walk is
 * correct here — cold path by construction. */
typedef struct
{
    uint32_t idx;
    uint32_t len;
} LenEsc;
#define LEN_ESC_MAX 64
static __thread LenEsc esc_list[LEN_ESC_MAX];
static __thread int esc_n = 0;

static size_t tok_len(const Tok16 *t, size_t i)
{
    if (t[i].len != TOK16_LEN_ESC)
    {
        return t[i].len;
    }
    for (int k = 0; k < esc_n; k++)
    {
        if (esc_list[k].idx == i)
        {
            return esc_list[k].len;
        }
    }
    return 0;
}

/* Shared by both scanners: store a token length in 1 byte, escaping into
 * the TLS overflow list when it doesn't fit. Returns 0 only when the
 * (cold-path) overflow list itself is exhausted. */
static inline int tok16_set_len(Tok16 *t, size_t idx, size_t len)
{
    if (len >= TOK16_LEN_ESC)
    {
        if (esc_n == LEN_ESC_MAX)
        {
            return 0;
        }
        t->len = (uint8_t)TOK16_LEN_ESC;
        esc_list[esc_n].idx = (uint32_t)idx;
        esc_list[esc_n].len = (uint32_t)len;
        esc_n++;
    }
    else
    {
        t->len = (uint8_t)len;
    }
    return 1;
}

/* line/col derivation (cold path): count newlines in src[0..off). The
 * leading '\n' sentinel makes the first body line number 1, matching what
 * scan-time bookkeeping produced before P4.5. */
static void tok_line_col(const char *src, uint32_t off, int *line, int *col)
{
    int ln = 0;
    size_t ls = 0;
    for (size_t i = 0; i < off; i++)
    {
        if (src[i] == '\n')
        {
            ln++;
            ls = i + 1;
        }
    }
    *line = ln;
    *col = (int)(off - ls) + 1;
}

/* Intern table: name -> small dense id. Populated during the startup gates
 * (single-threaded, before workers spawn; read-only afterwards), so worker
 * scans take pure lookup paths. */
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

/* last-symbol memo: identifiers repeat heavily ("const"/"x" alternate).
 * Names are stable pointers into the source buffer and entries are never
 * deleted, so an exact-pointer hit implies the same id — no hash, no
 * memcmp on the hot path. MUST be thread-local: a shared memo lets one
 * thread observe another's transient id during the table's settling
 * window, giving two tokens of the SAME identifier different ids inside
 * a single scan (decl vs use) -> spurious "undefined identifier". The
 * table itself is written only before pthread_create (startup gates
 * pre-intern every g_src identifier), so worker threads
 * only ever take read paths on g_syms. */
static __thread const char *memo_s = NULL;
static __thread int memo_id = -1;

static int intern(const char *s, unsigned char len)
{
    if (s == memo_s)
    {
        return memo_id;
    }
    int id = -1;
    uint32_t h = sym_hash(s, len) & (SYM_TAB_MAX - 1);
    for (;;)
    {
        if (g_syms[h].name == NULL)
        {
            if (g_nsyms >= (int)SYM_TAB_MAX)
            {
                goto out;
            }
            g_syms[h].name = s;
            g_syms[h].len = len;
            g_syms[h].id = g_nsyms;
            id = g_nsyms++;
            goto out;
        }
        if (g_syms[h].len == len && memcmp(g_syms[h].name, s, len) == 0)
        {
            id = g_syms[h].id;
            goto out;
        }
        h = (h + 1) & (SYM_TAB_MAX - 1);
    }
out:
    memo_s = s;
    memo_id = id;
    return id;
}

/* ---------------------------------------------------------------------------
 * P4.1/P4.5 — source buffer + token production.
 *
 * Turns raw bytes into the same token stream phases 1-3 consumed from static
 * arrays. This is the baseline every SWAR/SIMD step (P4.2+) must match; it
 * also becomes part of the measured pipeline, so ns/op now covers lex ->
 * parse -> resolve -> fold of the demo program from source text.
 *
 * Document techniques applied here (scalar forms, vectorized later):
 *   - sentinels: buffer starts with '\n' (safe lookback, line counter starts
 *     at 0 and the leading newline bumps it to 1 before any token) and ends
 *     with "\"'\n" past the logical end, so quote scanning can peek past the
 *     last byte and check for the terminator after the loop. P4.4 extends
 *     the tail with NUL padding so keyword matching can always load a full
 *     8-byte word from any position (the masked compare ignores the pad).
 *   - upper-bound allocation: caller provides body_len+1 token slots (every
 *     byte its own token is the worst case); no growth checks in the loop.
 *   - lengths attached at scan time (real spans), numbers decoded at scan
 *     time (no strtoll), symbols interned at scan time.
 *
 * P4.5 — zero-copy mapped source + 16-byte tokens:
 *   - the source lives in one mmap'd region, copied there ONCE at setup;
 *     steady state reads bytes in place — no per-run allocation or copy for
 *     source or token texts (texts are byte offsets into the mapping)
 *   - Token shrinks 48B -> 16B (see Tok16 above); the scanner writes ~3x
 *     less memory per run and the parser re-reads correspondingly less.
 * ------------------------------------------------------------------------- */
#define SRC_BODY                                                                                   \
    "\nconst x := 1 + 2 * 3\ntest \"m\" {\n  assert x < 10\n}\nconst _ := 4\nwrite \"done\"\n"
/* tail sentinel: "\"'\n" for quote peeking + NUL pad so an 8-byte load at
 * any body offset stays inside the mapping (keyword SWAR needs >=8). */
static const char g_src[] = SRC_BODY "\"'\n"
                                     "\0\0\0\0\0\0\0\0\0\0\0\0\0";
#define G_SRC_BODY_LEN (sizeof(SRC_BODY) - 1)
static const char *g_src_map = g_src; /* replaced by the mmap in main() */

/* ---------------------------------------------------------------------------
 * D4 — guard-page sentinels (brief §6 D4).
 *
 * Every span walker's tail sentinel already guarantees a terminator exists
 * within the mapping (>=13-16 NUL bytes past the logical body, and NUL is
 * not a member of the ident/digit/space classes), so span_ident/span_digit/
 * span_space's "i + 8 <= limit" bounds check is redundant on any call whose
 * remaining bytes cover the tail pad — it can NEVER actually stop the loop
 * before the sentinel does, so removing it doesn't change scanner behavior,
 * only cuts one branch per 8-byte word. That claim only holds for a REAL
 * mapping backed by real memory past the pad, though: without something
 * enforcing "stop reading here" at the OS level, a bug that violates the
 * sentinel invariant (a corpus lacking the NUL pad, a body_len that doesn't
 * match the actual mapping, etc.) would silently read past the allocation
 * into whatever memory happens to follow it instead of crashing where the
 * bug is, which is far worse to debug than a clean SIGSEGV at the fault
 * site. map_source_with_guard_page gives both: the removed checks are safe
 * BECAUSE a PROT_NONE page sits immediately after the readable region, so
 * any read that somehow does go past the sentinel (violated invariant, or a
 * hypothetically miscounted len elsewhere) faults immediately and loudly
 * instead of reading adjacent heap/mmap contents.
 *
 * NOTE (per explicit scope decision): span_str_end/span_to_quote_d's
 * unterminated-string check is NOT covered by this argument — a string
 * missing its closing quote has no guaranteed in-bounds terminator (the
 * NUL pad isn't a quote), so that call site KEEPS its limit check exactly
 * as before; scan_tokens16's "unterminated string" error path is
 * unchanged. Guard page is additive safety there, not a basis for removing
 * the check — the check is load-bearing for a real, reachable input shape. */
static char *map_source_with_guard_page(const char *body, size_t alloc_bytes, size_t *out_map_sz)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t data_sz = (alloc_bytes + page - 1) & ~(page - 1); /* whole pages for the readable part */
    size_t total_sz = data_sz + page;                        /* + one PROT_NONE guard page */
    char *base = mmap(NULL, total_sz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
    {
        return NULL;
    }
    if (mprotect(base, data_sz, PROT_READ | PROT_WRITE) != 0)
    {
        munmap(base, total_sz);
        return NULL;
    }
    /* base[data_sz .. total_sz) stays PROT_NONE: the guard page. Any read
     * at or past base+data_sz faults. data_sz is >= alloc_bytes and a
     * whole number of pages, so the guard page starts exactly one page
     * past the last page containing real data — no gap. */
    memcpy(base, body, alloc_bytes);
    if (out_map_sz)
    {
        *out_map_sz = total_sz;
    }
    return base;
}

/* ---------------------------------------------------------------------------
 * D0 — realistic-scale corpus (brief §6, directive D0).
 *
 * The 77B demo above is fixed-cost dominated (brief §4: hollow-EMIT floor
 * ~198 ns/call regardless of technique), so every ns/op number measured on
 * it is dominated by per-token dispatch overhead, not by scan/parse work
 * that scales with bytes. D0 says: generate >=1MB of representative source
 * (mixed idents/numbers/strings/nesting) and re-baseline EVERYTHING there
 * before judging D1-D9. This corpus is built from the SAME four-production
 * grammar the parser already accepts (const decl / test decl / assert /
 * write, nested blocks) — no new syntax, so ref_scan16, scan_tokens16, and
 * the parser/resolver all exercise real code paths, not a synthetic stub.
 *
 * Deterministic (fixed seed, no clock/env input) so the corpus, its
 * ref_scan16 oracle stream, and every gate diff are exactly reproducible
 * across runs and machines, per the brief's "interleave A/B ... medians of
 * >=6 reps" methodology (a corpus that changes between reps would make
 * that meaningless).
 *
 * Sizing: identifiers are drawn from a fixed name pool with skewed
 * (Zipf-ish) reuse — real source repeats a small set of names heavily,
 * which is exactly the case the intern memo (D7) and per-scope resolve
 * (D8) are meant to exploit; a corpus of all-unique identifiers would
 * flatter neither and misrepresent D0's own re-baseline. Numbers are
 * mostly 1-3 digits with an occasional wider one, matching brief §6/D6's
 * note that demo numbers are 1-2 digits (no-op today) but real numeric
 * spans are wider. Nesting depth is bounded so generated blocks always
 * close (grammar has no early-exit block recovery to lean on here).
 * ------------------------------------------------------------------------- */
#define BIG_CORPUS_MIN_BYTES (1u << 20) /* >=1MB per D0 */

static char *g_big = NULL;           /* heap; freed never (process-lifetime) */
static size_t g_big_len = 0;         /* logical body length, like G_SRC_BODY_LEN */
static const char *g_big_map = NULL; /* mmap'd copy, like g_src_map */
static Tok16 *g_big_prep = NULL;     /* D0 oracle token stream, for nolex-replay */
static size_t g_big_n_happy = 0;

/* xorshift32: fast, deterministic, no library dependency, plenty for
 * generating source text (not a security context). */
static inline uint32_t xrand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Small pool of legal identifier spellings, deliberately reused heavily
 * (see header comment) rather than generated fresh each time. All are
 * valid [A-Za-z_][A-Za-z0-9_]* and none collide with the four keywords. */
static const char *const BIG_NAME_POOL[] = {
    "x",   "y",     "z",      "n",     "acc",   "tmp",    "val",   "idx",
    "sum", "count", "total",  "base",  "delta", "offset", "limit", "scale",
    "a1",  "b2",    "helper", "state", "flag",  "buffer", "size",  "step",
    "row", "col",   "width",  "depth", "ratio", "result", "seed",  "mask",
};
#define BIG_NAME_POOL_N (sizeof(BIG_NAME_POOL) / sizeof(BIG_NAME_POOL[0]))
_Static_assert(BIG_NAME_POOL_N == 32, "scope_mask is a uint32_t bitmask, one bit per pool name");

/* References a name known to resolve (declared in the current scope or
 * an ancestor — anything in `visible_mask`). assert/write statements
 * that reference an undeclared identifier hit scope_lookup's failure
 * path (error_at "undefined identifier"), which is exactly the kind of
 * pipeline-level correctness bug D0's full-pipeline run needs to catch,
 * not just scanner-level token equivalence. Falls back to "_" (always
 * declared at top level by big_corpus_generate before any block starts,
 * mirroring the toy corpus's own `const _ := 4`) if nothing is visible
 * yet, which cannot happen in practice given that seeding but is kept as
 * a defensive fallback. */
static const char *big_pick_visible(uint32_t rng_val, uint32_t visible_mask)
{
    if (visible_mask == 0)
    {
        return "_";
    }
    uint32_t start = rng_val % BIG_NAME_POOL_N;
    for (uint32_t k = 0; k < BIG_NAME_POOL_N; k++)
    {
        uint32_t idx = (start + k) % BIG_NAME_POOL_N;
        if ((visible_mask >> idx) & 1u)
        {
            return BIG_NAME_POOL[idx];
        }
    }
    return "_"; /* unreachable given visible_mask != 0, kept defensive */
}

/* Appends one const decl: "const <name> := <expr>\n". expr is a small
 * binary-op chain (+, -, *, <, >) over numbers and previously-declared
 * VISIBLE names so the resolver's scope_lookup path gets real hits, not
 * just decls, and never hits scope_lookup's failure path.
 *
 * scope_mask: bit i set means BIG_NAME_POOL[i] is already declared in the
 * CURRENT block (mirrors rsl_declare's own check, which only scans
 * r->cur->objs — the current scope's own list, not parent scopes). A
 * generated corpus that ignores this redeclares names constantly once a
 * block has more than a few const decls (32-name pool, 3-8 decls/block),
 * which the parser correctly rejects as an error — exactly the class of
 * gate-relevant bug D0 exists to surface, not something to paper over by
 * only checking gate pass/fail on the SCANNER; the full pipeline (parse+
 * resolve+fold) needs a corpus it actually accepts. If every pool name is
 * already used in this scope, falls back to "_" — the SAME name the toy
 * corpus itself uses for "declare and don't care" (rsl_declare special-
 * cases len==1 && name=='_' to skip the redeclaration check entirely).
 *
 * visible_mask: names resolvable right NOW via scope_lookup (declared in
 * this scope so far, or any ancestor). The RHS operand of the expression
 * must be drawn from here, not the raw pool — a name that's merely in the
 * pool but never declared/visible would hit scope_lookup's failure path
 * (error_at "undefined identifier") exactly like big_emit_stmt's assert/
 * write targets. */
static size_t big_emit_const(char *out, size_t cap, size_t w, uint32_t *rng, uint32_t visible_mask,
                             uint32_t *scope_mask)
{
    static const char ops[] = {'+', '-', '*', '<', '>'};
    const char *name = "_";
    /* BIG_NAME_POOL_N == 32 == width of scope_mask, so "all bits set" is
     * ~0u; (1u << 32) would be UB on a 32-bit shift, hence this form. */
    if (*scope_mask != ~0u) /* pool not fully used in this scope */
    {
        uint32_t start = xrand(rng) % BIG_NAME_POOL_N;
        for (uint32_t k = 0; k < BIG_NAME_POOL_N; k++)
        {
            uint32_t idx = (start + k) % BIG_NAME_POOL_N;
            if (!((*scope_mask >> idx) & 1u))
            {
                name = BIG_NAME_POOL[idx];
                *scope_mask |= (1u << idx);
                break;
            }
        }
    }
#define APPEND(s)                                                                                  \
    do                                                                                             \
    {                                                                                              \
        size_t l_ = strlen(s);                                                                     \
        if (w + l_ >= cap)                                                                         \
            return w;                                                                              \
        memcpy(out + w, s, l_);                                                                    \
        w += l_;                                                                                   \
    } while (0)
    APPEND("const ");
    APPEND(name);
    APPEND(" := ");
    char numbuf[16];
    /* 1-3 digit numbers dominate (brief D6: demo numbers are 1-2 digits,
     * a no-op for D6's chunked-fold today); occasionally emit a wide
     * number so numeric-heavy real input (D6's stated target case) is
     * actually represented in the corpus. */
    int v;
    if ((xrand(rng) % 37) == 0)
    {
        v = (int)(xrand(rng) % 900000) + 100000; /* 6-digit wide number */
    }
    else
    {
        v = (int)(xrand(rng) % 900) + 1; /* 1-3 digit, matches toy corpus's range */
    }
    snprintf(numbuf, sizeof numbuf, "%d", v);
    APPEND(numbuf);
    if (visible_mask != 0 && (xrand(rng) % 3) != 0)
    {
        /* Reference an already-VISIBLE name (declared in this scope so
         * far, or an ancestor) — reading doesn't redeclare, and
         * scope_lookup's parent-chain walk is exactly what real resolve
         * traffic looks like. Must come from visible_mask, not the raw
         * pool: an undeclared-but-in-pool name would fail to resolve. */
        char op = ops[xrand(rng) % (sizeof ops)];
        const char *other = big_pick_visible(xrand(rng), visible_mask);
        char opbuf[4] = {' ', op, ' ', 0};
        APPEND(opbuf);
        APPEND(other);
    }
    APPEND("\n");
#undef APPEND
    return w;
}

/* References a name known to resolve (declared in the current scope or
 * an ancestor — anything in `visible_mask`). assert/write statements
 * that reference an undeclared identifier hit scope_lookup's failure
 * path (error_at "undefined identifier"), which is exactly the kind of
 * pipeline-level correctness bug D0's full-pipeline run needs to catch,
 * not just scanner-level token equivalence. See big_pick_visible above
 * for the fallback rationale. */
static size_t big_emit_stmt(char *out, size_t cap, size_t w, uint32_t *rng, uint32_t visible_mask)
{
    const char *name = big_pick_visible(xrand(rng), visible_mask);
#define APPEND(s)                                                                                  \
    do                                                                                             \
    {                                                                                              \
        size_t l_ = strlen(s);                                                                     \
        if (w + l_ >= cap)                                                                         \
            return w;                                                                              \
        memcpy(out + w, s, l_);                                                                    \
        w += l_;                                                                                   \
    } while (0)
    if (xrand(rng) & 1)
    {
        APPEND("assert ");
        APPEND(name);
        char opbuf[4] = {' ', (xrand(rng) & 1) ? '<' : '>', ' ', 0};
        APPEND(opbuf);
        char numbuf[16];
        snprintf(numbuf, sizeof numbuf, "%u", (xrand(rng) % 900) + 1);
        APPEND(numbuf);
        APPEND("\n");
    }
    else
    {
        APPEND("write \"line-");
        char numbuf[16];
        snprintf(numbuf, sizeof numbuf, "%u", xrand(rng) % 100000);
        APPEND(numbuf);
        APPEND("\"\n");
    }
#undef APPEND
    return w;
}

/* Recursively emits a block body up to `depth_left` more levels of
 * "test \"...\" { ... }" nesting (brief: "mixed ... nesting"). Every
 * open brace this function writes is closed before it returns, so the
 * grammar's block production always terminates cleanly regardless of
 * where generation is cut off by `cap`.
 *
 * scope_mask: names declared in THIS block (fresh per block, matching
 * p_block pushing a fresh Scope — reused across sibling/nested blocks is
 * wrong, rsl_declare only rejects redeclaration within r->cur->objs).
 * visible_mask: names resolvable via scope_lookup's parent-chain walk —
 * i.e. everything declared in this block SO FAR, unioned with everything
 * visible in enclosing blocks. Nested test{} blocks get scope_mask reset
 * to 0 (fresh child scope) but visible_mask carried in (child can see
 * parent names) and OR'd with whatever the child itself declares before
 * returning to the parent's still-accumulating visible_mask. */
static size_t big_emit_block_body(char *out, size_t cap, size_t w, uint32_t *rng, int depth_left,
                                  uint32_t visible_mask)
{
    uint32_t scope_mask = 0;
    int nstmts = 3 + (int)(xrand(rng) % 6);
    for (int i = 0; i < nstmts && w + 64 < cap; i++)
    {
        uint32_t pick = xrand(rng) % 10;
        if (pick < 4)
        {
            uint32_t before = scope_mask;
            w = big_emit_const(out, cap, w, rng, visible_mask, &scope_mask);
            visible_mask |= (scope_mask & ~before); /* newly declared name becomes visible */
        }
        else if (pick < 8)
        {
            w = big_emit_stmt(out, cap, w, rng, visible_mask);
        }
        else if (depth_left > 0 && w + 256 < cap)
        {
            char hdr[32];
            int tn = (int)(xrand(rng) % 10000);
            int hl = snprintf(hdr, sizeof hdr, "test \"t%d\" {\n", tn);
            if (w + (size_t)hl >= cap)
            {
                break;
            }
            memcpy(out + w, hdr, (size_t)hl);
            w += (size_t)hl;
            w = big_emit_block_body(out, cap, w, rng, depth_left - 1, visible_mask);
            if (w + 2 < cap)
            {
                out[w++] = '}';
                out[w++] = '\n';
            }
        }
    }
    return w;
}

/* Builds the >=1MB corpus into a freshly malloc'd buffer with the SAME
 * sentinel layout as g_src: leading '\n', body, then "\"'\n" + >=16 NUL
 * pad bytes so every span walker / keyword-SWAR 8-byte load the scanner
 * performs stays in-bounds exactly as it does for the toy corpus (brief
 * §5 "Sentinel contract"). Returns the logical body length (like
 * G_SRC_BODY_LEN); *out_buf receives the full padded allocation.
 *
 * Top level is a flat sequence of const decls / top-level test blocks
 * (mirrors the toy corpus's shape: const, test{...}, const, write), so
 * the existing T_DECL grammar covers 100% of generated content. */
static size_t big_corpus_generate(char **out_buf)
{
    size_t cap = BIG_CORPUS_MIN_BYTES + (BIG_CORPUS_MIN_BYTES / 4) + 4096; /* headroom past 1MB */
    char *buf = (char *)malloc(cap);
    if (!buf)
    {
        fprintf(stderr, "fatal: big corpus alloc failed\n");
        exit(1);
    }
    size_t w = 0;
    buf[w++] = '\n';            /* leading sentinel, matches g_src's line-1 convention */
    uint32_t rng = 0xC0FFEE42u; /* fixed seed: deterministic corpus, see header */
    /* Top level IS a scope too (run_pipeline_cx's outermost scope_push),
     * so it needs the same scope_mask/visible_mask discipline as any
     * nested block — a bare "have_prior" flag (the pre-D0-fix version)
     * let const decls reference undeclared/not-yet-visible names and let
     * the SAME name repeat at top level past the redeclaration check. */
    uint32_t top_scope_mask = 0;
    uint32_t visible_mask = 0;
    /* Seed with a guaranteed declaration before any generator function
     * can be asked to reference a visible name: big_pick_visible's
     * visible_mask==0 fallback returns "_", but "_" itself must have been
     * DECLARED at least once for scope_lookup to resolve it — otherwise
     * the very first assert/write statement (if picked before any const)
     * would reference an undeclared "_" and fail to resolve. "_" is safe
     * to declare unconditionally: rsl_declare exempts it from the
     * redeclaration check entirely (mirrors the toy corpus's own
     * `const _ := 4`), and it stays valid to re-read after any later
     * const decl also assigns to a tracked name. */
    const char *seed = "const _ := 1\n";
    size_t seed_len = strlen(seed);
    memcpy(buf + w, seed, seed_len);
    w += seed_len;
    while (w < BIG_CORPUS_MIN_BYTES && w + 512 < cap)
    {
        uint32_t pick = xrand(&rng) % 10;
        if (pick < 5)
        {
            uint32_t before = top_scope_mask;
            w = big_emit_const(buf, cap, w, &rng, visible_mask, &top_scope_mask);
            visible_mask |= (top_scope_mask & ~before);
        }
        else if (pick < 7)
        {
            w = big_emit_stmt(buf, cap, w, &rng, visible_mask);
        }
        else
        {
            char hdr[32];
            int tn = (int)(xrand(&rng) % 10000);
            int hl = snprintf(hdr, sizeof hdr, "test \"t%d\" {\n", tn);
            if (w + (size_t)hl >= cap)
            {
                break;
            }
            memcpy(buf + w, hdr, (size_t)hl);
            w += (size_t)hl;
            w = big_emit_block_body(buf, cap, w, &rng, 3, visible_mask);
            if (w + 2 < cap)
            {
                buf[w++] = '}';
                buf[w++] = '\n';
            }
        }
    }
    size_t body_len = w;
    /* tail sentinel: identical shape to g_src's "\"'\n" + NUL pad (>=16
     * readable NULs past body end so every 8-byte SWAR/keyword load stays
     * in-bounds from any body offset, same contract as g_src). Built as a
     * plain byte array (not a string literal) so sizeof is exactly the
     * intended tail length with no hidden implicit terminator. */
    static const char tail[19] = {'"', '\'', '\n', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    size_t tail_len = sizeof(tail);
    if (w + tail_len > cap)
    {
        cap = w + tail_len;
        buf = (char *)realloc(buf, cap);
        if (!buf)
        {
            fprintf(stderr, "fatal: big corpus realloc failed\n");
            exit(1);
        }
    }
    /* GCC's _FORTIFY_SOURCE object-size checker can't prove buf's size
     * across the malloc/realloc reassignment above at -O2 (confirmed via
     * ASan: zero errors/leaks on this exact path) and emits a spurious
     * stringop-overflow warning here; disabled locally rather than
     * silenced for the whole file. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memcpy(buf + w, tail, tail_len);
#pragma GCC diagnostic pop
    *out_buf = buf;
    return body_len;
}

static inline int is_ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline int is_ident_cont(unsigned char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* P4.4 — branchless keyword classification, second revision. First byte
 * selects ONE candidate word/mask/kind through four 256-entry const tables
 * (every keyword has a distinct first byte => bijective, the doc's
 * perfect-match idea without a hash): one xor, one masked compare, one
 * len check, arithmetic select — zero branches, ~half the ops of the
 * previous all-candidates form. The 8-byte load stays in-bounds thanks to
 * the tail sentinel; masking to the keyword's real length ignores pad
 * bytes, and the len guard rejects prefixes like "tests"/"constx". */
static const uint64_t KW_WORD[256] = {
    ['a'] = 0x00747265737361ULL,   /* "assert" */
    ['c'] = 0x000074736E6F63ULL,   /* "const"  */
    ['t'] = 0x0000000074736574ULL, /* "test"   */
    ['w'] = 0x00006574697277ULL,   /* "write"  */
};
static const uint64_t KW_MASK[256] = {
    ['a'] = 0xFFFFFFFFFFFFULL,
    ['c'] = 0xFFFFFFFFFFULL,
    ['t'] = 0xFFFFFFFFULL,
    ['w'] = 0xFFFFFFFFFFULL,
};
static const uint8_t KW_KLEN[256] = {['a'] = 6, ['c'] = 5, ['t'] = 4, ['w'] = 5};
static const uint8_t KW_KIND[256] = {
    ['a'] = TOK_ASSERT,
    ['c'] = TOK_CONST,
    ['t'] = TOK_TEST,
    ['w'] = TOK_WRITE,
};
static inline TokenKind keyword_or_ident(const char *s, unsigned len)
{
    unsigned char c0 = (unsigned char)s[0];
    uint64_t w;
    memcpy(&w, s, 8); /* safe: >=16 tail-pad bytes past body end */
    unsigned hit = (unsigned)(KW_KLEN[c0] == len) & (((w ^ KW_WORD[c0]) & KW_MASK[c0]) == 0u);
    return (TokenKind)(hit * KW_KIND[c0] + (1u - hit) * TOK_IDENTIFIER);
}

/* ---------------------------------------------------------------------------
 * P4.2 — SWAR primitives (u64, portable C, no intrinsics; AVX2 comes later).
 *
 * Techniques per the phase-4 reference document:
 *   swar_eq    : broadcast-xor turns target bytes to 0x00, then the classic
 *                "(x - 0x01..) & ~x & 0x80.." marks 0x80 per surviving zero.
 *   swar_ge    : with inputs pre-masked to <=0x7F per byte, adding
 *                (0x80 - lo) sets the byte's high bit iff byte >= lo; sums
 *                fit in-byte, so no cross-byte carries (the document's
 *                "mask the high bit first" carry guard).
 *   movemask8  : emulate PEXT/movemask by concentrating the eight byte MSBs
 *                into one byte with a single PLAIN multiply — Muła's scalar
 *                PMOVMSKB trick kept inside the low 64 bits of the product
 *                (no widening mulhi: lower latency, exists everywhere).
 *   DEFINE_SPAN: consolidated span walker shape, specialized per class at
 *                compile time (document §3, inlining rationale). Returns
 *                the offset of the first byte OUTSIDE the class; ctz on the
 *                inverted mask replaces the unpredictable per-character
 *                exit branch. Whitespace stays scalar — its spans are too
 *                short for SWAR's fixed prologue to pay for itself.
 * ------------------------------------------------------------------------- */
#define ONES 0x0101010101010101ULL
#define HIGH 0x8080808080808080ULL

/* 0x80 in every byte whose value equals c */
static inline uint64_t swar_eq(uint64_t x, unsigned char c)
{
    uint64_t y = x ^ (ONES * c);
    return (y - ONES) & ~y & HIGH;
}

/* per-byte unsigned >= lo, assuming every byte <= 0x7F */
static inline uint64_t swar_ge(uint64_t x, unsigned char lo)
{
    return (x + (ONES * (unsigned char)(0x80 - lo))) & HIGH;
}

/* per-byte unsigned <= hi, assuming every byte <= 0x7F */
static inline uint64_t swar_le(uint64_t x, unsigned char hi)
{
    /* high bit set iff byte+K stayed below 0x80 iff byte <= hi */
    return ~((x + (ONES * (unsigned char)(0x7F - hi)))) & HIGH;
}

/* 0x80 per byte in [lo, hi]; caller guarantees bytes are ASCII (< 0x80) so
 * the swar_ge/swar_le carry precondition holds. */
static inline uint64_t swar_in(uint64_t x, unsigned char lo, unsigned char hi)
{
    return swar_ge(x, lo) & swar_le(x, hi);
}

/* concentrate per-byte MSBs into the low 8 bits of a u64 — movemask/PEXT
 * emulation via ONE PLAIN MULTIPLY, verified against all 2^8 mask subsets.
 * Modification of Muła's scalar PMOVMSKB trick (0x80.pl, 2014): his form
 * reads the HIGH half of a widening product,
 *     (u128)m * 0x0204081020408100 >> 64,
 * i.e. a mulhi — higher latency, worse throughput than low mul, and missing
 * outright on some ISAs. Shifting his multiplier down one byte moves the
 * same grade-school concentration into the TOP byte of the ordinary 64-bit
 * product, so no widening is ever materialized:
 *
 *   m = a.......b.......c.......d.......e.......f.......g.......h.......
 *   C = 1......1......1......1......1......1......1......1..............
 *   rows m<<0, m<<7, ..., m<<49 stack without collision: column k receives
 *   a bit from exactly one row (r ≡ 7k mod 8 is unique), so no carry ever
 *   disturbs a neighbour and the product's top byte is exactly abcdefgh:
 *
 *       (m * 0x0002040810204081) >> 56 == movemask(m)   (bit j = byte j MSB)
 *
 * Caller guarantees every byte of m is 0x00 or 0x80 — what swar_eq/swar_in/
 * span leave-masks emit; stray low bits carry junk into the top byte.
 * First-match sites below don't call this: ctz on the raw mask is fewer
 * dependent ops than mul+shift. This is the tool when you want ALL match
 * positions as a bitmask. */
static inline unsigned swar_movemask8(uint64_t m)
{
    return (unsigned)((m * 0x0002040810204081ULL) >> 56);
}

/* offset (in bytes) of the lowest byte whose high bit is set */
static inline size_t swar_first_high_byte(uint64_t m)
{
    return (size_t)(__builtin_ctzll(m) >> 3);
}

/* ---- THE span engine (document §3: consolidation / "Table lookups") --
 * ONE hot-loop shape answers every "how many bytes can I jump over?"
 * question — non_newline, identifier, non_unescaped_quote, space, digits —
 * instead of four hand-copied loops. A class is fully described by:
 *   LeaveExpr : SWAR mask with 0x80 under each byte that ENDS the span
 *   ContFn    : same decision for the scalar tail (< 8 bytes left)
 * Each instantiation is static inline so it fuses into scan_tokens and
 * unrolls freely; that inlining measured worth it even when the compiler
 * unrolls 8x (the loop is too hot to afford a call frame or runtime class
 * dispatch). Newline bookkeeping stays in the caller's ws path, never a
 * second pass over the span. */
static inline int digit_cont(unsigned char c)
{
    return c >= '0' && c <= '9';
}
static inline int space_cont(unsigned char c)
{
    return c == ' ' || c == '\t';
}
static inline int non_nl_cont(unsigned char c)
{
    return c != '\n';
}
static inline int non_quote_cont(unsigned char c)
{
    return c != '"';
}

#define DEFINE_SPAN(Name, LeaveExpr, ContFn)                                                       \
    static inline size_t Name(const char *p, size_t limit)                                         \
    {                                                                                              \
        size_t i = 0;                                                                              \
        while (i + 8 <= limit)                                                                     \
        {                                                                                          \
            uint64_t w;                                                                            \
            memcpy(&w, p + i, 8);                                                                  \
            uint64_t leave = (LeaveExpr);                                                          \
            if (leave)                                                                             \
            {                                                                                      \
                return i + swar_first_high_byte(leave);                                            \
            }                                                                                      \
            i += 8;                                                                                \
        }                                                                                          \
        while (i < limit && ContFn((unsigned char)p[i]))                                           \
        {                                                                                          \
            i++;                                                                                   \
        }                                                                                          \
        return i;                                                                                  \
    }

/* run of identifier/keyword characters: longest spans in real source.
 * [A-Za-z] collapses into ONE range check via the ASCII case-fold: for
 * ASCII, w|0x20 maps 'A'..'Z' onto 'a'..'z' while digits/underscore pass
 * through untouched ('0'..'9' and '_' already have bit 5 set; '_' folds to
 * 0x7f which fails the eq below — hence eq on the RAW word). */
DEFINE_SPAN(span_ident,
            (~(swar_eq(w, '_') | swar_in(w, '0', '9') | swar_in(w | (ONES * 0x20), 'a', 'z')) &
             HIGH),
            is_ident_cont)
DEFINE_SPAN(span_digit, ~(swar_in(w, '0', '9')) & HIGH, digit_cont)
/* spaces/tabs only: '\n' is handled singly in scan_tokens so line/col stay
 * O(1) pointer arithmetic instead of a recount pass over the span */
DEFINE_SPAN(span_space, ~(swar_eq(w, ' ') | swar_eq(w, '\t')) & HIGH, space_cont)
/* everything except newline: jump-to-EOL questions (comments, carets).
 * Leave mask = the newline positions themselves (bytes that END the run). */
DEFINE_SPAN(span_non_newline, swar_eq(w, '\n'), non_nl_cont)
/* inverted polarity instance: leave-mask IS the match mask — distance to
 * the next quote rather than run length of a class. Tail predicate stays
 * in the "keep advancing" convention: continue while NOT a quote. */
DEFINE_SPAN(span_to_quote, swar_eq(w, '"'), non_quote_cont)

/* string-literal content length: distance to the first UNESCAPED quote.
 * A candidate terminator is real iff the backslashes immediately before
 * it are even in number; after an escaped quote, resume just past it. */
static inline size_t span_str_end(const char *p, size_t limit)
{
    size_t base = 0;
    for (;;)
    {
        size_t q = base + span_to_quote(p + base, limit - base);
        if (q >= limit)
        {
            return limit;
        }
        size_t bs = 0;
        while (q > base + bs && p[q - 1 - bs] == '\\')
        {
            bs++;
        }
        if (!(bs & 1))
        {
            return q;
        }
        base = q + 1;
    }
}

/* ---- P4.3 — SIMD span paths with runtime dispatch --------------------
 * Same consolidated shape as DEFINE_SPAN above — load W bytes, build the
 * leave mask, ctz on its first set bit — with the u64 word widened to a
 * vector. Dispatch chain: AVX2 (32B) -> SSE2 (16B, baseline x86-64) ->
 * SWAR u64 (P4.2 engine, untouched) -> scalar tail inside the SWAR walker.
 * Intrinsics live ONLY in this section so the SWAR code above stays the
 * readable, portable reference.
 *
 * Safety: vector loops run only while a FULL lane fits inside [p, limit),
 * so loads stay within the caller-declared region — the same contract as
 * the u64 loop, one wider step at a time. Every tail delegates to the next
 * narrower implementation, so the sub-lane remainder logic exists exactly
 * once (in the P4.2 walkers).
 *
 * Range checks use the min/max epu8 identity — min(w,lo)==lo <=> w>=lo,
 * max(w,hi)==hi <=> w<=hi — no carry tricks needed unlike swar_ge/swar_le.
 * Leave masks come out in 0xFF-per-leaf-byte polarity (cmpeq output), so
 * movemask yields one status bit per byte and ctz finds the first leaf.
 *
 * The SSE2 level uses only the SSE2 subset (cmp/min/max/movemask epu8),
 * which is baseline x86-64: no feature check needed or possible. The
 * header's earlier "SSE4.2" mention referred to pcmpestri-style matching;
 * this engine deliberately does not use it (per-class masks compose into
 * the SAME codepath for all five classes).
 *
 * g_simd is fixed once at startup (before threads exist) and only read
 * afterwards, so each _d stub below is a perfectly predicted branch; the
 * SWAR arm still inlines into scan_tokens exactly as P4.2 left it. The
 * target("avx2") attribute both enables the intrinsics and forbids the
 * compiler from inlining AVX2 bodies into generic callers — with -mavx2
 * removed from the build flags, nothing outside this section can emit
 * AVX2, keeping non-AVX2 machines safe under pure runtime dispatch.
 * --------------------------------------------------------------------- */
typedef enum
{
    SIMD_SWAR = 0, /* P4.2 portable u64 engine (default) */
    SIMD_SSE2,     /* 16B lanes */
    SIMD_AVX2      /* 32B lanes */
} SimdLevel;

static SimdLevel g_simd = SIMD_SWAR;

static const char *simd_name(SimdLevel l)
{
    switch (l)
    {
    case SIMD_AVX2:
        return "avx2";
    case SIMD_SSE2:
        return "sse2";
    default:
        return "swar";
    }
}

#define ATTR_AVX2 __attribute__((target("avx2")))
#define ATTR_NONE

#ifdef MODAL_X86
ATTR_AVX2 static inline __m256i loadu256(const char *p)
{
    return _mm256_loadu_si256((const __m256i *)p);
}
static inline __m128i loadu128(const char *p)
{
    return _mm_loadu_si128((const __m128i *)p);
}

/* w >= lo per byte (0xFF where true) */
ATTR_AVX2 static inline __m256i v256_ge(__m256i w, unsigned char lo)
{
    return _mm256_cmpeq_epi8(_mm256_min_epu8(w, _mm256_set1_epi8((char)lo)),
                             _mm256_set1_epi8((char)lo));
}
static inline __m128i v128_ge(__m128i w, unsigned char lo)
{
    return _mm_cmpeq_epi8(_mm_min_epu8(w, _mm_set1_epi8((char)lo)), _mm_set1_epi8((char)lo));
}
/* w <= hi per byte */
ATTR_AVX2 static inline __m256i v256_le(__m256i w, unsigned char hi)
{
    return _mm256_cmpeq_epi8(_mm256_max_epu8(w, _mm256_set1_epi8((char)hi)),
                             _mm256_set1_epi8((char)hi));
}
static inline __m128i v128_le(__m128i w, unsigned char hi)
{
    return _mm_cmpeq_epi8(_mm_max_epu8(w, _mm_set1_epi8((char)hi)), _mm_set1_epi8((char)hi));
}
ATTR_AVX2 static inline __m256i v256_range(__m256i w, unsigned char lo, unsigned char hi)
{
    return _mm256_and_si256(v256_ge(w, lo), v256_le(w, hi));
}
static inline __m128i v128_range(__m128i w, unsigned char lo, unsigned char hi)
{
    return _mm_and_si128(v128_ge(w, lo), v128_le(w, hi));
}
/* negate a continue-mask into a leave-mask */
ATTR_AVX2 static inline __m256i v256_not(__m256i cont)
{
    return _mm256_andnot_si256(cont, _mm256_set1_epi8(-1));
}
static inline __m128i v128_not(__m128i cont)
{
    return _mm_andnot_si128(cont, _mm_set1_epi8(-1));
}

ATTR_AVX2 static inline __m256i v256_leave_ident(__m256i w)
{
    __m256i fold = _mm256_or_si256(w, _mm256_set1_epi8(0x20));
    return v256_not(
        _mm256_or_si256(_mm256_cmpeq_epi8(w, _mm256_set1_epi8('_')),
                        _mm256_or_si256(v256_range(w, '0', '9'), v256_range(fold, 'a', 'z'))));
}
static inline __m128i v128_leave_ident(__m128i w)
{
    __m128i fold = _mm_or_si128(w, _mm_set1_epi8(0x20));
    return v128_not(
        _mm_or_si128(_mm_cmpeq_epi8(w, _mm_set1_epi8('_')),
                     _mm_or_si128(v128_range(w, '0', '9'), v128_range(fold, 'a', 'z'))));
}
ATTR_AVX2 static inline __m256i v256_leave_digit(__m256i w)
{
    return v256_not(v256_range(w, '0', '9'));
}
static inline __m128i v128_leave_digit(__m128i w)
{
    return v128_not(v128_range(w, '0', '9'));
}
ATTR_AVX2 static inline __m256i v256_leave_space(__m256i w)
{
    return v256_not(_mm256_or_si256(_mm256_cmpeq_epi8(w, _mm256_set1_epi8(' ')),
                                    _mm256_cmpeq_epi8(w, _mm256_set1_epi8('\t'))));
}
static inline __m128i v128_leave_space(__m128i w)
{
    return v128_not(_mm_or_si128(_mm_cmpeq_epi8(w, _mm_set1_epi8(' ')),
                                 _mm_cmpeq_epi8(w, _mm_set1_epi8('\t'))));
}
ATTR_AVX2 static inline __m256i v256_eq_nl(__m256i w)
{
    return _mm256_cmpeq_epi8(w, _mm256_set1_epi8('\n'));
}
static inline __m128i v128_eq_nl(__m128i w)
{
    return _mm_cmpeq_epi8(w, _mm_set1_epi8('\n'));
}
ATTR_AVX2 static inline __m256i v256_eq_quote(__m256i w)
{
    return _mm256_cmpeq_epi8(w, _mm256_set1_epi8('"'));
}
static inline __m128i v128_eq_quote(__m128i w)
{
    return _mm_cmpeq_epi8(w, _mm_set1_epi8('"'));
}

/* One vector instance of the span shape: full lanes through the vector,
 * everything narrower delegated to the next-walker-down. MM yields one
 * status bit per byte (bit k = byte k), so ctz lands on the first leaf. */
#define DEFINE_SPAN_VEC(Name, TargetAttr, W, V, LoadFn, Movemask, LeaveFn, SwarFn)                 \
    static TargetAttr size_t Name(const char *p, size_t limit)                                     \
    {                                                                                              \
        size_t i = 0;                                                                              \
        while (i + (W) <= limit)                                                                   \
        {                                                                                          \
            V w = LoadFn(p + i);                                                                   \
            int m = Movemask(LeaveFn(w));                                                          \
            if (m)                                                                                 \
            {                                                                                      \
                return i + (size_t)__builtin_ctz((unsigned)m);                                     \
            }                                                                                      \
            i += (W);                                                                              \
        }                                                                                          \
        return i + SwarFn(p + i, limit - i);                                                       \
    }

DEFINE_SPAN_VEC(span_ident_avx2, ATTR_AVX2, 32, __m256i, loadu256, _mm256_movemask_epi8,
                v256_leave_ident, span_ident)
DEFINE_SPAN_VEC(span_digit_avx2, ATTR_AVX2, 32, __m256i, loadu256, _mm256_movemask_epi8,
                v256_leave_digit, span_digit)
DEFINE_SPAN_VEC(span_space_avx2, ATTR_AVX2, 32, __m256i, loadu256, _mm256_movemask_epi8,
                v256_leave_space, span_space)
DEFINE_SPAN_VEC(span_non_newline_avx2, ATTR_AVX2, 32, __m256i, loadu256, _mm256_movemask_epi8,
                v256_eq_nl, span_non_newline)
DEFINE_SPAN_VEC(span_to_quote_avx2, ATTR_AVX2, 32, __m256i, loadu256, _mm256_movemask_epi8,
                v256_eq_quote, span_to_quote)

DEFINE_SPAN_VEC(span_ident_sse2, ATTR_NONE, 16, __m128i, loadu128, _mm_movemask_epi8,
                v128_leave_ident, span_ident)
DEFINE_SPAN_VEC(span_digit_sse2, ATTR_NONE, 16, __m128i, loadu128, _mm_movemask_epi8,
                v128_leave_digit, span_digit)
DEFINE_SPAN_VEC(span_space_sse2, ATTR_NONE, 16, __m128i, loadu128, _mm_movemask_epi8,
                v128_leave_space, span_space)
DEFINE_SPAN_VEC(span_non_newline_sse2, ATTR_NONE, 16, __m128i, loadu128, _mm_movemask_epi8,
                v128_eq_nl, span_non_newline)
DEFINE_SPAN_VEC(span_to_quote_sse2, ATTR_NONE, 16, __m128i, loadu128, _mm_movemask_epi8,
                v128_eq_quote, span_to_quote)

#endif /* MODAL_X86 */

/* Runtime feature detection, universal: AVX2 via __builtin_cpu_supports
 * (cpu_init once first); SSE2 needs no check — it is baseline x86-64;
 * non-x86 targets stay on the SWAR level. */
static SimdLevel simd_detect(void)
{
#if defined(MODAL_X86) && defined(__GNUC__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2"))
    {
        return SIMD_AVX2;
    }
    return SIMD_SSE2;
#else
    return SIMD_SWAR;
#endif
}

/* Dispatch stubs: scan_tokens and span_str_end call these. Default arm
 * calls the P4.2 walkers directly so they keep inlining on the hot path. */
static inline size_t span_ident_d(const char *p, size_t limit)
{
#ifndef MODAL_X86
    return span_ident(p, limit);
#else
    if (g_simd == SIMD_AVX2)
    {
        return span_ident_avx2(p, limit);
    }
    if (g_simd == SIMD_SSE2)
    {
        return span_ident_sse2(p, limit);
    }
    return span_ident(p, limit);
#endif
}
static inline size_t span_digit_d(const char *p, size_t limit)
{
#ifndef MODAL_X86
    return span_digit(p, limit);
#else
    if (g_simd == SIMD_AVX2)
    {
        return span_digit_avx2(p, limit);
    }
    if (g_simd == SIMD_SSE2)
    {
        return span_digit_sse2(p, limit);
    }
    return span_digit(p, limit);
#endif
}
static inline size_t span_space_d(const char *p, size_t limit)
{
#ifndef MODAL_X86
    return span_space(p, limit);
#else
    if (g_simd == SIMD_AVX2)
    {
        return span_space_avx2(p, limit);
    }
    if (g_simd == SIMD_SSE2)
    {
        return span_space_sse2(p, limit);
    }
    return span_space(p, limit);
#endif
}
static inline size_t span_non_newline_d(const char *p, size_t limit)
{
#ifndef MODAL_X86
    return span_non_newline(p, limit);
#else
    if (g_simd == SIMD_AVX2)
    {
        return span_non_newline_avx2(p, limit);
    }
    if (g_simd == SIMD_SSE2)
    {
        return span_non_newline_sse2(p, limit);
    }
    return span_non_newline(p, limit);
#endif
}
static inline size_t span_to_quote_d(const char *p, size_t limit)
{
#ifndef MODAL_X86
    return span_to_quote(p, limit);
#else
    if (g_simd == SIMD_AVX2)
    {
        return span_to_quote_avx2(p, limit);
    }
    if (g_simd == SIMD_SSE2)
    {
        return span_to_quote_sse2(p, limit);
    }
    return span_to_quote(p, limit);
#endif
}
/* composite walker rides on the dispatched quote distance, so string
 * scanning automatically follows the active level */
static inline size_t span_str_end_d(const char *p, size_t limit)
{
    size_t base = 0;
    for (;;)
    {
        size_t q = base + span_to_quote_d(p + base, limit - base);
        if (q >= limit)
        {
            return limit;
        }
        size_t bs = 0;
        while (q > base + bs && p[q - 1 - bs] == '\\')
        {
            bs++;
        }
        if (!(bs & 1))
        {
            return q;
        }
        base = q + 1;
    }
}

/* ---- SWAR self-test gate ---------------------------------------------
 * Every primitive above is checked against a dumb scalar reference at
 * startup: exhaustively for movemask8 (all 2^8 mask subsets) and the ident
 * predicate rewrite (all 128 ASCII bytes), differentially over the real
 * demo source plus an adversarial synthetic buffer for the span walkers
 * and find_byte (every start offset exercises tail paths and 8-byte word
 * boundaries). A miscompile or future edit dies here instead of silently
 * corrupting measurements. Returns nonzero on first mismatch. */
static int spans_differential_run(void);
#ifdef MODAL_X86
static int spans_selftest_level(SimdLevel lvl);
#endif

static int swar_selftest(void)
{
    SimdLevel simd_saved = g_simd; /* restore caller's runtime choice below */
    /* movemask8: exhaustive over mask subsets, natural-order reference */
    for (uint32_t v = 0; v < 256; v++)
    {
        uint64_t m = 0;
        for (int b = 0; b < 8; b++)
        {
            if ((v >> b) & 1)
            {
                m |= 0x80ULL << (8 * b);
            }
        }
        unsigned want = 0;
        for (int b = 0; b < 8; b++)
        {
            want |= (unsigned)((m >> (8 * b + 7)) & 1) << b;
        }
        if (swar_movemask8(m) != want)
        {
            return 1;
        }
    }

    /* spread bytes: NUL digit brace upper punct caret lower underscore */
    uint64_t probe = 0x5f785e2b417d3800ULL;

    /* swar_eq: every target byte value vs per-byte reference */
    for (int c = 0; c < 128; c++)
    {
        uint64_t want = 0;
        for (int b = 0; b < 8; b++)
        {
            if ((unsigned char)(probe >> (8 * b)) == (unsigned char)c)
            {
                want |= 0x80ULL << (8 * b);
            }
        }
        if (swar_eq(probe, (unsigned char)c) != want)
        {
            return 2;
        }
    }

    /* swar_in: assorted ranges vs reference; also proves the case-folded
     * ident predicate is EXACTLY [A-Za-z0-9_] over the whole ASCII domain */
    for (int c = 0; c < 128; c++)
    {
        uint64_t w = ONES * (unsigned char)c;
        int classic =
            c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        uint64_t folded =
            swar_eq(w, '_') | swar_in(w, '0', '9') | swar_in(w | (ONES * 0x20), 'a', 'z');
        if (!classic != !(folded & 0x80))
        {
            return 3;
        }
        for (int hi = c; hi < 128; hi += 13)
        {
            uint64_t want = 0;
            for (int b = 0; b < 8; b++)
            {
                unsigned char byte = (unsigned char)(probe >> (8 * b));
                if (byte >= (unsigned char)c && byte <= (unsigned char)hi)
                {
                    want |= 0x80ULL << (8 * b);
                }
            }
            if (swar_in(probe, (unsigned char)c, (unsigned char)hi) != want)
            {
                return 4;
            }
        }
    }

    /* blocks 5-9: the span differential runs first on the SWAR level, then
     * AGAIN under each SIMD dispatch level — same corpora, same scalar
     * references, so any lane-math mistake in the vector walkers dies at
     * startup instead of corrupting a benchmark. */
    int sp = spans_differential_run();
    if (sp)
    {
        return sp;
    }
#if defined(MODAL_X86)
    int rc = spans_selftest_level(SIMD_SSE2);
    if (rc)
    {
        g_simd = simd_saved;
        return rc + 100;
    }
    if (simd_detect() == SIMD_AVX2)
    {
        rc = spans_selftest_level(SIMD_AVX2);
        if (rc)
        {
            g_simd = simd_saved;
            return rc + 200;
        }
    }
#endif
    g_simd = simd_saved;
    return 0;
}

/* Span-walker differential (blocks 5-9): two corpora, every start offset,
 * limit always "to end of corpus" exactly as the scanner calls them. The
 * synthetic corpus adds tabs and escaped quotes so space/quote paths leave
 * their demo-source comfort zone. Runs under WHICHEVER dispatch level
 * g_simd currently selects; the want= references are plain scalar loops,
 * so every implementation is judged against ground truth. Returns the
 * failing block number (5..9) or 0. */
static int spans_differential_run(void)
{
    static const char synth[] =
        "const x := 12 ab_9 \"s\\\"q\\\\\" test\tt1 { assert(1+2==3) }\nzz9_";
    const struct
    {
        const char *p;
        size_t n;
    } corpora[2] = {{synth, sizeof(synth) - 1}, {g_src, G_SRC_BODY_LEN}};
#define SPAN_FAIL(blk, g, w)                                                                       \
    do                                                                                             \
    {                                                                                              \
        fprintf(stderr,                                                                            \
                "gate fail: level=%s block=%d corpus=%zu off=%zu lim=%zu got=%zu want=%zu\n",      \
                simd_name(g_simd), (blk), bi, s, lim, (size_t)(g), (size_t)(w));                   \
        return blk;                                                                                \
    } while (0)
    for (size_t bi = 0; bi < 2; bi++)
    {
        const char *p = corpora[bi].p;
        size_t n = corpora[bi].n;
        for (size_t s = 0; s < n; s++)
        {
            size_t lim = n - s;
            size_t got = span_ident_d(p + s, lim), want = 0;
            while (want < lim && is_ident_cont((unsigned char)p[s + want]))
            {
                want++;
            }
            if (got != want)
            {
                SPAN_FAIL(5, got, want);
            }
            got = span_digit_d(p + s, lim);
            want = 0;
            while (want < lim && digit_cont((unsigned char)p[s + want]))
            {
                want++;
            }
            if (got != want)
            {
                SPAN_FAIL(6, got, want);
            }
            got = span_space_d(p + s, lim);
            want = 0;
            while (want < lim && space_cont((unsigned char)p[s + want]))
            {
                want++;
            }
            if (got != want)
            {
                SPAN_FAIL(7, got, want);
            }
            got = span_non_newline_d(p + s, lim);
            want = 0;
            while (want < lim && p[s + want] != '\n')
            {
                want++;
            }
            if (got != want)
            {
                SPAN_FAIL(8, got, want);
            }
            /* non-unescaped-quote distance vs parity-counting reference */
            got = span_str_end_d(p + s, lim);
            want = 0;
            for (;;)
            {
                while (want < lim && p[s + want] != '"')
                {
                    want++;
                }
                if (want >= lim)
                {
                    break;
                }
                size_t bs = 0;
                while (bs < want && p[s + want - 1 - bs] == '\\')
                {
                    bs++;
                }
                if (!(bs & 1))
                {
                    break;
                }
                want++;
            }
            if (got != want)
            {
                SPAN_FAIL(9, got, want);
            }
        }
    }
    return 0;
}

/* Same differential under one explicit dispatch level. Returns the raw
 * block code (5..9) or 0; callers add 100 per level so logs name the
 * culprit implementation: 105..109 sse2 | 205..209 avx2. */
#ifdef MODAL_X86
static int spans_selftest_level(SimdLevel lvl)
{
    g_simd = lvl;
    return spans_differential_run();
}
#endif

/* ---------------------------------------------------------------------------
 * P4.4/P4.5 dispatch tables: byte-class LUT + operator precedence LUT.
 *
 * The old scanner cascaded 4-5 compares per non-whitespace byte (ident?
 * digit? quote? ':'? then a 10-case switch). One 256-entry table answers
 * "what is this byte" with a single load; single-byte token bytes map
 * DIRECTLY to their TokenKind so the default case emits without another
 * switch. The remaining switch runs over ~6 dense class ids — a jump table
 * the predictor nails once the source repeats. Operator precedence also
 * becomes a load instead of a switch at push time (branch removal).
 * ------------------------------------------------------------------------- */
enum
{
    CLS_WS = TOK__COUNT, /* space/tab/newline          */
    CLS_IDENT,           /* [A-Za-z_]                  */
    CLS_DIGIT,           /* [0-9]                      */
    CLS_STR,             /* '"'                        */
    CLS_COLON,           /* ':' (':=' or error)        */
    CLS_BAD = 255
};
static uint8_t g_cls[256];
static int8_t g_op_prec[256]; /* op char -> precedence (0 = not an operator) */

static void class_tables_init(void)
{
    memset(g_cls, CLS_BAD, sizeof g_cls);
    memset(g_op_prec, 0, sizeof g_op_prec);
    g_cls[(unsigned char)' '] = CLS_WS;
    g_cls[(unsigned char)'\t'] = CLS_WS;
    g_cls[(unsigned char)'\n'] = CLS_WS;
    for (int c = 'a'; c <= 'z'; c++)
    {
        g_cls[c] = CLS_IDENT;
    }
    for (int c = 'A'; c <= 'Z'; c++)
    {
        g_cls[c] = CLS_IDENT;
    }
    g_cls[(unsigned char)'_'] = CLS_IDENT;
    for (int c = '0'; c <= '9'; c++)
    {
        g_cls[c] = CLS_DIGIT;
    }
    g_cls[(unsigned char)'"'] = CLS_STR;
    g_cls[(unsigned char)':'] = CLS_COLON;
    /* single-byte tokens carry their TokenKind directly */
    const char *ops = "+-*/<>";
    for (const char *q = ops; *q; q++)
    {
        g_cls[(unsigned char)*q] = TOK_OPERATOR;
    }
    g_op_prec[(unsigned char)'*'] = 3;
    g_op_prec[(unsigned char)'/'] = 3;
    g_op_prec[(unsigned char)'+'] = 2;
    g_op_prec[(unsigned char)'-'] = 2;
    g_op_prec[(unsigned char)'<'] = 1;
    g_op_prec[(unsigned char)'>'] = 1;
    g_cls[(unsigned char)'{'] = TOK_LBRACE;
    g_cls[(unsigned char)'}'] = TOK_RBRACE;
    g_cls[(unsigned char)'('] = TOK_LPAREN;
    g_cls[(unsigned char)')'] = TOK_RPAREN;
}

static size_t scan_tokens16(const char *src, size_t body_len, Tok16 *out, int *err)
{
    esc_n = 0; /* fresh escape list per scan (TLS) */
    const char *p = src;
    const char *end = src + body_len; /* first sentinel byte */
    size_t n = 0;
    /* Newline bookkeeping survives ONLY to print accurate scan errors —
     * tokens themselves stopped carrying line/col in P4.5. */
    int cur_line = 0;
    const char *line_start = src;

#define EMIT(k, tp, ln)                                                                            \
    do                                                                                             \
    {                                                                                              \
        size_t l_ = (ln);                                                                          \
        out[n].kind = (uint8_t)(k);                                                                \
        out[n].off = (uint32_t)((tp) - src);                                                       \
        out[n].sym = -1;                                                                           \
        out[n].num = 0;                                                                            \
        if (__builtin_expect(!tok16_set_len(&out[n], n, l_), 0))                                   \
        {                                                                                          \
            fprintf(stderr, "scan error: too many long tokens\n");                                 \
            *err = 1;                                                                              \
            goto done;                                                                             \
        }                                                                                          \
        n++;                                                                                       \
    } while (0)

    while (p < end)
    {
        /* One class lookup drives BOTH whitespace skipping and token
         * dispatch — the previous shape made every token start pay a
         * 3-compare (' '/'\t'/'\n') preamble before consulting g_cls;
         * ablation showed the per-token skeleton, not spans or stores,
         * owns the cost, so the entry path is now: load g_cls[c], one
         * compare against CLS_WS, jump table. Whitespace bodies keep the
         * consolidated span for runs >= 2 (indentation); lone spaces — the
         * demo's dominant gap — stay scalar; '\n' is consumed singly so
         * cur_line/line_start update is exact pointer math. */
        uint8_t cls = g_cls[(unsigned char)*p];
        if (cls == CLS_WS)
        {
            if (*p == '\n')
            {
                cur_line++;
                line_start = p + 1;
                p++;
            }
            else if (p + 1 < end && space_cont((unsigned char)p[1]))
            {
                p += span_space_d(p, (size_t)(end - p));
            }
            else
            {
                p++; /* lone space/tab */
            }
            continue;
        }

        switch (cls)
        {
        case CLS_IDENT:
        {
            size_t len = span_ident_d(p, (size_t)(end - p));
            TokenKind k = keyword_or_ident(p, (unsigned)len);
            EMIT(k, p, len);
            if (k == TOK_IDENTIFIER)
            {
                out[n - 1].sym = intern(p, (unsigned char)len);
            }
            p += len;
            continue;
        }
        case CLS_DIGIT:
        {
            size_t nd = span_digit_d(p, (size_t)(end - p));
            long long v = 0;
            for (size_t j = 0; j < nd; j++)
            {
                v = v * 10 + (p[j] - '0');
            }
            EMIT(TOK_NUMBER, p, nd);
            out[n - 1].num = v;
            p += nd;
            continue;
        }
        case CLS_STR:
        {
            size_t avail = (size_t)(end - p);
            size_t qoff = span_str_end_d(p + 1, avail - 1);
            if (qoff == avail - 1)
            {
                fprintf(stderr, "scan error [%d:%d]: unterminated string\n", cur_line,
                        (int)(p - line_start) + 1 + (int)qoff);
                *err = 1;
                return n;
            }
            size_t slen = qoff + 2; /* both quotes included */
            EMIT(TOK_STRING, p, slen);
            p += slen;
            continue;
        }
        case CLS_COLON:
            if (p + 1 < end && p[1] == '=')
            {
                EMIT(TOK_DEFINE, p, 2);
                p += 2;
                continue;
            }
            fprintf(stderr, "scan error [%d:%d]: lone ':'\n", cur_line, (int)(p - line_start) + 1);
            *err = 1;
            return n;
        default:
            /* class value < TOK__COUNT means "single-byte token of this kind" */
            if (cls < TOK__COUNT)
            {
                EMIT((TokenKind)cls, p, 1);
                p++;
                continue;
            }
            fprintf(stderr, "scan error [%d:%d]: unexpected byte '%c'\n", cur_line,
                    (int)(p - line_start) + 1, *p);
            *err = 1;
            return n;
        }
    }
done:;
#undef EMIT

    out[n].kind = TOK_EOF;
    out[n].off = (uint32_t)(p - src);
    out[n].len = 0;
    out[n].sym = -1;
    out[n].num = 0;
    return n + 1;
}

/* ---------------------------------------------------------------------------
 * Reference scanner (gate side). Deliberately DUMB: plain character loops,
 * an explicit keyword table walk, no SWAR/SIMD, no class LUT, no memo.
 * Independence is the point — this is the oracle the optimized scanner is
 * diffed against at every dispatch level. It additionally RECORDS the
 * line/col of every token (counted directly), which the gate uses to prove
 * the P4.5 derive-on-error positions match scan-time bookkeeping.
 * ------------------------------------------------------------------------- */
static size_t ref_scan16(const char *src, size_t body_len, Tok16 *out, int *rline, int *rcol,
                         int *err)
{
    esc_n = 0; /* fresh escape list per scan (TLS) */
    const char *p = src;
    const char *end = src + body_len;
    size_t n = 0;
    int line = 0; /* leading '\n' sentinel bumps this to 1 before any token */
    const char *ls = src;

#define RPOS(i)                                                                                    \
    do                                                                                             \
    {                                                                                              \
        rline[n] = line;                                                                           \
        rcol[n] = (int)(i - ls) + 1;                                                               \
    } while (0)

    while (p < end)
    {
        if (*p == ' ' || *p == '\t')
        {
            p++;
            continue;
        }
        if (*p == '\n')
        {
            line++;
            ls = p + 1;
            p++;
            continue;
        }
        if (*p >= '0' && *p <= '9')
        {
            const char *st = p;
            RPOS(st);
            while (p < end && digit_cont((unsigned char)*p))
            {
                p++;
            }
            long long v = 0;
            for (const char *q = st; q < p; q++)
            {
                v = v * 10 + (*q - '0');
            }
            out[n].kind = TOK_NUMBER;
            out[n].off = (uint32_t)(st - src);
            out[n].num = v;
            out[n].sym = -1;
            if (!tok16_set_len(&out[n], n, (size_t)(p - st)))
            {
                fprintf(stderr, "ref scan error: too many long tokens\n");
                *err = 1;
                return n;
            }
            n++;
            continue;
        }
        if (is_ident_start((unsigned char)*p))
        {
            const char *st = p;
            RPOS(st);
            while (p < end && is_ident_cont((unsigned char)*p))
            {
                p++;
            }
            size_t len = (size_t)(p - st);
            static const struct
            {
                const char *s;
                TokenKind k;
            } KWS[] = {
                {"assert", TOK_ASSERT},
                {"const", TOK_CONST},
                {"test", TOK_TEST},
                {"write", TOK_WRITE},
            };
            TokenKind k = TOK_IDENTIFIER;
            for (size_t i = 0; i < sizeof(KWS) / sizeof(KWS[0]); i++)
            {
                if (strlen(KWS[i].s) == len && memcmp(KWS[i].s, st, len) == 0)
                {
                    k = KWS[i].k;
                    break;
                }
            }
            out[n].kind = (uint8_t)k;
            out[n].off = (uint32_t)(st - src);
            out[n].num = 0; /* D0 fix: scan_tokens16's EMIT always zeroes num;
                             * ref_scan16 left it uninitialized here, invisible
                             * on the toy corpus's zero-initialized static
                             * buffer but real garbage on heap-allocated
                             * buffers (caught by the D0 big-corpus gate). */
            /* same u8 truncation as the optimized scanner keeps sym ids
             * identical for pathological >=255B identifiers */
            out[n].sym = (k == TOK_IDENTIFIER) ? (int16_t)intern(st, (unsigned char)len) : -1;
            if (!tok16_set_len(&out[n], n, len))
            {
                fprintf(stderr, "ref scan error: too many long tokens\n");
                *err = 1;
                return n;
            }
            n++;
            continue;
        }
        if (*p == '"')
        {
            const char *st = p;
            RPOS(st);
            const char *q = st + 1;
            while (q < end && *q != '"')
            {
                q++;
            }
            if (q >= end)
            {
                fprintf(stderr, "ref scan error: unterminated string\n");
                *err = 1;
                return n;
            }
            p = q + 1;
            out[n].kind = TOK_STRING;
            out[n].off = (uint32_t)(st - src);
            out[n].sym = -1;
            out[n].num = 0;
            if (!tok16_set_len(&out[n], n, (size_t)(p - st)))
            {
                fprintf(stderr, "ref scan error: too many long tokens\n");
                *err = 1;
                return n;
            }
            n++;
            continue;
        }
        if (*p == ':')
        {
            RPOS(p);
            if (p + 1 < end && p[1] == '=')
            {
                out[n].kind = TOK_DEFINE;
                out[n].off = (uint32_t)(p - src);
                out[n].sym = -1;
                out[n].num = 0;
                out[n].len = 2;
                n++;
                p += 2;
                continue;
            }
            fprintf(stderr, "ref scan error: lone ':'\n");
            *err = 1;
            return n;
        }
        {
            uint8_t cls = g_cls[(unsigned char)*p];
            if (cls < TOK__COUNT)
            {
                RPOS(p);
                out[n].kind = cls;
                out[n].off = (uint32_t)(p - src);
                out[n].sym = -1;
                out[n].num = 0;
                out[n].len = 1;
                n++;
                p++;
                continue;
            }
        }
        fprintf(stderr, "ref scan error: unexpected byte '%c'\n", *p);
        *err = 1;
        return n;
    }
#undef RPOS

    rline[n] = line;
    rcol[n] = (int)(end - ls) + 1;
    out[n].kind = TOK_EOF;
    out[n].off = (uint32_t)(end - src);
    out[n].len = 0;
    out[n].sym = -1;
    out[n].num = 0;
    return n + 1;
}

/* Fast line/col lookup for gate_check16's O(n)-token loop. tok_line_col
 * itself is intentionally a cold-path O(off) scan (brief: line/col derived
 * on error only, at most once per production run) and stays untouched —
 * every OTHER caller (error_at, rsl_declare, dispatch_mapped) still uses
 * it directly, unmodified. But gate_check16 calls it TWICE PER TOKEN,
 * unconditionally, for every token in the stream: fine at 23 tokens (the
 * 77B demo), O(tokens * source_len) at D0 scale (~220k tokens over ~1MB
 * measured ~2.3e11 byte-compares — hangs in practice, not just "slow").
 * This is a scaling gap in the GATE's implementation, not a relaxation of
 * what it checks: same inputs, same line/col values, same comparison,
 * same failure report. Build the newline-offset table once (O(n)) and
 * binary-search it per lookup (O(log n)), giving the full gate O(n log n)
 * instead of O(n^2) while verifying bit-for-bit the same thing. */
typedef struct
{
    const uint32_t *nl_off; /* offsets of '\n' bytes, ascending */
    size_t n_nl;
} LineIndex;

static LineIndex line_index_build(const char *src, size_t len, uint32_t *scratch)
{
    size_t k = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (src[i] == '\n')
        {
            scratch[k++] = (uint32_t)i;
        }
    }
    LineIndex idx = {.nl_off = scratch, .n_nl = k};
    return idx;
}

/* Same semantics as tok_line_col: line = count of '\n' bytes in src[0,off),
 * col = 1 + distance since the last '\n' (or since byte 0 if none). Binary
 * search for the first newline offset >= off; that count of newlines
 * strictly before `off` is exactly what tok_line_col's linear scan counts. */
static inline void line_index_lookup(const LineIndex *idx, uint32_t off, int *line, int *col)
{
    size_t lo = 0, hi = idx->n_nl;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if (idx->nl_off[mid] < off)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    /* lo = number of newlines with offset < off = tok_line_col's `ln` */
    int ln = (int)lo;
    uint32_t ls = (lo == 0) ? 0 : idx->nl_off[lo - 1] + 1;
    *line = ln;
    *col = (int)(off - ls) + 1;
}

/* Equivalence gate, P4.5 form: the optimized scanner must reproduce the
 * reference stream field-for-field — including byte offsets, decoded
 * lengths (escape list walked), nums, syms AND the derived-on-error
 * line/col, which must equal the reference scanner's directly-counted
 * positions. Runs under every dispatch level before workers spawn. */
static int gate_check16(const char *src, size_t src_len, const Tok16 *ref, size_t nr,
                        const uint32_t *rlens, const int *rline, const int *rcol, const Tok16 *got,
                        size_t ng)
{
    if (nr != ng)
    {
        fprintf(stderr, "GATE FAIL: token count %zu != %zu\n", nr, ng);
        return 0;
    }
    /* one-time O(n) index build, shared by every token's line/col lookup
     * below instead of re-scanning from byte 0 each time (see header). */
    uint32_t *scratch = (uint32_t *)malloc((src_len + 1) * sizeof(uint32_t));
    if (!scratch)
    {
        fprintf(stderr, "GATE FAIL: line-index alloc failed (src_len=%zu)\n", src_len);
        return 0;
    }
    LineIndex lidx = line_index_build(src, src_len, scratch);
    int ok = 1;
    for (size_t i = 0; i < nr; i++)
    {
        const Tok16 *x = &ref[i], *y = &got[i];
        int xl, xc, yl_, yc;
        line_index_lookup(&lidx, x->off, &xl, &xc);
        line_index_lookup(&lidx, y->off, &yl_, &yc);
        if (x->kind != y->kind || rlens[i] != tok_len(got, i) ||
            memcmp(src + x->off, src + y->off, rlens[i]) != 0 || x->num != y->num ||
            x->sym != y->sym || xl != rline[i] || xc != rcol[i])
        {
            fprintf(stderr,
                    "GATE FAIL at tok %zu:\n"
                    "  ref : kind=%u off=%u len=%u '%.*s' [%d:%d] num=%lld sym=%d\n"
                    "  scan: kind=%u off=%u len=%zu '%.*s' [%d:%d] num=%lld sym=%d\n",
                    i, x->kind, x->off, rlens[i], (int)rlens[i], src + x->off, rline[i], rcol[i],
                    (long long)x->num, x->sym, y->kind, y->off, tok_len(got, i),
                    (int)tok_len(got, i), src + y->off, yl_, yc, (long long)y->num, y->sym);
            ok = 0;
            break;
        }
    }
    free(scratch);
    return ok;
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
    LFPool *pool;
    const char *src; /* mapped source bytes (P4.5 zero-copy base) */
    size_t src_len;  /* logical body length; resolved[] spans it */
    uint8_t verbose;
} Cx;

static __thread Cx *t_cx = NULL;

typedef struct
{
    const Tok16 *current;
    const Tok16 *previous;
    const Tok16 *base;
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
/* P4.5: line/col are derived on demand from the token's byte offset —
 * this is the ONLY consumer on the hot path's horizon, and it runs at
 * most once per run (first error wins). */
static void error_at(Parser *p, const Tok16 *t, const char *msg)
{
    if (p->had_error)
    {
        return;
    }
    p->had_error = 1;
    int ln, cl;
    tok_line_col(t_cx->src, t->off, &ln, &cl);
    printf("erro [%d:%d]: %s\n", ln, cl, msg);
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
    const char *name; /* points INTO the mapped source (zero-copy) */
    size_t len;
    uint32_t decl_off; /* name token's byte offset (was a stream index) */
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
    Cx *cx;
    Scope *cur;
    _mvec_objref resolved;
    uint8_t had_error;
} Rsl;

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
static void rsl_declare(Rsl *r, const Tok16 *ident, ObjKind kind, long long val)
{
    /* P4.5: resolved[] is indexed by BYTE OFFSET now (ident->off), which is
     * also the zero-copy name pointer's base. */
    size_t idx = ident->off;
    if (r->resolved.data[idx] != NULL)
    {
        fprintf(stderr, "internal: '%.*s' already declared or resolved\n", (int)ident->len,
                r->cx->src + ident->off);
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
        .name = r->cx->src + ident->off,
        /* escape valve: >=255B names resolve via strlen (mapped source is
         * NUL-padded past its body) */
        .len = ident->len != TOK16_LEN_ESC ? ident->len : strlen(r->cx->src + ident->off),
        .decl_off = ident->off,
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
            int nl, nc, pl, pc;
            tok_line_col(r->cx->src, ident->off, &nl, &nc);
            tok_line_col(r->cx->src, alt->decl_off, &pl, &pc);
            printf("erro [%d:%d]: %.*s redeclarado neste bloco\n"
                   "\tdeclaração anterior em [%d:%d]\n",
                   nl, nc, (int)o->len, o->name, pl, pc);
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
    int8_t prec; /* LUT-loaded at push: no switch on the compare path */
    const Tok16 *tok;
} OpTok;
_Static_assert(sizeof(OpTok) <= CHUNK_SZ, "OpTok deve caber em 1 chunk");
MVEC_IMPL(optok, OpTok)

static int prec_of(const Tok16 *t)
{
    if (t->kind != TOK_OPERATOR)
    {
        return -1;
    }
    return g_op_prec[(unsigned char)t_cx->src[t->off]];
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
        .tok_idx = op.tok->off,
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
            .tok_idx = p->previous->off,
            .d.num = p->previous->num,
        };
        return mvec_push_nodeptr(values, n);
    }
    if (match(p, TOK_IDENTIFIER))
    {
        const Tok16 *tok = p->previous;
        Obj *o = scope_lookup(r->cur, tok->sym);
        if (!o)
        {
            error_at(p, tok, "undefined identifier");
            return 0;
        }
        r->resolved.data[tok->off] = o;
        Node *n = (Node *)pool.alloc_sz(t_cx->pool, sizeof(Node));
        if (!n)
        {
            return 0;
        }
        *n = (Node){
            .kind = ND_IDENT,
            .tok_idx = tok->off,
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
        while (ops.size > 0 && ops.data[ops.size - 1].prec >= prec_of(p->current))
        {
            lr_reduce(&values, &ops);
        }
        if (!mvec_push_optok(&ops,
                             (OpTok){.op = t_cx->src[p->current->off],
                                     .prec = g_op_prec[(unsigned char)t_cx->src[p->current->off]],
                                     .tok = p->current}))
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
        const Tok16 *tok = p->previous;
        Obj *o = scope_lookup(r->cur, tok->sym);
        if (!o)
        {
            error_at(p, tok, "undefined identifier");
            return 0;
        }
        r->resolved.data[tok->off] = o;
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
        while (ops.size > 0 && ops.data[ops.size - 1].prec >= prec_of(p->current))
        {
            lr_reduce(&values, &ops);
        }
        if (!mvec_push_optok(&ops,
                             (OpTok){.op = t_cx->src[p->current->off],
                                     .prec = g_op_prec[(unsigned char)t_cx->src[p->current->off]],
                                     .tok = p->current}))
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
    const Tok16 *name = p->current;
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
        printf("      declarado: const %.*s\n", (int)name->len, t_cx->src + name->off);
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
            int vl, vc;
            tok_line_col(t_cx->src, p->current->off, &vl, &vc);
            printf("  [%d:%d] %-8s => %s\n", vl, vc, kind_name(p->current->kind), prod->name);
        }
        return prod->fn(p, r);
    }
    int vl, vc;
    tok_line_col(t_cx->src, p->current->off, &vl, &vc);
    printf("erro [%d:%d]: inesperado %s; produções válidas:", vl, vc, kind_name(p->current->kind));
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

static int run_pipeline_cx(Cx *cx, const Tok16 *stream, size_t ntokens)
{
    (void)ntokens; /* resolved[] spans the source now, not the token count */
    Parser p = {.current = stream, .previous = NULL, .base = stream, .had_error = 0};
    t_cx = cx;
    Rsl r = {
        .cx = cx,
        .had_error = 0,
        .cur = NULL,
    };
    /* P4.5: resolved is indexed by BYTE OFFSET (tok->off), so it spans the
     * source, not the token count. Still one bulk NULL-fill, no pushes. */
    r.resolved = mvec_init_objref(cx->pool, cx->src_len + 2);
    if (!r.resolved.data)
    {
        return 1;
    }
    memset(r.resolved.data, 0, (cx->src_len + 2) * sizeof(ObjRef));
    r.resolved.size = cx->src_len + 2;

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
 * run over the 77B toy corpus needs ~4096 chunks, so each stream keeps its
 * own 4096-chunk slab and resets it per run. Footprint = threads *
 * nstreams * 256 KiB.
 *
 * D0: 4096 chunks is sized for the toy corpus's ~23 tokens; the resolver/
 * parser's pool traffic (Obj per const decl, Scope per block, OpTok/value-
 * stack vectors per expression, all via pool.alloc_sz) scales with token
 * count, so a ~230k-token corpus exhausts a 4096-chunk pool immediately
 * ("pool ran out of memory" — a real capacity bug, not a grammar one).
 * pool_capacity_for() gives a generous per-token budget (empirically each
 * token touches at most a handful of CHUNK_SZ=64B chunks across Obj/
 * OpTok/mvec growth) plus the fixed 4096 floor so toy-corpus behavior is
 * byte-for-byte unchanged when ntokens is small.
 * --------------------------------------------------------------------- */

static inline size_t pool_capacity_for(size_t ntokens)
{
    size_t budget = ntokens * 8 + 4096; /* 8 chunks/token headroom + toy-corpus floor */
    return budget < 4096 ? 4096 : budget;
}

#define POOL_CAPACITY 4096

typedef struct
{
    LFPool *pool;
    Cx cx;
    Tok16 *toks; /* P4.5: persistent, 64B-aligned; reused in place */
} Stream;

/* D10 — false-sharing hygiene: the batch cursor is the ONE field every
 * worker writes every batch (atomic fetch_add). Before this fix it was a
 * bare 8-byte atomic embedded mid-struct in GlobalCfg, sharing its cache
 * line with nstreams/target_util_pm/lex/lex_src/lex_src_len — fields every
 * worker also READS every round. Each fetch_add invalidated that whole
 * line for every other thread reading those neighbors, forcing a coherence
 * round-trip on data that never changes after setup. Padded to a full 64B
 * line (verified by the _Static_assert below, previously disabled because
 * the struct had zero padding) and hoisted to its own separately-allocated
 * cache line via CACHELINE_ALIGN so it can't straddle a line boundary
 * with ANYTHING, hot or cold. */
#define CACHELINE_SZ 64
#if defined(__GNUC__) || defined(__clang__)
#define CACHELINE_ALIGN __attribute__((aligned(CACHELINE_SZ)))
#else
#define CACHELINE_ALIGN
#endif

typedef struct CACHELINE_ALIGN
{
    atomic_uint_fast64_t v;
    char _pad[CACHELINE_SZ - sizeof(atomic_uint_fast64_t)];
} CacheLineU64;
_Static_assert(sizeof(CacheLineU64) >= CACHELINE_SZ, "cursor must own its cache line");
_Static_assert(_Alignof(CacheLineU64) >= CACHELINE_SZ,
               "cursor must be cache-line ALIGNED, not just sized, or it can still straddle "
               "a line boundary depending on struct placement");

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
    const Tok16 *tokens;
    size_t ntokens;
    int nstreams;
    int target_util_pm;  /* per-worker CPU budget in permille (0.1% units);
                          * 1000 = unpaced; e.g. 3 = 0.3% */
    CacheLineU64 cursor; /* shared; the ONLY hot shared line */
    /* P4.1: when lex is set, workers scan lex_src into a fresh token array
     * each run instead of replaying `tokens`. "nolex" on the command line
     * restores phase-3 semantics exactly (parity mode for A/B). */
    int lex;
    /* D0: which source the lex path scans. Defaults to g_src_map/
     * G_SRC_BODY_LEN (the 77B toy corpus, unchanged behavior); argv[9]
     * "big" points these at the D0 >=1MB realistic corpus instead, so the
     * SAME harness (same pinning, same batching, same pacing) produces a
     * real per-stream GB/s number instead of the fixed-cost-dominated toy
     * one (brief §6: "toy corpus can't show GB/s ... validate on a
     * >=1MB realistic corpus FIRST"). */
    const char *lex_src;
    size_t lex_src_len;
} GlobalCfg;
/* D10: verify cursor actually landed on its own cache line inside the
 * enclosing struct — the aligned attribute on CacheLineU64 forces this,
 * but asserting it directly (rather than trusting the attribute silently)
 * catches any future struct-layout regression at compile time instead of
 * a runtime perf regression nobody notices for a while. */
_Static_assert(offsetof(GlobalCfg, cursor) % CACHELINE_SZ == 0,
               "GlobalCfg.cursor is not cache-line aligned; false-sharing D10 fix regressed");

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

/* D10 — false-sharing hygiene: g_stats[K] is one slot per worker, written
 * ONLY by that worker (st = &g_stats[t->slot] in bench_worker) every
 * single batch (busy_ns/batches_done/rounds_done all update in the hot
 * loop). Unpadded, adjacent workers' slots can share a 64B line — worker
 * N's write then invalidates worker N+1's cached copy of ITS OWN slot,
 * forcing a coherence fetch on every batch for data nobody else is
 * reading until pthread_join. Padded to a full cache line per slot; the
 * array itself is allocated via harness_pool's 64B-aligned alloc_sz (see
 * main()), so slot 0 starts cache-line aligned and every subsequent slot
 * (each exactly CACHELINE_SZ bytes) stays aligned too. */
typedef struct CACHELINE_ALIGN
{
    uint64_t batches_done;
    uint64_t rounds_done;
    uint64_t busy_ns;
    int ok;
    char _pad[CACHELINE_SZ - (3 * sizeof(uint64_t) + sizeof(int))];
} WorkerStats;
_Static_assert(sizeof(WorkerStats) == CACHELINE_SZ,
               "WorkerStats must be exactly one cache line so g_stats[] slots never straddle "
               "or share a line across workers");

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
            if (g->lex)
            {
                /* P4.5 path: lex the source bytes into the stream's
                 * persistent, cache-aligned token buffer. Reused in place —
                 * scan overwrites slots [0, nt) and the parser reads exactly
                 * nt tokens, so stale data beyond nt is never observed and
                 * there is ZERO per-run token allocation (zero-copy
                 * steady state: source mmap'd once, tokens preallocated). */
                int serr = 0;
                size_t nt = scan_tokens16(g->lex_src, g->lex_src_len, streams[s].toks, &serr);
                *had_err |= serr;
                *had_err |= run_pipeline_cx(&streams[s].cx, streams[s].toks, nt);
            }
            else
            {
                *had_err |= run_pipeline_cx(&streams[s].cx, g->tokens, g->ntokens);
            }
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
    /* P4.5 cache-conscious setup: each stream's token buffer is one 64B-
     * aligned block, allocated ONCE per worker and reused every run. The
     * upper bound (body_len+1 slots) can never grow, so no growth checks
     * and no allocator traffic inside the timed region.
     * D0: sized off g->lex_src_len (the active corpus, toy or big) rather
     * than the fixed G_SRC_BODY_LEN, so the big-corpus lex path never
     * overflows the token buffer — same worst-case-slot-per-byte bound as
     * the toy corpus, just scaled up. */
    size_t toks_sz = ((g->lex_src_len + 2) * sizeof(Tok16) + 63) & ~(size_t)63;
    for (int s = 0; s < g->nstreams; s++)
    {
        /* D0: pool sized off g->ntokens (the actual reference token count
         * for the active corpus), not the fixed POOL_CAPACITY — see
         * pool_capacity_for's header comment. Toy-corpus runs get exactly
         * the old 4096-chunk floor since pool_capacity_for clamps small
         * ntokens up to it; only large corpora scale the allocation. */
        if (!pool.allocator(&streams[s].pool, pool_capacity_for(g->ntokens)))
        {
            fprintf(stderr, "fatal: stream pool init failed (core %d, stream %d)\n", t->core_id, s);
            st->ok = 0;
            return NULL;
        }
        streams[s].toks = aligned_alloc(64, toks_sz);
        if (!streams[s].toks)
        {
            fprintf(stderr, "fatal: token buffer alloc failed (core %d)\n", t->core_id);
            st->ok = 0;
            return NULL;
        }
        streams[s].cx = (Cx){
            .pool = streams[s].pool,
            .src = g->lex_src,
            .src_len = g->lex_src_len,
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
        free(streams[s].toks);
        pool.drop(streams[s].pool);
    }
    return NULL;
}

#ifdef SCAN_MICRO
/* Isolated scanner cost probe: ns per scan_tokens16 call over g_src, plus
 * the reference scanner for contrast. Excludes parser/pool/harness noise —
 * use it to attribute pipeline deltas to the scan stage.
 *
 * D0 extension: the toy 77B corpus is fixed-cost-dominated (brief §4:
 * hollow-EMIT floor ~198ns/call regardless of technique), so its GB/s
 * number is not meaningful on its own — brief §6 D0 says validate on a
 * >=1MB realistic corpus FIRST. This probe now also times scan_tokens16
 * over the D0 big corpus and reports GB/s directly (bytes/ns == GB/s
 * numerically), so per-stream throughput claims have a real basis before
 * D1-D9 are judged against them. */
static double gbps_of(size_t bytes, uint64_t ns)
{
    return ns ? (double)bytes / (double)ns : 0.0; /* bytes/ns == GB/s */
}

int main(void)
{
    class_tables_init();
    static Tok16 ref[G_SRC_BODY_LEN + 2];
    static int rl[G_SRC_BODY_LEN + 2], rc[G_SRC_BODY_LEN + 2];
    int e = 0;
    (void)ref_scan16(g_src, G_SRC_BODY_LEN, ref, rl, rc, &e); /* warm intern */

    Tok16 *buf = aligned_alloc(64, ((G_SRC_BODY_LEN + 2) * sizeof(Tok16) + 63) & ~(size_t)63);
#ifndef MICRO_N
#define MICRO_N 50000000ull
#endif
    const uint64_t N = MICRO_N;
    uint64_t t0 = now_ns();
    size_t sink = 0;
    for (uint64_t i = 0; i < N; i++)
    {
        sink += scan_tokens16(g_src, G_SRC_BODY_LEN, buf, &e);
    }
    uint64_t dt = now_ns() - t0;
    printf("[77B]  scan_tokens16: %llu ns/call, %.3f GB/s (%llu tok total, sink=%zu)\n",
           (unsigned long long)(dt / N), gbps_of(G_SRC_BODY_LEN, dt / N), (unsigned long long)(dt),
           sink % 1000);

    t0 = now_ns();
    for (uint64_t i = 0; i < N; i++)
    {
        sink += ref_scan16(g_src, G_SRC_BODY_LEN, buf, rl, rc, &e);
    }
    dt = now_ns() - t0;
    printf("[77B]  ref_scan16   : %llu ns/call, %.3f GB/s (sink=%zu)\n",
           (unsigned long long)(dt / N), gbps_of(G_SRC_BODY_LEN, dt / N), sink % 1000);
    free(buf);

    /* D0 big corpus: same measurement shape, >=1MB realistic source. Fewer
     * iterations since each call now does ~50000x more work per call. */
    {
        char *raw = NULL;
        size_t blen = big_corpus_generate(&raw);
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t raw_bytes = blen + 19;
        size_t map_sz = (raw_bytes + page - 1) & ~(page - 1);
        char *m = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
        {
            fprintf(stderr, "fatal: D0 probe mmap failed\n");
            return 1;
        }
        memcpy(m, raw, raw_bytes);
        free(raw);

        static int brl_s, brc_s; /* silence unused warnings if oracle skipped */
        (void)brl_s;
        (void)brc_s;
        int *brl = malloc((blen + 2) * sizeof(int));
        int *brc = malloc((blen + 2) * sizeof(int));
        Tok16 *bref = aligned_alloc(64, ((blen + 2) * sizeof(Tok16) + 63) & ~(size_t)63);
        Tok16 *bbuf = aligned_alloc(64, ((blen + 2) * sizeof(Tok16) + 63) & ~(size_t)63);
        if (!brl || !brc || !bref || !bbuf)
        {
            fprintf(stderr, "fatal: D0 probe alloc failed\n");
            return 1;
        }
        int be = 0;
        (void)ref_scan16(m, blen, bref, brl, brc, &be); /* warm intern, oracle reference */

#ifndef MICRO_N_BIG
#define MICRO_N_BIG 2000ull
#endif
        const uint64_t NB = MICRO_N_BIG;
        size_t bsink = 0;
        t0 = now_ns();
        for (uint64_t i = 0; i < NB; i++)
        {
            bsink += scan_tokens16(m, blen, bbuf, &be);
        }
        dt = now_ns() - t0;
        printf("[D0 %.2fMB] scan_tokens16: %llu ns/call, %.3f GB/s (sink=%zu)\n",
               (double)blen / 1e6, (unsigned long long)(dt / NB), gbps_of(blen, dt / NB),
               bsink % 1000);

        t0 = now_ns();
        for (uint64_t i = 0; i < NB; i++)
        {
            bsink += ref_scan16(m, blen, bbuf, brl, brc, &be);
        }
        dt = now_ns() - t0;
        printf("[D0 %.2fMB] ref_scan16   : %llu ns/call, %.3f GB/s (sink=%zu)\n",
               (double)blen / 1e6, (unsigned long long)(dt / NB), gbps_of(blen, dt / NB),
               bsink % 1000);

        free(brl);
        free(brc);
        free(bref);
        free(bbuf);
    }
    return 0;
}
#else
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
    /* Optional argv[7]: "nolex" replays the prepared token array like
     * phase 3 (parity mode); default lexes raw bytes every run. */
    int lex = !(argc > 7 && strcmp(argv[7], "nolex") == 0);

    /* D0: optional argv[9] "big" points the lex path (and nolex's Cx
     * binding) at the D0 realistic corpus instead of the 77B toy corpus,
     * so the SAME harness produces a real per-stream GB/s number (brief
     * §6 D0). Anything else, or omitted, keeps prior behavior exactly. */
    int use_big_corpus = (argc > 9 && strcmp(argv[9], "big") == 0);

    /* P4.3: optional argv[8] forces the span dispatch level ("avx2"|"sse2"
     * |"swar") so A/B attribution never depends on which machine ran the
     * benchmark; without it the best detected level runs. Forcing a level
     * the CPU lacks would execute instructions it cannot decode, so the
     * request is validated against detection before anything executes. */
    SimdLevel simd_level = simd_detect();
    if (argc > 8)
    {
        SimdLevel want;
        if (strcmp(argv[8], "avx2") == 0)
        {
            want = SIMD_AVX2;
        }
        else if (strcmp(argv[8], "sse2") == 0)
        {
            want = SIMD_SSE2;
        }
        else if (strcmp(argv[8], "swar") == 0)
        {
            want = SIMD_SWAR;
        }
        else
        {
            fprintf(stderr, "fatal: unknown simd level '%s' (avx2|sse2|swar)\n", argv[8]);
            return 1;
        }
        if (want > simd_level)
        {
            fprintf(stderr, "fatal: %s requested but not detected on this cpu\n", argv[8]);
            return 1;
        }
        simd_level = want;
    }
    g_simd = simd_level;
    if (util_in < 0.1)
    {
        util_in = 0.1;
    }
    if (util_in > 100.0)
    {
        util_in = 100.0;
    }
    int util_pct = (int)(util_in * 10.0 + 0.5); /* permille */

    /* P4.5 zero-copy source holder: ONE anonymous mapping, source copied in
     * once at setup. Steady state reads bytes in place — no per-run copy or
     * allocation anywhere (token texts are offsets into this mapping). */
    {
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t map_sz = (sizeof g_src + page - 1) & ~(page - 1);
        char *m = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
        {
            fprintf(stderr, "fatal: source mmap failed\n");
            return 1;
        }
        memcpy(m, g_src, sizeof g_src);
        g_src_map = m;
    }

    /* P4.4 dispatch tables must exist before ANY scanner runs. */
    class_tables_init();

    /* P4.4 keyword gate: branchless SWAR classifier must agree with an
     * explicit reference walk over keywords, prefixes, and mutations.
     * NOTE (pre-existing, orthogonal to D0): keyword_or_ident's contract
     * requires >=8 readable bytes from s (it always issues an 8-byte
     * memcpy), which scanner-sourced tokens satisfy via the source
     * buffer's tail pad but bare string literals like "x" (2 bytes incl.
     * NUL) do not. Padding these test strings to >=8 bytes preserves
     * every case's semantics (kind and strlen()-derived length are
     * unchanged) while keeping the 8-byte read in-bounds under ASan/
     * hardened allocators; the gate's oracle comparison is untouched. */
    {
        static const struct
        {
            const char *s;
            TokenKind k;
        } cases[] = {
            {"assert\0\0", TOK_ASSERT},          {"const\0\0\0", TOK_CONST},
            {"test\0\0\0\0", TOK_TEST},          {"write\0\0\0", TOK_WRITE},
            {"x\0\0\0\0\0\0\0", TOK_IDENTIFIER}, {"constant", TOK_IDENTIFIER},
            {"tests\0\0\0", TOK_IDENTIFIER},     {"asser\0\0", TOK_IDENTIFIER},
            {"Const\0\0\0", TOK_IDENTIFIER},     {"writ\0\0\0\0", TOK_IDENTIFIER},
            {"_\0\0\0\0\0\0\0", TOK_IDENTIFIER}, {"a1\0\0\0\0\0\0", TOK_IDENTIFIER},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            TokenKind got = keyword_or_ident(cases[i].s, (unsigned)strlen(cases[i].s));
            if (got != cases[i].k)
            {
                fprintf(stderr, "fatal: keyword gate FAILED on '%s' (%d != %d)\n", cases[i].s, got,
                        cases[i].k);
                return 1;
            }
        }
        printf("gate: keyword swar == reference\n");
    }

    /* Prepared streams come from the REFERENCE scanner now (it doubles as
     * the nolex-replay stream and the gate oracle). It records line/col
     * truth per token, which the P4.5 gate checks against derive-on-error. */
    static Tok16 prep_happy[G_SRC_BODY_LEN + 2];
    static int pline[G_SRC_BODY_LEN + 2], pcol[G_SRC_BODY_LEN + 2];
    static uint32_t plens[G_SRC_BODY_LEN + 2];
    int gerr = 0;
    size_t n_happy = ref_scan16(g_src_map, G_SRC_BODY_LEN, prep_happy, pline, pcol, &gerr);
    if (gerr)
    {
        fprintf(stderr, "fatal: reference scan failed\n");
        return 1;
    }
    for (size_t i = 0; i < n_happy; i++)
    {
        plens[i] = (uint32_t)tok_len(prep_happy, i);
    }

    /* P4.2 self-test gate: SWAR primitives must equal their scalar
     * references before anything built on top of them is trusted. */
    int st = swar_selftest();
    if (st)
    {
        fprintf(stderr, "fatal: SWAR self-test FAILED (block %d)\n", st);
        return 1;
    }
    printf("gate: spans == scalar references (swar+sse2%s)\n",
           simd_detect() == SIMD_AVX2 ? "+avx2" : "");
    printf("simd: dispatch = %s\n", simd_name(g_simd));

    /* Equivalence gate under EVERY dispatch level <= detected: the optimized
     * scanner must match the reference field-for-field (kind/off/len-decoded/
     * num/sym + derived line/col vs recorded positions) before workers spawn. */
    {
        Tok16 *gate_buf =
            aligned_alloc(64, ((G_SRC_BODY_LEN + 2) * sizeof(Tok16) + 63) & ~(size_t)63);
        if (!gate_buf)
        {
            fprintf(stderr, "fatal: gate alloc failed\n");
            return 1;
        }
        for (int lvl = SIMD_SWAR; lvl <= (int)simd_detect(); lvl++)
        {
            SimdLevel old = g_simd;
            g_simd = (SimdLevel)lvl;
            size_t ng = scan_tokens16(g_src_map, G_SRC_BODY_LEN, gate_buf, &gerr);
            int ok = !gerr && gate_check16(g_src_map, G_SRC_BODY_LEN, prep_happy, n_happy, plens,
                                           pline, pcol, gate_buf, ng);
            printf("gate: scan == ref [%s] (%zu tokens)%s\n", simd_name((SimdLevel)lvl), ng,
                   ok ? "" : "  <-- FAIL");
            g_simd = old;
            if (!ok)
            {
                fprintf(stderr, "fatal: scanner equivalence gate FAILED at %s\n",
                        simd_name((SimdLevel)lvl));
                return 1;
            }
        }
        free(gate_buf);
    }

    /* P4.5 escape-valve gate: tokens >=255B must round-trip through the
     * 1-byte length escape identically in BOTH scanners. */
    {
        char big[768] = {'\n'};
        char *q = big + 1;
        memcpy(q, "const ", 6);
        q += 6;
        memset(q, 'a', 300);
        q += 300;
        memcpy(q, " := 1\nwrite \"", 13);
        q += 13;
        memset(q, 'b', 280);
        q += 280;
        memcpy(q, "\"\n", 2);
        q += 2;
        memset(q, 0, 32); /* tail pad: >=8 readable past any position */
        size_t blen = (size_t)(q - big);
        static Tok16 rbuf[768], gbuf[768];
        static int rl[768], rc[768];
        static uint32_t elens[768];
        int eerr = 0;
        size_t nr = ref_scan16(big, blen, rbuf, rl, rc, &eerr);
        for (size_t i = 0; i < nr; i++)
        {
            elens[i] = (uint32_t)tok_len(rbuf, i);
        }
        size_t ngot = scan_tokens16(big, blen, gbuf, &eerr);
        if (eerr || !gate_check16(big, blen, rbuf, nr, elens, rl, rc, gbuf, ngot))
        {
            fprintf(stderr, "fatal: escape-length gate FAILED\n");
            return 1;
        }
        int found = 0;
        for (size_t i = 0; i < nr; i++)
        {
            if (elens[i] > 254 || tok_len(gbuf, i) > 254)
            {
                found = 1;
            }
        }
        if (!found)
        {
            fprintf(stderr, "fatal: escape gate did not exercise >254B tokens\n");
            return 1;
        }
        printf("gate: len-escape (>254B idents/strings) == ref\n");
    }

    /* D0 — realistic-scale corpus: generate, mmap, and gate BEFORE any
     * conclusion from the 77B corpus is trusted at GB/s scale (brief §6:
     * "toy corpus can't show GB/s ... re-baseline everything there before
     * judging techniques below"). Same discipline as the 77B gates above:
     * ref_scan16 is the oracle, gate_check16 diffs the optimized scanner
     * against it at every SIMD level, failure is fatal. */
    {
        char *raw = NULL;
        g_big_len = big_corpus_generate(&raw);
        if (g_big_len < BIG_CORPUS_MIN_BYTES)
        {
            fprintf(stderr, "fatal: big corpus generation under 1MB (%zu bytes)\n", g_big_len);
            return 1;
        }

        /* mmap'd copy, same rationale as g_src_map: zero-copy steady state,
         * one copy at setup, workers only ever read this mapping. */
        size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t raw_bytes = g_big_len + 19; /* body + tail sentinel (19B, see big_corpus_generate) */
        size_t map_sz = (raw_bytes + page - 1) & ~(page - 1);
        char *m = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED)
        {
            fprintf(stderr, "fatal: big corpus mmap failed\n");
            return 1;
        }
        memcpy(m, raw, raw_bytes);
        free(raw);
        g_big_map = m;
        g_big = m; /* g_big kept for symmetry with g_src (mutable heap view) */

        size_t cap = g_big_len + 2;
        Tok16 *bref = aligned_alloc(64, (cap * sizeof(Tok16) + 63) & ~(size_t)63);
        int *brl = malloc(cap * sizeof(int));
        int *brc = malloc(cap * sizeof(int));
        uint32_t *brlens = malloc(cap * sizeof(uint32_t));
        Tok16 *bgot = aligned_alloc(64, (cap * sizeof(Tok16) + 63) & ~(size_t)63);
        if (!bref || !brl || !brc || !brlens || !bgot)
        {
            fprintf(stderr, "fatal: big corpus gate alloc failed\n");
            return 1;
        }
        int berr = 0;
        size_t bn = ref_scan16(g_big_map, g_big_len, bref, brl, brc, &berr);
        if (berr)
        {
            fprintf(stderr, "fatal: big corpus reference scan failed\n");
            return 1;
        }
        for (size_t i = 0; i < bn; i++)
        {
            brlens[i] = (uint32_t)tok_len(bref, i);
        }
        for (int lvl = SIMD_SWAR; lvl <= (int)simd_detect(); lvl++)
        {
            SimdLevel old = g_simd;
            g_simd = (SimdLevel)lvl;
            berr = 0;
            size_t bng = scan_tokens16(g_big_map, g_big_len, bgot, &berr);
            int bok =
                !berr && gate_check16(g_big_map, g_big_len, bref, bn, brlens, brl, brc, bgot, bng);
            printf("gate: D0 big-corpus scan == ref [%s] (%zu bytes, %zu tokens)%s\n",
                   simd_name((SimdLevel)lvl), g_big_len, bng, bok ? "" : "  <-- FAIL");
            g_simd = old;
            if (!bok)
            {
                fprintf(stderr, "fatal: D0 big-corpus equivalence gate FAILED at %s\n",
                        simd_name((SimdLevel)lvl));
                return 1;
            }
        }
        free(brl);
        free(brc);
        free(brlens);
        free(bgot);
        /* keep bref/bn alive for nolex-replay against the D0 corpus
         * (argv[9]="big" with argv[7]="nolex"); not freed here. */
        g_big_prep = bref;
        g_big_n_happy = bn;
    }

    /* Single "warm-up" pool for the correctness demo runs below (happy path
     * + intentional redeclaration failure), unrelated to the pinned bench
     * workers. */
    LFPool *demo_pool;
    if (!pool.allocator(&demo_pool, POOL_CAPACITY))
    {
        fprintf(stderr, "fatal: pool init failed\n");
        return 1;
    }
    Cx demo_cx = {.pool = demo_pool, .src = g_src_map, .src_len = G_SRC_BODY_LEN, .verbose = 0};
    t_cx = &demo_cx;

    uint64_t t0 = now_ns();
    build_dispatch_maps();
    // selfcheck("decl", T_DECL, T_DECL_N);
    // selfcheck("block", T_BLOCK, T_BLOCK_N);
    printf("selfcheck: %llu ns\n\n", (unsigned long long)(now_ns() - t0));

    printf("== pipeline (fluxo feliz) ==\n");
    t0 = now_ns();
    int err = run_pipeline_cx(&demo_cx, prep_happy, n_happy);

    /* Redeclaration demo: its own tiny source, scanned by the REFERENCE
     * scanner (positions derived on error are exercised here). */
    static const char redecl_src[] = "\nconst y := 1\nconst y := 2\n";
    static Tok16 redecl[sizeof(redecl_src) + 2];
    static int rline2[sizeof(redecl_src) + 2], rcol2[sizeof(redecl_src) + 2];
    int rerr = 0;
    size_t n_redecl = ref_scan16(redecl_src, sizeof(redecl_src) - 1, redecl, rline2, rcol2, &rerr);
    Cx redecl_cx = {
        .pool = demo_pool, .src = redecl_src, .src_len = sizeof(redecl_src) - 1, .verbose = 0};
    printf("\npipeline: %llu ns → %s\n\n", (unsigned long long)(now_ns() - t0),
           err ? "FALHOU" : "aceito");
    printf("== pipeline (redeclaração proposital) ==\n");
    t_cx = &redecl_cx;
    err = run_pipeline_cx(&redecl_cx, redecl, n_redecl);
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
        .tokens = use_big_corpus ? g_big_prep : prep_happy,
        .ntokens = use_big_corpus ? g_big_n_happy : n_happy,
        .nstreams = nstreams,
        .target_util_pm = util_pct,
        .cursor = {.v = 0},
        .lex = lex,
        .lex_src = use_big_corpus ? g_big_map : g_src_map,
        .lex_src_len = use_big_corpus ? g_big_len : G_SRC_BODY_LEN,
    };
    if (use_big_corpus)
    {
        printf("note: harness targeting D0 corpus (%zu bytes, %zu tokens) instead of 77B toy "
               "corpus\n\n",
               g_big_len, g_big_n_happy);
        /* One-shot diagnostic pipeline run before spawning workers: if the
         * generated corpus doesn't actually parse/resolve/fold cleanly,
         * fail loudly HERE with the real error_at/rsl_declare message
         * (both print via printf on the failure path) instead of letting
         * every worker silently accumulate ERROR status with no visible
         * cause. */
        LFPool *diag_pool;
        if (!pool.allocator(&diag_pool, pool_capacity_for(g_big_n_happy)))
        {
            fprintf(stderr, "fatal: D0 diagnostic pool init failed\n");
            return 1;
        }
        Cx diag_cx = {.pool = diag_pool, .src = g_big_map, .src_len = g_big_len, .verbose = 0};
        t_cx = &diag_cx;
        int diag_err = run_pipeline_cx(&diag_cx, g_big_prep, g_big_n_happy);
        pool.drop(diag_pool);
        if (diag_err)
        {
            fprintf(stderr, "fatal: D0 corpus failed full-pipeline validation (see error above)\n");
            return 1;
        }
        printf("gate: D0 big-corpus full pipeline (scan+parse+resolve+fold) == aceito\n\n");
    }

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
    /* D0: per-stream GB/s across the FULL pipeline (scan+parse+resolve+
     * fold), not just isolated scan (that's what the SCAN_MICRO probe
     * reports). Only meaningful when lex is on (each run re-scans
     * lex_src_len fresh bytes; in nolex-replay mode no bytes are
     * processed per run, so this is skipped — brief §6: GB/s is a
     * per-stream, single-core throughput question, not a batching one).
     * sum_busy is aggregate busy-ns across all K threads; dividing by K
     * gives the average single-core busy-ns share, matching "per-stream,
     * single-core efficiency, NOT more threads" from brief §1. */
    if (g.lex && total_runs && K > 0)
    {
        double avg_core_busy_ns = (double)sum_busy / (double)K;
        double bytes_per_run = (double)g.lex_src_len;
        double runs_per_core = (double)total_runs / (double)K;
        double ns_per_run_per_core = runs_per_core ? avg_core_busy_ns / runs_per_core : 0.0;
        double gbps_per_stream = ns_per_run_per_core > 0.0 ? bytes_per_run / ns_per_run_per_core
                                                           : 0.0; /* bytes/ns == GB/s */
        printf("throughput: %.3f GB/s/stream (full pipeline: scan+parse+resolve+fold, %zu "
               "bytes/run, %s corpus)\n",
               gbps_per_stream, g.lex_src_len, use_big_corpus ? "D0" : "77B toy");
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
#endif /* SCAN_MICRO */
