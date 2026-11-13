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
# working on the compiler itself: docs/development.md
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

[]u8 size(u8 n) {                          -- arms: clauses without their head (std.md §8)
    0: "none"                              -- a value, a range `1..9` / `10..`, a string,
    1..9: "small"                          -- true / false, or a name (the rest, named)
    10..255: "big"                         -- every value must be taken: the compiler checks
}
nil show(u8 n) { print(n) }                -- nil: no result. Overloads are chosen by the
nil show([]u8 s) { print(s) }              -- argument types (a literal: the first it fits)

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
  `{ }`, declarations, assignment `=`, calls, and matches: `value { pattern: result ... }`
  (`c > 40 { true: ... false: ... }` is if / else).

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

The compiler fills one context itself: a parameter `site: site` receives the place of each call,
`file:line:col: <the call as written>`. A function that has its own `site` passes it on.

## 5b. Primitives and std

The language has a few predeclared names. They are not keywords, so a declaration of your own
with the same name wins. Everything else comes from std, which is written in Modal
(`docs/std.md`).

```
sizeof(header)      -- bytes in the target's layout, an unsized constant (no import)
u8(300)             -- 44: a conversion between integer types wraps, as a C cast does
trap("why: ", msg)  -- its strings to the error output, then status 1
assert := import("std").assert    -- std: assert(n < 4) prints `f.modal:9:5: assert(n < 4) is false`
print := import("std").io.print   -- still the compiler's (variadic), by this path
```

At module level, `assert(...)` or any call is a statement the compiler runs, and a false assert is
a compile error there.

Compile-time values are read at module level only (a body that reads one is an error):

```
target := import("target")      -- the machine compiled for
words := target.ptr_size * 8    -- also word_size, arch ("arm64"), os ("macos", "linux", "none"),
                                -- has_output, big_endian
budget := max_ram / 2           -- the domain's mod.modal values, by name
assert(target.ptr_size == 8)
```

## 6. Known gaps (what to expect)

- **Control flow:** pattern clauses, arms (`value { ... }`, a body of arms), recursion and
  `and`/`or`. No `if` or loop statements: a match on a bool is the `if`, and a tail call the loop.
  A match at module level is not supported yet (put it in a function).
  A loop is a clause that calls its function again as its last act (`u64 sum(u64 n, u64 acc):
  sum(n - 1, acc + n)`): that call is a jump, so it runs in constant stack on every backend, and
  in registers on native. A call that is not the last act (`n + sum(n - 1)`) takes a frame per turn.
- **No arrays,** slice operations, casts, floats, `%`, bitwise operators or shifts, if we want support array we wan to have 
several modalities to working with it. fixed-width arrays, 0 to k with T[..], grow-only, shrink-only, grow-n-shrink. in those modalities im accepting design suggestions to design. because our core philosophy its developer have control,safety but without taking hand-off perfomance.   
- **Overloads** are chosen by argument count and types: the first definition, in the order
  written, that every argument fits (a literal also by its value: `show(300)` skips `u8`).
- **Module-level values are computed by the compiler:** `limit: u64 = fact(10)` runs `fact`
  while compiling. That covers numbers, bools, strings and structs of these.
  - A function that prints, touches a device or uses a mutable module variable cannot run there;
    the error says which one.
  - `n := 5` stays unsized, like the literal, until it meets a type.
  - Functions declared in `mod.modal` run only there, for the files of their domain.
  - A module-level `assert(...)` is checked by the compiler.
  - Calls whose arguments are all constant are computed while optimizing (native backends).
- **Not supported in code generation yet,** though the checker accepts them, so the error
  comes late: function values (`f := double` then `f(1)`), closures, `==` on strings, pointers
  and structs, and structs inside structs.
- **`T_EXTERN`:** an indexing expression, a dotted type path (`pkg.T`) and unknown `std`
  members are typed as "anything", so mistakes there are found only by the backend.
- **Diagnostics:** a syntax error in a deferred body can repeat as "expected '{'" at another
  place. An unsized literal prints as `int`.
- **Tooling:** no warnings, test runner (`test` is reserved), formatter, LSP,
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
