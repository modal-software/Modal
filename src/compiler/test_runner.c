#include "test_runner.h"
#include "parser/parser.h"
#include <stdio.h>

static TestResults results = {0, 0, 0};

void exec_test(AstNode *test_node)
{
    if (!test_node || test_node->kind != AST_TEST_STMT)
    {
        return;
    }

    const char *name = test_node->data.test.name;
    size_t name_len = test_node->data.test.len;
    AstNode *block = test_node->data.test.block;

    results.total++;

    printf("Running test: \"");
    printf("%.*s", (int)name_len, name);
    printf("\" ... ");

    int test_passed = 1;

    if (block && block->kind == AST_BLOCK)
    {
        AstNode *stmt;
        AST_EACH(block, stmt)
        {
            if (stmt->kind == AST_ASSERT_STMT)
            {
                if (eval_expr(stmt->data.unary.expr) == 0)
                {
                    test_passed = 0;
                    break;
                }
            }
        }
    }

    if (test_passed)
    {
        printf("✓ PASSED\n\n");
        results.passed++;
    }
    else
    {
        printf("✗ FAILED\n\n");
        results.failed++;
    }
}

void print_test_summary(void)
{
    if (results.total == 0)
    {
        return;
    }

    if (results.passed > 0)
    {
        printf("%d/%d", results.passed, results.total);
    }
    if (results.failed > 0)
    {
        printf("Failed: %d", results.failed);
    }

    if (results.failed == 0)
    {
        printf("\n✓ All tests passed!\n");
    }
    else
    {
        printf("\n❌ Some tests failed.\n");
    }

    results = (TestResults){0, 0, 0};
}
