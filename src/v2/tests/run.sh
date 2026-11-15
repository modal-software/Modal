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

expect_no_output() { # description, pattern, file, flags...: the pattern must not appear
    desc=$1
    pattern=$2
    file=$3
    shift 3
    if $BIN "$@" "$file" 2>&1 | grep -q -- "$pattern"; then
        echo "FAIL  $desc (found: $pattern)"
        fail=1
    else
        echo "ok    $desc"
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
expect_output "a field no other domain uses stays in the domain" "field   john_nested_class.n2 *domain" $EX/main.modal $S
expect_output "a method no other domain uses stays in the domain" "method  john_nested_class.check *domain" $EX/main.modal $S
expect_output "a type another domain uses is public" "struct  john_nested_class *public" $EX/main.modal $S
expect_output "accessor may share the field name" "method  john_nested_class.n1 " $EX/main.modal $S
expect_output "free binding stays in the domain" "method  print_file_name *domain" $EX/main.modal $S
expect_output "config binding" "const   implicit_self *config" $EX/main.modal $S
expect_output "overloads share one name" "john_nested_class.add_oneline .* mut" $EX/main.modal $S
expect_output "one-line method body" "add_oneline.*\[oneline\]" $EX/main.modal --ast
expect_output "function literal binding" "fn () int" $EX/main.modal --ast
expect_output "import alias" "item new as make_john" $EX/main.modal --ast

G="--sig"
expect_output "contract lists opaque types" "^  type john_nested_class" $EX/main.modal $G
expect_output "contract lists config" "^config version = \"0.1.0\"" $EX/main.modal $G
if $BIN --sig $EX/main.modal 2>&1 | grep -q "check\|my_name\|print_file_name"; then
    echo "FAIL  contract leaks private names"
    fail=1
else
    echo "ok    contract holds no private names"
fi

expect_fail "invisible names are rejected" $EX/errors/private.modal
if $BIN $EX/errors/private.modal 2>&1 | grep -q "private.modal:[2-5]:"; then
    echo "FAIL  fields, helpers, bindings and unbound modules of another domain are reachable"
    fail=1
else
    echo "ok    fields, helpers, bindings and unbound modules of another domain are reachable"
fi
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
if $BIN $L 2>&1 | grep -q "leak.modal:\(6\|7\|11\|15\|16\):"; then
    echo "FAIL  bindings, aliases and imports in bodies reach any member"
    fail=1
else
    echo "ok    bindings, aliases and imports in bodies reach any member"
fi
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
expect_output "return type mismatch" "types.modal:27:.*.5. should be \[\]u8, as the function declares, but it is int" $T
expect_output "operator on mismatched types" "types.modal:33:.*cannot be applied to u8 and \[\]u8" $T
expect_output "argument type mismatch" "types.modal:34:.*.q. should be u16, but it is u8 (argument 1 of 'wide', parameter 'n')" $T
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
for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms arm_edges arm_search arm_consts clauses_nil arrays fields reflect field_use nested module_structs any types generic fields_each bound pipes errors error_stages panics error_sets arena_errors closures functions struct_arrays generic_loop print_errors struct_errors captures; do
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
expect_output "a bool pattern on an integer" "patterns_fit.modal:7:.*is a bool, but the value matched is not" $F

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

if [ "$($BIN --run $EX/errors/closure.modal >/dev/null 2>&1; echo $?)" = 3 ]; then
    echo "ok    a literal reading a local of its function runs (f(2) = 3)"
else
    echo "FAIL  a literal reading a local of its function runs (f(2) = 3)"
    fail=1
fi

# the native backend (copy-and-patch): same output and exit status as the C backend
if [ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ]; then
    for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms arm_edges arm_search arm_consts clauses_nil arrays fields reflect field_use nested module_structs any types generic fields_each bound pipes errors error_stages panics error_sets arena_errors closures functions struct_arrays generic_loop print_errors struct_errors captures; do
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
        "util.modal:18:.*this struct value is not supported yet (in 'lambda_user', which the program calls)" $EX/errors/lazy/main.modal --backend native -o $LZ
    rm -f "$LZ"
fi
# x86-64: on an x86-64 host, or on Apple Silicon through Rosetta
if [ "$(uname -m)" = x86_64 ] || arch -x86_64 /usr/bin/true 2>/dev/null; then
    for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms arm_edges arm_search arm_consts clauses_nil arrays fields reflect field_use nested module_structs any types generic fields_each bound pipes errors error_stages panics error_sets arena_errors closures functions struct_arrays generic_loop print_errors struct_errors captures; do
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
# A binary written with -o says `trap N` where a guard's place would be; <binary>.traps gives
# the place back, so its output compares with the same .expected as --run's.
untrap() {
    [ -f "$1" ] || { cat; return; }
    awk -F'\t' 'NR == FNR { if ($1 ~ /^trap [0-9]+$/) m[$1] = $2; next }
        match($0, /trap [0-9]+/) && (substr($0, RSTART, RLENGTH) in m) {
            $0 = substr($0, 1, RSTART - 1) m[substr($0, RSTART, RLENGTH)] substr($0, RSTART + RLENGTH) }
        { print }' "$1" -
}
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
    for prog in hello counter context patterns globals literals structs output locals divide same comptime guards primitives loops quotients matching arms arm_edges arm_search arm_consts clauses_nil arrays fields reflect field_use nested module_structs any types generic fields_each bound pipes errors error_stages panics error_sets arena_errors closures functions struct_arrays generic_loop print_errors struct_errors captures; do
        if $BIN --target $cpu-linux -o "$ELF" $EX/run/$prog.modal >/dev/null 2>&1 &&
            $runner "$ELF" 2>&1 | untrap "$ELF.traps" | cmp -s - $EX/run/$prog.expected; then
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
CV_OUT=$(mktemp)
if [ "$($BIN $EX/run/compile_print.modal -o "$CV_OUT" 2>/dev/null | head -1)" = 11 ] &&
    ! grep -q "loop\|m_desc_\|assert\|g_a" "$CV_OUT"; then
    echo "ok    c: a function only the compiler ran prints in the build and is not in the program"
else
    echo "FAIL  c: a function only the compiler ran prints in the build and is not in the program"
    fail=1
fi
if [ "$($BIN --backend arm64 --run $EX/run/compile_print.modal 2>/dev/null)" = 11 ]; then
    echo "ok    native: the compiler's print comes before the program, which prints nothing"
else
    echo "FAIL  native: the compiler's print comes before the program, which prints nothing"
    fail=1
fi
rm -f "$CV_OUT"
expect_output "sig: a generic is listed once, its specializations under it" "loop(T: type, n: T) *generic: made once per type" $EX/run/compile_print.modal --sig
expect_output "sig: a specialization with its own types" "^    loop(type, u64) *run time" $EX/run/compile_print.modal --sig
expect_output "sig: a function that ran for a module-level value says so" "ran while compiling, for a value the program uses" $EX/run/compile_print.modal --sig
expect_output "a function that needs nothing runs at both times" "twice(n: u8): u8 *both$" $EX/kinds.modal --sig
expect_output "a function that prints is run time, where" "log(n: u8) *run time  (prints, kinds.modal:10)" $EX/kinds.modal --sig
expect_output "the kind follows the calls" "step(n: u8): u8 *run time  (prints, through 'log', kinds.modal:10)" $EX/kinds.modal --sig
expect_output "a mutable module variable is run time" "count(): u8 *run time  (a mutable module variable, kinds.modal:19)" $EX/kinds.modal --sig
expect_output "a cycle of calls has one kind" "u8 down(u8 0) *run time  (prints, through 'down'" $EX/kinds.modal --sig
expect_output "a device register is run time" "send(self: pl011, b: u8) *run time  (a device register, device.modal:19)" $EX/run/device.modal --sig
CT=$EX/errors/comptime.modal
expect_output "compile time prints in the build (a module-level value)" "^n 3$" $CT
expect_output "compile time sees only the first value of a mutable variable" "comptime.modal:15:.*'count' uses a mutable module variable" $CT
expect_output "compile time has a depth budget" "comptime.modal:16:.*nest deeper than comptime_depth = 1000" $CT
expect_output "a division by zero at compile time is an error" "comptime.modal:17:.*'half' divides by zero" $CT
AS=$EX/errors/asserts.modal
expect_output "a failed assert at module level is a compile error, with both sides" "asserts.modal:12:.*assert(sizeof(header) == 12) is false (8 == 12)" $AS
expect_output "a failed assert shows signed values" "asserts.modal:13:.*assert(neg(5) >= 0) is false (-5 >= 0)" $AS
expect_output "an assert inside a function called at module level fails at compile time" "asserts.modal:14:.*'checks' fails at .*asserts.modal:9:5: assert(neg(3) > 0) is false" $AS
expect_output "assert (std, in Modal) needs a bool" "asserts_types.modal:2:.*argument 1 of 'assert' should be bool, but it is int (parameter 'ok')" $EX/errors/asserts_types.modal
expect_output "sizeof takes one argument" "asserts_types.modal:3:.*sizeof takes one type or value" $EX/errors/asserts_types.modal
expect_output "an unsized constant is range-checked where it meets a type" "unsized.modal:5:.*300 does not fit u8" $EX/errors/unsized.modal
expect_output "an unsized constant must fit some integer type" "unsized.modal:2:.*'big' is too large for any integer type" $EX/errors/unsized.modal
expect_output "a range pattern must take some value" "literals.modal:2:.*this range takes no value" $EX/errors/literals.modal
expect_output "a string pattern needs a []u8 parameter" "literals.modal:4:.*a string pattern needs a \\[\\]u8 value to match" $EX/errors/literals.modal
expect_output "a range's end must fit the parameter" "literals.modal:8:.*the pattern 300 does not fit u8" $EX/errors/literals.modal
expect_output "overloads are chosen by the argument types" "literals.modal:16:.*no definition of 'show' takes .u16.: it takes .u8., .bool." $EX/errors/literals.modal
expect_output "arms: a value no arm takes is named" "arms.modal:2:.*no arm takes 10..255" $EX/errors/arms.modal
expect_output "arms: an arm covered by the ones above never runs" "arms.modal:8:.*this arm never runs" $EX/errors/arms.modal
expect_output "arms: strings end with _" "arms.modal:11:.*end with .*_: ...., which takes the rest" $EX/errors/arms.modal
expect_output "arms: a catch-all after full coverage never runs" "arms.modal:16:.*this arm never runs: the arms above already take every value" $EX/errors/arms.modal
expect_output "arms take disjoint values" "arms.modal:20:.*this arm overlaps an arm above on 5..9" $EX/errors/arms.modal
expect_output "arms: several values take parentheses" "arms.modal:24:.*write one pattern for each, in parentheses" $EX/errors/arms.modal
expect_output "arms: a value match gives one type" "arms.modal:31:.*this arm gives .*u8, but the arms above give a number" $EX/errors/arms.modal
expect_output "arms: signed coverage names the gap" "arms.modal:34:.*no arm takes 0" $EX/errors/arms.modal
expect_output "if: the condition is a bool" "arms.modal:38:.*an if's condition is a bool, not i8" $EX/errors/arms.modal
expect_output "arms: a pattern naming the value matched takes every value" "arm_names.modal:3:.*'n' is the value being matched, so this pattern takes every value" $EX/errors/arm_names.modal
expect_output "arms: the old 'value: {' form names the new one" "arm_forms.modal:3:.*a match is written .if value { arms }." $EX/errors/arm_forms.modal
expect_output "arms take no else" "arm_forms.modal:7:.*arms take no 'else'" $EX/errors/arm_forms.modal
expect_output "an if as a value takes arms" "arm_forms.modal:41:.*an 'if' used as a value takes arms" $EX/errors/arm_forms.modal
expect_output "arms: several statements under an arm take braces" "arm_forms.modal:19:.*an arm with several statements takes braces" $EX/errors/arm_forms.modal
expect_output "arms: a constant name counts like its value" "arm_forms.modal:28:.*this arm never runs" $EX/errors/arm_forms.modal
expect_output "arms: a constant name must fit the value matched" "arm_forms.modal:32:.*the pattern 'big' does not fit u8" $EX/errors/arm_forms.modal
expect_output "arms: a name pattern must exist" "arm_forms.modal:33:.*'nope' is not declared" $EX/errors/arm_forms.modal
expect_output "arms: a match used as a value takes every value" "arm_forms.modal:37:.*not every value is taken" $EX/errors/arm_forms.modal
expect_output "arms: several values take one pattern each" "arm_forms.modal:45:.*this match has 2 values" $EX/errors/arm_forms.modal
expect_output "arms: a range's ends are known while compiling" "arm_forms.modal:50:.*'moving' is not known while compiling" $EX/errors/arm_forms.modal
SL=$EX/errors/struct_literals.modal
expect_output "literals: a literal gives every field" "struct_literals.modal:10:.*'y' is not given" $SL
expect_output "literals: a field is given once" "struct_literals.modal:11:.*'x' is given twice" $SL
expect_output "literals: a type-last field is not written" "struct_literals.modal:12:.*'y' is immutable" $SL
if $BIN $SL 2>&1 | grep -q "struct_literals.modal:13:"; then
    echo "FAIL  literals: a local may hold a reference to a literal (p: *T = &T{ ... })"
    fail=1
else
    echo "ok    literals: a local may hold a reference to a literal (p: *T = &T{ ... })"
fi
RE=$EX/errors/references.modal
expect_output "a literal's reference is not returned" "references.modal:22:.*points to a literal of this function, which ends when it returns" $RE
expect_output "a reference to a literal is a local's value or a constructor's result" "references.modal:25:.*a reference to a literal is a local's value" $RE
expect_output "literals: a literal's values fit their fields" "struct_literals.modal:14:.*'x' is i32, but this value is bool" $SL
expect_output "literals: a literal names fields of its struct" "struct_literals.modal:15:.*'point' has no field 'z'" $SL
expect_output "a method is called, not read" "struct_literals.modal:16:.*'norm' is a method of 'point': call it" $SL
expect_output "visibility: a face line is not needed" "face_line.modal:5:.*needs no face line" $EX/errors/face_line.modal
DM=$EX/run/domains/main.modal
for b in c native; do
    if $BIN --backend $b --run $DM 2>&1 | cmp -s - $EX/run/domains/main.expected; then
        echo "ok    a struct of another domain: literal, fields and methods ($b)"
    else
        echo "FAIL  a struct of another domain: literal, fields and methods ($b)"
        fail=1
    fi
done
expect_output "visibility: what another domain uses is public" "^    lo: i32" $DM --sig
expect_output "visibility: a writable public field says so" "^    x: i32  (writable)" $DM --sig
expect_output "visibility: a used method is public" "method  span.width *public" $DM --syms
expect_output "visibility: a member only its domain uses stays there" "field   span.reads *domain" $DM --syms
RF=$EX/errors/reflect.modal
expect_output "reflect: offsetof names a field" "reflect.modal:6:.*offsetof takes a field of a struct" $RF
expect_output "reflect: offsetof of a missing field" "reflect.modal:7:.*offsetof takes a field of a struct" $RF
expect_output "reflect: typeof gives std's type" "reflect.modal:8:.*typeof gives std's type: bind it first" $RF
expect_output "reflect: an unsized constant has no type" "any.modal:18:.*typeof: an unsized constant has no type yet" $EX/errors/any.modal
expect_output "reflect: a query takes one argument" "reflect.modal:9:.*sizeof takes one type or value" $RF
RB=${TMPDIR:-/tmp}/modal-reflect-$$
if $BIN --backend native -o "$RB" $EX/run/reflect.modal >/dev/null 2>&1 && ! strings "$RB" | grep -q "header\|point\|kind\|flags"; then
    echo "ok    reflect: no type or field name reaches the binary"
else
    echo "FAIL  reflect: no type or field name reaches the binary"
    fail=1
fi
if $BIN --backend native -o "$RB" $EX/run/guards.modal >/dev/null 2>&1 && ! strings "$RB" | grep -q "guards.modal\|assert(" &&
    "$RB" 2>&1 | grep -q "^trap [0-9]* is false" && grep -q "guards.modal:27:5: assert(n < 4)" "$RB.traps"; then
    echo "ok    a binary written with -o carries no source: trap N, and the place in its .traps"
else
    echo "FAIL  a binary written with -o carries no source: trap N, and the place in its .traps"
    fail=1
fi
rm -f "$RB" "$RB.traps"
RE=$EX/errors/references.modal
expect_output "a value is not passed as a reference" "references.modal:14:.*.a. should be \*item, but it is item: pass a reference to it, .&a." $RE
expect_output "a reference is not returned as a value" "references.modal:10:.*.it. should be item, as the function declares, but it is \*item: it is a reference, and .\*it. is the value" $RE
AN=$EX/errors/any.modal
expect_output "any: a made *any is not returned" "any.modal:7:.*an \*any made here is a pair in this function's frame" $AN
expect_output "any: a local *any is not returned" "any.modal:12:.*is an \*any made in this function" $AN
expect_output "any: T(a) takes an *any" "any.modal:16:.*takes an \*any and gives its \*point back" $AN
expect_output "any: a value is given by reference" "any.modal:17:.*cannot initialize \*any with point: pass a reference to it" $AN
TP=$EX/errors/type_params.modal
expect_output "types: a value is not a type" "type_params.modal:11:.*.p. should be type, but it is point" $TP
GE=$EX/errors/generic.modal
expect_output "generic: an argument of the bound type" "generic.modal:15:.*.c. should be point, but it is rgb" $GE
expect_output "generic: the type argument names a type" "generic.modal:16:.*takes this argument as a type" $GE
expect_output "generic: a constant fits the bound type" "generic.modal:17:.*300 does not fit u8" $GE
expect_output "order: a function written below its caller is checked first" "order.modal:4:.*'f' is always nil: 'add' can neither fail nor panic" $EX/errors/order.modal --warn
expect_output "arena: an error gives back what the call allocated" "arena_errors.modal:48:.*'build': on an error the arena goes back" $EX/run/arena_errors.modal --audit
expect_output "arena: kept when a module variable could hold what it allocated" "arena_errors.modal:54:.*'count': the arena keeps what it allocated on an error: it, or a function it calls, uses a mutable module variable" $EX/run/arena_errors.modal --audit
CL=$EX/errors/closures.modal
expect_output "closures: a literal reads a local, never assigns it" "closures.modal:5:.*'count' is a local of the function around this literal" $CL
expect_output "closures: a capture is one slot" "closures.modal:7:.*a literal reads 'p' of the function around it, a \\[4\\]u8" $CL
expect_output "closures: a pattern parameter's type is its first call's" "closures.modal:10:.*argument 1 of 'small' is u64, but it takes int" $CL
FN=$EX/errors/functions.modal
expect_output "functions: the one given has the signature's types" "functions.modal:8:.*the function given to 'apply' takes u8, but its signature gives it u64" $FN
expect_output "functions: a function is given, not a value" "functions.modal:10:.*'apply' takes a function here" $FN
NE_OUT=${TMPDIR:-/tmp}/modal-test-noerrnames-$$
if $BIN --backend native -o "$NE_OUT" $EX/run/error_sets.modal >/dev/null 2>&1 && ! grep -q "bad_input\|too_big\|panic\." "$NE_OUT"; then
    echo "ok    errors: a program that prints no error carries no error names"
else
    echo "FAIL  errors: a program that prints no error carries no error names"
    fail=1
fi
rm -f "$NE_OUT" "$NE_OUT.traps"
expect_output "errors: a module-level value's call that gives an error stops the compiler" "module_errors.modal:10:.*'b' cannot be computed by the compiler: gets an error from a call" $EX/errors/module_errors.modal
SE=$EX/errors/error_sets.modal
expect_output "error sets: a match on a call's error takes its whole set" "error_sets.modal:18:.*'serve' can also give too_big, panic.division" $SE
expect_output "error sets: an error that is always nil is said" "error_sets.modal:26:.*'f' is always nil: 'add' can neither fail nor panic" $SE --warn
expect_output "error sets: --sig lists what each function can give" "errors: bad_input, too_big, panic.division" $EX/run/error_sets.modal --sig
ES=$EX/errors/error_stages.modal
expect_output "error stages: an error no arm takes goes up" "error_stages.modal:5:.*'parse' can fail, and its error is not handled here" $ES
expect_output "error stages: a type pattern is the value's type" "error_stages.modal:6:.*'u8' is not the value's type, u64" $ES
expect_output "error stages: a value no arm takes must fit the arms" "error_stages.modal:7:.*a value no arm takes goes on, but it is u64 and the arms give \[\]u8" $ES
ER=$EX/errors/errors.modal
expect_output "errors: an error not handled needs a function that can fail" "errors.modal:12:.*'parse' can fail, and its error is not handled here" $ER
expect_output "errors: the second name is the error of a call" "errors.modal:13:.*'b' would be the error of a call" $ER
expect_output "errors: a call that gives only an error binds one name" "errors.modal:14:.*this call gives only an error: bind it alone" $ER
expect_output "errors: a call alone sends its error up" "errors.modal:15:.*'check' can fail, and its error is not handled here" $ER
PI=$EX/errors/pipes.modal
expect_output "pipes: what no arm takes goes on, so it must fit the arms" "pipes.modal:5:.*the values no arm takes go on as the stage's value, but they are u8 and the arms give \[\]u8" $PI
expect_output "pipes: a stage continues one arm" "pipes_open.modal:4:.*would continue several arms" $EX/errors/pipes_open.modal
BC=$EX/errors/bound.modal
expect_output "bound: a bound call takes no arguments" "bound.modal:7:.*holds a call with its arguments already given" $BC
expect_output "bound: a bound call is not its result" "bound.modal:8:.*cannot be applied to function inc and int" $BC
expect_output "bound: no array is bound by its address" "bound.modal:10:.*an array is passed by its address" $BC
FI=$EX/errors/field_index.modal
expect_output "field: the number is one of the fields" "field_index.modal:12:.*cannot compute for this call: .*assert(u64(i) < self.fields) is false" $FI
expect_output "field: a number its caller knows only when it runs" "field_index.modal:14:.*'at' takes this argument while compiling" $FI
expect_output "field: of the struct its type says" "field_index.modal:15:.*argument 2 of 'field' should be \*u8, but it is \*rec" $FI
if $BIN $FI 2>&1 | grep -q "type.modal:3[0-9]"; then
    echo "FAIL  field: a specialization whose types cannot be computed is not checked further"
    fail=1
else
    echo "ok    field: a specialization whose types cannot be computed is not checked further"
fi
FU=$EX/run/field_use.modal
expect_output "fields: one nothing reads has no bytes" "field_use.modal:8  item.tag  never read" $FU --audit
expect_output "fields: one always the same constant is that constant" "field_use.modal:10  item.bias  always -4" $FU --audit
if $BIN --audit $FU 2>&1 | grep -q "made\.\|item.hits"; then
    echo "FAIL  fields: a field read and written with values, or of a struct sizeof looks at, keeps its bytes"
    fail=1
else
    echo "ok    fields: a field read and written with values, or of a struct sizeof looks at, keeps its bytes"
fi
expect_output "audit: dependencies between domains" "domains -> geo: point point.x point.y span span.lo span.hi span.new span.width" $DM --audit
expect_output "audit: an index the compiler cannot prove is listed" "arrays.modal:12:33: index out of range (0 to 3)  (in sum)" $EX/run/arrays.modal --audit
expect_output "audit: recursion that grows the stack" "^  .*arrays.modal:20  fill" $EX/run/arrays.modal --audit
expect_output "audit: a divisor the compiler cannot prove" "divide.modal:5:14: division by zero  (in d)" $EX/run/divide.modal --audit
expect_output "audit: functions run only while compiling" "guards.modal:12  fact" $EX/run/guards.modal --audit
expect_output "audit: an assert left for run time" "guards.modal:27:5: assert(n < 4) is false" $EX/run/guards.modal --audit
expect_output "audit: the writes to the system" "main  9 writes" $EX/run/guards.modal --audit
if $BIN --sig $DM 2>&1 | grep -q "reads"; then
    echo "FAIL  visibility: the contract holds only what is used"
    fail=1
else
    echo "ok    visibility: the contract holds only what is used"
fi
expect_output "an index out of range stops the program with its place (C)" "arrays.modal:51:12: index out of range (0 to 3)" $EX/run/arrays.modal --run
expect_output "an index out of range stops the program with its place (native)" "arrays.modal:51:12: index out of range (0 to 3)" $EX/run/arrays.modal --backend native --run
expect_output "clauses take disjoint values" "clauses_disjoint.modal:2:.*this clause takes values the clause at line 1 takes too" $EX/errors/clauses_disjoint.modal
expect_output "a nil clause set may leave values (a warning)" "clauses_disjoint.modal:5:.*no clause takes the other values" $EX/errors/clauses_disjoint.modal --warn
expect_no_output "an error before the bodies gives no stray '{' reports" "expected '\{' to start a block" $EX/errors/clauses_disjoint.modal
expect_output "arms: a range with names at its ends takes values" "arm_forms.modal:51:.*this range takes no value" $EX/errors/arm_forms.modal
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
    expect_output "target.arch is the machine compiled for (arm64)" "^arm64 macos arm$" $EX/run/target/arch.modal --backend native --run
fi
if arch -x86_64 /usr/bin/true 2>/dev/null || [ "$(uname -m)" = x86_64 ]; then
    expect_output "target.arch is the machine compiled for (x86-64)" "^x86_64 macos intel$" $EX/run/target/arch.modal --backend x86_64 --run
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
expect_output "memory over max_ram is an error, with what it is made of" "deep.modal:21:.*over max_ram = 512 (mod.modal): value stack 24, output buffer 64, 2 nested calls" $EX/budget/deep.modal --backend native -o "$BG"
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
if echo "$msg" | grep -q "backend c: .* is not supported yet" && [ ! -e "$OUT" ]; then
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
