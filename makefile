CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -I src/

SRCS = ./tokenizer/tokenizer.c ./ast/parser.c ./ast/ast_alloc.c ./ast/error.c ./ast/parse_decl.c ./ast/  # adicione todos .c
OBJS = $(SRCS:.c=.o)  # mágica: tokenizer.c → tokenizer.o

modal: $(OBJS)
	$(CC) $(OBJS) -o modal

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o modal
