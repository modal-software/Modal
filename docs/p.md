# Modal: performance and memory plan

Status: 2026-11-11. Companion to `docs/context.md`, which holds the full design record (§13 the
compiler internals, §14 the embedded work). This file is the plan: where things stand, what was
measured, the targets, and the steps in order.

**Ground rule for every step:** the language does not change. No feature is removed, no
behaviour is altered to make a number smaller. Every optimisation must produce byte-identical
program output and exit status to the C backend (the reference), checked by the suite, the
random-program fuzzers (scalar, struct, deep-chain, sink generators) and an ASan/UBSan build.

---

## 1. Where things stand

Done (details in context.md):

| Step | What | Section |
|---|---|---|
| Front end | interned names at scan time, deferred bodies, 12-byte tokens, 16-byte nodes, regions | §13.1–13.5 |
| MIR + native | copy-and-patch stencils for arm64 and x86-64, accumulator, inlining, dead-code shake, Mach-O written directly | §13.2–13.4 |
| 1 | target description, C layout, struct values by value, sized loads/stores | §14.1 |
| 2 | Linux static ELF, no libc (hello: 704 B) | §14.2 |
| 3 | runtime as context parameters: the program's `out`, `std.sys.put`, target capabilities, shared output routines | §14.3 |
| 5 | memory budgets: exact value stack from the call graph, `max_code` / `max_ram` | §14.4 |
| 6 | register blocks and devices (`u32 dr @ 0x00`, `uart0: pl011 @ addr`) | §14.5 |
| — | bare metal `arm64-none` / `x86_64-none`, memory map in mod.modal | §14.6 |
| §4.1 | `--run` in memory (native), `posix_spawn` + build cache (C): 116 ms → 1.71 ms | §15.1 |
| §4.2 | writer: hardware SHA-256, constant hash for padding pages, one allocation, one `write`; emit 0.139 → 0.079 ms | §15.2 |
| §4.8 | `make bench`: every number of this file in one table | §15.3 |
| §4.3 | eight regions in one shared page, name table from 128 slots: taken 256 → 144 KB, in use 43.1 → 36.1 KB | §15.4 |
| B1 | macOS arm64 floor measured: ~16.6 KB (dyld required, `__LINKEDIT` 16 KB-aligned) | §18 |
| B2, B3 | MIR optimizer, direct output, constant programs: secret code 1,116 → 40 B, RAM 4,576 → 128 B | §19 |
| §4.7 | register allocation (linear scan, two register classes, fused compare-branch, direct calls): code −25–55% (structs 5,716 → 3,576 B, device 2,052 → 952 B) | §20 |
| §4.4 | exact output buffer from the call graph (counter 4,096 → 168 B), runtime frame = deepest chain | §20 |
| §4.5 | scalar fields (a frame struct's whole-slot fields become its slots), runtime compiled for size | §20 |
| B5 | values split from slots (webs), direct runtime calls in leaves, identical-code folding: counter 1,324 → 1,052 B, structs 3,576 → 3,072 B | §21 |
| comptime 0, 1 | function kinds in `--sig`; module-level initializers computed by the compiler (MIR interpreter) | §22, §23 |
| comptime 1, 2 | string and struct values, `mod.modal` functions, unsized constants, `assert` / `sizeof` | §24 |
| B5 | calls with constant arguments computed by the compiler (big: code 16,680 → 52 B, RAM 6,336 → 64 B, emit 35 → 31.5 ms) | §24 |

Not done: the Cortex-M backend (register allocation, its base, is done: §20), interrupts (need
QEMU or a board to verify).

---

## 2. Baseline, measured (examples/secret.modal, Mac mini arm64)

`secret.modal` computes `math.pow2(8, 2) - 1` into a struct field and prints `255`.

| Measure | Value | How |
|---|---|---|
| check only, warm (`--repeat 200`) | **0.020 ms** | `--time` |
| native `-o`, one process | 0.40–0.67 ms (yours: 1.44 ms cold) | `--time` |
| of which `emit` | 0.30–0.55 ms (75–80 %) | `--time` |
| `--run --backend native`, end to end | **116 ms** | wall clock, best of 30 |
| `-o ./a && ./a`, end to end | 5.1 ms | wall clock |
| starting any process (`/usr/bin/true`) | 2.95 ms | wall clock (same harness) |
| executable, macOS arm64 | 16,688 B | `ls -l` |
| executable, macOS x86-64 | 4,320 B | |
| executable, Linux arm64 | 1,316 B | |
| machine code | 1,116 B | `--stats` |
| program RAM | 4,576 B (output buffer 4,096 of it) | `--stats` |
| compiler memory taken | 256 KB | `--stats` |
| compiler memory in use | 43.1 KB (arena 20.5 KB) | `--stats` |
| compiler process RSS | 2.1 MB (dyld + libSystem ≈ 1.2 MB of it) | `time -l` |
| produced program RSS | 1.2 MB (dyld + libSystem) | `time -l` |

### 2.1 What the time actually is

Instrumented build (timers around each part of the native backend; numbers include the
timer prints, so read them as proportions):

| Part | Time | Why |
|---|---|---|
| Mach-O writer | **226–459 µs** | SHA-256 in software over the file twice (UUID + page hashes, ~33 KB at ~12 cycles/byte), the 16 KB page padding written one byte per call into a `realloc`ed buffer, then creating the file (APFS) |
| first function lowered and emitted | 20–120 µs | first touch of fresh regions: page faults, not work (measured later, context.md §15.4: pre-touching every region saves only 2–4 µs; it is cold caches, not faults) |
| `native_begin` | 23–51 µs | the same: regions and tables touched for the first time |
| every other function | 1–3 µs each | the real work |
| stack analysis, shake, runtime, fix-ups | ~15 µs | |

So the real compilation work for this program is ~25 µs; everything else is the operating
system: page faults, hashing, file creation.

### 2.2 Why `--run` is 23× slower than `-o && ./a`

Measured: **the first execution of any new executable file costs 110–130 ms on this Mac**,
the next ones 4–13 ms. It does not matter where the file is written or how (compiler, `cp`,
`$TMPDIR`, scratch). macOS validates the code signature of a new file (a vnode it has not seen)
and its security checks run on first launch. Rewriting the same path keeps the inode, so only the
first run after the content changes pays.

`--run` writes a new file every time (`$TMPDIR/modal-run-<pid>`), runs it through `system()`
(which also starts `/bin/sh`), then deletes it. So it pays the ~110 ms every time. The compiler is
not the cost; the file is.

### 2.3 Why compiler memory is 256 KB

16 regions (arena, tokens, nodes, syms, types, scopes, names, symbol names, code, MIR, fn
ranges, calls, runtime calls, lib calls, jumps, list stage), each mapped on its own, and a page
on this machine is 16 KB: 16 × 16 KB = 256 KB "taken" even though 43 KB is used. Of the 43 KB, the
arena's 20.5 KB is mostly tables created at a fixed minimum size (name hash table, scope
buckets, per-node side tables sized to the node capacity, `fn_at` and analysis arrays sized
to the symbol capacity).

---

## 3. Targets

| Measure | Today | Target | Factor |
|---|---|---|---|
| `--run`, end to end | 116 ms → **1.71 ms** (0.6 ms over a 1.10 ms process start; done, §4.1) | ≤ 1 ms over process start | > 100× |
| `emit` | 0.30–0.74 ms (0.139 ms best of 30 on the M4) → **0.079 ms** after §4.2; the rest is §4.3's page faults | ≤ 0.07 ms | 8× |
| machine code | 1,116 B → **40 B** (secret; done, §19) | ≤ 140 B | 8× |
| executable, macOS arm64 | 16,688 B | **floor measured: ~16.6 KB** (dyld required, `__LINKEDIT` 16 KB-aligned; context.md §18). 2 KB applies to Linux/bare metal | — |
| executable, Linux / bare metal | 1,316 B → **240 B** (secret, arm64 Linux; 176 B of it is the ELF header and its two program headers) | ≤ 170 B | 8× |
| program RAM | 4,576 B → **64 B** (secret; done, §19, §20) | ≤ 458 B | 10× |
| compiler memory taken | 256 KB → **144 KB** (§4.3; the rest needs regions that must not move, see context.md §15.4) | ≤ 25.6 KB (on 16 KB pages: 16 KB, one page) | 10× |
| compiler memory in use | 43.1 KB → **36.1 KB** | ≤ 4.3 KB | 10× |

Two floors are not ours to remove and are said here so no step pretends otherwise:
starting a process costs ~1–3 ms (kernel + dyld), and a macOS arm64 Mach-O that maps its
signature on its own page cannot be smaller than one page (16 KB).

---

## 4. Steps, in order of impact

Each step lists what changes, where, what it saves, and how it is verified.

### 4.1 `--run` runs the program in memory (116 ms → < 1 ms)

What fast compilers do: TCC `-run`, LuaJIT, V8. The code is already position-independent
bytes in `nat.code`. For `--run` on the host target, skip the file entirely:
- map the code read-write, copy it, `mprotect` to read-execute (`sys_icache_invalidate` on
  arm64), resolve the calls to C functions (`write`) to their addresses in this process (the
  fix-ups the Mach-O writer turns into GOT entries), call the entry as `int (*)(void)`, flush,
  exit with its result. `tools/elfrun.c` already proves the mechanism on this Mac.
- No file, no signature, no new vnode, no `fork`, no `/bin/sh`, no `remove`.
- The C backend's `--run` keeps building with `cc`, but uses `posix_spawn` instead of
  `system()` (no shell), and caches the built executable by content hash so an unchanged
  program is not rebuilt or re-validated.
- Where: `src/v2/main.c` (`build_and_run`), `backend/native/emit.c` (an in-memory finish beside
  the Mach-O / ELF writers), `backend.h` (a `run` entry for backends that can).
- Verify: every `examples/run` program through `--run` gives the same output and status as
  `-o` + exec; fuzzers in `--run` mode; end-to-end wall clock in the bench script (4.8).

### 4.2 The executable writer (emit 0.3–0.5 ms → ≤ 0.07 ms)

- **Page hashes:** the padding pages are all zeros: their SHA-256 is a constant, computed once
  (in the source). Only pages with code are hashed.
- **Hardware hashing:** SHA-256 with the CPU's instructions (ARMv8 `sha256h`/`sha256su0`, x86
  SHA-NI), with the portable version as fallback. ~2 cycles/byte instead of ~12.
- **UUID:** derived from the code's hash already computed for the page hashes, not a second
  pass.
- **No byte-at-a-time output:** the file's size is known before writing (header + code +
  padding + signature). Allocate it once (or `ftruncate` + `mmap` the output file and write in
  place, as mold does), `memset` the padding, write the header and signature into it. One
  `write`, no `realloc`.
- Verify: files byte-identical to today's (`cmp`), `codesign -v`, every program runs.

### 4.3 Compiler memory: one region, sized tables (256 KB → ≤ 25.6 KB taken, 43 → ≤ 4.3 KB used)

- **One reservation, sub-ranges inside it.** The 16 regions stay as separate growable ranges
  (they must not move), but are carved from one `mmap` reservation. Small programs keep every
  range inside the first pages, so they share pages instead of touching 16. A range that
  outgrows its share moves to its own reserved area (address space only, no copy needed when
  the reservation is laid out with gaps).
- **Tables sized from the input:** the name table, scope buckets, side tables and the
  `fn_at` / `index_of` / stack-analysis arrays are sized from the counts that are known
  (tokens, nodes, symbols of this program), not from fixed minimums or capacities.
- **Per-function scratch reused:** MIR, jumps and staging already reset per function; their
  first pages become shared scratch.
- **Tokens:** today every file's tokens stay while it is parsed. For the 10× "in use" target
  the remaining large item is the token array (254 × 12 B for this program). Option measured
  later: scan on demand with a small window (the parser needs at most two tokens of lookahead;
  a deferred body is re-scanned from its `TOK_BODY` range already). Only if the numbers say it
  is needed; it changes no behaviour.
- Verify: `--stats` before/after; peak RSS of the 2.9 MB benchmark must not grow; suite and
  fuzzers.

### 4.4 Program RAM (4,576 B → ≤ 458 B)

- The output buffer is 4 KB whenever the program writes to the system output. The compiler
  already walks the call graph for the stack; the same walk bounds the output: when no
  printing call is reached through recursion, the most bytes the program can write between two
  flushes is known (each print's maximum: constant text + 20 digits per number). The buffer is
  then exactly that (secret.modal: under 32 bytes), otherwise `output_buffer`.
- The runtime's frame estimate (240 B, an upper bound summing every runtime function) becomes
  the deepest runtime call chain (the runtime's call graph is known from its relocations).
- Verify: the instrumented stack check of §14.4 (pattern-filled double stack, highest slot
  written) extended to the output buffer.

### 4.5 Machine code (1,116 B → ≤ 140 B)

What secret.modal needs at run time is "write `255`, exit 0". The code is large because each
step is kept: a fresh struct in the frame, a field stored then loaded, calls kept or inlined
one level, a number formatted at run time. The passes below are what optimising compilers
(Cranelift's egraph, LLVM's SROA/GVN/IPSCCP, QBE's copy elimination) do, kept small:
- **MIR operands name the instruction that made them** (SSA-like, as revo and Cranelift do)
  instead of frame slots, within a function. This is the base for the next three.
- **Store-to-load forwarding and scalar replacement:** a struct that never escapes (the
  constructor's fresh value, `self` of an inlined method) becomes separate values; `self.code`
  stored then loaded is the stored value.
- **Constant folding across inlined calls**, with **dead-code removal** in the same pass (revo
  fuses fold, DCE and peephole into one walk).
- **Constant output:** a print whose arguments fold to constants is already merged into one
  string; after the above, `write(self.code)` becomes the string `255`, and `rt_write_int` /
  `rt_write_uint` are no longer reached, so the shake drops them.
- **Runtime size:** the runtime functions are compiled by clang with `-O2`; compile them for
  size (`-Os`) since they are cold, and check each stencil for code clang can shorten.
- Verify: fuzzers (scalar, struct, deep, sink) on arm64 and x86-64 against C; `--mir` diffs
  reviewed; code size of every example tracked by the bench script.

### 4.6 Executable size on macOS arm64 (16,688 B → ≤ 2,100 B, experiment)

Today `__LINKEDIT` (which holds the signature) starts on its own 16 KB page, so the file is one
page of `__TEXT` plus the signature. Experiment: `__LINKEDIT` mapped from the same file page as
`__TEXT` (its file range inside the first page, its address after `__TEXT`), so the file is
header + code + signature (~1.5 KB). If the kernel or AMFI refuses it (an earlier attempt to
drop the padding was killed at launch), 16 KB is the floor on this system and is documented as
such; Linux and bare metal already meet the target.
- Also measured: a Mach-O without dyld and libSystem (the system calls made directly, as on
  Linux). It would cut the produced program's 1.2 MB RSS and dyld's start time. Apple does
  not promise the system-call numbers, so this would be an opt-in target, if the kernel
  accepts it at all.

### 4.7 Speed of the generated code

The produced program runs in microseconds; its wall clock is the process start. For
programs that compute, the value stack is the cost: every operation loads its operands from the
frame and stores its result (the accumulator removes some). In order:
- **Superinstructions:** common pairs fused into one stencil (load-op, compare-branch,
  op-store), as CPython's JIT and LuaJIT's interpreter do. No new compiler architecture.
- **Compact holes everywhere:** arm64 uses one `movz` per hole when the value fits 16 bits
  (done for some stencils); extend to all.
- **Register allocation over MIR** (linear scan): values in registers, frames only for what
  spills. The largest gain for compute-heavy code and the base of the Cortex-M backend. This was
  step 4, skipped by decision; it stays here as the one item that needs your go-ahead.

### 4.8 Measurement, kept in the tree

A `make bench` script: cold and warm compile (`--time`, `--repeat`), end-to-end wall clock for
`-o` + exec and `--run` (best of N), page faults and peak RSS (`time -l`), code / file / RAM
sizes from `--stats`, for every example and the 2.9 MB benchmark. Each step above records its
before/after here and in context.md.

---

## 5. Later studies (design, before code)

These come after the numbers above. Each will be one argued proposal tied to Modal's goals,
not a list of options.

### 5.1 Memory safety without a garbage collector or a borrow checker: scoped ownership

A direction to study and test against real programs, built from what Modal already has:
- **Values first:** structs are values (passed and returned by value, §14.1); most data never
  needs a reference.
- **References are second class** (Hylo / mutable value semantics): a reference can be a
  parameter, `self` or a context, but cannot be stored in a field or a module binding, or
  returned (except `&self` into storage the caller owns, as constructors do today). Then a
  reference can never outlive what it points to, with no lifetime annotations.
- **Heap memory comes from an allocator context** (`arena: arena`, already the context
  parameter rule). Memory is released where its allocator's scope ends: one known point, the
  end of the block that declared the arena. No `defer`, no destructor calls hidden among
  statements, no `deinit` to remember. The allocator is the switchable memory model AGENTS.md
  asks for (arena, general heap, a collected arena), chosen by the caller.
- **Resources that are not memory are linear** (Austral): a file or socket must be consumed
  (closed, handed on) exactly once; forgetting is a compile error at the end of the scope, with
  the line. Explicit, but the compiler tells you, instead of you declaring cleanup everywhere.
- **Long-lived graphs** use handles from std (index + generation, checked: Vale's generational
  references as a library type), not raw references.
- **Bounds:** slice indexing checked, the check removed where the existing constant-bounds
  analysis proves it, and an explicit unchecked form for hot paths.

Why it fits: predictable (every release point and check is visible in the source), fast (no
collector, no reference counts), and light to write (no lifetimes, no `defer` boilerplate).

### 5.1b Compile-time evaluation

Its own plan: `docs/comptime.md` (`comptime e` at the use, evaluated by an interpreter over the
MIR, compile time as a target with no capabilities).

### 5.2 Seeing the machine (C-like transparency)

`--asm`: each source line, its MIR, and the exact machine code of each stencil, side by side
(the backend knows every boundary). The cost model is documented: every operation maps to a
known instruction sequence, so an experienced reader can predict the assembly, as in C.

### 5.3 C interop

Layout already matches C and is checked by the C compiler (§14.5). Next: declaring C functions
and calling them from native code (the Mach-O import path already calls `write`), a thunk
stencil between the value-stack frames and the C calling convention, and exporting Modal
functions with a generated C header.

### 5.4 Assembler and linker

Modal writes executables directly; its "linker" is the shake and the fix-ups. Lessons from mold
that apply: know every output size first and write once into the mapped output file; process
independent units (functions) in parallel and place them by prefix sums; intern strings
concurrently. Plus incremental builds: code bytes cached per function by content hash, so a
rebuild re-emits only what changed. An embedded assembler for `asm` blocks in std, validated,
comes with §5.5.

### 5.5 Everything raw lives in the language

Today the runtime (output buffering, number formatting) is C compiled into stencils. Once the
language can express loops over bytes, it moves to std written in Modal, so the user can read and
change it; the stencils keep only instruction templates. Same for the system layers (Linux
system calls, bare-metal `_start`).

### 5.6 Developer experience, notes to discuss

- Control flow is only pattern clauses and `and`/`or` today; conditional expressions and loops
  are the largest gap for new users (a design question for you, not a default).
- Messages are already specific; an `--explain CODE` page per error, and the checker reused for
  an editor server (it is fast enough to run on every keystroke), are the next steps.

---

## 6. Next three actions

1. ~~§4.1: `--run` in memory, plus `posix_spawn` for the C backend.~~ Done (context.md §15.1).
2. ~~§4.2 and §4.8: the writer and the bench script.~~ Done (context.md §15.2, §15.3).
3. ~~§4.3: one reservation for the regions, sized tables.~~ Done as far as the ground rule allows (context.md §15.4).

~~Then §4.4, §4.5 (the MIR rework), §4.6 (experiment), §4.7's register allocation.~~ Done
(context.md §18, §19, §20). Left of them:
- §4.5's SSA-named operands: liveness over slots served the allocator, so they were not needed.
- Narrow fields not at a slot's start: MIR has no bit insertion.
- Recursive programs still take `stack` (512 KB by default) for their value stack: their
  depth is not known when compiling.
