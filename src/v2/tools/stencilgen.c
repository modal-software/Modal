// stencilgen.c (v2): turns the compiled stencils into a C header (build time only).
//
//   stencilgen stencils.o arm64  > stencils_arm64.h
//   stencilgen stencils.o x86_64 > stencils_x86_64.h
//
// Reads an ELF relocatable object built from backend/native/stencils.c with -ffunction-sections
// (ELF for every target: its relocations are simple to read, and the machine code is the same)
// and writes, for every function, its machine code and its holes: where each relocation is,
// what kind it is and what it refers to. The native backend copies those bytes and fills the
// holes. A relocation it could not fill (data, an unknown kind) is an error here, at build time.
//
// The ids (ST_*, RT_*) are the functions' names in sorted order, so every target's header has
// the same ids; STENCIL_SET_HASH lets the backend check that.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;

typedef struct
{
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} Shdr;

typedef struct
{
    uint32_t name;
    unsigned char info, other;
    uint16_t shndx;
    uint64_t value, size;
} Sym;

typedef struct
{
    uint64_t offset, info;
    int64_t addend;
} Rela;

#define SHT_SYMTAB 2
#define SHT_RELA 4
#define EM_X86_64 62
#define EM_AARCH64 183

// Relocation kinds the backend knows how to fill (its StencilRelocKind). The two machines'
// ELF numbers do not overlap.
static const struct
{
    uint32_t elf;
    const char *kind;
    int addend_ok; // a pc-relative x86 field: the addend (-4) is kept
} kinds[] = {
    {264, "SR_MOVW_G0", 0},  {266, "SR_MOVW_G1", 0},  {268, "SR_MOVW_G2", 0},
    {269, "SR_MOVW_G3", 0},  {282, "SR_BRANCH26", 0}, {283, "SR_BRANCH26", 0},
    {280, "SR_BRANCH19", 0}, {279, "SR_BRANCH14", 0}, {10, "SR_ABS32", 0},
    {11, "SR_ABS32S", 0},    {2, "SR_REL32", 1},      {4, "SR_REL32", 1},
};

static unsigned char *file;
static size_t file_len;
static int aarch64;

static void fail(const char *what, const char *detail)
{
    fprintf(stderr, "stencilgen: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

static const Shdr *section(const Ehdr *h, unsigned i)
{
    return (const Shdr *)(file + h->shoff + (uint64_t)i * h->shentsize);
}

// externs (C library functions) seen so far, in order
static const char *externs[64];
static int nexterns;

static int extern_index(const char *name)
{
    for (int i = 0; i < nexterns; i++)
    {
        if (strcmp(externs[i], name) == 0)
        {
            return i;
        }
    }
    if (nexterns == 64)
    {
        fail("too many C library functions", name);
    }
    externs[nexterns] = name;
    return nexterns++;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

typedef struct
{
    uint64_t offset;
    const char *kind;
    int64_t addend;
    char what[96];
    int high; // a movk of a hole's upper 16-bit pieces: absent in the compact form
} RelInfo;

// The compact form (arm64): when every hole value fits in 16 bits, a hole's movk pieces only
// write zeros, so they are left out. Instructions after them move up, so branches that stay
// inside the stencil are re-aimed; relocated ones are filled at copy time anyway.
static size_t compact_arm64(const char *name, const unsigned char *code, size_t size,
                            const RelInfo *rel, size_t nrel, uint8_t *out, uint32_t *new_at)
{
    static uint8_t keep[16384];
    size_t ninsn = size / 4;
    for (size_t k = 0; k < ninsn; k++)
    {
        keep[k] = 1;
    }
    for (size_t r = 0; r < nrel; r++)
    {
        if (rel[r].high)
        {
            keep[rel[r].offset / 4] = 0;
        }
    }
    uint32_t at = 0;
    for (size_t k = 0; k < ninsn; k++)
    {
        new_at[k] = at;
        at += keep[k] ? 4 : 0;
    }
    new_at[ninsn] = at;
    for (size_t k = 0; k < ninsn; k++)
    {
        if (!keep[k])
        {
            continue;
        }
        uint32_t w;
        memcpy(&w, code + k * 4, 4);
        int relocated = 0;
        for (size_t r = 0; r < nrel; r++)
        {
            relocated |= rel[r].offset == k * 4;
        }
        if ((w & 0x1f000000u) == 0x10000000u || (w & 0x3b000000u) == 0x18000000u)
        {
            fail("a PC-relative data reference (adr, adrp or a literal load)", name);
        }
        if (!relocated)
        {
            int64_t target = -1;
            int bits = 0;
            if ((w & 0x7c000000u) == 0x14000000u) // b, bl
            {
                target = (int64_t)k + ((int64_t)(int32_t)(w << 6) >> 6);
                bits = 26;
            }
            else if ((w & 0xff000010u) == 0x54000000u || (w & 0x7e000000u) == 0x34000000u)
            {
                target = (int64_t)k + ((int64_t)(int32_t)((w >> 5) << 13) >> 13); // b.cond, cbz
                bits = 19;
            }
            else if ((w & 0x7e000000u) == 0x36000000u) // tbz, tbnz
            {
                target = (int64_t)k + ((int64_t)(int32_t)(((w >> 5) & 0x3fffu) << 18) >> 18);
                bits = 14;
            }
            if (bits)
            {
                if (target < 0 || target > (int64_t)ninsn)
                {
                    fail("an internal branch out of the stencil", name);
                }
                int64_t delta = ((int64_t)new_at[target] - (int64_t)new_at[k]) / 4;
                if (bits == 26)
                {
                    w = (w & 0xfc000000u) | ((uint32_t)delta & 0x03ffffffu);
                }
                else if (bits == 19)
                {
                    w = (w & ~(0x7ffffu << 5)) | (((uint32_t)delta & 0x7ffffu) << 5);
                }
                else
                {
                    w = (w & ~(0x3fffu << 5)) | (((uint32_t)delta & 0x3fffu) << 5);
                }
            }
        }
        memcpy(out + new_at[k], &w, 4);
    }
    return at;
}

static void print_bytes(const char *array, const char *name, const uint8_t *p, size_t n)
{
    printf("static const uint8_t %s_%s[] = {", array, name);
    for (size_t k = 0; k < n; k++)
    {
        printf("%s0x%02x,", k % 16 ? "" : "\n    ", p[k]);
    }
    printf("\n};\n");
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fail("usage: stencilgen stencils.o ARCH", NULL);
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in)
    {
        fail("cannot read", argv[1]);
    }
    fseek(in, 0, SEEK_END);
    file_len = (size_t)ftell(in);
    fseek(in, 0, SEEK_SET);
    file = malloc(file_len);
    if (!file || fread(file, 1, file_len, in) != file_len)
    {
        fail("cannot read", argv[1]);
    }
    fclose(in);

    const Ehdr *h = (const Ehdr *)file;
    if (memcmp(h->ident, "\177ELF", 4) != 0 || h->ident[4] != 2 ||
        (h->machine != EM_AARCH64 && h->machine != EM_X86_64))
    {
        fail("not a 64-bit ELF object for aarch64 or x86-64", argv[1]);
    }
    aarch64 = h->machine == EM_AARCH64;

    const Shdr *symtab = NULL;
    for (unsigned i = 0; i < h->shnum; i++)
    {
        if (section(h, i)->type == SHT_SYMTAB)
        {
            symtab = section(h, i);
        }
    }
    if (!symtab)
    {
        fail("no symbol table", NULL);
    }
    const Sym *syms = (const Sym *)(file + symtab->offset);
    size_t nsyms = symtab->size / sizeof(Sym);
    const char *strtab = (const char *)file + section(h, symtab->link)->offset;

    printf("// stencils_%s.h: GENERATED by tools/stencilgen.c from backend/native/stencils.c.\n"
           "// Do not edit; run `make stencils` after changing stencils.c.\n\n",
           argv[2]);

    // Every function in its own .text.<name> section: a stencil (st_) or runtime (rt_).
    const char *names[4096];
    int nnames = 0;
    for (size_t s = 0; s < nsyms; s++)
    {
        const Sym *sym = &syms[s];
        if ((sym->info & 0xf) == 2 /* STT_FUNC */ && sym->shndx && sym->shndx < h->shnum)
        {
            if (nnames == 4096)
            {
                fail("too many functions", NULL);
            }
            const char *name = strtab + sym->name;
            if (strncmp(name, "st_", 3) != 0 && strncmp(name, "rt_", 3) != 0)
            {
                fail("a function that is neither a stencil (st_) nor runtime (rt_)", name);
            }
            names[nnames++] = name;
        }
    }
    qsort(names, (size_t)nnames, sizeof(names[0]), by_name);

    // The ids, shared by every target.
    uint64_t set_hash = 1469598103934665603ull;
    for (int i = 0; i < nnames; i++)
    {
        for (const char *c = names[i]; *c; c++)
        {
            set_hash = (set_hash ^ (unsigned char)*c) * 1099511628211ull;
        }
        set_hash = (set_hash ^ ',') * 1099511628211ull;
    }
    printf("#ifndef STENCIL_IDS_DEFINED\n#define STENCIL_IDS_DEFINED\nenum\n{\n");
    for (int i = 0; i < nnames; i++)
    {
        if (names[i][0] == 'r')
        {
            printf("    RT_%s,\n", names[i] + 3);
        }
    }
    printf("    RT__COUNT\n};\n\nenum\n{\n");
    for (int i = 0; i < nnames; i++)
    {
        if (names[i][0] == 's')
        {
            printf("    ST_%s,\n", names[i] + 3);
        }
    }
    printf("    ST__COUNT\n};\n#endif\n\n#define STENCIL_SET_HASH 0x%016llxull\n\n",
           (unsigned long long)set_hash);
    printf("#ifndef STENCIL_IDS_ONLY\n\n");

    for (int f = 0; f < nnames; f++)
    {
        const char *name = names[f];
        const Sym *sym = NULL;
        for (size_t s = 0; s < nsyms && !sym; s++)
        {
            if ((syms[s].info & 0xf) == 2 && strcmp(strtab + syms[s].name, name) == 0)
            {
                sym = &syms[s];
            }
        }
        const Shdr *text = section(h, sym->shndx);
        if (sym->value != 0 || sym->size != text->size)
        {
            fail("a function that does not have its section to itself (use -ffunction-sections)",
                 name);
        }
        const Shdr *rela = NULL;
        for (unsigned i = 0; i < h->shnum; i++)
        {
            if (section(h, i)->type == SHT_RELA && section(h, i)->info == sym->shndx)
            {
                rela = section(h, i);
            }
        }

        const unsigned char *code = file + text->offset;
        size_t size = text->size;
        if (size > 65536 || (aarch64 && size % 4))
        {
            fail("a function of an unexpected size", name);
        }

        size_t nrel = rela ? rela->size / sizeof(Rela) : 0;
        static RelInfo rel[1024];
        if (nrel > 1024)
        {
            fail("too many relocations", name);
        }
        for (size_t r = 0; r < nrel; r++)
        {
            const Rela *re = (const Rela *)(file + rela->offset) + r;
            uint32_t type = (uint32_t)re->info;
            const Sym *target = &syms[re->info >> 32];
            const char *tname = strtab + target->name;
            int k = -1;
            for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
            {
                if (kinds[i].elf == type)
                {
                    k = (int)i;
                }
            }
            if (k < 0)
            {
                char detail[128];
                snprintf(detail, sizeof(detail), "type %u in %s", type, name);
                fail("a relocation the backend cannot fill", detail);
            }
            if ((target->info & 0xf) == 3 /* STT_SECTION */ || (target->shndx && !*tname))
            {
                fail("a reference to data (constants and strings are not allowed in stencils)", name);
            }
            if (re->addend != 0 && !kinds[k].addend_ok)
            {
                fail("a relocation with an addend", name);
            }
            int hole = 0;
            if (strcmp(tname, "HOLE_A") == 0 || strcmp(tname, "HOLE_B") == 0 ||
                strcmp(tname, "HOLE_C") == 0)
            {
                snprintf(rel[r].what, sizeof(rel[r].what), "ST_HOLE_%c", tname[5]);
                hole = 1;
            }
            else if (strcmp(tname, "CONTINUE") == 0 || strcmp(tname, "JUMP") == 0 ||
                     strcmp(tname, "CALLEE") == 0)
            {
                snprintf(rel[r].what, sizeof(rel[r].what), "ST_%s", tname);
            }
            else if (strncmp(tname, "rt_", 3) == 0)
            {
                snprintf(rel[r].what, sizeof(rel[r].what), "ST_RUNTIME + RT_%s", tname + 3);
            }
            else if (!target->shndx)
            {
                snprintf(rel[r].what, sizeof(rel[r].what), "ST_EXTERN + %d", extern_index(tname));
            }
            else
            {
                fail("a call to a function that is not runtime (rt_)", tname);
            }
            if (hole && kinds[k].addend_ok)
            {
                fail("a hole used as a pc-relative value", name);
            }
            rel[r].offset = re->offset;
            rel[r].kind = kinds[k].kind;
            rel[r].addend = re->addend;
            rel[r].high = hole && aarch64 && type != 264; // G1, G2, G3 (G0_NC is 264)
        }

        static uint8_t compact[65536];
        static uint32_t new_at[16385];
        size_t csize = aarch64 ? compact_arm64(name, code, size, rel, nrel, compact, new_at) : size;

        print_bytes("code", name, code, size);
        if (nrel)
        {
            printf("static const StencilReloc relocs_%s[] = {\n", name);
            for (size_t r = 0; r < nrel; r++)
            {
                printf("    {%llu, %s, %lld, %s},\n", (unsigned long long)rel[r].offset, rel[r].kind,
                       (long long)rel[r].addend, rel[r].what);
            }
            printf("};\n");
        }
        size_t ncrel = 0;
        if (csize != size)
        {
            print_bytes("ccode", name, compact, csize);
            printf("static const StencilReloc crelocs_%s[] = {\n", name);
            for (size_t r = 0; r < nrel; r++)
            {
                if (!rel[r].high)
                {
                    printf("    {%u, %s, %lld, %s},\n", new_at[rel[r].offset / 4], rel[r].kind,
                           (long long)rel[r].addend, rel[r].what);
                    ncrel++;
                }
            }
            printf("};\n");
        }
        char full[256], small[256];
        if (nrel)
        {
            snprintf(full, sizeof(full), "code_%s, %zu, %zu, relocs_%s", name, size, nrel, name);
        }
        else
        {
            snprintf(full, sizeof(full), "code_%s, %zu, 0, NULL", name, size);
        }
        if (csize != size)
        {
            snprintf(small, sizeof(small), "ccode_%s, %zu, %zu, crelocs_%s", name, csize, ncrel,
                     name);
        }
        else
        {
            snprintf(small, sizeof(small), "%s", full);
        }
        printf("#define STENCIL_%s {%s, %s}\n\n", name, full, small);
    }

    printf("static const Stencil runtime_stencils[RT__COUNT] = {\n");
    for (int i = 0; i < nnames; i++)
    {
        if (names[i][0] == 'r')
        {
            printf("    [RT_%s] = STENCIL_%s,\n", names[i] + 3, names[i]);
        }
    }
    printf("};\n\nstatic const Stencil stencils[ST__COUNT] = {\n");
    for (int i = 0; i < nnames; i++)
    {
        if (names[i][0] == 's')
        {
            printf("    [ST_%s] = STENCIL_%s,\n", names[i] + 3, names[i]);
        }
    }
    printf("};\n\nstatic const char *const stencil_externs[] = {");
    for (int i = 0; i < nexterns; i++)
    {
        printf("\"%s\", ", externs[i]);
    }
    printf("NULL};\n\n#endif // STENCIL_IDS_ONLY\n");
    return 0;
}
