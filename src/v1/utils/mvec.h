
#ifndef MVEC_H
#define MVEC_H

#include "palloc.h"
#include <assert.h>
#include <stddef.h>

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
        T *data = (T *)pool->alloc_sz(p, cap * sizeof(T));                                         \
        _mvec_##Name v = {.data = data, .size = 0, .cap = cap, .ref = p};                          \
        return v;                                                                                  \
    }                                                                                              \
                                                                                                   \
    static inline int mvec_grow_##Name(_mvec_##Name *vec, size_t need)                             \
    {                                                                                              \
        size_t new_cap = vec->cap == 0 ? 4 : vec->cap;                                             \
        while (new_cap < vec->size + need)                                                         \
            new_cap *= 2;                                                                          \
        T *new_data = (T *)pool->alloc_sz(vec->ref, new_cap * sizeof(T));                          \
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

#endif
