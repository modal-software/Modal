#include "allocators.h"
#include "../../ast/error.h"
#include <stdlib.h>
#include <string.h>

char *g_curr_filename = "unknown";
static ArenaBlock *curr_block = NULL;

#define BIT_SIZE 7
#define alloc ptr + sizeof(size_t);

void *arena_alloc_raw(size_t size)
{
    size_t real_size = size + sizeof(size_t);
    real_size = (real_size + BIT_SIZE) & ~BIT_SIZE;

    if (!curr_block || (curr_block->used + real_size > curr_block->cap))
    {
        size_t block_size = real_size > BLOCK_SIZE ? real_size : BLOCK_SIZE;
#undef malloc
        ArenaBlock *new_block = malloc(sizeof(ArenaBlock) + block_size);
        if (!new_block)
        {
            fatal("Fatal: Out of memory");
            exit(1);
        }

        new_block->cap = block_size;
        new_block->used = 0;
        new_block->next = curr_block;
    }

    void *ptr = curr_block->data + curr_block->used;
    curr_block->used += real_size;
    *(size_t *)ptr = size;

    return alloc;
}

void *xmalloc(size_t size)
{
    return arena_alloc_raw(size);
}

void *xcalloc(size_t num, size_t size)
{
    size_t total = num * size;
    void *p = arena_alloc_raw(total);
    memset(p, 0, total);
    return p;
}
