#include "writer.h"
#include "ast/ast.h"
#include <stdio.h>

void write(AstNode *program)
{
    if (program->kind != AST_WRITE_STMT)
    {
        return;
    }

    AstNode *group = program->data.print.group;
    for (size_t i = 0; i < group->data.block_or_group.count; i++)
    {
        AstNode *child = group->data.block_or_group.stmts[i];
        switch (child->kind)
        case AST_STRING_LIT:
        {
            printf("%.*s\n", (int)child->data.string.len, child->data.string.value);
        }
    }
}
