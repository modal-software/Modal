
#ifndef MVEC_H
#define MVEC_H

#include "palloc.h"
#include <assert.h>
#include <stddef.h>

#define MVEC_IMPL(T)                                                                               \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        T *data;                                                                                   \
        size_t size;                                                                               \
        size_t cap;                                                                                \
        size_t chunks;                                                                             \
        Pool *ref;                                                                                 \
    } _mvec;                                                                                       \
                                                                                                   \
    typedef _mvec Vector;                                                                          \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        Vector (*init)(Pool * p, size_t reserved);                                                 \
        void (*push)(_mvec * vec, T value);                                                        \
        T (*sum)(const _mvec *vec);                                                                \
        void (*free)(_mvec * vec);                                                                 \
    } _mvec_constructor;                                                                           \
                                                                                                   \
    static inline _mvec mvec_init(Pool *p, size_t reserved)                                        \
    {                                                                                              \
        size_t data_per_chunk = (CHUNK_SZ / sizeof(T));                                            \
        if (reserved == 0)                                                                         \
        {                                                                                          \
            reserved = 1;                                                                          \
        }                                                                                          \
                                                                                                   \
        T *initial_data = pool.alloc(p);                                                           \
                                                                                                   \
        _mvec vec = {                                                                              \
            .data = initial_data,                                                                  \
            .size = 0,                                                                             \
            .cap = data_per_chunk,                                                                 \
            .chunks = 1,                                                                           \
            .ref = p,                                                                              \
        };                                                                                         \
                                                                                                   \
        for (size_t i = vec.chunks; i < reserved; i++)                                             \
        {                                                                                          \
            pool.alloc(p);                                                                         \
            vec.chunks++;                                                                          \
            vec.cap = vec.chunks * data_per_chunk;                                                 \
        }                                                                                          \
        return vec;                                                                                \
    }                                                                                              \
                                                                                                   \
    static inline void mvec_push(_mvec *vec, T value)                                              \
    {                                                                                              \
        size_t data_per_chunk = (CHUNK_SZ / sizeof(T));                                            \
                                                                                                   \
        if (vec->size < vec->cap)                                                                  \
        {                                                                                          \
            vec->data[vec->size] = value;                                                          \
            vec->size++;                                                                           \
            return;                                                                                \
        }                                                                                          \
                                                                                                   \
        void *new_chunk = pool.alloc(vec->ref);                                                    \
        if (!new_chunk)                                                                            \
        {                                                                                          \
            fprintf(stderr, "vector: failed to resize (pool out of memory)\n");                    \
            return;                                                                                \
        }                                                                                          \
                                                                                                   \
        vec->chunks = vec->chunks * 2;                                                             \
        vec->cap = vec->chunks * data_per_chunk;                                                   \
        vec->data[vec->size] = value;                                                              \
        vec->size++;                                                                               \
    }                                                                                              \
                                                                                                   \
    static inline T mvec_sum_fast(const _mvec *vec)                                                \
    {                                                                                              \
        size_t n = vec->size;                                                                      \
        const T *arr = vec->data;                                                                  \
        T total_sum = 0;                                                                           \
        size_t i = 0;                                                                              \
        size_t data_per_chunk = (CHUNK_SZ / sizeof(T));                                            \
                                                                                                   \
        for (; i <= n - 4; i += 4)                                                                 \
        {                                                                                          \
            &arr[i + data_per_chunk];                                                              \
            total_sum += arr[i] + arr[i + 1] + arr[i + 2] + arr[i + 3];                            \
        }                                                                                          \
                                                                                                   \
        for (; i < n; i++)                                                                         \
        {                                                                                          \
            total_sum += arr[i];                                                                   \
        }                                                                                          \
        return total_sum;                                                                          \
    }                                                                                              \
    static inline void mvec_pop(_mvec *vec)                                                        \
    {                                                                                              \
        assert(vec->size > 0);                                                                     \
                                                                                                   \
        vec->data[--vec->size];                                                                    \
    }                                                                                              \
                                                                                                   \
    static inline void mvec_free(_mvec *vec)                                                       \
    {                                                                                              \
        if (!vec->data)                                                                            \
        {                                                                                          \
            return;                                                                                \
        }                                                                                          \
                                                                                                   \
        T *old_ptr = vec->data;                                                                    \
                                                                                                   \
        for (size_t i = 0; i < vec->chunks; i++)                                                   \
        {                                                                                          \
            void *chunk_to_free = (void *)((uintptr_t)old_ptr + (i * CHUNK_SZ));                   \
            pool.free(vec->ref, &chunk_to_free);                                                   \
        }                                                                                          \
        vec->data = NULL;                                                                          \
        vec->size = 0;                                                                             \
        vec->cap = 0;                                                                              \
        vec->chunks = 0;                                                                           \
    }                                                                                              \
                                                                                                   \
    static const _mvec_constructor vec = {                                                         \
        .init = mvec_init,                                                                         \
        .push = mvec_push,                                                                         \
        .sum = mvec_sum_fast,                                                                      \
        .free = mvec_free,                                                                         \
    };

#endif
