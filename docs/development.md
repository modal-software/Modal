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
- **MIR_JUMP exists only after the optimizer** (self_loops in lower.c). opt.c does not handle it,
  so a pass that makes jumps must run after `mir_optimize`. Branch checks use `MIR_BRANCHES` and
  `MIR_NO_FALLTHROUGH`, never `op == MIR_JZ || op == MIR_JNZ`.
- **Pattern clauses reach the native backend joined** (`mir_clause_hold` / `mir_clause_merge`): a
  clause with a next one is held, copied to the arena, until the next is lowered. Its `--mir`
  therefore comes after the next clause's. What is still held at the end is generated as it was.
- **A stencil change needs `make stencils`**, and the emitter refuses stale headers
  ("its stencils are out of date"). After `git stash` and back, touch the sources: make compares
  whole seconds, and a stash round trip within one second leaves a stale `./modal`.

## 9. Git

- Work on the feature branch. Commit or push only when the owner asks.
- Commit messages end with the attribution line given to the session.
- Never commit `modal`, `modal-asan`, `*.dSYM`, `.DS_Store` or scratch files (`a`, `a.c`, ...).
