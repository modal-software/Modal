/* ll1_parsing_4.c — PHASE 4: extreme-throughput front end (SWAR + SIMD).
 *
 * Starts as a byte-for-byte functional copy of ll1_parsing_3.c (phase 3:
 * fold-at-parse, interning, O(1) dispatch tables, adaptive pacing — see the
 * phase-3 header at the bottom of this comment block). Every technique below
 * lands behind its own step so A/B attribution stays clean; the baseline
 * binary is /tmp/opencode/tp3_phase3_final.
 *
 * The document's techniques, mapped onto this codebase. The big structural
 * change: phases 1-3 measured a parser over PRE-MATERIALIZED token arrays.
 * Phase 4 adds the missing stage they all assumed — a real scanner that
 * turns bytes into tokens — because SWAR/SIMD only pay off against raw text:
 *
 *   P4.1  Scanner front end (scalar reference):
 *         - tokenize a byte buffer into the existing Token stream
 *         - sentinels per the document: leading '\n', trailing "\"'\\n"
 *           (+NUL) so hot loops skip bounds checks and can always peek -1/+2
 *         - upper-bound token allocation (tokens <= src_len), shrink after
 *         - equivalence gate: same TokenKind sequence as prepare_tokens()
 *           on identical input, else abort
 *
 *   P4.2  SWAR primitives (u64, portable C, no intrinsics):
 *         - broadcast-xor +0x7F trick to find target bytes in a word,
 *           highest-bit-of-each-byte pre-masked to stop cross-byte carry
 *         - movemask emulation via the multiply trick (concentrate byte MSBs)
 *         - count-trailing-ones on an inverted bitstring replaces the
 *           unpredictable per-char while loop for spans:
 *           identifiers, whitespace runs, quote/comment terminators
 *         - one consolidated span-matcher codepath (table-driven class
 *           lookup), not four hand-copied loops — doc §3 "Table lookups"
 *
 *   P4.3  SIMD paths with runtime dispatch [DONE]:
 *         - machine has AVX2 + SSE4.2 (verified via /proc/cpuinfo); dispatch:
 *           AVX2 (32B) -> SSE2 (16B, universal x86-64) -> SWAR u64 -> scalar
 *         - GNU __builtin_cpu_supports for detection; intrinsics isolated in
 *          one small section so the SWAR path stays the readable reference
 *         - inline the span loop even when unrolled (doc §3, it's the hot loop)
 *         - LANDED: same consolidated shape widened to __m256i/__m128i
 *           (leave-mask -> movemask -> ctz); tails delegate down the chain,
 *           so sub-lane remainder logic exists exactly once (in the SWAR
 *           walkers). Detection at startup; argv[8]=avx2|sse2|swar forces a
 *           level for A/B attribution (validated against detection).
 *           -mavx2 removed from the build flags: only the target("avx2")
 *           -tagged section can emit AVX2, keeping any x86-64 safe. The
 *           startup gate re-runs the offset-by-offset span differential
 *           under EVERY level against scalar references (+100/level codes).
 *         - MEASURED (dyn, unpaced, lexed, 4x interleaved triples, N=400k):
 *           swar 125 | sse2 125 | avx2 124 ns/op — parity. On token-dense
 *           source the span work is noise vs dispatch+emit+intern, exactly
 *           as the block-proto predicted; vectors pay only on long
 *           homogeneous runs. The chain stays: correct everywhere, faster
 *           nowhere-yet, and the infrastructure is what P4.5 builds on.
 *
 *   MEASURED FINDINGS (prototype /tmp/opencode/block_proto.c, gated
 *   byte-identical at every suffix offset incl. line/col/sym/num):
 *   - Block classification (classify-once-per-window, spans answered by
 *     ctz on cached masks; SWAR u64 AND AVX2 32B variants) LOSES to the
 *     span engine here: 0.76x on the 77B demo, 0.67x on a 9.7KB buffer
 *     (engine 28.8us @0.34GB/s vs block-swar 42.6us). Per-token cost is
 *     dispatch+emit+intern, not character classification; block walking
 *     adds mask-index bookkeeping per token and only saves a 1-2 cyc/byte
 *     tail loop over 3.5-byte average tokens. simdjson-style blocking pays
 *     when classification IS the semantics or tokens are long homogeneous
 *     runs — neither holds for token-dense source.
 *   - AVX2 classify is not the bottleneck either: producing masks for
 *     bytes never consumed per token costs more than the u64 register
 *     math it replaces (store->reload round-trip).
 *   - intern last-pointer memo: no effect on this corpus (identifiers
 *     never repeat consecutively); kept for repetitive-source cases.
 *   Conclusion: the consolidated span engine IS the local optimum for
 *   token-dense input; further scanner speedups must attack per-token
 *   semantics (dispatch/intern/Token stores = P4.5 territory), not bytes.
 *
 *   P4.4  Branchless keyword/operator matching [LANDED]:
 *         - every keyword has a DISTINCT first byte, so classification is
 *           four masked 8-byte compares combined arithmetically — zero
 *           data-dependent branches (keyword gate diffs it against an
 *           explicit reference walk incl. prefixes/mutations)
 *         - byte-class LUT replaces the compare cascade per non-ws byte;
 *           single-byte tokens map straight to their TokenKind; operator
 *           precedence became a LUT load stored on the OpTok stack
 *
 *   P4.5  Token storage compression + zero-copy source [LANDED]:
 *         - Token 48B -> 16B (num i64 | off u32 | sym i16 | kind u8 |
 *           len u8): 4 tokens/cache line; scanner writes ~3x less memory
 *         - line/col derivable-on-error from off (cold); text pointers
 *           replaced by byte offsets into the mapped source (zero-copy)
 *         - len escape valve for >=255B tokens (TLS overflow list), gated
 *           with a synthetic >300-char corpus through BOTH scanners
 *         - source lives in ONE anonymous mmap, copied once at setup;
 *           steady state reads bytes in place — no per-run allocation
 *         - per-stream token buffers are 64B-aligned, allocated ONCE,
 *           reused in place (scan overwrites [0,nt), parser reads nt)
 *         - gates: ref-vs-opt differential under EVERY simd level,
 *           comparing kind/off/decoded-len/num/sym AND derived line/col
 *           against directly-counted reference positions
 *         - MEASURED (dyn unpaced, 8 interleaved A/B pairs, N=400k):
 *           p43 137 ns/op | p45 134 ns/op medians — parity-to-slightly-
 *           better. Isolated scan_tokens16: ~210 ns/call (~35 cyc/token
 *           of dispatch+intern+keyword+store fixed cost x 24 tokens);
 *           nolex replay: ~44 ns/op aggregate. The front end is now
 *           scanner-bound BY CONSTRUCTION: parse+resolve+fold is ~30%
 *           of a lexed run. Next real lever is fusion (scan+parse in one
 *           pass, no materialized stream) or accepting that a 77B corpus
 *           is fixed-cost-dominated: ~44M lines/s aggregate on 4 threads.
 *
 *   P4.5a Ablation attribution (interleaved 7 reps, SCAN_MICRO builds,
 *         /tmp/opencode/v1..v5.bin) — where the ~205 ns/call scanner
 *         floor actually lives:
 *         - v2 no-intern       ~188 ns/call → intern totals ~19 ns/scan
 *           (~6 calls x ~3 ns; consistent with gprof 2.17 ns/call)
 *         - v3 no-keyword-SWAR ~251 ns/call → keyword SWAR SAVES ~44 ns/
 *           scan; reverting it would be the worst single regression
 *         - v4 hollow-EMIT     ~198 ns/call → Tok16 stores cost ~9 ns;
 *           memory traffic is NOT the bottleneck
 *         - v5 no-ws-lookahead ~207 ns/call → post-ws span lookahead is
 *           neutral at this token density
 *         => even with stores hollowed out the scan costs ~198 ns: the
 *            floor IS the per-token serial control chain (load byte ->
 *            class -> dispatch -> span walk -> advance), a loop-carried
 *            dependence on p that stores/SIMD changes cannot hide.
 *
 *   P4.5b Entry-path restructure [KEPT AT PARITY]: objdump of the probe
 *         showed every token start paying a 3-compare whitespace preamble
 *         (' '/'\t'/'\n') before reaching the g_cls switch; the loop now
 *         does ONE g_cls load driving both ws skipping and token dispatch.
 *         Isolated scan: 206 vs 209 ns/call — parity (the compares were
 *         perfectly predicted; a LUT load is not cheaper than a taken-
 *         almost-always branch). Kept anyway: one dispatch shape instead
 *         of two (doc §3 consolidation); pipeline interleaved A/B vs p43
 *         baseline medians 163 vs 169 ns/op — no regression. Lesson
 *         recorded: predicted branches are nearly free, but so were the
 *         bytes they tested — entry-path micro-shape is not where time goes.
 *
 *   P4.6  Register-pressure discipline (doc §5):
 *         - pointers not pointer+index in scan loops
 *         - bitstrings written straight into the cursor buffer (sliding
 *           window trick) where applicable
 *
 * Throughput accounting: once P4.1 lands, "pipeline" = lex+parse+resolve+
 * fold of the demo program from raw bytes; ns/op then covers the whole front
 * end and phase-over-phase deltas stay honest.
 *
 * ---------------------------------------------------------------------------
 * phase-3 history (verbatim from ll1_parsing_3.c):
 *
 * ll1_parsing_3.c — dynamic work distribution benchmark.
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
 *   ./test_parser4 [dyn|static] [threads] [batch_rounds] [nstreams] [N] [util_pct] [nolex] [simd]
 * Defaults: dyn, ncpu, 4096, NSTREAMS (compile-time, default 8, max 8),
 *           RUNS_DEFAULT, 100 (= unpaced; e.g. 25 caps each worker at ~25%).
 *           Workers lex the mapped source every run unless argv[7] ==
 *           "nolex" (replays the reference-scanned Tok16 stream, parity
 *           mode). argv[8] forces a simd level for A/B attribution.
 *           Build with -DSCAN_MICRO for an isolated scanner-cost probe.
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

/* Equivalence gate, P4.5 form: the optimized scanner must reproduce the
 * reference stream field-for-field — including byte offsets, decoded
 * lengths (escape list walked), nums, syms AND the derived-on-error
 * line/col, which must equal the reference scanner's directly-counted
 * positions. Runs under every dispatch level before workers spawn. */
static int gate_check16(const char *src, const Tok16 *ref, size_t nr, const uint32_t *rlens,
                        const int *rline, const int *rcol, const Tok16 *got, size_t ng)
{
    if (nr != ng)
    {
        fprintf(stderr, "GATE FAIL: token count %zu != %zu\n", nr, ng);
        return 0;
    }
    for (size_t i = 0; i < nr; i++)
    {
        const Tok16 *x = &ref[i], *y = &got[i];
        int xl, xc, yl_, yc;
        tok_line_col(src, x->off, &xl, &xc);
        tok_line_col(src, y->off, &yl_, &yc);
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
            return 0;
        }
    }
    return 1;
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
 * run needs ~4096 chunks, so each stream keeps its own 4096-chunk slab and
 * resets it per run. Footprint = threads * nstreams * 256 KiB.
 * --------------------------------------------------------------------- */

#define POOL_CAPACITY 4096

typedef struct
{
    LFPool *pool;
    Cx cx;
    Tok16 *toks; /* P4.5: persistent, 64B-aligned; reused in place */
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
    const Tok16 *tokens;
    size_t ntokens;
    int nstreams;
    int target_util_pm;  /* per-worker CPU budget in permille (0.1% units);
                          * 1000 = unpaced; e.g. 3 = 0.3% */
    CacheLineU64 cursor; /* shared; the ONLY hot shared line */
    /* P4.1: when lex is set, workers scan g_src into a fresh token array
     * each run instead of replaying `tokens`. "nolex" on the command line
     * restores phase-3 semantics exactly (parity mode for A/B). */
    int lex;
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
            if (g->lex)
            {
                /* P4.5 path: lex the source bytes into the stream's
                 * persistent, cache-aligned token buffer. Reused in place —
                 * scan overwrites slots [0, nt) and the parser reads exactly
                 * nt tokens, so stale data beyond nt is never observed and
                 * there is ZERO per-run token allocation (zero-copy
                 * steady state: source mmap'd once, tokens preallocated). */
                int serr = 0;
                size_t nt = scan_tokens16(g_src_map, G_SRC_BODY_LEN, streams[s].toks, &serr);
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
     * and no allocator traffic inside the timed region. */
    size_t toks_sz = ((G_SRC_BODY_LEN + 2) * sizeof(Tok16) + 63) & ~(size_t)63;
    for (int s = 0; s < g->nstreams; s++)
    {
        if (!pool.allocator(&streams[s].pool, POOL_CAPACITY))
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
            .src = g_src_map,
            .src_len = G_SRC_BODY_LEN,
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
 * use it to attribute pipeline deltas to the scan stage. */
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
    printf("scan_tokens16: %llu ns/call (%llu tok total, sink=%zu)\n", (unsigned long long)(dt / N),
           (unsigned long long)(dt), sink % 1000);

    t0 = now_ns();
    for (uint64_t i = 0; i < N; i++)
    {
        sink += ref_scan16(g_src, G_SRC_BODY_LEN, buf, rl, rc, &e);
    }
    dt = now_ns() - t0;
    printf("ref_scan16   : %llu ns/call (sink=%zu)\n", (unsigned long long)(dt / N), sink % 1000);
    free(buf);
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
     * explicit reference walk over keywords, prefixes, and mutations. */
    {
        static const struct
        {
            const char *s;
            TokenKind k;
        } cases[] = {
            {"assert", TOK_ASSERT},    {"const", TOK_CONST},      {"test", TOK_TEST},
            {"write", TOK_WRITE},      {"x", TOK_IDENTIFIER},     {"constant", TOK_IDENTIFIER},
            {"tests", TOK_IDENTIFIER}, {"asser", TOK_IDENTIFIER}, {"Const", TOK_IDENTIFIER},
            {"writ", TOK_IDENTIFIER},  {"_", TOK_IDENTIFIER},     {"a1", TOK_IDENTIFIER},
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
            int ok = !gerr &&
                     gate_check16(g_src_map, prep_happy, n_happy, plens, pline, pcol, gate_buf, ng);
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
        if (eerr || !gate_check16(big, rbuf, nr, elens, rl, rc, gbuf, ngot))
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
        .tokens = prep_happy,
        .ntokens = n_happy,
        .nstreams = nstreams,
        .target_util_pm = util_pct,
        .cursor = {.v = 0},
        .lex = lex,
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
#endif /* SCAN_MICRO */
