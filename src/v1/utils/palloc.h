#ifndef PALLOCATOR_H
#define PALLOCATOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static inline int lfp_init(LFPool **out, size_t capacity)
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
static inline void lfp_reset(LFPool *p)
{
    if (!p)
    {
        return;
    }
    p->head = p->cursor;
}

static inline void lfp_drop(LFPool *self)
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

static inline void *lfp_alloc_sz(LFPool *self, size_t sz)
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

static inline void *lfp_alloc(LFPool *self)
{
    return lfp_alloc_sz(self, CHUNK_SZ);
}

#define POOL_DEFAULTS                                                                              \
    .allocator = lfp_init, .alloc = lfp_alloc, .alloc_sz = lfp_alloc_sz, .drop = lfp_drop,         \
    .reset = lfp_reset

#ifdef PALLOC_IMPLEMENTATION
const LFPoolInterface *pool = &(LFPoolInterface){POOL_DEFAULTS};
#else
extern const LFPoolInterface *pool;
#endif

#endif // PALLOCATOR_H
