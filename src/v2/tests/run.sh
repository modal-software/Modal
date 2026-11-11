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
check "v1 example still parses" $BIN ./examples/add.modal

S="--syms"
expect_output "type-first field is mutable" "field   john_nested_class.my_name .* mut" $EX/main.modal $S
expect_output "type-last field is immutable" "field   john_nested_class.n2 *private *$" $EX/main.modal $S
expect_output "manifest exposes a listed field" "john_nested_class.n1 *public" $EX/main.modal $S
expect_output "unlisted struct stays private" "struct  default_struct *private" $EX/main.modal $S
expect_output "constructor is always public" "method  init *public *constructor" $EX/main.modal $S
expect_output "outside method attaches to its struct" "john_nested_class.multiply" $EX/main.modal $S
expect_output "overloads share one name" "john_nested_class.add_oneline .* mut" $EX/main.modal $S
expect_output "one-line method body" "add_oneline.*\[oneline\]" $EX/main.modal --ast
expect_output "function literal binding" "fn () int" $EX/main.modal --ast
expect_output "import alias" "item new as make_john" $EX/main.modal --ast

expect_fail "private names are rejected across domains" $EX/errors/private.modal
expect_output "private field" "'my_name' is private to domain 'example'" $EX/errors/private.modal
expect_output "private struct through the manifest" "'default_struct' is private" $EX/errors/private.modal
expect_output "missing member" "has no member 'does_not_exist'" $EX/errors/private.modal

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
