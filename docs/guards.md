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
2. **Stack overflow:** a check of the value stack's top at each call into a recursive cycle
   (the call graph already knows which ones), trapping with kind `stack`.
3. **The `fault` context:** routing, the boundary (resetting the value stack's top, and the
   native stack to the boundary's frame), and the target defaults.
4. ~~**`assert` as a std function**~~ done (context.md §24): in a body it traps with
   `file:line:col: assertion failed: <condition>`, status 1, the same on every backend; at
   module level a failure is a compile error. Left: `--stats` listing the fault points, and the
   `fault` context (step 3) receiving it with kind `assert`.

## 4. Open question for the owner

What a boundary function returns after a fault: its declared return type with a value the
`fault` chose (simple and explicit; the `trap` method returns it), or a result type that
says "this call failed" (more checking for the caller). The first fits Modal today; the
second needs sum types, which Modal does not have.
