#!/usr/bin/env python3
# fuzz.py (v2 tool): differential testing of the backends on random programs.
#
#   python3 -I src/v2/tools/fuzz.py [N] [SEED]        BIN=./modal by default
#
# Each program is run four ways, which must agree on everything they print (output and error
# output) and on the exit status (a signal counts as 1):
#   the C backend (the reference)   native --run (in memory)   native -o + exec   x86-64 -o + exec
# The programs use every integer type, every local form (`r: T = e`, `T r = e`, `T r` then
# assigned), recursion through pattern clauses, `print`, and divisions whose divisor may be zero
# at run time (a trap); 40% are struct programs (fields stored several times, read back, copied
# and passed by value), for the optimizer's memory passes; 20% are register-pressure programs
# (more values live at once than registers, across calls), for the register allocator.
# Mixed programs also bind calls at module level (the compiler runs them, docs/comptime.md) and
# print whether each equals the same call made at run time: `ct false` is a difference.
# Programs the checker rejects (a constant that does not fit) are skipped.
# MODAL_REGS=2 in the environment generates every function with registers, MODAL_REGS=0 none.
# A program that differs is kept in the work folder and printed; the others are removed.
import os, random, subprocess, sys, tempfile

BIN = os.environ.get("BIN", "./modal")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 1
random.seed(SEED)
WORK = tempfile.mkdtemp(prefix="modal-fuzz-")
KIND = os.environ.get("FUZZ_KIND", "mix")  # mix, struct, scalar, pressure
TYPES = ["u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"]


def expr(vs, depth, div):
    if depth <= 0 or random.random() < 0.3:
        if vs and random.random() < 0.6:
            return random.choice(vs)
        return str(random.randint(0, 200))
    op = random.choice(["+", "-", "*"] + (["/"] if div else []))
    right = expr(vs, depth - 1, div)
    if op == "/" and not any(v in right for v in vs):
        right = random.choice(vs) if vs else "1"  # a divisor that can be zero only at run time
    return "(%s %s %s)" % (expr(vs, depth - 1, div), op, right)


def body(t, e):
    form = random.randint(0, 3)
    if form == 0:
        return ["    return %s" % e]
    if form == 1:
        return ["    r: %s = %s" % (t, e), "    return r"]
    if form == 2:
        return ["    %s r = %s" % (t, e), "    r = r + %d" % random.randint(0, 9), "    return r"]
    return ["    %s r" % t, "    r = %s" % e, "    return r"]


def call_args(n, rec, t):
    if rec == "tail":  # deep: thousands of turns, in the stack of one call
        top = {"u8": 255, "i8": 127}.get(t, 3000)
        return [str(random.randint(0, top)), str(random.randint(0, 120))]
    if rec:
        return [str(random.randint(0, 30))]
    return [str(random.randint(0, 120)) for _ in range(n)]


def program():
    out = ['print := import("std").io.print', ""]
    fns = []
    div = random.random() < 0.5
    types_of = {}
    for i in range(random.randint(1, 5)):
        t, name = random.choice(TYPES), "f%d" % i
        types_of[name] = t
        types_of[name + "_copy"] = t
        roll = random.random()
        if roll < 0.15:  # a loop: the recursion is a tail call (a jump), with an accumulator
            step = "%s(n - 1, %s)" % (name, expr(["n", "acc"], 2, False))
            if random.random() < 0.5:  # as clauses
                out.append("%s %s(%s 0, %s acc): acc" % (t, name, t, t))
                out.append("%s %s(%s n, %s acc): %s" % (t, name, t, t, step))
            else:  # as arms on the parameters, sometimes with a range first
                lo = random.randint(1, 9)
                out += ["%s %s(%s n, %s acc) {" % (t, name, t, t), "    0, acc: acc"]
                if random.random() < 0.5:
                    out.append("    1..%d, acc: acc + %d" % (lo, lo))
                out += ["    n, acc: %s" % step, "}"]
            fns.append((name, 2, "tail"))
        elif roll < 0.4:
            out.append("%s %s(%s 0): %s" % (t, name, t, random.randint(0, 9)))
            out.append("%s %s(%s n): %s + %s(n - 1)" % (t, name, t, expr(["n"], 2, False), name))
            fns.append((name, 1, True))
        else:
            ps = ["p%d" % j for j in range(random.randint(1, 3))]
            text = body(t, expr(ps, 3, div))
            out.append("%s(%s): %s {" % (name, ", ".join("%s: %s" % (p, t) for p in ps), t))
            out += text + ["}"]
            fns.append((name, len(ps), False))
            if random.random() < 0.3:  # the same function under another name: identical-code folding
                out += ["", "%s_copy(%s): %s {" % (name, ", ".join("%s: %s" % (p, t) for p in ps), t)] + text + ["}"]
                fns.append((name + "_copy", len(ps), False))
        out.append("")
    out += ["r(x: u8): u8 {", "    return %s" % expr(["x"], 2, False), "}", ""]
    # Compile time: the same call bound at module level (the compiler runs it) and made in the
    # body (the program runs it) must be equal: `ct false` in the output is a difference.
    cts = []
    for k in range(random.randint(0, 3)):
        name, n, rec = random.choice(fns)
        t = types_of[name]
        args = call_args(n, rec, t)
        call = "%s(%s)" % (name, ", ".join(args))
        out.append("ct%d: %s = %s" % (k, t, call))
        cts.append(("ct%d" % k, call))
    out += ["", "main(): u8 {"]
    for name, call in cts:
        out.append('    print("ct", %s == %s, %s)' % (name, call, name))
    for _ in range(random.randint(1, 6)):
        parts = ['"v"']
        for _ in range(random.randint(1, 4)):
            name, k, rec = random.choice(fns)
            args = call_args(k, rec, types_of[name])
            parts.append("%s(%s)" % (name, ", ".join(args)))
        out.append("    print(%s)" % ", ".join(parts))
    out += ["    return r(%d)" % random.randint(0, 255), "}"]
    return "\n".join(out) + "\n"


def struct_program():
    # A struct whose fields are of two types; methods store fields (several times), read them
    # back, and return the struct by value; main copies, passes and prints them.
    ta, tb = random.choice(TYPES), random.choice(TYPES)
    nf = random.randint(2, 4)
    ftype = [random.choice([ta, tb]) for _ in range(nf)]
    out = ['print := import("std").io.print', "", "st: struct {"]
    out += ["    %s f%d," % (ftype[i], i) for i in range(nf)]
    out += ["}", ""]

    def fexpr(t, extra, depth=2):
        vs = ["self.f%d" % i for i in range(nf) if ftype[i] == t] + extra
        return expr(vs, depth, random.random() < 0.3)

    out.append("init(self: st, x: %s): st {" % ta)
    for i in range(nf):
        out.append("    self.f%d = %s" % (i, fexpr(ftype[i], ["x"] if ftype[i] == ta else [], 1)))
    out += ["    return self", "}", ""]
    nm = random.randint(1, 3)
    mtype = []
    for j in range(nm):
        t = random.choice([ta, tb])
        mtype.append(t)
        out.append("mut%d(self: st, v: %s): st {" % (j, t))
        for _ in range(random.randint(1, 3)):
            i = random.choice([k for k in range(nf) if ftype[k] == t] or [None])
            if i is None:
                continue
            out.append("    self.f%d = %s" % (i, fexpr(t, ["v"])))
        out += ["    return self", "}", ""]
    for i in range(nf):
        out += ["g%d(self: st): %s {" % (i, ftype[i]), "    return self.f%d" % i, "}", ""]
    out += ["r(x: u8): u8 {", "    return x + 1", "}", "", "main(): u8 {"]
    out.append("    a := st.init(%d)" % random.randint(0, 100))
    names = ["a"]
    for k in range(random.randint(1, 4)):
        j = random.randrange(nm)
        src = random.choice(names)
        name = "s%d" % k
        if random.random() < 0.4:
            out.append("    st %s = %s" % (name, src))
            out.append("    %s = %s.mut%d(%d)" % (name, name, j, random.randint(0, 50)))
        else:
            out.append("    %s := %s.mut%d(%d)" % (name, src, j, random.randint(0, 50)))
        names.append(name)
        gets = ["%s.g%d()" % (random.choice(names), random.randrange(nf)) for _ in range(random.randint(1, 4))]
        out.append('    print("v", %s)' % ", ".join(gets))
    out += ["    return r(%d)" % random.randint(0, 200), "}"]
    return "\n".join(out) + "\n"


def pressure_program():
    # More values live at once than there are registers, many of them across calls: the
    # register allocator's spills, its two classes of registers and its call sequence.
    t = random.choice(TYPES)
    np_ = random.randint(2, 6)
    ps = ["p%d" % j for j in range(np_)]
    out = ['print := import("std").io.print', "",
           "h(%s): %s {" % (", ".join("%s: %s" % (p, t) for p in ps), t),
           "    return %s" % expr(ps, 2, False), "}", "",
           "g(%s): %s {" % (", ".join("%s: %s" % (p, t) for p in ps), t)]
    names = list(ps)
    for k in range(random.randint(8, 24)):
        name = "v%d" % k
        if random.random() < 0.3:
            e = "h(%s)" % ", ".join(random.choice(names) for _ in ps)
        else:
            e = expr(random.sample(names, min(len(names), 4)), 2, random.random() < 0.2)
        out.append("    %s: %s = %s" % (name, t, e))
        names.append(name)
    total = names[0]
    for n in names[1:]:
        total = "(%s %s %s)" % (total, random.choice(["+", "-"]), n)
    out += ["    return %s" % total, "}", "", "main(): u8 {"]
    for _ in range(random.randint(1, 4)):
        out.append('    print("v", g(%s))' % ", ".join(str(random.randint(0, 99)) for _ in ps))
    out += ["    return 0", "}"]
    return "\n".join(out) + "\n"


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    s = p.returncode
    return p.stdout, 1 if s < 0 or s > 128 else s


tested = differ = traps = 0
for i in range(N):
    path = os.path.join(WORK, "f%d.modal" % i)
    with open(path, "w") as f:
        roll = random.random()
        f.write(pressure_program() if KIND == "pressure" or (KIND == "mix" and roll < 0.2)
                else struct_program() if KIND == "struct" or (KIND == "mix" and roll < 0.6)
                else program())
    if subprocess.run([BIN, path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        os.remove(path)
        continue
    arm, x86 = path + ".arm", path + ".x86"
    subprocess.run([BIN, "--backend", "native", "-o", arm, path], stdout=subprocess.DEVNULL)
    subprocess.run([BIN, "--target", "x86_64-macos", "-o", x86, path], stdout=subprocess.DEVNULL)
    results = [run([BIN, "--run", path]), run([BIN, "--backend", "native", "--run", path]),
               run([arm]), run([x86])]
    tested += 1
    traps += b"division by zero" in results[0][0]
    if b"ct false" in results[0][0]:  # the compiler and the program computed different values
        differ += 1
        print("COMPTIME", path)
        print("  ", results[0])
    elif any(r != results[0] for r in results):
        differ += 1
        print("DIFF", path)
        for name, r in zip(["C", "in memory", "exec", "x86-64"], results):
            print("  %-9s" % name, r)
    else:
        for f in (path, arm, x86):
            os.remove(f)
print("tested %d programs (of %d generated, %d of them trap), %d differ" % (tested, N, traps, differ))
if not differ:
    os.rmdir(WORK)
sys.exit(1 if differ else 0)
