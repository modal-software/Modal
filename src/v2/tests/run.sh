#!/bin/sh
# Front-end checks for v2. Run from the repository root: `make test_v2`.
BIN=./modal-v2
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
msg=$($BIN -o "$OUT" $EX/main.modal 2>&1)
if echo "$msg" | grep -q "backend c: importing another Modal file is not supported yet" && [ ! -e "$OUT" ]; then
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
