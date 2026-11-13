# Modal: the core and std

Status: 2026-11-13. The design (the owner chose "core primitives + std in Modal"), then the
steps. Steps 1–3 are done (context.md §26). Step 4's optimizer prerequisites are done (§28); its
swap waits on arrays (§6, in progress, §9). Step 5: the owner decided `nil` and the overload rule,
and its first part is built (§7, context.md §29). Literal patterns and arms: §8. Companions: `docs/development.md` §2 (the core/std
rule), `docs/comptime.md`, `docs/guards.md`.

---

## 1. The rule

> The compiler implements only what Modal cannot express. Everything else is std, written in
> Modal, in `.modal` files.

- **The bootstrap needs no std.** The compiler builds from C sources. std files are read only
  when a program imports `std`.
- **std needs no libc.** Everything std does goes through the primitives below. The only system
  interface is the target layer: Linux system calls, a halt on bare metal, and libSystem's
  `write` and `exit` on macOS.
  - On macOS, Apple keeps the system-call numbers private and requires dyld (context.md §18).
    Go uses libSystem there for the same reason.
- **A primitive is a predeclared name, never a keyword.** A program's own declaration with the
  same name wins, as Go's predeclared identifiers do.
- **Every primitive has a written reason:** what Modal cannot express without it. A primitive
  with no such reason moves to std.

## 2. The primitives

| Primitive | What it is | Why it cannot be std |
|---|---|---|
| `sizeof(T)` | bytes of a type (or of a value's type) in the target's C layout; an unsized constant | it needs types and the target's layout |
| `u8(x)`, `i32(x)`, ... | a conversion between integer types, wrapping as a C cast does | Modal has no other way to change an integer's width |
| `trap(a, b, ...)` | writes its strings to the error output (after the program's own output), then stops: status 1 hosted, the halt on a board. At compile time, it is a compile error with that text | stopping the program is the target's, not the language's |
| `site: site` | a context parameter the compiler fills at each call with `file:line:col: <the call as written>` (a `[]u8`; a caller's own `site` is passed on instead) | only the compiler knows where a call is |
| `write(...)` | writes values to the `out` in scope, or the target's output | variadic over types: Modal has no variadic or type parameters yet (comptime.md step 3) |

`site` follows the context-parameter rule that already exists (a parameter named like its type,
type-last, filled from the caller). The difference is that its value comes from the compiler,
not from a local of the caller. No new syntax is needed.

## 3. std, in Modal

```
std/
  mod.modal      -- the domain, and its face: assert := import("./debug").assert
  debug.modal    -- assert
```

- **Where std is found:** `$MODAL_STD`, else `std/` next to the compiler's executable (`find_std`
  in `main.c`).
  - A program that never imports std reads no file. Neither does one that only uses the
    compiler's own namespaces, `std.io` and `std.sys` (§4).
  - Without a std folder, `import("std")` stays external, as it was before std had files: `print`
    and `put` still work, and nothing else is there.
- **std is a root domain** wherever its folder is. The rule that makes a nested domain internal to
  its parent does not apply to it.
- **std's face is what its mod.modal binds.** A free function of a module is private to its
  domain (the rule every domain follows), so std shares `assert` through a binding:
  `assert := import("std").assert`.
- **`assert` is Modal** (`std/debug.modal`):

```
bool assert(bool true, site: site): true
bool assert(bool ok, site: site) {
    trap(site, " is false\n")
    return ok
}
```

  - It returns its condition, because a type-first function needs a return type. Whether
    `nil`/`void` can be written there is the owner's open question (context.md §10).
  - Pattern clauses need a last clause that takes every value: `bool ok`, not `bool false`.
  - In a body, `assert(n < 4)` is an ordinary call. A false condition prints
    `guards.modal:27:5: assert(n < 4) is false` and status 1, the same on every backend.
  - At module level, it is a call statement the compiler runs (comptime.md §1). The trap's text
    is the compile error when the trap names the statement's own place. A comparison as the
    first argument gets both sides' values: `assert(sizeof(header) == 12) is false (8 == 12)`.
    Nothing in the compiler knows `assert` by name.
  - A function with its own `site: site` passes its caller's place on (`check(n < 5, site)`), so
    a helper built on assert reports the line that called the helper.

## 4. What still lives in the compiler, and what moves it out

| Still in the compiler | Why, today | Moved by |
|---|---|---|
| `print` (and `write`) formatting numbers, bools and strings | variadic over types; and a number formatted in Modal is 2.1× the C stencil without arrays (§6; speed first) | arrays, under the speed gate below (tail calls, the inline sink and constant division are done, §28); then §7 for `print` itself |
| `std.sys.put` | the target's byte sink: the end of every output path | stays a target primitive |
| the native runtime's `rt_write_*`, the C prelude's `m_write_*`, the generated `sink_*` MIR | formatting, as above | the same step as `print` |

`import("std").io.print` and `import("std").sys.put` keep their paths. `std.io` and `std.sys` are
the compiler's namespaces, so no std file is read for them (`compiler_namespace` in `module.c`).

**The speed gate** for moving formatting into Modal:
- `make bench` exec times, plus an output-heavy example (many numbers printed), must match the
  C stencils within noise.
- Until they do, the compiler's formatting stays. The optimizer prerequisites (tail calls as
  jumps, the byte sink inline, constant division) are done; the measured gap and what closes it
  are in §6.

## 5. Steps

1. ~~**Primitives:**~~ done (context.md §26):
   - `sizeof` predeclared, with no import;
   - integer conversions;
   - `trap(...)` with run-time strings;
   - the `site` context.
2. ~~**std as files:**~~ done (§26):
   - the folder is found and loaded on import, and `assert` is written in Modal;
   - the compiler's own assert is gone (its checker, lowering and C emission);
   - assert as a call measured against no assert at all: 116 against 118 ms for ten million,
     within noise.
3. ~~**The C backend without stdio:**~~ done (§26): its programs use only `write` and `exit`. The
   C backend also emits the functions of every file now (not their types or values yet).
4. **Formatting in Modal,** behind the speed gate (§4). The optimizer prerequisites are done
   (context.md §28); the swap itself is not, because the gate is not met (§6):
   - ~~tail calls as jumps~~: every backend, and a clause loop is a real loop;
   - ~~an inline byte sink~~: `put(b)` stores into the buffer and calls only to flush;
   - ~~division by a constant as a multiplication~~ (native; C compilers already do it);
   - left: arrays (§6), then `print` desugared to std functions, and `rt_write_*` /
     `m_write_*` / `sink_*` deleted.
5. **`print` and `write` as std functions** (§7). The owner chose `nil` and "the first overload
   it fits". ~~Part 1, overloads chosen by type, and `nil`~~ done (context.md §29); std's `assert`
   is now `nil` and arms. Left: parts 2 (`rest: args`, `any`) and 3 (a context of the caller's
   type), then `print` in std.

## 6. The speed gate, measured (2026-11-13)

A million numbers printed (`n * 7919`, one per line), best of 15, arm64 native:

| formatter | before §28 | after §28 |
|---|---|---|
| the compiler's (`rt_write_uint`, C) | 16.1 ms | 10.7 ms |
| Modal, digits by recursion (`digits(n / 10)` first, then `put`) | 44.3 ms | 28.1 ms |
| Modal, tail calls only (a power of ten found, then a digit each turn) | (crashed: no tail calls) | 23.0 ms |
| the compiler's, through C at `-O2` (reference) | 6.8 ms | |

The Modal formatter is 2.1× the compiler's: the gate is not met, and `print` stays the
compiler's. What is left is the algorithm, not the optimizer:
- **The C formatter writes the digits backwards into a 24-byte buffer, then puts them in one
  copy.** Modal has no array, so a formatter in Modal either recurses once per digit (a real
  call each time: it is not a tail call) or finds the largest power of ten first and divides by
  it (a division by a *variable* per digit, about 10 cycles on arm64, against a multiplication
  for `/ 10`).
- **With a local byte array** (`[24]u8`, the owner's open design question in guide.md §6), the
  Modal formatter is the C one, line for line: a tail-call loop storing `u8(48 + n - n / 10 * 10)`
  backwards, then one write of the slice. Every part of it is now a loop in registers or a
  multiplication. That is the next prerequisite, and the gate is measured again then.
- The byte stencil still saves three registers around its rare flush call on every byte (clang
  does not move them to the flush path): a separate cost, worth taking if `put` is ever the hot
  path.

## 7. `print` and `write` in std: the design (step 5)

One proposal. Three features of the language, each useful alone, make `print` an ordinary std
function. None is a keyword or a sigil; each follows a rule that already exists.

1. **Overloads chosen by parameter type** (guide.md §6 lists "chosen by argument count only" as a
   gap). std writes one function per type, as Modal code:

   ```
   nil write(u64 n) { ... digits ... }   nil write(i64 n) { ... sign, then write(u64(...)) }
   nil write([]u8 s) { ... }             nil write(bool true): write("true")
   nil write(u8 n): write(u64(n))        -- and the other widths, each one line
   ```

   Only the type-first form `T name(...)` can be overloaded (the mutability rule), so a function
   with no result needs a type to write first. This depends on the owner's open `nil`/`void`
   question (context.md §10); `nil` above stands for whichever is chosen. It is the reason std's
   `assert` returns its condition today.

   The rule: the overload whose parameter types equal the argument types is chosen; an unsized
   literal takes the first overload it fits, in declaration order; no match is the error it is
   today, listing the candidates.

2. **The rest of the arguments, `rest: args`:** `args` is a predeclared type (like `site`) meaning
   "any number of arguments, of any types, known at each call". A parameter of type `args` is
   compile time (comptime.md §3.2: its type decides). What a body can do with it is pass it on
   whole, so the recursion is over the list itself:

   ```
   nil print(any v, rest: args) {
       write(v)
       write(" ")
       print(rest)
   }
   nil print(any v) {
       write(v)
       write("\n")
   }
   ```

   `any` is the type parameter, inferred from the argument (comptime.md §3.2 without the `T: type`
   written out). Each call site's list of types gets one specialization, cached by that list
   (p.md B5), and the specialization unrolls at compile time. So `print("n is", n)` becomes
   `write("n is") write(" ") write(n) write("\n")`, which is what the compiler's desugaring writes
   today: the same machine code, no run-time cost.

3. **A context parameter of the caller's type:** `nil write(u64 n, out: out)`, in std, names a type
   std does not declare. A context parameter whose type is unknown where it is declared takes the
   caller's `out`, whatever its struct: a type parameter filled by the context rule. The body
   calls `out.put(b)`. Without an `out` in scope, the target's sink is the default, as today.

With these, `rt_write_*`, `m_write_*`, the generated `sink_*` MIR and the checker's `route_output`
go, and `print` keeps its name and path. Order: 1 is needed by 2 and 3 and is the smallest; 2 is
comptime.md step 3; 3 is a small extension of the context rule once 2 exists.

Decided by the owner (2026-11-13): `nil` is the "no result" type, and an unsized literal takes the
first overload it fits. Still open: the names `args` and `any`.

## 8. Literal patterns and arms

Built (context.md §29). One rule carries all of it: **a literal stands where a name would be**, and
then it is a test.

**The patterns:**

| pattern | takes | example |
|---|---|---|
| a number, `-` a number | that value | `u64 fact(u64 0): 1` |
| `lo..hi`, `lo..` | every value from lo to hi, both included; from lo up | `1..9`, `100..` |
| a string | those bytes | `u8 command([]u8 "help"): 1` |
| `true`, `false` | that bool | |
| a name | any value, bound to that name | `n: ...` |

**Arms** are clauses without their head: `pattern: result`, tried in order.

```
u8 command([]u8 s) {      -- a body of arms matches the parameters (self and contexts aside)
    "help": 1
    "quit": 2
    s: 0                  -- a name: the rest, named
}
kind := c {               -- `value { arms }` matches a value; a match is a value or a statement
    48..57: "digit"
    c: "other"
}
c > 40 {                  -- on a bool: if / else, with no keyword
    true: print("above")
    false: {}
}
u64 sum(u64 n, u64 acc) { -- several parameters: one pattern each
    0, acc: acc
    n, acc: sum(n - 1, acc + n)   -- a tail call: a loop
}
```

**Explicit for the developer:** every decision the arms make is visible, and the compiler says
what is wrong in terms of values:
- **Every value is taken, or the program does not compile.** For numbers and bools the compiler
  joins the arms' ranges and names what is missing: `not every value is taken: no arm takes
  10..255`. For strings, the last arm is a name.
- **An arm that can never run is an error**, not dead code: `this arm never runs: the arms
  above already take every value it takes`. That includes a catch-all after literals that
  already cover the type.
- **A catch-all has a name**, which binds the value (`s: 0`); there is no anonymous wildcard.
  When literals cover the whole type, none is needed, and the last arm has no test at all.
- **A match used as a value gives one type**; the error names the arm that differs.
- **No fall-through at run time:** with every value taken, there is nothing to trap.

**Cost:** the same tests as clauses (a compare and a branch per literal, two per range; a string
is its length, then eight bytes at a time), and no call. Arms on the parameters are one function,
so a loop written with them is a loop in registers.

**Limits now:** a match at module level (the scanner keeps a module-level `{` as a function body;
put the match in a function and bind the call); guards (`n if n > 3`), which would need a keyword
or a sigil and are left out while ranges cover the cases; tuple subjects (`(a, b) { ... }`)
outside a function's parameters. Next for speed: string arms dispatched by length first, and dense
number arms as a jump table.

## 9. Arrays (in progress)

The design, following the position rule:
- `[N]T`: N elements of T, N known at compile time (a literal or a constant). `[24]u8 buf` is a
  mutable local, zeroed; `a[i]` reads, `a[i] = v` writes, with a bounds check that traps like a
  division by zero (and is a compile error for a constant index).
- **An array parameter is a reference**, mutable when written type-first (`[24]u8 buf`), read-only
  type-last (`buf: [24]u8`). Loops are recursion, so a loop that fills an array is a function
  that takes it: copying it on every turn is what this avoids.
- `s[i]` on a `[]u8` reads a byte (read only), bounded by its length.
- Later modalities (the owner's list): `[..N]T` (a length from 0 to N, in place), and grow-only,
  shrink-only and grow-and-shrink arrays, which need an allocator (the `arena` context).

Done: the type, parsing, checking of indexes and element assignment. Not yet: code generation.
