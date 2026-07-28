#include "compiler/ast/ast.h"
#include "compiler/syntax/parser/parser.h"
#include "compiler/syntax/tokenizer.h"
#include "compiler/syntax/tokenizer.impl.h"
#include <stdio.h>
#include <stdlib.h>

static char *file_entry(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buffer = malloc(size + 1);
    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    return buffer;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "%s: fatal: no input files\n[Process exited %d]\n", argv[0], EXIT_FAILURE);
        return 1;
    }

    char *buffer = file_entry(argv[1]);

    Tokenizer lexer;
    tokenizer.init(&lexer, buffer);

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
