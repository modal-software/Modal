
#ifndef MVEC_H
#define MVEC_H

#include "palloc.h"
#include <assert.h>
#include <stddef.h>

#define MVEC_IMPL(Name, T)                                                                               \
    typedef struct                                                                                       \
    {                                                                                                    \
        T *data;                                                                                         \
        size_t size;                                                                                     \
        size_t cap;                                                                                      \
        size_t chunks;                                                                                   \
        Pool *ref;                                                                                       \
    } _mvec_##Name;                                                                                      \
                                                                                                         \
    typedef struct                                                                                       \
    {                                                                                                    \
        _mvec_##Name (*init)(Pool * p, size_t reserved);                                                 \
        void (*push)(_mvec_##Name * vec, T value);                                                       \
        T (*sum)(const _mvec_##Name *vec);                                                               \
        void (*free)(_mvec_##Name * vec);                                                                \
        T (*pop)(_mvec_##Name * vec);                                                                    \
        void (*release)(_mvec_##Name * *vec);                                                            \
    } _mvec_constructor_##Name;                                                                          \
                                                                                                         \
    static inline _mvec_##Name mvec_init_##Name(Pool *p, size_t reserved)                                \
    {                                                                                                    \
        size_t data_per_chunk = (CHUNK_SZ / sizeof(T));                                                  \
        if (reserved == 0)                                                                               \
        {                                                                                                \
            reserved = 1;                                                                                \
        }                                                                                                \
                                                                                                         \
        T *initial_data = (T *)pool.alloc(p);                                                            \
                                                                                                         \
        _mvec_##Name vec = {                                                                             \
            .data = initial_data,                                                                        \
            .size = 0,                                                                                   \
            .cap = data_per_chunk,                                                                       \
            .chunks = 1,                                                                                 \
            .ref = p,                                                                                    \
        };                                                                                               \
                                                                                                         \
        for (size_t i = vec.chunks; i < reserved; i++)                                                   \
        {                                                                                                \
            pool.alloc(p);                                                                               \
            vec.chunks++;                                                                                \
            vec.cap = vec.chunks * data_per_chunk;                                                       \
        }                                                                                                \
        return vec;                                                                                      \
    }                                                                                                    \
                                                                                                         \
    static inline void mvec_push_##Name(_mvec_##Name *vec, T value)                                      \
    {                                                                                                    \
        size_t data_per_chunk = (CHUNK_SZ / sizeof(T));                                                  \
                                                                                                         \
        if (vec->size < vec->cap)                                                                        \
        {                                                                                                \
            vec->data[vec->size] = value;                                                                \
            vec->size++;                                                                                 \
            return;                                                                                      \
        }                                                                                                \
                                                                                                         \
        void *new_chunk = pool.alloc(vec->ref);                                                          \
        if (!new_chunk)                                                                                  \
        {                                                                                                \
            fprintf(stderr, "vector: failed to resize (pool out of memory)\n");                          \
            return;                                                                                      \
        }                                                                                                \
                                                                                                         \
        vec->chunks = vec->chunks * 2;                                                                   \
        vec->cap = vec->chunks * data_per_chunk;                                                         \
        vec->data[vec->size] = value;                                                                    \
        vec->size++;                                                                                     \
    }                                                                                                    \
    /*                                                                                                   \
        static inline T mvec_sum_fast(const _mvec *vec)                                                  \
        {                                                                                                \
            size_t n = vec->size;                                                                        \
            T *arr = vec->data;                                                                          \
            T total_sum = {0};                                                                           \
            size_t i = 0;                                                                                \
            size_t data_per_chunk = (CHUNK_SZ / sizeof(T));                                              \
                                                                                                       \ \
            for (; i <= n - 4; i += 4)                                                                   \
            {                                                                                            \
                &arr[i + data_per_chunk];                                                                \
                total_sum += arr[i] + arr[i + 1] + arr[i + 2] + arr[i + 3];                              \
            }                                                                                            \
                                                                                                       \ \
            for (; i < n; i++)                                                                           \
            {                                                                                            \
                total_sum += arr[i];                                                                     \
            }                                                                                            \
            return total_sum;                                                                            \
        }                                                                                                \
        */                                                                                               \
    static inline T mvec_pop_##Name(_mvec_##Name *vec)                                                   \
    {                                                                                                    \
        assert(vec->size > 0);                                                                           \
                                                                                                         \
        return vec->data[--vec->size];                                                                   \
    }                                                                                                    \
    static inline void mvec_release_##Name(_mvec_##Name **vec)                                           \
    {                                                                                                    \
        *vec = NULL; /* reclaim real = pool.reset no fim da fase */                                      \
    }                                                                                                    \
                                                                                                         \
    static inline void mvec_free_##Name(_mvec_##Name *vec)                                               \
    {                                                                                                    \
        if (!vec->data)                                                                                  \
        {                                                                                                \
            return;                                                                                      \
        }                                                                                                \
                                                                                                         \
        T *old_ptr = vec->data;                                                                          \
                                                                                                         \
        for (size_t i = 0; i < vec->chunks; i++)                                                         \
        {                                                                                                \
            void *chunk_to_free = (void *)((uintptr_t)old_ptr + (i * CHUNK_SZ));                         \
            pool.free(vec->ref, &chunk_to_free);                                                         \
        }                                                                                                \
        vec->data = NULL;                                                                                \
        vec->size = 0;                                                                                   \
        vec->cap = 0;                                                                                    \
        vec->chunks = 0;                                                                                 \
    }

#endif
