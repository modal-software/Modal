# Developing Modal

Status: 2026-11-12. How to work on the compiler: what it optimises for, the language rules a
change must keep, where things are, how to build and test, what makes a change done, and how
each phase is recorded. Read this first. The full design record is `docs/context.md`.

---

## 1. What Modal optimises for, in this order

1. **Speed:** of the code the compiler generates, then of the compiler itself. A change that
   makes either slower is not done, whatever else it buys.
2. **Size and memory:** binary size, program RAM and compiler memory. They matter (embedded
   targets are coming), but they do not win against speed on their own. A size regression is
   allowed when it buys speed and the trade is written down.
3. **Explicit control:** the programmer can see what the machine does (C-like transparency,
   p.md §5.2). Nothing costly happens out of sight.
4. **Portability toward embedded:** no 64-bit word, operating system or libc assumptions in a
   design. A target is a description (`MachineTarget`, `sema/layout.c`); the runtime reaches a
   program through context parameters.

**The ground rule (p.md):** an optimisation never changes what a program does. Every backend
gives byte-identical output and exit status, and the C backend is the reference.

## 2. Language design rules

Every new feature must fit these. If it can't, the rule is discussed with the owner first, never
bent in passing.

- **Position decides.**
  - Mutability follows where the type is written: `T name` is mutable, `name: T` is immutable.
    This covers fields, parameters and methods.
  - Time follows where the code is: a module-level position runs in the compiler, a function
    body in the program (`docs/comptime.md` §1).
- **No keywords or sigils.** No `pub`, `mut`, `comptime`, `@inComptime`. A new capability is a
  position, a predeclared name (like `true`, `nil`), or a std function. `assert` and `sizeof`
  were removed from the scanner's keywords for this reason.
- **Inferred, never declared.**
  - Function kinds (both / run time / compile time) are inferred and shown in `--sig`.
  - Visibility follows from where a declaration sits (inside the struct's braces or below it,
    a file, a domain's `mod.modal`).
- **Context parameters are type-last** (`arena: arena`, `out: out`). They are filled from the
  caller's scope.
- **The core/std rule.** The compiler implements only the *primitives* Modal cannot express
  itself. Everything else is std, written in Modal, in `.modal` files.
  - The compiler must not need std to build, and std must not need C's libc.
  - A new name the compiler recognises by a std path is not allowed. A new primitive needs a
    written reason: what Modal cannot express without it (`docs/std.md`).
- **One argued design per feature.**
  - Tie a proposal to the goals in §1: its rules, examples, edge cases and costs.
  - A menu of quick options is not a proposal. Offer alternatives only when they are equally
    strong.

## 3. Where things are

### 3.1 The compiler, in pipeline order (`src/v2`)

| Stage | Files | Owns |
|---|---|---|
| driver | `main.c` | flags, phases, `--stats`, `--time`, `--repeat` |
| context | `compiler/cx.h`, `cx.c` | regions (one shared page, then own reservations), the arena, units, diagnostics |
| scan | `syntax/scanner.c` | 12-byte tokens, interned names, whole function bodies as one `TOK_BODY` (parsed later) |
| parse | `syntax/parser/parse_decl.c`, `parse_expr.c` (Pratt), `parse_stmt.c` | 16-byte nodes linked by index |
| declare | `sema/resolve.c` | symbols and scopes of one file |
| link | `sema/module.c` | imports, domains, `mod.modal` configuration, visibility, `--sig` |
| check | `sema/check.c` | names, calls and types in bodies; function kinds; std members (to shrink, §2) |
| compile time | `sema/comptime.c` | the MIR interpreter, module-level values and statements, the optimizer's call folds |
| layout | `sema/layout.c` | targets, C layout, field offsets |
| cache | `sema/cache.c` | `--cache`: unchanged files are not checked again |
| lower | `mir/lower.c`, `mir/mir.h` | checked tree → MIR, one function at a time, inlining of small bodies |
| optimise | `mir/opt.c` | fold (with calls run by the compiler), output merging, dead stores, scalar fields, DCE |
| native | `backend/native/emit.c`, `regalloc.c`, `stencils.c` (→ generated `stencils_*.h`), `macho.c`, `elf.c`, `run.c` | copy-and-patch machine code, registers, executables written directly, `--run` in memory |
| C | `backend/c/emit.c` | the reference backend (C, built with `cc`; uses only `write` and `exit`) |
| std | `std/*.modal` | the standard library, in Modal (`docs/std.md`) |
| registry | `backend/registry.c` | backend names (Go-style) |

`src/v1` is frozen: it is not built and is not changed.

### 3.2 Tests, tools, examples

- `src/v2/tests/run.sh`: the suite (`make test`).
- `src/v2/tools/`:
  - `fuzz.py`: differential random programs;
  - `bench.sh`: the p.md table;
  - `backends.sh`: one program on every backend;
  - `elfrun.c`: runs a Linux ELF on macOS;
  - `stencilgen.c`.
- `examples/run/`: programs run on every backend, each `x.modal` with `x.expected` (output and
  error output). Folders such as `multi/` and `compiled/` are domains run as a whole.
- `examples/errors/`: programs that must fail, matched by `expect_output` patterns
  (`file:line:` then the message).
- `examples/board/`, `examples/budget/`: bare metal and memory budgets.

### 3.3 Documents

| File | What it is |
|---|---|
| `development.md` | this file: how to work |
| `context.md` | the design record. One numbered section per phase: what, how, measured, verified, not yet |
| `p.md` | the performance plan: targets, the table of done steps |
| `comptime.md`, `guards.md`, `std.md` | the plans of those features, with their steps struck through as they land |
| `guide.md` | the language as a user sees it today, and its gaps |

## 4. Build, run, test

```
make                                  # ./modal (gcc -O2 -flto)
make test                             # the suite
make fuzz N=200 SEED=7                # random programs on every backend; 0 must differ
FUZZ_KIND=struct make fuzz            # also scalar, pressure, mix (default)
MODAL_REGS=0 make fuzz                # every function without registers (2: all with)
MODAL_TREE=0 make fuzz                # arms tested one by one, not by the search (std.md §8)
make backends F=examples/run/x.modal  # one program on every backend, side by side
make asan                             # ./modal-asan (ASan + UBSan)
BIN=./modal-asan make test            # the suite under the sanitizers (and BIN= for fuzz.py)
make bench                            # the numbers of p.md, best of N
make stencils                         # after editing stencils.c (needs clang)
```

Useful flags: `--stats` (code, RAM, compiler memory, calls computed), `--time`, `--mir`, `--sig`,
`--repeat N`, `--backend native|x86_64|c`, `--target arm64-linux|x86_64-linux|arm64-none|...`.

**Known failing checks** (failing since before 2026-11-12; do not count them as yours):

| Check | Why |
|---|---|
| example program compiles cleanly; import alias | `examples/main.modal` uses `nil` as a return type |
| v1 example still parses | `examples/add.modal` is v1 syntax |
| output.modal on 5 backends | the owner's edit prints `43`; the expected file says `42` |

## 5. How to add a feature

Walk the pipeline. For each stage, ask whether the feature touches it:

1. **Design** in the feature's doc (§2's rules), with the steps it will take.
2. **Scanner:** a new token is rare. Prefer a predeclared name over a keyword.
3. **Parser:** a new form must not collide with an existing one. Example: a module-level
   `name(...)` that ends its line is a call statement, because a declaration always has a body.
4. **Declare/link:** new symbols, visibility, what `mod.modal` may hold.
5. **Check:** types, errors that name the fix, the function-kind facts.
6. **Compile time:** whether it can run in the interpreter (`comptime.c`). Whatever it cannot
   do there must be refused, with the function and the reason.
7. **Lower:** MIR for it, and `const_value` if it folds.
8. **Backends:** native gets it from MIR; the C backend needs its own emission.
9. **Tests:**
   - a run example on every backend;
   - error checks with `file:line:`;
   - a fuzz generator extension when the feature has many shapes.
10. **Docs:** §7.

Context.md §24 and §26 are worked examples. In §24, `assert` and `sizeof` go through every stage.
In §26 they move out of the compiler: `sizeof` becomes a primitive, and `assert` becomes std
code in Modal on the `trap` and `site` primitives.

## 6. What makes a change done

- Every backend gives the same output and exit status: C, arm64, x86-64, and the Linux ELFs.
  Check with `make backends` and the suite.
- New behaviour has a test:
  - `examples/run/` and `.expected` for a program;
  - `examples/errors/` and an `expect_output` line for an error.
  An error file stops at its first checker error (compile-time evaluation runs only on a clean
  file), so keep separate files for checker errors and compile-time errors.
- `make fuzz` shows 0 differ, across the kinds touched.
- `make asan`, then the suite and the fuzzer under it, are clean.
- **Measured:**
  - anything touching generated code: `make bench` before and after. A speed regression blocks
    the change;
  - compiler memory: `--stats` on the examples and on bench's `big` (about 41 MB today).
    Memory per symbol counts: big has 54,000 functions.
  - a change to the checker or to generics: bench's `specs` as well as `big`. `big` makes no
    specialization, so it cannot see a cost that grows with them (context.md §69).
  - a change to code generation: bench's `reached` too, where main reaches every function. It
    is the case where finding what main reaches gains nothing and only costs (§70).
  - a change with a cost per declaration: the generators of context.md §70 (a feature at n and
    4n). Instructions retired (`/usr/bin/time -l`) separate a 0.5% change from the noise.
- The known failing checks of §4 are the only failures.
- Commit only when the owner asks. Work on the feature branch (`feat/...`), never on `main`.

## 7. Documenting every phase

A phase is not done until its record is written, in the same change:

1. **`context.md`:** a new numbered section before the appendix. Each part is a bold heading
   with short bullets:
   - **What works**;
   - **How**: the mechanism, with file and function names;
   - **Measured**: a before/after table;
   - **Verified**: suite count, fuzz numbers, sanitizers;
   - **Not yet**.
   Strike out the "not yet" items of earlier sections when they land, with a pointer to the new
   section.
2. **`p.md`:** a row in the table of §1, with the headline number.
3. **The feature's plan** (`comptime.md`, `guards.md`, `std.md`): strike through the step and say
   what is left of it.
4. **`guide.md`:** gaps closed, new usage.
5. **Examples changed to keep covering their feature:** say which and why. Example: a call given
   an argument from a mutable variable, so the optimizer does not compute it.
6. **This file:** a new pitfall, tool or rule.

Numbers in the docs are measured, never estimated. When an early version of the change measured
worse, say so; don't present that version as the "before".

## 8. Pitfalls

- **Regions move.** A shared region that outgrows its slice of the shared page moves to its own
  reservation.
  - What was handed out stays valid, but a saved `region_end` pointer no longer belongs to the
    region (`mir_nest_end` handles it).
  - Compare positions, never assume a fixed base.
- **The arena never frees.** Whatever is allocated per symbol or per call stays for the whole
  compilation. Prefer small maps to per-symbol tables (comptime's function map), and size
  buffers to their use.
- **Deferred bodies:** a function's tokens and nodes exist only while it is checked and lowered,
  then they are released. Keep byte offsets, not tokens, for anything that outlives the check.
- **`REGION_MIR` holds one function.** Lowering resets it. Lowering inside a lowering (the
  optimizer running a callee) must go through `mir_nest_begin` / `mir_nest_end`, which keep the
  caller's MIR and the lowering state `L`.
- **Quiet checks:** `cx.quiet` re-checks a body without reporting, for compile time. Its errors
  are counted in `cx.quiet_errors`, and the file reports them in its own turn. Don't fold or
  generate code during one.
- **The C emitter's `PUT(e, "...")` takes a string literal** (it uses `sizeof`). Use `put()` for
  anything computed.
- **A folded call hides a test's subject.** The optimizer computes calls whose arguments are all
  constant. An example meant to exercise recursion, folding of identical code or stack sizing
  must pass a run-time value (a mutable module variable).
- **The checker stops compile-time evaluation.** `comptime_unit` runs only on a file with no
  errors.
- **AVX2:** code tagged for `"avx2"` must use runtime dispatch (AGENTS.md); never assume the
  host has it.
- **The makefile lists every source by hand.** A new `.c` file goes into `SRCS`.
- **Stencils and clang's outliner.** The runtime functions are compiled with `minsize`, so clang
  may move code two functions share into an `OUTLINED_FUNCTION_*`, which stencilgen rejects. Make
  one function call the other instead (`rt_trap` calls `rt_trap_part`). `make stencils`
  regenerates the headers byte-identically when nothing changed: check that before and after.
- **A trap that is a part ends no block.** Use `MIR_ENDS(in)`, never `op == MIR_TRAP`, wherever a
  block or a live range ends.
- **Names in the generated C:** a program's names become `m_<name>` and `g_<name>`, and those of
  other files `m_u<file>_<name>`. Runtime helpers use `mrt_`, which no Modal name can become
  (`m_out` clashed with a program's `out`).
- **The interpreter keeps its own call stack** (`struct CtFrame` in comptime.c): a Modal call is
  not a C call, so `comptime_depth` is not bounded by the compiler's stack. Keep it that way,
  and format messages in `stopf`.
- **std is found next to the executable** (or `$MODAL_STD`). A test that runs a copied compiler
  from elsewhere needs `MODAL_STD`, or std is external.
- **Loops are recursion, so tail calls are not an optimization but semantics** (context.md §28).
  A loop of a million turns must run in the stack of one call on every backend: MIR_CALL_TAIL in
  native and the interpreter, `M_TAIL` (musttail) in C. A change that stops a call from being a
  tail call is a crash for long loops: test with `examples/run/loops.modal` (ten million turns).
  - A function that takes the address of one of its slots (MIR_ADDR) makes no tail call: the
    callee's frame takes this one's place.
  - In C, `M_TAIL` is used only with the same prototype and numbers and bools as parameters
    (musttail's rule, and no pointer can reach the frame).
- **MIR_JUMP** (an arm to its match's end; self_loops' loops). Branch checks use `MIR_BRANCHES`
  and `MIR_NO_FALLTHROUGH`, never `op == MIR_JZ || op == MIR_JNZ`: a site that forgets the jump
  treats the code after it as reached.
- **A module-level `{` is one token** (TOK_BODY: a function body, tokenized when it is checked).
  Syntax that puts a brace after a value at module level (a match) needs it tokenized first; that
  moves the token array under the parser's pointers, so it is not done yet.
- **Arms and patterns share one description:** `Pattern` (`sema_pattern_of` for a parameter,
  `sema_arm_pattern` for an arm), `pattern_test` in lower.c, `emit_pattern_cond` in the C
  backend. A new kind of pattern goes into all three, and into the checker's `Cover`.
- **Pattern clauses reach the native backend joined** (`mir_clause_hold` / `mir_clause_merge`): a
  clause with a next one is held, copied to the arena, until the next is lowered. Its `--mir`
  therefore comes after the next clause's. What is still held at the end is generated as it was.
- **No lookup that runs per function, per call or per body may walk a table.** Index it
  (by symbol, or a hash), and check it at scale (bench `specs`, context.md §69). An index is
  not free either: reading a side table per symbol costs cache misses on a program that does not
  need it. Gate it on a bit the symbol already has (`SYM_F_SPEC`), and compare instructions
  retired (`/usr/bin/time -l`) when times are within the noise.
- **Large arrays go at the end of `cx`.** A field past an arm64 load's immediate offset costs an
  instruction at every access: 1,024 units in front of `cx.syms` cost about 1M instructions on
  big's check (context.md §70).
- **A macro that reads its argument twice** breaks with `i++`: `SPEC_AT` is a function for that
  (`-Wunsequenced` caught it).
- **What main reaches is found from the text** (module.c, before any body is checked). A new
  way to call a function without naming it in a reached declaration (beyond a struct's methods
  and a specialization's generic) must be added there, or the function is not generated.
- **A stencil change needs `make stencils`**, and the emitter refuses stale headers
  ("its stencils are out of date"). After `git stash` and back, touch the sources: make compares
  whole seconds, and a stash round trip within one second leaves a stale `./modal`.

## 9. Depending on no C compiler: what is still missing

Status: 2026-11-15. A C compiler (gcc, clang or `cc`; on macOS `gcc` is Apple's clang) is used
in four places. Each one comes off separately, and they are listed here easiest first.

| # | Where | What uses it today | What is needed to drop it |
|---|---|---|---|
| 1 | Building Modal programs | the default backend is `c` (`backend_default`); `--run` with it calls `cc` (`build_and_run`, `$CC`); `-o x.c` writes C | native as the default (below) |
| 2 | Testing | the C backend is the reference (§1's ground rule); `make fuzz` compares every backend against it; `run.sh` runs every example through `cc`; `bench.sh` builds `bestof.c`; `backends.sh` builds `elfrun.c` | a reference that is not C (below); the tools written in Modal |
| 3 | The stencils | `make stencils`: clang compiles `stencils.c` for each CPU and `stencilgen` turns it into `stencils_*.h` | nothing while `stencils.c` does not change (the headers are in the tree); later, stencils that Modal builds itself |
| 4 | Building the compiler | `make`: `CC = gcc`, `-O2 -flto`; `make asan`: clang | self-hosting: the compiler written in Modal, built by the native backend from a kept bootstrap binary |

### 9.1 Native as the default backend (step 1)

The native backend already needs no toolchain. It writes Mach-O and ELF executables itself (no
assembler, no linker) for arm64 and x86-64, on macOS, on Linux, and on bare metal, and `--run`
runs the program in memory. What keeps `c` the default is what native does not do yet:

- **Features C has and native refuses** (found by probing, 2026-11-15; not an exhaustive list):
  - the address of an array element, `&ps[1]` ("this unary operator", lower.c);
  - an arm match whose value is a struct, `return if k { 0: point{...}, _: ... }` ("this struct
    value", lower.c).
  - The full list comes from an audit: one program per `unsupported(...)` site in `mir/lower.c`
    and `backend/native/emit.c`, run on both backends. A site C also refuses (strings compared
    with `==`, for example) does not block the switch.
- **Safety native lacks:**
  - a stack guard. A program that calls itself gets a 512 KB value stack (`stack` in mod.modal),
    and going past it crashes instead of stopping with a message (context.md §64; Phase E);
  - tail calls with struct blocks (100M calls end in status 139; Phase E).
- **Then:** `backend_default()` picks the native backend for the host (C stays available with
  `-b c`), `--run` no longer needs `cc`, and the docs and examples that assume C change with it.

### 9.2 A reference that is not C (step 2)

The ground rule stays the same (every backend gives byte-identical output and status). What
changes is which backend the others are checked against:

- **The MIR interpreter** (`sema/comptime.c`) becomes the fuzzer's reference, with native
  without registers (`MODAL_REGS=0`) as a second opinion. Today it runs compile-time positions
  only. To run a whole program it still needs to:
  - hold mutable module variables (it stops: "uses a mutable module variable");
  - write to the program's output instead of the compiler's;
  - end with the program's status on a trap;
  - run with no step budget (`comptime_steps`) in that mode.
- The C backend becomes one more backend under test, checked when a C compiler is present and
  skipped otherwise (`run.sh` already skips the ELF runs when `elfrun` cannot be built).
- `bestof.c` (bench) and `elfrun.c` (running Linux ELFs on macOS) are host tools. They are
  rewritten in Modal once std can spawn a process and read files, or they stay as optional
  tools.

### 9.3 The stencils (step 3)

Today the stencils are C functions that clang compiles with `-ffreestanding -fno-pic` for each
CPU, and `stencilgen` cuts the machine code and holes out of the object file. Building Modal
needs clang only after `stencils.c` changes. To drop clang entirely, the stencils are written in
Modal and compiled by Modal's own native backend into the same header. That needs step 4's
compiler first, or a small encoder of the instructions they use.

### 9.4 Self-hosting (step 4)

The compiler is 44,186 lines of C (`src/v2`, without the generated stencil headers and the tools). Written in Modal, it is built by a bootstrap `./modal`
that is kept, and from then on by itself. What Modal still lacks for that:

- the collections step (context.md §64): slices and growable arrays over the arena, for token,
  node and symbol tables;
- files, the program's arguments and its exit status in std (only output exists today);
- maps (hash tables), written in std;
- pointers into arrays (`&ps[1]`, above) and structs of pointers, which every table needs;
- the speed rule (§1) applies: the self-hosted compiler must compile as fast as the C one
  (bench's `big`: 24 ms) before it replaces it.

Also missing, found in the `unsupported(...)` sites of `mir/lower.c` and in the open questions
(2026-11-15):

| Missing | Why a compiler needs it |
|---|---|
| **Real memory behind the arena** | The `arena` of the examples is a struct that counts numbers; it hands out no bytes. A primitive is needed that gets memory from the target (a static region, or mmap/brk), with byte loads and stores through an address. The collections have nowhere to live without it. |
| **Tagged unions (closed sets)** | A tree node is "one of N kinds". Still an open question (context.md §10, question 2). A struct with a `kind` field works but is slow and unchecked. This is the largest *design* gap. |
| **A struct inside a struct** | lower.c refuses it ("a struct inside a struct"). A node holds a position, a symbol holds a type. |
| **Operations on strings** | `[]u8` compared (`==` is refused on both backends) and sliced: every keyword and symbol lookup. |
| **Mutable module variables that are not literals** | Otherwise all compiler state goes through context parameters. That may be the design; it has to be decided. |
| **The stack guard; tail calls with struct blocks** (Phase E) | A recursive-descent parser passes the 512 KB value stack easily, and that crashes with no message. Loops are recursion, and the compiler's loops carry structs. |
| **Bytes written to a file** | The backends write executables, not text on the output. |
| **Number formatting in std** (std.md step 4) | Error messages, `--sig`. |

**Bootstrap in stages.** Today's C `./modal` is kept as stage 0 and builds the compiler written in
Modal (stage 1). Stage 1 builds itself (stage 2), and stage 2 builds itself (stage 3). Stages 2
and 3 must be byte-identical (the fixed point), and the speed rule holds before the switch.

**A cheap test of readiness:** Modal's own scanner, written in Modal. It already needs a file,
bytes, strings, a growing table and a closed set of token kinds. Whatever blocks it is the next
item to build.

#### 9.4.1 Doing more with less: every fact written once

The aim is not the 44,186 lines translated. It is the same compiler in much less Modal, at the
same speed. Most of the C's size is not the problem's difficulty but **the same fact written in
many places**:

- **Measured (2026-11-15):** one MIR instruction, `MIR_MOVE` (context.md §64), appears in 7 files
  and 29 places: `mir.h` 2, `lower.c` 8, `opt.c` 7 (reads, kills, fold, dead stores, fields),
  `comptime.c` 2, `regalloc.c` 2, `native/emit.c` 6, `stencils.c` 2. `mir.h` has 44 instructions.
- The largest files: `sema/check.c` 7,990 lines (about 170 diagnostic calls), `mir/lower.c`
  5,956 (157 `case`s), `backend/native/emit.c` 3,825 (148 `case`s), `backend/c/emit.c` 3,776,
  `sema/comptime.c` 1,332, `mir/opt.c` 958.

Modal already has what C lacks to stop the repetition: code run while compiling with no keyword,
code made once per field (`field(v, i)`, context.md §49), arms decided while compiling, context
parameters, error sets. The self-hosted compiler is meant to be **the heaviest user of those
features**, not C written in another syntax. Five applications:

1. **A MIR instruction is a Modal function, and the rest is derived from it.**
   ```
   -- the instruction is its meaning
   add(a: u64, b: u64): a + b
   ```
   - the interpreter (`comptime.c`) runs the function;
   - folding (`opt.c`) runs the same function while compiling, when the operands are constant;
   - reads, writes and effects come from the signature (parameters read, the result written,
     `arena` / `out` as effects), so the optimizer has no hand-written table;
   - the stencil is the function compiled by the native backend itself, which also settles §9.3
     (clang leaves the build);
   - the C backend emits it as a `static inline` function.
   From 7 files per instruction to 1, plus a CPU encoder where a stencil is not enough.
2. **The tree is a tagged union, and the walks are generated.** The passes that only walk the
   tree (`resolve.c` 535, `audit.c` 502, parts of `layout.c` 625) are mostly "for each child, go
   down". With `field(v, i)` extended to a union's variants, the generic walk is written once in
   std, and each pass writes only the arms it cares about, with `_` for the rest. Arms on the tag
   become a jump table: no speed cost. This makes tagged unions requirement number one, before
   the collections.
3. **Diagnostics are error values, with no plumbing.** Each of check.c's diagnostic calls builds
   its place and text by hand. With what exists (`site`, error sets, `n, e :=`, errors going up),
   a check error is a value, `return type_mismatch{ want, got }`. The place comes from `site` or
   the node, the text from printing by reflection in std, and the `if (!ok) return` chains go away.
4. **Memory: one arena per phase, no free.** A compiler is a batch process. The `realloc` /
   `_push` / `grow` sites (about 80 in the five largest files) become collections over the
   context `arena`, passed implicitly. Each phase (parse, check, lower, emit) has its own arena
   and drops it whole when it ends: allocation is an add, there is no freeing code and no lifetime
   bugs.
5. **Tables are data; their matchers are generated.** The scanner's keywords, operator
   precedence, arm64 and x86-64 instruction encodings, and the Mach-O / ELF fields become
   constant structs. Their lookup is made while compiling (string patterns in arms already
   exist and become a comparison tree), which keeps the arms-for-predictability rule. A CPU's
   encoder is its table and a loop.

**What this moves up in the requirements:**
1. tagged unions, and `field(v, i)` over their variants;
2. one function used as an instruction: called while compiling, by the interpreter, and as a
   stencil's body;
3. errors that carry data (structs in an error set), not only names.

**How it is measured:**
- **The metric:** the number of places touched to add a MIR instruction, a node kind or a
  diagnostic. The target is 1. This tests the philosophy better than a line count.
- **The brake:** the speed rule (24 ms on `big`) holds for each derivation. A derivation that
  makes slower code than the hand-written one does not go in.
- No line count is given for the Modal compiler: it cannot be measured yet (§7).
- **The first experiment:** application 1 with three or four instructions (`add`, `move`, a load,
  a branch) through all four consumers (interpreter, fold, stencil, C). It shows whether the idea
  holds before any rewrite.

**Order:** 9.1, then 9.2, then the memory primitive and the collections step, then a struct inside
a struct, `&xs[i]`, strings, tagged unions and the stack guard, then the std pieces (files,
arguments, exit status, maps, formatting), then 9.4 (with 9.4.1's experiment first), then 9.3,
which 9.4.1's first application may settle. Each step is a phase with its own context.md section.

#### 9.4.2 The audit of the compiler's own cost (2026-11-15)

Every phase was profiled with `sample` on bench's `big` (and a version ten times as large), and
on a program written to make specializations. What it found:

- **The profile of `big` is flat.** The largest item, `native_function`, has 57 of 636 samples.
  Tokens of 12 bytes, bodies parsed when checked, regions and an arena that never frees all hold.
- **Fixed: specializations were found by walking their table** (context.md §69). Each lookup was
  O(specializations) and ran once or more per function, so 4,000 specializations made a 20,000
  function program 12 times slower (69 → 824 ms). Bench's `specs` now measures that case.
- **`big` cannot see most features.** It has no generics, structs, arms, arrays or std, so the
  speed rule (§1) did not protect them. `specs` covers specializations; a program that uses
  structs, arrays, `map` / `fold` and arms at scale is still to be written.
- ~~**Fixed limits the self-hosted compiler will reach**~~ (context.md §70: none left of these):
  - ~~`MAX_UNITS` 64 files (cx.h)~~: 1,024, and 256 domains;
  - ~~`SPEC_MAX` 4096, `GENERIC_MAX` 1024 (check.c)~~: they grow;
  - ~~256 specializations per generic~~: a depth of 8,192 specializations made inside one
    another, with a message that says so;
  - ~~`MAX_LOCALS` 256 (check.c) and 512 (lower.c), `MAX_LABELS` 4096, `MAX_LOOPS` 256~~: they
    grow.
- ~~**Native generates every function and shakes them at the end**~~ (§70): a function of
  another file that main cannot reach is not generated, on native or C. The entry file is
  generated whole: reading its bodies cost 7% of a file whose functions are all used, so big's
  own dead code is still generated and shaken.
- **Native's memory per function:** ~~`big` takes 41.8 MB~~ 39.0 MB (§70: `stack_need` kept its
  tables; the fold and the shake use 32-bit indices), against the C backend's 28.4 MB. Still
  about 100 B a function above C: the MIR kept for inlining to the end (`Inlinable`), `fn
  ranges` (57 B a function), `fn_at` regrown in the arena.

Found by the generators of §70 (a feature at n and 4n: a ratio well above 4 is a walk inside a
walk):

- ~~`struct_marked` walked every symbol per field~~ (12,000 structs: 508 ms; §70: 21 ms).
- ~~`names_a_generic` read each body once per generic~~ (400 unused generics made 20,000 bodies
  4.6× slower; §70).
- ~~`print(e)` wrote a compare per error at each print~~, capped at 512 names (100 prints × 100
  names did not compile on native; §70: one routine).
- ~~C: a match past about 5,000 arms crashed clang~~ (§70: a flat chain past 64).
- **Clauses:** one group of n pattern clauses is O(n²) in `sema_declare` (2,000: 93.7 ms). Not
  fixed.
- **`val` is still a keyword** (v1): a module-level `val(...)` parses as an import list.
- The C backend's chain per `print(e)`, the duplicate-local check (O(locals²) in a block), and
  the 200-level nesting guard stay.
- **`--time` costs time:** two `clock_gettime` per deferred body, about 2% of the samples. Its
  numbers are slightly above the real ones.

## 10. Git

- Work on the feature branch. Commit or push only when the owner asks.
- Commit messages end with the attribution line given to the session.
- Never commit `modal`, `modal-asan`, `*.dSYM`, `.DS_Store` or scratch files (`a`, `a.c`, ...).
