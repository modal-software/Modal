// test_runner.h
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include "ast/ast.h"

typedef struct
{
    int total;
    int passed;
    int failed;
} TestResults;

// Execute a single AST_TEST_STMT node.
void exec_test(AstNode *test_node);

// Print the accumulated test summary. Call once after all tests.
void print_test_summary(void);

#endif
