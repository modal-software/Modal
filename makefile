CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -I ./

SRCS = ./tokenizer/tokenizer.c ./ast/parser.c ./ast/ast.c ./ast/error.c ./ast/parse_decl.c ./ast/parse_expr.c ./ast/parse_stmt.c ./lib/compiler/test_runner.c ./main.c  # adicione todos .c
OBJS = $(SRCS:.c=.o)  # mágica: tokenizer.c → tokenizer.o

modal: $(OBJS)
	$(CC) $(OBJS) -o modal

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o modal
