#include "writer.h"
#include "ast/ast.h"
#include <stdio.h>

static Writer writer = {0};

void write(AstNode *program)
{
    printf("%s", program->token.start);
}
