// visitor.h (v2): iterative pre-order AST walk with an explicit stack.
//
// The stack is sized from the node count of the unit: every pending frame is one distinct
// parent->child edge, and there are fewer than 2 * nodes of those (a grouped declaration such
// as `a, b: T` shares its type node). So the walk cannot overflow however deep the tree is,
// and it never recurses on the C stack.
#ifndef MODAL_V2_VISITOR_H
#define MODAL_V2_VISITOR_H

#include "v2/compiler/cx.h"

enum
{
    VISIT_ABORT = -1,
    VISIT_CONTINUE = 0,
    VISIT_SKIP_A = 1, // do not descend into child a / b / c of this node
    VISIT_SKIP_B = 2,
    VISIT_SKIP_C = 4,
    VISIT_SKIP = 7,
};

typedef int (*VisitorFn)(Unit *u, NodeId id, uint32_t depth, void *ctx);

typedef struct
{
    NodeId id;
    uint32_t depth;
} WalkFrame;

// Visits `root` and everything under it (not its siblings). Returns 0 if a visitor aborted.
static inline int walk_ast_iterative(Unit *u, NodeId root, VisitorFn visit, void *ctx)
{
    if (!root)
    {
        return 1;
    }

    ArenaMark mark = arena_mark();
    WalkFrame *stack = (WalkFrame *)arena_alloc((2 * u->nodes.size + 1) * sizeof(WalkFrame));
    ArenaMark top = arena_mark();
    size_t sp = 0;
    int ok = 1;

    stack[sp++] = (WalkFrame){root, 0};

    while (sp > 0)
    {
        WalkFrame f = stack[--sp];
        const AstNode *n = NODE(u, f.id);
        uint8_t mask = ast_child_mask[n->kind];
        if (n->flags & AST_F_LAZY)
        {
            mask &= (uint8_t)~4u; // `c` is a token index while the body is unparsed
        }
        NodeId a = n->a, b = n->b, c = n->c, next = n->next;

        int r = visit(u, f.id, f.depth, ctx);
        if (r < 0)
        {
            ok = 0;
            break;
        }
        mask &= (uint8_t)~r;

        // Pushed in reverse so they pop as: a (and a's siblings), b, c, then our own sibling.
        if (next && f.id != root)
        {
            stack[sp++] = (WalkFrame){next, f.depth};
        }
        if ((mask & 4) && c)
        {
            stack[sp++] = (WalkFrame){c, f.depth + 1};
        }
        if ((mask & 2) && b)
        {
            stack[sp++] = (WalkFrame){b, f.depth + 1};
        }
        if ((mask & 1) && a)
        {
            stack[sp++] = (WalkFrame){a, f.depth + 1};
        }
    }

    // Give the stack back, unless the visitor allocated on top of it.
    if (arena_mark_equal(arena_mark(), top))
    {
        arena_release(mark);
    }
    return ok;
}

#endif // MODAL_V2_VISITOR_H
