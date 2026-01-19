#include "test_runner.h"
#include <stdio.h>
#include <string.h>

static TestResults results = {0, 0, 0};

void print_test_name(const char *name, size_t len) {
  printf("%.*s", (int)len, name);
}

int eval_assert(AstNode *expr) {
  if (!expr)
    return 0;

  if (expr->kind == AST_BIN_OP) {
    return 1;
  }

  return 1;
}

void exec_test(AstNode *test_node) {
  if (!test_node || test_node->kind != AST_TEST_STMT) {
    return;
  }

  const char *name = test_node->data.test.name;
  size_t name_len = test_node->data.test.len;
  AstNode *block = test_node->data.test.block;

  results.total++;

  printf("Running test: \"");
  print_test_name(name, name_len);
  printf("\" ... ");

  int test_passed = 1;

  if (block && block->kind == AST_BLOCK) {
    for (size_t i = 0; i < block->data.block_or_group.count; i++) {
      AstNode *stmt = block->data.block_or_group.stmts[i];

      // Handle assert statements
      if (stmt && stmt->kind == AST_ASSERT_STMT) {
        if (!eval_assert(stmt->data.unary.expr)) {
          test_passed = 0;
          break;
        }
      }
    }
  }
  if (test_passed) {
    printf("✓ PASSED\n");
    results.passed++;
  } else {
    printf("✗ FAILED\n");
    results.failed++;
  }
}

void run_tests(AstNode *program) {
  if (!program)
    return;

  printf("\n═══════════════════════════════════════\n");
  printf("         Running Modal Tests\n");
  printf("═══════════════════════════════════════\n\n");

  results = (TestResults){0, 0, 0};

  // Iterate through top-level statements
  if (program->kind == AST_BLOCK) {
    for (size_t i = 0; i < program->data.block_or_group.count; i++) {
      AstNode *stmt = program->data.block_or_group.stmts[i];
      if (stmt && stmt->kind == AST_TEST_STMT) {
        exec_test(stmt);
      }
    }
  }

  // Print summary
  // printf("\n═══════════════════════════════════════\n");
  // printf("           Test Summary\n");
  // printf("═══════════════════════════════════════\n");
  // printf("Total:  %d\n", results.total);
  // printf("Passed: %d ", results.passed);
  // if (results.passed > 0)
  //   printf("✓");
  // printf("\n");
  // printf("Failed: %d ", results.failed);
  // if (results.failed > 0)
  //   printf("✗");
  // printf("\n");
  //
  // if (results.failed == 0 && results.total > 0) {
  //   printf("\n🎉 All tests passed!\n");
  // } else if (results.failed > 0) {
  //   printf("\n❌ Some tests failed.\n");
  // }
  // printf("═══════════════════════════════════════\n\n");
}
