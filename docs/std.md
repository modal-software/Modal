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
| Modal, a `[24]u8` buffer filled backwards (§35) | | 24.4 ms (21.6 with the indexes proven by arms) |

With arrays (§35): the compiler's formatter 11.4 ms, the Modal one with a buffer 24.4 ms, tail calls
only 23.8. Still 2.1x: what is left is per byte. Each digit goes through the `write.byte` stencil
(its three register saves around the rare flush, guards.md / plan Phase E), and each number makes
three calls (`write_uint`, `fill`, `emit`; a function with a local array keeps its frame, so it
is not inlined). Bounds checks are 3 ms of it. The next steps are the inline byte sink in
register mode and a write of a whole range of bytes; the gate is measured again then.

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

Built (context.md §29–§31), in the form the owner chose on 2026-11-13. The rules:
- **A match is `if value { arms }`** (the owner's choice, 2026-11-13, over `value: { }`, which read
  like a declaration). The block holds arms when its first line is one; no statement starts with a
  pattern then `:`, so the two `if` forms never mix. Arms are separated by a comma or a newline,
  and take no `else` (`_` is the rest). As a value, `word := if c { ... }`; an `if` with a
  condition gives no value.
- **`:` takes a value; only `=` assigns.** `pattern: result`, `name: T`.
- **A pattern is a literal, `_` or an existing name:** a number (`-` included), a range (`1..9`,
  `100..`, both ends included), a string, `true` / `false`, `_` (every value), or a name that is
  already declared (`limit: ...`), which takes the value equal to it. A pattern never declares:
  inside an arm the value keeps its own name. A constant's name (`limit: u8 = 50`, `space := 32`)
  is checked like its literal (overlap, coverage, range fact) and costs the same; a local's or a
  parameter's is compared when the program runs, in the arm's place, and the compiler counts
  nothing from it. Naming the value being matched is an error (write `_`); a name on a `[]u8` and
  a name in a range are not supported yet.
- **Which block is arms:** one whose first line is a pattern then `:`. `name: T = v`, `name: u8`,
  `name: []T`, `name: *T` and `name: struct` stay declarations, so `if ok { x: u8 = 3 ... }` is a
  block. An arm holds one statement or a block: a second line under an arm is an error that says
  to use braces (`50: { a()  b() }`). `if (c) { 50: ... }` matches c (the parentheses group); in
  `if a if c { arms } else x` the `else` is the outer `if`'s.
- **Parentheses group** (they structure or destructure, as for parameters): several values take
  one pattern each, `(0, _): acc`. A lone `_` takes every value of all of them, so no `(_, _)`.
- **`if cond branch else branch`** is the statement for conditions (a branch is a block or one
  statement; `else if` chains). Arms are for values, `if` for conditions: neither repeats the other.

```
u8 c = 0
if c {                        -- a match: `if`, the value, its arms
    48..57: print("digit", c - 48)
    32: print("space")
    _: print("other")
}

u8 command([]u8 s) {          -- a body of arms matches the parameters (self and contexts aside)
    "help": 1
    "quit": 2
    _: 0
}

u64 sum(u64 n, u64 acc) {
    (0, _): acc
    _: sum(n - 1, acc + n)    -- a tail call: a loop in registers
}

word := if c {                -- a match is a value when every arm gives one
    48..57: "digit",          -- arms: a comma or a newline between them
    _: "other"
}

if c > 40 print("above") else {}
```

**Built for speed, through predictability** (the owner's direction). Every rule below is checked
at compile time, so what the program does at run time is known before it runs:
- **Arms take disjoint values.** An arm that shares values with one above is an error that names
  them (`this arm overlaps an arm above on 5..9`); one that adds nothing never runs and is an
  error too. So the order of the arms decides nothing but where `_` goes, and the compiler is free
  to test them in any order: by length first for strings, a jump table for dense numbers, a
  search for sparse ones. None of that changes what the program means.
- **A match that gives a value takes every value, or it does not compile** (`no arm takes
  10..255`); so does a body of arms that returns one. There is no fall-through and nothing to trap
  at run time. When literals cover the whole type, `_` is not needed and the last arm has no test.
- **A statement may leave values untaken** (the owner's rule): `if c { 50: print("fifty") }`, or a
  `nil` body of arms, does nothing for the others. That is a warning, kept for `--warn` (later, the
  build's setting); the last arm keeps its test.
- **`return if n { 0: acc, _: s(n - 1, acc + n) }`** is lowered as arms that each return, so the
  call is a tail call (a jump), as in a body of arms. (Before §33 it took a frame per turn and a
  100M-turn loop overflowed the stack.)
- **What an arm takes is a fact the compiler uses.** Inside `1..9: 100 / n`, n is known to be
  1..9, so the division has no zero guard (native backends), unless the arm assigns to n. Array
  bounds checks will use the same fact.
- **The cost today:** a compare and a branch per literal, two per range (the type's own ends
  skipped), a string's length then eight bytes at a time; no call. Arms on the parameters are one
  function, so a loop written with them is a loop. Measured with `make bench`: no change in code
  size or time against §28. A 100M-turn classify loop written with arms and with a hand-written
  `if` chain: 179 ms both, the same MIR (§33).
- **Dispatch by search** (built, §34): with one integer value and four or more arms the compiler
  knows, the first arm keeps its own test, in place (the order written says which to try first:
  a hot arm written first costs one compare and no jump), and the others are found by a binary
  search over the sorted values, gaps going to `_`. 7 arms, 100M turns: 228 -> 213 ms scrambled,
  209 -> 184 sequential; a hot first arm: the same code as before. `MODAL_TREE=0` turns it off,
  to compare. A jump table and string arms by length are not built: the search already takes
  log2 compares, and strings need arrays to be measured.
- **Inlining into loops** (§34): a body of up to 32 instructions, several returns included (each
  arm's becomes a jump to the end), is inlined into a function that calls itself; elsewhere the
  limit stays 16 (32 everywhere grew structs from 3,016 to 7,228 B and big's emit by 19%). A
  4-arm classify called in a 100M-turn loop: 178 -> 114 ms.
- **Several values:** `if a, b { (0, _): ..., _: ... }`, one column each; a condition is a value
  (`if n, n > limit { (_, true): ... }`).
- **At module level:** `family: []u8 = if target.arch { "arm64": "arm", _: "other" }`: the
  compiler takes one arm; the others are not in the program.
- **Names at a range's ends:** `lo..hi`, `-limit..-1`, `0..limit`, `limit..`, for constants only
  (a mutable module variable is an error there; as a single-value pattern it is compared when the
  program runs).
- **Clauses written as separate definitions** follow the arms' rules: two whose only pattern is in
  the same parameter take disjoint values (`u8 f(u8 1..9)` and `u8 f(u8 5..20)` is an error); a
  `nil` set may leave values to nobody (a warning; the call does nothing for them). Patterns in
  several parameters (`pick(0, y)`, `pick(x, 0)`) still overlap, first one wins, as in arms.

**Limits now:** `if` with a condition as a value; a name pattern on a `[]u8`.

## 9. Arrays (fixed width: built, §35)

- `[N]T`: N elements of T (a number or a bool), N known now: a literal or a module constant
  (`[N]u16 b` with `N := 4`). `[24]u8 buf` is a mutable local, zeroed; `a[i]` reads, `a[i] = v`
  writes, in the element's width.
- **Bounds:** an index out of range stops the program with its place (`f.modal:42:12: index out of
  range (0 to 3)`), on every backend; a constant index out of range is a compile error. No check
  where the compiler knows the index is in range: a literal, or a local an arm took
  (`return if i { 0..3: a[i], _: 0 }` is an add and a load).
- **Parameters, as built** (it differs from the earlier draft, which made a type-first array
  parameter a reference the caller saw written; for the owner to confirm):
  - `a: [4]u8` (type-last): read in place, by its address; nothing is copied.
  - `[4]u8 a` (type-first): the function's own copy, so writing it changes nothing of the
    caller's (value semantics, as for every other type).
  - `p: *[24]u8` with `&b` at the call: the caller's array, written in place. The `&` says at the
    call that it will change. This is how a loop fills a buffer (`fill(&b, n, 23)`).
- **Locals (the owner's rule, 2026-11-13):** without `&` it is a copy, with `&` a reference, and
  the type is inferred: `c := a` (as `c: [4]u8 = a`, `[4]u8 c = a`) copies; `r := &a` is a
  `*[4]u8`. `@` for references was proposed and rejected (counter-intuitive).
- `s[i]` on a `[]u8` reads a byte (read only), bounded by its length.
- **Later modalities** (the owner's list): `[..N]T` (a length from 0 to N, in place), and
  grow-only, shrink-only and grow-and-shrink arrays, which need an allocator (the `arena`
  context). Built since: arrays of structs (context.md §62), `xs.len` and `xs: []T` parameters,
  arrays as results, written in the caller's place (§64). Not yet: array literals, slices.

## 10. Struct literals and visibility by use (built, context.md §36, §37)

The owner's decisions (2026-11-13): literals take their type, `point{ x, y }`; a member alone is
`name: name`. A second decision the same day removed the face line: a struct is written once,
everything is private by default, and **use decides what is public** — a member (field,
method, module) that another domain uses is public, recorded while compiling. Nothing is
written for it.

```
span: struct {
    lo: i32,
    hi: i32,
    u32 reads,
}

new(self: span, lo: i32, hi: i32): *span {
    return &span{ lo, hi, reads: 0 }   -- built in self's storage
}
```

- **Inside a domain** (a folder, as Go's package) every member is seen, and a literal is
  written in any of its files.
- **From another domain** every member can be used; the use makes it public. `--syms` says
  `public` or `domain` per member, and `--sig` lists, per module, exactly what other domains use
  (fields with their type, `(writable)` when type-first). Any module of a domain in reach can be
  imported directly (`import("./geo/shapes")`); mod.modal bindings are names, no longer a
  gate. A domain's mod.modal configuration and functions stay compile time only.
- **Writing** follows position everywhere: a type-last field is never assigned.
- **A method is called**, `s.width()`: `s.width` is an error that says so. The face aliases
  (`len: width`) are gone with the face line.
- **A lock** (a member kept private even when another domain tries to use it) is not decided:
  it is studied apart and brought as one proposal.
- **A literal** gives every field, each once, with values that fit. `&T{ ... }` is a local's
  value, `p: *T = &T{ ... }` (the literal lives in the function's frame; returning p, or a local
  that holds it, is an error), or a constructor's result, `return &T{ ... }` in a method with
  `self: T`, built in self's storage. A value is never passed or returned as a reference by
  itself: `&x` makes one.
- **Across files** the backends generate structs of every file (`m_u<file>_<name>` in C) and
  call `T.m()` through an import binding (`examples/run/domains/`). A struct held by value in
  another struct runs on C; native does not support it yet (Phase D).
- **Speed:** a literal local used only as `p.field` keeps each field in a slot of its own (a
  register): no memory, so a loop that builds one stays a loop. Measured, 100M constructions:
  53 ms against 607 ms for the constructor and its block (§36). Visibility by use costs nothing
  at run time: it is one flag per symbol, set while checking.
- **Fields by use** (context.md §41): fields are laid out largest alignment first; a field
  nothing reads has no bytes; a field every write gives one constant is that constant. A
  register block, and a struct `sizeof`/`alignof`/`offsetof`/`fieldsof` names, keep every field
  as declared. `--audit` lists what was dropped or folded.

## 11. Reflection (built, context.md §40)

The owner's goal (2026-11-13): type introspection at compile time and at run time, for flexible
functions, with no cost to speed and nothing that helps reverse engineering.

- **`type` is std's** (std/type.modal, the owner's choice 2026-11-13: reflection is a value
  built in std, not a keyword): `type: struct { id: u64, size: u64, align: u64, fields: u64 }`,
  read only. `typeof(X)` gives one: X a type or a value (constants, folded where used:
  `typeof(point).size`, `pid := typeof(point).id`), or an `*any` (read from its descriptor at
  run time: `typeof(a).id`, `t := typeof(a)`). A type `typeof` names keeps its declared layout.
  **`T: type` parameters** (built, context.md §47): a parameter of std's type takes a type name
  (`bytes(point, 4)`, `describe(u32)`) whose descriptor the compiler makes, or any `type` value
  (`describe(typeof(a))`, known at run time). In the body T is a value: `T.size`, `T.id`.
  `v: T` (a parameter typed by another, or the result) makes the function one per type given
  to it: `first(point, p, q)` and `first(u32, 3, 4)` are two functions, each checked with T
  bound (context.md §48). No marker: the use says T is known while compiling.
  **Type variables** (the owner, 2026-11-15; context.md §67): std/type.modal declares `T: type`
  (no value), the default one; a program or module declares its own the same way (`U, A: type`).
  A parameter typed by one makes the function generic with no `T: type` parameter: the call
  reads T from the argument in that place (`max(a: T, b: T)`, `first(xs: []T)`, `p: *T`, the
  result of a function given, `f: function(T): U`). So `map(c, v: v + 1)`,
  `fold(xs, u64(0), (s, v): s + v)`. The arguments must agree; an untyped number gives no type.
  A name that is not declared `: type` is never a type variable (a typo stays an error).
  `T: type` as a parameter stays for a type no argument has: `zero(T: type): T`, `zero(u64)`.
- **Type queries**, predeclared names like `sizeof` (never keywords; a program's own name wins):
  `sizeof(T)`, `alignof(T)`, `fieldsof(T)` (how many fields; 0: not a struct),
  `offsetof(T.field)`, `typeof(T)`. Each takes a type or a value (its type). Each is an unsized
  constant of the target's layout, folded where it is used: in module-level asserts (checked by
  the compiler), in constants, in bodies. Nothing is left for run time.
- **Dynamic resolution** through `typeof`: a number per type of the program, the same in every
  backend of one build (not stable across builds: compare it with `typeof(T)`, never store it
  outside the program). Named, it is an arm pattern, so dispatch over types is an `if` of arms
  (and gets the arms' jump tables and searches):
  ```
  pid := typeof(point)
  kind(u64 id): u8 { if id { pid: return 1, _: return 0 } }
  ```
- **No names in the binary:** no query gives a name, and native binaries carry no symbol table.
  A binary written with `-o` carries no source either: a guard says `trap N: division by zero`
  and `<out>.traps` (written next to it) gives N's place and call. `--run` shows the text.
- **`*any`** (std/any.modal, `any := import("std").any`): a reference that carries the type of
  what it points to, as a pair (address, typeof) in the frame that made it. Any reference goes
  where `*any` is expected (`a: *any = &p`, an argument); `typeof(a)` is read at run time and
  compares with `typeof(T)`, so an `if typeof(a) { pid: ..., _: ... }` dispatches over types;
  `point(a)` gives the `*point` back and stops the program when a holds another type (the stop
  names no type). The pair is (address, the type's descriptor): `any: struct { u64 at, *type of }`. An `*any` made in a function is not returned from it. Built: context.md §45.
- **Code once per field** (context.md §49, §50): `field(T, &v, i)` (std, written in Modal over
  `load`) reads field number i, i known while compiling. A parameter given to it is known while compiling (no marker), so the function is
  made per value; arms on those values are decided while compiling, which ends a recursion:
  `(_, _, fieldsof(T)): 0` then `_: u64(field(v, i)) + soma(T, v, i + 1)`.

## 12. `--audit` (built, context.md §39)

`modal --audit file`: the domains each one uses and what of them; the functions main does not
reach, and those only the compiler runs; every guard left at run time with its place and reason;
recursion that is not a tail call; the writes to the system. It is made of what the compiler
knows anyway, and a normal compile does none of it.

## 13. A call bound to a name (built, context.md §51)

The owner's decisions (2026-11-14): `a := loop(u64, 1)` then `a()` holds the call; **use
decides**, the arguments are computed **at the binding**, and `print(a)` shows the function's
**descriptor with its name**.

- **Use decides:** `a := f(x)` is a bound call when the block that binds it calls `a()` after it
  (read from the tokens, as the fields-by-use scan reads uses). Without an `a()`, `a` is the
  result, as before: no existing program changes. No word or symbol is added.
- **Arguments at the binding,** copied into the frame (as Go's `defer` arguments): `s := sum(p,
  k)`, then `k = 100`, then `s()` sums with the old k. A method's receiver is a reference (self
  always is), so `w := sp.width()` sees later writes to sp. An array argument is refused (it is
  passed by its address, which is not its value at the binding).
- **No cost:** `a` is immutable, so the function is known while compiling. `a()` is a direct call
  (or the body inlined) with the bound values: no function pointer, no closure object. `return
  a()` is a tail call like any other.
- **A call with no result** can be bound (`v := show(3)`, `v()`): the binding has no value of its
  own, so the "no value" error does not apply.
- **`print(a)`** gives `function name(param types): result` (`function span.grow(i32): i32`), the
  specialization's types for a generic function. The name is in the binary only because the
  program printed it; a program that prints none carries none (§11's rule).
- **`f := double`** (a function named alone) is that function: `f(21)` calls double, and
  `print(f)` shows it the same way.
- **Not yet:** a function value passed to or returned from a function (a table of handlers),
  which needs addresses at run time (comptime.md: run time only); `a(more)` adding arguments; a
  std `function` struct read field by field (as `type`).

## 14. Pipe stages (built, context.md §53)

The owner's notes in run/arms.modal and decisions (2026-11-14). No word, no operator: `.(`.

```
word := c.(48..57: "a digit", _: "not a digit")   -- a match as a stage
x := u64(5).(double).(double)                     -- x.(f) is f(x): 20
n.(0..10: n + 1).(seven()).(print).(_: print("big"))
```

- **A stage of arms** is a match on the value before it.
- **Every pattern stage of a chain tests the chain's head** (rule 1, option a): the arms of
  later stages are arms of the same match, alternatives to the first ones ("parallel" in the
  owner's words: never concurrency). `n.(0..10: a).(_: b)` is `if n { 0..10: a, _: b }`.
- **A stage without a pattern continues the arm opened last.** A name alone is a function
  given the value (`.(print)`); anything else runs after it and its value goes on
  (`.(seven())`). Before any arm it applies to the value itself. After a stage of several
  arms it is an error (which arm?): put it inside the arm. A match on an intermediate value
  goes inside the function, by convention, or in a nested chain (`f(n).(0: ..., _: ...)`).
- **A stage is partial:** what no arm takes goes on, as the stage's value (it must fit the
  arms' type, or end with `_`), or nothing for a statement. `if value { arms }` stays
  exhaustive.
- **A condition is a pattern** (context.md §65): `x.(x > hi: hi)`, `a2.len.((a1.len > a2.len): 0)`,
  `if n { 0: "none", n < 10: "few", _: "many" }`. Written as a comparison (or `and` / `or`), alone
  or in parentheses; the arm is taken when it is true. It is tested in its place, first wins, and
  takes no values the compiler counts: a stage stays partial, a value match still needs `_`.
- **Cost:** the same AST_MATCH as `if` (tables, searches); a value that goes on is one copy. A
  match with a condition is tested in order (no search over its arms).
- **Not yet:** type patterns (`u64:`, `error:`) and error names, with guards.md §5; arms in
  function literals passed to calls (`john.map(x: x.id)`).
