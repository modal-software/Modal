// cache.c (v2): skipping files that have not changed (Go's build cache, per file).
//
// A file's bodies are checked against its own text and against the declarations of the other
// files of the program; never against their bodies, which the one-function pipeline does not
// even keep. So when neither changed, checking the bodies again gives the same answer, and a
// check-only run can skip it: no body is scanned, parsed or checked.
//
//   key(file) = hash(version, flags, the whole file, every other file's path and declarations)
//
// Only clean results are kept (a file with errors is always checked, so they are printed),
// and a file whose bodies import other files is never cached (the import changes which files
// are loaded). The cache of a program is one small file: one open and read per run.
#define _XOPEN_SOURCE 700
#include "v2/compiler/cx.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_MAGIC 0x31454843444f4dull // "MODCHE1"
#define CACHE_MAX 4096

typedef struct
{
    uint64_t path; // hash of the canonical path
    uint64_t key;
    uint32_t names, calls; // what checking it added to the counters of the ok line
} CacheEntry;

static CacheEntry entries[CACHE_MAX];
static size_t nentries;
static int changed;
static char cache_path[PATH_MAX];

// 8 bytes at a time with multiply-mix steps; enough for detecting changes (not cryptographic).
uint64_t hash_bytes(const void *data, size_t len, uint64_t seed)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = seed ^ (len * 0x9E3779B97F4A7C15ull);
    while (len >= 8)
    {
        uint64_t w;
        memcpy(&w, p, 8);
        h = (h ^ (w * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
        h ^= h >> 31;
        p += 8;
        len -= 8;
    }
    uint64_t w = 0;
    memcpy(&w, p, len);
    h = (h ^ (w * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
    h ^= h >> 29;
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 32);
}

void cache_unit_loaded(Unit *u)
{
    if (!cx.cache)
    {
        return;
    }
    u->src_hash = hash_bytes(u->src, u->len, 1);
    // The declarations as the other files see them: the text between deferred bodies, hashed
    // in large runs (a body counts only by its position). Comments and spacing outside bodies
    // are included, which can only make the cache miss, never hit wrongly.
    uint64_t h = 2;
    const char *from = u->src;
    for (size_t i = 0; i < u->tokens.size; i++)
    {
        const Token *t = &u->tokens.data[i];
        if (t->kind == TOK_BODY)
        {
            h = hash_bytes(from, (size_t)(t->start - from), h);
            from = t->start + t->len;
        }
    }
    h = hash_bytes(from, (size_t)(u->src + u->len - from), h);
    u->decl_hash = h;
}

static uint64_t path_hash(const Unit *u)
{
    return hash_bytes(u->canon, strlen(u->canon), 3);
}

static uint64_t unit_key(const Unit *u)
{
    uint64_t key = hash_bytes(MODAL_VERSION, sizeof(MODAL_VERSION) - 1, u->src_hash);
    key = hash_bytes(&(uint32_t){cx.eager}, 4, key);
    // Order-free (the same files may load in another order): XOR of each file's term.
    uint64_t others = 0;
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        const Unit *v = &cx.units[i];
        if (v != u)
        {
            others ^= hash_bytes(&v->decl_hash, 8, path_hash(v));
        }
    }
    return hash_bytes(&others, 8, key);
}

void cache_open(const char *entry)
{
    if (!cx.cache)
    {
        return;
    }
    const char *dir = getenv("MODAL_CACHE") ? getenv("MODAL_CACHE") : ".modal-cache";
    mkdir(dir, 0777); // fails harmlessly when it exists
    char abs[PATH_MAX];
    const char *name = realpath(entry, abs) ? abs : entry; // once per run, for the file name
    snprintf(cache_path, sizeof(cache_path), "%s/%016llx.cache", dir,
             (unsigned long long)hash_bytes(name, strlen(name), 4));

    int fd = open(cache_path, O_RDONLY);
    if (fd < 0)
    {
        return;
    }
    uint64_t magic = 0;
    if (read(fd, &magic, 8) == 8 && magic == CACHE_MAGIC)
    {
        ssize_t got = read(fd, entries, sizeof(entries));
        nentries = got > 0 ? (size_t)got / sizeof(CacheEntry) : 0;
    }
    close(fd);
}

static CacheEntry *find_entry(uint64_t path)
{
    for (size_t i = 0; i < nentries; i++)
    {
        if (entries[i].path == path)
        {
            return &entries[i];
        }
    }
    return NULL;
}

int cache_lookup(Unit *u)
{
    if (!cx.cache)
    {
        return 0;
    }
    const CacheEntry *e = find_entry(path_hash(u));
    if (!e || e->key != unit_key(u))
    {
        return 0;
    }
    cx.names_resolved += e->names;
    cx.calls_resolved += e->calls;
    u->cached = 1;
    return 1;
}

void cache_record(Unit *u, uint32_t names, uint32_t calls)
{
    if (!cx.cache)
    {
        return;
    }
    uint64_t path = path_hash(u);
    CacheEntry *e = find_entry(path);
    if (!e)
    {
        if (nentries == CACHE_MAX)
        {
            return;
        }
        e = &entries[nentries++];
    }
    *e = (CacheEntry){path, unit_key(u), names, calls};
    changed = 1;
}

void cache_save(void)
{
    if (!cx.cache || !changed || !cache_path[0])
    {
        return;
    }
    // Written aside and renamed over: a run that stops halfway never leaves a torn cache.
    char tmp[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s.%ld", cache_path, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        return;
    }
    uint64_t magic = CACHE_MAGIC;
    size_t bytes = nentries * sizeof(CacheEntry);
    int ok = write(fd, &magic, 8) == 8 && write(fd, entries, bytes) == (ssize_t)bytes;
    close(fd);
    if (!ok || rename(tmp, cache_path) != 0)
    {
        remove(tmp);
    }
}
