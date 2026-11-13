# Modal: compile-time evaluation

Status: 2026-11-12. This replaces the earlier keyword version. It is the design, then the steps.
Steps 0–2 are done except target and config values (context.md §22–24).
Companions: `docs/p.md` (performance), `docs/guards.md` (faults), `docs/context.md` (record).

---

## 1. The rule

> **Where code is written decides when it runs**, the same way where a type is written
> decides whether a name can change.

There is no keyword or sigil. The compiler runs everything written in a compile-time
position, and the program gets only the results.

| Position | Runs in |
|---|---|
| a module-level initializer (`limit: u64 = fact(10)`) | the compiler |
| a type, a pattern literal, an `@` address, a `mod.modal` value | the compiler |
| a module-level statement (a guard: `assert(sizeof(header) == 16)`) | the compiler |
| a function body | the program |

Inside a body, there is one rule that is guaranteed: **an operator whose operands are all
compile-time values is evaluated by the compiler** (`limit * 2` when `limit` is a module
constant). A *call* runs in the compiler only from a compile-time position, so the cost of a
call never moves out of sight. To get a call's value at compile time inside a body, give it a
module-level name; the value then has a name, a type and one place:

```
u64 fact(u64 0): 1
u64 fact(u64 n): n * fact(n - 1)

limit: u64 = fact(10)        -- the compiler runs fact: 3628800 is data in the program
u64 count = fact(3)          -- mutable; its first value comes from the compiler

main(): u8 {
    print(limit, fact(10))   -- the second call runs in the program
    return 0
}
```

**Why no forcing function** (`ct(fact(10))` from std): it would be a function that decides
*when its own argument runs*. No ordinary call can do that, so it would be a macro dressed as
a function. Position already expresses it.

---

## 2. Functions: three kinds, inferred

Not every function can run in the compiler. Some need the running program: its output, its
devices, its memory. Some can only run in the compiler. The kind is **inferred** from what
the function does, never declared, and it is part of the function's contract (`--sig`).

### 2.1 What the references do

- **C++** declares it: `constexpr` (can run at both times) and `consteval` (compile time
  only), with a list of forbidden operations.
- **Rust** declares `const fn`, a restricted subset.
- **Zig, D (CTFE)** declare nothing. Any function can run at compile time, and the compiler
  refuses one that does something only a running program can do (an external call, `asm`,
  volatile access, mutable global state). Zig decides per call, silently. `@inComptime()`
  lets a function change behaviour.
- **Jai** declares nothing either, and `#run` can do anything, including file I/O and OS
  calls. That makes it very powerful, but a build can depend on more than its sources.
- **revo**: `comp expr` compiles the expression and runs it on the real VM, with no limits
  and no sandbox (it can print). Its result becomes a constant.
- **zenc**: `comptime { }` plus `@comptime fn`, run by an AST interpreter limited to 1M steps
  and 64 levels of recursion. It writes source text that is parsed back in, and silently
  skips statements it cannot run.

None of them types compile-time results against the program's types, infers the kind, or
makes compile time deterministic by construction.

### 2.2 Modal's model: two machines with different capabilities

Modal already says what each target can do (`MachineTarget.caps`: system output, exit;
nothing on `*-none`), and a program that needs what its target lacks is an error at that
line. The compiler is one more machine with its own capabilities, and **a function runs on
the machine that has what it needs**. Each function's needs are inferred over the call graph
(from its MIR):

| Kind | Needs | Runs | Examples |
|---|---|---|---|
| **both** | nothing | in the compiler from a compile-time position, in the program otherwise | arithmetic, pattern clauses, struct values, immutable module constants, calls to other "both" functions |
| **run time only** | a program capability | in the program; from a compile-time position, an error naming the operation and the chain of calls | output (`print`, `write`, `out`, `sys.put`), device and register access, external C calls, **mutable** module variables, a closure over run-time locals, and later `asm`, faults, time |
| **compile time only** | a compiler capability | in the compiler; never in the binary; from run-time code, an error | **any function declared in `mod.modal`**, and any function using `type` values, `import`, target or config values, or reading the domain's files |

- **Mutable module variables are run time only to read**, because at compile time only
  their initial value exists. A function that reads one would give a different answer in the
  compiler than in the program.
- **`mod.modal` is the compiler's file.** It already holds what the compiler evaluates
  (configuration, imports). Functions declared there are compile time only and internal to
  the domain, the way C's `static` keeps a function inside its file. It is the position rule
  at the level of files. Done (context.md §24): every file of the domain calls them by name
  from a module-level position; a call from a body, or from another domain, is an error; they
  are never generated.
- **Compile-time capabilities are deterministic inputs only:** the source files, the target
  description and the configuration. Compile time cannot print, read the clock, or reach the
  network. So results can be cached by the hash of their inputs, and a rebuild with the same
  inputs gives the same bytes.
- **No `@inComptime` / `#compile_time`.** A "both" function gives the same result at both
  times, and the differential test (§5) proves it on every backend.
- **The contract shows the kind**:

```
fact(n: u64): u64                       both
log(n: i32)                             run time  (prints, through 'out', util.modal:8)
table(n: u32): u32                      compile time  (declared in mod.modal)
```

  A change of kind is visible to every user of the function, as a change of mutability is.
- **Deferred:** a function that is "both" for some arguments only (Zig decides per call).
  Modal decides per function, so the kind can be read from the contract alone.

---

## 3. What compile time supports

### 3.1 Variables

Every module-level binding is compile time:
- `limit: u64 = e` and `limit := e` are compile-time constants, embedded as data.
- `u64 count = e` is a run-time variable whose initial value the compiler computes.
- **Integer constants stay unsized** until they meet a type, like a literal. That way
  `n := 5` then `f(n)` with a `u8` parameter works exactly as `f(5)` does, and `n := 300` passed
  to a `u8` is the same range error as `f(300)`. The value is exact (the checker's 128-bit
  arithmetic, as for literal expressions) and must fit some integer type (-2^63 to 2^64 - 1).
  Done (context.md §24).

### 3.2 Parameters (generics without a marker)

A parameter is compile time when its type is `type` or a function type (§3.3), or when it is
used where only a compile-time value can be (a type, a pattern, later an array length):

```
max(T: type, a: T, b: T): T { ... }    -- T is compile time: its type is `type`
big := max(u64, x, y)                  -- one copy of max for u64, made once and kept
```

Every distinct set of compile-time arguments gets one specialization. Specializations are
cached and shared (p.md B5), so `max(u64, ...)` used ten times is one function.

### 3.3 Lambdas

A function literal is a compile-time value: a known function. Its kind is inferred like any
function's; a closure over run-time locals is run time only.

A **parameter of function type is compile time too**: a function is a compile-time value, so
the callee is specialized for each function passed, and the call inside is direct (no
function pointer, nothing to look up at run time):

```
apply(f: (u8) u8, x: u8): u8 { return f(x) }   -- f: a function type, so compile time
r := apply((x: u8) u8 { return x * 2 }, 21)    -- apply is specialized: f(x) becomes x * 2
```

Function values chosen at run time (a table of handlers) are a separate feature, run time
only, and not part of this plan.

### 3.4 `import` is a compile-time function

`import(x)` returns a module value; selectors on it are compile-time member access. So
`std := import("std")` is an ordinary module-level binding under §1, not a macro, and
`import("std").io.print` is a member of a compile-time value. Imports are "compile time only"
by §2.2: an `import` inside a body is an error that says to bind it at module level.

### 3.5 Guards at compile time

`assert` and `sizeof` stop being reserved keywords and become std functions. At module level,
`assert(...)` runs in the compiler and a failure is a compile error at that line, with both
sides of a comparison. In a body it is a run-time guard (`docs/guards.md`). One function, and
the position chooses the time. Done (context.md §24):

```
assert := import("std").assert   -- std, written in Modal (docs/std.md); sizeof is a primitive

assert(sizeof(header) == 12)   -- error: assert(sizeof(header) == 12) is false (8 == 12)
tables()                       -- a call at module level runs for the asserts inside it
```

- A module-level call statement is one that ends its line (`name(...)` with nothing after the
  `)`): a declaration always has a body, so the two never meet.
- `sizeof(T)` takes a type (`u32`, a struct) or a value, and is an unsized constant from the
  target's layout.

---

## 4. The engine

- **An interpreter over the MIR**, the instructions the backends already turn into machine
  code and C. That gives the same wrapping, comparisons and struct copies by construction.
  Layout questions are answered from the *target* description, so cross-compiling is exact.
  It is not a JIT: a board's code is not built for the host. A JIT for host = target can come
  later, checked against the interpreter.
- **The two existing constant evaluators fold into it:** the checker's 128-bit `exact_value`
  (range errors) and the lowering's wrapping `const_value`.
- **On-demand MIR:** a compile-time call may name a function further down the file. Its body
  (already a deferred token range) is parsed, checked and lowered out of order, and its MIR is
  kept while compile time can call it. A cycle is an error that shows the cycle.
- **Budgets** in `mod.modal`: `comptime_steps` (MIR instructions executed) and
  `comptime_depth` (calls). Meeting one is an error naming the call; it never stops silently
  (zenc skips silently; revo has no limit).
- **Errors that a running program would hit are compile errors**, with the chain of calls:
  division by zero, a budget, a pattern with no clause.
- **Results** (all done, context.md §23–24):
  - integers and bool become immediates;
  - strings and `[]u8` become bytes in the code (as string literals are);
  - structs of these become their fields, in the target's C layout;
  - a pointer is an error, since it would point into the compiler.
- **Cache:** per (function content hash, arguments), joining `--cache`.
- **`--comptime`** prints every evaluation: where, the call, the result, steps.

---

## 5. Steps

These are implemented after the binary work of p.md. Each is usable on its own.

0. ~~**Infer each function's kind** and print it in `--sig`. Pure analysis; no output changes.~~
   Done (context.md §22). The checker notes, per body, output, device registers and mutable
   module variables, with where each first happens, and the functions it calls (including the
   next pattern clause). `--sig` closes that over the call graph, a cycle included, and says
   why: `step(n: u8): u8   run time  (prints, through 'log', kinds.modal:10)`. A file outside
   any contract gets a `== functions:` listing.
1. ~~**The MIR interpreter and module-level initializers** (`limit: u64 = fact(10)`)~~ done:
   numbers and bools (context.md §23); string and struct values, functions in `mod.modal`,
   callees in files not checked yet, unsized integer constants (§24). Verified by a
   differential that cannot be gamed: generated programs print `f(args)` from a body and the
   same call bound at module level, and they must agree on C, arm64, x86-64 and Linux.
2. ~~**`assert`, `sizeof`**~~ done (§24, and §26 moves them to a primitive and std).
   ~~**Target and config values**~~ done (§27):
   - `target := import("target")` gives `ptr_size`, `word_size`, `arch`, `os`, `has_output` and
     `big_endian`;
   - a domain's mod.modal values are readable by name in its files;
   - both at module level only. A body that reads one is an error that says how to use it.

   Left of this step: arrays (`[N]T`, with `N` a compile-time value). The main embedded use is a
   table computed once, in flash.
3. **`type` parameters** and specialization.
4. **Lambdas** as compile-time arguments.
