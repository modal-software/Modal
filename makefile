CC = gcc
CFLAGS = -Wall -Wextra -g -std=c11 -I ./
TARGET = jma
LIBS = -lm -lpthread -ldl

SRCS = ./lib/compiler/tokenizer/tokenizer.c ./lib/compiler/parser/parser.c ./lib/compiler/ast/ast.c ./lib/compiler/ast/error.c ./lib/compiler/parser/parse_decl.c ./lib/compiler/parser/parse_expr.c ./lib/compiler/parser/parse_stmt.c ./lib/compiler/test_runner.c ./lib/compiler/writer.c ./main.c  # adicione todos .c
OBJS = $(SRCS:.c=.o)
	
# Paths
PREFIX ?= /usr/local
BINDIR = ${PREFIX}/bin

modal: $(OBJS)
	$(CC) $(OBJS) -o modal

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: modal
	clear; ./modal ./examples/add.modal

clean:
	rm -f *.o modal
