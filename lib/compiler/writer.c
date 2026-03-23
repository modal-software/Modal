#include "writer.h"
#include "ast/ast.h"
#include <stdio.h>

void write(AstNode *program)
{
    printf("%s", program->token.start);
}
