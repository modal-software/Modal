#ifndef PALLOCATOR_H
#define PALLOCATOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHUNK_SZ 64
#define TRACING 0

typedef union Chunk Chunk;

union Chunk
{
    Chunk *next;
    uint8_t buffer[CHUNK_SZ];
};

typedef struct _pool_context
{
    uint32_t capacity;
    void *obj;
} PoolCX;

static inline void *align_backward(void *ptr, size_t align)
{
    if (TRACING == 1)
    {
        printf("ptr: %p\n", ptr);
        printf("align: %zu\n", align);
    }
    return (void *)((uintptr_t)ptr & ~(align - 1));
}

typedef struct
{
    // Head start of pool
    Chunk *head;
    Chunk *slot;

    // Object-context ref;
    PoolCX cx;
} Pool;

typedef struct
{
    Pool *(*allocator)(size_t amount);
    void *(*alloc)(Pool *pool);

    void (*free)(Pool *pool, void **ptr);
    void (*drop)(Pool *pool);
} PAllocatorConstrutor;

static Pool *pool_init(size_t amount)
{
    Pool *pool = (Pool *)malloc(sizeof(Pool));
    if (pool == NULL)
    {
        return NULL;
    }

    pool->cx.obj = malloc((amount * sizeof(Chunk)) + 63);
    pool->slot = (Chunk *)align_backward(pool->cx.obj, CHUNK_SZ);
    printf("obj: %p\nslot: %p\n", pool->cx.obj, pool->slot);
    if (!pool->slot)
    {
        free(pool->cx.obj);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < amount - 1; i++)
    {
        pool->slot[i].next = &pool->slot[i + 1];
    }

    pool->slot[amount - 1].next = NULL;

    pool->head = pool->slot;

    if (TRACING == 1)
    {
        printf("Pool created! %zu chunks de %d bytes cada (total ~%.1f KB)\n", amount, CHUNK_SZ,
               (amount * sizeof(Chunk)) / 1024.0);
    }

    return pool;
}

static void *pool_alloc(Pool *self)
{
    if (!self || !self->head)
    {
        fprintf(stderr, "fatal: pool ran out of memory\n[Process exited %d]\n", EXIT_FAILURE);
        return NULL;
    }

    Chunk *chunk = self->head;
    self->head = chunk->next;

    if (TRACING == 1)
    {
        printf("allocated {\n  addr: %p\n}\n", (void *)chunk);
    }
    return chunk->buffer;
}

static void pool_free(Pool *self, void **ptr)
{
    if (self == NULL || ptr == NULL)
    {
        fprintf(stderr, "fatal: no object available to free\n[Process exited %d]\n", EXIT_FAILURE);
        return;
    }

    Chunk *chunk = (Chunk *)*ptr;

    // printf("before freeing\n\n");
    // printf("slot->buffer: %u (%p)\n", *self->slot->buffer, self->slot->buffer);
    // printf("head->buffer: %u (%p)\n", *self->head->buffer, self->head->buffer);
    // printf("chunk->buffer: %u (%p)\n", *chunk->buffer, chunk->buffer);
    // printf("next->buffer: %u (%p)\n\n", *chunk->next->buffer,
    //        chunk->next->buffer);
    // printf("next->next->buffer: %u\n\n", *chunk->next->next->buffer);

    chunk->next = self->head;
    self->head = chunk;

    *ptr = NULL;
    if (TRACING == 1)
    {
        printf("freed amount: %p\n", ptr);
    }
}

static void pool_drop(Pool *self)
{
    if (!self || self == NULL)
    {
        fprintf(stderr, "fatal: no pool to drop\n[Process exited %d]\n", EXIT_FAILURE);
        return;
    }

    free(self->cx.obj);
    free(self);
    if (TRACING == 1)
    {
        printf("Pool destroyed!\n");
    }
}

#define FOO_DEFAULTS                                                                               \
    .allocator = pool_init, .alloc = pool_alloc, .free = pool_free, .drop = pool_drop,

const PAllocatorConstrutor pool = {FOO_DEFAULTS};

#endif // PALLOCATOR_H
