// elfrun.c (v2 tool): runs a Linux executable written by the native backend on macOS, for tests.
//
// It loads the file the way the kernel does (the PT_LOAD segment, then a jump to e_entry with
// an aligned stack) and replaces only the two Linux system calls, which macOS does not have:
// the write(2) stub becomes a jump to the host's write, and exit_group in _start a jump to the
// host's _exit. Everything else, the header, the segment, every branch the backend patched and
// _start itself, runs as written. Build it for the CPU of the file: arm64 natively, x86-64
// with `-arch x86_64` (it then runs under Rosetta).
//
//   elfrun [--device ADDRESS SIZE] [--ram ADDRESS SIZE] FILE
//
// A bare-metal image (`--target arm64-none`) runs too: --ram maps the memory map's RAM, where
// its _start puts the stack, and the halt loop after main (wfi / hlt) ends the run with main's
// result as the exit status.
//
// --device maps SIZE bytes of memory at ADDRESS for the program's memory-mapped registers
// (`uart0: pl011 @ ADDRESS`), each byte set to its offset (so a register reads as its own
// offset pattern), and prints them when the program ends: what it stored, at what width.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__aarch64__) || defined(__arm64__)
#include <libkern/OSCacheControl.h>
#endif

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint8_t *find(uint8_t *hay, size_t n, const void *needle, size_t k)
{
    for (size_t i = 0; i + k <= n; i++)
    {
        if (memcmp(hay + i, needle, k) == 0)
        {
            return hay + i;
        }
    }
    return NULL;
}

static void *map_at(unsigned long long at, size_t size, size_t below)
{
    size_t span = (size + below + 0x3fff) & ~(size_t)0x3fff;
    void *want = (void *)(uintptr_t)((at - below) & ~(unsigned long long)0x3fff);
    uint8_t *got = mmap(want, span + 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return got == want ? got + (at - (uintptr_t)want) : NULL;
}

static uint8_t *device;
static size_t device_size;
static unsigned long long device_at;

// In place of exit_group: the device's bytes after the program's own output, then the exit.
static void finish(int status)
{
    if (device)
    {
        printf("device 0x%llx:", device_at);
        for (size_t i = 0; i < device_size; i++)
        {
            printf("%s%02x", i % 16 ? " " : "\n  ", device[i]);
        }
        printf("\n");
        fflush(stdout);
    }
    _exit(status);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ram") == 0 && i + 2 < argc)
        {
            unsigned long long at = strtoull(argv[i + 1], NULL, 0);
            size_t size = (size_t)strtoull(argv[i + 2], NULL, 0);
            // 64 KB below it for this program's own code at the end (printing the device)
            if (!map_at(at, size, 0x10000))
            {
                fprintf(stderr, "elfrun: cannot map the RAM at 0x%llx\n", at);
                return 2;
            }
            i += 2;
        }
        else if (strcmp(argv[i], "--device") == 0 && i + 2 < argc)
        {
            device_at = strtoull(argv[i + 1], NULL, 0);
            device_size = (size_t)strtoull(argv[i + 2], NULL, 0);
            i += 2;
        }
        else
        {
            path = argv[i];
        }
    }
    if (!path)
    {
        fprintf(stderr, "usage: elfrun [--device ADDRESS SIZE] FILE\n");
        return 2;
    }
    if (device_size)
    {
        device = map_at(device_at, device_size, 0);
        if (!device)
        {
            fprintf(stderr, "elfrun: cannot map the device at 0x%llx\n", device_at);
            return 2;
        }
        for (size_t i = 0; i < device_size; i++)
        {
            device[i] = (uint8_t)i;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        perror(path);
        return 2;
    }
    static uint8_t file[1 << 24];
    size_t len = fread(file, 1, sizeof(file), f);
    fclose(f);
    if (len < 0xb0 || memcmp(file, "\x7f" "ELF", 4) != 0 || file[4] != 2 || rd16(file + 16) != 2)
    {
        fprintf(stderr, "elfrun: not a 64-bit ELF executable\n");
        return 2;
    }
    uint16_t machine = rd16(file + 18);
#if defined(__aarch64__) || defined(__arm64__)
    if (machine != 183)
#else
    if (machine != 62)
#endif
    {
        fprintf(stderr, "elfrun: the file is for another CPU (e_machine %u)\n", machine);
        return 2;
    }
    uint64_t entry = rd64(file + 24), phoff = rd64(file + 32);
    uint16_t phnum = rd16(file + 56);
    const uint8_t *load = NULL;
    for (uint16_t i = 0; i < phnum; i++)
    {
        const uint8_t *ph = file + phoff + 56u * i;
        if (ph[0] == 1 && !load)
        {
            load = ph;
        }
    }
    if (!load)
    {
        fprintf(stderr, "elfrun: no PT_LOAD segment\n");
        return 2;
    }
    uint64_t offset = rd64(load + 8);
    uint64_t vaddr = rd64(load + 16), filesz = rd64(load + 32), memsz = rd64(load + 40);
    if (offset + filesz > len || memsz < filesz || entry < vaddr || entry >= vaddr + filesz)
    {
        fprintf(stderr, "elfrun: the segment or the entry point is outside the file\n");
        return 2;
    }

    // The code is position-independent: any address will do. Room after it for two jumps.
    size_t size = (memsz + 64 + 0x3fff) & ~(size_t)0x3fff;
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED)
    {
        perror("mmap");
        return 2;
    }
    memcpy(mem, file + offset, filesz);
    uint8_t *tramp = mem + ((memsz + 15) & ~(uint64_t)15);

#if defined(__aarch64__) || defined(__arm64__)
    static const uint32_t sys_write[] = {0xd2800808u, 0xd4000001u, 0xd65f03c0u};
    static const uint32_t sys_exit[] = {0xd2800bc8u, 0xd4000001u};
    static const uint32_t halt[] = {0xd503207fu, 0x17ffffffu}; // wfi; b .-4 (bare metal)
    uint8_t *w = find(mem, filesz, sys_write, sizeof(sys_write));
    uint8_t *x = find(mem, filesz, sys_exit, sizeof(sys_exit));
    uint8_t *h = find(mem, filesz, halt, sizeof(halt));
    if (!(w && x) && !h)
    {
        fprintf(stderr, "elfrun: neither the Linux system layer nor a bare-metal halt was found\n");
        return 2;
    }
    // ldr x16, #8; br x16; .quad target  (once or twice)
    void *targets[2] = {(void *)write, (void *)finish};
    uint8_t *at[2] = {w, x ? x + 4 : NULL};
    if (!(w && x)) // bare: only the halt, which becomes the end of the run (w0: main's result)
    {
        targets[0] = (void *)finish;
        at[0] = h;
        at[1] = NULL;
    }
    for (int k = 0; k < 2 && at[k]; k++)
    {
        uint8_t *t = tramp + 16 * k;
        uint32_t code[2] = {0x58000050u, 0xd61f0200u};
        memcpy(t, code, 8);
        memcpy(t + 8, &targets[k], 8);
        uint32_t b = 0x14000000u | ((uint32_t)((t - at[k]) / 4) & 0x03ffffffu);
        memcpy(at[k], &b, 4);
    }
#else
    static const uint8_t sys_write[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc3};
    static const uint8_t sys_exit[] = {0x89, 0xc7, 0xb8, 0xe7, 0x00, 0x00, 0x00, 0x0f, 0x05};
    static const uint8_t halt[] = {0x89, 0xc7, 0xf4, 0xeb, 0xfd}; // mov edi, eax; hlt; jmp .-1
    uint8_t *w = find(mem, filesz, sys_write, sizeof(sys_write));
    uint8_t *x = find(mem, filesz, sys_exit, sizeof(sys_exit));
    uint8_t *h = find(mem, filesz, halt, sizeof(halt));
    if (!(w && x) && !h)
    {
        fprintf(stderr, "elfrun: neither the Linux system layer nor a bare-metal halt was found\n");
        return 2;
    }
    // movabs rax, target; jmp rax  (once or twice); the jumps replace the instruction that
    // sets eax. Bare: the halt's `mov edi, eax` is replaced, so the trampoline does it first.
    void *targets[2] = {(void *)write, (void *)finish};
    uint8_t *at[2] = {w, x ? x + 2 : NULL};
    int bare = !(w && x);
    if (bare)
    {
        targets[0] = (void *)finish;
        at[0] = h;
        at[1] = NULL;
    }
    for (int k = 0; k < 2 && at[k]; k++)
    {
        uint8_t *t = tramp + 16 * k;
        if (bare)
        {
            *t++ = 0x89; // mov edi, eax
            *t++ = 0xc7;
        }
        t[0] = 0x48;
        t[1] = 0xb8;
        memcpy(t + 2, &targets[k], 8);
        t[10] = 0xff;
        t[11] = 0xe0;
        t -= bare ? 2 : 0;
        int32_t rel = (int32_t)(t - (at[k] + 5));
        at[k][0] = 0xe9;
        memcpy(at[k] + 1, &rel, 4);
    }
#endif
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0)
    {
        perror("mprotect");
        return 2;
    }
#if defined(__aarch64__) || defined(__arm64__)
    sys_icache_invalidate(mem, size);
#endif
    fflush(stdout);
    void *start = mem + (entry - vaddr);
    // As the kernel starts a process: a 16-byte aligned stack, no return address.
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("mov x9, sp\n and x9, x9, #-16\n mov sp, x9\n br %0" ::"r"(start) : "x9");
#else
    __asm__ volatile("and $-16, %%rsp\n jmp *%0" ::"r"(start));
#endif
    return 0;
}
