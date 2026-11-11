// macho.c (v2): Mach-O files for the native backend, written directly (Go's linker does the same).
//
//   macho_write_executable  a signed arm64 executable: no assembler, no linker
//   macho_write_object      an object file, for linking with other code (-o x.o)
//
// The executable has the code in __TEXT, one GOT slot per C library function in __DATA (bound
// by dyld through classic bind opcodes, against libSystem), a 12-byte stub per import that jumps
// through its slot, LC_MAIN for the entry, and an ad-hoc code signature: arm64 macOS runs nothing
// unsigned. The signature is a CodeDirectory with the SHA-256 of every 4 KB page before it.
#include "v2/compiler/backend/native/macho.h"
#include <stdlib.h>
#include <string.h>

// ---- SHA-256 (FIPS 180-4) -----------------------------------------------------------------------

typedef struct
{
    uint32_t h[8];
    uint64_t len;
    uint8_t block[64];
    size_t fill;
} Sha256;

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha_block(Sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
    {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
               p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    Sha256 s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                 0x5be0cd19},
                0,
                {0},
                0};
    size_t i = 0;
    for (; i + 64 <= len; i += 64)
    {
        sha_block(&s, data + i);
    }
    uint8_t tail[128] = {0};
    size_t rest = len - i;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t blocks = rest + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++)
    {
        tail[blocks * 64 - 1 - k] = (uint8_t)(bits >> (8 * k));
    }
    for (size_t b = 0; b < blocks; b++)
    {
        sha_block(&s, tail + 64 * b);
    }
    for (int k = 0; k < 8; k++)
    {
        out[4 * k] = (uint8_t)(s.h[k] >> 24);
        out[4 * k + 1] = (uint8_t)(s.h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(s.h[k] >> 8);
        out[4 * k + 3] = (uint8_t)s.h[k];
    }
}

// ---- a growable byte buffer ------------------------------------------------------------------------

typedef struct
{
    uint8_t *p;
    size_t len, cap;
} Buf;

static void need(Buf *b, size_t n)
{
    if (b->len + n > b->cap)
    {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n)
        {
            cap *= 2;
        }
        b->p = (uint8_t *)realloc(b->p, cap);
        if (!b->p)
        {
            fprintf(stderr, "fatal: out of memory while writing the executable\n");
            exit(2);
        }
        b->cap = cap;
    }
}

static void le(Buf *b, uint64_t v, int n)
{
    need(b, (size_t)n);
    for (int i = 0; i < n; i++)
    {
        b->p[b->len++] = (uint8_t)(v >> (8 * i));
    }
}

static void be(Buf *b, uint64_t v, int n)
{
    need(b, (size_t)n);
    for (int i = n - 1; i >= 0; i--)
    {
        b->p[b->len++] = (uint8_t)(v >> (8 * i));
    }
}

static void bytes(Buf *b, const void *p, size_t n)
{
    need(b, n);
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static void name16(Buf *b, const char *s)
{
    char buf[16] = {0};
    memcpy(buf, s, strlen(s) < 16 ? strlen(s) : 16);
    bytes(b, buf, 16);
}

static void pad_to(Buf *b, size_t align)
{
    while (b->len % align)
    {
        le(b, 0, 1);
    }
}

static void uleb(Buf *b, uint64_t v)
{
    do
    {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        le(b, byte | (v ? 0x80 : 0), 1);
    } while (v);
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t w;
    memcpy(&w, p, 4);
    return w;
}

static void set32(uint8_t *p, uint32_t w)
{
    memcpy(p, &w, 4);
}

static void branch26(uint8_t *code, size_t at, size_t target)
{
    int64_t delta = ((int64_t)target - (int64_t)at) / 4;
    set32(code + at, (get32(code + at) & 0xfc000000u) | ((uint32_t)delta & 0x03ffffffu));
}

static size_t count_imports(const char *const *imports)
{
    size_t n = 0;
    while (imports[n])
    {
        n++;
    }
    return n;
}

static void segment(Buf *b, const char *name, uint64_t vmaddr, uint64_t vmsize, uint64_t fileoff,
                    uint64_t filesize, uint32_t prot, uint32_t nsects)
{
    le(b, 0x19, 4); // LC_SEGMENT_64
    le(b, 72 + 80 * nsects, 4);
    name16(b, name);
    le(b, vmaddr, 8);
    le(b, vmsize, 8);
    le(b, fileoff, 8);
    le(b, filesize, 8);
    le(b, prot, 4);
    le(b, prot, 4);
    le(b, nsects, 4);
    le(b, 0, 4);
}

static void section(Buf *b, const char *sect, const char *seg, uint64_t addr, uint64_t size,
                    uint32_t offset, uint32_t align, uint32_t reloff, uint32_t nreloc, uint32_t flags)
{
    name16(b, sect);
    name16(b, seg);
    le(b, addr, 8);
    le(b, size, 8);
    le(b, offset, 4);
    le(b, align, 4);
    le(b, reloff, 4);
    le(b, nreloc, 4);
    le(b, flags, 4);
    le(b, 0, 4);
    le(b, 0, 4);
    le(b, 0, 4);
}

static void build_version(Buf *b)
{
    le(b, 0x32, 4); // LC_BUILD_VERSION: macOS 11.0
    le(b, 24, 4);
    le(b, 1, 4);
    le(b, 0x000b0000, 4);
    le(b, 0x000b0000, 4);
    le(b, 0, 4);
}

// ---- executable -----------------------------------------------------------------------------------

#define BASE 0x100000000ull
#define SIG_PAGE 4096u

static size_t round_up(size_t v, size_t a)
{
    return (v + a - 1) / a * a;
}

int macho_write_executable(FILE *out, MachoCpu cpu, const uint8_t *code, size_t len, size_t entry,
                           const MachoImportCall *calls, size_t ncalls, const char *const *imports,
                           const char *ident)
{
    size_t nimports = count_imports(imports);
    int arm = cpu == MACHO_ARM64;
    const size_t PAGE = arm ? 0x4000u : 0x1000u; // segments are page-aligned: 16 KB on arm64
    const size_t stub = arm ? 12 : 6;
    const char *dylinker = "/usr/lib/dyld";
    const char *libsystem = "/usr/lib/libSystem.B.dylib";
    uint32_t dylinker_cmd = (uint32_t)round_up(12 + strlen(dylinker) + 1, 8);
    uint32_t dylib_cmd = (uint32_t)round_up(24 + strlen(libsystem) + 1, 8);
    uint32_t cmds = (72) + (72 + 80) + (72 + 80) + (72) /* segments */ + 48 /* dyld info */ +
                    24 /* symtab */ + 80 /* dysymtab */ + dylinker_cmd + 24 /* uuid */ +
                    24 /* build version */ + 24 /* main */ + dylib_cmd + 16 /* code signature */;
    uint32_t ncmds = 13;

    // __TEXT: the headers, then the code, then one stub per import.
    size_t text_off = round_up(32 + cmds, 16);
    size_t stubs_off = text_off + round_up(len, 4);
    size_t text_size = stubs_off + stub * nimports - text_off;
    size_t text_seg = round_up(text_off + text_size, PAGE);
    // __DATA: the GOT, one 8-byte slot per import. It is zero until dyld binds it, so it is a
    // zero-fill segment: memory when loaded, no bytes in the file (a page less per binary).
    size_t data_off = text_seg; // an address offset, not a file offset
    size_t data_size = nimports ? 8 * nimports : 8;
    size_t data_seg = round_up(data_size, PAGE);
    // __LINKEDIT: bind opcodes, an empty symbol table, then the signature. In the file it
    // follows __TEXT directly; in memory it follows __DATA.
    size_t link_off = text_seg;
    size_t link_vm = data_off + data_seg;

    Buf bind = {0};
    for (size_t i = 0; i < nimports; i++)
    {
        le(&bind, 0x10 | 1, 1); // SET_DYLIB_ORDINAL_IMM: libSystem
        le(&bind, 0x40, 1);     // SET_SYMBOL_TRAILING_FLAGS_IMM
        le(&bind, '_', 1);
        bytes(&bind, imports[i], strlen(imports[i]) + 1);
        le(&bind, 0x50 | 1, 1); // SET_TYPE_IMM: pointer
        le(&bind, 0x70 | 2, 1); // SET_SEGMENT_AND_OFFSET_ULEB: segment 2 (__DATA)
        uleb(&bind, 8 * i);
        le(&bind, 0x90, 1); // DO_BIND
    }
    le(&bind, 0x00, 1); // DONE
    pad_to(&bind, 8);
    size_t bind_off = link_off, sym_off = bind_off + bind.len, str_off = sym_off;
    size_t str_size = 8; // " \0" padded: an empty string table
    size_t sig_off = round_up(str_off + str_size, 16);

    size_t ident_len = strlen(ident) + 1;
    size_t ncode = (sig_off + SIG_PAGE - 1) / SIG_PAGE;
    size_t cd_header = 88;
    size_t cd_size = cd_header + ident_len + 32 * ncode;
    size_t sig_size = round_up(12 + 8 + cd_size, 16);
    size_t link_size = sig_off + sig_size - link_off;

    Buf b = {0};
    // mach_header_64
    le(&b, 0xfeedfacf, 4);
    le(&b, arm ? 0x0100000c : 0x01000007, 4); // CPU_TYPE_ARM64 / CPU_TYPE_X86_64
    le(&b, arm ? 0 : 3, 4);                   // CPU_SUBTYPE_ARM64_ALL / CPU_SUBTYPE_X86_64_ALL
    le(&b, 2, 4); // MH_EXECUTE
    le(&b, ncmds, 4);
    le(&b, cmds, 4);
    le(&b, 0x00200085, 4); // MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_PIE
    le(&b, 0, 4);

    segment(&b, "__PAGEZERO", 0, BASE, 0, 0, 0, 0);
    segment(&b, "__TEXT", BASE, text_seg, 0, text_seg, 5, 1);
    section(&b, "__text", "__TEXT", BASE + text_off, text_size, (uint32_t)text_off, 2, 0, 0,
            0x80000400);
    segment(&b, "__DATA", BASE + data_off, data_seg, 0, 0, 3, 1);
    section(&b, "__got", "__DATA", BASE + data_off, data_size, 0, 3, 0, 0, 0x1); // S_ZEROFILL
    segment(&b, "__LINKEDIT", BASE + link_vm, round_up(link_size, PAGE), link_off, link_size, 1, 0);

    le(&b, 0x80000022, 4); // LC_DYLD_INFO_ONLY
    le(&b, 48, 4);
    le(&b, 0, 4); // rebase
    le(&b, 0, 4);
    le(&b, bind_off, 4);
    le(&b, bind.len, 4);
    le(&b, 0, 4); // weak bind
    le(&b, 0, 4);
    le(&b, 0, 4); // lazy bind
    le(&b, 0, 4);
    le(&b, 0, 4); // export
    le(&b, 0, 4);

    le(&b, 0x2, 4); // LC_SYMTAB: no symbols
    le(&b, 24, 4);
    le(&b, sym_off, 4);
    le(&b, 0, 4);
    le(&b, str_off, 4);
    le(&b, str_size, 4);

    le(&b, 0xb, 4); // LC_DYSYMTAB: empty
    le(&b, 80, 4);
    for (int i = 0; i < 18; i++)
    {
        le(&b, 0, 4);
    }

    le(&b, 0xe, 4); // LC_LOAD_DYLINKER
    le(&b, dylinker_cmd, 4);
    le(&b, 12, 4);
    size_t start = b.len;
    bytes(&b, dylinker, strlen(dylinker) + 1);
    while (b.len - start < dylinker_cmd - 12)
    {
        le(&b, 0, 1);
    }

    le(&b, 0x1b, 4); // LC_UUID: derived from the code, so a build is reproducible
    le(&b, 24, 4);
    uint8_t digest[32];
    sha256(code, len, digest);
    digest[6] = (digest[6] & 0x0f) | 0x40;
    digest[8] = (digest[8] & 0x3f) | 0x80;
    bytes(&b, digest, 16);

    build_version(&b);

    le(&b, 0x80000028, 4); // LC_MAIN
    le(&b, 24, 4);
    le(&b, text_off + entry, 8);
    le(&b, 0, 8);

    le(&b, 0xc, 4); // LC_LOAD_DYLIB: libSystem
    le(&b, dylib_cmd, 4);
    le(&b, 24, 4);
    le(&b, 2, 4);
    le(&b, 0x05276403, 4); // current version 1319.100.3
    le(&b, 0x00010000, 4); // compatibility version 1.0
    start = b.len;
    bytes(&b, libsystem, strlen(libsystem) + 1);
    while (b.len - start < dylib_cmd - 24)
    {
        le(&b, 0, 1);
    }

    le(&b, 0x1d, 4); // LC_CODE_SIGNATURE
    le(&b, 16, 4);
    le(&b, sig_off, 4);
    le(&b, sig_size, 4);

    if (b.len != 32 + cmds)
    {
        fprintf(stderr, "fatal: backend native: Mach-O load commands are %zu bytes, not %u\n",
                b.len - 32, cmds);
        exit(2);
    }

    // __TEXT contents: the code, its calls into the C library sent to the stubs, the stubs.
    pad_to(&b, 16);
    while (b.len < text_off)
    {
        le(&b, 0, 1);
    }
    bytes(&b, code, len);
    pad_to(&b, 4);
    for (size_t i = 0; i < ncalls; i++)
    {
        size_t target = stubs_off - text_off + stub * calls[i].sym;
        if (arm)
        {
            branch26(b.p + text_off, calls[i].at, target);
        }
        else
        {
            set32(b.p + text_off + calls[i].at, (uint32_t)(int32_t)((int64_t)target - (int64_t)(calls[i].at + 4)));
        }
    }
    for (size_t i = 0; i < nimports && !arm; i++)
    {
        // jmp *slot(%rip)
        int64_t rel = (int64_t)(data_off + 8 * i) - (int64_t)(stubs_off + stub * i + 6);
        le(&b, 0xff, 1);
        le(&b, 0x25, 1);
        le(&b, (uint32_t)(int32_t)rel, 4);
    }
    for (size_t i = 0; i < nimports && arm; i++)
    {
        uint64_t pc = BASE + stubs_off + 12 * i;
        uint64_t slot = BASE + data_off + 8 * i;
        int64_t pages = (int64_t)(slot >> 12) - (int64_t)(pc >> 12);
        uint32_t lo = (uint32_t)(slot & 0xfff);
        le(&b, 0x90000000u | (((uint32_t)pages & 3u) << 29) | ((((uint32_t)pages >> 2) & 0x7ffffu) << 5) | 16u,
           4);                                                // adrp x16, slot@page
        le(&b, 0xf9400000u | ((lo / 8) << 10) | (16u << 5) | 16u, 4); // ldr x16, [x16, slot@pageoff]
        le(&b, 0xd61f0200u, 4);                               // br x16
    }
    while (b.len < link_off) // __TEXT is padded to its page; __DATA has no file bytes
    {
        le(&b, 0, 1);
    }
    // __LINKEDIT
    bytes(&b, bind.p, bind.len);
    le(&b, ' ', 1);
    while (b.len < str_off + str_size)
    {
        le(&b, 0, 1);
    }
    while (b.len < sig_off)
    {
        le(&b, 0, 1);
    }

    // The ad-hoc signature: a SuperBlob holding one CodeDirectory (all fields big-endian).
    size_t code_limit = sig_off;
    Buf sig = {0};
    be(&sig, 0xfade0cc0, 4); // CSMAGIC_EMBEDDED_SIGNATURE
    be(&sig, sig_size, 4);
    be(&sig, 1, 4);
    be(&sig, 0, 4); // CSSLOT_CODEDIRECTORY
    be(&sig, 20, 4);
    be(&sig, 0xfade0c02, 4); // CSMAGIC_CODEDIRECTORY
    be(&sig, cd_size, 4);
    be(&sig, 0x20400, 4);                    // version: with exec segment fields
    be(&sig, 0x20002, 4);                    // CS_ADHOC | CS_LINKER_SIGNED
    be(&sig, cd_header + ident_len, 4);      // hashOffset
    be(&sig, cd_header, 4);                  // identOffset
    be(&sig, 0, 4);                          // nSpecialSlots
    be(&sig, ncode, 4);                      // nCodeSlots
    be(&sig, code_limit, 4);                 // codeLimit
    be(&sig, 32, 1);                         // hashSize
    be(&sig, 2, 1);                          // hashType: SHA-256
    be(&sig, 0, 1);                          // platform
    be(&sig, 12, 1);                         // pageSize: 2^12
    be(&sig, 0, 4);                          // spare2
    be(&sig, 0, 4);                          // scatterOffset
    be(&sig, 0, 4);                          // teamOffset
    be(&sig, 0, 4);                          // spare3
    be(&sig, 0, 8);                          // codeLimit64
    be(&sig, 0, 8);                          // execSegBase
    be(&sig, text_seg, 8);                   // execSegLimit
    be(&sig, 1, 8);                          // CS_EXECSEG_MAIN_BINARY
    bytes(&sig, ident, ident_len);
    for (size_t i = 0; i < ncode; i++)
    {
        size_t from = i * SIG_PAGE;
        size_t n = code_limit - from < SIG_PAGE ? code_limit - from : SIG_PAGE;
        uint8_t h[32];
        sha256(b.p + from, n, h);
        bytes(&sig, h, 32);
    }
    while (sig.len < sig_size)
    {
        le(&sig, 0, 1);
    }
    bytes(&b, sig.p, sig.len);

    int ok = fwrite(b.p, 1, b.len, out) == b.len;
    free(b.p);
    free(bind.p);
    free(sig.p);
    return ok ? 0 : 1;
}

// ---- object file --------------------------------------------------------------------------------

int macho_write_object(FILE *out, MachoCpu cpu, const uint8_t *code, size_t len, size_t entry,
                       const MachoImportCall *calls, size_t ncalls, const char *const *imports)
{
    size_t nimports = count_imports(imports);
    size_t text = round_up(len, 8);
    Buf str = {0};
    le(&str, 0, 1);
    bytes(&str, "_main", 6);
    uint32_t *undef_str = (uint32_t *)calloc(nimports + 1, sizeof(uint32_t));
    for (size_t i = 0; i < nimports; i++)
    {
        undef_str[i] = (uint32_t)str.len;
        le(&str, '_', 1);
        bytes(&str, imports[i], strlen(imports[i]) + 1);
    }
    pad_to(&str, 8);

    const uint32_t cmds = (72 + 80) + 24 + 24 + 80;
    size_t text_off = round_up(32 + cmds, 16);
    size_t reloc_off = text_off + text;
    size_t sym_off = reloc_off + ncalls * 8;
    size_t nsyms = 1 + nimports;
    size_t str_off = sym_off + nsyms * 16;

    Buf b = {0};
    le(&b, 0xfeedfacf, 4);
    le(&b, cpu == MACHO_ARM64 ? 0x0100000c : 0x01000007, 4);
    le(&b, cpu == MACHO_ARM64 ? 0 : 3, 4);
    le(&b, 1, 4); // MH_OBJECT
    le(&b, 4, 4);
    le(&b, cmds, 4);
    le(&b, 0, 4);
    le(&b, 0, 4);
    segment(&b, "", 0, text, text_off, text, 7, 1);
    section(&b, "__text", "__TEXT", 0, text, (uint32_t)text_off, 2, (uint32_t)reloc_off,
            (uint32_t)ncalls, 0x80000400);
    build_version(&b);
    le(&b, 0x2, 4); // LC_SYMTAB
    le(&b, 24, 4);
    le(&b, sym_off, 4);
    le(&b, nsyms, 4);
    le(&b, str_off, 4);
    le(&b, str.len, 4);
    le(&b, 0xb, 4); // LC_DYSYMTAB: one external definition, then the undefined symbols
    le(&b, 80, 4);
    le(&b, 0, 4);
    le(&b, 0, 4);
    le(&b, 0, 4);
    le(&b, 1, 4);
    le(&b, 1, 4);
    le(&b, nimports, 4);
    for (int i = 0; i < 12; i++)
    {
        le(&b, 0, 4);
    }
    while (b.len < text_off)
    {
        le(&b, 0, 1);
    }
    bytes(&b, code, len);
    while (b.len < reloc_off)
    {
        le(&b, 0, 1);
    }
    for (size_t i = 0; i < ncalls; i++)
    {
        // ARM64_RELOC_BRANCH26 / X86_64_RELOC_BRANCH (both 2), pc-relative, 4 bytes, external
        le(&b, calls[i].at, 4);
        le(&b, (uint64_t)(1 + calls[i].sym) | 1u << 24 | 2u << 25 | 1u << 27 | 2u << 28, 4);
    }
    le(&b, 1, 4); // _main: defined in section 1, external
    le(&b, 0x0f, 1);
    le(&b, 1, 1);
    le(&b, 0, 2);
    le(&b, entry, 8);
    for (size_t i = 0; i < nimports; i++)
    {
        le(&b, undef_str[i], 4); // undefined, external
        le(&b, 0x01, 1);
        le(&b, 0, 1);
        le(&b, 0, 2);
        le(&b, 0, 8);
    }
    bytes(&b, str.p, str.len);
    int ok = fwrite(b.p, 1, b.len, out) == b.len;
    free(b.p);
    free(str.p);
    free(undef_str);
    return ok ? 0 : 1;
}
