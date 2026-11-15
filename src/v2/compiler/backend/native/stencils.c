// stencils.c (v2): the machine-code templates of the native backend (copy-and-patch).
//
// This file is never linked into the compiler. At build time clang compiles it for the target
// CPU into an object file, and tools/stencilgen.c turns every function in it into bytes plus a
// list of holes (stencils_<arch>.h). The backend then generates code by copying those bytes and
// filling the holes: no instruction selection, no assembler, tens of nanoseconds per operation
// (Xu & Kjolstad, "Copy-and-Patch Compilation", OOPSLA 2021; the technique of CPython's JIT).
//
// Execution model of the generated code:
//   - each Modal function has a frame of 8-byte slots; `f` (x0) points at it
//   - `c` (x1) is the runtime context: the output buffer
//   - a stencil works on frame slots and ends by tail-calling CONTINUE, which is the next
//     stencil; the backend drops that branch when the next stencil follows directly
//   - a call copies the arguments into the slots just above the caller's frame, which become
//     the callee's frame (register windows, as in Lua), and the result comes back in its slot 0
//
// Holes (filled when the stencil is copied):
//   HOLE_A..HOLE_C  64-bit immediates (slot numbers, constants, field offsets)
//   CONTINUE        the next stencil          JUMP     a branch target
//   CALLEE          a Modal function          rt_*     a runtime function (below)
// Any other symbol is a C library function, resolved by the system linker.
//
// Rules for this file: no switch (jump tables), no string literals or constant arrays (they
// would need data relocations), no static data. stencilgen rejects what it cannot patch.
#include <stdint.h>

// The runtime context. Its buffer and the value stack are sized when the program is compiled
// (backend/native/emit.c, memory budgets): exactly what the program needs when that is known.
typedef uint64_t u64;
typedef struct Ctx
{
    uint32_t len, cap; // bytes waiting in buf, and its size
    uint8_t *buf;
    uint64_t *globals; // module-level variables: the bottom of the value stack
    u64 *jb;           // the boundary a panic goes back to (`n, e := f()`, guards.md §5), or 0
} Ctx;


extern char HOLE_A[], HOLE_B[], HOLE_C[];
extern void CONTINUE(u64 *f, Ctx *c, u64 acc);
extern void JUMP(u64 *f, Ctx *c, u64 acc);
extern void CALLEE(u64 *f, Ctx *c, u64 acc);
extern long write(int fd, const void *buf, unsigned long n);
extern void exit(int status) __attribute__((noreturn));

#define A ((u64)(uintptr_t)HOLE_A)
#define B ((u64)(uintptr_t)HOLE_B)
#define C ((u64)(uintptr_t)HOLE_C)
#define NEXT __attribute__((musttail)) return CONTINUE(f, c, acc)
// After a call out, acc is dead (no stencil that calls produces a value in it): passing 0 lets
// the stencil forget it instead of saving it across the call.
#define NEXT_FRESH __attribute__((musttail)) return CONTINUE(f, c, 0)
#define STENCIL(name) void st_##name(u64 *f, Ctx *c, __attribute__((unused)) u64 acc)
// The runtime runs once per output call, far from the hot code: compiled for size (clang's
// minsize, -Oz for these functions only), as p.md 4.5 asks.
#define RUNTIME __attribute__((noinline, minsize))

// ---- runtime: copied once into every program --------------------------------------------------

RUNTIME void rt_flush(Ctx *c)
{
    uint32_t off = 0;
    while (off < c->len)
    {
        long n = write(1, c->buf + off, c->len - off);
        if (n <= 0)
        {
            break;
        }
        off += (uint32_t)n;
    }
    c->len = 0;
}

RUNTIME void rt_put(Ctx *c, const uint8_t *p, u64 n)
{
    while (n)
    {
        if (c->len == c->cap)
        {
            rt_flush(c);
        }
        u64 room = c->cap - c->len;
        u64 k = n < room ? n : room;
        for (u64 i = 0; i < k; i++)
        {
            c->buf[c->len + i] = p[i];
        }
        c->len += (uint32_t)k;
        p += k;
        n -= k;
    }
}

// A string is its length (8 bytes) followed by its bytes.
RUNTIME void rt_write_str(Ctx *c, const uint8_t *s)
{
    u64 n;
    __builtin_memcpy(&n, s, 8);
    rt_put(c, s + 8, n);
}

// A program whose only output is constant text, with no recursion, writes each string straight
// to the system (no buffer, no flush): the backend sends rt_write_str's calls here instead and
// starts it with entry_direct. The bytes and their order are the same.
RUNTIME void rt_write_direct(Ctx *c, const uint8_t *s)
{
    (void)c;
    u64 n;
    __builtin_memcpy(&n, s, 8);
    write(1, s + 8, n);
}

RUNTIME void rt_write_uint(Ctx *c, u64 v)
{
    uint8_t tmp[24];
    int i = 24;
    do
    {
        tmp[--i] = (uint8_t)('0' + v % 10);
        v /= 10;
    } while (v);
    rt_put(c, tmp + i, (u64)(24 - i));
}

RUNTIME void rt_write_int(Ctx *c, int64_t v)
{
    if (v < 0)
    {
        uint8_t minus = '-';
        rt_put(c, &minus, 1);
        rt_write_uint(c, 0 - (u64)v);
        return;
    }
    rt_write_uint(c, (u64)v);
}

RUNTIME void rt_write_bool(Ctx *c, u64 b)
{
    uint8_t text[8];
    u64 word = b ? 0x65757274ull /* "true" */ : 0x65736c6166ull /* "false" */;
    __builtin_memcpy(text, &word, 8);
    rt_put(c, text, b ? 4 : 5);
}

// A trap (a division by zero): the output written so far goes out first, then the message
// (a length and its bytes) to the error output, and the program ends with status 1. `exit` is
// the system's on macOS, exit_group on Linux, the halt on bare metal (each system layer).
// A part of a trap's message (`trap(site, " is false")`, docs/std.md): the program's output so
// far goes out first, then this part to the error output; the trap that follows ends it.
RUNTIME void rt_trap_part(Ctx *c, const uint8_t *msg)
{
    if (c->jb)
    {
        return; // a boundary waits: the panic goes back to it, and says nothing
    }
    rt_flush(c);
    u64 n;
    __builtin_memcpy(&n, msg, 8);
    n &= 0x00ffffffffffffffull; // (the top byte: a panic's number)
    write(2, msg + 8, n);
}

// A panic: back to the boundary there is (its number is the error there), or the program stops
// with its message.
RUNTIME __attribute__((noreturn)) void rt_trap(Ctx *c, const uint8_t *msg)
{
    if (c->jb)
    {
        // back to the boundary: the registers a call keeps, the stack, then its resume point
        // (saved by guarded_call: jb[0..] as that stencil lays them out); the panic's number,
        // the top byte of the message's length word, goes in jb[14]
        u64 len;
        __builtin_memcpy(&len, msg, 8);
        c->jb[14] = len >> 56;
#if defined(__aarch64__)
        register u64 *j __asm__("x17") = c->jb;
        __asm__ volatile("ldp x19, x20, [x17, #0]\n\t"
                         "ldp x21, x22, [x17, #16]\n\t"
                         "ldp x23, x24, [x17, #32]\n\t"
                         "ldp x25, x26, [x17, #48]\n\t"
                         "ldp x27, x28, [x17, #64]\n\t"
                         "ldp x29, x30, [x17, #80]\n\t"
                         "ldr x16, [x17, #96]\n\t"
                         "mov sp, x16\n\t"
                         "ldr x16, [x17, #104]\n\t"
                         "br x16"
                         :
                         : "r"(j)
                         : "memory");
#else
        u64 *j = c->jb;
        __asm__ volatile("movq 0(%0), %%rbx\n\t"
                         "movq 8(%0), %%rbp\n\t"
                         "movq 16(%0), %%r12\n\t"
                         "movq 24(%0), %%r13\n\t"
                         "movq 32(%0), %%r14\n\t"
                         "movq 40(%0), %%r15\n\t"
                         "movq 48(%0), %%rsp\n\t"
                         "jmpq *56(%0)"
                         :
                         : "D"(j)
                         : "memory");
#endif
        __builtin_unreachable();
    }
    rt_trap_part(c, msg); // (one copy of the code: clang would otherwise outline what they share)
    exit(1);
}

// ---- entry: the C `main` of the program ---------------------------------------------------------

// The value stack starts with the module-level variables (A of them); main's frame follows.
// B: the value stack's slots; C: the output buffer's bytes (both on the native stack).
int st_entry(void)
{
    Ctx ctx;
    u64 slots = B, bytes = C;
    __asm__("" : "+r"(slots), "+r"(bytes)); // the holes stay plain values (no addend)
    u64 stack[slots];
    uint8_t out[bytes];
    ctx.len = 0;
    ctx.cap = (uint32_t)bytes;
    ctx.buf = out;
    ctx.globals = stack;
    ctx.jb = 0;
    stack[A] = 0;
    CALLEE(stack + A, &ctx, 0);
    rt_flush(&ctx);
    return (int)stack[A]; // what main returned, as the C backend's `return (int)m_main()`
}

// The entries of a program that writes directly (rt_write_direct): no output buffer, nothing to
// flush at the end. len = 0, so a trap's flush writes nothing.
int st_entry_direct(void)
{
    Ctx ctx;
    u64 slots = B;
    __asm__("" : "+r"(slots));
    u64 stack[slots];
    ctx.len = 0;
    ctx.cap = 0;
    ctx.buf = 0;
    ctx.globals = stack;
    ctx.jb = 0;
    stack[A] = 0;
    CALLEE(stack + A, &ctx, 0);
    return (int)stack[A];
}

int st_entry_void_direct(void)
{
    Ctx ctx;
    u64 slots = B;
    __asm__("" : "+r"(slots));
    u64 stack[slots];
    ctx.len = 0;
    ctx.cap = 0;
    ctx.buf = 0;
    ctx.globals = stack;
    ctx.jb = 0;
    CALLEE(stack + A, &ctx, 0);
    return 0;
}

int st_entry_void(void)
{
    Ctx ctx;
    u64 slots = B, bytes = C;
    __asm__("" : "+r"(slots), "+r"(bytes)); // the holes stay plain values (no addend)
    u64 stack[slots];
    uint8_t out[bytes];
    ctx.len = 0;
    ctx.cap = (uint32_t)bytes;
    ctx.buf = out;
    ctx.globals = stack;
    ctx.jb = 0;
    CALLEE(stack + A, &ctx, 0);
    rt_flush(&ctx);
    return 0;
}

// ---- the accumulator ----------------------------------------------------------------------------
//
// `acc` (the third argument: x2 on arm64, rdx on x86-64) carries a value from one stencil to the
// next without going through the frame. A value-producing stencil comes in four forms:
//
//   name      f[A] = op(f[B], ...)      name_r    acc  = op(f[B], ...)
//   name_a    f[A] = op(acc, ...)       name_ar   acc  = op(acc, ...)
//
// and the backend uses _r when the very next instruction is the only one to read the value, which
// then takes it in its _a form: one store, one load and one slot number less each time.
#define FOR_EACH_INT_EARLY(X)                                                                      \
    X(u8, uint8_t, u64) X(u16, uint16_t, u64) X(u32, uint32_t, u64) X(u64, uint64_t, u64)         \
    X(i8, int8_t, int64_t) X(i16, int16_t, int64_t) X(i32, int32_t, int64_t)                       \
    X(i64, int64_t, int64_t)

#define FOUR(name, EXPR)                                                                           \
    STENCIL(name) { u64 x = f[B]; f[A] = (EXPR); NEXT; }                                           \
    STENCIL(name##_r) { u64 x = f[B]; acc = (EXPR); NEXT; }                                        \
    STENCIL(name##_a) { u64 x = acc; f[A] = (EXPR); NEXT; }                                        \
    STENCIL(name##_ar) { u64 x = acc; acc = (EXPR); NEXT; }

// ---- moving values ------------------------------------------------------------------------------

STENCIL(const) // f[A] = B
{
    f[A] = B;
    NEXT;
}

STENCIL(const_r) // acc = B
{
    acc = B;
    NEXT;
}

STENCIL(const64) // f[A] = B | C << 32: a constant wider than a hole (x86-64 holes are 32 bits)
{
    f[A] = B | (C << 32);
    NEXT;
}

FOUR(copy, x) // f[A] = f[B]

// Memory is read and written in the width of the field: f[A] = *(T *)(f[B] + C), extended to 64
// bits as the slot rule says; *(T *)(f[A] + B) = f[C], truncated. A struct is its C layout.
#define LOAD_STENCILS(t, T, S) FOUR(load_##t, (u64)(S)(*(const T *)(uintptr_t)(x + C)))
FOR_EACH_INT_EARLY(LOAD_STENCILS)

#define STORE_STENCILS(n, T)                                                                       \
    STENCIL(store_##n) { *(T *)(uintptr_t)(f[A] + B) = (T)f[C]; NEXT; }                            \
    STENCIL(store_##n##_a) { *(T *)(uintptr_t)(f[A] + B) = (T)acc; NEXT; }
STORE_STENCILS(8, uint8_t)
STORE_STENCILS(16, uint16_t)
STORE_STENCILS(32, uint32_t)
STORE_STENCILS(64, uint64_t)

// ---- module-level variables ----------------------------------------------------------------------

STENCIL(gaddr) // f[A] = the address of global B (data that holds addresses)
{
    f[A] = (uint64_t)(uintptr_t)&c->globals[B];
    NEXT;
}

STENCIL(gaddr_r) // acc = the address of global B
{
    acc = (uint64_t)(uintptr_t)&c->globals[B];
    NEXT;
}

STENCIL(gload) // f[A] = global B
{
    f[A] = c->globals[B];
    NEXT;
}

STENCIL(gload_r) // acc = global B
{
    acc = c->globals[B];
    NEXT;
}

STENCIL(gstore) // global A = f[B]
{
    c->globals[A] = f[B];
    NEXT;
}

STENCIL(gstore_a) // global A = acc
{
    c->globals[A] = acc;
    NEXT;
}

STENCIL(addr) // f[A] = address of slot B
{
    f[A] = (u64)(uintptr_t)(f + B);
    NEXT;
}

STENCIL(zero) // B slots from A set to 0
{
    for (u64 i = 0; i < B; i++)
    {
        f[A + i] = 0;
    }
    NEXT;
}

// ---- arithmetic, one variant per type ---------------------------------------------------------
//
// A slot holds a value zero-extended (unsigned types) or sign-extended (signed types) to 64
// bits. + - * wrap in the type, as the C backend's casts do; / and comparisons are done in it.
// The first operand is `x` (a slot or the accumulator); the second a slot (C) or, in the
// immediate forms, the constant C itself.

#define FOR_EACH_INT FOR_EACH_INT_EARLY

#define WRAP(T, S, op, y) (u64)(S)(T)((u64)(T)x op (u64)(T)(y))
#define EXACT(T, S, op, y) (u64)(S)(T)((T)x op (T)(y))
// Division: the smallest signed value divided by -1 wraps (as `0 - x` does) instead of
// trapping on x86-64; `(T)-1 < 0` is a constant, so unsigned division has no test at all.
// A zero divisor never gets here: the MIR checks it first (MIR_TRAP).
#define DIV(T, S, y) (((T)-1 < 0 && (T)(y) == (T)-1) ? (u64)(S)(T)(0 - (u64)(T)x) : EXACT(T, S, /, y))
#define CMP(T, op, y) (u64)((T)x op (T)(y))

#define INT_STENCILS(t, T, S)                                                                      \
    FOUR(add_##t, WRAP(T, S, +, f[C]))                                                             \
    FOUR(sub_##t, WRAP(T, S, -, f[C]))                                                             \
    FOUR(mul_##t, WRAP(T, S, *, f[C]))                                                             \
    FOUR(div_##t, DIV(T, S, f[C]))                                                            \
    FOUR(eq_##t, CMP(T, ==, f[C]))                                                                 \
    FOUR(ne_##t, CMP(T, !=, f[C]))                                                                 \
    FOUR(lt_##t, CMP(T, <, f[C]))                                                                  \
    FOUR(le_##t, CMP(T, <=, f[C]))                                                                 \
    FOUR(gt_##t, CMP(T, >, f[C]))                                                                  \
    FOUR(ge_##t, CMP(T, >=, f[C]))                                                                 \
    FOUR(addi_##t, WRAP(T, S, +, C))                                                               \
    FOUR(subi_##t, WRAP(T, S, -, C))                                                               \
    FOUR(muli_##t, WRAP(T, S, *, C))                                                               \
    FOUR(divi_##t, DIV(T, S, C))                                                              \
    FOUR(eqi_##t, CMP(T, ==, C))                                                                   \
    FOUR(nei_##t, CMP(T, !=, C))                                                                   \
    FOUR(lti_##t, CMP(T, <, C))                                                                    \
    FOUR(lei_##t, CMP(T, <=, C))                                                                   \
    FOUR(gti_##t, CMP(T, >, C))                                                                    \
    FOUR(gei_##t, CMP(T, >=, C))                                                                   \
    FOUR(neg_##t, (u64)(S)(T)(0 - (u64)(T)x))                                                      \
    FOUR(norm_##t, (u64)(S)(T)x)

FOR_EACH_INT(INT_STENCILS)

FOUR(not, (u64)!x) // f[A] = !f[B]

// ---- control ----------------------------------------------------------------------------------

STENCIL(jz) // to JUMP if f[A] is 0
{
    if (!f[A])
    {
        __attribute__((musttail)) return JUMP(f, c, acc);
    }
    NEXT;
}

STENCIL(jz_a)
{
    if (!acc)
    {
        __attribute__((musttail)) return JUMP(f, c, acc);
    }
    NEXT;
}

STENCIL(jump) // to JUMP (a loop's way back: a tail call to the function itself)
{
    __attribute__((musttail)) return JUMP(f, c, acc);
}

STENCIL(jnz) // to JUMP if f[A] is not 0
{
    if (f[A])
    {
        __attribute__((musttail)) return JUMP(f, c, acc);
    }
    NEXT;
}

STENCIL(jnz_a)
{
    if (acc)
    {
        __attribute__((musttail)) return JUMP(f, c, acc);
    }
    NEXT;
}

STENCIL(call) // CALLEE with its frame at slot A of this one
{
    CALLEE(f + A, c, 0);
    NEXT_FRESH;
}

// A boundary (MIR_CALL_GUARD, `n, e := f()`): CALLEE with its frame at slot A. B >> 1: its value's
// slots; B & 1: it says `, error`. A panic below comes back here: the value is zero and the error
// slot (after the value) holds the panic's number; a callee that cannot fail leaves it 0. The
// registers the caller keeps across the call come back too: a function with __builtin_setjmp
// saves every callee-saved register, and restores them when it ends.
STENCIL(guarded_call)
{
    u64 jb[15];
    u64 *outer = c->jb;
    u64 *g = f + A;
    u64 k = B >> 1;
    u64 resumed;
    c->jb = jb;
    // The call runs inside the asm, which first saves what rt_trap restores: the registers a call
    // keeps, the stack pointer, and the resume point (label 1). The compiler sees one asm that
    // keeps those registers, as any call does; on the way back it is the same.
#if defined(__aarch64__)
    __asm__ volatile("stp x19, x20, [%[jb], #0]\n\t"
                     "stp x21, x22, [%[jb], #16]\n\t"
                     "stp x23, x24, [%[jb], #32]\n\t"
                     "stp x25, x26, [%[jb], #48]\n\t"
                     "stp x27, x28, [%[jb], #64]\n\t"
                     "stp x29, x30, [%[jb], #80]\n\t"
                     "mov x16, sp\n\t"
                     "str x16, [%[jb], #96]\n\t"
                     "adr x16, 1f\n\t"
                     "str x16, [%[jb], #104]\n\t"
                     "mov x0, %[g]\n\t"
                     "mov x1, %[c]\n\t"
                     "mov x2, #0\n\t"
                     "bl CALLEE\n\t"
                     "mov %[r], #0\n\t"
                     "b 2f\n"
                     "1:\n\t"
                     "mov %[r], #1\n"
                     "2:"
                     : [r] "=r"(resumed)
                     : [jb] "r"(jb), [g] "r"(g), [c] "r"(c)
                     : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                       "x14", "x15", "x16", "x17", "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16",
                       "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29",
                       "v30", "v31", "memory", "cc");
#else
    __asm__ volatile("movq %%rbx, 0(%[jb])\n\t"
                     "movq %%rbp, 8(%[jb])\n\t"
                     "movq %%r12, 16(%[jb])\n\t"
                     "movq %%r13, 24(%[jb])\n\t"
                     "movq %%r14, 32(%[jb])\n\t"
                     "movq %%r15, 40(%[jb])\n\t"
                     "movq %%rsp, 48(%[jb])\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "movq %%rax, 56(%[jb])\n\t"
                     "movq %[g], %%rdi\n\t"
                     "movq %[c], %%rsi\n\t"
                     "subq $128, %%rsp\n\t" // past the red zone
                     "pushq %%rbx\n\t"
                     "movq %%rsp, %%rbx\n\t"
                     "andq $-16, %%rsp\n\t"
                     "xorl %%edx, %%edx\n\t"
                     "call CALLEE\n\t"
                     "movq %%rbx, %%rsp\n\t"
                     "popq %%rbx\n\t"
                     "addq $128, %%rsp\n\t"
                     "movq $0, %[r]\n\t"
                     "jmp 2f\n"
                     "1:\n\t"
                     "movq $1, %[r]\n"
                     "2:"
                     : [r] "=r"(resumed)
                     : [jb] "r"(jb), [g] "r"(g), [c] "r"(c)
                     : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3",
                       "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",
                       "xmm15", "memory", "cc");
#endif
    c->jb = outer;
    if (resumed)
    {
        for (u64 i = 0; i < k; i++)
        {
            g[i] = 0;
        }
        g[k] = jb[14]; // the panic's number, left by rt_trap
    }
    else if (!(B & 1))
    {
        g[k] = 0;
    }
    NEXT_FRESH;
}

// A tail call (MIR_CALL_TAIL): CALLEE in this function's place, its frame this one's; it
// returns to this function's caller. The arguments were copied to the frame's start before.
STENCIL(tail)
{
    __attribute__((musttail)) return CALLEE(f, c, 0);
}

STENCIL(ret)
{
    (void)f;
    (void)c;
    (void)acc;
}

// ---- output ---------------------------------------------------------------------------------------

#define WRITE(name, CALL)                                                                          \
    STENCIL(write_##name) { u64 x = f[A]; CALL; NEXT_FRESH; }                                      \
    STENCIL(write_##name##_a) { u64 x = acc; CALL; NEXT_FRESH; }

WRITE(str, rt_write_str(c, (const uint8_t *)(uintptr_t)x))
WRITE(int, rt_write_int(c, (int64_t)x))
WRITE(uint, rt_write_uint(c, x))
WRITE(bool, rt_write_bool(c, x))

// One byte to the output in place (the byte sink, inline): a store while the buffer has room,
// which is every byte but one in output_buffer; the flush is the only call, when it is full.
// std's formatting writes byte by byte through it (docs/std.md §4).
static inline __attribute__((always_inline)) void put_byte(Ctx *c, uint8_t ch)
{
    if (__builtin_expect(c->len == c->cap, 0))
    {
        rt_flush(c);
    }
    c->buf[c->len++] = ch;
}

WRITE(byte, put_byte(c, (uint8_t)x))
WRITE(trap_part, rt_trap_part(c, (const uint8_t *)(uintptr_t)x)) // MIR_TRAP with sub MIR_TRAP_PART

// A trap (MIR_TRAP): A holds the address of its message. It does not come back.
STENCIL(trap) { rt_trap(c, (const uint8_t *)(uintptr_t)f[A]); }
STENCIL(trap_a)
{
    (void)f;
    rt_trap(c, (const uint8_t *)(uintptr_t)acc);
}

STENCIL(write_space)
{
    put_byte(c, ' ');
    NEXT_FRESH;
}

STENCIL(write_nl)
{
    put_byte(c, '\n');
    NEXT_FRESH;
}
