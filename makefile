CC = gcc
CFLAGS = -Wall -Wextra -g -std=c17 -I ./
TARGET = jma
LIBS = -lm -lpthread -ldl

SRCS = ./lib/compiler/tokenizer/tokenizer.c ./lib/compiler/ast/parser.c ./lib/compiler/ast/ast.c ./lib/compiler/ast/error.c ./lib/compiler/ast/parse_decl.c ./lib/compiler/ast/parse_expr.c ./lib/compiler/ast/parse_stmt.c ./lib/compiler/test_runner.c ./main.c  # adicione todos .c
OBJS = $(SRCS:.c=.o)  # mágica: tokenizer.c → tokenizer.o
	
# Paths
PREFIX ?= /usr/local
BINDIR = ${PREFIX}/bin

modal: $(OBJS)
	$(CC) $(OBJS) -o modal

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o modal
