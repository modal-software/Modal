CC = gcc
CFLAGS = -Wall -O3 -Wextra -g -std=c17 -MMD -MP -I ./ -I src/v1/compiler -I src
TARGET = jma
LIBS = -lm -lpthread -ldl

SRCS = ./src/v1/compiler/cx.c ./src/v1/compiler/syntax/tokenizer.impl.c ./src/v1/compiler/syntax/tokenizer.c ./src/v1/compiler/syntax/parser/parser.c ./src/v1/compiler/ast/ast.c ./src/v1/compiler/ast/error.c ./src/v1/compiler/syntax/parser/parse_decl.c ./src/v1/compiler/syntax/parser/parse_expr.c ./src/v1/compiler/syntax/parser/parse_stmt.c ./src/v1/compiler/test_runner.c ./main.c  # adicione todos .c

SRCS_V2 = ./src/v2/compiler/syntax/scanner.c
OBJS = $(SRCS:.c=.o)

OUT_DIR := ./out
	
# Paths
PREFIX ?= /usr/local
BINDIR = ${PREFIX}/bin

modal: $(OBJS)
	$(CC) $(OBJS) -o ./modal


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

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
	rm -f $(OBJS) $(OBJS:.o=.d) modal

-include $(OBJS:.o=.d)
