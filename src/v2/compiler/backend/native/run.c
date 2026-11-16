// run.c (v2): `--run` without a file. The program is run in the compiler's own process, the
// way TCC's `-run` does.
//
// Writing an executable and starting it costs ~110 ms on macOS every time, because each new
// file is a vnode the system has not seen: its signature is validated and its security checks
// run on first launch (docs/p.md §2.2). The compiler spends ~0.5 ms. The code the native backend
// makes is already position-independent bytes, so instead:
//
//   - the code is copied into fresh read-write memory, followed by one stub per C library
//     function it calls (the calls the Mach-O writer sends to its GOT stubs are sent to these),
//     each stub jumping to the address of that function in this process (dlsym);
//   - the memory is made read-execute (and the instruction cache invalidated on arm64);
//   - the entry, `int main(void)` as in the executable, is called on a new thread whose stack is
//     the size a new process's main thread gets (RLIMIT_STACK), so a deeply recursive program
//     fails or not exactly as its executable would;
//   - a fault in the program (a stack overflow, x86-64's divide by zero) ends the compiler with
//     status 1, which is what `--run` gave for a program stopped by a signal.
//
// The program's output goes through write(1, ...) as in the executable; its buffer is its own,
// on its stack, so nothing of the compiler's is mixed in (the driver flushes before calling).
#include "v2/compiler/backend/native/run.h"
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__aarch64__) || defined(__arm64__)
#include <libkern/OSCacheControl.h>
#define HOST_CPU MACHO_ARM64
#elif defined(__x86_64__)
#define HOST_CPU MACHO_X86_64
#endif

int run_host_can(MachoCpu cpu)
{
#ifdef HOST_CPU
    return cpu == HOST_CPU;
#else
    (void)cpu;
    return 0;
#endif
}

#ifdef HOST_CPU

#define STUB 16 // bytes per import stub, both CPUs (an address in it is 8-byte aligned)

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

static void fault(int sig)
{
    (void)sig;
    _exit(1);
}

typedef struct
{
    int (*entry)(void);
    int status;
} Start;

static void *start(void *arg)
{
    Start *s = arg;
    // Faults are handled on their own stack: a stack overflow leaves none to run a handler on.
    // Both are put back when the program returns: the compiler goes on (--time, --stats).
    static uint8_t alt[64 * 1024];
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    enum
    {
        NSIGS = sizeof(sigs) / sizeof(sigs[0])
    };
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof(alt), .ss_flags = 0}, old_ss;
    sigaltstack(&ss, &old_ss);
    struct sigaction sa, old[NSIGS];
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fault;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < NSIGS; i++)
    {
        sigaction(sigs[i], &sa, &old[i]);
    }
    s->status = s->entry() & 0xff; // what a parent sees of exit(main())
    for (int i = 0; i < NSIGS; i++)
    {
        sigaction(sigs[i], &old[i], NULL);
    }
    sigaltstack(&old_ss, NULL);
    return NULL;
}

int run_in_memory(MachoCpu cpu, const uint8_t *code, size_t len, size_t entry,
                  const MachoImportCall *calls, size_t ncalls, const char *const *imports)
{
    if (cpu != HOST_CPU)
    {
        return -1;
    }
    size_t nimports = 0;
    while (imports && imports[nimports])
    {
        nimports++;
    }
    size_t stubs = (len + 15) & ~(size_t)15;
    size_t page = (size_t)getpagesize();
    size_t size = (stubs + STUB * nimports + page - 1) & ~(page - 1);
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED)
    {
        return -1;
    }
    memcpy(mem, code, len);

    for (size_t i = 0; i < nimports; i++)
    {
        void *fn = dlsym(RTLD_DEFAULT, imports[i]);
        if (!fn)
        {
            munmap(mem, size);
            return -1;
        }
        uint8_t *s = mem + stubs + STUB * i;
        uint64_t addr = (uint64_t)(uintptr_t)fn;
#if defined(__aarch64__) || defined(__arm64__)
        set32(s, 0x58000050u);     // ldr x16, #8
        set32(s + 4, 0xd61f0200u); // br x16
        memcpy(s + 8, &addr, 8);
#else
        s[0] = 0xff; // jmp *0(%rip): the address right after it
        s[1] = 0x25;
        set32(s + 2, 0);
        memcpy(s + 6, &addr, 8);
#endif
    }
    for (size_t i = 0; i < ncalls; i++)
    {
        size_t at = calls[i].at, target = stubs + STUB * calls[i].sym;
#if defined(__aarch64__) || defined(__arm64__)
        int64_t delta = ((int64_t)target - (int64_t)at) / 4; // bl
        set32(mem + at, (get32(mem + at) & 0xfc000000u) | ((uint32_t)delta & 0x03ffffffu));
#else
        set32(mem + at, (uint32_t)(int32_t)((int64_t)target - (int64_t)(at + 4))); // call rel32
#endif
    }
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0)
    {
        munmap(mem, size);
        return -1;
    }
#if defined(__aarch64__) || defined(__arm64__)
    sys_icache_invalidate(mem, size);
#endif

    // The stack a new process's main thread would get.
    struct rlimit rl;
    size_t stack = 8u << 20;
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur > 0)
    {
        stack = (size_t)rl.rlim_cur;
    }
    stack = (stack + page - 1) & ~(page - 1);
    Start s = {(int (*)(void))(void *)(mem + entry), 1};
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    int failed = pthread_create(&t, &attr, start, &s);
    pthread_attr_destroy(&attr);
    if (failed)
    {
        munmap(mem, size);
        return -1;
    }
    pthread_join(t, NULL);
    munmap(mem, size);
    return s.status;
}

#else // a host CPU the native backend does not generate for

int run_in_memory(MachoCpu cpu, const uint8_t *code, size_t len, size_t entry,
                  const MachoImportCall *calls, size_t ncalls, const char *const *imports)
{
    (void)cpu, (void)code, (void)len, (void)entry, (void)calls, (void)ncalls, (void)imports;
    return -1;
}

#endif
