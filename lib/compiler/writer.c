#include "writer.h"
#include "ast/ast.h"
// #include <stdio.h>

void write(AstNode *program)
{
    if (program->kind != AST_WRITE_STMT)
    {
        return;
    }

    // AstNode *group = program->data.write.group;
    // AstNode *child;
    // AST_EACH(group, child)
    // {
    //     switch (child->kind)
    //     {
    //     case AST_STRING_LIT:
    //         fprintf(stdout, "%.*s\n", (int)child->data.string.len, child->data.string.value);
    //         break;
    //     default:
    //         break;
    //     }
    // }
}
