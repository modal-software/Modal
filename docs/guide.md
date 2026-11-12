# Working with Modal

A short guide to the language as it is today (compiler v2), then what is missing. The full
design record is `docs/context.md`; plans are `docs/p.md`, `docs/comptime.md`, `docs/guards.md`.

## 1. Build and run

```
make                                   # builds ./modal
./modal file.modal                     # check only: errors, or one line saying what was verified
./modal --run file.modal               # build with cc (C backend, the reference) and run
./modal --backend native --run f.modal # machine code, run in memory (~1.7 ms end to end)
MODAL_REGS=0 ./modal ...               # the same without register allocation (to compare)
./modal --backend native -o prog f.modal            # a signed macOS executable, no toolchain
./modal --target arm64-linux -o prog f.modal        # also x86_64-linux, x86_64-macos
./modal --target arm64-none -o image f.modal        # bare metal, from mod.modal's memory map
make test                              # the suite;  make bench   the performance table
```

Useful flags:
- `--stats`: code, RAM and memory.
- `--time`: phases.
- `--mir`: the lowered code.
- `--sig`: each domain's contract, with each function's kind: `both` (it can run in the compiler
  too), or `run time` and why (it prints, touches a device, or uses a mutable module variable).
- `--syms`, `--ast`.

## 2. The one rule: where you write it decides

The position of a type decides whether a name can change:

```
limit: u8 = 7        -- type last: immutable
u8 hits              -- type first: mutable (starts at zero)
u64 total = 1000     -- type first, with a value
n := 5               -- bound to a value: immutable, its type taken from the value
```

The same rule applies to fields, parameters (`a: u8` cannot be assigned, `u8 a` can), locals
(`name: T = v`, `T name [= v]`) and methods (`f(..): T` is defined once; `T f(..)` may be
overloaded). The position also decides *when* code runs (`docs/comptime.md`): module level
is the compiler.

## 3. Declarations

```
-- a line comment          -{ a block comment }-
print := import("std").io.print

point: struct {
    i32 x,                 -- fields are always private to the struct
    i32 y,
    helper(self): i32 { return self.x }    -- inside the braces: private
}

new(self: point, x: i32): self {           -- below the struct, `self: T`: its public face
    self.x = x
    return &self                           -- `self` as a return type means *point
}
i32 x(self: point): self.x                 -- an accessor, one-line body

u64 fact(u64 0): 1                         -- pattern clauses, tried in order;
u64 fact(u64 n): n * fact(n - 1)           -- the last one must take every value

main(): u8 {
    p := point.new(3)                      -- T.m() runs m on a fresh zero value
    double := (v: u8) u8 { return v * 2 }  -- a function literal: (params) T { ... }
    print("x is", p.x(), "and", fact(5))   -- print: spaces between, a newline at the end
    return 0                               -- main's result is the exit status
}
```

- **Types:** `u8 u16 u32 u64 i8 i16 i32 i64`, `int` (32-bit signed), `bool`, `[]u8` (strings),
  `*T`, and struct names. There is **no implicit widening**: a `u8` is not a `u16`. A literal
  takes the type it meets and must fit it.
- **Arithmetic:** `+ - *` wrap in their type, by design. `/` traps on zero
  (`docs/guards.md`). `== != < <= > >=` compare (ordering is for numbers only), `and or !`
  combine bools.
- **Statements:** a statement ends at a newline, `;` or `}`. The forms are `return`, blocks
  `{ }`, declarations, assignment `=`, and calls.

## 4. Imports and domains

```
std := import("std")              m := import("./util")        m.f()  m.limit
io := import("std").io.*          import("std").io.(print, file as f)
val (file as f, print as p): import(std).io
```

- **A folder is a domain:** its `mod.modal` is its public face and its configuration.
- **Visibility:** what a file declares is visible to its domain. What `mod.modal` binds is
  visible outside it.
- **`mod.modal` keys:**
  - `implicit_self`
  - `stack`, `output_buffer`
  - `max_code`, `max_ram`
  - `flash_origin`, `flash_size`, `ram_origin`, `ram_size`
  - `version`, `profile`, `nil_return` (these are accepted but not used yet)

## 5. Context parameters: `out`, `arena`

A parameter named exactly like its struct type (`arena: arena`, always type-last) is filled
in by the caller from its own value of that name. Output uses this: every byte `print` and
`write` produce goes to the `out` in scope (a struct with `put(self: out, b: u8)`), or to the
target's system output if there is none. On a board, an `out` forwards bytes to a UART:

```
uart: struct { u8 dr @ 0x00, u32 status @ 0x04 }   -- a register block (offsets)
uart0: uart @ 0x0900_0000                          -- a device at an address
```

## 6. Known gaps (what to expect)

- **Control flow:** pattern clauses, recursion and `and`/`or` only. No `if`, loops or `match`.
- **No arrays,** slice operations, casts, floats, `%`, bitwise operators or shifts.
- **Overloads** are chosen by argument count only. `T f(u8)` and `T f([]u8)` collide.
- **Module-level values are computed by the compiler:** `limit: u64 = fact(10)` runs `fact`
  while compiling, for numbers and bools so far. A function that prints, touches a device or
  uses a mutable module variable cannot run there; the error says which one.
- **Not supported in code generation yet,** though the checker accepts them, so the error
  comes late: function values (`f := double` then `f(1)`), closures, `==` on strings, pointers
  and structs, and structs inside structs.
- **`T_EXTERN`:** an indexing expression, a dotted type path (`pkg.T`) and unknown `std`
  members are typed as "anything", so mistakes there are found only by the backend.
- **Diagnostics:** a syntax error in a deferred body can repeat as "expected '{'" at another
  place. An unsized literal prints as `int`.
- **Tooling:** no warnings, test runner (`test`/`assert` are reserved), formatter, LSP,
  debugger info, or REPL.

## 7. HPC and HPD: where Modal stands

- **High-performance development:**
  - checking takes microseconds (0.013 ms for a small program, about 18 ms for 3 MB of source);
  - `--run` takes about 1.7 ms end to end, under 1 ms over the process start;
  - errors name the fix;
  - next: an LSP reusing the checker, which is fast enough to run on every keystroke, then
    warnings and a test runner.
- **High-performance computing:**
  - values live in machine registers (linear scan over the MIR), and the instructions on them
    are encoded directly: an `add` is one instruction, and a compare and its branch are two;
  - copy-and-patch stencils remain for what calls out (output, traps). The MIR optimizer
    folds constants, forwards stores and removes dead code first;
  - next: keeping struct fields in registers when they are narrower than a slot, frames sized
    without the slots that live in registers, and the Cortex-M backend.
