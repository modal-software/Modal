#!/bin/sh
# Front-end checks for v2. Run from the repository root: `make test_v2`.
BIN=${BIN:-./modal}
EX=./examples
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
for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms bindings; do
    if $BIN --run $EX/run/$prog.modal 2>&1 | cmp -s - $EX/run/$prog.expected; then
        echo "ok    $prog.modal compiles to C and prints the expected output"
    else
        echo "FAIL  $prog.modal compiles to C and prints the expected output"
        fail=1
    fi
done

C=$EX/errors/context.modal
expect_output "a context must be a struct" "context.modal:2:.*a context must be a struct" $C
expect_output "a context cannot be reassigned" "context.modal:8:.*context parameter: it cannot be reassigned" $C
expect_output "a missing context is reported" "context.modal:11:.*'needs' needs a context 'arena'" $C

P=$EX/errors/patterns.modal
expect_output "a clause after a catch-all never runs" "patterns.modal:5:.*this clause would never run" $P
expect_output "two clauses with the same patterns" "patterns.modal:8:.*with the same patterns" $P
expect_output "clauses need a final catch-all" "patterns.modal:2:.*none after them that takes every value" $P
F=$EX/errors/patterns_fit.modal
expect_output "a pattern must fit its type" "patterns_fit.modal:1:.*the pattern 300 does not fit u8" $F
expect_output "a negative pattern on an unsigned type" "patterns_fit.modal:4:.*the pattern -1 does not fit u8" $F
expect_output "a bool pattern on an integer" "patterns_fit.modal:7:.*is a bool, but the parameter is not" $F

N=$EX/errors/bounds.modal
expect_output "a constant must fit its field" "bounds.modal:1:.*300 does not fit u8 (0 to 255, 8 bits)" $N
expect_output "a constant must fit the other operand" "bounds.modal:6:.*200 does not fit i8" $N
expect_output "an argument must fit its parameter" "bounds.modal:9:.*256 does not fit u8" $N
expect_output "a negative argument must fit" "bounds.modal:10:.*-129 does not fit i8 (-128 to 127" $N
expect_output "a returned constant must fit" "bounds.modal:11:.*1000 does not fit u8" $N

O=$EX/errors/output.modal
expect_output "printing with no out where the file has one" "output.modal:16:.*there is none here. Take the context parameter \`out: out\`" $O
expect_output "std.sys.put takes a byte that fits" "output.modal:20:.*300 does not fit u8" $O
expect_output "std.sys.put takes a u8" "output.modal:21:.*std.sys.put takes a u8, not \[\]u8" $O
expect_output "std.sys.put takes one argument" "output.modal:22:.*takes one byte" $O

D=$EX/errors/devices.modal
expect_output "a register block gives every field an offset" "devices.modal:4:.*give 'b' one too" $D
expect_output "a register's offset is aligned" "devices.modal:7:.*offset 2 is not a multiple of 4" $D
expect_output "registers do not overlap" "devices.modal:11:.*overlaps the register before it, which ends at 8" $D
expect_output "a register is a number or a bool" "devices.modal:14:.*is a register: a sized number or bool, not \[\]u8" $D
expect_output "only a register block is placed at an address" "devices.modal:26:.*give each field of plain its offset" $D
expect_output "a device is type-last" "devices.modal:27:.*a device stays at its address" $D
expect_output "a device's address is aligned" "devices.modal:28:.*address 0x2002 is not a multiple of 4" $D
expect_output "a read-only register cannot be assigned" "devices.modal:24:.*'status' is immutable" $D
DC=${TMPDIR:-/tmp}/modal-test-device-$$.c
if $BIN -o "$DC" $EX/run/device.modal >/dev/null 2>&1 && grep -q "volatile uint16_t ibrd" "$DC" &&
    ${CC:-cc} -std=c99 -Wall -Wextra -Werror -c -o "$DC.o" "$DC" 2>/dev/null; then
    echo "ok    a register block is volatile in C, and C agrees with its layout"
else
    echo "FAIL  a register block is volatile in C, and C agrees with its layout"
    fail=1
fi
rm -f "$DC" "$DC.o"

expect_output "an immutable module binding cannot be assigned" "globals.modal:17:.*'limit' is immutable" $EX/errors/globals.modal
L=$EX/errors/locals.modal
expect_output "an immutable local cannot be assigned" "locals.modal:4:.*local 'a' is immutable" $L
expect_output "a local's value is checked against its type" "locals.modal:5:.*300 does not fit u8" $L
K=$EX/errors/checks.modal
expect_output "a body that can end without return" "checks.modal:2:.*'f' returns u8, but its body can reach its end" $K
expect_output "a := binding is immutable" "checks.modal:8:.*'n' is immutable (bound with" $K
expect_output "a := literal is range-checked" "checks.modal:9:.*3000000000 does not fit int" $K
expect_output "bools have no order" "checks.modal:10:.*'<' cannot be applied to bool and bool" $K
L=$EX/errors/locals_syntax.modal
expect_output "an immutable local needs its value" "locals_syntax.modal:3:.*immutable local needs its value" $L
expect_output "a local declares one name" "locals_syntax.modal:4:.*a local declares one name" $L
expect_output "a struct is not declared in a function" "locals_syntax.modal:5:.*declared at module level" $L

expect_output "a closure is reported" "closure.modal:2:.*a closure" $EX/errors/closure.modal -o ${TMPDIR:-/tmp}/modal-test-closure-$$.c

# the native backend (copy-and-patch): same output and exit status as the C backend
if [ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ]; then
    for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms bindings; do
        if $BIN --backend native --run $EX/run/$prog.modal 2>&1 | cmp -s - $EX/run/$prog.expected; then
            echo "ok    $prog.modal compiles to arm64 by copy-and-patch and prints the expected output"
        else
            echo "FAIL  $prog.modal compiles to arm64 by copy-and-patch and prints the expected output"
            fail=1
        fi
    done
    $BIN --backend native --run $EX/run/counter.modal >/dev/null 2>&1
    native=$?
    $BIN --run $EX/run/counter.modal >/dev/null 2>&1
    if [ $native = $? ]; then
        echo "ok    native and C programs exit with the same status"
    else
        echo "FAIL  native and C programs exit with the same status"
        fail=1
    fi
    if $BIN --backend native --run $EX/run/multi/main.modal 2>&1 | cmp -s - $EX/run/multi/main.expected; then
        echo "ok    a program in several files compiles natively"
    else
        echo "FAIL  a program in several files compiles natively"
        fail=1
    fi
    LZ=${TMPDIR:-/tmp}/modal-test-lazy-$$
    expect_output "what cannot be generated is reported only when called" \
        "util.modal:16:.*closure.*in 'f', which the program calls" $EX/errors/lazy/main.modal --backend native -o $LZ
    rm -f "$LZ"
fi
# x86-64: on an x86-64 host, or on Apple Silicon through Rosetta
if [ "$(uname -m)" = x86_64 ] || arch -x86_64 /usr/bin/true 2>/dev/null; then
    for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms bindings; do
        if $BIN --backend x86_64 --run $EX/run/$prog.modal 2>&1 | cmp -s - $EX/run/$prog.expected; then
            echo "ok    $prog.modal compiles to x86-64 by copy-and-patch and prints the expected output"
        else
            echo "FAIL  $prog.modal compiles to x86-64 by copy-and-patch and prints the expected output"
            fail=1
        fi
    done
fi
# Linux: static ELF executables, run on macOS through tools/elfrun.c (only the two system
# calls are replaced), or directly on Linux.
ELF=${TMPDIR:-/tmp}/modal-test-elf-$$
for cpu in arm64 x86_64; do
    if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = "$cpu" -o "$(uname -m)-$cpu" = aarch64-arm64 ]; then
        runner=""
    elif [ "$(uname -s)" = Darwin ] && { [ $cpu = arm64 ] && [ "$(uname -m)" = arm64 ] || { [ $cpu = x86_64 ] && arch -x86_64 /usr/bin/true 2>/dev/null; }; }; then
        ${CC:-cc} -arch $cpu -O2 -std=c17 ./src/v2/tools/elfrun.c -o "$ELF.run" 2>/dev/null || continue
        runner="$ELF.run"
        [ $cpu = x86_64 ] && runner="arch -x86_64 $ELF.run"
    else
        continue
    fi
    for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms bindings; do
        if $BIN --target $cpu-linux -o "$ELF" $EX/run/$prog.modal >/dev/null 2>&1 &&
            $runner "$ELF" 2>&1 | cmp -s - $EX/run/$prog.expected; then
            echo "ok    $prog.modal as a Linux $cpu ELF prints the expected output"
        else
            echo "FAIL  $prog.modal as a Linux $cpu ELF prints the expected output"
            fail=1
        fi
    done
    if $BIN --target $cpu-linux -o "$ELF" $EX/run/device.modal >/dev/null 2>&1 &&
        $runner --device 0x10000000000 48 "$ELF" 2>&1 | cmp -s - $EX/run/device.expected; then
        echo "ok    device.modal on $cpu: registers stored at their offsets and widths"
    else
        echo "FAIL  device.modal on $cpu: registers stored at their offsets and widths"
        fail=1
    fi
    # bare metal: the image at the memory map's flash, the stack at the top of its RAM, print
    # through `out` to a UART; the halt after main ends the run with main's result
    $BIN --target $cpu-none -o "$ELF" $EX/board/blink.modal >/dev/null 2>&1 &&
        $runner --ram 0x10000020000 4096 --device 0x10000000000 8 "$ELF" > "$ELF.out" 2>&1
    bare_status=$?
    if [ $bare_status = 42 ] && cmp -s "$ELF.out" $EX/board/blink.expected; then
        echo "ok    a bare-metal $cpu image runs from its memory map and prints to its UART"
    else
        echo "FAIL  a bare-metal $cpu image runs from its memory map and prints to its UART"
        fail=1
    fi
    rm -f "$ELF.out"
    $BIN --target $cpu-linux -o "$ELF" $EX/run/counter.modal >/dev/null 2>&1 && $runner "$ELF" >/dev/null 2>&1
    elf_status=$?
    $BIN --run $EX/run/counter.modal >/dev/null 2>&1
    if [ $elf_status = $? ]; then
        echo "ok    a Linux $cpu ELF exits with the C program's status"
    else
        echo "FAIL  a Linux $cpu ELF exits with the C program's status"
        fail=1
    fi
    rm -f "$ELF" "$ELF.run"
done
# memory budgets: the value stack sized from the call graph; mod.modal's max_code / max_ram
BG=${TMPDIR:-/tmp}/modal-test-budget-$$
expect_output "a function that needs nothing runs at both times" "twice(n: u8): u8 *both$" $EX/kinds.modal --sig
expect_output "a function that prints is run time, where" "log(n: u8) *run time  (prints, kinds.modal:10)" $EX/kinds.modal --sig
expect_output "the kind follows the calls" "step(n: u8): u8 *run time  (prints, through 'log', kinds.modal:10)" $EX/kinds.modal --sig
expect_output "a mutable module variable is run time" "count(): u8 *run time  (a mutable module variable, kinds.modal:19)" $EX/kinds.modal --sig
expect_output "a cycle of calls has one kind" "u8 down(u8 0) *run time  (prints, through 'down'" $EX/kinds.modal --sig
expect_output "a device register is run time" "send(self: pl011, b: u8) *run time  (a device register, device.modal:19)" $EX/run/device.modal --sig
CT=$EX/errors/comptime.modal
expect_output "compile time cannot print" "comptime.modal:14:.*'a' cannot be computed by the compiler: 'log' prints" $CT
expect_output "compile time sees only the first value of a mutable variable" "comptime.modal:15:.*'count' uses a mutable module variable" $CT
expect_output "compile time has a depth budget" "comptime.modal:16:.*nest deeper than comptime_depth = 1000" $CT
expect_output "a division by zero at compile time is an error" "comptime.modal:17:.*'half' divides by zero" $CT
AS=$EX/errors/asserts.modal
expect_output "a failed assert at module level is a compile error, with both sides" "asserts.modal:12:.*assert(sizeof(header) == 12) is false (8 == 12)" $AS
expect_output "a failed assert shows signed values" "asserts.modal:13:.*assert(neg(5) >= 0) is false (-5 >= 0)" $AS
expect_output "an assert inside a function called at module level fails at compile time" "asserts.modal:14:.*'assert' fails at .*asserts.modal:9:5: assert(neg(3) > 0) is false" $AS
expect_output "assert (std, in Modal) needs a bool" "asserts_types.modal:2:.*argument 1 of 'assert' is int, but parameter 'ok' is bool" $EX/errors/asserts_types.modal
expect_output "sizeof takes one argument" "asserts_types.modal:3:.*sizeof takes one type or value" $EX/errors/asserts_types.modal
expect_output "an unsized constant is range-checked where it meets a type" "unsized.modal:5:.*300 does not fit u8" $EX/errors/unsized.modal
expect_output "an unsized constant must fit some integer type" "unsized.modal:2:.*'big' is too large for any integer type" $EX/errors/unsized.modal
expect_output "a range pattern must take some value" "literals.modal:2:.*this range takes no value" $EX/errors/literals.modal
expect_output "a string pattern needs a []u8 parameter" "literals.modal:4:.*a string pattern needs a \\[\\]u8 parameter" $EX/errors/literals.modal
expect_output "a range's end must fit the parameter" "literals.modal:8:.*the pattern 300 does not fit u8" $EX/errors/literals.modal
expect_output "overloads are chosen by the argument types" "literals.modal:16:.*no definition of 'show' takes .u16.: it takes .u8., .bool." $EX/errors/literals.modal
expect_output "arms: a value no arm takes is named" "arms.modal:2:.*no arm takes 10..255" $EX/errors/arms.modal
expect_output "arms: an arm covered by the ones above never runs" "arms.modal:8:.*this arm never runs" $EX/errors/arms.modal
expect_output "arms: strings end with a name" "arms.modal:11:.*end with an arm that names the value" $EX/errors/arms.modal
expect_output "arms: a catch-all after full coverage never runs" "arms.modal:16:.*this arm never runs: the arms above already take every value" $EX/errors/arms.modal
expect_output "arms: a value match gives one type" "arms.modal:22:.*this arm gives .*u8, but the arms above give a number" $EX/errors/arms.modal
expect_output "arms: signed coverage names the gap" "arms.modal:25:.*no arm takes 0" $EX/errors/arms.modal
expect_output "arms take disjoint values" "bindings.modal:4:.*this arm overlaps an arm above on 5..9" $EX/errors/bindings.modal
expect_output "a binding takes the value's own type" "bindings.modal:9:.*this binding is u64, but the value is u8" $EX/errors/bindings.modal
expect_output "an arm's binding is immutable" "bindings.modal:14:.*bound by the arm's pattern, so it is immutable" $EX/errors/bindings.modal
expect_output "comptime_depth is configuration of the domain" "budget_ct/main.modal:3:.*nest deeper than comptime_depth = 5" $EX/errors/budget_ct/main.modal
expect_output "configuration is a compile-time value" "ctvalues/main.modal:3:.*'max_ram' is configuration of the domain, a compile-time value" $EX/errors/ctvalues/main.modal
expect_output "a target fact is a compile-time value" "ctvalues/main.modal:6:.*'ptr_size' is a fact of the target" $EX/errors/ctvalues/main.modal
for backend in c native x86_64; do
    [ $backend = native ] && ! { [ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ]; } && continue
    [ $backend = x86_64 ] && ! { [ "$(uname -m)" = x86_64 ] || arch -x86_64 /usr/bin/true 2>/dev/null; } && continue
    if $BIN --backend $backend --run $EX/run/target/main.modal 2>&1 | cmp -s - $EX/run/target/main.expected; then
        echo "ok    target and config values at compile time ($backend)"
    else
        echo "FAIL  target and config values at compile time ($backend)"
        fail=1
    fi
done
if [ "$(uname -m)" = arm64 ]; then
    expect_output "target.arch is the machine compiled for (arm64)" "^arm64 " $EX/run/target/arch.modal --backend native --run
fi
if arch -x86_64 /usr/bin/true 2>/dev/null || [ "$(uname -m)" = x86_64 ]; then
    expect_output "target.arch is the machine compiled for (x86-64)" "^x86_64 " $EX/run/target/arch.modal --backend x86_64 --run
fi
expect_output "a mod.modal function is compile time only" "compiled/main.modal:3:.*'square' is declared in mod.modal, so it runs only in the compiler" $EX/errors/compiled/main.modal
expect_output "a mod.modal function is internal to its domain" "outside.modal:2:.*'twice' is a function of inner's mod.modal" $EX/errors/compiled/outside.modal
for backend in c native x86_64; do
    [ $backend = native ] && ! { [ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ]; } && continue
    [ $backend = x86_64 ] && ! { [ "$(uname -m)" = x86_64 ] || arch -x86_64 /usr/bin/true 2>/dev/null; } && continue
    if $BIN --backend $backend --run $EX/run/compiled/main.modal 2>&1 | cmp -s - $EX/run/compiled/main.expected; then
        echo "ok    mod.modal functions run in the compiler ($backend)"
    else
        echo "FAIL  mod.modal functions run in the compiler ($backend)"
        fail=1
    fi
done
expect_output "identical functions are folded into one" "^functions .* 1 folded into an identical one" $EX/run/same.modal --backend native -o "$BG" --stats
expect_output "the value stack is sized exactly from the call graph" "^memory .*value stack 16 B (2 slots, exact)" $EX/run/counter.modal --backend native -o "$BG" --stats
expect_output "a recursive program says which function has no known depth" "'fact' calls itself" $EX/run/patterns.modal --backend native -o "$BG" --stats
expect_output "a program within its budgets compiles" "^ok: .*small.modal" $EX/budget/small.modal --backend native -o "$BG"
expect_output "code over max_code is an error" "deep.modal:21:.*code is [0-9]* bytes, over max_code = 1200" $EX/budget/deep.modal --backend native -o "$BG"
expect_output "memory over max_ram is an error, with what it is made of" "deep.modal:21:.*over max_ram = 512 (mod.modal): value stack 40, output buffer 64, 3 nested calls" $EX/budget/deep.modal --backend native -o "$BG"
rm -f "$BG"
BM=${TMPDIR:-/tmp}/modal-test-bare-$$
expect_output "bare metal needs a memory map" "bare.modal:2:.*runs without a system: its mod.modal gives the memory map" $EX/errors/bare.modal --target arm64-none -o "$BM"
expect_output "bare metal has no system output: print needs an out" "hello.modal:5:.*arm64-none has no system output" $EX/run/hello.modal --target arm64-none -o "$BM"
expect_output "the memory map is the budget" "^memory .*ram 1376 B" $EX/board/blink.modal --target arm64-none -o "$BM" --stats
rm -f "$BM"
expect_output "an unknown target is rejected with the known ones" "unknown target 'mips'. Known targets: arm64-macos" $EX/run/hello.modal --target mips
OUT=${TMPDIR:-/tmp}/modal-test-$$.c
if $BIN -o "$OUT" $EX/run/counter.modal >/dev/null 2>&1 && ${CC:-cc} -std=c99 -Wall -Wextra -Werror -o "$OUT.bin" "$OUT" 2>/dev/null; then
    echo "ok    generated C builds with -Wall -Wextra -Werror"
else
    echo "FAIL  generated C builds with -Wall -Wextra -Werror"
    fail=1
fi
rm -f "$OUT" "$OUT.bin"
msg=$($BIN -o "$OUT" $EX/main.modal 2>&1)
if echo "$msg" | grep -q "backend c: a type in an imported file is not supported yet" && [ ! -e "$OUT" ]; then
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

# --cache: a cold run, a warm run and no cache at all must say exactly the same thing
MC=${TMPDIR:-/tmp}/modal-test-cache-$$
for f in main.modal calls.modal errors/calls.modal; do
    plain=$($BIN $EX/$f 2>&1)
    cold=$(MODAL_CACHE=$MC $BIN --cache $EX/$f 2>&1)
    warm=$(MODAL_CACHE=$MC $BIN --cache $EX/$f 2>&1)
    if [ "$plain" = "$cold" ] && [ "$plain" = "$warm" ]; then
        echo "ok    --cache gives the same result for $f"
    else
        echo "FAIL  --cache gives the same result for $f"
        fail=1
    fi
done
rm -rf "$MC"
expect_output "bodies are not kept in memory" "nodes kept [78][0-9] " $EX/run/counter.modal --stats
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
