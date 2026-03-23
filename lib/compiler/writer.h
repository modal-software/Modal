// writer.h
#ifndef WRITER_H
#define WRITER_H

#include "ast/ast.h"

typedef struct
{
    const char *content;
} Writer;

void write(AstNode *program);

#endif
