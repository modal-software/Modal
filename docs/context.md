
# Modal v2 compiler: full context export

Exported 2026-11-11. This document is the complete state of the work on the Modal v2
compiler: what was asked, what was decided and why, what exists, how it was verified, what
is unfinished, and (in the appendix) the full source of every file. It is written so that a
person or a model with no access to the original conversation can continue from it.

- Repository: `modal-software/Modal`, branch `feat/vars` (base commit `a41637d`).
- Nothing was pushed. All work was delivered as `modal-v2-frontend.zip` (46 files by repo
  path) and `modal-v2-frontend.patch` (same content, for `git apply`).
- `src/v1` is untouched. v2 lives in `src/v2` and does not depend on v1.
- Built and tested on Linux with gcc and clang. The owner builds on macOS (arm64); that
  build links and runs, but the test suite was not run there by the author of this export.

```bash
unzip -o modal-v2-frontend.zip              # at the repo root; always apply the whole zip
make v2 && make test_v2                     # 80 checks
./modal-v2 --run examples/v2/run/counter.modal
```

---

## 1. What the owner wants

Stated across the conversation, in priority order as it evolved:

1. A very fast, low-memory compiler for Modal written in pure C. "KB scale" for small
   programs; 16 MB reserved up front was called "a lot".
2. Inspirations: **QBE, TCC, Cranelift and Go**. Prefer their techniques.
3. Flexible function syntax: several declaration forms for the same thing.
4. Everything private by default, with a visibility mechanism that is **none** of:
   - `pub` / `private` keywords
   - section labels (`pub:` / `mut:`, the V approach)
   - upper/lower case conventions
   - an `export { ... }` block or any list of names
   - nested interface declarations
   - anything resembling a C header (signatures written twice)
5. `mod.modal` in a folder holds "declarable configs of module behaviour".
6. Import macros with selectors, wildcards, destructuring and `as` aliases.
7. A struct's members should not sit far away from the struct.
8. Code generation with **C as the first backend**, behind a **registry of backends**
   modelled on how good compilers manage targets (Go especially).
9. Later: a TCC-like "skip the tree" design, but safe.

Stated but not yet acted on:

- "`void` and `nil` should be the same thing."
- "The types should be more customized." (Meaning not yet clarified; see section 10.)

---

## 2. The language as implemented

### 2.1 Declarations: where the type sits decides mutability

| Form | Meaning |
|---|---|
| `name: T` and `a, b: T` | immutable field(s) |
| `T name` | mutable field (one name per declaration) |
| `name: struct { ... }` | struct: a domain of fields and methods |
| `name(params): T { ... }` | immutable method, defined once |
| `T name(params) { ... }` | mutable method, may be overloaded |
| `T name(params): expr` | mutable method with a one-line body |
| `name := (params) T { ... }` | binding that holds a function; defined once |
| `fun name(params) T { ... }` | keyword profile of the immutable method |
| `name := value` | binding (constant, import, function) |
| `val (a as b, c): import(x).y` | destructuring import |

The form is chosen from the first two tokens; nothing is parsed twice:

- identifier then `:=` → binding
- identifier then `:` or `,` → type-last field(s) or struct
- identifier then `(` → immutable method
- anything else → parse a type, then a name; `(` after the name makes it a mutable method

Consequences:

- `:` after a parameter list means *return type* in the name-first form and *expression
  body* in the type-first form. One-line bodies exist only for type-first methods.
- A type applies backwards to the untyped names before it: `(n3, n4: u8)`. `self` is the
  only parameter that may stay untyped and never joins a group.
- Type-first takes **one** name: `u8 n1, n2` is rejected; write two declarations.
- Members are separated by `,`, `;` or a newline. Statements end at a newline, `;` or `}`.
  Two statements on one line with no separator is an error.
- An expression continues across a newline only inside `( )` / `[ ]` or before a leading `.`.
- Comments: `-- line` and `-{ block }-`.
- Lua-style call with one string argument: `print "hello"`.
- Types: `u8 u16 u32 u64 i8 i16 i32 i64 int bool`, `[]T` (`[]u8` is the string type),
  `*T`, struct names, dotted paths (treated as external).
- String escapes: `\n \t \r \0 \\ \"`. Anything else is an error.
- There is **no control flow** yet (`if`, loops). Locals are declared like module-level
  variables (`name: T = v`, `T name [= v]`) or bound with `name := expr`, which is immutable
  (§16).

### 2.2 Who owns a method

- Declared inside a struct body → that struct.
- Declared at top level with `self: T` → struct `T` of the same file.
- Declared at top level with a bare `self`, or `self: <module name>` → the module (file).
- If a struct and its file share a name (`counter.modal` declaring `counter: struct`), the
  struct wins.

`T name(...)` definitions with the same name form an overload set, selected by **argument
count** (not types). Two members may not have the same parameter types. `name(...): T` and
`name := (...)` reject a second definition.

### 2.3 Semantics decided during the work (owner should confirm)

- **`self` is a reference.** `&self` and a return type written `self` both mean a pointer
  to the struct. This makes the owner's `new(self: T): self { return &self }` valid.
- **`T.new()` is how a value is created.** Calling a method on the type name runs it on a
  fresh zero value. The language had no other construction syntax.
- **`print`** (from `import("std").io.print`) separates arguments with a space and ends
  with a newline; **`write(...)`** prints its arguments as given. `print` is the one `std`
  function the compiler knows.
- **`u8` arithmetic wraps at 256 at every step**, as the type says.
- **A field and its accessor may share a name**: `u8 n1` and `u8 n1(self: T): self.n1`.
  `x.n1(...)` is the method; `x.n1` is the field where the field can be seen.
- `x := 1` makes `x` an `int`. An integer literal becomes whatever integer type it meets.
- An immutable field (`name: T`) cannot be assigned to.

---

## 3. Visibility: "a body hides its inside, its face is written just outside"

One rule, three levels. No keyword, label, casing, list or header.

| Body | Inside (private) | Outside (the face) |
|---|---|---|
| struct `{ }` | fields and helper methods | methods below it in the same file, with `self: T` |
| file (module) | its bindings and free functions | what the folder's `mod.modal` binds or defines |
| folder (domain) | its files | what the parent folder's `mod.modal` binds |

```
-- example/john.modal
john_nested_class: struct {
    u8 n1,                                   -- inside: private
    check(self): u8 { ... }                  -- inside: private helper
}

add_normal(self: john_nested_class, n3: u8): u8 { ... }   -- outside: public
u8 n1(self: john_nested_class): self.n1                   -- accessor
```

```
-- example/mod.modal
implicit_self := false        -- configuration
john := import("./john")      -- john is part of the domain's face
```

Rules:

- Fields are never visible outside their struct. A type crosses a domain boundary as an
  **opaque type**.
- Files of one domain share their module-level names; a file also sees the domains above it.
- A domain can be imported from its parent folder and from anything below that parent.
  Further out it is reached only through the parent's `mod.modal`.
- A file of another domain can be imported only if that domain's `mod.modal` binds it.
- These rules hold however a path is written: as an import, as member access on a binding,
  through an alias of a binding, at top level or inside a body. (Four leaks of exactly
  these kinds were found by the owner and fixed; `examples/v2/errors/leak.modal` holds the
  regression cases.)
- Inside a single file the rule is enforced through types: `value.field` and
  `value.helper()` are rejected outside that struct's own methods.

### 3.1 `mod.modal`

- `name := <literal>` is **configuration**; `name := import(...)` **binds a module** into
  the domain's face.
- Config names are a closed set (`sema/module.c`): `implicit_self`, `nil_return`, `profile`,
  `version`, `stack`, `output_buffer`, `max_code`, `max_ram`, `flash_origin`, `flash_size`,
  `ram_origin`, `ram_size`. An unknown name or wrong value kind is an error.
- Acted on: `implicit_self` (a bare `n1` in a method resolves to `self.n1`), the budgets and
  sizes (§14.4), the memory map (§14.6). `nil_return`, `profile`, `version` are accepted only.
- Config is not inherited by subfolders yet.

### 3.2 Contract

`modal-v2 --sig file.modal` prints per domain what other domains can rely on: config, bound
modules, opaque types and face signatures. It is derived from the code. It is printed only;
writing it to a file, diffing it against a previous one, and compiling against it are not
implemented.

### 3.3 Alternatives that were considered and why they were dropped

| Idea | Source | Outcome |
|---|---|---|
| `export { ... }` block | earlier model's Phase 3 | owner dislikes it |
| Name lists in `mod.modal` (`import("./john").T.(a, b)`) | first implementation here | same idea as `export`, removed |
| Declaration form decides (`:=` private, declared public) | Elixir `def`/`defp` | proposed, not chosen |
| Hand-written interface with nested structs in `mod.modal` | OCaml `.mli` | "nesting is painful" |
| Flat signatures in `mod.modal` | OCaml, flattened | "similar to a C header" |
| Definitions living in `mod.modal` | Go `internal/` inverted | puts methods far from their struct |
| **Inside / outside the braces + module bindings + derived contract** | Go location rule + OCaml opaque types + Newspeak/Smalltalk private data | **chosen and implemented** |

Known costs of the chosen design: visibility is not written on the line itself; changing
it is a move across the closing brace; a top-level `self: T` method is public to the domain
as soon as it is written; no public fields; only three levels (no "friend").

---

## 4. Imports

`import` is a comptime macro and an ordinary expression:

```
std := import("std")                  -- by string
std := import(std)                    -- by symbol
io  := import("std").io.*             -- everything under a submodule
fp  := import("std").io.(file, print) -- a list
import("std").io.(file as f, print as p)        -- list with aliases, bound directly
val (file as f, print as p): import(std).io     -- the same, declaration style
```

Resolution:

- A **path** (`./x`, `../x`, `a/b`, `x.modal`) is looked up next to the importing file:
  `x.modal`, then the folder `x/mod.modal`.
- A **bare name** (`example`) is looked up there and then in every folder above, so it
  means the same anywhere in the project, including inside `example/` itself.
- An import that resolves to nothing is an error.
- `std` is reserved for the external standard library: no file lookup, members not checked.
- A malformed item in an import list is reported and skipped on its own.

---

## 5. Compiler architecture

### 5.1 Pipeline: one function at a time

A hybrid of TCC (emit while parsing, keep nothing; fast but unsafe for forward references)
and a classic compiler (keep everything; safe but heavy).

1. **Scan.** Declarations are tokenized. At a function body the scanner reads raw text to
   find the matching `}` (only strings, comments and braces matter) and emits **one token
   for the whole body**, `TOK_BODY`, while counting an upper bound of the body's tokens.
2. **Parse and declare.** The parser builds declarations and signatures. Every name, type
   and signature in the file is now known.
3. **Link.** Imports are resolved (loading more files), `mod.modal` files are read.
4. **Per function, in source order:** tokenize the body into the tail of the token array,
   parse it into the tail of the node array, check module paths and types, hand it to the
   backend, cut both tails off.

Both arrays are trimmed after step 2 to the declarations plus room for the largest body,
so they never grow or move afterwards. `--eager` selects the classic pipeline (everything
tokenized and parsed up front, tree kept); `--ast` implies it.

### 5.2 Data structures

| Thing | Size | Notes |
|---|---|---|
| `Token` | 16 B | pointer into source, 24-bit length, 8-bit kind, 31-bit bracket partner, 1-bit "first on line". Line/column are recomputed only for diagnostics. |
| `AstNode` | 24 B | kind, flags, aux, token index, three child slots, sibling link. Index-linked, never pointers. |
| `Symbol` | 24 B | name token, kind, flags, unit, owner, declaring node, overload/scope link, bucket link |
| `Type` | 8 B | interned; primitive ids are fixed, so comparing types is comparing integers |

- One growable **arena** (4 KB blocks, freed at once). Token and node arrays are malloc'd
  because they grow while a file is read.
- Scopes are hash tables sized from their declaration count.
- The checker leaves two per-node tables for the backend: `unit->types[node]` and
  `unit->refs[node]`.

### 5.3 Files

| File | Lines | Role |
|---|---|---|
| `src/v2/main.c` | 290 | driver, flags, `--run` |
| `src/v2/compiler/cx.h`, `cx.c` | 198, 172 | context, `Unit`, arena, diagnostics, phase timing |
| `src/v2/compiler/syntax/scanner.h`, `scanner.c` | 141, 640 | tokens, body skipping, on-demand body tokenizing |
| `src/v2/compiler/syntax/parser/parser.h`, `parser.c` | 82, 199 | parser core, recovery, deferred bodies |
| `src/v2/compiler/syntax/parser/parse_decl.c` | 576 | declaration classifier, fields, structs, methods, `val` |
| `src/v2/compiler/syntax/parser/parse_expr.c` | 348 | Pratt engine with NUD/LED table, imports, function literals |
| `src/v2/compiler/syntax/parser/parse_stmt.c` | 85 | blocks and statements |
| `src/v2/compiler/ast/ast.h`, `ast.c` | 113, 328 | nodes, printer, signature rendering |
| `src/v2/compiler/ast/visitor.h` | 92 | iterative tree walker (used by `--ast`) |
| `src/v2/compiler/ast/symbol.h`, `symbol.c` | 87, 187 | symbols, scopes, symbol dump |
| `src/v2/compiler/sema/resolve.c` | 376 | declaration pass: ownership, overload sets, inside/outside flag |
| `src/v2/compiler/sema/module.c` | 887 | file loading, domains, `mod.modal`, visibility, contract |
| `src/v2/compiler/sema/types.h`, `types.c` | 63, 131 | interned type table |
| `src/v2/compiler/sema/check.c` | 1295 | names, invocations, types; drives the per-function pipeline |
| `src/v2/compiler/backend/backend.h`, `registry.c` | 54, 111 | backend interface, registry, streaming |
| `src/v2/compiler/backend/c/emit.c` | 813 | the C backend |
| `src/v2/tests/run.sh` | 191 | 80 checks |
| `docs/v2-frontend.md` | 297 | rules and design, shorter than this export |
| `examples/v2/**` | | example domain, runnable programs, error cases |

### 5.4 Backends

The registry follows Go's `archInits` (`cmd/compile/main.go`): a table from a name to an
`Init` function that fills one struct of data and function pointers.

```c
static const struct { const char *name; void (*init)(Backend *b); } backend_inits[] = {
    {"c", backend_c_init},
};

typedef struct Backend {
    const char *name, *summary, *extension, *toolchain;
    void (*begin)(const BackendJob *job);              // types, globals, prototypes
    void (*function)(const BackendJob *job, SymId fn); // one checked function, body in memory
    int (*end)(const BackendJob *job);                 // entry point; returns its error count
} Backend;
```

A backend is streamed to: it must emit each function when handed it, because the body is
discarded afterwards. Adding a backend is one directory with an `Init` and one table row.

**C backend mapping:**

| Modal | C |
|---|---|
| `name: struct { ... }` | `struct modal_name { ... }` |
| `f(a: u8): u8 { }` | `static uint8_t modal_f(uint8_t v_a)` |
| `m(self: T, a: u8): u8 { }` | `static uint8_t modal_T_m(modal_T *self, uint8_t v_a)` |
| `u8 f(a: u8)` and `u8 f(a, b: u8)` | `modal_f__1` and `modal_f__2` |
| `x := expr` | `<type> v_x = expr;` |
| `value.m(a)` | `modal_T_m(&value, a)` |
| `T.m(a)` | `modal_T_m(&(modal_T){0}, a)` |
| `main(): u8` | `int main(void) { return (int)modal_main(); }` |

Not translated yet (each is an error at the construct, and no output file is left behind):
imports of other Modal files, function literals inside functions, module-level functions
that take `self`, structs nested in structs, slices other than `[]u8`, indexing, operators
on strings. So `examples/v2/main.modal` and `examples/v2/calls.modal` check but do not
compile to C; `examples/v2/run/hello.modal` and `counter.modal` compile and run.

### 5.5 Command line

```
modal-v2 [options] file.modal
  (no option)      check only: errors, or one line saying what was verified
  -o FILE          generate code into FILE
  --run            generate, build with the backend's toolchain (cc, or $CC), and run
  --backend NAME   backend to generate with (default: c); also -b NAME
  --backends       list the registered backends
  --target NAME    arm64-macos, arm64-linux, x86_64-macos, x86_64-linux, arm64-none, x86_64-none
  --ast            print the syntax tree of every loaded file (implies --eager)
  --syms           print the symbol table with visibility
  --sig            print the derived contract of every domain
  --stats          print token, node, symbol and memory counts
  --time           print the time spent in each phase
  --eager          parse every body up front and keep the whole tree
  --repeat N       compile N times in one process (measuring; --time shows the fastest)
  --mir            print the MIR of every function generated
  --cache          skip checking files that did not change (check-only runs)
```

Makefile targets: `make` (builds `./modal`), `make test`, `make bench [N=…]`, `make stencils`.

---

## 6. Performance and memory

Benchmark: a generated 3.2 MB program (168,000 lines, 60,000 declarations). Timings on the
development machine vary by about 15% between runs.

| Stage of the work | Check only | Check + generate C | Peak memory |
|---|---|---|---|
| First backend version | about 95 ms | about 165 ms | 55 MB |
| After tuning (eager pipeline) | 54 ms | 90 ms | 46.4 MB |
| One function at a time (current default) | 48 ms | 68 ms | 26.6 MB |

Small programs: `counter.modal` (1.2 KB) compiles in about 0.05 ms using 16 KB. Before the
arena rewrite the compiler reserved 16 MB regardless of input.

What was done, and the source of each idea:

- Growable block arena instead of a fixed 16 MB slab; 16-byte tokens (were 32).
- One-pass scanner with a 256-entry character-class table; keywords by length (TCC).
- Linear sweeps over the flat node array instead of tree walks (Cranelift's flat IR).
- No printf in the emitter: one 64 KB buffer of raw copies.
- No per-function state copies in the checker.
- Line lookup for diagnostics resumes from the last position (thousands of errors in a
  large file used to take 8 seconds); at most 20 errors are printed in full.
- One function at a time, bodies as one token (the TCC hybrid).

Honest limits: the one-function pipeline executes about 9% **more** instructions than the
eager one (it reads each body twice) and is faster only because it touches less memory. No
order-of-magnitude gain remains in a single-threaded, from-scratch run. Larger gains need:

- **Parallelism** across files (needs a per-thread arena).
- **Not redoing work**: write the contract to disk and skip unchanged domains (Go's
  export data). On a real project this beats any single-pass speedup.
- **A native backend**: today `--run` spends nearly all its time in `cc`. Emitting QBE's
  input language would be the cheapest route to native code on several architectures.
- A small typed intermediate language between checker and backends (QBE/Cranelift style),
  which a native backend would need anyway.

---

## 7. Verification

- `make test_v2`: 80 checks covering syntax forms, visibility (including the leak cases),
  config, contract, invocations, types, the backend registry, compiling and running two
  programs with expected output, generated C building with `-Wall -Wextra -Werror`, and
  equality of the two pipelines' output.
- AddressSanitizer + UndefinedBehaviorSanitizer builds, with mutation fuzzing of every
  example (tens of thousands of inputs across the work): no crash or hang in the final state.
- Backend fuzzing: thousands of mutated programs accepted by the front-end were each
  compiled with gcc. This found five bugs where accepted Modal produced invalid C
  (multi-line strings, `print` or `write` used as a value, `-` on a string, a bad escape);
  all were fixed at the checker.
- Differential fuzzing of the two pipelines over about 8,000 mutated programs: they never
  disagreed on accepting or rejecting a program and generated byte-identical C. In files
  that already have syntax errors the error *lists* can differ (the default pipeline still
  checks functions that parsed; with broken brackets it can report fewer follow-on errors).

---

## 8. Chronology of the work

1. Owner supplied a retrospective from a previous model plus the pending inputs: struct
   syntax, nested class syntax, encapsulation ideas, top-level module, import forms, method
   forms. (Reproduced in Appendix A.)
2. Design review: the single mutability rule, the declaration classifier, contradictions
   found in the spec.
3. Owner supplied the previous model's four code phases and pointed at `feat/vars`. The
   existing `src/v2` did not compile; it was rewritten: scanner, parser, AST, symbols,
   iterative walker, module loader, with visibility through name lists in `mod.modal`.
4. Visibility redesign through several rounds of discussion (section 3.3), ending in
   inside/outside, implemented with config validation and the derived contract.
5. Owner found visibility leaks (member access on bindings, aliases, imports in bodies,
   unresolved paths, bare names). All fixed.
6. Memory: 16 MB slab removed; tokens halved. Invocation checking added.
7. Type checker, backend registry, C backend, `--run`.
8. Performance pass with `--time` and a profiler.
9. One-function-at-a-time pipeline; bare-name imports search upward.

Recurring practical problem: the owner applied individual files and three times one file
stayed behind (`resolve.c`, the `makefile`, `check.c`), producing linker errors. Each round
changes many interdependent files; apply the whole zip.

---

## 9. Known limits and unfinished work

- No control flow (typed locals exist since §16).
- Overloads are selected by argument count, not argument types.
- The implicit `nil` in return types is not modelled; `nil` and "returns nothing" are two
  type ids (`T_NIL`, `T_VOID`).
- A method can attach to a struct of the same file only.
- The C backend handles one file and the constructs listed in 5.4.
- The contract is printed only.
- Config: only `implicit_self` has an effect; no inheritance.
- `std` members are not checked; only `std.io.print` is known.
- A local variable named like an import binding is treated as that binding when it starts
  a module path (can cause a false error, never a leak).
- A struct field named like a C keyword would produce invalid C.
- v1's `test` / `assert` are not ported.
- Not verified on macOS by the author beyond the owner's own runs.

---

## 10. Open questions for the owner

1. **`nil` / `void`**: merge them? (Proposed: a function with no return type returns `nil`;
   `return` and `return nil` are the same; C still emits `void`.)
2. **"Types should be more customized"**: which of these is meant?
   - new kinds of user types (aliases, distinct types, enums, unions)
   - control over representation (sizes, alignment, packed layout)
   - typed overloads and typed locals
3. Confirm the semantics in section 2.3 (`self` as reference, `T.new()` construction,
   `print` / `write` behaviour, struct-over-module naming).
4. Should type-first declarations allow several names (`u8 n1, n2`)?
5. Should `implicit_self` default to true or false? (Currently false.)

---

## 11. Suggested next steps

In the order that unlocks the most:

1. Control flow (`if`, a loop) so programs can do real work.
2. Multi-file code generation in the C backend (so `main.modal` compiles).
3. A small typed intermediate language, then a QBE-text backend for native code.
4. Contract file on disk: breaking-change detection and skipping unchanged domains.
5. Overload selection by argument types.
6. Parallel scanning and parsing of files.

---

## 12. Sources consulted

- Go: `cmd/compile/main.go` (`archInits`), `cmd/compile/internal/x86/galign.go` (`Init`
  filling `ssagen.ArchInfo`); the `internal/` directory rule.
- LLVM: `TargetRegistry.h` (targets registering themselves, lookup by triple).
- QBE: the commit introducing the `Target` structure.
- Rust Book: module privacy between parents and children.
- Newspeak: no global namespace, instance variables reachable only through accessors.
- Odin (`#+private` file tag) and Jai (`#scope_file`), as examples of label-based schemes
  that were ruled out.

---

## 13. Changes after the export (2026-11-11)

- **Regression fixed:** `parse_decl.c` had been left at its pre-TCC-hybrid version, so every
  `name(...): T { }` failed on the deferred `TOK_BODY`. Restored from Appendix B; also restored
  `tests/run.sh` (paths now `examples/…`, no `v2/`) and `examples/example/inner/up.modal`.
- **File loading without `realpath`:** `realpath` costs 100–200 µs per call on macOS and was
  93% of compile time for small programs. Paths are now normalized lexically against a cached
  `getcwd`. Files are identified by `(st_dev, st_ino)`, so a file reached through a link still
  loads once. Opening is the existence test (no `access` probes), and each domain's
  `mod.modal` is looked for once. One difference: a `..` after a symlinked folder is now
  resolved lexically.
- **Buffered diagnostics:** stderr is fully buffered (64 KB) and flushed before stdout output
  and child processes.
- **Memory: regions instead of malloc and arena blocks.** `cx.c` reserves address space per
  region (`mmap`, pages supplied on first write): arena, tokens, nodes, symbols, types,
  scopes. The files' token and node arrays follow one another in their region and grow in
  place, so the scanner has no growth path (`len + 2` tokens is a strict bound) and no array
  is ever realloc'd or copied. `--stats` reports touched pages.
- Generated C marks its runtime helpers `__attribute__((unused))` (clang's `-Werror` check).

| `--time`, min of 30 runs on the owner's Mac | before | after |
|---|---|---|
| `examples/calls.modal` total | 0.245 ms (0.87 ms with the parse errors) | 0.042 ms |
| of which link | 0.213 ms | 0.012 ms |
| 2.9 MB generated program | 17.7 ms, 36.8 MB RSS | 16.6 ms, 26.7 MB RSS |

### 13.1 Second round: steps 0–7 of the performance plan

- `--repeat N` compiles N times in one process and `--time` shows the fastest run (steady
  state, no page faults). All numbers below are min-of-50 on the 2.9 MB benchmark.
- **Interned names** (LuaJIT, Go): the scanner interns every identifier as it reads it,
  storing the `NameId` in the token's `match` field, which identifiers never used. Keywords
  are interned first and marked with their token kind, so one hash lookup does both
  keyword detection and interning. Scopes, locals and lookups compare integers.
  The word-at-a-time hash needs a real finalizer: a plain product left the low bits
  (the table index) depending on the low bytes only, and made everything 2× slower.
- The overload error text is built only on failure, and domain base names are cached.
- **Body skipping** no longer counts tokens (a body's byte length bounds them). It finds
  `{ } " - \0` 16 bytes at a time: NEON on arm64, SSE2 on x86-64, both baseline, so no
  AVX2 dispatch is involved.
- `node_new` is inlined (the `ast.new.node` function-pointer call could not be).
- `-O2 -flto` (about 7%). Tried and dropped: prefix bytes in intern slots and `-O3`
  (no gain).
- **`--cache`** (Go's build cache, per file, opt-in): skips the bodies of files whose
  text, and the declarations of every other file, are unchanged. Clean results only;
  check-only runs. Verified by 800 cached-against-uncached comparisons under random edits.
- Total: 13.6 → 10.8 ms (8.8 ms with a warm cache); `calls.modal` 17–21 µs warm.

### 13.2 The native backend: copy-and-patch AOT (`--backend native`)

```
build:   stencils.c --clang (ELF, arm64)--> stencilgen --> stencils_arm64.h (in the tree)
compile: per checked function, choose a stencil per node by its exact type, copy it,
         fill its holes; then write a Mach-O object; `cc` links (and signs) it
```

- One stencil per typed operation, for 8 integer types (`add_u8`, `div_i32`, …), plus
  moves, field access, calls, branches and output. The runtime (`rt_*`) is copied once.
- Frame of 8-byte slots: `x0` holds the frame and `x1` the output buffer through tail
  calls. A call copies its arguments into the window above the frame, which becomes the
  callee's frame.
- JIT techniques applied ahead of time:
  - stencils chosen by exact type, with no guards;
  - the branch to the next stencil is dropped (fall-through);
  - backpatching for calls to functions not emitted yet, and for frame-size holes.
- Strings are placed inline after an `adr`, so no absolute address is written anywhere.
- Integer semantics match the C backend's output exactly, including C's integer
  promotions and conversions for `/` and comparisons, and literal-only expressions
  computed in C's own types.
- Verification: the differential test (random programs over all integer types and
  operators, overloads, comparisons, `and`/`or`) gives identical output and exit status
  on both backends. A feature test covers structs, constructors, strings with escapes
  and short-circuit side effects. ASan/UBSan are clean.
- 2.9 MB benchmark: machine code for 54,000 functions in ~11.5 ms. A linked executable
  takes ~60 ms in total, against ~330 ms through C.
- Not done yet:
  - shorter holes (each is 4 instructions today), register variants and fused stencils;
  - writing the executable directly (Mach-O with an ad-hoc signature);
  - x86-64 stencils;
  - control flow.

### 13.3 MIR, compact code, executables written directly, x86-64

- **MIR** (`compiler/mir/`): each checked function is lowered to a flat array of typed
  three-address instructions over frame slots, before any backend sees it.
  - All C-semantics decisions live in the lowering.
  - It also does constant folding, immediate operands (`x + 1` is one `addi`), and results
    computed into their destination (locals, the return slot, call arguments).
  - The frame layout is final before emission, so there are no frame fix-ups.
  - `--mir` prints it. The native backends consume it through `Backend.function_mir`.
- **Code size:** for arm64, `stencilgen` also builds a compact form of every stencil, without
  the `movk` instructions of holes whose values fit in 16 bits, and re-aims the stencil's
  internal branches. On the 2.9 MB benchmark the code went 13.3 MB → 10.6 MB (MIR) →
  5.4 MB (compact).
- **Executables written directly** (`backend/native/macho.c`, as Go's linker does):
  - Mach-O `MH_EXECUTE`: `__PAGEZERO`, `__TEXT` (code plus a stub per import), `__DATA`
    (GOT), `__LINKEDIT`;
  - dyld bind opcodes against libSystem, `LC_MAIN`;
  - an ad-hoc code signature: a SHA-256 per 4 KB page, in a CodeDirectory.
  - No `cc` or `ld` is involved; `-o x.o` still writes an object.
  - Source to signed executable: under 10 ms for `counter.modal`, 60–70 ms for the
    2.9 MB program (about 330 ms through C).
- **x86-64** (`--backend x86_64`; `native` picks this machine's CPU, and `arm64` can be
  given explicitly):
  - stencils built with the small code model, so holes are 32-bit immediates and calls and
    jumps use `rel32`;
  - `const64` for wider constants, `jmp *slot(%rip)` stubs, 4 KB pages.
  - Tested under Rosetta.
- **Shared stencil ids:** names are sorted, so every CPU's header has the same ids, and
  `STENCIL_SET_HASH` checks it. The tables sit in `arch_<cpu>.c`.
- **Verification:**
  - the differential against the C backend: 200 programs on arm64 after the MIR,
    100 after the refactor, and 150 on x86-64, all identical;
  - the feature test on both CPUs;
  - ASan/UBSan clean (they caught a misaligned MIR region, now fixed).

### 13.4 Smaller code, and three language features

- **Code size and speed** (2.9 MB benchmark, arm64 / x86-64: 5.38 / 4.95 MB → 3.34 / 2.99 MB):
  - module constants fold;
  - constant print text is merged into single strings;
  - only the runtime functions used are copied;
  - `acc` stencil variants keep a value that only the next instruction reads in a register;
  - small functions are inlined from the MIR, reading their arguments in place.
- **Type-first parameters:** `u8 a` is mutable, `a: u8` immutable (the field rule), and
  assigning to an immutable parameter is an error.
- **Context parameters:** a parameter named like its struct, written type-last,
  `arena: arena`.
  - It is an immutable reference.
  - A call may leave it out: the checker inserts the caller's own `arena` (a local or a
    parameter, found lexically) as a plain argument, so the backends see a full call.
  - Errors: a context that is not a struct, reassigning one, none in scope, and the
    type-first form `arena arena`.
  - Example: `examples/run/context.modal`.
- **Literal patterns:** `u8 fact(u8 0): 1` / `u8 fact(u8 n): n * fact(n - 1)`.
  - A literal in the name slot of a type-first parameter makes a clause: integers,
    negative numbers, `true` / `false`.
  - Clauses with as many parameters are tried in source order; a call goes to the first,
    and each clause tests its patterns and otherwise passes the same arguments to the next
    (no dispatcher is generated).
  - Errors: no final catch-all, a clause after the catch-all, duplicate patterns, a literal
    that does not fit, a bool pattern on an integer.
  - Works on the C, arm64 and x86-64 backends. Example: `examples/run/patterns.modal`.

### 13.5 Memory layout: 12-byte tokens, 16-byte nodes

- **Token (12 bytes):** the text is a 32-bit offset from `token_base` (the arena's start,
  where every source is read), read through `TSTART(t)`.
- **AstNode (16 bytes):** `kind, flags, aux, tok, a, b`.
  - No third child: a function's `b` is an `AST_SIG` node holding the return type and the
    body, read through `fn_ret_node`, `fn_body` and `fn_set_body`.
  - No sibling link: a list's elements are consecutive nodes, and the last carries
    `AST_F_LAST`, so the next sibling is `NEXT(u, id)`. Every node starts as a list of one
    (`LAST` set).
  - The parser stages each list's elements (`list_begin`, `node_list_push`, `list_at`,
    `list_finish`, `list_abort`) and lays them out when the list ends. An element that was
    the last node made is moved (its slot is reused); otherwise its old slot becomes
    `AST_NONE`. A staged element is reached through `list_at`, never its old id.
  - Grouped fields (`a, b: T`) are pushed straight into the enclosing list by
    `parse_declaration(p, ctx, into)`.
- **Native lists:** the backend's fix-up lists live in their own regions and grow in place
  (doubling in the arena had left every old copy behind). The inliner's kept bodies are one
  allocation each.
- `--stats` prints memory per region.
- **2.9 MB benchmark:**
  - check: 28.4 → 25.0 MB peak RSS;
  - native build: 42.7 → 36.0 MB;
  - tokens: 9.3 → 7.0 MB;
  - nodes: 7.6 → 5.7 MB resident (366,000 nodes, including one signature node per function).

## 14. Toward embedded targets

(The performance and memory plan that follows this work, with its measurements, is
`docs/p.md`.)

Roadmap agreed with the owner: (1) target description and struct values, (2) ELF output,
(3) a runtime made of context parameters, (5) memory budgets checked at compile time,
(6) hardware features (memory-mapped registers, interrupts, explicit layout). Step 4, a
Cortex-M backend with register allocation, is deferred.

### 14.1 Target description, C layout, struct values (step 1)

- **`MachineTarget`** (`cx.h`, `sema/layout.c`): pointer size, word size, alignment of 64-bit
  integers, byte order, whether 64-bit arithmetic is native. `target_get()` returns the one
  set, or the host's. Nothing else may assume 8-byte pointers or little-endian memory.
- **Layout:** `type_layout(t)` gives size and alignment; `field_offset_of(field)` gives C's
  offset. Struct sizes are cached per symbol. A native string is one pointer (to its length
  and bytes), so it has the size of a pointer.
- **Sized memory operations:** `MIR_LOAD` / `MIR_STORE` carry the field's variant, and read
  or write exactly that width (`load_u8 … load_i64`, `store_8 … store_64` stencils). Before,
  each field took an 8-byte slot. Now `{u8, i16, u32, i64, bool}` is 24 bytes, as in C, not
  40, and a two-`i32` struct fits in one slot.
- **Struct values in the native backends:**
  - A struct value takes `ceil(size / 8)` consecutive frame slots holding its C layout.
    Every struct object in memory is such a block, so a copy is whole slots.
  - Parameters take as many window slots; a result comes back in the callee's first slots
    (`MirFunc.ret_slots`). A method on a struct value receives the address of its block. A
    value with no place of its own (a call's result) gets a block in the constructor area,
    because the method may keep the reference.
  - The emitter never keeps a slot whose address is taken (`MIR_ADDR` now carries the
    block's size) in the accumulator, and treats all result slots as read on return.
  - Functions with a struct parameter or result are not inlined.
- **C backend fix:** a method called on a struct that a call returned now gets
  `(T[1]){call}` instead of the invalid `&call`.
- **Verified:**
  - new `examples/run/structs.modal`, the same on C, arm64 and x86-64; suite 118 ok;
  - a struct program generator, 250 programs the same as C (150 arm64, 100 x86-64);
  - ASan/UBSan clean;
  - checking speed unchanged (11.7 ms on the 2.9 MB benchmark).

### 14.2 Linux: static ELF, no libc (step 2)

- `--target NAME` picks machine and system: `arm64-macos`, `arm64-linux`, `x86_64-macos`,
  `x86_64-linux`. A target is built by the native backend for its CPU. `--run` refuses a
  target this machine cannot run.
- `backend/native/elf.c` writes the smallest executable the kernel runs: the ELF header,
  one `PT_LOAD` (read+execute, the whole file, 64 KB aligned so 4/16/64 KB pages all work),
  `PT_GNU_STACK` (non-executable stack). No sections, no data segment: the runtime context
  and value stack are on the stack, and string bytes are in the code.
- The system layer replaces the C library: `write(2)` is three instructions
  (`mov x8,#64; svc #0; ret` / `mov eax,1; syscall; ret`), and `_start` calls the entry and
  passes its status to `exit_group`. A call to any other C function is an error naming it.
- **Size:** hello world is 704 bytes on arm64 and 802 on x86-64. The macOS versions are 16,704
  and 4,320.
- **Testing on macOS:** `src/v2/tools/elfrun.c` loads the file as the kernel would (the
  segment, then `e_entry` on an aligned stack) and replaces only the two system calls with
  jumps to the host's `write` and `_exit`. Header, segment, every patched branch and
  `_start` run as written. arm64 runs natively, x86-64 under Rosetta; on Linux the files run
  directly. The suite runs every example this way on both CPUs; 140 fuzz programs matched C.

### 14.3 A runtime made of context parameters (step 3)

- **The program's own output.** A struct named `out` with `put(self: out, b: u8)` receives
  every byte that `print` and `write` produce, in order, from the `out` in scope: a local,
  or the context parameter `out: out`, which calls fill in from the caller. With no `out`
  in scope the bytes go to the target's system output. A file that declares an `out` and
  prints where none is in scope is an error, so output never goes somewhere unseen.
- `import("std").sys.put` writes one byte to the system output (a desktop `out` forwards
  to it; on a board, `put` stores to a UART register instead, step 6).
- **Target capabilities** (`MachineTarget.caps`, revo's comptime `Caps`, Zig's
  `builtin.os`): `TCAP_SYSOUT`, `TCAP_EXIT`. A program that needs what its target lacks is
  an error at that line, at compile time.
- **How it is built:** the checker rewrites a routed call with the `out` as its first
  argument and marks it `AST_F_SINK`; its ref is the `put` method. The native lowering
  writes the loops in MIR (a string byte by byte, a number into a 24-byte block of digits),
  so MIR now has loops. The accumulator pass was made loop-aware: a value read again on the
  loop's next pass stays in its slot. The C backend calls helpers through one adapter per
  `put` method.
- **Shared output routines:** the loops are not written at each call. The lowering makes one
  function per `put` method and kind of value (`out.string`, `out.unsigned`, `out.signed`,
  `out.bool`; frame: the `out` in slot 0, the value in slot 1) the first time a call needs it,
  and the registry hands them to the backend before it finishes (`mir_next_helper`). A call
  is three instructions. Dead-code removal and the stack analysis treat them like any
  function. Code: output.modal 4,376 → 3,300 B; a random program that prints a lot
  29.5 → 15.4 KB (arm64), 30.2 → 15.1 KB (x86-64). 340 sink runs the same as C, including
  `out` passed as a context through 25-call chains.
- **Fixed on the way:** the C backend printed integer literals above `INT64_MAX` as
  negative (native printed them unsigned, as C's type for them is unsigned); such a literal
  is now printed unsigned and written with a `u` suffix.
- **Verified:** `examples/run/output.modal` and `examples/errors/output.modal`; 240 sink
  runs (random programs whose output goes through an `out`) byte-identical to the plain C
  program on C, arm64 and x86-64; ASan/UBSan clean; suite 144 ok.

### 14.4 Memory budgets checked at compile time (step 5)

- **Everything a native program uses at run time is on the native stack, and all of it is
  computed:** the value stack (the deepest chain of frames, from the call graph: a callee's
  frame starts where its caller's ends; a function's own need includes the window slots its
  inlined bodies use), one machine frame per nested call (read from the call stencil's
  prologue), the runtime context and output buffer, and the runtime functions.
- **The value stack is sized exactly** (a hole of `st_entry`) instead of a fixed 512 KB:
  counter.modal takes 19 slots (152 B). A program that calls itself has no known depth: it
  gets `stack` bytes from `mod.modal` (512 KB by default), and `--stats` names a function of
  the cycle. The output buffer is `output_buffer` bytes (4 KB by default) when the program
  writes to the system output, 16 otherwise.
- **Budgets** in `mod.modal`: `max_code` (bytes of machine code: flash) and `max_ram` (bytes
  at run time). Over either is an error at `main`, with what the memory is made of:
  `needs 624 bytes of memory, over max_ram = 512 (mod.modal): value stack 160, output buffer 64,
  3 nested calls 96, runtime 240, entry 64`. Examples: `examples/budget/`.
- `--stats` on a native build prints the same breakdown.
- **Verified:** an instrumented build that gives every program twice its computed stack,
  fills it with a pattern and reports the highest slot written: 248 runs (deep call chains
  with struct values, recursive struct programs, sinks, every example), none over. Fuzzers
  re-run on the exact stacks: all the same as C.

### 14.5 Hardware: register blocks and devices (step 6)

- **Literals:** hexadecimal (`0x4000_0000`) and `_` between digits, read by one function
  (`number_value`, cx.h) everywhere a literal's value is needed. The C backend writes every
  literal in decimal, because C gives a hex literal a different type.
- **`@` places things.** In a struct, `u32 dr @ 0x00` gives a field its offset. A struct
  whose fields all have one is a **register block**:
  - its layout is exactly what it says, whatever the ABI;
  - its fields are sized numbers or bools, aligned, in order, without overlap;
  - every access is one load or store of the field's width (volatile in C).
- At module level, `uart0: pl011 @ 0x0900_0000` places a register block at an address: a
  **device**.
  - It is a constant reference to those registers, declared type-last, and only a register
    block can be one.
  - A read-only register is a type-last field (`fr: u32 @ 0x18`). The usual mutability rule
    rejects assigning it.
  - Methods on the device read and write the registers (fields stay private to their struct).
  - A program's `out` can send each byte to the device (`examples/run/device.modal`): on a
    board, print goes to the UART.
- **Layout checked by the C compiler:** the C backend emits padding for register blocks, and
  for every struct without string fields a C99 check that each `offsetof` and the `sizeof`
  equal sema/layout.c's. 101 random struct programs (about 15 checks each) compile.
- **Tested** through `elfrun --device ADDRESS SIZE`. It maps memory at the device's address,
  with each byte equal to its offset, and prints it when the program ends. Reads see the
  pattern; writes change exactly the bytes of their width (`ibrd` u16 = `ef be`, its neighbours
  untouched). The same on arm64 and x86-64.
- **Errors:** `examples/errors/devices.modal` (mixed offsets, misaligned, overlapping, a
  non-register field, a device of a plain struct, a type-first device, a misaligned address,
  a read-only register assigned).
- **Not done yet: interrupts.** A handler needs a vector table, and installing one
  (`msr vbar_el1` on arm64) needs a privileged CPU level that a user-space test loader cannot
  give. Verifying it needs an emulator (QEMU) or a board.

### 14.6 Bare metal: `arm64-none`, `x86_64-none`

- **Targets with no system:** no capabilities, so `print` needs an `out` (a UART through a
  device) and there is no exit.
- **The memory map is in `mod.modal`:** `flash_origin`, `flash_size`, `ram_origin`,
  `ram_size` (required; an error names them). flash_size and ram_size are the default
  `max_code` / `max_ram`: a program that does not fit the chip is a compile error.
- **No data segment to copy, nothing to zero:** module-level variables, the runtime context
  and every frame are on the stack. So `_start` is: the stack pointer to the top of RAM, call
  the entry, then `wfi` (arm64) / `hlt` (x86-64) forever, main's result in x0 / eax.
- `write(2)` becomes `return 0`: the checker already keeps every byte off the (absent)
  system output, so the runtime only ever flushes an empty buffer.
- **The ELF segment is the code alone, at `flash_origin`:** the image's first byte is the
  flash's first byte; the headers stay in the file for the loader. 2.3 KB for
  `examples/board/blink.modal`.
- **A recursive program on a board must say how deep it may go:** `stack` in `mod.modal`.
  With the hosted default of 512 KB, the budget rejects it and says so.
- **Tested** with `elfrun --ram ADDRESS SIZE` (maps the RAM where `_start` puts the stack) and
  `--device`. The halt ends the run with main's result as the exit status. blink.modal prints
  "blinks: 5" through `out` to a test UART that counts bytes: data register `\n`, count +10,
  status 42, on arm64 and x86-64.

## 15. Performance and memory (plan in docs/p.md)

### 15.1 `--run` in memory (p.md §4.1)

- **Why:** the first launch of a new executable file on macOS costs ~110 ms (its signature
  validated, a vnode the system has not seen). `--run` wrote a new file every time.
- **Native backend, host CPU, macOS target:** no file. `backend/native/run.c` copies the code
  into fresh memory, adds one 16-byte stub per C library function it calls (`ldr x16, #8;
  br x16; .quad` on arm64, `jmp *0(%rip); .quad` on x86-64, the address from `dlsym`), sends
  the calls the Mach-O writer would send to its GOT stubs to these, makes it read-execute,
  invalidates the instruction cache, and calls the entry (`int main(void)`, the same code as
  in the executable).
- **Same result as the executable:** the entry runs on a new thread with the stack a new
  process's main thread gets (`RLIMIT_STACK`); a fault (stack overflow, x86-64 divide by zero,
  a store to an unmapped device) ends the compiler with status 1, as `--run` reported a program
  stopped by a signal; status is `main() & 0xff`. Handlers and the alternate signal stack are
  put back afterwards (ASan's own alt stack included).
- **Interface:** `Backend.can_run` / `Backend.run`. When `can_run()` holds, the driver starts
  code generation with no output, `end()` keeps the code, `codegen_run()` runs it. Other
  targets (x86-64 under Rosetta, Linux, bare metal) are unchanged.
- **C backend:** `posix_spawn` instead of `system()` (no `/bin/sh`; `$CC` is split on blanks as
  the shell did). Built executables are kept in `$TMPDIR/modal-run/<FNV-1a of toolchain +
  source>` beside a copy of the source; a hit needs the copy byte-identical, so a collision only
  costs a rebuild. Built under a per-process name, then renamed.
- **Measured** (Mac mini arm64, best of 30, `/usr/bin/true` = 1.10 ms in the same harness):
  native `--run examples/secret.modal` 116 ms → **1.71 ms**; C backend `--run` 157 ms cold,
  **2.64 ms** on a cache hit.
- **Verified:** every `examples/run` program gives the same output and status in memory as
  `-o` + exec; suite unchanged (165 pass; the 3 failures predate it: `examples/main.modal` uses
  `nil`, `examples/add.modal` no longer parses); ASan/UBSan build clean on every example;
  258 random scalar programs (8 integer types, recursion through pattern clauses, prints,
  exit status) identical across in-memory, exec and the C backend.

### 15.2 The Mach-O writer (p.md §4.2)

- **Hardware SHA-256:** ARMv8 `sha256h`/`sha256h2`/`sha256su0`/`sha256su1` (NEON intrinsics,
  `target("sha2")`), chosen at run time (`hw.optional.arm.FEAT_SHA256` on macOS, `HWCAP_SHA2`
  on Linux), the portable rounds otherwise. 1.6–2.8 GB/s against 0.37–0.47 GB/s on the M4.
  x86-64 SHA-NI is not written: Rosetta does not provide it, so it could not be tested here.
- **Padding pages are not hashed:** a 4 KB page wholly between the code and `__LINKEDIT` is
  zeros; its hash is a constant in the source (`zero_page_sha256`, checked against `shasum`).
- **Allocated once, padding skipped:** the file's size is known before writing; the buffer
  is reserved at that size, zeroed, and padding is a skip (`skip_to`), not a call per byte.
  The signature is written in place after the bytes it hashes. The object writer and the bind
  opcodes use the same buffer rules.
- **One `write(2)`:** past stdio (fwrite of a 16 KB buffer copied it through the FILE and
  wrote twice).
- **UUID unchanged:** still the SHA-256 of the code. p.md proposed deriving it from the page
  hashes, but the UUID is inside page 0, which is itself hashed, and changing its value would
  change every file. With the hardware hash it costs ~1 µs.
- **What is left is the system:** instrumented, the writer is ~12 µs of work (allocate,
  headers, sign) and ~12 µs of `write`; `close` of the new file then costs ~30 µs (APFS), which
  the process would pay at exit anyway. The rest of `emit` (~50 µs for secret.modal) is the
  first touch of the regions while lowering: §4.3.
- **Measured** (`make bench`, best of 30, against the compiler of §15.1): emit for
  secret.modal 0.139 → **0.079 ms**, hello 0.117 → 0.058 ms; `-o` wall clock 0.05–0.15 ms
  lower on every example.
- **Verified:** 79 files (Mach-O executables and objects for both CPUs, Linux ELF, bare-metal
  images, every example) byte-identical to the previous writer's (`cmp`), `codesign -v` on
  every arm64 executable; the hardware hash equal to the portable one at every length
  0–300 B and every 997 B up to 70 KB, and to `shasum`; suite unchanged.

### 15.3 `make bench` (p.md §4.8)

- `src/v2/tools/bench.sh` (with `bestof.c`, best wall-clock of N runs via posix_spawn): for
  secret, hello, output, structs, patterns and a generated `big` (54,000 functions in chains
  of 100, ~3 MB): check warm (`--repeat`), emit, `-o`, exec of the result, `--run`, code /
  file / RAM bytes, compiler memory used / taken, peak RSS and page faults (`time -l`). Prints
  a markdown table. `make bench N=10` for a quick run (~4 s).
- The generator from the earlier 2.9 MB benchmark was not kept; `big` replaces it, so its
  numbers are not comparable with §13's.
- Seen while writing it: for `big`, `--stats` reports more memory in use (44,771 KB) than
  taken (40,496 KB). Explained in §15.4: "in use" counts capacity handed out, written or not.

### 15.4 Compiler memory: a shared page, a smaller name table (p.md §4.3)

- **The premise was measured first, and it does not hold here.** p.md §2.1 put the cost of
  the first function and of `native_begin` on first-touch page faults. Touching the first
  32 KB of every region before the timers start saves 2–4 µs of secret.modal's ~0.1 ms.
  Code generation adds ~10,000 instructions (884K → 894K for the whole process, dyld
  included) and no page faults over checking alone. The ~60 µs emit shows is cold caches and
  a CPU that has barely started, in a process that lives ~1 ms. So this step is about
  memory, not time.
- **Eight regions share one 16 KB page:** code, MIR, function ranges, calls, runtime calls, C
  library calls, branches and the list stage. Each starts in a 1–2 KB slice of it, and on the
  first push that does not fit, moves to its own reservation. Allocations already made in the
  slice stay where they are (the slice is never given to anyone else); an array still growing
  is copied out by `region_grow_array` (at most once, at most the slice). Only regions whose
  every user reaches its arrays through a pointer `region_grow_array` reassigns, and keeps no
  pointer across a growth (audited: `emit.c`, `lower.c`, the parser's `list_at` callers).
- **Not shared:** arena, tokens, nodes, symbols, types, scopes, names, symbol names. Code keeps
  pointers into them (`SYM(id)`, `NODE(u, id)`, arena data), so moving them could change
  output in a path no test runs. Sharing them would reach p.md's one page, but not under
  the ground rule.
- **Move test:** `-DREGION_SHARE_TEST=N` gives each shared region an N-byte slice on pages of
  its own, made `PROT_NONE` once copied out of, so a pointer kept across the move faults
  instead of reading stale bytes. Run with N = 1, 2, 4, 16 and 64 KB: the suite unchanged at
  every N, the examples identical, the `big` executable byte-identical at every N, 52 fuzz
  programs identical (N = 2 KB). This found a real bug in the first version: a region that
  left its slice was handed a slice again instead of its own reservation.
- **Name table:** starts at 128 slots (1 KB; the 23 keywords, then doubling), not 1,024
  (8 KB). Name ids follow insertion order, not the table, so output cannot change.
- **`--stats`** counts the shared page once and says which regions are still in it.
- **Measured** (`make bench`): taken at peak, secret.modal 256 → **144 KB**, hello 240 → 160,
  structs 272 → 176; in use 43.1 → **36.1 KB** (arena 20.5 → 13.5 KB, one page instead of two);
  compiler RSS 1.9 → 1.8 MB, page faults 319 → 312. `big` unchanged (41.2 MB RSS; +16 KB for
  the shared page, which its regions leave at once).
- **Verified:** 61 files byte-identical to the previous compiler's (every example, both CPUs,
  Linux, bare metal, objects); suite unchanged; ASan/UBSan clean.
- **What the remaining "in use" is:** tokens 5.4 KB and nodes 5.5 KB (the program's own),
  the arena's source text, side tables sized to the nodes, and per-symbol tables at the
  64-entry minimum of `region_grow_array`. p.md's 4.3 KB needs on-demand scanning (tokens
  alone are 3 KB); that changes no behaviour, but is a scanner/parser rework of its own, and
  the numbers do not ask for it.

## 16. Locals declared with their type

- **The module level's forms, in a block,** under the same rule (type first mutable, type last
  immutable): `name: T = value` (immutable; it needs its value), `T name = value`, and `T name`
  (mutable, starting at zero). `name := value` stays, mutable as before, its type taken from
  the value.
- **Parsing:** `local_decl_at` decides from the first tokens: `name:` / `name,` cannot start an
  expression, nor can a type followed by a name on the same line (`u8 x`, `pkg.T x`, `[]u8 s`,
  `*T p`); `*p = 3` stays an assignment. `parse_local` reuses the module level's parsers; the
  node is `AST_FIELD_DECL` (a = type, b = value).
- **Refused in a block, each with its fix:** several names (`a, b: T = v` would compute the
  shared value once per name), `name: T` without a value, a struct, a function (use a
  literal), a device (`@`).
- **Checking:** the value must fit the declared type, as at module level (no implicit
  widening: `u64 t = a` with `a: u8` is an error), and constants are range-checked.
  Assigning an immutable local says "local 'a' is immutable" (parameters say "parameter").
- **Backends:** the checker gives the node the declared type, and the lowering and the C
  backend convert the value to it, as they do for `:=`. With no value: zero (a struct, every
  slot).
- **Tested:** `examples/run/locals.modal` on C, arm64, x86-64 and Linux ELF (both CPUs);
  `examples/errors/locals.modal` and `locals_syntax.modal`; 82 fuzz programs whose functions
  return through typed locals of every form, identical across in-memory, exec and C;
  existing programs compile byte-identically (51 files, C and native).

## 17. Checker fixes, and run-time behaviour made the same everywhere

- **`:=` is immutable everywhere** (owner's decision): assigning one says "bound with
  `n := ...`; declare it `T n = ...`". `examples/run/structs.modal` was the one program that
  reassigned one (`rec u = r` now).
- **`:=` literals are range-checked** (`x := 3000000000` does not fit the `int` it becomes).
- **A body with a return type must reach a `return`** (with no control flow yet: a `return`
  among its statements, or in a block among them). A literal is named by its binding.
- **`< > <= >=` order numbers only;** bools, strings and structs are refused by the checker.
  Unary `-` on unsigned stays: it wraps, like `0 - x`.
- **The C backend's arithmetic is no longer undefined:** signed `+ - *` (and `u16 * u16`, which
  C promotes to `int`) were computed in C's own types. Now they are computed in `uint64_t` and
  converted back, which is the native code's wrap. UBSan reported 3 errors on the old output of
  a test program and none on the new one.
- **Division is defined on every backend** (docs/guards.md):
  - a divisor that is not a known non-zero constant is checked in the MIR (`JNZ` then
    `MIR_TRAP`);
  - the trap writes the output so far, then `file:line:col: division by zero`, and the
    status is 1;
  - `INT_MIN / -1` wraps (the `DIV` stencil macro, which costs nothing for unsigned);
  - the C backend's `m_div_<type>` does the same;
  - a literal zero divisor is a compile error.
- **Ending through `exit`:** `__builtin_setjmp` does not exist for arm64 in Apple's clang, so a
  trap calls `exit(1)`:
  - macOS: the system's `exit`, imported only by programs that can trap (the Mach-O writer
    now imports only the C functions a program calls);
  - Linux: an `exit_group` stub;
  - bare metal: the halt, with the status in x0 / edi;
  - `elfrun` now patches every system call and halt it finds, not only the first.
- **A trap does not count as output** for the output buffer, so a board program does not grow
  4 KB because it divides.
- **Verified:**
  - `examples/run/divide.modal` gives the same bytes and status on C, arm64, x86-64 and
    Linux ELF (both CPUs);
  - bare metal halts with 1;
  - `examples/errors/checks.modal` for the checker;
  - `make fuzz` (`src/v2/tools/fuzz.py`, now kept in the tree): 224 programs (8 trapping)
    identical across C, in-memory, exec and x86-64;
  - native executables of every example without a division are byte-identical to the last
    commit's (31 files);
  - ASan/UBSan clean.

## 18. The macOS arm64 executable has a floor of ~16.6 KB (p.md §4.6, B1), measured

Experiments with a modified writer (outside the tree) and a hand-built probe; every file
passed `codesign -v` unless noted.

| Layout | File | Result |
|---|---|---|
| today: `__TEXT` padded to 16 KB, `__LINKEDIT` at 16 KB | 16,704 B | runs |
| `__TEXT` file size = its real end (not a page), `__LINKEDIT` at 16 KB | 16,704 B | runs |
| only the first 4 KB hashed (codeLimit 4096; `codesign` calls it invalid) | 16,576 B | runs |
| `__LINKEDIT` at offset 0, overlapping `__TEXT` | 1,520 B | killed (SIGKILL) |
| `__LINKEDIT` right after the code (16-byte aligned) | 1,520 B | killed |
| `__LINKEDIT` at 4 KB | 4,320 B | killed |
| `__LINKEDIT` at 0, overlapping, file over 16 KB | 16,640 B | killed |
| static: `LC_UNIXTHREAD`, direct system calls, `__LINKEDIT` at 16 KB | 16,640 B | killed |
| static, no `__LINKEDIT`, header + code + signature | 800 B | killed ("main executable failed strict validation") |
| dyld + `LC_MAIN`, no imports, `__LINKEDIT` at 16 KB (probe) | 16,640 B | runs |

- **The kernel's rules, as measured:**
  - an arm64 executable must use dyld;
  - dyld needs a `__LINKEDIT` at a 16 KB-aligned file offset that does not overlap `__TEXT`;
  - so the smallest file that runs is 16,384 B plus `__LINKEDIT`.
- **Where hello stands:** 16,704 B, within 64 B of the probe. **2 KB is not reachable on
  macOS arm64 as an executable file**; that number is for Linux and bare metal (where hello
  is 1.3 KB today).
- **Other macOS targets:** x86-64 macOS uses 4 KB pages, and its hello is 4,320 B.
- **What still shrinks on macOS:** the code and RAM inside the page (B2, B3), and the start-up
  cost, not the file.

## 19. The MIR optimizer and constant programs (p.md B2, B3)

- **`mir/opt.c`:** one function at a time, after `finish_frame` and before `keep_if_small`, so
  an inlined body is already optimized and its constants then meet the caller's. Each pass is
  conservative:
  - **fold:** constants through copies, arithmetic (the lowering's own `compute`, exported as
    `mir_compute`, so the same wrap as the stencils) and memory (a store then a load of the
    same place and width); a branch on a known value; a divisor guard that cannot fire.
    Knowledge is per straight-line block.
  - **output:** a known number or bool written as text; runs of constant text, spaces and
    newlines merged into one string.
  - **stores:** a store overwritten before anything could read it; stores into a frame struct
    whose address is only ever stored through.
  - **dce:** a pure instruction whose result is not read before it is overwritten, or before
    the function ends (window slots are read only by a CALL).
  - Slots whose address is taken are never tracked.
- **Device registers:** a load or store of a field placed with `@` is marked
  `MIR_MEM_DEVICE` (and a struct copy of a register block). It is never forwarded, merged or
  removed, and it is a barrier. A device can arrive as an ordinary `self`, so without the mark
  the optimizer could not tell a register from memory.
- **Direct output:** a program whose only output is constant text, written at most 16 times
  with no recursion, writes each string straight to the system (`rt_write_direct`, the
  `entry_direct` stencils). There is no 4 KB buffer, and no `rt_put` / `rt_flush`.
- **Constant programs:** when `main`'s optimized MIR only writes constant text and returns a
  constant, and there are no module variables, the entry is that text and that status. It is
  hand-written per CPU (`const_entry`): `write(1, text, n)`, then the status. main, the runtime
  and the value stack are not in the program.
- **Measured** (`make bench`, Linux sizes from `-o`):

| | before | after |
|---|---|---|
| secret: code / RAM | 1,116 B / 4,576 B | **40 B / 128 B** |
| hello: code / RAM | 492 B / 4,320 B | **56 B / 128 B** |
| counter, literals, locals: code | 2,824 / 1,120 / 2,616 B | 1,852 / 56 / 1,956 B |
| hello / secret, arm64 Linux ELF | 692 / 1,316 B | **256 / 240 B** |
| structs, patterns: code | 5,816 / 5,820 B | 5,816 / 5,488 B (struct copies and recursion: next) |
| big (3 MB): emit | 23.6 ms | 29.5 ms (the optimizer on 54,000 functions: to win back) |

- **Verified:**
  - suite (179);
  - `make fuzz` with struct programs (`FUZZ_KIND=struct`: fields stored several times, read
    back, copied and passed): 400+ programs identical across C, in-memory, exec and x86-64;
  - ASan/UBSan on the examples and on 33 fuzz programs;
  - every constant program on arm64, x86-64, both Linux ELFs and in memory, exit status
    included.
- **What is left for code size:**
  - struct copies (`structs.modal`);
  - frame size not recomputed after dead slots;
  - the 108 B `zero` stencil;
  - register-indexed slot loads (`movz` + `ldr [x0, x8, lsl 3]` instead of `ldr [x0, #off]`);
  - the generic entry when the program is not constant.

## 20. Register allocation, exact output buffer, scalar fields (p.md §4.4, §4.5, §4.7)

- **Register allocation (`backend/native/regalloc.c`), linear scan** (Poletto & Sarkar) over
  the MIR's frame slots:
  - **Which slots:** a slot whose address is never taken (`ADDR`), that no `ZERO` block covers,
    that is not part of a call window, and is below 4096. Everything else stays in the frame.
  - **Liveness** by basic blocks, iterated to a fixed point (loops from output routines settle
    in two passes). A slot's range is the hull of every point where it is live or named.
  - **Two classes:**
    - *preserved* registers (arm64 x19–x28, x86-64 rbx rbp r12–r15) for ranges a call out
      crosses. The C calling convention keeps them, so every stencil, runtime function and C
      function leaves them intact; a Modal function saves the ones it uses, in pairs to keep
      the stack aligned.
    - *scratch* registers (arm64 x3–x8 and x12–x15, x86-64 r8–r11) for ranges no call out
      crosses. Nothing is saved, so most leaf functions have no prologue.
  - **Fused compare and branch:** a comparison read only by the branch after it gets no slot;
    the pair becomes `cmp` + `b.cond` (`cbz`/`cbnz` against 0) or `cmp` + `jcc`.
  - Parameters live at entry are loaded first; a slot no instruction names (a parameter only
    passed back) gets no register.
- **Generation (`emit.c`, "registers"):** a function with registers is encoded instruction by
  instruction, in 1–4 machine instructions each.
  - **Frame access:** slots in the frame are read at short offsets (`ldr x9, [x0, #s*8]`,
    `mov rax, [rdi + s*8]`). That is also the register-indexed load p.md listed as left.
  - **Stencils still used:** output, traps, and division on x86-64 (`idiv` needs rax and rdx,
    and faults on INT64_MIN / -1).
  - **Direct calls:** move `f` up by the frame, `bl`/`call`, then move it back. That is 4–5
    instructions against the call stencil's 18, and the callee returns with `f` and `c`
    unchanged because every stencil passes them on.
  - **The value rule:** values stay canonical (zero- or sign-extended), so arithmetic is done
    in 64 bits and the result is extended from its width again. That is the same wrap as the
    stencils and the C backend.
  - **Always on:** every function gets registers. Generating each small function both ways
    showed the stencil form shorter only for a lone `ret`, and that now gets no register
    either. `MODAL_REGS=0` turns registers off, to compare.
- **Frames without register-only slots:** a slot that lives only in a register loses its place
  in the frame, and the slots above it (the call window included) move down.
  - **What keeps its place:** parameters (the caller writes them), the result (the caller reads
    it), slots in no register, and on x86-64 the operands of a division done by its stencil
    from the frame.
  - **Where that is known:** `MirFunc.param_slots`, which the lowering now records.
  - **Effect:** value stacks are 55–80% smaller (counter 152 → 56 B, device 184 → 80 B,
    divide 72 → 16 B). Together with the exact output buffer, RAM is below the no-register
    figure, despite the saved registers.
- **The machine stack is counted exactly:** saved registers and the call frame of each
  function on the deepest chain (`StackNeed.machine`), not a maximum times the depth.
- **§4.4, exact output buffer:**
  - Each function's most output is computed: a string's length from the `str` before it in
    its block, a number 20 characters, a bool 5. It is unknown for a string from elsewhere or
    a function that loops.
  - The call graph sums it, counting each call site, and unknown under recursion.
  - When the total fits `output_buffer`, the buffer is exactly that. The program still
    flushes once at the end, so its output and order are unchanged.
- **§4.4, runtime frame:** the deepest chain of runtime functions from the stencil that calls
  them, down to the system call. Before, it was the sum of every runtime function, without the
  calling stencil's frame.
- **§4.5, scalar fields (`mir/opt.c` fields):** a load or store through the address of this
  function's own struct, at a whole slot, becomes the slot itself. A narrower field at the start
  of a slot becomes `norm` from its width, since both targets are little-endian. When the address
  has no other use left, dce removes it and the slots are plain values again.
- **§4.5, runtime for size:** the runtime functions are compiled `minsize` (clang's `-Oz`, for
  them only).
- **Compile time:** `plan_binop` called `strlen` on its table for every operator, about 10 ms
  on big. Without it, emit with registers takes 26.5 ms, against 29.5 ms before registers.
- **Measured:** arm64 code and RAM from `--stats`; RAM includes the exact output buffer in both
  columns. The x86-64 code is in the next table.

| program | no registers | registers |
|---|---|---|
| structs | 5,716 B | **3,576 B** |
| patterns | 5,464 B | **3,832 B** |
| device | 2,052 B / RAM 512 B | **952 B** / RAM **432 B** |
| context | 1,852 B / RAM 576 B | **1,196 B** / RAM **528 B** |
| counter | 1,828 B / RAM 672 B (4,096 B buffer before) | **1,324 B** / RAM **608 B** |
| locals | 1,924 B | **1,492 B** |
| blink (arm64-none) | 2,112 B / RAM 1,296 B | **896 B** / RAM 1,328 B |
| big (3 MB): emit / RAM | 29.5 ms / 7,376 B (before this step) | **27.1 ms / 6,496 B** |

| x86-64 code | no registers | registers |
|---|---|---|
| structs | 5,152 B | **3,380 B** |
| patterns | 4,532 B | **3,084 B** |
| device | 1,984 B | **984 B** |

  - Linux arm64 ELF: structs 5,916 → 3,776 B, device 2,252 → 1,152 B.
  - Constant programs are unchanged: secret 40 B of code, now 64 B of RAM (the runtime
    estimate is gone).
- **p.md §4.7, the other two items:**
  - *Superinstructions:* replaced by the direct encoding (a fused compare and branch is one).
  - *Compact holes everywhere:* the direct encoding uses exact immediates.

  Stencils remain for what calls out, where a hole's size does not matter.
- **Verified:**
  - suite: 179 passing, also with registers forced on every function before that became the
    rule;
  - `make fuzz` with a new generator, `FUZZ_KIND=pressure`: more values live than registers,
    across calls. Over 2,000 programs across mix, struct, scalar and pressure, with registers
    on, forced and off, all identical across C, in memory, exec and x86-64;
  - ASan/UBSan: every example on four targets, with registers forced.

## 21. Values split from slots, direct runtime calls, identical-code folding (p.md B5)

- **Webs (regalloc.c, step 2).** The lowering reuses a temporary slot for unrelated values:
  every string address of a `print` goes through one slot, for example. One range for all of
  them crossed every write, so the slot needed a preserved register.
  - Each value that dies inside its block now gets a name of its own (`RA_VIRTUAL | k`). A value
    still live when its block ends keeps the slot's name, since the next blocks read it.
  - Liveness runs again over the names. A split value that gets no register uses its slot's
    place in the frame, which no other value of that slot needs at the same time.
  - The result's slots are never split, because the return reads them by slot. A function in
    which no slot is written twice is not split at all (no second pass).
- **Direct runtime calls.** In a function that calls no other Modal function (a leaf), `f`
  and `c` live in x19/x20 (rbx/r14).
  - Each output or trap call is `mov x1, value; mov x0, x20; bl rt_*`, then `f` and `c` are
    moved back: 20 B, against 56–76 B for the write stencil. On x86-64 it's 17 B against 41–47 B.
  - Leaf functions only: there, the two registers cost what the write stencil's own frame did,
    at the same moment. In a function that calls others they would stay saved under every
    nested call, which is RAM along the deepest chain.
- **arm64 link register:** a function that calls out saves x30 once, in its prologue, paired
  with a saved register when the count is odd, instead of around each call.
- **Identical-code folding** (gold/lld `--icf`, in the shake): two kept functions with the same
  bytes, and the same calls, runtime calls and C calls at the same places, are one. The later
  one is dropped and its callers go to the first. `--stats` prints how many were folded.
  `examples/run/same.modal` has two (the suite checks it), and the fuzzer adds renamed copies.
- **Compile time:** emit on big is 29.5 ms, the same as before register allocation. The
  per-function scans are merged into one, and `main` is found once instead of per function.
- **Measured** (arm64 `--stats`; "before" is §20):

| program | code §20 → now | RAM §20 → now |
|---|---|---|
| counter | 1,324 → **1,052 B** | 608 → **576 B** |
| locals | 1,492 → **1,096 B** | 592 → **544 B** |
| divide | 1,428 → **1,100 B** | 512 → 512 B |
| globals | 1,056 → **892 B** | 448 → 464 B |
| patterns | 3,832 → **3,632 B** | — (recursive) |
| structs | 3,576 → **3,072 B** | — (recursive) |
| blink (arm64-none) | 896 → **832 B** | 1,328 → 1,360 B |
| x86-64: counter / structs | 1,116 / 3,380 → **964 / 3,172 B** | |

  - `examples/budget/mod.modal`'s `max_code` went from 1500 to 1200: deep.modal fits 1,500 B
    now (1,452 B), and the example is there to be over it.
  - deep.modal's RAM is 624 → 672 B. Its function `a` keeps four locals live across a print,
    and they live in saved registers now; its value stack is 72 → 40 B.
- **Verified:**
  - suite: 185 passing, 6 of them new (`same` on every backend, and folding);
  - fuzz: over 1,300 programs across mix (with renamed copies), struct, scalar and pressure,
    with registers on and off; 0 differ;
  - ASan/UBSan: every example on four targets. It found pointer arithmetic on a `NULL` call
    list in the folding code, now fixed.
- **What is left of B5:** the optimized-MIR cache and specialization of constant-argument
  calls, for which comptime is the natural base. Also machine outlining, string pooling, and
  tail and partial inlining (none of the examples has their patterns).

## 22. Function kinds in the contract (comptime step 0)

- **What the checker notes** (`sema/check.c`, "function kinds"), per body:
  - its effects, each with the byte offset of its first occurrence. That is an offset, not a
    token, because a deferred body's tokens are gone once it is checked. The effects are:
    output (`print`, `write`, `std.sys.put`, an `out`), a device register (a field placed with
    `@`, or a device), and a mutable module variable (read or written);
  - the functions it calls, plus the next pattern clause, since a clause that does not match
    goes on to it.
- **The closure** (`sema_fn_kind`):
  - callee lists are built once by counting sort;
  - a memoized walk gives every function's kind exactly where there is no cycle;
  - a fixed point passed back along the edges settles cycles. Mutual recursion through
    clauses was the case that needed it;
  - the reason is found breadth first: the nearest function that has the effect itself, and
    the callee of this function that leads there.
- **`--sig`:**
  - each contract line gets a column: `both`, `run time  (prints, through 'log',
    kinds.modal:10)`, or `compile time  (declared in mod.modal)`;
  - a file no contract covers, such as the entry file, gets a `== functions: path` listing;
  - nothing generated depends on it yet.
- **Cost:** checking is unchanged on big (18.4 ms in total), and `--sig` on big takes 0.1 s.
- **Verified:**
  - `examples/kinds.modal` covers both, prints, through a callee, a mutable variable, and a
    cycle through pattern clauses. With `device.modal` for registers, that's 6 suite checks
    (191 passing);
  - ASan/UBSan on `--sig` of every example, with `--repeat`.

## 23. The compiler runs code: module-level initializers (comptime step 1)

- **What works:**
  - `limit: u64 = fact(10)`, `a := area(7)` and `u64 total = limit / 1000` are computed while
    compiling;
  - the C, arm64, x86-64 and Linux backends then use the number, as if it were written there:
    a constant is folded, and a variable's first value is stored by main's prologue;
  - a module-level `x := expr` now has its value's type (it was "anything" before, the gap in
    the type audit).
- **When:** after a file's declarations are checked and before its bodies (`comptime_unit`
  in `sema_check`). Every signature is known by then, and so is every earlier constant.
- **The engine (`sema/comptime.c`):** an interpreter over the MIR, the instructions the
  machine backends generate from.
  - Arithmetic is `mir_compute`, the wrap every backend has; INT_MIN / -1 wraps as it does in
    the program.
  - Memory is its own value stack, so struct values and methods on them work. Any other
    pointer is refused.
  - Strings it makes are its own blocks.
- **Callees, on demand:** a function the evaluation calls is checked again, quietly. That's a
  new `cx.quiet`: its errors are reported in its own turn, not twice. It is then lowered (the
  capture replaces `codegen_function` for that check), and its MIR is kept in the arena for
  the rest of the compilation.
- **No capabilities** (docs/comptime.md §2). Output, a device register, a mutable module
  variable and a division by zero each stop the evaluation, with an error at the declaration
  that names the function: `'a' cannot be computed by the compiler: 'log' prints (it runs
  only in the program)`.
- **Budgets:** `comptime_steps` (1,000,000) and `comptime_depth` (1,000) in mod.modal.
- **The differential test** (`make fuzz`): mixed programs bind random calls at module level
  and print `ct <module value == the same call at run time>`.
  - `ct false` is a difference, beyond the four backends agreeing.
  - In 200 generated programs, 105 ran such calls: 200 comparisons, all equal. The 21 the
    compiler refused divide by zero.
- **Found on the way:** the lowering kept bodies for inlining, globals and output routines
  from the last compile in the same process (`--repeat`). The compiler lowers while checking
  now, so `mir_reset` runs with each compile; ASan found it.
- **Memory:**
  - kind facts are recorded only for `--sig`;
  - the shake releases its tables (identical-code folding needed 3.7 MB on big);
  - big uses 41.1 MB in use, against 42.5 MB before.
- **Verified:**
  - suite: 200 passing. `comptime.modal` runs on every backend and on Linux ELF, and 4 checks
    cover the refusals;
  - fuzz: 0 differ across mix, struct, scalar and pressure, with registers on and off;
  - ASan/UBSan on the comptime examples with `--run`, native, x86-64 Linux, `--sig` and
    `--repeat`.
- **Not yet:** all four done in §24 (string and struct values, `mod.modal` functions, a callee in a
  file not checked yet, `assert` / `sizeof`).

---

## 24. Strings and structs from the compiler, mod.modal, assert and sizeof, folded calls

Comptime steps 1 and 2 (docs/comptime.md), and what p.md B5 left: calls with constant arguments.

- **Strings and structs leave the compiler** (`sema/comptime.c`):
  - `p := pt.make(-7)`, `title := name(1)`, `[]u8 status = name(0)`. A module-level `:=` now has
    the type of a string or struct value too (it was "external");
  - the result's slots are kept, in the target's C layout. Each string field is found from the
    type (`portable`), and its bytes are copied out of the interpreter's block;
  - the lowering writes a struct as constants and strings into its slots, at each use. A string
    variable's first value is stored by main's prologue;
  - the C backend writes a designated initializer (`{.x = ..., .name = {(const uint8_t *)"...", 5}}`),
    every byte in octal. A struct constant is not `const` in C, since methods take its address
    as `self`;
  - refused, with the reason: a reference (it would point into the compiler), a block of
    device registers, a mutable module variable holding a struct (not supported yet), and a
    string on a target whose pointers are not 8 bytes.
- **mod.modal functions run in the compiler:**
  - every file of the domain sees them by name, after its own declarations
    (`module_domain_function`);
  - a call from a body is an error: "'square' is declared in mod.modal, so it runs only in the
    compiler. To use its value here, bind it at module level";
  - from another domain they are not part of the face (like C's `static`);
  - they are never handed to a backend.
  - `examples/run/compiled/` runs them on C, arm64 and x86-64.
- **A callee in a file that is not checked yet:** its declarations are checked first, quietly,
  then its body (`sema_lower_for_comptime`). This is what makes mod.modal work, since it is
  checked last.
- **Unsized constants:**
  - `n := 5` and `k := n * 3 + 1` at module level are unsized. Their value is the checker's
    exact one (`sema_unsized_value`);
  - the range check sees through the name: `f(n)` with `n := 300` is "300 does not fit u8";
  - a constant outside -2^63 .. 2^64 - 1 is an error at its declaration;
  - the C backend writes the number where the name is used, so C types it as it types the
    literal. The native backends give it the literal's variant (`c_variant`).
  - A local `x := n` is an `int`, as `x := 5` is.
- **assert and sizeof** are std functions (`import("std").debug.assert`,
  `import("std").mem.sizeof`); the two keywords are gone from the scanner.
  - The parser: a module-level `name(...)` or `a.b(...)` that ends its line is a statement. A
    declaration always has a body, so the two never meet; a body that opens on the next line is
    still a declaration.
  - At module level (`statement` in comptime.c), the compiler runs it:
    - `assertion failed: sizeof(header) == 12 (8 == 12)`: both sides of a comparison, signed
      when the operands are;
    - a call made for the asserts inside it fails with where: `'checks' fails at
      asserts.modal:10:5: assertion failed: neg(3) > 0`;
    - output there is refused, as for initializers.
  - In a body, it is a run-time guard. The MIR checks the condition and calls `trap` with
    `file:line:col: assertion failed: <condition>`, so the output so far comes first, then
    status 1. The C backend calls `m_trap` with the same text.
  - `sizeof(T or value)` is an unsized constant of the target's layout. The checker,
    `const_value`, and the C backend fold it.
- **Calls with constant arguments are computed (B5):**
  - in the optimizer's `fold`, a `MIR_CALL` whose argument slots are all known runs in the same
    interpreter, and the call becomes `const` (a void one goes away). On anything the
    interpreter refuses (output, a trap, a mutable variable, a budget), the call stays: it is an
    optimization, never an error;
  - a fold has a budget of 20,000 steps and a compilation 2,000,000;
  - results are kept per (function, arguments), in 64 entries;
  - only int, bool and void results are folded (a string would point into the compiler);
  - the callee's argument slots come from its signature (`mir_param_slots`), so it is lowered
    for the compiler only when every argument is known. The first version lowered every
    callee early: emit on big went 35 → 71 ms;
  - lowering inside a lowering: `mir_nest_begin` / `mir_nest_end` save the lowering state.
    The nested functions are lowered after the caller's MIR in the region. When the region
    moves to a reservation of its own meanwhile, the nested start follows it; without this,
    patterns.modal ran out of memory;
  - one level only, and none inside a quiet (compile-time) lowering;
  - `--stats` says how many calls were computed.
- **Memory:**
  - the interpreter's value stack starts at 8 KB and is given 4 times more when an
    evaluation needs it, which then runs again; it cannot grow in place, since it holds
    addresses into itself. A fold has a fixed 8 KB;
  - lowered functions are found through a small map, not a 64 B table per symbol;
  - in use: comptime.modal 72 KB (a first version took 715 KB, with fixed 512 KB and 64 KB stacks
    and a 90 KB memo); big 41.25 MB (41.14 MB without folds; 46.9 MB with the per-symbol table).
- **Measured** (arm64 `--stats`, `--time` best of 3):

| program | before | now |
|---|---|---|
| big: code / RAM | 16,680 / 6,336 B | **52 / 64 B** (the three chains computed) |
| big: emit / total | 35 / 56 ms | **31.5 / 52 ms** |
| comptime.modal: calls computed | — | 2 |
| patterns.modal: calls computed | — | 12 |

- **Examples changed so that they still cover what they test:**
  - `same.modal`, `patterns.modal` and `board/blink.modal` pass one argument through a
    mutable module variable. Their calls would otherwise be computed: no folding left to
    see, no recursion left in the image;
  - blink's RAM is 1,360 → 1,376 B, the variable's slot.
- **Verified:**
  - suite: 217 passing. The 8 failures are as before this work: `main.modal` uses `nil` as
    a type, `add.modal` is v1 syntax, and the owner's edit of `output.modal` prints 43, not
    42;
  - new: guards.modal (every backend and both ELFs), compiled/ (3 backends), and 11 error
    checks;
  - fuzz: 0 differ in 991 programs (mix, struct, scalar, pressure, registers on and off). In
    a sample of 40, 49 calls were computed;
  - ASan/UBSan: the suite (217), the fuzzer, and big with `--repeat`.
- **Not yet:**
  - partial specialization: a copy of a function for some constant arguments. It costs code
    size, so it needs a policy;
  - a cross-run cache of optimized MIR (p.md §5.4, incremental builds);
  - target and config values at compile time, arrays;
  - `assert` to a `fault` context (guards.md step 3).

---

## 25. The development guide, and two tools

- **`docs/development.md`:** how to work on the compiler. It covers:
  - the priority order the owner set on 2026-11-12: **speed first, binary size second**;
  - the language rules a change keeps, including the new **core/std rule**: the compiler holds
    only primitives Modal cannot express, std is Modal source, and neither the bootstrap nor std
    depends on libc;
  - the map of `src/v2`, build and test, what makes a change done;
  - how each phase is documented, and the pitfalls met so far.
- **`make backends F=file.modal`** (`tools/backends.sh`): one program on C, arm64, x86-64 and
  both Linux ELFs, with each status and output hash. It replaces the ad-hoc script of §24.
- **`make asan`:** builds `./modal-asan`. `BIN=./modal-asan make test` runs the suite under the
  sanitizers.
- **`guide.md`:** module-level strings and structs, mod.modal functions, unsized constants and
  folded calls are no longer listed as gaps. `assert` is no longer reserved.
- **Decided, for the phases after this one** (the owner's answers): std moves into Modal files
  on core primitives (§26, `docs/std.md`); then the stack overflow guard and target/config
  values at compile time. Partial specialization is deferred, and will be opt-in when it comes.

---

## 26. The core and std: primitives, std as Modal files, C without stdio

docs/std.md steps 1–3. The owner chose "core primitives + std in Modal" (§25): the compiler
implements only what Modal cannot express. std is Modal source, never needed to build the
compiler, and using no libc.

- **Primitives** (predeclared names, not keywords; a program's own declaration wins). The checker
  marks a call to one in the call node's `aux` (`PRIM_*`, `sema_primitive`):
  - `sizeof(T)`: no import any more (it was `import("std").mem.sizeof` in §24);
  - `u8(x)`, `i32(x)`, ...: a conversion, wrapping as a C cast does. It is `lower_as` in the MIR
    (a `norm`), folded when constant, and a C cast in the C backend;
  - `trap(a, b, ...)`: its strings to the error output after the program's output, then status 1.
    - Literal parts are joined into one message. Any other part (a parameter) is a
      `MIR_TRAP` with sub `MIR_TRAP_PART`, which ends no block (`MIR_ENDS`).
    - Native: `rt_trap_part` and the stencil `write_trap_part`. `rt_trap` now calls
      `rt_trap_part`, because clang's machine outliner otherwise made a shared function
      stencilgen cannot patch.
    - C: `m_trap_part`.
    - The interpreter gathers the parts: a trap at compile time is a compile error with that
      text;
  - `site: site`: a context parameter of type `[]u8`. When the caller has no `site` of its own,
    `fill_contexts` inserts an `AST_SITE` node, whose text `sema_site_text` computes from the
    call's tokens: `file:line:col: <the call as written>`.
- **std is files** (`std/mod.modal`, `std/debug.modal`):
  - found through `$MODAL_STD` or `std/` next to the executable (`find_std`, kept across
    `context_reset`);
  - a root domain wherever it lives. Its face is what its mod.modal binds:
    `assert := import("std").assert`;
  - `std.io` and `std.sys` stay the compiler's namespaces (`compiler_namespace`), so a program
    that only prints reads no std file. hello.modal: 1 file, as before;
  - without a std folder, `import("std")` is external, as it was before std had files.
- **assert is Modal:** `bool assert(bool true, site: site): true`, then
  `bool assert(bool ok, site: site) { trap(site, " is false\n") ... }`.
  - The compiler's assert is gone (`STD_ASSERT`, `check_assert`, its lowering and C emission).
  - A module-level call statement is generic (`statement` in comptime.c). When the trap names
    the statement's own place, its text is the error, and a comparison as the first argument
    gets both sides: `assert(sizeof(header) == 12) is false (8 == 12)`.
  - Messages changed: `assertion failed: n < 4` is now `assert(n < 4) is false`.
- **A binding that names a function is that function:** `module_binding_function` follows
  re-exports, so `assert := import("std").assert` is a direct call.
- **The C backend emits the functions of every file,** each in its own file's context and named
  `m_u<file>_...`.
  - A type or a module-level value in an imported file is reported as not supported yet. That
    is the remaining multi-file work; it was "importing another Modal file" before.
  - Its programs no longer use stdio. A 4 KB buffer is written with `write`, numbers are
    formatted by the prelude, and traps use `write` and `exit`. The helpers are `mrt_*`, a
    prefix no Modal name can become: a program's own `out` became `m_out` and clashed with the
    first version.
- **Found on the way:**
  - `comptime_depth` and `comptime_steps` (§23) were never in the list of known mod.modal keys,
    so setting one was an error. They are now known, and checked (`budget_ct/`).
  - The interpreter's C frame held message buffers at every Modal call, and ASan's larger frames
    overflowed the C stack at depth 1,000. The messages moved to `stopf`.
  - A very high `comptime_depth` could overflow the compiler's own stack. First a guard turned
    that into an error; §27 removes the cause, since the interpreter no longer recurses in C.
  - A pattern mismatch said "parameter 'true' is bool". It now says "argument 1 of 'assert' is
    int, but it takes bool".
- **Measured:**
  - assert as a call: 10,000,000 asserts in a recursive walk take 116 ms, against 118 ms with no
    assert at all (best of 10). Within noise, so the speed gate passes;
  - guards.modal: code 1,060 → 1,164 B and RAM 496 → 624 B (the assert calls). Accepted under
    speed first, size second;
  - traps: divide.modal 1,100 → 1,120 B (`rt_trap_part` is always beside `rt_trap`);
  - compiling a program that imports std: 2 more files, 0.018 ms checking for guards.modal,
    61.3 KB of compiler memory.
- **Verified:**
  - suite: 223 passing, the 8 known failures only. New: primitives.modal on every backend and
    both ELFs, the comptime budget check, and the assert checks updated to the new messages;
  - fuzz: 0 differ in 269 programs (mix, struct, scalar, pressure, and registers off). The C
    backend, the reference, now writes through `write`;
  - ASan/UBSan: the suite (all but the 8 known), `--repeat 3` and `--sig` on the std examples,
    and the fuzzer.
- **Not yet:** formatting in Modal (std.md step 4, behind the speed gate), and types and values in
  imported files on the C backend.

---

## 27. Target and config values at compile time; the interpreter's own call stack

comptime.md step 2's remainder, and guards.md step 2 analysed.

- **`target := import("target")`:** a module the compiler makes from `MachineTarget`
  (`module_target_unit`). It is text scanned and parsed like a file, at `/(compiler)/target.modal`,
  marked `provided`.
  - Members: `ptr_size`, `word_size` (unsized), `arch` (`"arm64"`, `"x86_64"`), `os` (`"macos"`,
    `"linux"`, `"none"`), `has_output` and `big_endian`.
  - Visible from every domain. Its values are compile time only, so the C backend never emits
    them.
  - `unit_load` was split: `unit_from_source` builds a unit from text, from disk or not.
- **Configuration by name:** a domain's mod.modal values (`max_ram := 4096`) are readable by name
  in its files, after their own declarations, as mod.modal functions are
  (`module_domain_function`).
- **Compile time only:** reading either one from a function body is an error that says how to use
  it (`compile_time_value` in check.c):
  `'max_ram' is configuration of the domain, a compile-time value: a function body cannot read it.
  Give its value a name at module level (n := ... max_ram ...) and use that name here`.
- **A bool constant of another file** (`has_output := true`) now folds (`const_value`). It was
  "a module-level constant that is not a literal".
- **The interpreter keeps its own call stack** (`struct CtFrame` in comptime.c). A Modal call
  pushes a frame record instead of recursing in C. `comptime_depth` is bounded only by itself
  and the 512 KB value stack, no longer by the compiler's own stack, which under ASan stopped
  evaluations at 523 calls. The guard of §26 is gone with its cause.
- **The stack overflow guard (guards.md step 2) is analysed, not built:**
  - the native backend learns that a program recurses only after its functions are generated;
  - a guard that costs nothing elsewhere needs patching afterwards;
  - and the same depth on every backend needs MIR frame sizes in the C backend.
  guards.md records both.
- **Measured:** big: emit 29–30 ms, 41.26 MB, the same as §24.
- **Verified:**
  - suite: 230 passing, the 8 known failures only. New: `run/target/` on C, arm64 and x86-64;
    `target.arch` on arm64 and x86-64; two compile-time value errors;
  - fuzz: 0 differ (mix, 181 programs since §26);
  - ASan/UBSan: the full suite with the 8 known failures only (the depth check now passes
    under ASan). The target and primitives examples with `--repeat`, `--sig` and two targets.
- **Not yet:** the stack overflow guard (above), arrays `[N]T`, formatting in Modal (std.md step 4).

## 28. Tail calls, clause loops, the inline byte sink, constant division (std.md step 4)

std.md step 4's optimizer prerequisites. Measuring a formatter written in Modal found a crash
first: a loop of more than ~16,000 turns (`u64 loop(u64 n, ...): loop(n - 1, ...)`) ended with
SIGSEGV on native, and the C backend overflowed too (`--run` builds with no `-O`; even clang
`-O2` did not make it a jump). Modal's loops are recursion, so a tail call is semantics, not an
optimization. Since this section every backend runs them in constant stack.

- **MIR_CALL_TAIL** (a `sub` of MIR_CALL; `b`: the callee's parameter slots). `mark_tail_calls`
  in lower.c turns `call g; copy s0.. = window; ret` into one, after the function is optimized
  and kept for inlining (a spliced body must return to its caller).
  - Not in a function with MIR_ADDR (a struct it built, a local passed by reference): the
    callee's frame takes this one's place.
  - Native, registers: the arguments move from the window to slots 0.. in the callee's own
    numbering, the saved registers come back (`regs_restore`, split from the epilogue), then
    `b callee` / `jmp callee`.
  - Native, stencils: `copy` stencils, then the new `tail` stencil (`musttail CALLEE(f, c, 0)`).
  - The interpreter moves the arguments and changes function in the same frame (no CtFrame), so
    `comptime_depth` does not count tail calls: `total := sum(100000, 0)` at module level was "calls
    nest deeper than comptime_depth = 1000" and is now 5000050000.
  - C: `M_TAIL` is clang's and GCC's `musttail` (`return` elsewhere), on `return f(...)`, a
    one-line body, a void function's last call, and the clause fall-through. Only with the same
    prototype and numbers and bools as parameters and result: musttail's rule, and no argument
    can point into the frame that the callee takes over.
- **Pattern clauses joined** (`mir_clause_hold` / `mir_clause_merge`, registry.c): every clause
  is its own function, and a loop went through two of them per turn (the first clause's tests,
  the tail call to the next, its tail call back to the first).
  - A clause that ends by going on to the next is held (copied to the arena) until the next is
    lowered. The two then become one function: the first up to its miss label, then the next,
    with its labels, strings and window renumbered (`reframe_inst`). A chain joins in turn.
  - In the joined function the recursion is a tail call to itself. `self_loops` makes it
    `copy params = window; jump top`, with **MIR_JUMP** (new, made only after the optimizer) and
    a label before the first instruction. The register allocator sees an ordinary loop.
  - The next clause is still generated on its own; nothing calls it, so the shake drops it.
- **The inline byte sink:** the `write_byte`, `write_space` and `write_nl` stencils store into the
  buffer and call `rt_flush` only when it is full (`put_byte`), instead of `rt_put` for one byte.
  Direct mode is unchanged: it is chosen from the runtime calls, and `rt_flush` is not
  `rt_write_str`.
- **Unsigned division by a constant** (native): `umulh` and a shift on arm64, `mul` (rdx) and
  `shr` on x86-64, the add form when the multiplier needs 65 bits, a shift for a power of two
  (`div_magic`, Granlund–Montgomery, validated on 20 million divisor and value pairs). Not for
  signed division or a divisor above 2^63, which still divide. On x86-64 it no longer goes
  through the division stencil, so its operands no longer need frame homes.
- **Measured:**
  - `sum` over 100,000,000 turns: 290 ms (two calls per turn) → 23.5 ms on native registers;
    stencil mode 87.7 ms.
  - The formatter experiment (std.md §6): a million numbers printed, the compiler's 16.1 →
    10.7 ms; Modal digits by recursion 44.3 → 28.1 ms; Modal by tail calls 23.0 ms. 2.1×: the
    gate is not met, and the rest is the algorithm without arrays.
  - `make bench` (Apple M4, best of 10): exec 0.87–1.45 ms for every program, at the process
    start (0.73 ms); code secret 40 B, hello 56, output 1,336, structs 3,016 (3,072 in §21),
    patterns 1,252; big: check 18.7 ms, emit 32.1 ms, 41.26 MB as in §27. No regression.
- **Verified:**
  - suite: 240 passing, the 8 known failures only. New: `run/loops.modal` (ten million turns:
    a clause loop, two functions calling each other, a loop that prints) and
    `run/quotients.modal` (18 constant divisors, from 1 to above 2^63, on values from module
    variables so nothing is folded), each on C, arm64, x86-64 and both Linux ELFs;
  - the fuzzer now generates accumulator loops (a tail call, up to 3,000 turns, also at module
    level for the compiler). 0 differ in 12 rounds: mix, struct, scalar, pressure, MODAL_REGS=0
    and 2, before and after the sink and division (about 1,050 programs);
  - ASan/UBSan: the suite with no sanitizer report; the 8 known failures, and `loops.modal` on the
    four native lines because the example was edited after its `.expected` (a `print("sum")` in
    `count`, at 00:34) — not by this work, left as it is. Every backend agrees on the edited
    program (`make backends`).
- **Not yet:** formatting in Modal (std.md §6: arrays first); signed division by a constant;
  the stack overflow guard for the calls that stay calls (guards.md step 2); `print` as std
  (designed, std.md §7, for the owner).

---

## Appendix A. The owner's original specification inputs

These are the inputs the design started from, as the owner wrote them. Where the
implementation departs from them, section 2 and section 3 say how.

### A.1 Struct

`````
default_struct: struct {
    -- this are a convention because are massively adopted by language, in our language this is a immutable member referencing to a type
    example_1_of_member: u8,
    -- this are a convention because are massively adopted by language, in our language this is a mutable member referencing to a type
    u8 example_2_of_member,

-- disconsider this few codes below, they are reference of other approachs

-- this is the approach that V language uses, but are poorly well to developer experience and we need to find a walkround to that private, pub and cannot be the the "export"
-- the same thing to methods encapsulation, the mutable issue i fixed with two example above (need to review)
private:
    private_member_1: int,
    ...other private member
pub:
    public_member_1: int,
    ...other public member
mut:
    mutable_member_1: int,
    ...other mutable member
}
`````

### A.2 Struct with methods ("nested class")

`````
john_nested_class: struct {
    []u8 my_name,
    n1, n2: u8,
    -- first example "nil" doesnt appear as returnal here because optional to put compiler automatically inserts it
    new(self: john_nested_class): self {
        return &self
    }

    -- same thing of struct that we mentioned previously of type comes first on a member turns member mutable and type coming last turns it immutable
    -- you could noticed some differences in syntax here we dont have the ":=", right?
    -- when we make function_name := (..params) return_type {} its a variable that receives a method as value typically its the same thing
    -- when we do a function_name(..params): return_type {} its a actual method being declare.
    add_normal(self: john_nested_class, n3: u8): u8 {
        return self.n1 + self.n2 + n3
    }

    -- here its a alternative syntax to one-line function declare
    u8 add_oneline(self: john_nested_class, n3: u8): self.n1 + self.n2 + n3;
    -- this syntax make the function be mutable (support function overloading)
    u8 add_oneline(self: john_nested_class, n3, n4: u8): n1 + n2 + n3 + n4

}

-- also you can only refer directly the data structure that owns the current method with parameter: "self"
multiply(self: john_nested_class): u8 {
    return self.n1 * self.n2
}

-- all this above is the domain of "john_nested_class"
`````

### A.3 Encapsulation idea

`````
this are previously coverede in <struct> but we need heavility suggestion to that can be suggested file/path domain via (i.e example/john.modal) "john" module and inside we have a configuration of this domain "example" example/mod.modal
`````

### A.4 Top-level module

`````
-- encapsulation keeps private by default (need to review how will wok)
print_file_name := (self) []u8 {
    return fake_print_function(self.fake_get_filename_function())
}

-- top-level constructor always will be public
init := (self) ...nothing goes here because compiler instead module name here automatically {
    return &self
}


-- other file...

-- this are already considered a constant because are a DEFINE in top-level
-- import are a comptime macro: import -> std.io.import, one of few macro available anytime
can_import_directly_the_module := import("nested_classes")
or_can_import_directly_the_file := import("./examples/nested_classes/001.modal")
`````

### A.5 Import forms

`````
-- self can type explicit the module or just leave only "self" with no-typing (syntax sugar)
-- new := (self: nested_classes)

-- stringfyed import name
-- std := import("std")

-- symbol-like import name
-- std := import(std)

-- import all children from a module
-- std := import("std").*

-- import all children from a sub module
-- io := import("std").io.*

-- descontruction-like import from a submodule
-- file_and_print := import("std").io.(file,print)

-- need "val" keyword for descontruction of "std.io"
-- descontruction_definition like importation with alias definition
-- val (
--      file as fake_get_filename_function,
--      print as fake_print_function
): import(std).io

-- descontruction-like importation from a submodule with alias definition
-- import("std").io.(
--     file as fake_get_filename_function,
--     print as fake_print_function
)

-- default/standard directly module import
-- fake_print_function := std.io.print
-- fake_get_filename_function := std.io.file
`````

### A.6 Method forms

`````
-- via variable with a depending where this was instanciated will be mutable or constant
main := () int {
    then_john_nested_classes.new() print_fake := can_import_directly_the_module()

    return 0
}

-- normal instanciation immutable always
main(): u8 {
    then_john_nested_classes.new()
    print_fake := can_import_directly_the_module()

    return 0
}

-- mutable instanciation, supporting function overloading
u8 main() {
    then_john_nested_classes.new()
    print_fake := can_import_directly_the_module()

    return 0
}
`````

### A.7 How the implementation departs from these inputs

- `u8 n1, n2` style grouping is not allowed for type-first; `n1, n2: u8` (type-last) is.
- Methods written inside the braces are **private**; public ones go below the struct.
- `init` / `new` are no longer public by special rule; position decides.
- `n1 + n2` without `self.` needs `implicit_self := true` in the domain's `mod.modal`.
- Two statements on one line (`...new() print_fake := ...`) is an error.
- `fake := std.io.print` works as member access on a binding; `std` itself is unchecked.

---

## Appendix B. Full source of every file

45 of the 46 files of the work, exactly as in `modal-v2-frontend.zip`. The one left out is
`docs/v2-frontend.md`, which this export supersedes.

| File | Lines |
|---|---|
| `src/v2/main.c` | 290 |
| `src/v2/compiler/cx.c` | 172 |
| `src/v2/compiler/cx.h` | 198 |
| `examples/v2/errors/syntax.modal` | 33 |
| `src/v2/compiler/syntax/parser/parse_decl.c` | 576 |
| `src/v2/compiler/syntax/parser/parse_expr.c` | 348 |
| `src/v2/compiler/syntax/parser/parse_stmt.c` | 85 |
| `src/v2/compiler/syntax/parser/parser.c` | 199 |
| `src/v2/compiler/syntax/parser/parser.h` | 82 |
| `src/v2/compiler/syntax/scanner.c` | 640 |
| `src/v2/compiler/syntax/scanner.h` | 141 |
| `src/v2/compiler/ast/ast.c` | 328 |
| `src/v2/compiler/ast/ast.h` | 113 |
| `src/v2/compiler/ast/symbol.c` | 187 |
| `src/v2/compiler/ast/symbol.h` | 87 |
| `src/v2/compiler/ast/visitor.h` | 92 |
| `src/v2/compiler/sema/check.c` | 1295 |
| `src/v2/compiler/sema/module.c` | 887 |
| `src/v2/compiler/sema/resolve.c` | 376 |
| `src/v2/compiler/sema/types.c` | 131 |
| `src/v2/compiler/sema/types.h` | 63 |
| `src/v2/compiler/backend/backend.h` | 54 |
| `src/v2/compiler/backend/c/emit.c` | 813 |
| `src/v2/compiler/backend/registry.c` | 111 |
| `src/v2/tests/run.sh` | 191 |
| `makefile` | 81 |
| `examples/v2/calls.modal` | 84 |
| `examples/v2/errors/calls.modal` | 48 |
| `examples/v2/errors/config/mod.modal` | 2 |
| `examples/v2/errors/far/reach.modal` | 2 |
| `examples/v2/errors/leak.modal` | 26 |
| `examples/v2/errors/private.modal` | 8 |
| `examples/v2/errors/types.modal` | 43 |
| `examples/v2/example/inner/deep.modal` | 5 |
| `examples/v2/example/inner/mod.modal` | 2 |
| `examples/v2/example/inner/up.modal` | 4 |
| `examples/v2/example/john.modal` | 52 |
| `examples/v2/example/mod.modal` | 8 |
| `examples/v2/example/secret.modal` | 8 |
| `examples/v2/main.modal` | 42 |
| `examples/v2/run/counter.expected` | 6 |
| `examples/v2/run/counter.modal` | 60 |
| `examples/v2/run/hello.expected` | 1 |
| `examples/v2/run/hello.modal` | 7 |
| `.gitignore` | 5 |

### `src/v2/main.c`

`````c
// Modal v2 driver: scan -> parse -> declare -> link -> check -> generate.
//
//   modal-v2 [options] file.modal
#define _XOPEN_SOURCE 700
#include "v2/compiler/backend/backend.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int usage(const char *argv0)
{
    fprintf(stderr,
            "modal v2 %s\n"
            "usage: %s [options] file.modal\n"
            "  check only (default): report errors, or one line saying what was verified\n"
            "  -o FILE          generate code into FILE\n"
            "  --run            generate, build with the backend's toolchain, and run\n"
            "  --backend NAME   backend to generate with (default: %s)\n"
            "  --backends       list the registered backends\n"
            "  --ast            print the syntax tree of every loaded file\n"
            "  --syms           print the symbol table with visibility\n"
            "  --sig            print the derived contract of every domain\n"
            "  --stats          print token, node, symbol and memory counts\n"
            "  --time           print the time spent in each phase\n"
            "  --eager          parse every body up front and keep the whole tree\n",
            MODAL_VERSION, argv0, backend_default()->name);
    return 1;
}

// --run: build the generated file with the backend's toolchain and run the result.
// Returns the exit status of the program, or 1 if it could not be built.
static int build_and_run(const Backend *backend, const char *source, const char *exe)
{
    const char *toolchain = getenv("CC") ? getenv("CC") : backend->toolchain;
    char cmd[1600];

    snprintf(cmd, sizeof(cmd), "%s -o '%s' '%s'", toolchain, exe, source);
    int status = system(cmd);
    if (status != 0)
    {
        fprintf(stderr, "fatal: '%s' failed; the generated file was kept at %s\n", toolchain,
                source);
        return 1;
    }
    remove(source);

    fflush(stdout);
    snprintf(cmd, sizeof(cmd), "'%s'", exe);
    status = system(cmd);
    remove(exe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char **argv)
{
    const char *entry = NULL;
    int show_ast = 0, show_syms = 0, show_stats = 0, show_sig = 0;
    int run = 0, show_time = 0, eager = 0;
    char gen_path[512] = "", exe_path[512] = "";
    FILE *gen = NULL;
    const char *out_path = NULL;
    const Backend *backend = backend_default();

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ast") == 0)
        {
            show_ast = 1;
        }
        else if (strcmp(argv[i], "--syms") == 0)
        {
            show_syms = 1;
        }
        else if (strcmp(argv[i], "--backends") == 0)
        {
            backend_list(stdout);
            return 0;
        }
        else if ((strcmp(argv[i], "--backend") == 0 || strcmp(argv[i], "-b") == 0) && i + 1 < argc)
        {
            backend = backend_find(argv[++i]);
            if (!backend)
            {
                fprintf(stderr, "%s: unknown backend '%s'. Registered backends:\n", argv[0],
                        argv[i]);
                backend_list(stderr);
                return 2;
            }
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
            out_path = argv[++i];
        }
        else if (strcmp(argv[i], "--eager") == 0)
        {
            eager = 1;
        }
        else if (strcmp(argv[i], "--time") == 0)
        {
            show_time = 1;
        }
        else if (strcmp(argv[i], "--run") == 0)
        {
            run = 1;
        }
        else if (strcmp(argv[i], "--sig") == 0)
        {
            show_sig = 1;
        }
        else if (strcmp(argv[i], "--stats") == 0)
        {
            show_stats = 1;
        }
        else if (argv[i][0] == '-' || entry)
        {
            return usage(argv[0]);
        }
        else
        {
            entry = argv[i];
        }
    }
    if (!entry)
    {
        fprintf(stderr, "%s: fatal: no input files\n", argv[0]);
        return usage(argv[0]);
    }

    symbol_new(&cx.units[0], SYM_NONE, 0, 0, 0, 0); // slot 0 = "no symbol"
    types_init();

    // Printing the tree needs all of it, so --ast implies --eager.
    cx.eager = (uint8_t)(eager || show_ast);
    cx.timing = (uint8_t)show_time;

    // Code generation is streamed while the program is checked, so the output is opened
    // before the first file is read.
    if (out_path || run)
    {
        if (run && !out_path)
        {
            const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
            if (!backend->toolchain)
            {
                fprintf(stderr, "fatal: backend '%s' has no toolchain to build with\n",
                        backend->name);
                return 1;
            }
            snprintf(exe_path, sizeof(exe_path), "%s/modal-run-%ld", tmp, (long)getpid());
            snprintf(gen_path, sizeof(gen_path), "%s%s", exe_path, backend->extension);
        }
        else
        {
            snprintf(gen_path, sizeof(gen_path), "%s", out_path);
        }
        gen = fopen(gen_path, "w");
        if (!gen)
        {
            fprintf(stderr, "fatal: cannot write '%s'\n", gen_path);
            return 1;
        }
        codegen_start(backend, gen);
    }

    if (!unit_load(entry))
    {
        fprintf(stderr, "%s: fatal: cannot read '%s'\n", argv[0], entry);
        if (gen)
        {
            fclose(gen);
            remove(gen_path);
        }
        arena_drop();
        return 1;
    }
    modules_link();

    if (gen)
    {
        codegen_finish();
        uint64_t t = clock_ns();
        fclose(gen);
        phase_end(PHASE_EMIT, t);
        if (cx.errors)
        {
            remove(gen_path); // never leave a half-translated file behind
        }
    }

    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        Unit *u = &cx.units[i];
        if (show_ast)
        {
            printf("== ast: %s\n", u->path);
            ast.print(u);
            putchar('\n');
        }
        if (show_syms)
        {
            printf("== symbols: %s\n", u->path);
            symbols_print(u);
            putchar('\n');
        }
    }

    if (show_sig)
    {
        contract_print();
    }

    if (show_stats)
    {
        size_t source = 0, tokens = 0, nodes = 0;
        for (uint16_t i = 0; i < cx.nunits; i++)
        {
            source += cx.units[i].len;
            tokens += cx.units[i].tokens.size;
            nodes += cx.units[i].nodes.size - 1;
        }
        printf("files %u  source %zu B  tokens %zu  nodes kept %zu  symbols %zu\n", cx.nunits,
               source, tokens, nodes, cx.syms.size - 1);
        printf("memory: %.1f KB in use, %.1f KB taken from the system at peak\n",
               (double)(arena_used() + cx.heap_bytes) / 1024.0,
               (double)(arena_peak() + cx.heap_peak) / 1024.0);
    }

    int status = cx.errors > 0;

    if (!status && run && !out_path)
    {
        status = build_and_run(backend, gen_path, exe_path); // the program's output and status
    }
    else if (!status && out_path)
    {
        printf("ok: %s -> %s (backend %s)\n", entry, out_path, backend->name);
    }
    else if (!status)
    {
        // Say what was verified: silence looks the same as not having run at all.
        printf("ok: %s: %u file%s, %zu declarations, %u invocations and %u names resolved\n", entry,
               cx.nunits, cx.nunits == 1 ? "" : "s", cx.syms.size - 1, cx.calls_resolved,
               cx.names_resolved);
    }

    if (show_time)
    {
        static const char *const names[PHASE__COUNT] = {"scan", "parse", "declare",
                                                        "link", "check", "emit"};
        uint64_t total = 0;
        size_t source = 0;
        for (uint16_t i = 0; i < cx.nunits; i++)
        {
            source += cx.units[i].len;
        }
        for (int i = 0; i < PHASE__COUNT; i++)
        {
            total += cx.phase_ns[i];
        }
        for (int i = 0; i < PHASE__COUNT; i++)
        {
            printf("%-8s %8.3f ms  %5.1f%%\n", names[i], (double)cx.phase_ns[i] / 1e6,
                   total ? 100.0 * (double)cx.phase_ns[i] / (double)total : 0.0);
        }
        printf("%-8s %8.3f ms  %.1f MB/s of source\n", "total", (double)total / 1e6,
               total ? (double)source / 1e6 / ((double)total / 1e9) : 0.0);
    }

    if (cx.errors > 0)
    {
        if (cx.errors > MAX_REPORTED)
        {
            fprintf(stderr, "\n%d errors (the first %d are shown)\n", cx.errors, MAX_REPORTED);
        }
        else
        {
            fprintf(stderr, "\n%d error%s\n", cx.errors, cx.errors == 1 ? "" : "s");
        }
    }
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        free(cx.units[i].tokens.data);
        free(cx.units[i].nodes.data);
    }
    arena_drop();
    return status;
}
`````

### `src/v2/compiler/cx.c`

`````c
#define _XOPEN_SOURCE 700
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

context cx = {.ok = 1};

static void arena_new_block(size_t need)
{
    size_t cap = need > ARENA_BLOCK_MIN ? need : ARENA_BLOCK_MIN;
    ArenaBlock *block = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
    if (!block)
    {
        fprintf(stderr, "fatal: out of memory while allocating %zu bytes\n", cap);
        exit(2);
    }
    block->prev = cx.arena.block;
    block->cap = cap;
    block->used = 0;
    cx.arena.block = block;
    cx.arena.reserved += sizeof(ArenaBlock) + cap;
    if (cx.arena.reserved > cx.arena.peak)
    {
        cx.arena.peak = cx.arena.reserved;
    }
}

void *arena_alloc(size_t sz)
{
    sz = (sz + 7u) & ~(size_t)7u;
    ArenaBlock *block = cx.arena.block;
    if (!block || block->used + sz > block->cap)
    {
        arena_new_block(sz);
        block = cx.arena.block;
    }
    void *mem = (char *)(block + 1) + block->used;
    block->used += sz;
    return mem;
}

ArenaMark arena_mark(void)
{
    return (ArenaMark){cx.arena.block, cx.arena.block ? cx.arena.block->used : 0};
}

void arena_release(ArenaMark mark)
{
    while (cx.arena.block != mark.block)
    {
        ArenaBlock *block = cx.arena.block;
        cx.arena.block = block->prev;
        cx.arena.reserved -= sizeof(ArenaBlock) + block->cap;
        free(block);
    }
    if (cx.arena.block)
    {
        cx.arena.block->used = mark.used;
    }
}

void arena_shrink(void *ptr, size_t old_size, size_t new_size)
{
    ArenaBlock *block = cx.arena.block;
    old_size = (old_size + 7u) & ~(size_t)7u;
    new_size = (new_size + 7u) & ~(size_t)7u;
    if (block && (char *)ptr + old_size == (char *)(block + 1) + block->used)
    {
        block->used -= old_size - new_size;
    }
}

void *arena_grow(void *data, size_t elem, size_t *cap)
{
    size_t new_cap = *cap ? *cap * 2 : 16;
    void *grown = arena_alloc(new_cap * elem);
    if (*cap)
    {
        memcpy(grown, data, *cap * elem);
    }
    *cap = new_cap;
    return grown;
}

size_t arena_used(void)
{
    size_t used = 0;
    for (const ArenaBlock *b = cx.arena.block; b; b = b->prev)
    {
        used += b->used;
    }
    return used;
}

size_t arena_reserved(void)
{
    return cx.arena.reserved;
}

size_t arena_peak(void)
{
    return cx.arena.peak;
}

void arena_drop(void)
{
    arena_release((ArenaMark){NULL, 0});
}

uint64_t clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void diag_vat(Unit *u, uint32_t tok, const char *level, const char *fmt, va_list ap)
{
    const Token *t = TOK(u, tok);

    // After MAX_REPORTED errors the rest are counted but not printed: a mistake repeated
    // across a large file should not bury the first reports or cost seconds of output.
    if (level[0] == 'e' && cx.errors >= MAX_REPORTED)
    {
        u->errors++;
        cx.errors++;
        cx.ok = 0;
        return;
    }

    int line, col;
    source_pos(u->src, t->start, &line, &col);

    fprintf(stderr, "\n%s [%s:%d:%d]: ", level, u->path, line, col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);

    // Whole source line for context: the token points straight into the source buffer.
    const char *line_start = t->start;
    while (line_start > u->src && line_start[-1] != '\n')
    {
        line_start--;
    }
    const char *line_end = t->start;
    while (*line_end && *line_end != '\n')
    {
        line_end++;
    }
    fprintf(stderr, "%4d | %.*s\n     | ", line, (int)(line_end - line_start), line_start);
    for (const char *c = line_start; c < t->start; c++)
    {
        fputc(*c == '\t' ? '\t' : ' ', stderr);
    }
    fputs("^\n", stderr);

    if (level[0] == 'e')
    {
        u->errors++;
        cx.errors++;
        cx.ok = 0;
    }
}

void diag_at(Unit *u, uint32_t tok, const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_vat(u, tok, level, fmt, ap);
    va_end(ap);
}
`````

### `src/v2/compiler/cx.h`

`````c
// cx.h (v2): compilation context. One growable arena for everything, one Unit per source file.
#ifndef MODAL_V2_CX_H
#define MODAL_V2_CX_H

#include "v2/compiler/ast/ast.h"
#include "v2/compiler/ast/symbol.h"
#include "v2/compiler/sema/types.h"
#include "v2/compiler/syntax/scanner.h"
#include <stdarg.h>
#include <stdint.h>

// Compiler phases, for `--time` (the same idea as `go tool compile -bench`).
typedef enum
{
    PHASE_SCAN,
    PHASE_PARSE,
    PHASE_DECLARE,
    PHASE_LINK,
    PHASE_CHECK,
    PHASE_EMIT,
    PHASE__COUNT,
} Phase;

typedef struct ArenaBlock
{
    struct ArenaBlock *prev;
    size_t cap; // usable bytes after this header
    size_t used;
} ArenaBlock;

typedef struct
{
    ArenaBlock *block; // newest block
    size_t reserved;
    size_t peak;
} Arena;

typedef struct
{
    ArenaBlock *block;
    size_t used;
} ArenaMark;

#define ARENA_BLOCK_MIN (4 * 1024)

#define MAX_UNITS 64
#define MAX_REPORTED 20 // errors printed in full; the rest are only counted
#define MAX_DOMAINS 32

// A source file. The file is also a module: `example/john.modal` is module `john` in domain
// (folder) `example`, and `example/mod.modal` is the domain's public face.
struct Unit
{
    const char *path;  // as shown in diagnostics
    const char *canon; // canonical path: identity for the module cache
    const char *src;
    size_t len;
    const char *name; // module name: file stem, or the folder name for mod.modal
    int name_len;
    TokenVec tokens;
    NodeVec nodes;
    NodeId root;
    Scope *scope;
    TypeId *types;   // per node: the type of each expression, filled by the checker
    SymId *refs;     // per node: the declaration a name or invocation resolved to
    size_t side_cap; // entries allocated in types and refs
    uint16_t id;
    uint16_t domain;
    size_t max_body;     // upper bound on the tokens of the largest deferred body
    uint32_t nimports;   // import(...) expressions in this file
    uint8_t is_manifest; // this file is a mod.modal
    uint8_t bound;       // the domain's mod.modal binds this module: it is part of the face
    uint8_t broken;      // did not scan, parse or declare cleanly: later phases skip it
    int errors;
};

typedef struct
{
    Arena arena;
    Unit units[MAX_UNITS];
    uint16_t nunits;
    uint16_t linked;                  // units whose imports and manifest have been processed
    const char *domains[MAX_DOMAINS]; // canonical folder paths
    uint16_t ndomains;
    SymVec syms;    // slot 0 is reserved as "no symbol"
    TypeVec types;  // the type table (see sema/types.h)
    Scope **scopes; // scope table: a 24-byte Symbol refers to its member scope by index
    uint32_t nscopes;
    uint32_t scopes_cap;
    int errors;
    // Token and node arrays grow while a file is read, which an arena cannot do without
    // leaving old copies behind, so they are malloc'd, trimmed to size and freed at exit.
    size_t heap_bytes; // their current total
    size_t heap_peak;
    uint64_t phase_ns[PHASE__COUNT]; // time spent per phase
    uint32_t names_resolved;         // names and invocations the body checker led to a declaration
    uint32_t calls_resolved;
    uint8_t ok;
    uint8_t eager;  // parse every body up front and keep the whole tree (--eager, --ast)
    uint8_t timing; // --time: measure the per-function phases too
} context;

extern context cx;

// ---- arena --------------------------------------------------------------------------------
//
// Everything the compiler allocates comes from here and is freed at once at the end. The
// arena grows in blocks, on demand: a small program takes a few KB from the system, not a
// slab reserved up front. Allocations never move, so pointers into the arena stay valid.

void *arena_alloc(size_t sz); // 8-byte aligned; never returns NULL: exits on exhaustion
ArenaMark arena_mark(void);
void arena_release(ArenaMark mark); // frees everything allocated after the mark
// Gives back the tail of the most recent allocation (no-op if it is not the most recent).
void arena_shrink(void *ptr, size_t old_size, size_t new_size);
// Doubles an array of `elem`-sized items: returns the new storage, updates *cap.
void *arena_grow(void *data, size_t elem, size_t *cap);
size_t arena_used(void);     // bytes handed out
size_t arena_reserved(void); // bytes taken from the system right now
size_t arena_peak(void);     // the most ever taken from the system
void arena_drop(void);

static inline int arena_mark_equal(ArenaMark a, ArenaMark b)
{
    return a.block == b.block && a.used == b.used;
}

// Accounts a change in the size of a malloc'd array (see heap_bytes).
static inline void heap_account(size_t before, size_t after)
{
    cx.heap_bytes = cx.heap_bytes - before + after;
    if (cx.heap_bytes > cx.heap_peak)
    {
        cx.heap_peak = cx.heap_bytes;
    }
}

// ---- timing ----------------------------------------------------------------------------------

uint64_t clock_ns(void);

// phase_end(PHASE_X, start) adds the time since `start` to the phase and returns "now", so
// consecutive phases chain: t = phase_end(A, t); ...; t = phase_end(B, t);
static inline uint64_t phase_end(Phase phase, uint64_t start)
{
    uint64_t now = clock_ns();
    cx.phase_ns[phase] += now - start;
    return now;
}

// ---- diagnostics ---------------------------------------------------------------------------

// Prints `level [file:line:col]: message`, the source line and a caret under the token.
// level "error" also bumps the unit's and the global error counters.
void diag_vat(Unit *u, uint32_t tok, const char *level, const char *fmt, va_list ap);
void diag_at(Unit *u, uint32_t tok, const char *level, const char *fmt, ...);

// ---- pipeline ------------------------------------------------------------------------------

// module.c
Unit *unit_load(const char *path); // read + scan + parse + declare; cached by canonical path
void modules_link(void);           // load imports, read mod.modal, check visibility and bodies
// A function body was just parsed into nodes [from, size): resolve the imports written in it
// and check its module paths.
void module_body_nodes(Unit *u, NodeId from);
// Module-path checks over the nodes [from, to) of a file.
void module_check_paths(Unit *u, NodeId from, NodeId to);
void contract_print(void); // derived public signatures of every loaded domain
const char *domain_name(uint16_t domain);

// What a mod.modal says for a domain; `fallback` when it does not say.
int domain_config_bool(uint16_t domain, const char *key, int fallback);
// Could a wildcard import of this file (`import(x).*`) supply `name`?
int module_wildcard_provides(Unit *u, const char *name, size_t len);

// resolve.c
void sema_declare(Unit *u);
NodeId sema_callable_params(const Unit *u, NodeId decl);

// backend/registry.c: code generation is streamed, one checked function at a time.
struct Backend;
void codegen_start(const struct Backend *backend, void *out_file);
void codegen_begin(Unit *u);              // declarations are checked: types, prototypes
void codegen_function(Unit *u, SymId fn); // one function is checked and still in memory
int codegen_finish(void);                 // returns the number of errors the backend reported

// check.c
// What a backend asks about a declaration, beyond the per-node tables.
TypeId sema_fn_ret(Unit *u, SymId fn);                   // declared return type (T_VOID if none)
TypeId sema_param_type(Unit *u, SymId fn, NodeId param); // type of one of its parameters
TypeId sema_value_type(Unit *u, SymId field_or_const);
NodeId sema_fn_params(const Unit *u, SymId fn);      // its parameters after `self`
int sema_is_std_print(const Unit *u, SymId binding); // `x := import("std").io.print`
void sema_check(Unit *u);
void sema_syntax_only(Unit *u); // for a file that is not checked: parse its bodies for errors //
                                // every name and invocation in a body leads to a declaration

#endif // MODAL_V2_CX_H
`````

### `examples/v2/errors/syntax.modal`

`````
-- each mistake below is reported once, and parsing carries on after it

val (
    file as,
    123,
    print as fake_print
): import(std).io

point: struct {
pub:
    x: u8,
    u8 y
}

area(self: point): u8 {
    a := self.x b := self.y
    return a * b
}

area(self: point): u8 {
    return 0
}

u8 twice(n: u8): n * 2
u8 twice(m: u8): m + m

grow(self: elsewhere): u8 {
    return 1
}

sum(a, b): u8 {
    return a + b
}
`````

### `src/v2/compiler/syntax/parser/parse_decl.c`

`````c
// parse_decl.c (v2): declarations.
//
// One rule covers fields and methods: where the type is written decides mutability.
//
//   name: T                     immutable field            (type last)
//   a, b: T                     immutable fields, grouped
//   T name                      mutable field              (type first, one name)
//   name: struct { ... }        struct (a domain of fields and methods)
//   name(params): T { ... }     immutable method, defined once
//   T name(params) { ... }      mutable method, may be overloaded
//   T name(params): expr        mutable method, one-line body
//   name := value               binding (a function literal, an import, a constant)
//   val (a as b, c): import(x)  destructuring import
//   fun name(params) T { ... }  keyword profile of the immutable method
//
// The form is decided from the first two tokens, so nothing is ever re-parsed.
#include "v2/compiler/syntax/parser/parser.h"

static NodeId parse_struct_body(Parser *p);

int type_starts_at(const Token *t)
{
    switch (t->kind)
    {
    case TOK_IDENTIFIER:
    case TOK_STAR:
    case TOK_STRUCT:
        return 1;
    case TOK_LBRACKET:
        return t[1].kind == TOK_RBRACKET;
    default:
        return 0;
    }
}

// type := '[' ']' type | '*' type | 'struct' '{' members '}' | IDENT ('.' IDENT)*
NodeId parse_type(Parser *p)
{
    Unit *u = p->unit;
    const Token *t = p->current;
    NodeId type = 0;

    if (!parser_enter(p))
    {
        return 0;
    }

    switch (t->kind)
    {
    case TOK_LBRACKET:
        parser_advance(p);
        if (parser_consume(p, TOK_RBRACKET, "expected ']' (a slice type is written []T)"))
        {
            NodeId elem = parse_type(p);
            type = elem ? ast.new.node(u, AST_TYPE_SLICE, tok_idx(p, t), elem, 0, 0) : 0;
        }
        break;
    case TOK_STAR:
    {
        parser_advance(p);
        NodeId elem = parse_type(p);
        type = elem ? ast.new.node(u, AST_TYPE_PTR, tok_idx(p, t), elem, 0, 0) : 0;
        break;
    }
    case TOK_STRUCT:
    {
        parser_advance(p);
        NodeId members = parse_struct_body(p);
        type = p->panic ? 0 : ast.new.node(u, AST_TYPE_STRUCT, tok_idx(p, t), members, 0, 0);
        break;
    }
    case TOK_IDENTIFIER:
    {
        uint16_t extra = 0;
        parser_advance(p);
        while (p->current->kind == TOK_DOT && !(p->current->nl) &&
               p->current[1].kind == TOK_IDENTIFIER)
        {
            parser_advance(p);
            parser_advance(p);
            extra++;
        }
        type = ast.new.node(u, AST_TYPE_NAME, tok_idx(p, t), 0, 0, 0);
        NODE(u, type)->aux = extra;
        break;
    }
    default:
        parser_error_at(p, t, "expected a type");
        break;
    }

    parser_leave(p);
    return type;
}

// params := '(' [ param (',' param)* ] ')'      param := IDENT [ ':' type ]
//
// A type applies backwards to the untyped names before it: (n3, n4: u8).
// `self` is the only parameter that may stay untyped, and it never joins a group:
// (self, n3: u8) leaves self as the owner.
NodeId parse_params(Parser *p, int *has_self)
{
    Unit *u = p->unit;
    NodeList params = {0};
    NodeId pending = 0; // first name still waiting for its type
    int index = 0;

    *has_self = 0;
    if (!parser_consume(p, TOK_LPAREN, "expected '(' to open the parameter list"))
    {
        return 0;
    }
    p->paren_depth++;

    while (p->current->kind != TOK_RPAREN && p->current->kind != TOK_EOF)
    {
        const Token *name = p->current;
        if (!parser_consume(p, TOK_IDENTIFIER, "expected a parameter name"))
        {
            break;
        }

        NodeId param = ast.new.node(u, AST_PARAM, tok_idx(p, name), 0, 0, 0);
        int is_self = is_self_token(name->start, name->len);
        node_list_push(u, &params, param);

        if (is_self)
        {
            if (index != 0)
            {
                parser_error_at(p, name, "'self' must be the first parameter");
                break;
            }
            *has_self = 1;
        }

        if (parser_match(p, TOK_COLON))
        {
            NodeId type = parse_type(p);
            if (!type)
            {
                break;
            }
            NODE(u, param)->a = type;
            for (NodeId id = pending; id && id != param; id = NODE(u, id)->next)
            {
                NODE(u, id)->a = type;
            }
            pending = 0;
        }
        else if (!is_self && !pending)
        {
            pending = param;
        }

        index++;
        if (!parser_match(p, TOK_COMMA))
        {
            break;
        }
    }

    p->paren_depth--;
    if (p->panic)
    {
        return 0;
    }
    if (pending)
    {
        const Token *name = TOK(u, NODE(u, pending)->tok);
        parser_error_at(p, name, "parameter '%.*s' needs a type", name->len, name->start);
        return 0;
    }
    parser_consume(p, TOK_RPAREN, "expected ')' after the parameters");
    return params.head;
}

// Everything after the method name. `flags` carries AST_F_MUT for the type-first form, in
// which case `ret` is the type that was already parsed.
static NodeId parse_method(Parser *p, const Token *name, NodeId ret, uint8_t flags)
{
    Unit *u = p->unit;
    int has_self = 0;
    NodeId body = 0;
    int deferred = 0;
    NodeId params = parse_params(p, &has_self);

    if (p->panic)
    {
        return 0;
    }

    if (flags & AST_F_MUT)
    {
        if (parser_match(p, TOK_COLON))
        {
            body = parse_expression(p); // T name(params): expr
            flags |= AST_F_ONELINE;
        }
        else if (is_body_start(p->current))
        {
            body = parse_body(p, &deferred);
        }
        else
        {
            parser_error_at(p, p->current,
                            "expected '{' or ': expression' after the parameters of '%.*s'",
                            name->len, name->start);
        }
    }
    else
    {
        if (parser_match(p, TOK_COLON))
        {
            if ((p->current->nl) || !type_starts_at(p->current))
            {
                parser_error_at(p, p->previous, "expected a return type after ':'");
                return 0;
            }
            ret = parse_type(p);
        }
        else if (!is_body_start(p->current) && !(p->current->nl) && type_starts_at(p->current))
        {
            ret = parse_type(p); // keyword profile: fun name(params) T { ... }
        }

        if (!p->panic && !is_body_start(p->current))
        {
            parser_error_at(p, p->current,
                            "expected '{' to open the body of '%.*s' (a one-line body needs the "
                            "type-first form: `T %.*s(...): expression`)",
                            name->len, name->start, name->len, name->start);
        }
        if (!p->panic)
        {
            body = parse_body(p, &deferred);
        }
    }

    if (p->panic || !body)
    {
        return 0;
    }

    NodeId method = ast.new.node(u, AST_METHOD_DECL, tok_idx(p, name), params, ret, body);
    NODE(u, method)->flags =
        (uint8_t)(flags | (has_self ? AST_F_SELF : 0) | (deferred ? AST_F_LAZY : 0));
    return method;
}

static NodeId parse_initializer(Parser *p)
{
    return parser_match(p, TOK_EQ) ? parse_expression(p) : 0;
}

// name: T    a, b: T    name: struct { ... }
static NodeId parse_type_last(Parser *p)
{
    Unit *u = p->unit;
    NodeList fields = {0};
    int count = 0;

    do
    {
        const Token *name = p->current;
        if (!parser_consume(p, TOK_IDENTIFIER, "expected a name"))
        {
            return 0;
        }
        node_list_push(u, &fields, ast.new.node(u, AST_FIELD_DECL, tok_idx(p, name), 0, 0, 0));
        count++;
    } while (parser_match(p, TOK_COMMA));

    if (!parser_consume(p, TOK_COLON, "expected ':' and a type after the name"))
    {
        return 0;
    }
    if ((p->current->nl) || !type_starts_at(p->current))
    {
        parser_error_at(p, p->previous,
                        "expected a type after ':' on the same line (there are no `pub:` / "
                        "`mut:` sections: mutability is `T name`, exposure is declared in the "
                        "domain's mod.modal)");
        return 0;
    }

    if (count == 1 && p->current->kind == TOK_STRUCT)
    {
        parser_advance(p);
        NodeId members = parse_struct_body(p);
        if (p->panic)
        {
            return 0;
        }
        AstNode *decl = NODE(u, fields.head);
        decl->kind = AST_STRUCT_DECL;
        decl->a = members;
        return fields.head;
    }

    NodeId type = parse_type(p);
    if (!type)
    {
        return 0;
    }
    NodeId init = parse_initializer(p);
    if (p->panic)
    {
        return 0;
    }
    // A grouped declaration shares one type (and initializer) node between its names.
    for (NodeId id = fields.head; id; id = NODE(u, id)->next)
    {
        NODE(u, id)->a = type;
        NODE(u, id)->b = init;
    }
    return fields.head;
}

// T name    T name(params) ...
static NodeId parse_type_first(Parser *p)
{
    Unit *u = p->unit;
    const Token *start = p->current;
    NodeId type = parse_type(p);

    if (!type)
    {
        return 0;
    }
    if (p->current->kind != TOK_IDENTIFIER || (p->current->nl))
    {
        parser_error_at(p, start,
                        "expected a declaration: `name: T`, `T name`, `name(...): T { }`, "
                        "`T name(...) { }` or `name := value`");
        return 0;
    }

    const Token *name = p->current;
    parser_advance(p);

    if (p->current->kind == TOK_LPAREN && !(p->current->nl))
    {
        return parse_method(p, name, type, AST_F_MUT);
    }

    NodeId init = parse_initializer(p);
    if (p->panic)
    {
        return 0;
    }
    NodeId field = ast.new.node(u, AST_FIELD_DECL, tok_idx(p, name), type, init, 0);
    NODE(u, field)->flags = AST_F_MUT;
    return field;
}

// name := value
NodeId parse_define(Parser *p)
{
    const Token *name = p->current;
    parser_advance(p); // name
    parser_advance(p); // :=
    NodeId value = parse_expression(p);
    if (!value)
    {
        return 0;
    }
    return ast.new.node(p->unit, AST_DEFINE, tok_idx(p, name), value, 0, 0);
}

// items := item (',' item)*      item := IDENT [ 'as' IDENT ]
//
// Called just after the '(' token `open`; stops at the closing ')' without consuming it.
// A malformed item is reported and skipped on its own: the scanner already paired the
// parentheses, so the parser can always find the next ',' or the end of this list and carry
// on, instead of losing the rest of the file.
NodeId parse_import_items(Parser *p, const Token *open)
{
    Unit *u = p->unit;
    NodeList items = {0};

    if (p->current->kind == TOK_RPAREN)
    {
        parser_error_at(p, open, "the import list is empty");
        p->panic = 0;
        return 0;
    }

    while (p->current->kind != TOK_RPAREN && p->current->kind != TOK_EOF)
    {
        const Token *name = p->current;
        NodeId item = 0;

        if (name->kind == TOK_IDENTIFIER)
        {
            parser_advance(p);
            item = ast.new.node(u, AST_IMPORT_ITEM, tok_idx(p, name), 0, 0, 0);
            if (parser_match(p, TOK_AS))
            {
                if (p->current->kind == TOK_IDENTIFIER)
                {
                    NODE(u, item)->c = tok_idx(p, p->current);
                    parser_advance(p);
                }
                else
                {
                    parser_error_at(p, p->current, "expected an alias name after 'as'");
                }
            }
        }
        else
        {
            parser_error_at(p, name, "expected a name in the import list");
        }

        if (!p->panic && p->current->kind != TOK_COMMA && p->current->kind != TOK_RPAREN)
        {
            parser_error_at(p, p->current, "expected ',' or ')' in the import list");
        }

        if (p->panic)
        {
            while (p->current->kind != TOK_EOF && p->current->kind != TOK_COMMA &&
                   p->current->kind != TOK_RPAREN && !(open->match == 0 && (p->current->nl)))
            {
                parser_skip_one(p);
            }
            p->panic = 0;
        }
        else
        {
            node_list_push(u, &items, item);
        }

        if (!parser_match(p, TOK_COMMA))
        {
            break;
        }
    }
    return items.head;
}

// val (a as b, c): import(x).y
static NodeId parse_val_decl(Parser *p)
{
    Unit *u = p->unit;
    const Token *val = p->current;
    parser_advance(p);

    const Token *open = p->current;
    if (!parser_consume(p, TOK_LPAREN, "expected '(' after 'val'"))
    {
        return 0;
    }
    p->paren_depth++;
    NodeId items = parse_import_items(p, open);
    p->paren_depth--;

    if (!parser_consume(p, TOK_RPAREN, "expected ')' to close the val list") ||
        !parser_consume(p, TOK_COLON, "expected ': import(...)' after the val list"))
    {
        return 0;
    }

    NodeId source = parse_expression(p);
    if (!source)
    {
        return 0;
    }
    if (!ast_import_root(u, source))
    {
        parser_error_at(p, val, "val (...) destructures an import: `val (a as b): import(x).y`");
        return 0;
    }
    return ast.new.node(u, AST_VAL_DECL, tok_idx(p, val), items, source, 0);
}

// '{' member* '}' with ',', ';' or a newline between members.
static NodeId parse_struct_body(Parser *p)
{
    Unit *u = p->unit;
    NodeList members = {0};

    if (!parser_consume(p, TOK_LBRACE, "expected '{' to open the struct body") || !parser_enter(p))
    {
        return 0;
    }
    int saved_paren_depth = p->paren_depth;
    p->paren_depth = 0;

    while (p->current->kind != TOK_RBRACE && p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_COMMA) || parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }

        NodeId member = parse_declaration(p, DECL_STRUCT);
        if (p->panic)
        {
            parser_synchronize(p);
            continue;
        }
        node_list_push(u, &members, member);

        TokenKind k = p->current->kind;
        if (k != TOK_RBRACE && k != TOK_COMMA && k != TOK_SEMICOLON && k != TOK_EOF &&
            !(p->current->nl))
        {
            parser_error_at(p, p->current, "expected ',' or a newline between struct members");
            parser_synchronize(p);
        }
    }

    p->paren_depth = saved_paren_depth;
    parser_leave(p);
    parser_consume(p, TOK_RBRACE, "expected '}' to close the struct");
    return members.head;
}

NodeId parse_declaration(Parser *p, DeclContext ctx)
{
    const Token *t = p->current;

    switch (t->kind)
    {
    case TOK_VAL:
    case TOK_IMPORT:
    case TOK_WRITE:
        if (ctx != DECL_TOP)
        {
            parser_error_at(p, t, "only fields, methods and bindings can appear in a struct");
            return 0;
        }
        return t->kind == TOK_VAL ? parse_val_decl(p) : parse_expression(p);

    case TOK_FUNCTION:
        parser_advance(p);
        t = p->current;
        if (!parser_consume(p, TOK_IDENTIFIER, "expected a function name after 'fun'"))
        {
            return 0;
        }
        return parse_method(p, t, 0, 0);

    case TOK_IDENTIFIER:
        switch (t[1].kind)
        {
        case TOK_DEFINE:
            return parse_define(p);
        case TOK_COLON:
        case TOK_COMMA:
            return parse_type_last(p);
        case TOK_LPAREN:
            if (!(t[1].nl))
            {
                parser_advance(p);
                return parse_method(p, t, 0, 0);
            }
            break;
        default:
            break;
        }
        return parse_type_first(p);

    case TOK_LBRACKET:
    case TOK_STAR:
        return parse_type_first(p);

    default:
        parser_error_at(p, t,
                        "expected a declaration: `name: T`, `T name`, `name(...): T { }`, "
                        "`T name(...) { }` or `name := value`");
        return 0;
    }
}
`````

### `src/v2/compiler/syntax/parser/parse_expr.c`

`````c
// parse_expr.c (v2): Pratt expression engine.
//
// Each token kind has at most one prefix handler (NUD), one infix handler (LED) and an infix
// precedence, held in a table indexed by TokenKind. The core loop is
//
//     left = nud();  while (prec < precedence(current)) left = led(left);
//
// so a chain of operators costs one loop iteration each instead of one call per grammar level.
#include "v2/compiler/syntax/parser/parser.h"

typedef enum
{
    PREC_NONE,
    PREC_ASSIGN, // =
    PREC_OR,     // or
    PREC_AND,    // and
    PREC_CMP,    // == != < > <= >=
    PREC_TERM,   // + -
    PREC_FACTOR, // * /
    PREC_UNARY,  // - ! & *
    PREC_CALL,   // . () [] "string"
} Precedence;

typedef NodeId (*NudFn)(Parser *p);
typedef NodeId (*LedFn)(Parser *p, NodeId left);

typedef struct
{
    NudFn nud;
    LedFn led;
    uint8_t prec;
} ParseRule;

static const ParseRule rules[TOK__COUNT];

static NodeId parse_precedence(Parser *p, int prec)
{
    const ParseRule *rule = &rules[p->current->kind];
    if (!rule->nud)
    {
        parser_error_at(p, p->current, "expected an expression");
        return 0;
    }
    if (!parser_enter(p))
    {
        return 0;
    }

    NodeId left = rule->nud(p);
    while (left)
    {
        const Token *t = p->current;
        rule = &rules[t->kind];
        if (!rule->led || rule->prec <= prec)
        {
            break;
        }
        // A newline ends the expression, except inside ( ) / [ ] and before a leading '.'.
        if ((t->nl) && p->paren_depth == 0 && t->kind != TOK_DOT)
        {
            break;
        }
        left = rule->led(p, left);
    }

    parser_leave(p);
    return left;
}

NodeId parse_expression(Parser *p)
{
    return parse_precedence(p, PREC_NONE);
}

// ---- prefix handlers -------------------------------------------------------------------------

static NodeId nud_number(Parser *p)
{
    parser_advance(p);
    return ast.new.node(p->unit, AST_NUMBER_LIT, tok_idx(p, p->previous), 0, 0, 0);
}

static NodeId nud_string(Parser *p)
{
    parser_advance(p);
    return ast.new.node(p->unit, AST_STRING_LIT, tok_idx(p, p->previous), 0, 0, 0);
}

static NodeId nud_ident(Parser *p)
{
    parser_advance(p);
    return ast.new.node(p->unit, AST_IDENT, tok_idx(p, p->previous), 0, 0, 0);
}

static NodeId nud_unary(Parser *p)
{
    const Token *op = p->current;
    parser_advance(p);
    NodeId operand = parse_precedence(p, PREC_UNARY);
    return operand ? ast.new.node(p->unit, AST_UNARY_OP, tok_idx(p, op), operand, 0, 0) : 0;
}

// `(` opens either a grouped expression or a function literal `(params) [T] { ... }`.
// The scanner stored the index of the matching `)`, so one look at the token after it decides
// which, in O(1) and without parsing anything twice.
static int paren_opens_fn_literal(const Parser *p, const Token *open)
{
    if (!open->match)
    {
        return 0;
    }
    const Token *after = TOK(p->unit, open->match) + 1;
    if (is_body_start(after))
    {
        return 1;
    }
    if (after->nl)
    {
        return 0;
    }
    return after->kind == TOK_IDENTIFIER ||
           (after->kind == TOK_LBRACKET && after[1].kind == TOK_RBRACKET);
}

// (params) [T] { body }
static NodeId parse_fn_literal(Parser *p)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    int has_self = 0, deferred = 0;
    NodeId ret = 0;
    NodeId params = parse_params(p, &has_self);
    if (p->panic)
    {
        return 0;
    }
    if (!is_body_start(p->current))
    {
        ret = parse_type(p);
        if (!ret)
        {
            return 0;
        }
    }
    NodeId body = parse_body(p, &deferred);
    if (!body)
    {
        return 0;
    }
    NodeId fn = ast.new.node(u, AST_FN_LIT, tok_idx(p, open), params, ret, body);
    NODE(u, fn)->flags = (uint8_t)((has_self ? AST_F_SELF : 0) | (deferred ? AST_F_LAZY : 0));
    return fn;
}

static NodeId nud_paren(Parser *p)
{
    Unit *u = p->unit;
    const Token *open = p->current;

    if (paren_opens_fn_literal(p, open))
    {
        return parse_fn_literal(p);
    }

    parser_advance(p);
    p->paren_depth++;
    NodeId inner = parse_precedence(p, PREC_NONE);
    p->paren_depth--;
    if (!inner || !parser_consume(p, TOK_RPAREN, "expected ')' to close the group"))
    {
        return 0;
    }
    return ast.new.node(u, AST_PAREN_GROUP, tok_idx(p, open), inner, 0, 0);
}

// import("std")   import(std)   -- a comptime macro; selectors are applied by led_dot.
static NodeId nud_import(Parser *p)
{
    parser_advance(p);
    if (!parser_consume(p, TOK_LPAREN, "expected '(' after import"))
    {
        return 0;
    }
    const Token *arg = p->current;
    if (arg->kind != TOK_STRING && arg->kind != TOK_IDENTIFIER)
    {
        parser_error_at(p, arg, "import takes a module name: import(\"std\") or import(std)");
        return 0;
    }
    parser_advance(p);
    if (!parser_consume(p, TOK_RPAREN, "expected ')' after the module name"))
    {
        return 0;
    }
    return ast.new.node(p->unit, AST_IMPORT, tok_idx(p, arg), 0, 0, 0);
}

// ---- infix handlers --------------------------------------------------------------------------

static NodeId led_binary(Parser *p, NodeId left)
{
    const Token *op = p->current;
    parser_advance(p);
    NodeId right = parse_precedence(p, rules[op->kind].prec); // left-associative
    return right ? ast.new.node(p->unit, AST_BIN_OP, tok_idx(p, op), left, right, 0) : 0;
}

static NodeId led_assign(Parser *p, NodeId left)
{
    const Token *op = p->current;
    parser_advance(p);
    NodeId value = parse_precedence(p, PREC_ASSIGN - 1); // right-associative
    return value ? ast.new.node(p->unit, AST_ASSIGN, tok_idx(p, op), left, value, 0) : 0;
}

static NodeId led_call(Parser *p, NodeId left)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    NodeList args = {0};

    parser_advance(p);
    p->paren_depth++;
    while (p->current->kind != TOK_RPAREN && p->current->kind != TOK_EOF)
    {
        NodeId arg = parse_precedence(p, PREC_ASSIGN);
        if (!arg)
        {
            break;
        }
        node_list_push(u, &args, arg);
        if (!parser_match(p, TOK_COMMA))
        {
            break;
        }
    }
    p->paren_depth--;

    if (p->panic || !parser_consume(p, TOK_RPAREN, "expected ')' after the arguments"))
    {
        return 0;
    }
    return ast.new.node(u, AST_CALL, tok_idx(p, open), left, args.head, 0);
}

// Lua-style call with a single string argument: print "hello"
static NodeId led_string_call(Parser *p, NodeId left)
{
    Unit *u = p->unit;
    uint8_t kind = NODE(u, left)->kind;

    if (kind != AST_IDENT && kind != AST_SELECTOR)
    {
        parser_error_at(p, p->current, "unexpected string after an expression");
        return 0;
    }
    NodeId arg = nud_string(p);
    return ast.new.node(u, AST_CALL, NODE(u, arg)->tok, left, arg, 0);
}

static NodeId led_index(Parser *p, NodeId left)
{
    const Token *open = p->current;
    parser_advance(p);
    p->paren_depth++;
    NodeId index = parse_precedence(p, PREC_NONE);
    p->paren_depth--;
    if (!index || !parser_consume(p, TOK_RBRACKET, "expected ']' after the index"))
    {
        return 0;
    }
    return ast.new.node(p->unit, AST_INDEX, tok_idx(p, open), left, index, 0);
}

// left.name   and, on an import chain only:   left.*   left.(a, b as c)
static NodeId led_dot(Parser *p, NodeId left)
{
    Unit *u = p->unit;
    uint8_t left_kind = NODE(u, left)->kind;
    parser_advance(p); // '.'
    const Token *t = p->current;

    if (left_kind == AST_IMPORT_ALL || left_kind == AST_IMPORT_PICK)
    {
        parser_error_at(p, t, "nothing can follow `.*` or an import list");
        return 0;
    }

    if (t->kind == TOK_IDENTIFIER)
    {
        parser_advance(p);
        return ast.new.node(u, AST_SELECTOR, tok_idx(p, t), left, 0, 0);
    }

    if (t->kind == TOK_STAR || t->kind == TOK_LPAREN)
    {
        if (!ast_import_root(u, left))
        {
            parser_error_at(p, t, "`.*` and `.(...)` are only valid on an import");
            return 0;
        }
        parser_advance(p);
        if (t->kind == TOK_STAR)
        {
            return ast.new.node(u, AST_IMPORT_ALL, tok_idx(p, t), left, 0, 0);
        }

        p->paren_depth++;
        NodeId items = parse_import_items(p, t);
        p->paren_depth--;
        if (!parser_consume(p, TOK_RPAREN, "expected ')' to close the import list"))
        {
            return 0;
        }
        return ast.new.node(u, AST_IMPORT_PICK, tok_idx(p, t), left, items, 0);
    }

    parser_error_at(p, t, "expected a member name after '.'");
    return 0;
}

// ---- dispatch table --------------------------------------------------------------------------

static const ParseRule rules[TOK__COUNT] = {
    [TOK_NUMBER] = {nud_number, NULL, PREC_NONE},
    [TOK_STRING] = {nud_string, led_string_call, PREC_CALL},
    [TOK_IDENTIFIER] = {nud_ident, NULL, PREC_NONE},
    [TOK_WRITE] = {nud_ident, NULL, PREC_NONE}, // write(...) parses as an ordinary call
    [TOK_IMPORT] = {nud_import, NULL, PREC_NONE},
    [TOK_LPAREN] = {nud_paren, led_call, PREC_CALL},
    [TOK_LBRACKET] = {NULL, led_index, PREC_CALL},
    [TOK_DOT] = {NULL, led_dot, PREC_CALL},
    [TOK_EQ] = {NULL, led_assign, PREC_ASSIGN},
    [TOK_OR] = {NULL, led_binary, PREC_OR},
    [TOK_AND] = {NULL, led_binary, PREC_AND},
    [TOK_CMP] = {NULL, led_binary, PREC_CMP},
    [TOK_NEQ] = {NULL, led_binary, PREC_CMP},
    [TOK_LT] = {NULL, led_binary, PREC_CMP},
    [TOK_GT] = {NULL, led_binary, PREC_CMP},
    [TOK_LE] = {NULL, led_binary, PREC_CMP},
    [TOK_GE] = {NULL, led_binary, PREC_CMP},
    [TOK_PLUS] = {NULL, led_binary, PREC_TERM},
    [TOK_MINUS] = {nud_unary, led_binary, PREC_TERM},
    [TOK_STAR] = {nud_unary, led_binary, PREC_FACTOR},
    [TOK_SLASH] = {NULL, led_binary, PREC_FACTOR},
    [TOK_BANG] = {nud_unary, NULL, PREC_NONE},
    [TOK_AMP] = {nud_unary, NULL, PREC_NONE},
};
`````

### `src/v2/compiler/syntax/parser/parse_stmt.c`

`````c
// parse_stmt.c (v2): blocks and statements. A statement ends at a newline, a ';' or the '}'.
#include "v2/compiler/syntax/parser/parser.h"

static int at_statement_end(const Parser *p)
{
    TokenKind k = p->current->kind;
    return k == TOK_RBRACE || k == TOK_SEMICOLON || k == TOK_EOF || (p->current->nl);
}

NodeId parse_statement(Parser *p)
{
    const Token *t = p->current;

    switch (t->kind)
    {
    case TOK_RETURN:
    {
        parser_advance(p);
        NodeId value = 0;
        if (!at_statement_end(p))
        {
            value = parse_expression(p);
            if (!value)
            {
                return 0;
            }
        }
        return ast.new.node(p->unit, AST_RETURN, tok_idx(p, t), value, 0, 0);
    }
    case TOK_LBRACE:
        return parse_block(p);
    case TOK_IDENTIFIER:
        if (t[1].kind == TOK_DEFINE)
        {
            return parse_define(p);
        }
        return parse_expression(p);
    default:
        return parse_expression(p);
    }
}

NodeId parse_block(Parser *p)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    NodeList stmts = {0};

    if (!parser_consume(p, TOK_LBRACE, "expected '{' to start a block") || !parser_enter(p))
    {
        return 0;
    }
    int saved_paren_depth = p->paren_depth; // a block inside ( ) gets newline rules back
    p->paren_depth = 0;

    while (p->current->kind != TOK_RBRACE && p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }

        NodeId stmt = parse_statement(p);
        if (p->panic)
        {
            parser_synchronize(p);
            continue;
        }
        node_list_push(u, &stmts, stmt);

        if (!at_statement_end(p))
        {
            parser_error_at(p, p->current, "expected a newline or ';' between statements");
            parser_synchronize(p);
        }
    }

    p->paren_depth = saved_paren_depth;
    parser_leave(p);
    if (!parser_consume(p, TOK_RBRACE, "expected '}' at the end of the block"))
    {
        return 0;
    }
    return ast.new.node(u, AST_BLOCK, tok_idx(p, open), stmts.head, 0, 0);
}
`````

### `src/v2/compiler/syntax/parser/parser.c`

`````c
#include "v2/compiler/syntax/parser/parser.h"
#include <stdarg.h>

void parser_init(Parser *p, Unit *u)
{
    *p = (Parser){
        .unit = u,
        .current = u->tokens.data, // the stream always holds at least the EOF sentinel
    };
}

NodeId parse_body(Parser *p, int *deferred)
{
    *deferred = p->current->kind == TOK_BODY;
    if (*deferred)
    {
        parser_advance(p); // the scanner already stepped over it: the body is this one token
        return tok_idx(p, p->previous);
    }
    return parse_block(p);
}

NodeId parse_function_body(Unit *u, uint32_t body_tok)
{
    size_t first = u->tokens.size;
    int lexical = scan_body(u->path, u->src, TOK(u, body_tok), &u->tokens);
    if (lexical)
    {
        u->errors += lexical;
        cx.errors += lexical;
        return 0;
    }

    Parser p;
    int errors_before = u->errors;
    parser_init(&p, u);
    p.current = &u->tokens.data[first];
    NodeId body = parse_block(&p);
    return u->errors == errors_before ? body : 0;
}

void parser_advance(Parser *p)
{
    p->previous = p->current;
    if (p->current->kind != TOK_EOF)
    {
        p->current++;
    }
}

int parser_match(Parser *p, TokenKind kind)
{
    if (p->current->kind == kind)
    {
        parser_advance(p);
        return 1;
    }
    return 0;
}

int parser_consume(Parser *p, TokenKind kind, const char *msg)
{
    if (p->current->kind == kind)
    {
        parser_advance(p);
        return 1;
    }
    parser_error_at(p, p->current, "%s", msg);
    return 0;
}

void parser_error_at(Parser *p, const Token *tok, const char *fmt, ...)
{
    if (p->panic)
    {
        return; // no cascades: one report until the parser has resynchronised
    }
    p->panic = 1;
    p->err_tok = tok;

    va_list ap;
    va_start(ap, fmt);
    diag_vat(p->unit, tok_idx(p, tok), "error", fmt, ap);
    va_end(ap);
}

void parser_skip_one(Parser *p)
{
    const Token *t = p->current;
    if ((t->kind == TOK_LPAREN || t->kind == TOK_LBRACKET || t->kind == TOK_LBRACE) &&
        t->match > tok_idx(p, t))
    {
        p->current = TOK(p->unit, t->match); // the scanner matched it: jump to the closer
    }
    parser_advance(p);
}

// Skips to the next place a declaration or statement can start: a new line, a separator, or
// the '}' of the enclosing body. Bracketed groups are jumped over whole.
void parser_synchronize(Parser *p)
{
    p->panic = 0;

    if (p->current == p->err_tok && p->current->kind != TOK_RBRACE)
    {
        parser_skip_one(p); // always make progress past the offending token
    }

    while (p->current->kind != TOK_EOF)
    {
        if ((p->current->nl) || p->current->kind == TOK_RBRACE)
        {
            return;
        }
        if (p->current->kind == TOK_SEMICOLON || p->current->kind == TOK_COMMA)
        {
            parser_advance(p);
            return;
        }
        parser_skip_one(p);
    }
}

int parser_enter(Parser *p)
{
    if (p->depth >= PARSE_MAX_DEPTH)
    {
        parser_error_at(p, p->current, "nesting is too deep (limit is %d levels)", PARSE_MAX_DEPTH);
        return 0;
    }
    p->depth++;
    return 1;
}

void parser_leave(Parser *p)
{
    p->depth--;
}

// Appends `id` (or a ready-made chain starting at `id`) to the list.
void node_list_push(Unit *u, NodeList *list, NodeId id)
{
    if (!id)
    {
        return;
    }
    if (list->tail)
    {
        NODE(u, list->tail)->next = id;
    }
    else
    {
        list->head = id;
    }
    while (NODE(u, id)->next)
    {
        id = NODE(u, id)->next;
    }
    list->tail = id;
}

NodeId parse_program(Parser *p)
{
    Unit *u = p->unit;
    NodeId root = ast.new.node(u, AST_MODULE, 0, 0, 0, 0);
    NodeList decls = {0};

    while (p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }
        if (p->current->kind == TOK_RBRACE)
        {
            parser_error_at(p, p->current, "unexpected '}'");
            parser_advance(p);
            p->panic = 0;
            continue;
        }

        NodeId decl = parse_declaration(p, DECL_TOP);
        if (p->panic)
        {
            parser_synchronize(p);
            continue;
        }
        node_list_push(u, &decls, decl);

        if (p->current->kind != TOK_EOF && p->current->kind != TOK_SEMICOLON && !(p->current->nl))
        {
            parser_error_at(p, p->current, "expected a newline or ';' after this declaration");
            parser_synchronize(p);
        }
    }

    NODE(u, root)->a = decls.head;
    return root;
}
`````

### `src/v2/compiler/syntax/parser/parser.h`

`````c
// parser.h (v2)
//
// Two layers over the flat token stream:
//   - declarations and statements: one- or two-token lookahead, no backtracking
//     (parse_decl.c, parse_stmt.c)
//   - expressions: Pratt loop over a NUD/LED function-pointer table (parse_expr.c)
// Tokens are never copied: current/previous point INTO unit->tokens.data.
#ifndef MODAL_V2_PARSER_H
#define MODAL_V2_PARSER_H

#include "v2/compiler/cx.h"

#define PARSE_MAX_DEPTH 200

typedef struct Parser
{
    Unit *unit;
    const Token *current;
    const Token *previous;
    const Token *err_tok; // where the last error was reported
    int panic;            // set by an error, cleared by synchronisation: one report per mistake
    int depth;            // recursion guard
    int paren_depth;      // > 0 inside ( ) or [ ]: newlines do not end the expression there
} Parser;

typedef struct
{
    NodeId head, tail;
} NodeList;

typedef enum
{
    DECL_TOP,
    DECL_STRUCT,
} DeclContext;

// parser.c
void parser_init(Parser *p, Unit *u);
void parser_advance(Parser *p);
int parser_match(Parser *p, TokenKind kind);
int parser_consume(Parser *p, TokenKind kind, const char *msg);
void parser_error_at(Parser *p, const Token *tok, const char *fmt, ...);
void parser_synchronize(Parser *p);
void parser_skip_one(Parser *p); // one token, or a whole ( ) [ ] { } group
int parser_enter(Parser *p);     // returns 0 (and reports) when nesting is too deep
void parser_leave(Parser *p);
void node_list_push(Unit *u, NodeList *list, NodeId id);
NodeId parse_program(Parser *p);
// Is this token where a function body starts: a '{', or a body the scanner deferred?
static inline int is_body_start(const Token *t)
{
    return t->kind == TOK_LBRACE || t->kind == TOK_BODY;
}
// Parses the function body at the current token. If the scanner deferred it (TOK_BODY), it
// is one token: it is consumed, *deferred is set and its token index returned.
NodeId parse_body(Parser *p, int *deferred);
// Tokenizes and parses a deferred body into the tails of the unit's token and node arrays.
// `body_tok` is the token index of its TOK_BODY. Returns the block, or 0 if the body has
// errors (already reported). The caller cuts both tails off when it is done with the body.
NodeId parse_function_body(Unit *u, uint32_t body_tok);

static inline uint32_t tok_idx(const Parser *p, const Token *t)
{
    return (uint32_t)(t - p->unit->tokens.data);
}

// parse_decl.c
NodeId parse_declaration(Parser *p, DeclContext ctx);
NodeId parse_define(Parser *p);
NodeId parse_type(Parser *p);
int type_starts_at(const Token *t);
NodeId parse_params(Parser *p, int *has_self);
NodeId parse_import_items(Parser *p, const Token *open);

// parse_stmt.c
NodeId parse_statement(Parser *p);
NodeId parse_block(Parser *p);

// parse_expr.c
NodeId parse_expression(Parser *p);

#endif // MODAL_V2_PARSER_H
`````

### `src/v2/compiler/syntax/scanner.c`

`````c
#include "v2/compiler/syntax/scanner.h"
#include "v2/compiler/cx.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_MAX_NESTING 256

// Keywords grouped by length: a lookup only compares against the few of the same length.
static const Keyword keywords[] = {
    {"as", 2, TOK_AS},
    {"or", 2, TOK_OR},
    {"fun", 3, TOK_FUNCTION},
    {"use", 3, TOK_USE},
    {"asm", 3, TOK_ASM},
    {"and", 3, TOK_AND},
    {"val", 3, TOK_VAL},
    {"test", 4, TOK_TEST},
    {"defer", 5, TOK_DEFER},
    {"alias", 5, TOK_ALIAS},
    {"union", 5, TOK_UNION},
    {"async", 5, TOK_ASYNC},
    {"await", 5, TOK_AWAIT},
    {"write", 5, TOK_WRITE},
    {"const", 5, TOK_CONST},
    {"assert", 6, TOK_ASSERT},
    {"sizeof", 6, TOK_SIZEOF},
    {"struct", 6, TOK_STRUCT},
    {"import", 6, TOK_IMPORT},
    {"return", 6, TOK_RETURN},
    {"autofree", 8, TOK_AUTOFREE},
    {"comptime", 8, TOK_COMPTIME},
    {"volatile", 8, TOK_VOLATILE},
};

// keyword_first[n] .. keyword_first[n + 1]: the keywords of length n.
static const uint8_t keyword_first[10] = {0, 0, 0, 2, 7, 8, 15, 20, 20, 23};

static inline TokenKind keyword_kind(const char *s, size_t len)
{
    if (len < 2 || len > 8)
    {
        return TOK_IDENTIFIER;
    }
    for (unsigned i = keyword_first[len]; i < keyword_first[len + 1]; i++)
    {
        if (keywords[i].kw[0] == s[0] && memcmp(s, keywords[i].kw, len) == 0)
        {
            return keywords[i].kind;
        }
    }
    return TOK_IDENTIFIER;
}

// One table lookup classifies a byte, instead of a chain of range comparisons.
enum
{
    C_IDENT_START = 1,
    C_IDENT = 2, // letters, digits, underscore
    C_DIGIT = 4,
    C_BLANK = 8, // space, tab, carriage return
};

static uint8_t char_class[256];

static void char_class_init(void)
{
    for (int c = 'a'; c <= 'z'; c++)
    {
        char_class[c] = C_IDENT_START | C_IDENT;
        char_class[c - 'a' + 'A'] = C_IDENT_START | C_IDENT;
    }
    for (int c = '0'; c <= '9'; c++)
    {
        char_class[c] = C_IDENT | C_DIGIT;
    }
    char_class['_'] = C_IDENT_START | C_IDENT;
    char_class[' '] = char_class['\t'] = char_class['\r'] = C_BLANK;
}

#define CLASS(ch) (char_class[(unsigned char)(ch)])

void source_pos(const char *src, const char *at, int *line, int *col)
{
    // Diagnostics mostly arrive in source order, so counting resumes from the last answer
    // instead of from the top of the file each time.
    static const char *last_src, *last_at, *last_line_start;
    static int last_line;

    const char *c = src;
    const char *line_start = src;
    *line = 1;
    if (last_src == src && last_at <= at)
    {
        c = last_at;
        line_start = last_line_start;
        *line = last_line;
    }
    for (; c < at; c++)
    {
        if (*c == '\n')
        {
            (*line)++;
            line_start = c + 1;
        }
    }
    *col = (int)(at - line_start) + 1;

    last_src = src;
    last_at = at;
    last_line_start = line_start;
    last_line = *line;
}

// Errors are rare, so the scanner does not track lines: the position is computed here.
static void scan_error(const char *filename, const char *src, const char *at, const char *fmt, ...)
{
    int line, col;
    va_list ap;
    source_pos(src, at, &line, &col);
    va_start(ap, fmt);
    fprintf(stderr, "\nerror [%s:%d:%d]: ", filename, line, col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static TokenKind opener_of(TokenKind closer)
{
    switch (closer)
    {
    case TOK_RPAREN:
        return TOK_LPAREN;
    case TOK_RBRACKET:
        return TOK_LBRACKET;
    default:
        return TOK_LBRACE;
    }
}

// Finds the '}' that closes the '{' at `open`, reading the body as raw text: only strings,
// comments and braces matter. Returns a pointer to that '}', or NULL if the file ends first.
//
// *tokens receives an upper bound on the number of tokens in the body, counted on the way:
// one per run of identifier characters, one per other visible byte, one per string. That is
// what lets the caller reserve room for the body without tokenizing it.
static const char *skip_body(const char *open, size_t *tokens)
{
    const char *c = open + 1;
    int depth = 1;
    int in_word = 0;
    size_t n = 1; // the '{' itself

    for (;;)
    {
        uint8_t cls = CLASS(*c);
        if (cls & C_IDENT)
        {
            n += !in_word;
            in_word = 1;
            c++;
            continue;
        }
        in_word = 0;

        switch (*c)
        {
        case '\0':
            return NULL;
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            c++;
            break;
        case '"':
            n++;
            c++;
            while (*c && *c != '"')
            {
                if (*c == '\\' && c[1])
                {
                    c++;
                }
                c++;
            }
            if (!*c)
            {
                return NULL;
            }
            c++;
            break;
        case '-':
            if (c[1] == '-')
            {
                while (*c && *c != '\n')
                {
                    c++;
                }
            }
            else if (c[1] == '{')
            {
                c += 2;
                while (*c && !(c[0] == '}' && c[1] == '-'))
                {
                    c++;
                }
                if (!*c)
                {
                    return NULL;
                }
                c += 2;
            }
            else
            {
                n++;
                c++;
            }
            break;
        case '{':
            n++;
            depth++;
            c++;
            break;
        case '}':
            n++;
            if (--depth == 0)
            {
                *tokens = n;
                return c;
            }
            c++;
            break;
        default:
            n++;
            c++;
            break;
        }
    }
}

// The scanning loop, shared by whole files and single bodies. It appends to `v` starting at
// v->size and stops at the end of the text, or at `end` when one is given. With `defer`,
// function bodies become TOK_BODY tokens.
static int scan_into(const char *filename, const char *src, const char *from, const char *end,
                     TokenVec *v, int defer, size_t *max_body)
{
    size_t cap = v->cap;
    size_t count = v->size;
    Token *toks = v->data;
    uint32_t open_stack[SCAN_MAX_NESTING];
    int sp = 0;
    int errors = 0;
    int too_deep = 0;

    const char *c = from;
    uint32_t nl = 1;

    if (!char_class['a'])
    {
        char_class_init();
    }

    for (;;)
    {
        // Whitespace and comments: `-- line` and `-{ block }-`.
        for (;;)
        {
            if (CLASS(*c) & C_BLANK)
            {
                c++;
            }
            else if (*c == '\n')
            {
                c++;
                nl = 1;
            }
            else if (c[0] == '-' && c[1] == '-')
            {
                while (*c && *c != '\n')
                {
                    c++;
                }
            }
            else if (c[0] == '-' && c[1] == '{')
            {
                const char *open = c;
                c += 2;
                while (*c && !(c[0] == '}' && c[1] == '-'))
                {
                    nl |= *c == '\n';
                    c++;
                }
                if (!*c)
                {
                    scan_error(filename, src, open, "unterminated block comment");
                    errors++;
                }
                else
                {
                    c += 2;
                }
            }
            else
            {
                break;
            }
        }

        const char *start = c;
        TokenKind kind = TOK_UNKNOWN;
        uint32_t match = 0;

        if (count + 1 >= cap || !toks)
        {
            cap += cap / 2 + 16;
            toks = (Token *)realloc(toks, cap * sizeof(Token));
            if (!toks)
            {
                fprintf(stderr, "fatal: out of memory while scanning %s\n", filename);
                exit(2);
            }
        }

        if (!*c || (end && c >= end))
        {
            toks[count++] = (Token){.start = start, .kind = TOK_EOF, .nl = 1};
            break;
        }

        uint8_t cls = CLASS(*c);
        if (cls & C_IDENT_START)
        {
            do
            {
                c++;
            } while (CLASS(*c) & C_IDENT);
            kind = keyword_kind(start, (size_t)(c - start));
        }
        else if (cls & C_DIGIT)
        {
            do
            {
                c++;
            } while (CLASS(*c) & C_DIGIT);
            if (c[0] == '.' && (CLASS(c[1]) & C_DIGIT))
            {
                do
                {
                    c++;
                } while (CLASS(*c) & C_DIGIT);
            }
            kind = TOK_NUMBER;
        }
        else if (*c == '"')
        {
            c++;
            while (*c && *c != '"')
            {
                if (*c == '\\' && c[1])
                {
                    c++;
                }
                c++;
            }
            if (*c == '"')
            {
                c++;
            }
            else
            {
                scan_error(filename, src, start, "unterminated string literal");
                errors++;
            }
            kind = TOK_STRING;
        }
        else
        {
            char ch = *c++;
            switch (ch)
            {
            case '(':
                kind = TOK_LPAREN;
                break;
            case ')':
                kind = TOK_RPAREN;
                break;
            case '{':
                kind = TOK_LBRACE;
                // Every '{' at declaration level opens a function body, except the one
                // after `struct`. A body is not tokenized now: it is one token.
                if (defer && !(count > v->size && toks[count - 1].kind == TOK_STRUCT))
                {
                    size_t body_tokens = 0;
                    const char *close = skip_body(start, &body_tokens);
                    if (close && (size_t)(close - start) + 1 < (1u << 24))
                    {
                        c = close + 1;
                        kind = TOK_BODY;
                        if (max_body && body_tokens > *max_body)
                        {
                            *max_body = body_tokens;
                        }
                    }
                }
                break;
            case '}':
                kind = TOK_RBRACE;
                break;
            case '[':
                kind = TOK_LBRACKET;
                break;
            case ']':
                kind = TOK_RBRACKET;
                break;
            case ',':
                kind = TOK_COMMA;
                break;
            case ';':
                kind = TOK_SEMICOLON;
                break;
            case '+':
                kind = TOK_PLUS;
                break;
            case '*':
                kind = TOK_STAR;
                break;
            case '/':
                kind = TOK_SLASH;
                break;
            case '&':
                kind = TOK_AMP;
                break;
            case '|':
                kind = TOK_PIPE;
                break;
            case '-':
                kind = TOK_MINUS;
                if (*c == '>')
                {
                    c++;
                    kind = TOK_ARROW;
                }
                break;
            case '=':
                kind = TOK_EQ;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_CMP;
                }
                break;
            case '!':
                kind = TOK_BANG;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_NEQ;
                }
                break;
            case '<':
                kind = TOK_LT;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_LE;
                }
                break;
            case '>':
                kind = TOK_GT;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_GE;
                }
                break;
            case ':':
                kind = TOK_COLON;
                if (*c == ':')
                {
                    c++;
                    kind = TOK_DCOLON;
                }
                else if (*c == '=')
                {
                    c++;
                    kind = TOK_DEFINE;
                }
                break;
            case '.':
                kind = TOK_DOT;
                if (c[0] == '.' && c[1] == '.')
                {
                    c += 2;
                    kind = TOK_ELLIPSIS;
                }
                else if (c[0] == '.')
                {
                    c++;
                    kind = TOK_DOTDOT;
                }
                break;
            case '?':
                kind = TOK_QUESTION;
                if (c[0] == '?' && c[1] == '=')
                {
                    c += 2;
                    kind = TOK_QQ_EQ;
                }
                else if (c[0] == '?')
                {
                    c++;
                    kind = TOK_QQ;
                }
                else if (c[0] == '.')
                {
                    c++;
                    kind = TOK_Q_DOT;
                }
                break;
            default:
                scan_error(filename, src, start, "unexpected character '%c'", ch);
                errors++;
                break;
            }

            // Bracket matching happens here, once, so the parser can jump over any group in
            // O(1) (function-literal detection, error recovery).
            if (kind == TOK_LPAREN || kind == TOK_LBRACKET || kind == TOK_LBRACE)
            {
                if (sp < SCAN_MAX_NESTING)
                {
                    open_stack[sp++] = (uint32_t)count;
                }
                else if (!too_deep)
                {
                    scan_error(filename, src, start,
                               "brackets are nested too deeply (limit is %d levels)",
                               SCAN_MAX_NESTING);
                    errors++;
                    too_deep = 1; // say it once; the unmatched closers would only repeat it
                }
            }
            else if (kind == TOK_RPAREN || kind == TOK_RBRACKET || kind == TOK_RBRACE)
            {
                if (sp > 0 && toks[open_stack[sp - 1]].kind == (uint32_t)opener_of(kind))
                {
                    match = open_stack[--sp];
                    toks[match].match = (uint32_t)count;
                }
                else if (!too_deep)
                {
                    scan_error(filename, src, start, "unbalanced '%c'", *start);
                    errors++;
                }
            }
        }

        toks[count++] = (Token){
            .start = start,
            .len = (uint32_t)(c - start),
            .kind = kind,
            .match = match,
            .nl = nl,
        };
        nl = 0;
    }

    while (sp > 0 && !too_deep)
    {
        const Token *open = &toks[open_stack[--sp]];
        scan_error(filename, src, open->start, "'%c' is never closed", *open->start);
        errors++;
    }

    v->data = toks;
    v->size = count;
    v->cap = cap;
    return errors;
}

int scan_all(const char *filename, const char *src, size_t len, int eager, TokenVec *out,
             size_t *max_body)
{
    // A token per four bytes of source is typical; the array grows by half when that was
    // too low and is trimmed to size by the caller once it knows how much room it wants.
    out->cap = len / (eager ? 4 : 8) + 16;
    out->size = 0;
    out->data = (Token *)malloc(out->cap * sizeof(Token));
    *max_body = 0;
    return scan_into(filename, src, src, NULL, out, !eager, max_body);
}

int scan_body(const char *filename, const char *src, const Token *body, TokenVec *out)
{
    return scan_into(filename, src, body->start, body->start + body->len, out, 0, NULL);
}

const char *token_kind_name(TokenKind kind)
{
    switch (kind)
    {
    case TOK_EOF:
        return "end of file";
    case TOK_LPAREN:
        return "'('";
    case TOK_RPAREN:
        return "')'";
    case TOK_LBRACE:
    case TOK_BODY:
        return "'{'";
    case TOK_RBRACE:
        return "'}'";
    case TOK_LBRACKET:
        return "'['";
    case TOK_RBRACKET:
        return "']'";
    case TOK_COLON:
        return "':'";
    case TOK_COMMA:
        return "','";
    case TOK_SEMICOLON:
        return "';'";
    case TOK_DEFINE:
        return "':='";
    case TOK_EQ:
        return "'='";
    case TOK_DOT:
        return "'.'";
    case TOK_IDENTIFIER:
        return "identifier";
    case TOK_NUMBER:
        return "number";
    case TOK_STRING:
        return "string";
    default:
        return "token";
    }
}
`````

### `src/v2/compiler/syntax/scanner.h`

`````c
// scanner.h (v2): flat, zero-copy token stream.
//
// Tokens are slices into the source buffer (start + len). The whole file is scanned once into
// one contiguous array that ends with a TOK_EOF sentinel, so the parser walks it
// with a plain pointer and can look ahead with tok[1] at no cost.
#ifndef MODAL_V2_SCANNER_H
#define MODAL_V2_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#define MODAL_VERSION "0.0.3"

typedef enum
{
    TOK_EOF,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COLON,
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_OPERATOR,
    TOK_IDENTIFIER,
    TOK_UNKNOWN,
    TOK_NUMBER,
    TOK_STRING,
    TOK_PREPROC,
    // DECLARATION
    TOK_CONST,
    // Keywords
    TOK_FUNCTION,
    TOK_TEST,
    TOK_ASSERT,
    TOK_SIZEOF,
    TOK_DEFER,
    TOK_AUTOFREE,
    TOK_ALIAS,
    TOK_USE,
    TOK_COMPTIME,
    TOK_UNION,
    TOK_ASM,
    TOK_VOLATILE,
    TOK_ASYNC,
    TOK_AWAIT,
    TOK_AND,
    TOK_OR,
    TOK_WRITE,
    // Multi-char operators
    TOK_Q_DOT,
    TOK_EQ,
    TOK_CMP,
    TOK_DEFINE,
    TOK_QQ_EQ,
    TOK_QQ,
    TOK_QUESTION,
    TOK_PIPE,
    TOK_DCOLON,
    TOK_ELLIPSIS,
    TOK_DOTDOT,
    TOK_ARROW,
    // v2: operators that used to be lumped into TOK_OPERATOR
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_DOT,
    TOK_STAR,
    TOK_PLUS,
    TOK_MINUS,
    TOK_SLASH,
    TOK_AMP,
    TOK_BANG,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    // v2: keywords
    TOK_STRUCT,
    TOK_IMPORT,
    TOK_AS,
    TOK_VAL,
    TOK_RETURN,
    // A whole function body, `{ ... }`, as ONE token: `start` is its '{' and `len` covers it
    // to its '}'. The scanner produces it in place of the body's own tokens, which are made
    // only when that function is compiled (see scan_body).
    TOK_BODY,
    TOK__COUNT,
} TokenKind;

// 16 bytes. A token is a slice of the source buffer plus what the parser needs to move fast.
// Line and column are not stored: they are only needed for a diagnostic, and are recomputed
// from `start` at that moment (see source_pos).
typedef struct
{
    const char *start;   // into the source buffer, never copied
    uint32_t len : 24;   // a token is at most 16 MB long
    uint32_t kind : 8;   // TokenKind
    uint32_t match : 31; // for ( [ { and their closers: index of the partner, 0 if unbalanced
    uint32_t nl : 1;     // first token on its line: this is how statements end without ';'
} Token;

_Static_assert(sizeof(Token) == 16, "Token must stay 16 bytes");

typedef struct
{
    Token *data;
    size_t size;
    size_t cap;
} TokenVec;

typedef struct
{
    const char *kw;
    size_t len;
    TokenKind kind;
} Keyword;

// Scans `src` (NUL-terminated, `len` bytes) into `out` in one pass.
//
// Unless `eager`, a function body is not tokenized: it is skipped as raw text and becomes a
// single TOK_BODY token. The token stream of a file is then only its declarations, and
// `*max_body` receives an upper bound on the token count of its largest body.
//
// The token array is malloc'd (it grows while scanning, which an arena cannot do without
// leaving old copies behind) and freed by the driver.
// Returns the number of lexical errors (already reported to stderr).
int scan_all(const char *filename, const char *src, size_t len, int eager, TokenVec *out,
             size_t *max_body);

// Tokenizes one deferred body: appends its tokens, '{' through '}' and then an EOF, to the
// end of `out`, which must have room for them (see `max_body` above).
// Returns the number of lexical errors in the body.
int scan_body(const char *filename, const char *src, const Token *body, TokenVec *out);

// Line and column (1-based) of a position in `src`.
void source_pos(const char *src, const char *at, int *line, int *col);

const char *token_kind_name(TokenKind kind);

#endif // MODAL_V2_SCANNER_H
`````

### `src/v2/compiler/ast/ast.c`

`````c
#include "v2/compiler/ast/ast.h"
#include "v2/compiler/ast/visitor.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t ast_child_mask[AST__COUNT] = {
    [AST_MODULE] = 1,     [AST_STRUCT_DECL] = 1, [AST_FIELD_DECL] = 3, [AST_METHOD_DECL] = 7,
    [AST_PARAM] = 1,      [AST_DEFINE] = 1,      [AST_VAL_DECL] = 3,   [AST_TYPE_SLICE] = 1,
    [AST_TYPE_PTR] = 1,   [AST_TYPE_STRUCT] = 1, [AST_FN_LIT] = 7,     [AST_BLOCK] = 1,
    [AST_RETURN] = 1,     [AST_ASSIGN] = 3,      [AST_BIN_OP] = 3,     [AST_UNARY_OP] = 1,
    [AST_CALL] = 3,       [AST_INDEX] = 3,       [AST_SELECTOR] = 1,   [AST_PAREN_GROUP] = 1,
    [AST_IMPORT_ALL] = 1, [AST_IMPORT_PICK] = 3,
};

static NodeId ast_new_node(Unit *u, AstNodeKind kind, uint32_t tok, NodeId a, NodeId b, NodeId c)
{
    AstNode n = {.kind = (uint8_t)kind, .tok = tok, .a = a, .b = b, .c = c};
    if (u->nodes.size == u->nodes.cap)
    {
        size_t cap = u->nodes.cap * 2 + 64;
        AstNode *grown = (AstNode *)realloc(u->nodes.data, cap * sizeof(AstNode));
        if (!grown)
        {
            fprintf(stderr, "fatal: out of memory while building the AST\n");
            exit(2);
        }
        heap_account(u->nodes.cap * sizeof(AstNode), cap * sizeof(AstNode));
        u->nodes.data = grown;
        u->nodes.cap = cap;
    }
    u->nodes.data[u->nodes.size++] = n;
    return (NodeId)(u->nodes.size - 1);
}

static const char *ast_kind_name(AstNodeKind kind)
{
    static const char *const names[AST__COUNT] = {
        [AST_NONE] = "none",
        [AST_MODULE] = "module",
        [AST_STRUCT_DECL] = "struct",
        [AST_FIELD_DECL] = "field",
        [AST_METHOD_DECL] = "method",
        [AST_PARAM] = "param",
        [AST_DEFINE] = "define",
        [AST_VAL_DECL] = "val",
        [AST_TYPE_NAME] = "type",
        [AST_TYPE_SLICE] = "type-slice",
        [AST_TYPE_PTR] = "type-ptr",
        [AST_TYPE_STRUCT] = "type-struct",
        [AST_FN_LIT] = "fn",
        [AST_BLOCK] = "block",
        [AST_RETURN] = "return",
        [AST_ASSIGN] = "assign",
        [AST_BIN_OP] = "binop",
        [AST_UNARY_OP] = "unary",
        [AST_CALL] = "call",
        [AST_INDEX] = "index",
        [AST_SELECTOR] = "select",
        [AST_PAREN_GROUP] = "group",
        [AST_IDENT] = "ident",
        [AST_NUMBER_LIT] = "number",
        [AST_STRING_LIT] = "string",
        [AST_IMPORT] = "import",
        [AST_IMPORT_ALL] = "import-all",
        [AST_IMPORT_PICK] = "import-pick",
        [AST_IMPORT_ITEM] = "item",
    };
    return kind < AST__COUNT && names[kind] ? names[kind] : "?";
}

NodeId ast_import_root(const Unit *u, NodeId expr)
{
    while (expr)
    {
        const AstNode *n = NODE(u, expr);
        switch (n->kind)
        {
        case AST_IMPORT:
            return expr;
        case AST_SELECTOR:
        case AST_IMPORT_ALL:
        case AST_IMPORT_PICK:
            expr = n->a;
            break;
        default:
            return 0;
        }
    }
    return 0;
}

// ---- text rendering ------------------------------------------------------------------------

typedef struct
{
    char *buf;
    size_t cap;
    size_t len;
} Str;

static void str_put(Str *s, const char *text, size_t n)
{
    if (s->len + 1 >= s->cap)
    {
        return;
    }
    size_t room = s->cap - 1 - s->len;
    if (n > room)
    {
        n = room;
    }
    memcpy(s->buf + s->len, text, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void str_puts(Str *s, const char *text)
{
    str_put(s, text, strlen(text));
}

static void str_type(Str *s, const Unit *u, NodeId type)
{
    while (type)
    {
        const AstNode *n = NODE(u, type);
        switch (n->kind)
        {
        case AST_TYPE_SLICE:
            str_puts(s, "[]");
            type = n->a;
            continue;
        case AST_TYPE_PTR:
            str_puts(s, "*");
            type = n->a;
            continue;
        case AST_TYPE_STRUCT:
            str_puts(s, "struct {...}");
            return;
        case AST_TYPE_NAME:
        {
            const Token *first = TOK(u, n->tok);
            const Token *last = TOK(u, n->tok + 2u * n->aux);
            str_put(s, first->start, (size_t)((last->start + last->len) - first->start));
            return;
        }
        default:
            str_puts(s, "?");
            return;
        }
    }
}

static char *ast_type_str(const Unit *u, NodeId type, char *buf, size_t cap)
{
    Str s = {buf, cap, 0};
    buf[0] = '\0';
    str_type(&s, u, type);
    return buf;
}

static void str_params(Str *s, const Unit *u, NodeId param)
{
    str_puts(s, "(");
    for (int first = 1; param; param = NODE(u, param)->next, first = 0)
    {
        const AstNode *n = NODE(u, param);
        const Token *name = TOK(u, n->tok);
        if (!first)
        {
            str_puts(s, ", ");
        }
        str_put(s, name->start, (size_t)name->len);
        if (n->a)
        {
            str_puts(s, ": ");
            str_type(s, u, n->a);
        }
    }
    str_puts(s, ")");
}

// `name(params): T`, or `T name(params)` for the type-first (overloadable) form.
static char *ast_sig_str(const Unit *u, NodeId decl, char *buf, size_t cap)
{
    Str s = {buf, cap, 0};
    const AstNode *n = NODE(u, decl);
    const Token *name = TOK(u, n->tok);
    const AstNode *fn = n->kind == AST_DEFINE ? NODE(u, n->a) : n;
    int type_first = (n->flags & AST_F_MUT) != 0;

    buf[0] = '\0';
    if (type_first)
    {
        str_type(&s, u, fn->b);
        str_puts(&s, " ");
    }
    str_put(&s, name->start, (size_t)name->len);
    str_params(&s, u, fn->a);
    if (!type_first && fn->b)
    {
        str_puts(&s, ": ");
        str_type(&s, u, fn->b);
    }
    return buf;
}

// ---- printer: one visitor over the iterative walker ------------------------------------------

static int print_visit(Unit *u, NodeId id, uint32_t depth, void *ctx)
{
    (void)ctx;
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    char text[256];
    Str s = {text, sizeof(text), 0};
    int skip = VISIT_CONTINUE;
    text[0] = '\0';

    printf("%*s%s", (int)depth * 2, "", ast_kind_name((AstNodeKind)n->kind));

    switch (n->kind)
    {
    case AST_MODULE:
        printf(" %.*s", u->name_len, u->name);
        break;
    case AST_FIELD_DECL:
    case AST_PARAM:
        str_type(&s, u, n->a);
        printf(" %.*s: %s", t->len, t->start, text);
        // An inline struct type keeps its members as children; plain types are fully shown.
        if (!n->a || NODE(u, n->a)->kind != AST_TYPE_STRUCT)
        {
            skip = VISIT_SKIP_A;
        }
        break;
    case AST_METHOD_DECL:
        str_params(&s, u, n->a);
        if (n->b)
        {
            str_puts(&s, ": ");
            str_type(&s, u, n->b);
        }
        printf(" %.*s%s", t->len, t->start, text);
        skip = VISIT_SKIP_A | VISIT_SKIP_B;
        break;
    case AST_FN_LIT:
        str_params(&s, u, n->a);
        if (n->b)
        {
            str_puts(&s, " ");
            str_type(&s, u, n->b);
        }
        printf(" %s", text);
        skip = VISIT_SKIP_A | VISIT_SKIP_B;
        break;
    case AST_TYPE_NAME:
    case AST_TYPE_SLICE:
    case AST_TYPE_PTR:
        str_type(&s, u, id);
        printf(" %s", text);
        skip = VISIT_SKIP;
        break;
    case AST_IMPORT_ITEM:
        printf(" %.*s", t->len, t->start);
        if (n->c)
        {
            const Token *alias = TOK(u, n->c);
            printf(" as %.*s", alias->len, alias->start);
        }
        break;
    case AST_IMPORT:
        printf(" %.*s", t->len, t->start);
        if (n->aux)
        {
            printf("  -> %s", cx.units[n->aux - 1].path);
        }
        break;
    case AST_STRUCT_DECL:
    case AST_DEFINE:
    case AST_IDENT:
    case AST_NUMBER_LIT:
    case AST_STRING_LIT:
    case AST_BIN_OP:
    case AST_UNARY_OP:
        printf(" %.*s", t->len, t->start);
        break;
    case AST_SELECTOR:
        printf(" .%.*s", t->len, t->start);
        break;
    default:
        break;
    }

    if (n->flags & AST_F_MUT)
    {
        printf("  [mut]");
    }
    if (n->flags & AST_F_ONELINE)
    {
        printf("  [oneline]");
    }
    if (n->flags & AST_F_SELF)
    {
        printf("  [self]");
    }
    if (n->flags & AST_F_LAZY)
    {
        printf("  [body not parsed]");
    }
    putchar('\n');
    return skip;
}

static void ast_print(Unit *u)
{
    walk_ast_iterative(u, u->root, print_visit, NULL);
}

const AstImpl ast = {
    .new = {.node = ast_new_node},
    .print = ast_print,
    .kind_name = ast_kind_name,
    .type_str = ast_type_str,
    .sig_str = ast_sig_str,
};
`````

### `src/v2/compiler/ast/ast.h`

`````c
// ast.h (v2): compact, index-linked AST.
//
// A node is 24 bytes and lives in one flat array per file. Children are referenced by index
// (NodeId), never by pointer, so the array can sit in the arena and the tree stays valid no
// matter where it is stored. Lists (statements, members, params, args) are chained through
// `next`. Source text is reached through `tok`, an index into the unit's token stream.
#ifndef MODAL_V2_AST_H
#define MODAL_V2_AST_H

#include <stddef.h>
#include <stdint.h>

typedef struct Unit Unit;
typedef uint32_t NodeId; // 0 = no node

typedef enum
{
    AST_NONE,
    //                  tok          a            b            c
    AST_MODULE,      // -            decls
    AST_STRUCT_DECL, // name         members
    AST_FIELD_DECL,  // name         type         init
    AST_METHOD_DECL, // name         params       return type  body
    AST_PARAM,       // name         type
    AST_DEFINE,      // name         value                               name := value
    AST_VAL_DECL,    // 'val'        items        source                 val (a as b): import(x)
    AST_TYPE_NAME,   // first ident  (aux = number of extra `.segment`s)
    AST_TYPE_SLICE,  // '['          element
    AST_TYPE_PTR,    // '*'          pointee
    AST_TYPE_STRUCT, // 'struct'     members
    AST_FN_LIT,      // '('          params       return type  body
    AST_BLOCK,       // '{'          statements
    AST_RETURN,      // 'return'     value
    AST_ASSIGN,      // '='          target       value
    AST_BIN_OP,      // operator     left         right
    AST_UNARY_OP,    // operator     operand
    AST_CALL,        // '(' / string callee       args
    AST_INDEX,       // '['          target       index
    AST_SELECTOR,    // member name  target
    AST_PAREN_GROUP, // '('          expr
    AST_IDENT,       // name
    AST_NUMBER_LIT,  // literal
    AST_STRING_LIT,  // literal
    AST_IMPORT,      // module arg   (aux = target unit id + 1, 0 = external; set by the linker)
    AST_IMPORT_ALL,  // '*'          chain                               import(x).*
    AST_IMPORT_PICK, // '('          chain        items                  import(x).(a, b as c)
    AST_IMPORT_ITEM, // name                                   alias TOKEN index (0 = none)
    AST__COUNT,
} AstNodeKind;

enum
{
    AST_F_MUT = 1u << 0,     // type-first declaration: mutable field / overloadable method
    AST_F_ONELINE = 1u << 1, // method body is `: expression`
    AST_F_SELF = 1u << 2,    // callable whose first parameter is `self`
    // The body has not been parsed yet: `c` holds the TOKEN index of its '{', not a node.
    // It is parsed, checked, emitted and discarded one function at a time (see check.c).
    AST_F_LAZY = 1u << 3,
};

typedef struct AstNode
{
    uint8_t kind;
    uint8_t flags;
    uint16_t aux;
    uint32_t tok;
    NodeId a, b, c;
    NodeId next; // next sibling in the list this node belongs to
} AstNode;

_Static_assert(sizeof(AstNode) == 24, "AstNode must stay 24 bytes");

typedef struct
{
    AstNode *data;
    size_t size;
    size_t cap;
} NodeVec;

#define NODE(u, id) (&(u)->nodes.data[(id)])
#define TOK(u, i) (&(u)->tokens.data[(i)])

// Which of a/b/c hold child nodes, per kind (bit 0 = a, bit 1 = b, bit 2 = c).
extern const uint8_t ast_child_mask[AST__COUNT];

typedef struct
{
    NodeId (*node)(Unit *u, AstNodeKind kind, uint32_t tok, NodeId a, NodeId b, NodeId c);
} AstConstructor;

typedef struct
{
    AstConstructor new;
    void (*print)(Unit *u);
    const char *(*kind_name)(AstNodeKind kind);
    // Renders a type node as source-like text into buf; returns buf.
    char *(*type_str)(const Unit *u, NodeId type, char *buf, size_t cap);
    // Renders the signature of a method declaration or function binding; returns buf.
    char *(*sig_str)(const Unit *u, NodeId decl, char *buf, size_t cap);
} AstImpl;

extern const AstImpl ast;

// If `expr` is an import chain (import(x), import(x).a.b, import(x).*, import(x).(a, b)),
// returns its AST_IMPORT node; otherwise 0.
NodeId ast_import_root(const Unit *u, NodeId expr);

static inline int is_self_token(const char *s, int len)
{
    return len == 4 && s[0] == 's' && s[1] == 'e' && s[2] == 'l' && s[3] == 'f';
}

#endif // MODAL_V2_AST_H
`````

### `src/v2/compiler/ast/symbol.c`

`````c
#include "v2/compiler/ast/symbol.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <string.h>

static inline uint32_t symbol_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < len; i++)
    {
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    }
    return h;
}

Scope *scope_new(Scope *parent, SymId owner, uint32_t nbuckets)
{
    Scope *s = (Scope *)arena_alloc(sizeof(Scope) + nbuckets * sizeof(SymId));
    s->parent = parent;
    s->owner = owner;
    s->nbuckets = nbuckets;
    if (cx.nscopes == cx.scopes_cap)
    {
        size_t cap = cx.scopes_cap;
        cx.scopes = (Scope **)arena_grow(cx.scopes, sizeof(Scope *), &cap);
        cx.scopes_cap = (uint32_t)cap;
    }
    s->id = cx.nscopes;
    cx.scopes[cx.nscopes++] = s;
    memset(s->buckets, 0, nbuckets * sizeof(SymId));
    return s;
}

// A 24-byte Symbol has no room for a pointer, so it refers to its member scope by index.
uint32_t scope_ref(const Scope *s)
{
    return s->id;
}

Scope *scope_at(uint32_t ref)
{
    return cx.scopes[ref];
}

SymId symbol_new(Unit *u, SymKind kind, uint8_t flags, uint32_t tok, SymId owner, NodeId decl)
{
    Symbol s = {
        .tok = tok,
        .kind = (uint8_t)kind,
        .flags = flags,
        .unit = u->id,
        .owner = owner,
        .decl = decl,
    };
    if (cx.syms.size == cx.syms.cap)
    {
        cx.syms.data = (Symbol *)arena_grow(cx.syms.data, sizeof(Symbol), &cx.syms.cap);
    }
    cx.syms.data[cx.syms.size++] = s;
    return (SymId)(cx.syms.size - 1);
}

const char *symbol_name(SymId id, int *len)
{
    const Symbol *s = SYM(id);
    const Token *t = TOK(&cx.units[s->unit], s->tok);
    *len = t->len;
    return t->start;
}

void scope_insert(Scope *s, SymId id)
{
    int len;
    const char *name = symbol_name(id, &len);
    uint32_t b = symbol_hash(name, (size_t)len) % s->nbuckets;
    SYM(id)->next = s->buckets[b];
    s->buckets[b] = id;
}

SymId scope_find(const Scope *s, const char *name, size_t len)
{
    for (SymId id = s->buckets[symbol_hash(name, len) % s->nbuckets]; id; id = SYM(id)->next)
    {
        int n;
        const char *other = symbol_name(id, &n);
        if ((size_t)n == len && memcmp(other, name, len) == 0)
        {
            return id;
        }
    }
    return 0;
}

SymId scope_lookup(const Scope *s, const char *name, size_t len)
{
    for (; s; s = s->parent)
    {
        SymId id = scope_find(s, name, len);
        if (id)
        {
            return id;
        }
    }
    return 0;
}

static const char *sym_kind_name(uint8_t kind)
{
    switch (kind)
    {
    case SYM_STRUCT:
        return "struct";
    case SYM_FIELD:
        return "field";
    case SYM_METHOD:
        return "method";
    case SYM_CONST:
        return "const";
    case SYM_IMPORT:
        return "import";
    default:
        return "?";
    }
}

// How far a symbol can be seen:
//   inside  only within its struct
//   domain  within its folder
//   public  from other domains
static const char *visibility(const Unit *u, const Symbol *s)
{
    if (s->owner)
    {
        if (!(s->flags & SYM_F_EXPOSED))
        {
            return "inside";
        }
        return u->bound ? "public" : "domain";
    }
    if (u->is_manifest)
    {
        return s->kind == SYM_CONST ? "config" : "public";
    }
    return (s->kind == SYM_STRUCT && u->bound) ? "public" : "domain";
}

// Symbols are appended in declaration order, so a linear pass prints each unit as written.
void symbols_print(const Unit *u)
{
    printf("module %.*s  (domain %s%s)\n", u->name_len, u->name, domain_name(u->domain),
           u->is_manifest ? ", manifest" : "");

    for (SymId id = 1; id < cx.syms.size; id++)
    {
        const Symbol *s = SYM(id);
        if (s->unit != u->id)
        {
            continue;
        }

        char qualified[128];
        int len;
        const char *name = symbol_name(id, &len);
        int n = 0;
        // Build `outer.inner.name`: owners are at most a few levels deep.
        SymId chain[8];
        int depth = 0;
        for (SymId o = s->owner; o && depth < 8; o = SYM(o)->owner)
        {
            chain[depth++] = o;
        }
        while (depth > 0)
        {
            int olen;
            const char *oname = symbol_name(chain[--depth], &olen);
            n += snprintf(qualified + n, sizeof(qualified) - (size_t)n, "%.*s.", olen, oname);
            if (n >= (int)sizeof(qualified))
            {
                n = (int)sizeof(qualified) - 1;
            }
        }
        snprintf(qualified + n, sizeof(qualified) - (size_t)n, "%.*s", len, name);

        printf("  %-7s %-40s %-7s %s\n", sym_kind_name(s->kind), qualified, visibility(u, s),
               (s->flags & SYM_F_MUT) ? "mut" : "");
    }
}
`````

### `src/v2/compiler/ast/symbol.h`

`````c
// symbol.h (v2): symbols, lexical scopes and visibility.
//
// A symbol is 24 bytes and refers back to its name through the token stream (no copies).
// Visibility needs no keyword and no list; it follows from where a thing is written:
//   inside a struct's braces   -> private to the struct (fields always are)
//   outside, with `self: T`    -> the struct's face
//   inside a file              -> private to the domain (folder)
//   bound in the mod.modal     -> part of the domain's face
// See docs/v2-frontend.md.
#ifndef MODAL_V2_SYMBOL_H
#define MODAL_V2_SYMBOL_H

#include "v2/compiler/ast/ast.h"
#include <stdint.h>

typedef uint32_t SymId; // 0 = no symbol

typedef enum
{
    SYM_NONE,
    SYM_STRUCT,
    SYM_FIELD,
    SYM_METHOD,
    SYM_CONST,
    SYM_IMPORT,
} SymKind;

enum
{
    SYM_F_MUT = 1u << 0,     // mutable field, or overloadable (type-first) method
    SYM_F_EXPOSED = 1u << 1, // the struct's face: a method declared outside its braces
};

typedef struct Symbol
{
    uint32_t tok; // name token, in the owning unit's stream
    uint8_t kind;
    uint8_t flags;
    uint16_t unit;
    SymId owner; // enclosing struct, 0 = the module itself
    NodeId decl; // declaring node, in the owning unit
    union
    {
        SymId next_overload; // SYM_METHOD: next definition in the overload set
        uint32_t scope_ref;  // SYM_STRUCT: member scope (see scope_at)
    };
    SymId next; // hash bucket chain
} Symbol;

_Static_assert(sizeof(Symbol) == 24, "Symbol must stay 24 bytes");

typedef struct
{
    Symbol *data;
    size_t size;
    size_t cap;
} SymVec;

typedef struct Scope
{
    struct Scope *parent;
    SymId owner; // struct that owns this scope, 0 = module scope
    uint32_t nbuckets;
    uint32_t id; // index in cx.scopes
    SymId buckets[];
} Scope;

#define SCOPE_MODULE_BUCKETS 16 // minimums: a scope is sized from its declaration count
#define SCOPE_STRUCT_BUCKETS 8

#define SYM(id) (&cx.syms.data[(id)])

Scope *scope_new(Scope *parent, SymId owner, uint32_t nbuckets);
uint32_t scope_ref(const Scope *s);
Scope *scope_at(uint32_t ref);

SymId symbol_new(Unit *u, SymKind kind, uint8_t flags, uint32_t tok, SymId owner, NodeId decl);
void scope_insert(Scope *s, SymId id);
// Looks in this scope only.
SymId scope_find(const Scope *s, const char *name, size_t len);
// Looks in this scope, then outwards through the parents.
SymId scope_lookup(const Scope *s, const char *name, size_t len);

const char *symbol_name(SymId id, int *len);
void symbols_print(const Unit *u);

#endif // MODAL_V2_SYMBOL_H
`````

### `src/v2/compiler/ast/visitor.h`

`````c
// visitor.h (v2): iterative pre-order AST walk with an explicit stack.
//
// The stack is sized from the node count of the unit: every pending frame is one distinct
// parent->child edge, and there are fewer than 2 * nodes of those (a grouped declaration such
// as `a, b: T` shares its type node). So the walk cannot overflow however deep the tree is,
// and it never recurses on the C stack.
#ifndef MODAL_V2_VISITOR_H
#define MODAL_V2_VISITOR_H

#include "v2/compiler/cx.h"

enum
{
    VISIT_ABORT = -1,
    VISIT_CONTINUE = 0,
    VISIT_SKIP_A = 1, // do not descend into child a / b / c of this node
    VISIT_SKIP_B = 2,
    VISIT_SKIP_C = 4,
    VISIT_SKIP = 7,
};

typedef int (*VisitorFn)(Unit *u, NodeId id, uint32_t depth, void *ctx);

typedef struct
{
    NodeId id;
    uint32_t depth;
} WalkFrame;

// Visits `root` and everything under it (not its siblings). Returns 0 if a visitor aborted.
static inline int walk_ast_iterative(Unit *u, NodeId root, VisitorFn visit, void *ctx)
{
    if (!root)
    {
        return 1;
    }

    ArenaMark mark = arena_mark();
    WalkFrame *stack = (WalkFrame *)arena_alloc((2 * u->nodes.size + 1) * sizeof(WalkFrame));
    ArenaMark top = arena_mark();
    size_t sp = 0;
    int ok = 1;

    stack[sp++] = (WalkFrame){root, 0};

    while (sp > 0)
    {
        WalkFrame f = stack[--sp];
        const AstNode *n = NODE(u, f.id);
        uint8_t mask = ast_child_mask[n->kind];
        if (n->flags & AST_F_LAZY)
        {
            mask &= (uint8_t)~4u; // `c` is a token index while the body is unparsed
        }
        NodeId a = n->a, b = n->b, c = n->c, next = n->next;

        int r = visit(u, f.id, f.depth, ctx);
        if (r < 0)
        {
            ok = 0;
            break;
        }
        mask &= (uint8_t)~r;

        // Pushed in reverse so they pop as: a (and a's siblings), b, c, then our own sibling.
        if (next && f.id != root)
        {
            stack[sp++] = (WalkFrame){next, f.depth};
        }
        if ((mask & 4) && c)
        {
            stack[sp++] = (WalkFrame){c, f.depth + 1};
        }
        if ((mask & 2) && b)
        {
            stack[sp++] = (WalkFrame){b, f.depth + 1};
        }
        if ((mask & 1) && a)
        {
            stack[sp++] = (WalkFrame){a, f.depth + 1};
        }
    }

    // Give the stack back, unless the visitor allocated on top of it.
    if (arena_mark_equal(arena_mark(), top))
    {
        arena_release(mark);
    }
    return ok;
}

#endif // MODAL_V2_VISITOR_H
`````

### `src/v2/compiler/sema/check.c`

`````c
// check.c (v2): names, invocations and types inside bodies.
//
// Before anything can be generated, every body is resolved and typed:
//   - a name is a local, a parameter, `self`, a module-level declaration, an import binding,
//     a member reached through implicit self (when the domain enables it), or it is an error
//   - `f(args)` must name a function; the number of arguments selects the definition (any of
//     them, for an overload set) and each argument must fit its parameter
//   - `value.name` and `value.name(args)` are resolved through the type of `value`, which is
//     also where the inside / outside rule is enforced: fields and helpers written inside a
//     struct's braces are only reachable from that struct's own methods
//   - `T.name(args)` on a struct type calls the method on a fresh zero value (a constructor)
//   - a local takes the type of its initializer; a return must fit the declared type
//
// The results are left in two per-node tables on the unit, which is all a backend reads:
//   unit->types[node]  the type of the expression
//   unit->refs[node]   the declaration a name or an invocation resolved to
#include "v2/compiler/cx.h"
#include "v2/compiler/syntax/parser/parser.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MAX_LOCALS 256

typedef struct
{
    uint32_t tok;
    TypeId type;
} Local;

typedef struct
{
    Unit *u;
    SymId owner;       // struct that owns the callable being checked, 0 = the module
    int has_self;      // the callable declares `self`
    int implicit_self; // domain config: a bare member name stands for self.name
    int in_callable;
    TypeId ret; // declared return type of the callable being checked
    int nlocals;
    int block_base; // first local of the innermost block
    Local locals[MAX_LOCALS];
} Check;

typedef enum
{
    NAME_MISSING,
    NAME_LOCAL,
    NAME_SELF,
    NAME_BUILTIN,
    NAME_SYMBOL, // module-level declaration or import binding
    NAME_MEMBER, // member of the owning struct, through implicit self
    NAME_WILDCARD,
} NameKind;

static TypeId check_expr(Check *c, NodeId id);

// ---- small helpers ---------------------------------------------------------------------------

static int text_is(const Token *t, const char *s)
{
    size_t n = strlen(s);
    return t->len == n && memcmp(t->start, s, n) == 0;
}

static int is_self_node(const Unit *u, NodeId id)
{
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    return n->kind == AST_IDENT && is_self_token(t->start, (int)t->len);
}

static int count_list(const Unit *u, NodeId id)
{
    int n = 0;
    for (; id; id = NODE(u, id)->next)
    {
        n++;
    }
    return n;
}

static Scope *struct_scope(SymId s)
{
    return scope_at(SYM(s)->scope_ref);
}

static TypeId set_type(Check *c, NodeId id, TypeId t)
{
    c->u->types[id] = t;
    return t;
}

// Can a value of type `from` be used where `to` is expected?
static int fits(TypeId from, TypeId to)
{
    if (from == to || from == T_NONE || to == T_NONE || from == T_EXTERN || to == T_EXTERN)
    {
        return 1;
    }
    if ((from == T_INT_LIT && type_is_int(to)) || (to == T_INT_LIT && type_is_int(from)))
    {
        return 1;
    }
    if (from == T_NIL && type_kind(to) == T_PTR)
    {
        return 1;
    }
    // `self` is a reference: a struct value can be returned or passed where *T is expected.
    return type_kind(to) == T_PTR && type_ref(to) == from && type_kind(from) == T_STRUCT;
}

// ---- types of declarations -------------------------------------------------------------------

// The built-in type a name spells, or T_NONE. Decided from the length and two characters,
// since this runs for every type written in the program.
static TypeId prim_type(const Token *t)
{
    const char *s = t->start;
    switch (t->len)
    {
    case 2:
        if (s[1] == '8')
        {
            return s[0] == 'u' ? T_U8 : s[0] == 'i' ? T_I8 : T_NONE;
        }
        return T_NONE;
    case 3:
        if (s[0] == 'i' && s[1] == 'n' && s[2] == 't')
        {
            return T_INT;
        }
        if (s[0] == 'u' || s[0] == 'i')
        {
            int is_unsigned = s[0] == 'u';
            if (s[1] == '1' && s[2] == '6')
            {
                return is_unsigned ? T_U16 : T_I16;
            }
            if (s[1] == '3' && s[2] == '2')
            {
                return is_unsigned ? T_U32 : T_I32;
            }
            if (s[1] == '6' && s[2] == '4')
            {
                return is_unsigned ? T_U64 : T_I64;
            }
        }
        return T_NONE;
    case 4:
        return memcmp(s, "bool", 4) == 0 ? T_BOOL : T_NONE;
    default:
        return T_NONE;
    }
}

// Turns a type as written into a TypeId. `owner` gives `self` its meaning. With `loud` the
// errors are reported; signatures are resolved loudly once and quietly afterwards.
static TypeId resolve_type(Unit *u, NodeId tn, SymId owner, int loud)
{
    if (!tn)
    {
        return T_VOID;
    }

    const AstNode *n = NODE(u, tn);
    switch (n->kind)
    {
    case AST_TYPE_SLICE:
    {
        TypeId elem = resolve_type(u, n->a, owner, loud);
        return elem == T_U8 ? T_STR : elem == T_NONE ? T_NONE : type_make(T_SLICE, elem);
    }
    case AST_TYPE_PTR:
    {
        TypeId elem = resolve_type(u, n->a, owner, loud);
        return elem == T_NONE ? T_NONE : type_make(T_PTR, elem);
    }
    case AST_TYPE_NAME:
    {
        const Token *t = TOK(u, n->tok);
        if (n->aux)
        {
            return T_EXTERN; // a dotted path: a type of another module
        }
        TypeId prim = prim_type(t);
        if (prim != T_NONE)
        {
            return prim;
        }
        if (is_self_token(t->start, (int)t->len))
        {
            return owner ? type_make(T_PTR, type_make(T_STRUCT, owner)) : T_MODULE;
        }
        SymId s = scope_lookup(owner ? struct_scope(owner) : u->scope, t->start, t->len);
        if (s && SYM(s)->kind == SYM_STRUCT)
        {
            return type_make(T_STRUCT, s);
        }
        if (s && SYM(s)->kind == SYM_IMPORT)
        {
            return T_EXTERN;
        }
        if (loud)
        {
            diag_at(u, n->tok, "error", "unknown type '%.*s'", t->len, t->start);
        }
        return T_NONE;
    }
    default:
        if (loud)
        {
            diag_at(u, n->tok, "error",
                    "an inline struct type cannot be used here: declare it "
                    "with a name first");
        }
        return T_NONE;
    }
}

// The function node behind a callable declaration: the method itself, or the literal bound
// by `name := (params) T { }`.
static const AstNode *callable_node(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    return n->kind == AST_DEFINE ? NODE(u, n->a) : n;
}

static TypeId fn_ret_type(Unit *u, SymId fn)
{
    return resolve_type(u, callable_node(u, SYM(fn)->decl)->b, SYM(fn)->owner, 0);
}

// The parameters a caller supplies: everything after `self`.
static NodeId fn_params(const Unit *u, SymId fn)
{
    NodeId params = callable_node(u, SYM(fn)->decl)->a;
    if (params &&
        is_self_token(TOK(u, NODE(u, params)->tok)->start, (int)TOK(u, NODE(u, params)->tok)->len))
    {
        return NODE(u, params)->next;
    }
    return params;
}

// The type of a field or a module-level constant.
static TypeId value_type(Unit *u, SymId s)
{
    const AstNode *decl = NODE(u, SYM(s)->decl);
    if (decl->kind == AST_FIELD_DECL)
    {
        return resolve_type(u, decl->a, SYM(s)->owner, 0);
    }
    if (decl->kind == AST_DEFINE)
    {
        const AstNode *v = NODE(u, decl->a);
        const Token *t = TOK(u, v->tok);
        if (v->kind == AST_NUMBER_LIT)
        {
            return T_INT;
        }
        if (v->kind == AST_STRING_LIT)
        {
            return T_STR;
        }
        if (v->kind == AST_IDENT && (text_is(t, "true") || text_is(t, "false")))
        {
            return T_BOOL;
        }
    }
    return T_EXTERN;
}

// ---- names -----------------------------------------------------------------------------------

static void push_local(Check *c, uint32_t tok, TypeId type)
{
    const Token *t = TOK(c->u, tok);
    for (int i = c->block_base; i < c->nlocals; i++)
    {
        const Token *l = TOK(c->u, c->locals[i].tok);
        if (l->len == t->len && memcmp(l->start, t->start, t->len) == 0)
        {
            diag_at(c->u, tok, "error", "'%.*s' is already declared in this block", t->len,
                    t->start);
            return;
        }
    }
    if (c->nlocals == MAX_LOCALS)
    {
        diag_at(c->u, tok, "error", "too many locals in one function (limit is %d)", MAX_LOCALS);
        return;
    }
    c->locals[c->nlocals++] = (Local){tok, type};
}

static Scope *self_scope(const Check *c)
{
    return c->owner ? struct_scope(c->owner) : c->u->scope;
}

static TypeId self_type(const Check *c)
{
    return c->owner ? type_make(T_STRUCT, c->owner) : T_MODULE;
}

static NameKind find_name(Check *c, const Token *t, SymId *sym, TypeId *type)
{
    *sym = 0;
    *type = T_EXTERN;
    if (is_self_token(t->start, (int)t->len))
    {
        *type = self_type(c);
        return c->has_self ? NAME_SELF : NAME_MISSING;
    }
    for (int i = c->nlocals - 1; i >= 0; i--)
    {
        const Token *l = TOK(c->u, c->locals[i].tok);
        if (l->len == t->len && memcmp(l->start, t->start, t->len) == 0)
        {
            *type = c->locals[i].type;
            return NAME_LOCAL;
        }
    }
    char first = t->start[0];
    if ((first == 't' && text_is(t, "true")) || (first == 'f' && text_is(t, "false")))
    {
        *type = T_BOOL;
        return NAME_BUILTIN;
    }
    if (first == 'n' && text_is(t, "nil"))
    {
        *type = T_NIL;
        return NAME_BUILTIN;
    }
    if (t->kind == TOK_WRITE)
    {
        return NAME_BUILTIN;
    }
    if ((*sym = scope_find(c->u->scope, t->start, t->len)))
    {
        return NAME_SYMBOL;
    }
    if (c->implicit_self && c->has_self && c->owner &&
        (*sym = scope_find(self_scope(c), t->start, t->len)))
    {
        return NAME_MEMBER;
    }
    return module_wildcard_provides(c->u, t->start, t->len) ? NAME_WILDCARD : NAME_MISSING;
}

// The type a declaration has when it is named as a value.
static TypeId symbol_type(Check *c, SymId s)
{
    switch (SYM(s)->kind)
    {
    case SYM_METHOD:
        return type_make(T_FUNC, s);
    case SYM_STRUCT:
        return type_make(T_TYPE, s);
    case SYM_FIELD:
    case SYM_CONST:
        return value_type(c->u, s);
    default:
        return T_EXTERN;
    }
}

// If some struct of this file has a member with this name, says how to reach it.
static void report_missing(Check *c, uint32_t tok, int is_call)
{
    const Token *t = TOK(c->u, tok);

    if (is_self_token(t->start, (int)t->len))
    {
        diag_at(c->u, tok, "error", "`self` is used in a function that does not declare it");
        return;
    }
    for (SymId id = 1; id < cx.syms.size; id++)
    {
        const Symbol *s = SYM(id);
        int len, tlen;
        const char *name = symbol_name(id, &len);
        if (s->unit != c->u->id || !s->owner || (uint32_t)len != t->len ||
            memcmp(name, t->start, t->len) != 0)
        {
            continue;
        }
        const char *type = symbol_name(s->owner, &tlen);
        if (s->owner == c->owner && c->has_self)
        {
            diag_at(c->u, tok, "error",
                    "'%.*s' is a member of '%.*s': write `self.%.*s` (or set `implicit_self := "
                    "true` in the domain's mod.modal)",
                    len, name, tlen, type, len, name);
        }
        else
        {
            diag_at(c->u, tok, "error",
                    "'%.*s' is a member of '%.*s', not a free name: reach it through a value, "
                    "`value.%.*s`",
                    len, name, tlen, type, len, name);
        }
        return;
    }
    diag_at(c->u, tok, "error",
            is_call ? "call to undeclared function '%.*s'" : "'%.*s' is not declared", t->len,
            t->start);
}

// `print := import("std").io.print`: the one std function the compiler knows. It prints its
// arguments and returns nothing.
int sema_is_std_print(const Unit *u, SymId s)
{
    if (!s || SYM(s)->kind != SYM_IMPORT || NODE(u, SYM(s)->decl)->kind != AST_DEFINE)
    {
        return 0;
    }
    static const char *const path[] = {"print", "io"};
    NodeId id = NODE(u, SYM(s)->decl)->a;
    for (int i = 0; i < 2; i++)
    {
        const AstNode *n = NODE(u, id);
        const Token *t = TOK(u, n->tok);
        if (n->kind != AST_SELECTOR || t->len != strlen(path[i]) ||
            memcmp(t->start, path[i], t->len) != 0)
        {
            return 0;
        }
        id = n->a;
    }
    const Token *arg = TOK(u, NODE(u, id)->tok);
    return NODE(u, id)->kind == AST_IMPORT &&
           ((arg->len == 3 && memcmp(arg->start, "std", 3) == 0) ||
            (arg->len == 5 && memcmp(arg->start, "\"std\"", 5) == 0));
}

// ---- invocations -----------------------------------------------------------------------------

// Picks the definition whose parameter count matches (any one, for an overload set).
// Returns 0 after reporting when none does.
static SymId select_overload(Check *c, uint32_t at, SymId first, int argc)
{
    char accepted[96];
    size_t len = 0;
    int defs = 0;

    accepted[0] = '\0';
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        int arity = count_list(c->u, fn_params(c->u, s));
        if (arity == argc)
        {
            return s;
        }
        if (len < sizeof(accepted) - 8)
        {
            len += (size_t)snprintf(accepted + len, sizeof(accepted) - len, "%s%d",
                                    defs ? " or " : "", arity);
        }
        defs++;
    }

    int nlen, line, col;
    const char *name = symbol_name(first, &nlen);
    source_pos(c->u->src, TOK(c->u, SYM(first)->tok)->start, &line, &col);
    diag_at(c->u, at, "error", "'%.*s' takes %s argument%s, but %d %s given (declared at %s:%d)",
            nlen, name, accepted, (defs == 1 && accepted[0] == '1' && !accepted[1]) ? "" : "s",
            argc, argc == 1 ? "is" : "are", c->u->path, line);
    return 0;
}

// Types the arguments and, when the definition is known, checks each against its parameter.
static void check_args(Check *c, NodeId args, SymId fn)
{
    Unit *u = c->u;
    NodeId param = fn ? fn_params(u, fn) : 0;
    int index = 1;

    for (NodeId a = args; a; a = NODE(u, a)->next, index++)
    {
        TypeId at = check_expr(c, a);
        if (at == T_VOID)
        {
            diag_at(u, NODE(u, a)->tok, "error", "argument %d has no value", index);
        }
        if (!param)
        {
            continue;
        }
        TypeId pt = resolve_type(c->u, NODE(u, param)->a, SYM(fn)->owner, 0);
        if (!fits(at, pt))
        {
            char got[64], want[64];
            int nlen;
            const char *name = symbol_name(fn, &nlen);
            const Token *pn = TOK(u, NODE(u, param)->tok);
            diag_at(u, NODE(u, a)->tok, "error",
                    "argument %d of '%.*s' is %s, but parameter '%.*s' is %s", index, nlen, name,
                    type_str(at, got, sizeof(got)), pn->len, pn->start,
                    type_str(pt, want, sizeof(want)));
        }
        param = NODE(u, param)->next;
    }
}

// Calls `first` (a function or an overload set) with the arguments of call node `id`.
static TypeId call_function(Check *c, NodeId id, uint32_t at, SymId first)
{
    NodeId args = NODE(c->u, id)->b;
    SymId fn = select_overload(c, at, first, count_list(c->u, args));
    check_args(c, args, fn);
    c->u->refs[id] = fn;
    return fn ? fn_ret_type(c->u, fn) : T_NONE;
}

// Finds `name` among the members of struct `s`, applying the inside / outside rule: from
// outside the struct's own methods only its face (methods written below its braces) exists.
//
// A field and its accessor may share a name (`u8 n1` and `u8 n1(self: T): self.n1`). Which
// one is meant follows from the use: `x.n1(...)` is the method, `x.n1` is the field when the
// field can be seen from here, and the accessor otherwise.
static SymId find_member(Check *c, SymId s, uint32_t name_tok, int is_call)
{
    const Token *name = TOK(c->u, name_tok);
    int tlen;
    const char *type = symbol_name(s, &tlen);
    SymId member = scope_find(struct_scope(s), name->start, name->len);

    if (member && !is_call && SYM(member)->kind == SYM_METHOD && c->owner == s)
    {
        for (SymId other = SYM(member)->next; other; other = SYM(other)->next)
        {
            int olen;
            const char *oname = symbol_name(other, &olen);
            if (SYM(other)->kind == SYM_FIELD && (uint32_t)olen == name->len &&
                memcmp(oname, name->start, name->len) == 0)
            {
                member = other;
                break;
            }
        }
    }

    if (!member)
    {
        diag_at(c->u, name_tok, "error", "'%.*s' has no member '%.*s'", tlen, type, name->len,
                name->start);
        return 0;
    }
    if (c->owner == s)
    {
        return member; // a struct's own methods see all of it
    }
    if (SYM(member)->kind == SYM_FIELD)
    {
        diag_at(c->u, name_tok, "error",
                "'%.*s' is a field of '%.*s', and fields are never visible outside their "
                "struct. Add an accessor below it: `T %.*s(self: %.*s): self.%.*s`",
                name->len, name->start, tlen, type, name->len, name->start, tlen, type, name->len,
                name->start);
        return 0;
    }
    if (!(SYM(member)->flags & SYM_F_EXPOSED))
    {
        diag_at(c->u, name_tok, "error",
                "'%.*s' is inside '%.*s'. To use it here, move it below the closing brace of "
                "the struct",
                name->len, name->start, tlen, type);
        return 0;
    }
    return member;
}

// The struct a value of this type gives access to, or 0.
static SymId struct_of(TypeId t)
{
    TypeKind k = type_kind(t);
    if (k == T_PTR)
    {
        t = type_ref(t);
        k = type_kind(t);
    }
    return (k == T_STRUCT || k == T_TYPE) ? type_ref(t) : 0;
}

static TypeId check_call(Check *c, NodeId id)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    NodeId callee = n->a;
    NodeId args = n->b;
    const AstNode *cn = NODE(u, callee);

    cx.calls_resolved++;

    if (cn->kind == AST_IDENT)
    {
        SymId sym;
        TypeId type;
        const Token *t = TOK(u, cn->tok);
        NameKind kind = find_name(c, t, &sym, &type);

        if (kind == NAME_MISSING)
        {
            report_missing(c, cn->tok, 1);
            check_args(c, args, 0);
            return T_NONE;
        }
        if (kind == NAME_SYMBOL || kind == NAME_MEMBER)
        {
            u->refs[callee] = sym;
            if (SYM(sym)->kind == SYM_METHOD)
            {
                set_type(c, callee, type_make(T_FUNC, sym));
                return call_function(c, id, cn->tok, sym);
            }
            if (SYM(sym)->kind == SYM_STRUCT)
            {
                diag_at(u, cn->tok, "error",
                        "'%.*s' is a type, not a function. To create one, call a constructor "
                        "on it: `%.*s.new()`",
                        t->len, t->start, t->len, t->start);
                check_args(c, args, 0);
                return T_NONE;
            }
            if (SYM(sym)->kind != SYM_IMPORT && value_type(c->u, sym) != T_EXTERN)
            {
                diag_at(u, cn->tok, "error", "'%.*s' is not a function", t->len, t->start);
                check_args(c, args, 0);
                return T_NONE;
            }
        }
        else if (kind == NAME_LOCAL && type_kind(type) == T_FUNC)
        {
            return call_function(c, id, cn->tok, type_ref(type)); // f := double; f(1)
        }
        else if (kind == NAME_LOCAL && type != T_EXTERN && type != T_NONE)
        {
            diag_at(u, cn->tok, "error", "'%.*s' is not a function", t->len, t->start);
        }
        set_type(c, callee, T_EXTERN);
        check_args(c, args, 0);
        // write(...) and std's print return nothing; anything else here is external
        return (kind == NAME_BUILTIN || sema_is_std_print(u, sym)) ? T_VOID : T_EXTERN;
    }

    if (cn->kind == AST_SELECTOR)
    {
        TypeId base = (is_self_node(u, cn->a) && c->has_self) ? set_type(c, cn->a, self_type(c))
                                                              : check_expr(c, cn->a);
        SymId s = struct_of(base);
        const Token *name = TOK(u, cn->tok);

        if (s)
        {
            SymId member = find_member(c, s, cn->tok, 1);
            if (member && SYM(member)->kind != SYM_METHOD)
            {
                diag_at(u, cn->tok, "error", "'%.*s' is a field, not a method", name->len,
                        name->start);
                member = 0;
            }
            if (!member)
            {
                check_args(c, args, 0);
                return T_NONE;
            }
            u->refs[callee] = member;
            set_type(c, callee, type_make(T_FUNC, member));
            return call_function(c, id, cn->tok, member);
        }
        if (base == T_MODULE) // `self.f()` in a module-level function: a name of the module
        {
            SymId member = scope_find(u->scope, name->start, name->len);
            if (!member)
            {
                diag_at(u, cn->tok, "error", "'%.*s' has no member '%.*s'", u->name_len, u->name,
                        name->len, name->start);
            }
            else if (SYM(member)->kind == SYM_METHOD)
            {
                u->refs[callee] = member;
                return call_function(c, id, cn->tok, member);
            }
        }
        else if (base != T_EXTERN && base != T_NONE)
        {
            char got[64];
            diag_at(u, cn->tok, "error", "a value of type %s has no method '%.*s'",
                    type_str(base, got, sizeof(got)), name->len, name->start);
        }
        check_args(c, args, 0);
        return base == T_NONE ? T_NONE : T_EXTERN;
    }

    check_expr(c, callee);
    check_args(c, args, 0);
    return T_EXTERN;
}

// ---- expressions and statements --------------------------------------------------------------

static TypeId check_binary(Check *c, const AstNode *n)
{
    TypeId l = check_expr(c, n->a);
    TypeId r = check_expr(c, n->b);
    TokenKind op = (TokenKind)TOK(c->u, n->tok)->kind;
    int compare = op == TOK_CMP || op == TOK_NEQ || op == TOK_LT || op == TOK_GT || op == TOK_LE ||
                  op == TOK_GE;
    int logic = op == TOK_AND || op == TOK_OR;

    if (l == T_NONE || r == T_NONE)
    {
        return T_NONE;
    }
    if (l == T_EXTERN || r == T_EXTERN)
    {
        return (compare || logic) ? T_BOOL : T_EXTERN;
    }

    int ok = logic     ? (l == T_BOOL && r == T_BOOL)
             : compare ? (fits(l, r) || fits(r, l))
                       : (type_is_int(l) && type_is_int(r) && fits(l, r));
    if (!ok)
    {
        char a[64], b[64];
        const Token *t = TOK(c->u, n->tok);
        diag_at(c->u, n->tok, "error", "'%.*s' cannot be applied to %s and %s", t->len, t->start,
                type_str(l, a, sizeof(a)), type_str(r, b, sizeof(b)));
        return T_NONE;
    }
    if (compare || logic)
    {
        return T_BOOL;
    }
    return l == T_INT_LIT ? r : l;
}

static TypeId check_selector(Check *c, NodeId id)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    const Token *name = TOK(u, n->tok);
    TypeId base = (is_self_node(u, n->a) && c->has_self) ? set_type(c, n->a, self_type(c))
                                                         : check_expr(c, n->a);
    SymId s = struct_of(base);

    if (s)
    {
        SymId member = find_member(c, s, n->tok, 0);
        if (!member)
        {
            return T_NONE;
        }
        u->refs[id] = member;
        return symbol_type(c, member);
    }
    if (base == T_MODULE)
    {
        SymId member = scope_find(u->scope, name->start, name->len);
        if (!member)
        {
            diag_at(u, n->tok, "error", "'%.*s' has no member '%.*s'", u->name_len, u->name,
                    name->len, name->start);
            return T_NONE;
        }
        u->refs[id] = member;
        return symbol_type(c, member);
    }
    if (base != T_EXTERN && base != T_NONE)
    {
        char got[64];
        diag_at(u, n->tok, "error", "a value of type %s has no member '%.*s'",
                type_str(base, got, sizeof(got)), name->len, name->start);
        return T_NONE;
    }
    return base;
}

static void check_return(Check *c, NodeId id, NodeId value)
{
    TypeId got = value ? check_expr(c, value) : T_VOID;
    char a[64], b[64];

    if (!c->in_callable || c->ret == T_NONE)
    {
        return;
    }
    if (c->ret == T_VOID && value && got != T_VOID)
    {
        diag_at(c->u, NODE(c->u, id)->tok, "error",
                "this function declares no return type, but returns %s",
                type_str(got, a, sizeof(a)));
    }
    else if (c->ret != T_VOID && !fits(got, c->ret))
    {
        diag_at(c->u, NODE(c->u, id)->tok, "error", "returns %s, but the function declares %s",
                type_str(got, a, sizeof(a)), type_str(c->ret, b, sizeof(b)));
    }
}

// Makes sure the per-node tables cover every node, and clears the entries of [from, size).
static void side_tables(Unit *u, NodeId from)
{
    if (u->nodes.size > u->side_cap)
    {
        // Sized like the node array, which already has room for the largest body.
        size_t cap = u->nodes.cap > u->nodes.size ? u->nodes.cap : u->nodes.size;
        TypeId *types = (TypeId *)arena_alloc(cap * sizeof(TypeId));
        SymId *refs = (SymId *)arena_alloc(cap * sizeof(SymId));
        if (u->side_cap)
        {
            memcpy(types, u->types, u->side_cap * sizeof(TypeId));
            memcpy(refs, u->refs, u->side_cap * sizeof(SymId));
        }
        u->types = types;
        u->refs = refs;
        u->side_cap = cap;
    }
    memset(u->types + from, 0, (u->nodes.size - from) * sizeof(TypeId));
    memset(u->refs + from, 0, (u->nodes.size - from) * sizeof(SymId));
}

// Checks one function. `fn` is a method declaration or a function literal; `sym` is its
// symbol when it is a declared function (0 for a literal inside another body).
//
// This is where memory is kept small. A deferred body is tokenized and parsed here, into
// the tails of the token and node arrays; its module paths and types are checked; the
// backend, if one is running, emits the function while the body is in memory; then both
// tails are cut off again. At any moment the compiler holds the declarations of the file
// plus the tokens and tree of one function.
static void check_callable(Check *c, NodeId fn, SymId owner, SymId sym)
{
    Unit *u = c->u;
    // Copies, not a pointer: parsing the body may move the node array.
    uint8_t flags = NODE(u, fn)->flags;
    uint8_t kind = NODE(u, fn)->kind;
    NodeId params = NODE(u, fn)->a;
    NodeId ret_node = NODE(u, fn)->b;
    NodeId body = NODE(u, fn)->c;
    uint32_t deferred_tok = body;
    size_t mark = u->nodes.size;
    size_t tok_mark = u->tokens.size;

    if (flags & AST_F_LAZY)
    {
        uint64_t t = cx.timing ? clock_ns() : 0;
        body = parse_function_body(u, deferred_tok);
        if (cx.timing)
        {
            phase_end(PHASE_PARSE, t);
        }
        if (!body)
        {
            u->nodes.size = mark; // errors inside, already reported
            u->tokens.size = tok_mark;
            return;
        }
        NODE(u, fn)->c = body;
        NODE(u, fn)->flags = (uint8_t)(flags & ~AST_F_LAZY);
        side_tables(u, (NodeId)mark);
        module_body_nodes(u, (NodeId)mark);
    }

    // Only the scalars are saved: the locals stay where they are, which is also what lets a
    // nested function literal see the enclosing ones.
    SymId saved_owner = c->owner;
    int saved_self = c->has_self, saved_in = c->in_callable, saved_base = c->block_base;
    int saved_locals = c->nlocals;
    TypeId saved_ret = c->ret;

    c->owner = owner;
    c->has_self = (flags & AST_F_SELF) ? 1 : (kind == AST_FN_LIT && saved_self);
    c->in_callable = 1;
    c->ret = resolve_type(u, ret_node, owner, 1);
    c->block_base = c->nlocals;

    for (NodeId p = params; p; p = NODE(u, p)->next)
    {
        const Token *name = TOK(u, NODE(u, p)->tok);
        if (!is_self_token(name->start, (int)name->len))
        {
            push_local(c, NODE(u, p)->tok, resolve_type(u, NODE(u, p)->a, owner, 1));
        }
    }

    if (flags & AST_F_ONELINE)
    {
        check_return(c, body, body); // `T name(params): expr` returns the expression
    }
    else
    {
        check_expr(c, body);
    }

    c->owner = saved_owner;
    c->has_self = saved_self;
    c->in_callable = saved_in;
    c->block_base = saved_base;
    c->nlocals = saved_locals;
    c->ret = saved_ret;

    if (sym)
    {
        codegen_function(u, sym);
    }
    if (flags & AST_F_LAZY)
    {
        NODE(u, fn)->c = deferred_tok; // back to "not parsed": its tokens and nodes are gone
        NODE(u, fn)->flags = flags;
        u->nodes.size = mark;
        u->tokens.size = tok_mark;
    }
}

// Depth is bounded by the parser's nesting limit, so this recursion is too.
static TypeId check_expr(Check *c, NodeId id)
{
    if (!id)
    {
        return T_VOID;
    }
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    TypeId type = T_VOID;

    switch (n->kind)
    {
    case AST_NUMBER_LIT:
        type = T_INT_LIT;
        break;
    case AST_STRING_LIT:
    {
        const Token *t = TOK(u, n->tok);
        for (uint32_t i = 1; i + 1 < t->len; i++) // the escapes a string may contain
        {
            if (t->start[i] == '\\')
            {
                char esc = t->start[++i];
                if (!strchr("ntr0\\\"", esc) || esc == '\0')
                {
                    diag_at(u, n->tok, "error",
                            "unknown escape '\\%c' in a string (known: \\n \\t \\r \\0 \\\\ \\\")",
                            esc);
                    break;
                }
            }
        }
        type = T_STR;
        break;
    }
    case AST_IDENT:
    {
        SymId sym;
        NameKind kind = find_name(c, TOK(u, n->tok), &sym, &type);
        cx.names_resolved++;
        if (kind == NAME_MISSING)
        {
            report_missing(c, n->tok, 0);
            type = T_NONE;
        }
        else if (kind == NAME_SYMBOL || kind == NAME_MEMBER)
        {
            u->refs[id] = sym;
            type = symbol_type(c, sym);
        }
        else if (TOK(u, n->tok)->kind == TOK_WRITE)
        {
            diag_at(u, n->tok, "error", "`write` can only be called: write(...)");
            type = T_NONE;
        }
        break;
    }
    case AST_CALL:
        type = check_call(c, id);
        break;
    case AST_SELECTOR:
        type = check_selector(c, id);
        break;
    case AST_BIN_OP:
        type = check_binary(c, n);
        break;
    case AST_UNARY_OP:
    {
        TypeId operand = check_expr(c, n->a);
        TokenKind op = (TokenKind)TOK(u, n->tok)->kind;
        if (operand == T_VOID)
        {
            diag_at(u, n->tok, "error", "the operand has no value");
            type = T_NONE;
        }
        else if (operand == T_NONE || operand == T_EXTERN)
        {
            type = operand;
        }
        else if (op == TOK_AMP)
        {
            type = type_make(T_PTR, operand);
        }
        else if (op == TOK_STAR)
        {
            type = type_kind(operand) == T_PTR ? type_ref(operand) : T_NONE;
            if (type == T_NONE)
            {
                diag_at(u, n->tok, "error", "only a pointer can be dereferenced");
            }
        }
        else if ((op == TOK_MINUS && !type_is_int(operand)) ||
                 (op == TOK_BANG && operand != T_BOOL))
        {
            char got[64];
            diag_at(u, n->tok, "error", "'%c' cannot be applied to %s", op == TOK_MINUS ? '-' : '!',
                    type_str(operand, got, sizeof(got)));
            type = T_NONE;
        }
        else
        {
            type = operand;
        }
        break;
    }
    case AST_PAREN_GROUP:
        type = check_expr(c, n->a);
        break;
    case AST_INDEX:
        check_expr(c, n->a);
        check_expr(c, n->b);
        type = T_EXTERN;
        break;
    case AST_ASSIGN:
    {
        TypeId target = check_expr(c, n->a);
        TypeId value = check_expr(c, n->b);
        const AstNode *t = NODE(u, n->a);
        SymId field = t->kind == AST_SELECTOR ? u->refs[n->a] : 0;

        if (t->kind != AST_IDENT && t->kind != AST_SELECTOR)
        {
            diag_at(u, n->tok, "error", "only a local or a field can be assigned to");
        }
        else if (field && SYM(field)->kind == SYM_FIELD && !(SYM(field)->flags & SYM_F_MUT))
        {
            const Token *name = TOK(u, t->tok);
            diag_at(u, n->tok, "error",
                    "'%.*s' is immutable (declared `%.*s: T`). Declare it type-first, "
                    "`T %.*s`, to assign to it",
                    name->len, name->start, name->len, name->start, name->len, name->start);
        }
        else if (!fits(value, target))
        {
            char a[64], b[64];
            diag_at(u, n->tok, "error", "cannot assign %s to %s", type_str(value, a, sizeof(a)),
                    type_str(target, b, sizeof(b)));
        }
        type = T_VOID;
        break;
    }
    case AST_DEFINE:
    {
        const Token *name = TOK(u, n->tok);
        type = check_expr(c, n->a);
        if (type == T_VOID || type == T_NIL)
        {
            diag_at(u, n->tok, "error", "'%.*s' cannot be bound: the expression has no value",
                    name->len, name->start);
            type = T_NONE;
        }
        else if (type == T_INT_LIT)
        {
            type = T_INT; // an integer literal on its own is an int
        }
        push_local(c, n->tok, type); // visible after its own initializer
        break;
    }
    case AST_RETURN:
        check_return(c, id, n->a);
        break;
    case AST_BLOCK:
    {
        int saved_count = c->nlocals, saved_base = c->block_base;
        c->block_base = c->nlocals;
        for (NodeId s = n->a; s; s = NODE(u, s)->next)
        {
            check_expr(c, s);
        }
        c->nlocals = saved_count; // a block's locals end with the block
        c->block_base = saved_base;
        break;
    }
    case AST_FN_LIT:
        check_callable(c, id, c->owner, 0);
        type = T_EXTERN; // a function value: callable, not otherwise typed yet
        break;
    default: // import chains: checked by the module linker
        type = T_EXTERN;
        break;
    }

    return set_type(c, id, type);
}

// ---- declarations ----------------------------------------------------------------------------

// The struct a top-level callable attaches to through `self: T`, or 0 for the module.
static SymId attached_owner(const Unit *u, NodeId params)
{
    if (!params || !NODE(u, params)->a)
    {
        return 0;
    }
    const Token *name = TOK(u, NODE(u, params)->tok);
    const AstNode *type = NODE(u, NODE(u, params)->a);
    if (!is_self_token(name->start, (int)name->len) || type->kind != AST_TYPE_NAME || type->aux)
    {
        return 0;
    }
    const Token *t = TOK(u, type->tok);
    SymId s = scope_find(u->scope, t->start, t->len);
    return (s && SYM(s)->kind == SYM_STRUCT) ? s : 0;
}

static void check_field(Check *c, NodeId decl, SymId owner)
{
    const AstNode *n = NODE(c->u, decl);
    TypeId declared = resolve_type(c->u, n->a, owner, 1);
    if (n->b)
    {
        TypeId init = check_expr(c, n->b);
        if (!fits(init, declared))
        {
            char a[64], b[64];
            diag_at(c->u, NODE(c->u, n->b)->tok, "error", "cannot initialize %s with %s",
                    type_str(declared, b, sizeof(b)), type_str(init, a, sizeof(a)));
        }
    }
}

// The symbol a callable declaration created: looked up by name in the scope that owns it,
// then matched by declaration among the overloads and any field sharing the name.
static SymId decl_symbol(const Unit *u, SymId owner, NodeId decl, uint32_t name_tok)
{
    const Token *name = TOK(u, name_tok);
    const Scope *scope = owner ? struct_scope(owner) : u->scope;
    SymId first = scope_find(scope, name->start, name->len);

    while (first)
    {
        for (SymId o = first; o; o = SYM(o)->kind == SYM_METHOD ? SYM(o)->next_overload : 0)
        {
            if (SYM(o)->decl == decl)
            {
                return o;
            }
        }
        // the same name further down the bucket: an accessor sharing its field's name
        SymId s = SYM(first)->next;
        for (; s; s = SYM(s)->next)
        {
            int len;
            const char *other = symbol_name(s, &len);
            if ((uint32_t)len == name->len && memcmp(other, name->start, name->len) == 0)
            {
                break;
            }
        }
        first = s;
    }
    return 0;
}

// Declarations are checked in two sweeps. First everything that is not a function (fields,
// constants, module-level statements), so the backend can emit types, globals and
// prototypes; then the functions, one at a time.
static void check_decl(Check *c, NodeId decl, Scope *scope, SymId enclosing, int functions)
{
    Unit *u = c->u;
    uint8_t kind = NODE(u, decl)->kind;
    uint32_t tok = NODE(u, decl)->tok;
    NodeId a = NODE(u, decl)->a;

    switch (kind)
    {
    case AST_METHOD_DECL:
        if (functions)
        {
            SymId owner = enclosing ? enclosing : attached_owner(u, a);
            check_callable(c, decl, owner, decl_symbol(u, owner, decl, tok));
        }
        break;
    case AST_DEFINE:
        if (NODE(u, a)->kind == AST_FN_LIT)
        {
            if (functions)
            {
                SymId owner = enclosing ? enclosing : attached_owner(u, NODE(u, a)->a);
                check_callable(c, a, owner, decl_symbol(u, owner, decl, tok));
            }
        }
        else if (!functions)
        {
            check_expr(c, a);
        }
        break;
    case AST_FIELD_DECL:
        if (!functions)
        {
            check_field(c, decl, enclosing);
        }
        break;
    case AST_STRUCT_DECL:
    {
        const Token *name = TOK(u, tok);
        SymId self = scope_find(scope, name->start, name->len);
        if (self && SYM(self)->kind == SYM_STRUCT)
        {
            for (NodeId m = a; m; m = NODE(u, m)->next)
            {
                check_decl(c, m, struct_scope(self), self, functions);
            }
        }
        break;
    }
    case AST_VAL_DECL:
        break;
    default:
        if (!functions)
        {
            check_expr(c, decl); // a top-level statement such as write("...")
        }
        break;
    }
}

TypeId sema_fn_ret(Unit *u, SymId fn)
{
    return fn_ret_type(u, fn);
}

TypeId sema_param_type(Unit *u, SymId fn, NodeId param)
{
    return resolve_type(u, NODE(u, param)->a, SYM(fn)->owner, 0);
}

TypeId sema_value_type(Unit *u, SymId field_or_const)
{
    return value_type(u, field_or_const);
}

NodeId sema_fn_params(const Unit *u, SymId fn)
{
    return fn_params(u, fn);
}

// A file whose declarations did not parse is not checked, but the syntax errors inside its
// function bodies are still worth reporting in the same run.
void sema_syntax_only(Unit *u)
{
    size_t decls = u->nodes.size, toks = u->tokens.size;
    for (NodeId id = 1; id < decls; id++)
    {
        if (NODE(u, id)->flags & AST_F_LAZY)
        {
            parse_function_body(u, NODE(u, id)->c);
            u->nodes.size = decls;
            u->tokens.size = toks;
        }
    }
}

void sema_check(Unit *u)
{
    // One type and one resolved declaration per node (8 bytes), which is all a backend
    // reads. They cover the declarations now and grow to fit the largest body later.
    side_tables(u, 0);

    // The checker state is a few KB of locals: it lives on the C stack.
    Check c;
    memset(&c, 0, offsetof(Check, locals)); // the locals array needs no clearing
    c.u = u;
    c.implicit_self = domain_config_bool(u->domain, "implicit_self", 0);

    for (int functions = 0; functions < 2; functions++)
    {
        if (functions)
        {
            codegen_begin(u); // everything a signature can mention is checked by now
        }
        for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
        {
            c.nlocals = 0;
            c.block_base = 0;
            check_decl(&c, d, u->scope, 0, functions);
        }
    }
}
`````

### `src/v2/compiler/sema/module.c`

`````c
// module.c (v2): files, domains and visibility.
//
//   example/john.modal   module `john`, in domain `example`
//   example/mod.modal    the domain's configuration, and the modules that form its face
//
// One rule at every level: a body hides its inside, and its face is written just outside it.
//   struct  inside the braces: fields and helpers    outside: methods with `self: T`
//   file    its bindings and free functions          outside: what mod.modal binds or defines
//   folder  its files                                outside: what the parent's mod.modal binds
// Nothing is listed anywhere, and a type crosses a domain boundary as an opaque type.
#define _XOPEN_SOURCE 700
#include "v2/compiler/cx.h"
#include "v2/compiler/syntax/parser/parser.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHAIN_MAX 32
#define FOLLOW_MAX 8

typedef enum
{
    CHAIN_SILENT, // just find where the chain leads
    CHAIN_CHECK,  // report names that do not exist or cannot be seen from here
} ChainMode;

typedef struct
{
    Unit *unit;
    Scope *scope;
    int known;
} Target;

static Target chain_eval(Unit *u, NodeId chain, ChainMode mode, int depth);

// ---- paths and domains -----------------------------------------------------------------------

static const char *arena_strndup(const char *s, size_t n)
{
    char *copy = (char *)arena_alloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

const char *domain_name(uint16_t domain)
{
    return base_name(cx.domains[domain]);
}

static uint16_t domain_intern(const char *dir, size_t len)
{
    for (uint16_t i = 0; i < cx.ndomains; i++)
    {
        if (strlen(cx.domains[i]) == len && memcmp(cx.domains[i], dir, len) == 0)
        {
            return i;
        }
    }
    if (cx.ndomains == MAX_DOMAINS)
    {
        fprintf(stderr, "fatal: too many domains (limit is %d)\n", MAX_DOMAINS);
        exit(2);
    }
    cx.domains[cx.ndomains] = arena_strndup(dir, len);
    return cx.ndomains++;
}

static int read_file(Unit *u)
{
    FILE *f = fopen(u->canon, "rb");
    if (!f)
    {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0)
    {
        fclose(f);
        return 0;
    }
    char *buffer = (char *)arena_alloc((size_t)size + 1);
    size_t got = fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[got] = '\0';
    u->src = buffer;
    u->len = strlen(buffer); // a stray NUL byte ends the source
    return 1;
}

Unit *unit_load(const char *path)
{
    char canon[PATH_MAX];
    if (!realpath(path, canon))
    {
        return NULL;
    }
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (strcmp(cx.units[i].canon, canon) == 0)
        {
            return &cx.units[i];
        }
    }
    if (cx.nunits == MAX_UNITS)
    {
        fprintf(stderr, "fatal: too many source files (limit is %d)\n", MAX_UNITS);
        exit(2);
    }

    Unit *u = &cx.units[cx.nunits];
    *u = (Unit){.id = cx.nunits};
    u->path = arena_strndup(path, strlen(path));
    u->canon = arena_strndup(canon, strlen(canon));

    const char *base = base_name(u->canon);
    size_t dir_len = base > u->canon + 1 ? (size_t)(base - u->canon) - 1 : 1;
    u->domain = domain_intern(u->canon, dir_len);
    u->is_manifest = strcmp(base, "mod.modal") == 0;

    if (u->is_manifest)
    {
        u->name = domain_name(u->domain);
        u->name_len = (int)strlen(u->name);
    }
    else
    {
        const char *dot = strrchr(base, '.');
        u->name = base;
        u->name_len = dot ? (int)(dot - base) : (int)strlen(base);
    }

    if (!read_file(u))
    {
        return NULL;
    }
    cx.nunits++;

    uint64_t t = clock_ns();
    int lex_errors = scan_all(u->path, u->src, u->len, cx.eager, &u->tokens, &u->max_body);
    u->errors += lex_errors;
    cx.errors += lex_errors;

    t = phase_end(PHASE_SCAN, t);

    // The node array starts small and doubles; slot 0 means "no node".
    ast.new.node(u, AST_NONE, 0, 0, 0, 0);

    Parser parser;
    parser_init(&parser, u);
    u->root = parse_program(&parser);

    // Both arrays are trimmed to the declarations plus room for the largest deferred body:
    // the scanner bounded its token count while skipping it, and a body has fewer nodes
    // than tokens. Bodies are tokenized and parsed into that room one at a time, so neither
    // array grows or moves again, and what the compiler holds is the declarations and one
    // body.
    size_t room = u->max_body ? u->max_body + 16 : 0;
    Token *toks = (Token *)realloc(u->tokens.data, (u->tokens.size + room) * sizeof(Token));
    AstNode *nodes = (AstNode *)realloc(u->nodes.data, (u->nodes.size + room) * sizeof(AstNode));
    if (!toks || !nodes)
    {
        fprintf(stderr, "fatal: out of memory while parsing %s\n", u->path);
        exit(2);
    }
    heap_account(u->nodes.cap * sizeof(AstNode), (u->nodes.size + room) * sizeof(AstNode));
    heap_account(0, (u->tokens.size + room) * sizeof(Token));
    u->tokens.data = toks;
    u->tokens.cap = u->tokens.size + room;
    u->nodes.data = nodes;
    u->nodes.cap = u->nodes.size + room;

    t = phase_end(PHASE_PARSE, t);

    sema_declare(u);
    u->broken = u->errors > 0;
    phase_end(PHASE_DECLARE, t);
    return u;
}

// ---- import resolution -----------------------------------------------------------------------

static int file_exists(const char *path)
{
    return access(path, R_OK) == 0;
}

// Finds the file an import names, relative to the importing file:
//   import("./a/b.modal")  that file
//   import("./a/b")        a/b.modal, or the domain a/b/mod.modal
//   import(b)              a bare name: also looked for in every folder above
// Only `std` may be missing from disk; any other import that resolves to nothing is an error.
static Unit *import_target(Unit *u, NodeId import)
{
    const Token *arg = TOK(u, NODE(u, import)->tok);
    const char *name = arg->start;
    int len = arg->len;
    if (arg->kind == TOK_STRING)
    {
        name++;
        len -= (len >= 2 && arg->start[len - 1] == '"') ? 2 : 1;
    }
    if (len <= 0)
    {
        diag_at(u, NODE(u, import)->tok, "error", "import needs a module name");
        return NULL;
    }

    const char *slash = strrchr(u->path, '/');
    int dir_len = slash ? (int)(slash - u->path) : 1;
    const char *dir = slash ? u->path : ".";
    int relative = name[0] == '.' && (name[1] == '/' || (name[1] == '.' && name[2] == '/'));
    int has_ext = len > 6 && memcmp(name + len - 6, ".modal", 6) == 0;

    const char *rel = name; // "./x" composes as "dir/x", which reads better in diagnostics
    int rel_len = len;
    while (rel_len > 2 && rel[0] == '.' && rel[1] == '/')
    {
        rel += 2;
        rel_len -= 2;
    }

    // `std` lives outside the project and is reserved: no file is looked for, so importing
    // it costs no system call.
    static const char *const external_roots[] = {"std"};
    for (size_t i = 0; i < sizeof(external_roots) / sizeof(external_roots[0]); i++)
    {
        if (!relative && strlen(external_roots[i]) == (size_t)len &&
            memcmp(external_roots[i], name, (size_t)len) == 0)
        {
            return NULL;
        }
    }

    // A path (`./x`, `a/b`, `x.modal`) is looked for next to the importing file only. A bare
    // name is a module or a domain: it is looked for next to the file, then in each folder
    // above it, so `import("example")` means the same anywhere inside the project, including
    // inside `example/` itself, where it finds the domain's own mod.modal.
    char path[PATH_MAX];
    const char *const forms[] = {"%.*s/%.*s", "%.*s/%.*s.modal", "%.*s/%.*s/mod.modal"};
    int bare = !relative && !has_ext && !memchr(name, '/', (size_t)len);
    int up_len = dir_len;
    for (;;)
    {
        for (int i = has_ext ? 0 : 1; i < (has_ext ? 1 : 3); i++)
        {
            int n = snprintf(path, sizeof(path), forms[i], up_len, dir, rel_len, rel);
            if (n > 0 && (size_t)n < sizeof(path) && file_exists(path))
            {
                Unit *target = unit_load(path);
                if (target)
                {
                    return target;
                }
            }
        }
        if (!bare || !slash)
        {
            break;
        }
        while (up_len > 0 && dir[up_len - 1] != '/') // one folder up
        {
            up_len--;
        }
        if (up_len <= 1)
        {
            break;
        }
        up_len--; // drop the '/'
    }

    // Nothing was found. An unresolved import is always an error, so a mistyped or
    // unreachable path can never pass unchecked.
    if (has_ext)
    {
        diag_at(u, NODE(u, import)->tok, "error", "cannot find module %.*s (looked for %.*s/%.*s)",
                arg->len, arg->start, dir_len, dir, rel_len, rel);
    }
    else
    {
        diag_at(u, NODE(u, import)->tok, "error",
                "cannot find module %.*s (looked for %.*s/%.*s.modal and %.*s/%.*s/mod.modal%s)",
                arg->len, arg->start, dir_len, dir, rel_len, rel, dir_len, dir, rel_len, rel,
                bare ? ", and in the folders above" : "");
    }
    return NULL;
}

// The import chain a top-level declaration carries, if any.
static NodeId decl_chain(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    NodeId expr = n->kind == AST_DEFINE ? n->a : n->kind == AST_VAL_DECL ? n->b : decl;
    return ast_import_root(u, expr) ? expr : 0;
}

// ---- location rules --------------------------------------------------------------------------

// Like Go's internal/: a domain can be imported from its parent folder and from anything
// below that parent. Further out, it is reached only through the parent's mod.modal. And a
// file of another domain can be imported directly only if its domain's mod.modal binds it.
// A file sees its own domain and every domain above it, as a child sees its ancestors.
static int sees_inside(const Unit *u, const Unit *target)
{
    const char *tdir = cx.domains[target->domain];
    const char *udir = cx.domains[u->domain];
    size_t tlen = strlen(tdir);
    return strncmp(udir, tdir, tlen) == 0 && (udir[tlen] == '/' || udir[tlen] == '\0');
}

static int may_import(Unit *u, uint32_t tok, const Unit *target)
{
    if (sees_inside(u, target))
    {
        return 1;
    }

    const char *tdir = cx.domains[target->domain];
    const char *udir = cx.domains[u->domain];
    const char *slash = strrchr(tdir, '/');
    size_t plen = slash ? (size_t)(slash - tdir) : 0;

    if (strncmp(udir, tdir, plen) != 0 || (udir[plen] != '/' && udir[plen] != '\0'))
    {
        const char *parent_end = tdir + plen;
        const char *parent = parent_end;
        while (parent > tdir && parent[-1] != '/')
        {
            parent--;
        }
        diag_at(u, tok, "error",
                "domain '%s' is internal to '%.*s'. From here, reach it through %.*s/mod.modal",
                domain_name(target->domain), (int)(parent_end - parent), parent,
                (int)(parent_end - parent), parent);
        return 0;
    }
    if (!target->is_manifest && !target->bound)
    {
        diag_at(u, tok, "error",
                "module '%.*s' is not part of the face of domain '%s'. To share it, bind it in "
                "%s/mod.modal: `%.*s := import(\"./%.*s\")`",
                target->name_len, target->name, domain_name(target->domain),
                domain_name(target->domain), target->name_len, target->name, target->name_len,
                target->name);
        return 0;
    }
    return 1;
}

// ---- chain evaluation ------------------------------------------------------------------------

// Where a binding leads: `john := import("./john")` leads into john's module scope.
static Target follow_import(SymId id, int depth)
{
    const Symbol *s = SYM(id);
    Unit *owner = &cx.units[s->unit];
    const AstNode *decl = NODE(owner, s->decl);

    // `a := import(x).y` and also the aliases built on it: `b := a.z`, `c := b`.
    if (decl->kind != AST_DEFINE || depth >= FOLLOW_MAX)
    {
        return (Target){0};
    }
    return chain_eval(owner, decl->a, CHAIN_SILENT, depth + 1);
}

// The inside / outside rule, applied to one name. Returns 0 after reporting a violation.
static int may_see(Unit *u, uint32_t tok, SymId id)
{
    const Token *name = TOK(u, tok);
    const Symbol *s = SYM(id);
    Unit *owner = &cx.units[s->unit];

    if (s->owner) // a struct member: only the struct's face can be named from elsewhere
    {
        int tlen;
        const char *type = symbol_name(s->owner, &tlen);
        if (s->kind == SYM_FIELD)
        {
            diag_at(u, tok, "error",
                    "'%.*s' is a field of '%.*s', and fields are never visible outside their "
                    "struct. Add an accessor below it: `T %.*s(self: %.*s): self.%.*s`",
                    name->len, name->start, tlen, type, name->len, name->start, tlen, type,
                    name->len, name->start);
            return 0;
        }
        if (!(s->flags & SYM_F_EXPOSED))
        {
            diag_at(u, tok, "error",
                    "'%.*s' is inside '%.*s'. To use it here, move it below the closing brace "
                    "of the struct",
                    name->len, name->start, tlen, type);
            return 0;
        }
        return 1;
    }

    if (sees_inside(u, owner))
    {
        return 1; // files of one domain share everything at module level
    }
    if (owner->is_manifest)
    {
        if (s->kind == SYM_CONST)
        {
            diag_at(u, tok, "error", "'%.*s' is configuration of domain '%s', not part of its face",
                    name->len, name->start, domain_name(owner->domain));
            return 0;
        }
        return 1; // what mod.modal binds or defines is the domain's face
    }
    if (s->kind != SYM_STRUCT) // a type travels as an opaque type; nothing else leaves the file
    {
        diag_at(u, tok, "error",
                "'%.*s' is inside module '%.*s', private to domain '%s'. To share it, define "
                "it in %s/mod.modal",
                name->len, name->start, owner->name_len, owner->name, domain_name(owner->domain),
                domain_name(owner->domain));
        return 0;
    }
    return 1;
}

// Resolves one name of a chain inside `cur`.
static Target chain_step(Unit *u, uint32_t tok, Target cur, ChainMode mode, int depth)
{
    const Token *name = TOK(u, tok);
    const char *container = cur.unit->name;
    int container_len = cur.unit->name_len;

    if (cur.scope && cur.scope->owner)
    {
        container = symbol_name(cur.scope->owner, &container_len);
    }

    SymId id = cur.scope ? scope_find(cur.scope, name->start, (size_t)name->len) : 0;
    if (!id)
    {
        if (mode == CHAIN_CHECK)
        {
            diag_at(u, tok, "error", "'%.*s' has no member '%.*s'", container_len, container,
                    name->len, name->start);
        }
        return (Target){0};
    }
    if (mode == CHAIN_CHECK && !may_see(u, tok, id))
    {
        return (Target){0};
    }

    Unit *owner = &cx.units[SYM(id)->unit];
    switch (SYM(id)->kind)
    {
    case SYM_STRUCT:
        return (Target){owner, scope_at(SYM(id)->scope_ref), 1};
    case SYM_IMPORT:
        return follow_import(id, depth);
    default:
        return (Target){owner, NULL, 1}; // a field, method or constant: nothing below it
    }
}

static void chain_items(Unit *u, NodeId item, Target cur, ChainMode mode, int depth)
{
    for (; item; item = NODE(u, item)->next)
    {
        chain_step(u, NODE(u, item)->tok, cur, mode, depth);
    }
}

static int is_chain_segment(uint8_t kind)
{
    return kind == AST_SELECTOR || kind == AST_IMPORT_ALL || kind == AST_IMPORT_PICK;
}

// Walks a chain from its base outwards. The base is either an import, as in
// import(x).a.b / .* / .(c, d), or a name bound to one, as in `ex.a.b` after
// `ex := import(x)`: member access gets exactly the same checks as a direct import.
static Target chain_eval(Unit *u, NodeId chain, ChainMode mode, int depth)
{
    NodeId segs[CHAIN_MAX];
    int n = 0;
    NodeId id = chain;

    while (is_chain_segment(NODE(u, id)->kind))
    {
        if (n == CHAIN_MAX)
        {
            if (mode == CHAIN_CHECK)
            {
                diag_at(u, NODE(u, id)->tok, "error", "import chain is too long");
            }
            return (Target){0};
        }
        segs[n++] = id;
        id = NODE(u, id)->a;
    }

    const AstNode *base = NODE(u, id);
    Target cur = {0};

    if (base->kind == AST_IMPORT)
    {
        uint16_t target = base->aux;
        if (!target || cx.units[target - 1].broken)
        {
            return (Target){0}; // external module, or one that did not compile
        }
        cur = (Target){&cx.units[target - 1], cx.units[target - 1].scope, 1};
        if (mode == CHAIN_CHECK && !may_import(u, base->tok, cur.unit))
        {
            return (Target){0};
        }
    }
    else if (base->kind == AST_IDENT)
    {
        const Token *name = TOK(u, base->tok);
        SymId binding = scope_find(u->scope, name->start, (size_t)name->len);
        if (!binding || (SYM(binding)->kind != SYM_IMPORT && SYM(binding)->kind != SYM_CONST))
        {
            return (Target){0}; // a local value, `self`, ...: not a module path
        }
        cur = follow_import(binding, depth);
    }
    else
    {
        return (Target){0};
    }

    while (n > 0 && cur.known)
    {
        const AstNode *seg = NODE(u, segs[--n]);
        switch (seg->kind)
        {
        case AST_SELECTOR:
            cur = chain_step(u, seg->tok, cur, mode, depth);
            break;
        case AST_IMPORT_PICK:
            chain_items(u, seg->b, cur, mode, depth);
            return cur;
        default: // AST_IMPORT_ALL: brings in whatever is visible
            return cur;
        }
    }
    return cur;
}

// Checks every module path in a file, wherever it is written: top level or inside a body,
// spelled as an import or as member access on a binding.
//
// The node array is scanned linearly, not walked as a tree: a chain is checked once, from
// its outermost node, and a first pass marks which chain nodes sit inside another (one bit
// per node) so the second can tell. Two sequential sweeps over a flat array are several
// times cheaper than a traversal, and a file with no import is skipped outright, since no
// name in it can lead to another module.
void module_check_paths(Unit *u, NodeId from, NodeId to)
{
    if (!u->nimports || from >= to)
    {
        return;
    }

    ArenaMark mark = arena_mark();
    size_t words = (to - from + 63) / 64;
    uint64_t *inner = (uint64_t *)arena_alloc(words * sizeof(uint64_t));
    memset(inner, 0, words * sizeof(uint64_t));

    for (NodeId id = from; id < to; id++)
    {
        const AstNode *n = NODE(u, id);
        NodeId child = is_chain_segment(n->kind) ? n->a : n->kind == AST_VAL_DECL ? n->b : 0;
        if (child >= from) // a chain's inner nodes are created just before it
        {
            inner[(child - from) >> 6] |= (uint64_t)1 << ((child - from) & 63);
        }
    }

    for (NodeId id = from; id < to; id++)
    {
        const AstNode *n = NODE(u, id);
        if (n->kind == AST_VAL_DECL)
        {
            Target end = chain_eval(u, n->b, CHAIN_CHECK, 0);
            if (end.known)
            {
                chain_items(u, n->a, end, CHAIN_CHECK, 0);
            }
        }
        else if ((n->kind == AST_IMPORT || is_chain_segment(n->kind)) &&
                 !(inner[(id - from) >> 6] >> ((id - from) & 63) & 1))
        {
            NodeId base = id;
            while (is_chain_segment(NODE(u, base)->kind))
            {
                base = NODE(u, base)->a;
            }
            uint8_t kind = NODE(u, base)->kind;
            if (kind == AST_IMPORT || kind == AST_IDENT)
            {
                chain_eval(u, id, CHAIN_CHECK, 0);
            }
        }
    }
    arena_release(mark);
}

// Finds the file behind every import among the nodes [from, to) of a unit. Returns how many
// imports were found.
static uint32_t discover_imports(Unit *u, NodeId from, NodeId to)
{
    uint32_t found = 0;
    for (NodeId id = from; id < to; id++)
    {
        if (NODE(u, id)->kind == AST_IMPORT)
        {
            Unit *target = import_target(u, id);
            NODE(u, id)->aux = target ? (uint16_t)(target->id + 1) : 0;
            found++;
        }
    }
    u->nimports += found;
    return found;
}

// ---- mod.modal: configuration and the face of the domain ----------------------------------------

typedef enum
{
    CONFIG_BOOL,
    CONFIG_STRING,
} ConfigKind;

// The closed set of behaviour switches. A name outside it is an error, so a typo never
// silently does nothing.
static const struct
{
    const char *name;
    ConfigKind kind;
} config_keys[] = {
    {"implicit_self", CONFIG_BOOL}, // may `n1` stand for `self.n1` in a method body
    {"nil_return", CONFIG_BOOL},    // the compiler adds the implicit nil to return types
    {"profile", CONFIG_STRING},     // which function declaration forms the domain accepts
    {"version", CONFIG_STRING},     // version of the domain's contract
};

// In a manifest, `name := <literal>` is configuration; `name := import(...)` binds a module.
static int is_config(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    return n->kind == AST_DEFINE && NODE(u, n->a)->kind != AST_FN_LIT && !ast_import_root(u, n->a);
}

static void check_config(Unit *u, NodeId decl)
{
    const Token *name = TOK(u, NODE(u, decl)->tok);
    const AstNode *value = NODE(u, NODE(u, decl)->a);
    const Token *vt = TOK(u, value->tok);

    for (size_t i = 0; i < sizeof(config_keys) / sizeof(config_keys[0]); i++)
    {
        if (strlen(config_keys[i].name) != (size_t)name->len ||
            memcmp(config_keys[i].name, name->start, (size_t)name->len) != 0)
        {
            continue;
        }
        int is_bool =
            value->kind == AST_IDENT && ((vt->len == 4 && !memcmp(vt->start, "true", 4)) ||
                                         (vt->len == 5 && !memcmp(vt->start, "false", 5)));
        int ok = config_keys[i].kind == CONFIG_BOOL ? is_bool : value->kind == AST_STRING_LIT;
        if (!ok)
        {
            diag_at(u, value->tok, "error", "'%.*s' expects %s", name->len, name->start,
                    config_keys[i].kind == CONFIG_BOOL ? "true or false" : "a string");
        }
        return;
    }
    diag_at(u, NODE(u, decl)->tok, "error",
            "'%.*s' is not a known configuration of a domain (known: implicit_self, nil_return, "
            "profile, version)",
            name->len, name->start);
}

// Reads one mod.modal: validates its configuration and marks the modules it binds.
static void read_manifest(Unit *m)
{
    for (NodeId d = NODE(m, m->root)->a; d; d = NODE(m, d)->next)
    {
        if (is_config(m, d))
        {
            check_config(m, d);
            continue;
        }
        NodeId chain = decl_chain(m, d);
        uint16_t target = chain ? NODE(m, ast_import_root(m, chain))->aux : 0;
        if (target && cx.units[target - 1].domain == m->domain)
        {
            cx.units[target - 1].bound = 1;
        }
    }
}

int domain_config_bool(uint16_t domain, const char *key, int fallback)
{
    size_t klen = strlen(key);
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        const Unit *m = &cx.units[i];
        if (!m->is_manifest || m->domain != domain || m->broken)
        {
            continue;
        }
        for (NodeId d = NODE(m, m->root)->a; d; d = NODE(m, d)->next)
        {
            const Token *name = TOK(m, NODE(m, d)->tok);
            if (is_config(m, d) && name->len == klen && memcmp(name->start, key, klen) == 0)
            {
                const Token *v = TOK(m, NODE(m, NODE(m, d)->a)->tok);
                return v->len == 4 && memcmp(v->start, "true", 4) == 0;
            }
        }
    }
    return fallback;
}

int module_wildcard_provides(Unit *u, const char *name, size_t len)
{
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        NodeId chain = decl_chain(u, d);
        if (!chain || NODE(u, chain)->kind != AST_IMPORT_ALL)
        {
            continue;
        }
        Target t = chain_eval(u, chain, CHAIN_SILENT, 0);
        if (!t.known || !t.scope)
        {
            return 1; // an external module: its names cannot be listed, so it may provide it
        }
        if (scope_find(t.scope, name, len))
        {
            return 1;
        }
    }
    return 0;
}

// The derived contract: what other domains can rely on. Nobody writes this by hand.
void contract_print(void)
{
    char sig[256];

    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        Unit *m = &cx.units[i];
        if (!m->is_manifest)
        {
            continue;
        }
        printf("== contract: %s  (%s)\n", domain_name(m->domain), m->path);

        for (NodeId d = NODE(m, m->root)->a; d; d = NODE(m, d)->next)
        {
            const AstNode *n = NODE(m, d);
            if (is_config(m, d))
            {
                const Token *k = TOK(m, n->tok), *v = TOK(m, NODE(m, n->a)->tok);
                printf("config %.*s = %.*s\n", k->len, k->start, v->len, v->start);
            }
            else if (n->kind == AST_METHOD_DECL || (n->kind == AST_DEFINE && !decl_chain(m, d)))
            {
                printf("%s\n", ast.sig_str(m, d, sig, sizeof(sig)));
            }
        }

        for (uint16_t j = 0; j < cx.nunits; j++)
        {
            Unit *u = &cx.units[j];
            if (u->domain != m->domain || !u->bound)
            {
                continue;
            }
            printf("module %.*s\n", u->name_len, u->name);
            for (SymId id = 1; id < cx.syms.size; id++)
            {
                const Symbol *s = SYM(id);
                int len;
                const char *name = symbol_name(id, &len);
                if (s->unit != u->id)
                {
                    continue;
                }
                if (s->kind == SYM_STRUCT && !s->owner)
                {
                    printf("  type %.*s\n", len, name); // opaque: no fields in the contract
                }
                else if (s->owner && (s->flags & SYM_F_EXPOSED))
                {
                    printf("  %s\n", ast.sig_str(u, s->decl, sig, sizeof(sig)));
                }
            }
        }
        putchar('\n');
    }
}

// ---- linking ---------------------------------------------------------------------------------

// Brings every unit loaded so far up to date: its domain's mod.modal is loaded, its imports
// are resolved (which may load more units, handled by the same loop), and if it is a
// manifest its configuration and bindings are read. Called again whenever a function body
// turns out to contain an import.
static void link_new_units(void)
{
    while (cx.linked < cx.nunits)
    {
        Unit *u = &cx.units[cx.linked++];
        uint64_t t = clock_ns();
        uint64_t loading =
            cx.phase_ns[PHASE_SCAN] + cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_DECLARE];

        if (!u->is_manifest)
        {
            char manifest[PATH_MAX];
            const char *slash = strrchr(u->path, '/');
            int n = snprintf(manifest, sizeof(manifest), "%.*s/mod.modal",
                             slash ? (int)(slash - u->path) : 1, slash ? u->path : ".");
            if (n > 0 && (size_t)n < sizeof(manifest) && file_exists(manifest))
            {
                unit_load(manifest);
            }
        }
        discover_imports(u, 1, (NodeId)u->nodes.size);
        if (u->is_manifest && !u->broken)
        {
            read_manifest(u);
        }

        // loading an imported file is accounted to its own scan/parse/declare
        cx.phase_ns[PHASE_LINK] += clock_ns() - t -
                                   (cx.phase_ns[PHASE_SCAN] + cx.phase_ns[PHASE_PARSE] +
                                    cx.phase_ns[PHASE_DECLARE] - loading);
    }
}

void module_body_nodes(Unit *u, NodeId from)
{
    if (discover_imports(u, from, (NodeId)u->nodes.size))
    {
        link_new_units(); // an import inside a body: its file must be linked before use
    }
    module_check_paths(u, from, (NodeId)u->nodes.size);
}

void modules_link(void)
{
    link_new_units();

    // Each file: the module paths of its declarations, then its bodies one at a time.
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        Unit *u = &cx.units[i];
        if (u->broken)
        {
            sema_syntax_only(u); // still report the syntax errors inside its bodies
            continue;
        }
        uint64_t t = clock_ns();
        module_check_paths(u, 1, (NodeId)u->nodes.size);
        t = phase_end(PHASE_LINK, t);

        uint64_t inside = cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_EMIT];
        sema_check(u);
        // bodies parsed and emitted during the check are accounted to their own phases
        cx.phase_ns[PHASE_CHECK] +=
            clock_ns() - t - (cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_EMIT] - inside);
    }
}
`````

### `src/v2/compiler/sema/resolve.c`

`````c
// resolve.c (v2): declaration pass. Turns the declarations of one unit into symbols and scopes.
//
// Rules enforced here:
//   - a struct is a domain: its fields and methods live in its own scope
//   - a top-level callable whose first parameter is `self: T` belongs to struct T of this
//     module; with a bare `self` (or `self: <module>`) it belongs to the module itself
//   - `name(...): T { }` and `name := (...) T { }` are defined once
//   - `T name(...)` may be defined again with different parameter types (an overload set)
//   - inside / outside: what is declared inside a struct's braces is private to the struct;
//     a method attached from outside with `self: T` is the struct's public face
// Bodies are not resolved yet; that needs types and is the next phase.
#include "v2/compiler/cx.h"
#include <string.h>

static void redeclared(Unit *u, uint32_t tok, SymId previous, const char *hint)
{
    const Token *t = TOK(u, tok);
    int line, col;
    source_pos(u->src, TOK(u, SYM(previous)->tok)->start, &line, &col);
    diag_at(u, tok, "error", "'%.*s' is already declared at line %d%s", t->len, t->start, line,
            hint);
}

// ---- signatures ------------------------------------------------------------------------------

NodeId sema_callable_params(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    if (n->kind == AST_DEFINE)
    {
        n = NODE(u, n->a); // name := (params) T { }
    }
    return n->a;
}

static int type_equal(const Unit *u, NodeId a, NodeId b)
{
    while (a && b)
    {
        const AstNode *x = NODE(u, a);
        const AstNode *y = NODE(u, b);
        if (x->kind != y->kind)
        {
            return 0;
        }
        if (x->kind == AST_TYPE_NAME)
        {
            const Token *xf = TOK(u, x->tok), *xl = TOK(u, x->tok + 2u * x->aux);
            const Token *yf = TOK(u, y->tok), *yl = TOK(u, y->tok + 2u * y->aux);
            size_t xn = (size_t)((xl->start + xl->len) - xf->start);
            size_t yn = (size_t)((yl->start + yl->len) - yf->start);
            return xn == yn && memcmp(xf->start, yf->start, xn) == 0;
        }
        if (x->kind == AST_TYPE_STRUCT)
        {
            return a == b;
        }
        a = x->a;
        b = y->a;
    }
    return a == b;
}

static NodeId skip_self(const Unit *u, NodeId param)
{
    if (param)
    {
        const Token *t = TOK(u, NODE(u, param)->tok);
        if (is_self_token(t->start, t->len))
        {
            return NODE(u, param)->next;
        }
    }
    return param;
}

// Two overloads collide when their parameter types match one by one (self aside).
static int same_signature(const Unit *u, NodeId pa, NodeId pb)
{
    pa = skip_self(u, pa);
    pb = skip_self(u, pb);
    while (pa && pb)
    {
        if (!type_equal(u, NODE(u, pa)->a, NODE(u, pb)->a))
        {
            return 0;
        }
        pa = NODE(u, pa)->next;
        pb = NODE(u, pb)->next;
    }
    return pa == pb;
}

// ---- declarations ----------------------------------------------------------------------------

static void declare_value(Unit *u, Scope *scope, SymId owner, SymKind kind, uint8_t flags,
                          uint32_t tok, NodeId decl)
{
    const Token *t = TOK(u, tok);
    SymId previous = scope_find(scope, t->start, (size_t)t->len);
    if (previous)
    {
        redeclared(u, tok, previous, "");
        return;
    }
    scope_insert(scope, symbol_new(u, kind, flags, tok, owner, decl));
}

// `face` is SYM_F_EXPOSED for a method written outside its struct's braces, 0 otherwise.
static void declare_callable(Unit *u, Scope *scope, SymId owner, uint32_t tok, NodeId decl,
                             int overloadable, uint8_t face)
{
    const Token *t = TOK(u, tok);
    uint8_t flags = (uint8_t)((overloadable ? SYM_F_MUT : 0) | face);

    SymId first = scope_find(scope, t->start, (size_t)t->len);
    if (!first)
    {
        scope_insert(scope, symbol_new(u, SYM_METHOD, flags, tok, owner, decl));
        return;
    }

    if (SYM(first)->kind == SYM_FIELD && face)
    {
        // An accessor may carry the name of the field it reads: `u8 n1(self: T): self.n1`.
        // From outside only the method can be seen, so the name is never ambiguous there.
        // It is inserted in front of the field, which is what a lookup then finds.
        scope_insert(scope, symbol_new(u, SYM_METHOD, flags, tok, owner, decl));
        return;
    }
    if (SYM(first)->kind != SYM_METHOD)
    {
        redeclared(u, tok, first, "");
        return;
    }
    if (!overloadable || !(SYM(first)->flags & SYM_F_MUT))
    {
        redeclared(u, tok, first,
                   " (only the type-first form `T name(...)` can be overloaded; "
                   "`name(...): T` and `name := (...)` are defined once)");
        return;
    }

    SymId last = first;
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        if (same_signature(u, sema_callable_params(u, SYM(s)->decl), sema_callable_params(u, decl)))
        {
            redeclared(u, tok, s, " with the same parameter types");
            return;
        }
        last = s;
    }
    // Later overloads hang off the first one; only the first sits in the hash bucket.
    SymId added = symbol_new(u, SYM_METHOD, flags, tok, owner, decl);
    SYM(last)->next_overload = added;
}

// Where does a top-level callable live? Returns the scope and sets *owner, or NULL on error.
static Scope *callable_home(Unit *u, NodeId params, SymId *owner)
{
    *owner = 0;
    if (!params)
    {
        return u->scope;
    }

    const AstNode *first = NODE(u, params);
    const Token *name = TOK(u, first->tok);
    if (!is_self_token(name->start, name->len) || !first->a)
    {
        return u->scope; // no self, or a bare `self`: the module owns it
    }

    const AstNode *type = NODE(u, first->a);
    if (type->kind == AST_TYPE_NAME && type->aux == 0)
    {
        const Token *t = TOK(u, type->tok);
        const char *domain = domain_name(u->domain);

        // A struct wins over the module when they share a name (counter.modal declaring
        // `counter: struct`): the file exists to hold that type.
        SymId target = scope_find(u->scope, t->start, (size_t)t->len);
        if (target && SYM(target)->kind == SYM_STRUCT)
        {
            *owner = target;
            return scope_at(SYM(target)->scope_ref);
        }

        if (is_self_token(t->start, t->len) ||
            (t->len == u->name_len && memcmp(t->start, u->name, (size_t)t->len) == 0) ||
            (strlen(domain) == (size_t)t->len && memcmp(t->start, domain, (size_t)t->len) == 0))
        {
            return u->scope; // `self: <module>` spelled out
        }
    }

    diag_at(u, type->tok, "error",
            "the type of `self` must be this module or a struct declared in this file: a "
            "method can only be attached to a type of its own domain");
    return NULL;
}

// Inside a struct body, `self` may be left bare or typed with the struct's own name.
static void check_member_self(Unit *u, SymId owner, NodeId params)
{
    if (!params || !NODE(u, params)->a)
    {
        return;
    }
    const AstNode *first = NODE(u, params);
    const Token *name = TOK(u, first->tok);
    if (!is_self_token(name->start, name->len))
    {
        return;
    }

    const AstNode *type = NODE(u, first->a);
    const Token *t = TOK(u, type->tok);
    int olen;
    const char *oname = symbol_name(owner, &olen);
    if (type->kind == AST_TYPE_NAME && type->aux == 0 &&
        (is_self_token(t->start, t->len) ||
         (t->len == olen && memcmp(t->start, oname, (size_t)olen) == 0)))
    {
        return;
    }
    diag_at(u, type->tok, "error", "inside '%.*s', `self` can only be typed as '%.*s'", olen, oname,
            olen, oname);
}

static void declare_define(Unit *u, Scope *scope, SymId owner, NodeId decl, int top_level)
{
    const AstNode *n = NODE(u, decl);
    uint32_t tok = n->tok;
    NodeId value = n->a;

    if (NODE(u, value)->kind == AST_FN_LIT)
    {
        NodeId params = NODE(u, value)->a;
        if (top_level)
        {
            scope = callable_home(u, params, &owner);
            if (!scope)
            {
                return;
            }
        }
        else
        {
            check_member_self(u, owner, params);
        }
        declare_callable(u, scope, owner, tok, decl, 0, (top_level && owner) ? SYM_F_EXPOSED : 0);
        return;
    }

    SymKind kind = ast_import_root(u, value) ? SYM_IMPORT : SYM_CONST;
    declare_value(u, scope, owner, kind, 0, tok, decl);
}

static void declare_import_items(Unit *u, NodeId item)
{
    for (; item; item = NODE(u, item)->next)
    {
        const AstNode *n = NODE(u, item);
        declare_value(u, u->scope, 0, SYM_IMPORT, 0, n->c ? n->c : n->tok, item);
    }
}

// A scope's hash table is sized once, from the number of declarations it will hold, so
// lookups stay O(1) in a file with thousands of declarations and small scopes stay small.
static uint32_t bucket_count(const Unit *u, NodeId first, uint32_t minimum)
{
    uint32_t n = 0, buckets = minimum;
    for (NodeId d = first; d; d = NODE(u, d)->next)
    {
        n++;
    }
    while (buckets < n)
    {
        buckets *= 2;
    }
    return buckets;
}

// Nesting is bounded by the parser's depth limit, so this recursion is too.
static void declare_struct(Unit *u, Scope *scope, SymId owner, NodeId decl)
{
    uint32_t tok = NODE(u, decl)->tok;
    const Token *t = TOK(u, tok);
    SymId previous = scope_find(scope, t->start, (size_t)t->len);
    if (previous)
    {
        redeclared(u, tok, previous, "");
        return;
    }

    SymId self = symbol_new(u, SYM_STRUCT, 0, tok, owner, decl);
    Scope *members =
        scope_new(scope, self, bucket_count(u, NODE(u, decl)->a, SCOPE_STRUCT_BUCKETS));
    SYM(self)->scope_ref = scope_ref(members);
    scope_insert(scope, self);

    for (NodeId m = NODE(u, decl)->a; m; m = NODE(u, m)->next)
    {
        const AstNode *n = NODE(u, m);
        switch (n->kind)
        {
        case AST_STRUCT_DECL:
            declare_struct(u, members, self, m);
            break;
        case AST_FIELD_DECL:
            declare_value(u, members, self, SYM_FIELD, (n->flags & AST_F_MUT) ? SYM_F_MUT : 0,
                          n->tok, m);
            break;
        case AST_METHOD_DECL:
            check_member_self(u, self, n->a);
            declare_callable(u, members, self, n->tok, m, n->flags & AST_F_MUT, 0); // inside
            break;
        case AST_DEFINE:
            declare_define(u, members, self, m, 0);
            break;
        default:
            break;
        }
    }
}

void sema_declare(Unit *u)
{
    u->scope = scope_new(NULL, 0, bucket_count(u, NODE(u, u->root)->a, SCOPE_MODULE_BUCKETS));

    // Pass 1: structs, so that `name(self: T)` can attach to a T declared further down.
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            declare_struct(u, u->scope, 0, d);
        }
    }

    // Pass 2: everything else at the top level.
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        const AstNode *n = NODE(u, d);
        switch (n->kind)
        {
        case AST_FIELD_DECL:
            declare_value(u, u->scope, 0, SYM_CONST, (n->flags & AST_F_MUT) ? SYM_F_MUT : 0, n->tok,
                          d);
            break;
        case AST_METHOD_DECL:
        {
            SymId owner;
            Scope *home = callable_home(u, n->a, &owner);
            if (home)
            {
                declare_callable(u, home, owner, n->tok, d, n->flags & AST_F_MUT,
                                 owner ? SYM_F_EXPOSED : 0); // outside the braces: the face
            }
            break;
        }
        case AST_DEFINE:
            declare_define(u, u->scope, 0, d, 1);
            break;
        case AST_VAL_DECL:
            declare_import_items(u, n->a);
            break;
        case AST_IMPORT_PICK:
            declare_import_items(u, n->b); // bare import(x).(a, b as c)
            break;
        default:
            break;
        }
    }
}
`````

### `src/v2/compiler/sema/types.c`

`````c
#include "v2/compiler/sema/types.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <string.h>

// Interning table: open addressing over (kind, ref), holding TypeIds. 0 = empty slot.
static uint32_t *type_map;
static size_t type_map_cap;

static size_t type_slot(uint8_t kind, uint32_t ref, size_t cap)
{
    return (size_t)((ref * 2654435761u) ^ ((uint32_t)kind * 40503u)) & (cap - 1);
}

static void type_map_put(TypeId id)
{
    const Type *t = &cx.types.data[id];
    size_t i = type_slot(t->kind, t->ref, type_map_cap);
    while (type_map[i])
    {
        i = (i + 1) & (type_map_cap - 1);
    }
    type_map[i] = id;
}

static TypeId type_push(uint8_t kind, uint32_t ref)
{
    if (cx.types.size == cx.types.cap)
    {
        cx.types.data = (Type *)arena_grow(cx.types.data, sizeof(Type), &cx.types.cap);
    }
    cx.types.data[cx.types.size] = (Type){kind, ref};
    return (TypeId)cx.types.size++;
}

void types_init(void)
{
    for (uint8_t k = 0; k < T__PRIM_COUNT; k++)
    {
        type_push(k, 0); // a primitive's id is its kind
    }
    type_map_cap = 64;
    type_map = (uint32_t *)arena_alloc(type_map_cap * sizeof(uint32_t));
    memset(type_map, 0, type_map_cap * sizeof(uint32_t));
}

TypeId type_make(TypeKind kind, uint32_t ref)
{
    if (kind < T__PRIM_COUNT)
    {
        return (TypeId)kind;
    }
    for (size_t i = type_slot((uint8_t)kind, ref, type_map_cap); type_map[i];
         i = (i + 1) & (type_map_cap - 1))
    {
        const Type *t = &cx.types.data[type_map[i]];
        if (t->kind == kind && t->ref == ref)
        {
            return type_map[i];
        }
    }

    TypeId id = type_push((uint8_t)kind, ref);
    if ((cx.types.size - T__PRIM_COUNT) * 2 > type_map_cap) // keep the table under half full
    {
        type_map_cap *= 2;
        type_map = (uint32_t *)arena_alloc(type_map_cap * sizeof(uint32_t));
        memset(type_map, 0, type_map_cap * sizeof(uint32_t));
        for (TypeId t = T__PRIM_COUNT; t < cx.types.size; t++)
        {
            type_map_put(t);
        }
    }
    else
    {
        type_map_put(id);
    }
    return id;
}

TypeKind type_kind(TypeId t)
{
    return (TypeKind)cx.types.data[t].kind;
}

uint32_t type_ref(TypeId t)
{
    return cx.types.data[t].ref;
}

int type_is_int(TypeId t)
{
    return t >= T_INT_LIT && t <= T_INT;
}

char *type_str(TypeId t, char *buf, size_t cap)
{
    static const char *const prims[T__PRIM_COUNT] = {
        [T_NONE] = "?",        [T_VOID] = "nothing", [T_BOOL] = "bool", [T_NIL] = "nil",
        [T_INT_LIT] = "int",   [T_U8] = "u8",        [T_U16] = "u16",   [T_U32] = "u32",
        [T_U64] = "u64",       [T_I8] = "i8",        [T_I16] = "i16",   [T_I32] = "i32",
        [T_I64] = "i64",       [T_INT] = "int",      [T_STR] = "[]u8",  [T_EXTERN] = "external",
        [T_MODULE] = "module",
    };
    size_t n = 0;

    buf[0] = '\0';
    for (;;)
    {
        TypeKind k = type_kind(t);
        if (k < T__PRIM_COUNT)
        {
            snprintf(buf + n, cap - n, "%s", prims[k]);
            return buf;
        }
        if (k == T_PTR || k == T_SLICE)
        {
            n += (size_t)snprintf(buf + n, cap - n, "%s", k == T_PTR ? "*" : "[]");
            if (n >= cap)
            {
                return buf;
            }
            t = type_ref(t);
            continue;
        }
        int len;
        const char *name = symbol_name(type_ref(t), &len);
        snprintf(buf + n, cap - n, k == T_FUNC ? "function %.*s" : "%.*s", len, name);
        return buf;
    }
}
`````

### `src/v2/compiler/sema/types.h`

`````c
// types.h (v2): the type table.
//
// A type is 8 bytes and is referred to by a 32-bit TypeId. Primitive types have fixed ids;
// struct, pointer and slice types are interned, so two equal types always have the same id
// and comparing types is comparing integers.
#ifndef MODAL_V2_TYPES_H
#define MODAL_V2_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t TypeId;

typedef enum
{
    T_NONE, // not known: an error was already reported, or the expression has no type
    T_VOID,
    T_BOOL,
    T_NIL,
    T_INT_LIT, // an integer literal: becomes whatever integer type it meets
    T_U8,
    T_U16,
    T_U32,
    T_U64,
    T_I8,
    T_I16,
    T_I32,
    T_I64,
    T_INT,
    T_STR,    // []u8
    T_EXTERN, // comes from outside the checked program (std): anything is allowed on it
    T_MODULE, // `self` of a module-level function
    T__PRIM_COUNT,
    // interned: `ref` says of what
    T_STRUCT = T__PRIM_COUNT, // ref = struct symbol
    T_PTR,                    // ref = pointee type
    T_SLICE,                  // ref = element type
    T_FUNC,                   // ref = function symbol (first of its overload set)
    T_TYPE,                   // ref = struct symbol: the type itself used as a value, `T.new()`
} TypeKind;

typedef struct
{
    uint8_t kind;
    uint32_t ref;
} Type;

typedef struct
{
    Type *data;
    size_t size;
    size_t cap;
} TypeVec;

void types_init(void);
TypeId type_make(TypeKind kind, uint32_t ref);
TypeKind type_kind(TypeId t);
uint32_t type_ref(TypeId t);
int type_is_int(TypeId t); // a sized integer or an integer literal
// Source-like spelling of a type, for diagnostics; returns buf.
char *type_str(TypeId t, char *buf, size_t cap);

#endif // MODAL_V2_TYPES_H
`````

### `src/v2/compiler/backend/backend.h`

`````c
// backend.h (v2): the boundary between the checked program and code generation.
//
// The front-end ends with a checked tree: every name resolved (unit->refs) and every
// expression typed (unit->types). A backend reads only that and writes output; it never
// parses, resolves or type-checks, and the front-end knows nothing about any target beyond
// the three calls below.
//
// Backends are kept in a registry, the way the Go compiler keeps its architectures: a table
// from a name to an Init function, where Init fills one struct of data and function
// pointers (cmd/compile/main.go `archInits`, filling `ssagen.ArchInfo`). Selecting a
// backend is one table lookup; adding one is one new directory and one new table row.
#ifndef MODAL_V2_BACKEND_H
#define MODAL_V2_BACKEND_H

#include "v2/compiler/cx.h"
#include <stdio.h>

typedef struct
{
    Unit *entry; // the checked file to generate code for
    FILE *out;
} BackendJob;

// Code generation is streamed. The compiler keeps the body of one function in memory at a
// time, so a backend is handed each function right after it is checked and must emit it
// then; it cannot come back to it later.
typedef struct Backend
{
    const char *name;      // what `--backend` selects
    const char *summary;   // one line for `--backends`
    const char *extension; // of the files it writes, e.g. ".c"
    // Command that turns the output into an executable, or NULL if the output is final.
    // Used by `--run` as: <toolchain> -o <executable> <output>
    const char *toolchain;

    // The declarations of job->entry are checked: emit whatever needs only those (types,
    // globals, prototypes).
    void (*begin)(const BackendJob *job);
    // `fn` is checked and its body is in memory until this returns.
    void (*function)(const BackendJob *job, SymId fn);
    // Every function has been handed over: emit the entry point and finish the output.
    // Returns the number of errors reported with diag_at for constructs it cannot translate.
    int (*end)(const BackendJob *job);
} Backend;

// registry.c
const Backend *backend_find(const char *name); // NULL if no backend has that name
const Backend *backend_default(void);
void backend_list(FILE *out);

// One Init per backend, each in its own directory.
void backend_c_init(Backend *b); // backend/c/emit.c

#endif // MODAL_V2_BACKEND_H
`````

### `src/v2/compiler/backend/c/emit.c`

`````c
// backend/c/emit.c (v2): the C backend.
//
// Translates one checked Modal file into one self-contained C99 file.
//
//   name: struct { ... }            typedef struct modal_name { ... } modal_name;
//   f(a: u8): u8 { ... }            static uint8_t modal_f(uint8_t v_a) { ... }
//   m(self: T, a: u8): u8 { ... }   static uint8_t modal_T_m(modal_T *self, uint8_t v_a) { ... }
//   u8 f(a: u8) / u8 f(a, b: u8)    modal_f__1 / modal_f__2      (overloads: one C function each)
//   x := expr                       <type of expr> v_x = expr;
//   value.m(a)                      modal_T_m(&value, a)
//   T.m(a)                          modal_T_m(&(modal_T){0}, a)   (a constructor: fresh zero value)
//   self.field                      self->field
//   main(): u8                      int main(void) { return (int)modal_main(); }
//
// Everything it needs was decided by the front-end: unit->types gives the type of every
// expression and unit->refs the declaration behind every name and call. What the backend
// cannot translate yet is reported as an error at the construct, never silently dropped.
//
// Output is streamed: types, globals and prototypes first (they need only declarations),
// then each function as the checker hands it over, then the entry point.
#include "v2/compiler/backend/backend.h"
#include <string.h>

typedef struct
{
    Unit *u;
    FILE *out;
    int errors;
    int indent;
    size_t len; // bytes waiting in buf
    char buf[1 << 16];
} Emit;

static void emit_expr(Emit *e, NodeId id);

// ---- output helpers --------------------------------------------------------------------------

// Output goes through one 64 KB buffer and is written in large pieces. Almost everything
// emitted is a fixed string or a slice of the source, so it is copied straight in: nothing
// on this path goes through printf.
static void flush(Emit *e)
{
    fwrite(e->buf, 1, e->len, e->out);
    e->len = 0;
}

static inline void put_n(Emit *e, const char *text, size_t n)
{
    if (e->len + n > sizeof(e->buf))
    {
        flush(e);
        if (n > sizeof(e->buf))
        {
            fwrite(text, 1, n, e->out);
            return;
        }
    }
    char *to = e->buf + e->len;
    e->len += n;
    if (n <= 16) // most pieces are a few bytes: cheaper than a call into memcpy
    {
        while (n--)
        {
            *to++ = *text++;
        }
        return;
    }
    memcpy(to, text, n);
}

static void put(Emit *e, const char *text)
{
    put_n(e, text, strlen(text));
}

// A string literal: its length is known at compile time.
#define PUT(e, literal) put_n((e), literal, sizeof(literal) - 1)

static inline void put_c(Emit *e, char ch)
{
    if (e->len == sizeof(e->buf))
    {
        flush(e);
    }
    e->buf[e->len++] = ch;
}

static void out_indent(Emit *e)
{
    static const char spaces[] = "                                                                ";
    size_t n = (size_t)e->indent * 4;
    put_n(e, spaces, n < sizeof(spaces) - 1 ? n : sizeof(spaces) - 1);
}

static void out_tok(Emit *e, uint32_t tok)
{
    const Token *t = TOK(e->u, tok);
    put_n(e, t->start, t->len);
}

static void unsupported(Emit *e, uint32_t tok, const char *what)
{
    diag_at(e->u, tok, "error", "backend c: %s is not supported yet", what);
    e->errors++;
}

// ---- names and types -------------------------------------------------------------------------

static void out_sym_name(Emit *e, SymId s)
{
    int len;
    const char *name = symbol_name(s, &len);
    put_n(e, name, (size_t)len);
}

// modal_<struct>_<name>, with __<parameter count> for the overloadable (type-first) form.
static void out_fn_name(Emit *e, SymId fn)
{
    PUT(e, "modal_");
    if (SYM(fn)->owner)
    {
        out_sym_name(e, SYM(fn)->owner);
        PUT(e, "_");
    }
    out_sym_name(e, fn);
    if (SYM(fn)->flags & SYM_F_MUT)
    {
        int arity = 0;
        for (NodeId p = sema_fn_params(e->u, fn); p; p = NODE(e->u, p)->next)
        {
            arity++;
        }
        char digits[12];
        int n = 0;
        do
        {
            digits[n++] = (char)('0' + arity % 10);
            arity /= 10;
        } while (arity);
        PUT(e, "__");
        while (n)
        {
            put_c(e, digits[--n]);
        }
    }
}

static void out_type(Emit *e, TypeId t, uint32_t at)
{
    static const char *const prims[T__PRIM_COUNT] = {
        [T_VOID] = "void",     [T_BOOL] = "bool",    [T_U8] = "uint8_t", [T_U16] = "uint16_t",
        [T_U32] = "uint32_t",  [T_U64] = "uint64_t", [T_I8] = "int8_t",  [T_I16] = "int16_t",
        [T_I32] = "int32_t",   [T_I64] = "int64_t",  [T_INT] = "int",    [T_INT_LIT] = "int",
        [T_STR] = "modal_str",
    };
    TypeKind k = type_kind(t);

    if (k < T__PRIM_COUNT && prims[k])
    {
        put(e, prims[k]);
    }
    else if (k == T_STRUCT)
    {
        PUT(e, "modal_");
        out_sym_name(e, type_ref(t));
    }
    else if (k == T_PTR)
    {
        out_type(e, type_ref(t), at);
        PUT(e, " *");
    }
    else
    {
        char name[64];
        char what[96];
        snprintf(what, sizeof(what), "a value of type %s", type_str(t, name, sizeof(name)));
        unsupported(e, at, what);
        PUT(e, "int");
    }
}

static int is_self_ident(const Unit *u, NodeId id)
{
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    return n->kind == AST_IDENT && is_self_token(t->start, (int)t->len);
}

// ---- expressions -----------------------------------------------------------------------------

// Emits `id` where a value of type `want` is expected. `self` is a reference, so a struct
// value may stand where *T is wanted: its address is taken.
static void emit_as(Emit *e, NodeId id, TypeId want)
{
    TypeId have = e->u->types[id];
    if (type_kind(want) == T_PTR && type_kind(have) == T_STRUCT)
    {
        if (is_self_ident(e->u, id))
        {
            PUT(e, "self");
            return;
        }
        PUT(e, "(&");
        emit_expr(e, id);
        PUT(e, ")");
        return;
    }
    emit_expr(e, id);
}

// write(a, b) / print(a, b): one runtime call per argument, chosen by its type. write emits
// its arguments as they are; print puts a space between them and a newline after.
static void emit_output(Emit *e, NodeId args, int newline)
{
    PUT(e, "(");
    for (NodeId a = args; a; a = NODE(e->u, a)->next)
    {
        TypeId t = e->u->types[a];
        if (newline && a != args)
        {
            PUT(e, "modal_write_str(MODAL_STR(\" \")), "); // print separates its arguments
        }
        if (t == T_STR)
        {
            PUT(e, "modal_write_str(");
        }
        else if (t == T_BOOL)
        {
            PUT(e, "modal_write_bool(");
        }
        else if (t == T_U64)
        {
            PUT(e, "modal_write_uint(");
        }
        else if (type_is_int(t))
        {
            PUT(e, "modal_write_int((long long)");
        }
        else
        {
            unsupported(e, NODE(e->u, a)->tok, "printing a value of this type");
            PUT(e, "modal_write_int((long long)");
        }
        emit_expr(e, a);
        PUT(e, "), ");
    }
    put(e, newline ? "modal_write_str(MODAL_STR(\"\\n\")))" : "(void)0)");
}

static void emit_call(Emit *e, NodeId id)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    const AstNode *callee = NODE(u, n->a);
    SymId fn = u->refs[id];

    if (callee->kind == AST_IDENT && TOK(u, callee->tok)->kind == TOK_WRITE)
    {
        emit_output(e, n->b, 0);
        return;
    }
    if (callee->kind == AST_IDENT && sema_is_std_print(u, u->refs[n->a]))
    {
        emit_output(e, n->b, 1);
        return;
    }
    if (!fn)
    {
        unsupported(e, callee->tok, "calling something that is not a function of this file");
        PUT(e, "0");
        return;
    }

    out_fn_name(e, fn);
    PUT(e, "(");
    int first = 1;

    if (SYM(fn)->owner) // a method: the receiver is the first C argument
    {
        if (callee->kind == AST_SELECTOR)
        {
            TypeId base = u->types[callee->a];
            if (type_kind(base) == T_TYPE) // T.m(...): a fresh zero value
            {
                PUT(e, "&(modal_");
                out_sym_name(e, type_ref(base));
                PUT(e, "){0}");
            }
            else
            {
                emit_as(e, callee->a, type_make(T_PTR, type_make(T_STRUCT, SYM(fn)->owner)));
            }
        }
        else
        {
            PUT(e, "self"); // a bare member name, through implicit self
        }
        first = 0;
    }

    NodeId param = sema_fn_params(u, fn);
    for (NodeId a = n->b; a && param; a = NODE(u, a)->next, param = NODE(u, param)->next)
    {
        put(e, first ? "" : ", ");
        emit_as(e, a, sema_param_type(u, fn, param));
        first = 0;
    }
    PUT(e, ")");
}

static void emit_expr(Emit *e, NodeId id)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    TypeId type = u->types[id];

    switch (n->kind)
    {
    case AST_NUMBER_LIT:
        if (memchr(t->start, '.', t->len))
        {
            unsupported(e, n->tok, "a fractional number");
        }
        out_tok(e, n->tok);
        break;
    case AST_STRING_LIT:
        PUT(e, "MODAL_STR(");
        for (uint32_t i = 0; i < t->len; i++) // a Modal string may span lines; a C one may not
        {
            char ch = t->start[i];
            if (ch == '\n')
            {
                PUT(e, "\\n");
            }
            else if (ch == '\r')
            {
                PUT(e, "\\r");
            }
            else
            {
                put_c(e, ch);
            }
        }
        PUT(e, ")");
        break;
    case AST_IDENT:
    {
        SymId s = u->refs[id];
        if (is_self_ident(u, id))
        {
            PUT(e, "(*self)");
        }
        else if (s && SYM(s)->kind == SYM_FIELD)
        {
            PUT(e, "self->"); // a bare member name, through implicit self
            out_tok(e, n->tok);
        }
        else if (s && SYM(s)->kind == SYM_CONST)
        {
            PUT(e, "g_");
            out_tok(e, n->tok);
        }
        else if (s)
        {
            unsupported(e, n->tok, "using a function, a type or an import as a value");
            PUT(e, "0");
        }
        else if (type == T_BOOL && (t->len == 4 || t->len == 5) &&
                 (!memcmp(t->start, "true", 4) || !memcmp(t->start, "false", 5)))
        {
            out_tok(e, n->tok);
        }
        else if (type == T_NIL)
        {
            PUT(e, "NULL");
        }
        else
        {
            PUT(e, "v_");
            out_tok(e, n->tok);
        }
        break;
    }
    case AST_PAREN_GROUP:
        PUT(e, "(");
        emit_expr(e, n->a);
        PUT(e, ")");
        break;
    case AST_UNARY_OP:
        if (t->kind == TOK_AMP && is_self_ident(u, n->a))
        {
            PUT(e, "self"); // &self: the reference itself
            break;
        }
        put_c(e, '(');
        put_c(e, *t->start);
        emit_expr(e, n->a);
        PUT(e, ")");
        break;
    case AST_BIN_OP:
    {
        const char *op = t->kind == TOK_AND ? "&&" : t->kind == TOK_OR ? "||" : NULL;
        if (u->types[n->a] == T_STR || u->types[n->b] == T_STR)
        {
            unsupported(e, n->tok, "an operator on strings");
        }
        // Arithmetic is done in the Modal type: u8 wraps at 256 at every step, as declared.
        if (type >= T_U8 && type <= T_INT)
        {
            PUT(e, "((");
            out_type(e, type, n->tok);
            PUT(e, ")");
        }
        else
        {
            PUT(e, "(");
        }
        PUT(e, "(");
        emit_expr(e, n->a);
        put_c(e, ' ');
        if (op)
        {
            put(e, op);
        }
        else
        {
            put_n(e, t->start, t->len);
        }
        put_c(e, ' ');
        emit_expr(e, n->b);
        PUT(e, "))");
        break;
    }
    case AST_SELECTOR:
    {
        SymId member = u->refs[id];
        if (!member || SYM(member)->kind != SYM_FIELD)
        {
            unsupported(e, n->tok, "using a method as a value");
            PUT(e, "0");
            break;
        }
        if (is_self_ident(u, n->a))
        {
            PUT(e, "self->");
        }
        else
        {
            PUT(e, "(");
            emit_expr(e, n->a);
            put(e, type_kind(u->types[n->a]) == T_PTR ? ")->" : ").");
        }
        out_tok(e, n->tok);
        break;
    }
    case AST_CALL:
        emit_call(e, id);
        break;
    case AST_ASSIGN:
        emit_expr(e, n->a);
        PUT(e, " = ");
        emit_as(e, n->b, u->types[n->a]);
        break;
    case AST_FN_LIT:
        unsupported(e, n->tok, "a function literal inside another function");
        PUT(e, "0");
        break;
    case AST_INDEX:
        unsupported(e, n->tok, "indexing");
        PUT(e, "0");
        break;
    default:
        unsupported(e, n->tok, "this expression");
        PUT(e, "0");
        break;
    }
}

// ---- statements ------------------------------------------------------------------------------

static void emit_stmt(Emit *e, NodeId id, TypeId ret)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);

    switch (n->kind)
    {
    case AST_BLOCK:
        out_indent(e);
        PUT(e, "{\n");
        e->indent++;
        for (NodeId s = n->a; s; s = NODE(u, s)->next)
        {
            emit_stmt(e, s, ret);
        }
        e->indent--;
        out_indent(e);
        PUT(e, "}\n");
        return;
    case AST_DEFINE:
        out_indent(e);
        if (NODE(u, n->a)->kind == AST_FN_LIT)
        {
            unsupported(e, n->tok, "a function literal inside another function");
            PUT(e, ";\n");
            return;
        }
        out_type(e, u->types[id], n->tok);
        PUT(e, " v_");
        out_tok(e, n->tok);
        PUT(e, " = ");
        emit_as(e, n->a, u->types[id]);
        PUT(e, ";\n");
        return;
    case AST_RETURN:
        out_indent(e);
        PUT(e, "return");
        if (n->a)
        {
            PUT(e, " ");
            emit_as(e, n->a, ret);
        }
        PUT(e, ";\n");
        return;
    default:
        out_indent(e);
        emit_expr(e, id);
        PUT(e, ";\n");
        return;
    }
}

// ---- declarations ----------------------------------------------------------------------------

static const AstNode *fn_node(const Unit *u, SymId fn)
{
    const AstNode *n = NODE(u, SYM(fn)->decl);
    return n->kind == AST_DEFINE ? NODE(u, n->a) : n;
}

// `static <ret> modal_name(<params>)`
static void emit_signature(Emit *e, SymId fn)
{
    Unit *u = e->u;
    int first = 1;

    PUT(e, "static ");
    out_type(e, sema_fn_ret(u, fn), SYM(fn)->tok);
    PUT(e, " ");
    out_fn_name(e, fn);
    PUT(e, "(");
    if (SYM(fn)->owner)
    {
        PUT(e, "modal_");
        out_sym_name(e, SYM(fn)->owner);
        PUT(e, " *self");
        first = 0;
    }
    for (NodeId p = sema_fn_params(u, fn); p; p = NODE(u, p)->next)
    {
        put(e, first ? "" : ", ");
        out_type(e, sema_param_type(u, fn, p), NODE(u, p)->tok);
        PUT(e, " v_");
        out_tok(e, NODE(u, p)->tok);
        first = 0;
    }
    put(e, first ? "void)" : ")");
}

static void emit_function(Emit *e, SymId fn)
{
    Unit *u = e->u;
    const AstNode *n = fn_node(u, fn);
    TypeId ret = sema_fn_ret(u, fn);

    emit_signature(e, fn);
    PUT(e, "\n{\n");
    e->indent = 1;
    if (SYM(fn)->owner)
    {
        PUT(e, "    (void)self;\n");
    }
    if (n->flags & AST_F_ONELINE)
    {
        out_indent(e);
        PUT(e, "return ");
        emit_as(e, n->c, ret);
        PUT(e, ";\n");
    }
    else
    {
        for (NodeId s = NODE(u, n->c)->a; s; s = NODE(u, s)->next)
        {
            emit_stmt(e, s, ret);
        }
    }
    e->indent = 0;
    PUT(e, "}\n\n");
}

// Is this a function the backend can emit? Module-level `self` functions are not, yet.
static int emittable(Emit *e, SymId fn, int report)
{
    const AstNode *n = fn_node(e->u, fn);
    if (!SYM(fn)->owner && (n->flags & AST_F_SELF))
    {
        if (report)
        {
            unsupported(e, SYM(fn)->tok, "a module-level function that takes `self`");
        }
        return 0;
    }
    return 1;
}

static void emit_struct(Emit *e, NodeId decl)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, decl);
    const Token *name = TOK(u, n->tok);
    SymId self = scope_find(u->scope, name->start, name->len);
    int fields = 0;

    PUT(e, "struct modal_");
    put_n(e, name->start, name->len);
    PUT(e, "\n{\n");
    for (NodeId m = n->a; m; m = NODE(u, m)->next)
    {
        const AstNode *member = NODE(u, m);
        if (member->kind == AST_STRUCT_DECL)
        {
            unsupported(e, member->tok, "a struct declared inside another struct");
        }
        if (member->kind != AST_FIELD_DECL)
        {
            continue;
        }
        const Token *field = TOK(u, member->tok);
        SymId s = scope_find(scope_at(SYM(self)->scope_ref), field->start, field->len);
        while (s && SYM(s)->kind != SYM_FIELD)
        {
            s = SYM(s)->next; // skip an accessor that shares the field's name
        }
        PUT(e, "    ");
        out_type(e, s ? sema_value_type(u, s) : T_NONE, member->tok);
        put_c(e, ' ');
        put_n(e, field->start, field->len);
        PUT(e, ";\n");
        fields++;
    }
    if (!fields)
    {
        PUT(e, "    char modal_empty;\n"); // C has no empty structs
    }
    PUT(e, "};\n\n");
}

static const char *const prelude =
    "// Generated by the Modal compiler (backend: c). Do not edit.\n"
    "#include <stdbool.h>\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "#include <stdio.h>\n"
    "\n"
    "typedef struct\n"
    "{\n"
    "    const uint8_t *ptr;\n"
    "    size_t len;\n"
    "} modal_str;\n"
    "\n"
    "#define MODAL_STR(s) ((modal_str){(const uint8_t *)(s), sizeof(s) - 1})\n"
    "\n"
    "static inline void modal_write_str(modal_str s) { fwrite(s.ptr, 1, s.len, stdout); }\n"
    "static inline void modal_write_int(long long n) { printf(\"%lld\", n); }\n"
    "static inline void modal_write_uint(unsigned long long n) { printf(\"%llu\", n); }\n"
    "static inline void modal_write_bool(bool b) { fputs(b ? \"true\" : \"false\", stdout); }\n"
    "\n";

// One Emit for the whole job: it holds the output buffer between the three calls.
static Emit emit_state;

static void backend_c_begin(const BackendJob *job)
{
    Unit *u = job->entry;
    emit_state = (Emit){.u = u, .out = job->out};
    Emit *e = &emit_state;

    // This backend translates one file. An import of another Modal file needs the linker's
    // module graph on the C side as well, which is the next step.
    for (NodeId id = 1; id < u->nodes.size; id++)
    {
        if (NODE(u, id)->kind == AST_IMPORT && NODE(u, id)->aux)
        {
            unsupported(e, NODE(u, id)->tok, "importing another Modal file");
        }
    }

    put(e, prelude);

    // 1. Types: every struct is declared before any is defined, so they can refer to each
    //    other through pointers in any order.
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            const Token *name = TOK(u, NODE(u, d)->tok);
            PUT(e, "typedef struct modal_");
            put_n(e, name->start, name->len);
            PUT(e, " modal_");
            put_n(e, name->start, name->len);
            PUT(e, ";\n");
        }
    }
    PUT(e, "\n");
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            emit_struct(e, d);
        }
    }

    // 2. Module-level values.
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        const AstNode *n = NODE(u, d);
        const Token *name = TOK(u, n->tok);
        int is_const =
            n->kind == AST_DEFINE && NODE(u, n->a)->kind != AST_FN_LIT && !ast_import_root(u, n->a);
        if (n->kind != AST_FIELD_DECL && !is_const)
        {
            continue;
        }
        SymId s = scope_find(u->scope, name->start, name->len);
        NodeId init = n->kind == AST_FIELD_DECL ? n->b : n->a;
        TypeId type = s ? sema_value_type(u, s) : T_NONE;

        put(e, (n->kind == AST_DEFINE || !(n->flags & AST_F_MUT)) ? "static const " : "static ");
        out_type(e, type, n->tok);
        PUT(e, " g_");
        put_n(e, name->start, name->len);
        if (init)
        {
            PUT(e, " = ");
            emit_expr(e, init);
        }
        PUT(e, ";\n");
    }
    PUT(e, "\n");

    // 3. A prototype for every function, so the bodies that follow can call each other in
    //    any order. Signatures need only the declarations.
    for (SymId fn = 1; fn < cx.syms.size; fn++)
    {
        if (SYM(fn)->unit == u->id && SYM(fn)->kind == SYM_METHOD && emittable(e, fn, 1))
        {
            emit_signature(e, fn);
            PUT(e, ";\n");
        }
    }
    PUT(e, "\n");
}

// Called once per function, right after it is checked, while its body is still in memory.
static void backend_c_function(const BackendJob *job, SymId fn)
{
    (void)job;
    if (emittable(&emit_state, fn, 0))
    {
        emit_function(&emit_state, fn);
    }
}

static int backend_c_end(const BackendJob *job)
{
    Unit *u = job->entry;
    Emit *e = &emit_state;
    SymId entry = scope_find(u->scope, "main", 4);

    if (!entry || SYM(entry)->kind != SYM_METHOD || sema_fn_params(u, entry) ||
        SYM(entry)->next_overload)
    {
        diag_at(u, entry ? SYM(entry)->tok : 0, "error",
                "backend c: the program needs one `main()` without parameters to start from");
        e->errors++;
    }
    else if (sema_fn_ret(u, entry) == T_VOID)
    {
        PUT(e, "int main(void)\n{\n    ");
        out_fn_name(e, entry);
        PUT(e, "();\n    return 0;\n}\n");
    }
    else
    {
        PUT(e, "int main(void)\n{\n    return (int)");
        out_fn_name(e, entry);
        PUT(e, "();\n}\n");
    }

    flush(e);
    return e->errors;
}

void backend_c_init(Backend *b)
{
    b->summary = "portable C99 source, one file per program";
    b->extension = ".c";
    b->toolchain = "cc";
    b->begin = backend_c_begin;
    b->function = backend_c_function;
    b->end = backend_c_end;
}
`````

### `src/v2/compiler/backend/registry.c`

`````c
#include "v2/compiler/backend/backend.h"
#include <string.h>

// name -> Init. Several names may share one Init (Go maps both "mips" and "mipsle" to
// mips.Init); the Init can read the name it was selected by and adjust what it fills in.
static const struct
{
    const char *name;
    void (*init)(Backend *b);
} backend_inits[] = {
    {"c", backend_c_init},
};

#define BACKEND_COUNT (sizeof(backend_inits) / sizeof(backend_inits[0]))

// Backends are initialised on first use and kept: there are only a handful.
static Backend backends[BACKEND_COUNT];

static const Backend *backend_at(size_t i)
{
    if (!backends[i].name)
    {
        backends[i].name = backend_inits[i].name;
        backend_inits[i].init(&backends[i]);
    }
    return &backends[i];
}

const Backend *backend_find(const char *name)
{
    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
        if (strcmp(backend_inits[i].name, name) == 0)
        {
            return backend_at(i);
        }
    }
    return NULL;
}

const Backend *backend_default(void)
{
    return backend_at(0);
}

void backend_list(FILE *out)
{
    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
        const Backend *b = backend_at(i);
        fprintf(out, "  %-8s %s%s\n", b->name, b->summary, i == 0 ? "  (default)" : "");
    }
}

// ---- streaming -------------------------------------------------------------------------------

static struct
{
    const Backend *backend;
    BackendJob job;
    int begun;
} codegen;

void codegen_start(const struct Backend *backend, void *out_file)
{
    codegen.backend = backend;
    codegen.job.out = (FILE *)out_file;
    codegen.begun = 0;
}

// Code is generated for the entry file, which is always the first unit.
void codegen_begin(Unit *u)
{
    if (codegen.backend && u->id == 0)
    {
        uint64_t t = cx.timing ? clock_ns() : 0;
        codegen.job.entry = u;
        codegen.backend->begin(&codegen.job);
        codegen.begun = 1;
        if (cx.timing)
        {
            phase_end(PHASE_EMIT, t);
        }
    }
}

void codegen_function(Unit *u, SymId fn)
{
    if (codegen.begun && u->id == 0)
    {
        uint64_t t = cx.timing ? clock_ns() : 0;
        codegen.backend->function(&codegen.job, fn);
        if (cx.timing)
        {
            phase_end(PHASE_EMIT, t);
        }
    }
}

int codegen_finish(void)
{
    if (!codegen.begun)
    {
        return 0; // the entry file did not get as far as being checked: nothing was emitted
    }
    uint64_t t = clock_ns();
    int errors = codegen.backend->end(&codegen.job);
    phase_end(PHASE_EMIT, t);
    codegen.begun = 0;
    return errors;
}
`````

### `src/v2/tests/run.sh`

`````sh
#!/bin/sh
# Front-end checks for v2. Run from the repository root: `make test_v2`.
BIN=./modal-v2
EX=./examples/v2
fail=0

check() { # description, command...
    desc=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "ok    $desc"
    else
        echo "FAIL  $desc"
        fail=1
    fi
}

expect_output() { # description, pattern, file, flags...
    desc=$1
    pattern=$2
    file=$3
    shift 3
    if $BIN "$@" "$file" 2>&1 | grep -q -- "$pattern"; then
        echo "ok    $desc"
    else
        echo "FAIL  $desc (no match for: $pattern)"
        fail=1
    fi
}

expect_fail() { # description, file
    if $BIN "$2" >/dev/null 2>&1; then
        echo "FAIL  $1 (expected errors)"
        fail=1
    else
        echo "ok    $1"
    fi
}

check "example program compiles cleanly" $BIN $EX/main.modal
check "valid invocations compile cleanly" $BIN $EX/calls.modal
check "v1 example still parses" $BIN ./examples/add.modal

S="--syms"
expect_output "type-first field is mutable" "field   john_nested_class.my_name .* mut" $EX/main.modal $S
expect_output "fields are always inside" "field   john_nested_class.n2 *inside" $EX/main.modal $S
expect_output "method inside the braces is private" "method  john_nested_class.check *inside" $EX/main.modal $S
expect_output "method outside the braces is the face" "method  john_nested_class.add_normal *public" $EX/main.modal $S
expect_output "accessor may share the field name" "method  john_nested_class.n1 *public" $EX/main.modal $S
expect_output "free binding stays in the domain" "method  print_file_name *domain" $EX/main.modal $S
expect_output "config binding" "const   implicit_self *config" $EX/main.modal $S
expect_output "overloads share one name" "john_nested_class.add_oneline .* mut" $EX/main.modal $S
expect_output "one-line method body" "add_oneline.*\[oneline\]" $EX/main.modal --ast
expect_output "function literal binding" "fn () int" $EX/main.modal --ast
expect_output "import alias" "item new as make_john" $EX/main.modal --ast

G="--sig"
expect_output "contract lists opaque types" "^  type john_nested_class" $EX/main.modal $G
expect_output "contract lists face signatures" "^  add_normal(self: john_nested_class, n3: u8): u8" $EX/main.modal $G
expect_output "contract lists config" "^config version = \"0.1.0\"" $EX/main.modal $G
if $BIN --sig $EX/main.modal 2>&1 | grep -q "check\|my_name\|print_file_name"; then
    echo "FAIL  contract leaks private names"
    fail=1
else
    echo "ok    contract holds no private names"
fi

expect_fail "invisible names are rejected" $EX/errors/private.modal
expect_output "field" "'my_name' is a field of 'john_nested_class'" $EX/errors/private.modal
expect_output "helper inside the braces" "'check' is inside 'john_nested_class'" $EX/errors/private.modal
expect_output "binding inside a module" "'print_file_name' is inside module 'john'" $EX/errors/private.modal
expect_output "module not bound in mod.modal" "module 'secret' is not part of the face" $EX/errors/private.modal
expect_output "config is not part of the face" "'implicit_self' is configuration" $EX/errors/private.modal
expect_output "missing member" "has no member 'does_not_exist'" $EX/errors/private.modal
if $BIN $EX/errors/private.modal 2>&1 | grep -q "add_oneline"; then
    echo "FAIL  a face method was rejected"
    fail=1
else
    echo "ok    face method is accepted across domains"
fi
L=$EX/errors/leak.modal
expect_output "member access on a binding: missing module" "leak.modal:5:.*'example' has no member 'secret'" $L
expect_output "member access on a binding: field" "leak.modal:6:.*'my_name' is a field" $L
expect_output "member access on a binding: private binding" "leak.modal:7:.*'print_file_name' is inside module" $L
expect_output "alias of a binding" "leak.modal:11:.*'check' is inside 'john_nested_class'" $L
expect_output "import inside a body" "leak.modal:15:.*module 'secret' is not part of the face" $L
expect_output "member access inside a body" "leak.modal:16:.*'my_name' is a field" $L
expect_output "unresolved path without ./" "leak.modal:21:.*cannot find module" $L
expect_output "bare name is found in a folder above" "leak.modal:24:.*'example' has no member 'secret'" $L
expect_output "unresolved symbol name" "leak.modal:25:.*cannot find module nowhere" $L
if $BIN $L 2>&1 | grep -q "leak.modal:26"; then
    echo "FAIL  std was rejected"
    fail=1
else
    echo "ok    std is the one external root"
fi
expect_output "subfolder is internal to its parent" "domain 'inner' is internal to 'example'" $EX/errors/far/reach.modal
expect_output "unknown config name" "'implict_self' is not a known configuration" $EX/errors/config/mod.modal
expect_output "config value kind" "'nil_return' expects true or false" $EX/errors/config/mod.modal

K=$EX/errors/calls.modal
expect_fail "bad invocations are rejected" $K
expect_output "undeclared function" "calls.modal:28:.*call to undeclared function 'missing_function'" $K
expect_output "wrong argument count" "calls.modal:29:.*'double' takes 1 argument, but 2 are given" $K
expect_output "no overload fits" "calls.modal:30:.*'scale' takes 1 or 2 arguments, but 3 are given" $K
expect_output "undeclared name" "calls.modal:31:.*'unknown_name' is not declared" $K
expect_output "method called as a free function" "calls.modal:32:.*'read' is a member of 'counter', not a free name" $K
expect_output "local used before its declaration" "calls.modal:33:.*undeclared function 'later_local'" $K
expect_output "self method argument count" "calls.modal:40:.*'bump' takes 1 argument, but 0 are given" $K
expect_output "self member missing" "calls.modal:41:.*'counter' has no member 'nothing'" $K
expect_output "bare member without implicit_self" "calls.modal:42:.*write .self.total." $K
expect_output "self outside a method" "calls.modal:47:.*does not declare it" $K
if $BIN $K 2>&1 | grep -q "calls.modal:2[3-6]:\|calls.modal:39:"; then
    echo "FAIL  a valid invocation was rejected"
    fail=1
else
    echo "ok    valid invocations and overloads are accepted"
fi
expect_output "memory is reported" "KB taken from the system at peak" $EX/main.modal --stats

T=$EX/errors/types.modal
expect_fail "type errors are rejected" $T
expect_output "assignment to an immutable field" "types.modal:18:.*'y' is immutable" $T
expect_output "return type mismatch" "types.modal:27:.*returns int, but the function declares \[\]u8" $T
expect_output "operator on mismatched types" "types.modal:33:.*cannot be applied to u8 and \[\]u8" $T
expect_output "argument type mismatch" "types.modal:34:.*argument 1 of 'wide' is u8, but parameter 'n' is u16" $T
expect_output "member access on a non-struct" "types.modal:35:.*a value of type u8 has no member 'x'" $T
expect_output "calling a type" "types.modal:37:.*'point' is a type, not a function" $T
expect_output "local declared twice" "types.modal:39:.*'q' is already declared in this block" $T

# code generation: the C backend
expect_output "backend registry lists c" "^  c  " $EX/run/hello.modal --backends
if $BIN --backend nope $EX/run/hello.modal 2>&1 | grep -q "unknown backend 'nope'"; then
    echo "ok    unknown backend is rejected"
else
    echo "FAIL  unknown backend is rejected"
    fail=1
fi
for prog in hello counter; do
    if $BIN --run $EX/run/$prog.modal 2>&1 | cmp -s - $EX/run/$prog.expected; then
        echo "ok    $prog.modal compiles to C and prints the expected output"
    else
        echo "FAIL  $prog.modal compiles to C and prints the expected output"
        fail=1
    fi
done
OUT=${TMPDIR:-/tmp}/modal-test-$$.c
if $BIN -o "$OUT" $EX/run/counter.modal >/dev/null 2>&1 && ${CC:-cc} -std=c99 -Wall -Wextra -Werror -o "$OUT.bin" "$OUT" 2>/dev/null; then
    echo "ok    generated C builds with -Wall -Wextra -Werror"
else
    echo "FAIL  generated C builds with -Wall -Wextra -Werror"
    fail=1
fi
rm -f "$OUT" "$OUT.bin"
if $BIN -o "$OUT" $EX/calls.modal 2>&1 | grep -q "backend c: a function literal inside another function is not supported yet" && [ ! -e "$OUT" ]; then
    echo "ok    unsupported construct is reported and no file is left behind"
else
    echo "FAIL  unsupported construct is reported and no file is left behind"
    fail=1
fi

check "a bare import name is found in the folders above" $BIN $EX/example/inner/up.modal

# the deferred pipeline (default) and the eager one must produce the same program
A=${TMPDIR:-/tmp}/modal-test-a-$$.c
B=${TMPDIR:-/tmp}/modal-test-b-$$.c
if $BIN -o "$A" $EX/run/counter.modal >/dev/null 2>&1 && $BIN --eager -o "$B" $EX/run/counter.modal >/dev/null 2>&1 && cmp -s "$A" "$B"; then
    echo "ok    deferred and eager pipelines generate identical C"
else
    echo "FAIL  deferred and eager pipelines generate identical C"
    fail=1
fi
rm -f "$A" "$B"
expect_output "bodies are not kept in memory" "nodes kept 7[0-9] " $EX/run/counter.modal --stats
expect_output "eager mode keeps the whole tree" "nodes kept 1[0-9][0-9] " $EX/run/counter.modal --stats --eager
expect_output "phase timing" "^total .* MB/s of source" $EX/run/counter.modal --time

expect_fail "syntax errors are reported" $EX/errors/syntax.modal
expect_output "malformed alias" "expected an alias name after 'as'" $EX/errors/syntax.modal
expect_output "recovery inside the import list" "expected a name in the import list" $EX/errors/syntax.modal
expect_output "no visibility sections" "there are no .pub:. / .mut:. sections" $EX/errors/syntax.modal
expect_output "statements need a separator" "expected a newline or ';' between statements" $EX/errors/syntax.modal
expect_output "immutable method defined twice" "'area' is already declared at line 15" $EX/errors/syntax.modal
expect_output "overload with the same signature" "'twice' is already declared at line 24 with the same" $EX/errors/syntax.modal
expect_output "self of a foreign type" "the type of .self. must be this module" $EX/errors/syntax.modal
expect_output "untyped parameter" "parameter 'a' needs a type" $EX/errors/syntax.modal

if [ $fail -eq 0 ]; then
    echo "all v2 checks passed"
fi
exit $fail
`````

### `makefile`

`````make
CC = gcc
CFLAGS = -Wall -O3 -Wextra -g -std=c17 -MMD -MP -I ./ -I src/v1/compiler -I src
TARGET = jma
LIBS = -lm -lpthread -ldl

SRCS = ./src/v1/compiler/cx.c ./src/v1/compiler/syntax/tokenizer.impl.c ./src/v1/compiler/syntax/tokenizer.c ./src/v1/compiler/syntax/parser/parser.c ./src/v1/compiler/ast/ast.c ./src/v1/compiler/ast/error.c ./src/v1/compiler/syntax/parser/parse_decl.c ./src/v1/compiler/syntax/parser/parse_expr.c ./src/v1/compiler/syntax/parser/parse_stmt.c ./src/v1/compiler/test_runner.c ./main.c  # adicione todos .c

SRCS_V2 = ./src/v2/main.c ./src/v2/compiler/cx.c ./src/v2/compiler/syntax/scanner.c ./src/v2/compiler/syntax/parser/parser.c ./src/v2/compiler/syntax/parser/parse_decl.c ./src/v2/compiler/syntax/parser/parse_expr.c ./src/v2/compiler/syntax/parser/parse_stmt.c ./src/v2/compiler/ast/ast.c ./src/v2/compiler/ast/symbol.c ./src/v2/compiler/sema/resolve.c ./src/v2/compiler/sema/module.c ./src/v2/compiler/sema/check.c ./src/v2/compiler/sema/types.c ./src/v2/compiler/backend/registry.c ./src/v2/compiler/backend/c/emit.c
HDRS_V2 = $(wildcard ./src/v2/compiler/*.h ./src/v2/compiler/*/*.h ./src/v2/compiler/*/*/*.h)
CFLAGS_V2 = -Wall -Wextra -O2 -g -std=c17 -I src
OBJS = $(SRCS:.c=.o)

OUT_DIR := ./out
	
# Paths
PREFIX ?= /usr/local
BINDIR = ${PREFIX}/bin

modal: $(OBJS)
	$(CC) $(OBJS) -o ./modal


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# v2 front-end: scan -> parse -> declare -> link (see docs/v2-frontend.md)
v2: modal-v2

modal-v2: $(SRCS_V2) $(HDRS_V2)
	$(CC) $(CFLAGS_V2) $(SRCS_V2) -o ./modal-v2

run_v2: modal-v2
	./modal-v2 --run ./examples/v2/run/counter.modal

test_v2: modal-v2
	sh ./src/v2/tests/run.sh

run: modal
	clear; ./modal ./examples/add.modal

test_parser3: src/v2/tests/ll1_parsing_3.c
	$(CC) -Wall -Wextra -O2 -std=c17 -pthread src/v2/tests/ll1_parsing_3.c -o src/v2/tests/test_parser3

# no -mavx2 here: P4.3 uses runtime dispatch; only the target("avx2")-
# tagged section may emit AVX2, so the binary stays safe on any x86-64.
test_parser4: src/v2/tests/ll1_parsing_4.c
	$(CC) -Wall -Wextra -O2 -std=c17 -pthread src/v2/tests/ll1_parsing_4.c -o src/v2/tests/test_parser4

test_parser5: src/v2/tests/ll1_parsing_5.c
	$(CC) -Wall -Wextra -O2 -std=c17 -pthread src/v2/tests/ll1_parsing_5.c -o src/v2/tests/test_parser5

bench5: test_parser5 test_parser4
	@echo "== phase4 (v4) =="
	./src/v2/tests/test_parser4 dyn
	@echo "== phase5 (line-index + unroll) =="
	./src/v2/tests/test_parser5 dyn

bench4: test_parser4 test_parser3
	@echo "== phase3 baseline =="
	./src/v2/tests/test_parser3 dyn
	@echo "== phase4 (SWAR/SIMD) =="
	./src/v2/tests/test_parser4 dyn

bench3: test_parser3
	@echo "== static baseline =="
	./src/v2/tests/test_parser3 static
	@echo "== dynamic queue =="
	./src/v2/tests/test_parser3 dyn
	@echo "== dynamic oversubscribed =="
	./src/v2/tests/test_parser3 dyn 32

verbose: modal
	./modal ./examples/add.modal

bundle:
	mkdir -p $(OUT_DIR)	

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) modal modal-v2

-include $(OBJS:.o=.d)
`````

### `examples/v2/calls.modal`

`````
-- invocations that resolve: each one leads to a declaration and fits it.
-- The failing counterparts are in errors/calls.modal.

counter: struct {
    u8 total,

    -- inside the braces: a helper, reached through self
    bump(self, by: u8): u8 {
        return self.total + by
    }

    -- overloaded helpers: told apart by the number of arguments
    u8 mix(self, a: u8): self.total + a
    u8 mix(self, a, b: u8): self.total + a + b
}

-- the face of counter: methods written outside its braces
read(self: counter): u8 {
    return self.total
}

advance(self: counter, by: u8): u8 {
    once := self.bump(by)
    twice := self.bump(once)
    return self.mix(once) + self.mix(once, twice)
}

-- free functions
double(n: u8): u8 {
    return n * 2
}

u8 scale(n: u8): n * 2
u8 scale(n, factor: u8): n * factor

-- a function declared later can be called from earlier code
first(): u8 {
    return second(1)
}

second(n: u8): u8 {
    return double(n)
}

-- a binding that holds a function is called like any other
triple := (n: u8) u8 {
    return n * 3
}

-- calls through imports
print := import("std").io.print

main(c: counter): u8 {
    -- one definition
    a := double(1)

    -- an overload set: either definition may match
    b := scale(a)
    d := scale(a, b)

    -- a call as an argument, and a call on the result of a binding
    e := double(scale(a, triple(b)))

    -- methods on a value
    f := c.read()
    g := c.advance(e)

    -- a local is visible after its declaration, also inside a nested function
    add_e := (n: u8) u8 {
        return n + e + double(n)
    }
    h := add_e(f)

    -- an inner block sees the outer locals
    {
        inner := first() + h
        print(inner)
    }

    -- Lua-style call with one string argument
    print "done"

    return g + second(d)
}
`````

### `examples/v2/errors/calls.modal`

`````
-- every invocation must lead to a declaration, and fit it

counter: struct {
    u8 total,

    bump(self, by: u8): u8 {
        return self.total + by
    }
}

read(self: counter): u8 {
    return self.total
}

double(n: u8): u8 {
    return n * 2
}

u8 scale(n: u8): n * 2
u8 scale(n, factor: u8): n * factor

main(c: counter): u8 {
    a := double(1)
    b := scale(1)
    d := scale(1, 2)
    ok := c.read()

    missing_function(a)
    e := double(1, 2)
    f := scale(1, 2, 3)
    g := a + unknown_name
    h := read(c)
    later_local(a)
    later_local := 1
    return 0
}

check(self: counter): u8 {
    i := self.bump(1)
    j := self.bump()
    k := self.nothing
    l := total
    return self.missing_method(1)
}

free(): u8 {
    return self.total
}
`````

### `examples/v2/errors/config/mod.modal`

`````
implict_self := false
nil_return := "yes"
`````

### `examples/v2/errors/far/reach.modal`

`````
-- `inner` is internal to `example`; this file is not under `example`
gauge := import("../../example/inner").deep.gauge
`````

### `examples/v2/errors/leak.modal`

`````
-- the same rules hold however a module path is written
ex := import("../example")

-- member access on a binding is checked like a direct import
a := ex.secret
b := ex.john.john_nested_class.my_name
c := ex.john.print_file_name

-- so is an alias of a binding
j := ex.john
d := j.john_nested_class.check

-- and an import or a member access inside a body
main(): u8 {
    s := import("../example/secret").vault
    t := ex.john.john_nested_class.my_name
    return 0
}

-- a path that does not resolve is an error, with or without a leading ./
e := import("example/secret")

-- a bare name that resolves to nothing is an error too; only `std` is external
f := import("example").secret.vault
g := import(nowhere)
ok := import("std").io
`````

### `examples/v2/errors/private.modal`

`````
-- every line but the last reaches for something it cannot see
field := import("../example").john.john_nested_class.my_name
helper := import("../example").john.john_nested_class.check
binding := import("../example").john.print_file_name
unbound := import("../example/secret").vault
config := import("../example").implicit_self
missing := import("../example").john.john_nested_class.does_not_exist
ok := import("../example").john.john_nested_class.add_oneline
`````

### `examples/v2/errors/types.modal`

`````
-- the type checker: each line below is one mistake

point: struct {
    u8 x,
    y: u8,

    hidden(self): u8 {
        return self.x
    }
}

sum(self: point): u8 {
    return self.x + self.y
}

move(self: point, dx: u8): u8 {
    self.x = self.x + dx
    self.y = 1
    return self.x
}

wide(n: u16): u16 {
    return n
}

label(): []u8 {
    return 5
}

main(): u8 {
    p := point.new()
    q := point.sum()
    a := q + "text"
    b := wide(q)
    c := q.x
    d := p.hidden()
    e := point()
    f := sum
    q := 1
    nothing := move(q)
    g := label() + 1
    return "done"
}
`````

### `examples/v2/example/inner/deep.modal`

`````
gauge: struct {
    u8 level
}

u8 read(self: gauge): self.level
`````

### `examples/v2/example/inner/mod.modal`

`````
-- a subfolder is internal to its parent: only `example` and what is below it can import this
deep := import("./deep")
`````

### `examples/v2/example/inner/up.modal`

`````
-- a bare name is looked for next to the file, then in every folder above it:
-- `example` here is the domain two levels up, found through its own mod.modal
john := import("example").john
own := import("inner").deep
`````

### `examples/v2/example/john.modal`

`````
-- module `john`, in domain `example` (the folder).
-- One rule: a body hides its inside, and its face is written just outside it.

default_struct: struct {
    -- type last: immutable member
    example_1_of_member: u8,
    -- type first: mutable member
    u8 example_2_of_member,
}

john_nested_class: struct {
    -- inside the braces: fields and helpers, private to the struct
    []u8 my_name,
    u8 n1,
    u8 n2,

    check(self): u8 {
        return self.n1 + self.n2
    }
}

-- outside the braces: the public face of john_nested_class, next to its data

new(self: john_nested_class): self {
    return &self
}

add_normal(self: john_nested_class, n3: u8): u8 {
    return self.check() + n3
}

-- type first: mutable method, one-line body, may be overloaded
u8 add_oneline(self: john_nested_class, n3: u8): self.n1 + self.n2 + n3;
u8 add_oneline(self: john_nested_class, n3, n4: u8): self.n1 + self.n2 + n3 + n4

multiply(self: john_nested_class): u8 {
    return self.n1 * self.n2
}

-- read access to a field is a one-line method
u8 n1(self: john_nested_class): self.n1

-- inside the file: imports, bindings and free functions stay in the domain
std := import("std")
val (
    file as fake_get_filename_function,
    print as fake_print_function
): import(std).io

print_file_name := (self) []u8 {
    return fake_print_function(self.fake_get_filename_function())
}
`````

### `examples/v2/example/mod.modal`

`````
-- domain `example`: its configuration, and the modules that form its face.
-- Members are never listed here; each struct decides its own face by position.

implicit_self := false
nil_return := true
version := "0.1.0"

john := import("./john")
`````

### `examples/v2/example/secret.modal`

`````
-- not bound in mod.modal: this module never leaves the domain
vault: struct {
    u8 code
}

open(self: vault): u8 {
    return self.code
}
`````

### `examples/v2/main.modal`

`````
-- entry point, in the root domain: it sees `example` only through example/mod.modal

-- the three import spellings
example := import("./example")
can_import_directly_the_module := import(example)
or_can_import_directly_the_file := import("./example/john.modal")

-- selectors, wildcard, destructuring, aliases
john_class := import("./example").john.john_nested_class
io := import("std").io.*
file_and_print := import("std").io.(file, print)

val (
    new as make_john,
    add_normal as add
): import("./example").john.john_nested_class

import("std").io.(
    file as fake_get_filename_function,
    print as fake_print_function
)

-- 1. a binding that holds a function
start := () int {
    print "starting"
    return 0
}

-- 2. immutable method
main(): u8 {
    john := make_john()
    total := add(john, 3)
    fake_print_function(total)
    return 0
}

-- 3. mutable methods: the same name with different parameter types
u8 scale(n: u8) {
    return n * 2
}

u8 scale(n, factor: u8): n * factor
`````

### `examples/v2/run/counter.expected`

`````
start: 1
double(21) = 42
scale(a) = 84 scale(a, 3) = 126
advance: 88 total now: 86
200 + 100 as u8 = 44
done
`````

### `examples/v2/run/counter.modal`

`````
-- structs, methods, overloads and constructors, compiled to C and run
print := import("std").io.print

limit: u8 = 200

counter: struct {
    u8 total,
    step: u8,

    -- inside the braces: helpers, private to counter
    bump(self, by: u8): u8 {
        self.total = self.total + by
        return self.total
    }

    u8 mix(self, a: u8): self.total + a
    u8 mix(self, a, b: u8): self.total + a + b
}

-- the face of counter
new(self: counter): self {
    self.total = 1
    return &self
}

read(self: counter): u8 {
    return self.total
}

advance(self: counter, by: u8): u8 {
    once := self.bump(by)
    twice := self.bump(once)
    return self.mix(once) + self.mix(once, twice)
}

double(n: u8): u8 {
    return n * 2
}

u8 scale(n: u8): n * 2
u8 scale(n, factor: u8): n * factor

main(): u8 {
    c := counter.new()
    print("start:", c.read())

    a := double(21)
    print("double(21) =", a)
    print("scale(a) =", scale(a), "scale(a, 3) =", scale(a, 3))

    g := c.advance(a)
    print("advance:", g, "total now:", c.read())

    -- u8 arithmetic wraps at 256, as the type says
    big := limit + 100
    print("200 + 100 as u8 =", big)

    write("done", "\n")
    return 0
}
`````

### `examples/v2/run/hello.expected`

`````
hello from Modal
`````

### `examples/v2/run/hello.modal`

`````
-- a first program that runs:  ./modal-v2 --run examples/v2/run/hello.modal
print := import("std").io.print

main(): u8 {
    print "hello from Modal"
    return 0
}
`````

### `.gitignore`

`````
# Ignoring outputs
*.o
*.d
*.exe
modal-v2
`````
