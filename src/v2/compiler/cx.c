#define _XOPEN_SOURCE 700
// MAP_ANONYMOUS / MAP_NORESERVE are outside plain XOPEN: glibc wants _DEFAULT_SOURCE,
// macOS wants _DARWIN_C_SOURCE.
#define _DEFAULT_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

context cx = {.ok = 1};

// ---- regions ---------------------------------------------------------------------------------

// Address space reserved per region. Only what is written is ever backed by memory, so these
// are limits, not costs. If the system refuses a reservation (a strict overcommit policy),
// smaller ones are tried down to REGION_MIN.
static const size_t region_reserve[REGION__COUNT] = {
    [REGION_ARENA] = (size_t)1 << 30,  [REGION_TOKENS] = (size_t)1 << 32,
    [REGION_NODES] = (size_t)1 << 30,  [REGION_SYMS] = (size_t)1 << 28,
    [REGION_TYPES] = (size_t)1 << 28,  [REGION_SCOPES] = (size_t)1 << 28,
    [REGION_NAMES] = (size_t)1 << 30,  [REGION_SYMNAMES] = (size_t)1 << 28,
};
#define REGION_MIN ((size_t)1 << 22)

static const char *const region_names[REGION__COUNT] = {"arena", "tokens", "nodes",
                                                        "symbols", "types", "scopes",
                                                        "names",  "symbol names"};

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static void region_reserve_space(Region *r, RegionId id)
{
    for (size_t size = region_reserve[id]; size >= REGION_MIN; size /= 2)
    {
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                       -1, 0);
        if (p != MAP_FAILED)
        {
            r->base = (char *)p;
            r->reserved = size;
            return;
        }
    }
    fprintf(stderr, "fatal: cannot reserve address space for the %s\n", region_names[id]);
    exit(2);
}

void *region_push(RegionId id, size_t bytes)
{
    Region *r = &cx.regions[id];
    if (!r->base)
    {
        region_reserve_space(r, id);
    }
    if (bytes > r->reserved - r->used)
    {
        fprintf(stderr, "fatal: out of memory: the %s need more than %zu MB\n", region_names[id],
                r->reserved >> 20);
        exit(2);
    }
    void *mem = r->base + r->used;
    r->used += bytes;
    if (r->used > r->peak)
    {
        r->peak = r->used;
    }
    return mem;
}

void *region_end(RegionId id)
{
    Region *r = &cx.regions[id];
    if (!r->base)
    {
        region_reserve_space(r, id);
    }
    return r->base + r->used;
}

void *region_room(RegionId id, size_t bytes)
{
    Region *r = &cx.regions[id];
    char *end = (char *)region_end(id);
    if (bytes > r->reserved - r->used)
    {
        fprintf(stderr, "fatal: out of memory: the %s need more than %zu MB\n", region_names[id],
                r->reserved >> 20);
        exit(2);
    }
    return end;
}

void region_set_end(RegionId id, const void *end)
{
    Region *r = &cx.regions[id];
    r->used = (size_t)((const char *)end - r->base);
    if (r->used > r->reserved)
    {
        fprintf(stderr, "fatal: out of memory: the %s need more than %zu MB\n", region_names[id],
                r->reserved >> 20);
        exit(2);
    }
    if (r->used > r->peak)
    {
        r->peak = r->used;
    }
}

void *region_grow_array(RegionId id, void *data, size_t elem, size_t *cap, size_t need)
{
    if (need <= *cap)
    {
        return data;
    }
    size_t new_cap = *cap * 2 > need ? *cap * 2 : need;
    if (new_cap < 64)
    {
        new_cap = 64;
    }
    if (data && (char *)data + *cap * elem == (char *)region_end(id))
    {
        region_push(id, (new_cap - *cap) * elem); // the last array in its region: in place
        *cap = new_cap;
        return data;
    }
    void *moved = region_push(id, new_cap * elem);
    if (data && *cap)
    {
        memcpy(moved, data, *cap * elem);
    }
    *cap = new_cap;
    return moved;
}

size_t regions_used(void)
{
    size_t used = 0;
    for (int i = 0; i < REGION__COUNT; i++)
    {
        used += cx.regions[i].used;
    }
    return used;
}

// Pages actually resident, asked of the system (mincore): reserved room that was never
// written is not counted. Regions never give pages back, so this is also the peak.
size_t regions_touched(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t touched = 0;
    unsigned char vec[4096];
    for (int i = 0; i < REGION__COUNT; i++)
    {
        const Region *r = &cx.regions[i];
        size_t pages = (r->peak + page - 1) / page;
        for (size_t first = 0; first < pages; first += sizeof(vec))
        {
            size_t n = pages - first < sizeof(vec) ? pages - first : sizeof(vec);
            if (mincore(r->base + first * page, n * page, (void *)vec) != 0)
            {
                touched += n * page; // cannot ask: count the whole range
                continue;
            }
            for (size_t k = 0; k < n; k++)
            {
                touched += (vec[k] & 1) ? page : 0;
            }
        }
    }
    return touched;
}

// ---- arena -----------------------------------------------------------------------------------

void *arena_alloc(size_t sz)
{
    return region_push(REGION_ARENA, (sz + 7u) & ~(size_t)7u);
}

ArenaMark arena_mark(void)
{
    return (ArenaMark){cx.regions[REGION_ARENA].used};
}

void arena_release(ArenaMark mark)
{
    cx.regions[REGION_ARENA].used = mark.used;
}

void arena_shrink(void *ptr, size_t old_size, size_t new_size)
{
    Region *r = &cx.regions[REGION_ARENA];
    old_size = (old_size + 7u) & ~(size_t)7u;
    new_size = (new_size + 7u) & ~(size_t)7u;
    if (r->base && (char *)ptr + old_size == r->base + r->used)
    {
        r->used -= old_size - new_size;
    }
}

void *arena_grow(void *data, size_t elem, size_t *cap)
{
    size_t need = *cap ? *cap * 2 : 16;
    size_t bytes = (*cap * elem + 7u) & ~(size_t)7u;
    Region *r = &cx.regions[REGION_ARENA];
    if (data && (char *)data + bytes == r->base + r->used)
    {
        // the most recent allocation: extend it where it is
        region_push(REGION_ARENA, ((need * elem + 7u) & ~(size_t)7u) - bytes);
        *cap = need;
        return data;
    }
    void *grown = arena_alloc(need * elem);
    if (*cap)
    {
        memcpy(grown, data, *cap * elem);
    }
    *cap = need;
    return grown;
}

void context_reset(void)
{
    Region regions[REGION__COUNT];
    memcpy(regions, cx.regions, sizeof(regions));
    memset(&cx, 0, sizeof(cx));
    cx.ok = 1;
    for (int i = 0; i < REGION__COUNT; i++)
    {
        cx.regions[i] = regions[i];
        cx.regions[i].used = 0;
    }
}

void arena_drop(void)
{
    for (int i = 0; i < REGION__COUNT; i++)
    {
        Region *r = &cx.regions[i];
        if (r->base)
        {
            munmap(r->base, r->reserved);
        }
        *r = (Region){0};
    }
}

uint64_t clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void diag_vat(Unit *u, uint32_t tok, const char *level, const char *fmt, va_list ap)
{
    const Token *t = TOK(u, tok);

    // After MAX_REPORTED errors the rest are counted but not printed: a mistake repeated
    // across a large file should not bury the first reports or cost seconds of output.
    if (level[0] == 'e' && cx.errors >= MAX_REPORTED)
    {
        u->errors++;
        cx.errors++;
        cx.ok = 0;
        return;
    }

    int line, col;
    source_pos(u->src, t->start, &line, &col);

    fprintf(stderr, "\n%s [%s:%d:%d]: ", level, u->path, line, col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    // Whole source line for context: the token points straight into the source buffer.
    const char *line_start = t->start;
    while (line_start > u->src && line_start[-1] != '\n')
    {
        line_start--;
    }
    const char *line_end = t->start;
    while (*line_end && *line_end != '\n')
    {
        line_end++;
    }
    // The caret line is built in memory and written once: tabs stay tabs so it lines up.
    char caret[256];
    size_t n = 0;
    fprintf(stderr, "%4d | %.*s\n     | ", line, (int)(line_end - line_start), line_start);
    for (const char *c = line_start; c < t->start; c++)
    {
        if (n == sizeof(caret) - 2) // a very long line: write what is built and go on
        {
            fwrite(caret, 1, n, stderr);
            n = 0;
        }
        caret[n++] = *c == '\t' ? '\t' : ' ';
    }
    caret[n++] = '^';
    caret[n++] = '\n';
    fwrite(caret, 1, n, stderr);

    if (level[0] == 'e')
    {
        u->errors++;
        cx.errors++;
        cx.ok = 0;
    }
}

void diag_at(Unit *u, uint32_t tok, const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vat(u, tok, level, fmt, ap);
    va_end(ap);
}
