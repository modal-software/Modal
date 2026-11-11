CC = gcc
CFLAGS = -Wall -O3 -Wextra -g -std=c17 -MMD -MP -I ./ -I src/v1/compiler -I src
TARGET = jma
LIBS = -lm -lpthread -ldl

SRCS = ./src/v1/compiler/cx.c ./src/v1/compiler/syntax/tokenizer.impl.c ./src/v1/compiler/syntax/tokenizer.c ./src/v1/compiler/syntax/parser/parser.c ./src/v1/compiler/ast/ast.c ./src/v1/compiler/ast/error.c ./src/v1/compiler/syntax/parser/parse_decl.c ./src/v1/compiler/syntax/parser/parse_expr.c ./src/v1/compiler/syntax/parser/parse_stmt.c ./src/v1/compiler/test_runner.c ./main.c  # adicione todos .c

SRCS_V2 = ./src/v2/main.c ./src/v2/compiler/cx.c ./src/v2/compiler/syntax/scanner.c ./src/v2/compiler/syntax/parser/parser.c ./src/v2/compiler/syntax/parser/parse_decl.c ./src/v2/compiler/syntax/parser/parse_expr.c ./src/v2/compiler/syntax/parser/parse_stmt.c ./src/v2/compiler/ast/ast.c ./src/v2/compiler/ast/symbol.c ./src/v2/compiler/sema/resolve.c ./src/v2/compiler/sema/module.c ./src/v2/compiler/sema/check.c ./src/v2/compiler/sema/types.c ./src/v2/compiler/sema/cache.c ./src/v2/compiler/backend/registry.c ./src/v2/compiler/backend/c/emit.c
HDRS_V2 = $(wildcard ./src/v2/compiler/*.h ./src/v2/compiler/*/*.h ./src/v2/compiler/*/*/*.h)
# -flto: the sources are optimized as one program (about 7% faster: hot helpers inline
# across files, as in TCC's one-source build).
CFLAGS_V2 = -Wall -Wextra -O2 -flto -g -std=c17 -I src
OBJS = $(SRCS:.c=.o)

OUT_DIR := ./out
	
# Paths
PREFIX ?= /usr/local
BINDIR = ${PREFIX}/bin

modal: $(OBJS)
	$(CC) $(OBJS) -o ./modal


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# v2 front-end: scan -> parse -> declare -> link (see docs/v2-frontend.md)
v2: modal-v2

modal-v2: $(SRCS_V2) $(HDRS_V2)
	$(CC) $(CFLAGS_V2) $(SRCS_V2) -o ./modal-v2

run_v2: modal-v2
	./modal-v2 --run ./examples/run/counter.modal

test_v2: modal-v2
	sh ./src/v2/tests/run.sh

run: modal
	clear; ./modal ./examples/add.modal

test_parser3: src/v2/tests/ll1_parsing_3.c
	$(CC) -Wall -Wextra -O2 -std=c17 -pthread src/v2/tests/ll1_parsing_3.c -o src/v2/tests/test_parser3

# no -mavx2 here: P4.3 uses runtime dispatch; only the target("avx2")-
# tagged section may emit AVX2, so the binary stays safe on any x86-64.
test_parser4: src/v2/tests/ll1_parsing_4.c
	$(CC) -Wall -Wextra -O2 -std=c17 -pthread src/v2/tests/ll1_parsing_4.c -o src/v2/tests/test_parser4

test_parser5: src/v2/tests/ll1_parsing_5.c
	$(CC) -Wall -Wextra -O2 -std=c17 -pthread src/v2/tests/ll1_parsing_5.c -o src/v2/tests/test_parser5

bench5: test_parser5 test_parser4
	@echo "== phase4 (v4) =="
	./src/v2/tests/test_parser4 dyn
	@echo "== phase5 (line-index + unroll) =="
	./src/v2/tests/test_parser5 dyn

bench4: test_parser4 test_parser3
	@echo "== phase3 baseline =="
	./src/v2/tests/test_parser3 dyn
	@echo "== phase4 (SWAR/SIMD) =="
	./src/v2/tests/test_parser4 dyn

bench3: test_parser3
	@echo "== static baseline =="
	./src/v2/tests/test_parser3 static
	@echo "== dynamic queue =="
	./src/v2/tests/test_parser3 dyn
	@echo "== dynamic oversubscribed =="
	./src/v2/tests/test_parser3 dyn 32

verbose: modal
	./modal ./examples/add.modal

bundle:
	mkdir -p $(OUT_DIR)	

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) modal modal-v2

-include $(OBJS:.o=.d)
