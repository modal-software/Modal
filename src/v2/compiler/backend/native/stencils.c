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
typedef struct Ctx
{
    uint32_t len, cap; // bytes waiting in buf, and its size
    uint8_t *buf;
    uint64_t *globals; // module-level variables: the bottom of the value stack
} Ctx;

typedef uint64_t u64;

extern char HOLE_A[], HOLE_B[], HOLE_C[];
extern void CONTINUE(u64 *f, Ctx *c, u64 acc);
extern void JUMP(u64 *f, Ctx *c, u64 acc);
extern void CALLEE(u64 *f, Ctx *c, u64 acc);
extern long write(int fd, const void *buf, unsigned long n);

#define A ((u64)(uintptr_t)HOLE_A)
#define B ((u64)(uintptr_t)HOLE_B)
#define C ((u64)(uintptr_t)HOLE_C)
#define NEXT __attribute__((musttail)) return CONTINUE(f, c, acc)
// After a call out, acc is dead (no stencil that calls produces a value in it): passing 0 lets
// the stencil forget it instead of saving it across the call.
#define NEXT_FRESH __attribute__((musttail)) return CONTINUE(f, c, 0)
#define STENCIL(name) void st_##name(u64 *f, Ctx *c, __attribute__((unused)) u64 acc)
#define RUNTIME __attribute__((noinline))

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
    stack[A] = 0;
    CALLEE(stack + A, &ctx, 0);
    rt_flush(&ctx);
    return (int)stack[A]; // what main returned, as the C backend's `return (int)m_main()`
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
#define CMP(T, op, y) (u64)((T)x op (T)(y))

#define INT_STENCILS(t, T, S)                                                                      \
    FOUR(add_##t, WRAP(T, S, +, f[C]))                                                             \
    FOUR(sub_##t, WRAP(T, S, -, f[C]))                                                             \
    FOUR(mul_##t, WRAP(T, S, *, f[C]))                                                             \
    FOUR(div_##t, EXACT(T, S, /, f[C]))                                                            \
    FOUR(eq_##t, CMP(T, ==, f[C]))                                                                 \
    FOUR(ne_##t, CMP(T, !=, f[C]))                                                                 \
    FOUR(lt_##t, CMP(T, <, f[C]))                                                                  \
    FOUR(le_##t, CMP(T, <=, f[C]))                                                                 \
    FOUR(gt_##t, CMP(T, >, f[C]))                                                                  \
    FOUR(ge_##t, CMP(T, >=, f[C]))                                                                 \
    FOUR(addi_##t, WRAP(T, S, +, C))                                                               \
    FOUR(subi_##t, WRAP(T, S, -, C))                                                               \
    FOUR(muli_##t, WRAP(T, S, *, C))                                                               \
    FOUR(divi_##t, EXACT(T, S, /, C))                                                              \
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

WRITE(byte, { uint8_t ch = (uint8_t)x; rt_put(c, &ch, 1); })

STENCIL(write_space)
{
    uint8_t ch = ' ';
    rt_put(c, &ch, 1);
    NEXT_FRESH;
}

STENCIL(write_nl)
{
    uint8_t ch = '\n';
    rt_put(c, &ch, 1);
    NEXT_FRESH;
}
