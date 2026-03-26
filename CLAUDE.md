# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Modal is an early-stage compiled programming language written in C (C17). It aims to be an explicit language with switchable memory features (borrow checker, GC, or manual), few reserved keywords, parametrized definitions, and metaprogramming support.

## Build Commands

```bash
make              # Build the `modal` executable
make run          # Build and run examples/add.modal (clears terminal first)
make clean        # Remove object files and the modal binary
./modal <file>    # Run a .modal file directly
```

Compiler: GCC with `-Wall -Wextra -g -std=c17`. No test suite beyond the built-in `test`/`assert` keywords in .modal files.

## Architecture

Classic multi-stage compiler pipeline, all in C:

```
Source (.modal) → Tokenizer → Parser → AST → [Test Runner / Writer]
```

- **Entry point:** `main.c` — reads file, initializes tokenizer+parser, calls `parse_program()`, frees AST
- **Tokenizer** (`lib/compiler/tokenizer/`) — state-machine lexer producing tokens (keywords, operators, literals)
- **Parser** (`lib/compiler/parser/`) — recursive descent, split across:
  - `parser.c` — core parser: advance, match, consume, error recovery (`parser_synchronize`)
  - `parse_expr.c` — expressions with precedence climbing: primary → factor → term → expression
  - `parse_stmt.c` — statements: print, test, assert, blocks `{}`, groups `()`
  - `parse_decl.c` — declaration parsing (stub/deprecated)
- **AST** (`lib/compiler/ast/`) — node types defined in `ast.h`, factory functions in `ast.c`, error reporting in `error.c`
- **Test runner** (`lib/compiler/test_runner.c`) — walks AST for `test "name" { assert ... }` blocks, evaluates assertions
- **Writer** (`lib/compiler/writer.c`) — output stub, not yet implemented
- **Utils** (`lib/utils/`) — arena allocator (incomplete), CLI color constants

No code generation backend exists yet. The compiler currently parses and executes print/test statements directly from the AST.

## Code Style

Configured via `.clang-format`: LLVM base style, 4-space indent, Allman braces, 100-char column limit.
