#ifndef ALLOCATORS_H
#define ALLOCATORS_H
#include <stddef.h>

#define BLOCK_SIZE (1024 * 1024)

typedef struct ArenaBlock
{
    struct ArenaBlock *next;
    size_t used;
    size_t cap;
    char data[];
} ArenaBlock;

void *arena_alloc_raw(size_t size);

#endif
