// test_runner.h
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include "../../ast/ast.h"

typedef struct
{
    int total;
    int passed;
    int failed;
} TestResults;

void run_tests(AstNode *program);

#endif
