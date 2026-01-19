#include "ast/parser.h"
#include "lib/compiler/test_runner.h"
#include "tokenizer/tokenizer.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Uso: %s arquivo.modal\n", argv[0]);
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
        fprintf(stderr, "erros falhou com erros.\n");
    }
    else
    {
        printf("AST root kind: %d\n", root ? root->kind : 0);
        run_tests(root);
    }

    ast_free(root);
    free(buffer);
    return parser.had_error ? 1 : 0;
}
