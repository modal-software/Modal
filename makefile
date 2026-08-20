CC = gcc
CFLAGS = -Wall -O3 -Wextra -g -std=c17 -I ./ -I src/compiler -I src
TARGET = jma
LIBS = -lm -lpthread -ldl

SRCS = ./src/compiler/cx.c ./src/compiler/syntax/tokenizer.impl.c ./src/compiler/syntax/tokenizer.c ./src/compiler/syntax/parser/parser.c ./src/compiler/ast/ast.c ./src/compiler/ast/error.c ./src/compiler/syntax/parser/parse_decl.c ./src/compiler/syntax/parser/parse_expr.c ./src/compiler/syntax/parser/parse_stmt.c ./src/compiler/test_runner.c ./main.c  # adicione todos .c
OBJS = $(SRCS:.c=.o)

OUT_DIR := ./out
	
# Paths
PREFIX ?= /usr/local
BINDIR = ${PREFIX}/bin

modal: $(OBJS)
	$(CC) $(OBJS) -o $(OUT_DIR)/modal

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: modal
	clear; $(OUT_DIR)/modal ./examples/add.modal

verbose: modal
	./modal ./examples/add.modal

bundle:
	mkdir -p $(OUT_DIR)	

clean:
	rm -f *.o modal
