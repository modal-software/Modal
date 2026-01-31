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
    if (!size)
    {
        return NULL;
    }

    return arena_alloc_raw(size);
}

void *xcalloc(size_t num, size_t size)
{
    size_t total = num * size;
    void *p = arena_alloc_raw(total);
    void *new_ptr = calloc(sizeof(p), total);
    if (!new_ptr)
    {
        return NULL;
    }

    return (char *)new_ptr + sizeof(size_t);
}

void *xrealloc(void *ptr, size_t new_size)
{
    if (!ptr)
    {
        return xmalloc(new_size);
    }
    size_t *header = (size_t *)((char *)ptr - sizeof(size_t));
    size_t old_size = *header;

    if (new_size <= old_size)
    {
        return ptr;
    }

    size_t old_alloc_size = sizeof(size_t) + old_size;
    size_t new_alloc_size = sizeof(size_t) + new_size;

    void *new_ptr = realloc(header, new_alloc_size);
    if (!new_ptr)
    {
        return NULL;
    }

    *(size_t *)new_ptr = new_size;

    return (char *)new_ptr + sizeof(size_t);
}
