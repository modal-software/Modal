#include "lib/compiler/ast/ast.h"
#include "lib/compiler/parser/parser.h"
#include "lib/compiler/tokenizer/tokenizer.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "%s: fatal: no input files\n", argv[0]);
        printf("[Process exited %d]\n", EXIT_FAILURE);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buffer = malloc(size + 1);
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    Tokenizer lexer;
    init(&lexer, buffer);

    Parser parser;
    parser_init(&parser, &lexer, argv[1]);

    AstNode *root = parse_program(&parser);

    if (parser.had_error)
    {
        ast_free(root);
        free(buffer);
        return 1;
    }

    exec_program(root);

    ast_free(root);
    free(buffer);
    return 0;
}
