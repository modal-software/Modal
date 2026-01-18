#ifndef ALLOCATORS_H
#define ALLOCATORS_H

#define free(ptr) ((void)0)
#define malloc(sz) xmalloc(sz)
#define realloc(p, s) xrealloc(p, s)
#define calloc(n, s) xcalloc(n, s)

#endif
