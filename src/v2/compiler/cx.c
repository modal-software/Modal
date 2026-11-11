#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

context cx = {.ok = 1};

static void arena_new_block(size_t need)
{
    size_t cap = need > ARENA_BLOCK_MIN ? need : ARENA_BLOCK_MIN;
    ArenaBlock *block = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    if (!block)
    {
        fprintf(stderr, "fatal: out of memory while allocating %zu bytes\n", cap);
        exit(2);
    }
    block->prev = cx.arena.block;
    block->cap = cap;
    block->used = 0;
    cx.arena.block = block;
    cx.arena.reserved += sizeof(ArenaBlock) + cap;
    if (cx.arena.reserved > cx.arena.peak)
    {
        cx.arena.peak = cx.arena.reserved;
    }
}

void *arena_alloc(size_t sz)
{
    sz = (sz + 7u) & ~(size_t)7u;
    ArenaBlock *block = cx.arena.block;
    if (!block || block->used + sz > block->cap)
    {
        arena_new_block(sz);
        block = cx.arena.block;
    }
    void *mem = (char *)(block + 1) + block->used;
    block->used += sz;
    return mem;
}

ArenaMark arena_mark(void)
{
    return (ArenaMark){cx.arena.block, cx.arena.block ? cx.arena.block->used : 0};
}

void arena_release(ArenaMark mark)
{
    while (cx.arena.block != mark.block)
    {
        ArenaBlock *block = cx.arena.block;
        cx.arena.block = block->prev;
        cx.arena.reserved -= sizeof(ArenaBlock) + block->cap;
        free(block);
    }
    if (cx.arena.block)
    {
        cx.arena.block->used = mark.used;
    }
}

void arena_shrink(void *ptr, size_t old_size, size_t new_size)
{
    ArenaBlock *block = cx.arena.block;
    old_size = (old_size + 7u) & ~(size_t)7u;
    new_size = (new_size + 7u) & ~(size_t)7u;
    if (block && (char *)ptr + old_size == (char *)(block + 1) + block->used)
    {
        block->used -= old_size - new_size;
    }
}

void *arena_grow(void *data, size_t elem, size_t *cap)
{
    size_t new_cap = *cap ? *cap * 2 : 16;
    void *grown = arena_alloc(new_cap * elem);
    if (*cap)
    {
        memcpy(grown, data, *cap * elem);
    }
    *cap = new_cap;
    return grown;
}

size_t arena_used(void)
{
    size_t used = 0;
    for (const ArenaBlock *b = cx.arena.block; b; b = b->prev)
    {
        used += b->used;
    }
    return used;
}

size_t arena_reserved(void)
{
    return cx.arena.reserved;
}

size_t arena_peak(void)
{
    return cx.arena.peak;
}

void arena_drop(void)
{
    arena_release((ArenaMark){NULL, 0});
}

void diag_vat(Unit *u, uint32_t tok, const char *level, const char *fmt, va_list ap)
{
    const Token *t = TOK(u, tok);

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
    fprintf(stderr, "%4d | %.*s\n     | ", line, (int)(line_end - line_start), line_start);
    for (const char *c = line_start; c < t->start; c++)
    {
        fputc(*c == '\t' ? '\t' : ' ', stderr);
    }
    fputs("^\n", stderr);

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
