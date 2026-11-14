# Modal: guards and faults

Status: 2026-11-12. What can go wrong in a running Modal program, what the compiler checks
before it runs, and the design for recovering from a fault (Go's `recover`, made explicit).
Companions: `docs/comptime.md` (compile-time guards are the same functions, by position).

---

## 1. Where things stand

### 1.1 Compile time (all exist)

- **Constants are checked against their types:** `x: u8 = 300`, `f(256)` for a u8 parameter,
  `x := 3000000000` for an `int`.
- **A literal zero divisor is an error:** `a / (2 - 2)`.
- **Capabilities:** a program that needs what its target lacks (system output on a board) is
  an error at that line.
- **Memory budgets:** the value stack is sized from the call graph, and `max_code` /
  `max_ram` come from `mod.modal`. A program that does not fit the chip does not compile.
- **Devices and register blocks:** offsets, widths, alignment, overlap, address space.
- **Every body with a return type must reach a `return`.**

### 1.2 Run time

| Event | Behaviour (every backend, the same) |
|---|---|
| `+ - *` overflow, unary `-` | wraps in the declared type (by design). The C backend now computes in `uint64_t` and converts back, so it is no longer undefined in C. |
| smallest signed value `/ -1` | wraps (as `0 - x` does). It used to trap on x86-64 and was undefined in C. |
| division by zero | **a trap:** the output written so far, then `file:line:col: division by zero` on the error output, status 1. On a board: the halt, with 1 in x0 / edi. It used to give 0 on arm64, fault on x86-64, and be undefined in C. |
| recursion deeper than `stack` | **not checked yet:** it runs over the value stack. |
| `nil` dereference | not checked yet. |
| index out of range | no indexing yet. |

How a trap is built:
- The MIR checks a divisor that is not a known non-zero constant (`JNZ`, then `MIR_TRAP` with
  the message as a string in the code).
- The `trap` stencil calls `rt_trap`, which flushes the program's output buffer, writes the
  message and calls `exit(1)`.
- `exit` is the system's on macOS (imported only by programs that can trap), `exit_group`
  on Linux, and the halt on bare metal.
- The C backend calls `m_div_<type>`, which does the same with stdio.

---

## 2. Design: faults go to a `fault`, the way output goes to an `out`

*Superseded by §5 (the owner, 2026-11-14): errors are values, bugs are panics, and the boundary
is a call bound with `, e`. Kept for the history of the decision.*

Modal already routes output by a context parameter: every byte printed goes to the `out` in
scope, or to the target's output if there is none. Faults get the same rule, with no keyword:

```
fault: struct {
    u32 count,
}

trap(self: fault, kind: u8, at: []u8) {
    self.count = self.count + 1
}

serve(fault: fault, req: u32): u32 {
    return 100 / req              -- a zero divisor goes to the fault in scope
}

main(): u8 {
    fault := fault.new()          -- this function is the recovery boundary
    ...
}
```

- **A trap goes to the `fault` in scope**, a struct with `trap(self: fault, kind: u8, at: []u8)`.
  `kind` names the fault (division, later stack, bounds, nil) and `at` is the place.
- **The recovery boundary is the function that declared the `fault`.** The frames above it
  are dropped: they live on the value stack, so dropping them is resetting its top. That
  function's call then ends with a value of its return type, the fault being a value it can
  check (the shape of that value is the open design question, §4). This is Go's `recover`,
  but placed by the caller, visible in the source, and with no `defer` to remember.
- **No `fault` in scope** means the target's default: hosted targets stop with the message and
  status 1 (as now); a board halts, or resets through its watchdog (a `mod.modal` choice).
- **Context parameters already reach every function that needs one** (calls fill them in
  from the caller), so a library function does not change its signature to be guarded.

### 2.1 Toward eight nines

Reliability past five nines comes from **supervision**, not from avoiding every fault
(Erlang/OTP): a part that fails is restarted in a known state while the rest goes on. In Modal
terms:

- the boundary is a function, its state is what it was given (values, and the context
  parameters it received), and restarting it means calling it again;
- because module-level memory is fixed at compile time and frames are on the value stack,
  a restart leaks nothing: there is no heap to clean;
- every fault point is known to the compiler. `--stats` will list them per function (where a
  division, later an index, may trap), so a reviewer can see which code can fail and what
  supervises it.

### 2.2 Run-time guards by position

`assert` becomes a std function (comptime.md §3.5). At module level it runs in the compiler,
and a failure is a compile error. In a body it is a run-time check that traps with kind
`assert`, to the `fault` in scope. One function, and the position decides the time.

---

## 3. Steps

1. **Done:** wrapping defined in C; division by zero and `INT_MIN / -1` the same everywhere;
   a literal zero divisor is a compile error.
2. **Stack overflow:** a check of the value stack's top at each call into a recursive cycle,
   trapping with kind `stack`. Analysed 2026-11-12 (context.md §27); not built yet:
   - **When the cycles are known:** the native backend generates each function as soon as it is
     checked, and learns whether the program recurses only at the end (`stack_need`). A guard
     that costs nothing in programs that do not recurse must therefore be patched out after the
     fact (a reserved sequence turned into a jump), or every function that calls another must
     check. The second costs a load, a compare and a branch per call. Speed comes first, so it
     is the patched form.
   - **The same depth on every backend:** the ground rule asks for the same output, and a
     program that overflows prints what it printed before the trap. So the limit must be in
     calls, `stack / largest frame of the cycle`. The C backend would then need the MIR frame
     sizes: it does not lower MIR today.
   - Today an overflow runs over the value stack on the native backends, and over C's stack in
     the C backend.
   - **Since context.md §28 loops do not use the stack:** a tail call (a clause calling its
     function again, the clause chain, `return f(...)`) is a jump on every backend. What can
     still overflow is recursion that is not a tail call (`n + f(n - 1)`), which the guard is for.
     A tail call needs no check, so the guard goes only on the calls that stay calls in a cycle.
3. **The `fault` context:** routing, the boundary (resetting the value stack's top, and the
   native stack to the boundary's frame), and the target defaults.
4. ~~**`assert` as a std function**~~ done (context.md §24): in a body it traps with
   `file:line:col: assert(<condition>) is false`, status 1, the same on every backend. Since
   context.md §26 it is std code in Modal, on the `trap` and `site` primitives (docs/std.md). At
   module level a failure is a compile error. Left: `--stats` listing the fault points, and the
   `fault` context (step 3) receiving it with kind `assert`.

## 4. Open question for the owner (answered in §5)

What a boundary function returns after a fault: its declared return type with a value the
`fault` chose (simple and explicit; the `trap` method returns it), or a result type that
says "this call failed" (more checking for the caller). The first fits Modal today; the
second needs sum types, which Modal does not have.

## 5. Errors and panics (decided by the owner, 2026-11-14)

Go's form, Zig's inference, Erlang's supervision; no keyword, no operator, no garbage collector.

```
print := import("std").io.print
error := import("std").error
panic := import("std").panic

bad_input: error                       -- an error of the program: a name, one word at run time

parse([]u8 s): u64, error {            -- it can fail: `, error` in the result (type last)
    "": return bad_input
    _: return digits(s)
}

double(u64 n): u64 { return n * 2 }

serve([]u8 s): u64, error {
    n := parse(s)                      -- not handled: an error goes up (serve says `, error`)
    return 100 / n                     -- n may be 0: panic.division can happen here
}

main(): u8 {
    n, e := serve("42")                -- the boundary: a panic below comes back here
    if e {                             -- `if` is exhaustive
        nil: print(n)
        bad_input: print("empty")
        panic.division: print("zero")
    }
    m := parse("").(u64: double, error: 0)   -- a pipe stage is partial: by the type
    print(m)
    return 0
}
```

- **`error`** (std): one word, the error's number and the place it was made; `nil` (0) is ok.
  A program's errors are module-level `name: error`; their numbers are the compiler's.
- **Saying it can fail:** a function that returns an error, or lets one go up, has `, error` in
  its result (`f(...): T, error`, or `f(...): error` with no value), always type last. Which
  errors is inferred (Zig's inferred sets); `--sig` lists them.
- **Panics** are bugs: `panic("why")` (was `trap`), and the std's `panic.division`,
  `panic.index`, `panic.assert`, `panic.anytype`, `panic.unhandled`, `panic.raised`, later
  `panic.stack`. They need no declaration. A call's set holds a panic only when the compiler
  could not prove it away (what `--audit` lists as "can stop at run time").
- **The caller:**
  - `n := f()`: not handled; an error goes up (the function needs `, error`), a panic too.
  - `n, e := f()`: the boundary. On an error or a panic below, the frames under it are
    dropped, execution recovers here, `n` is its type's zero and `e` the error. An `e` that is
    never handled is itself `panic.unhandled` when an error reaches it.
  - `if e { ... }` is exhaustive against the set; a pipe stage `f().(...)` is partial (what no
    arm takes passes through; an error no arm takes goes up).
  - **Type patterns:** on a call's result, `u64:` tests the value, `error:` any error, a name
    (`bad_input:`, `panic.index:`) one error. A function named in an arm receives the value
    (`u64: double`).
- **Hybrid `fault`:** a program may declare `fault` with `trap(self: fault, error e)`, run on
  each error before it reaches its handler (count, log); otherwise nothing runs.
- **Arena:** no rollback is written. The usage-analysis pass decides it: on an error, an arena
  goes back to its mark at the call when nothing allocated in the call escaped; `--audit`
  says which, and why not.
- **Cost:** a second register and a compare per call only for functions with `, error`; at a
  boundary a few stores in, one out; nothing at panic sites; a boundary whose call cannot
  fail is removed (a warning says `e` is always nil).
- **Build order** (pipe ops first: the error forms are written with them):
  1. pipe ops (plan Phase D1): `x.(f)`, `x.(arms)`, chains, rule 1 (a), partial stages;
  2. names on the left (`a, b :=`);
  3. errors returned and propagated (`, error`, `return bad_input`);
  4. type patterns (`u64:`, `error:`) and error names in arms;
  5. the boundary on native and C, `panic` replacing `trap`, `panic.unhandled`;
  6. the inferred sets in `--sig`;
  7. the arena decision by the usage-analysis pass.
