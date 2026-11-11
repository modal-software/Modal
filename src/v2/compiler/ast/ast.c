#include "v2/compiler/ast/ast.h"
#include "v2/compiler/ast/visitor.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8_t ast_child_mask[AST__COUNT] = {
    [AST_MODULE] = 1,     [AST_STRUCT_DECL] = 1, [AST_FIELD_DECL] = 3, [AST_METHOD_DECL] = 7,
    [AST_PARAM] = 1,      [AST_DEFINE] = 1,      [AST_VAL_DECL] = 3,   [AST_TYPE_SLICE] = 1,
    [AST_TYPE_PTR] = 1,   [AST_TYPE_STRUCT] = 1, [AST_FN_LIT] = 7,     [AST_BLOCK] = 1,
    [AST_RETURN] = 1,     [AST_ASSIGN] = 3,      [AST_BIN_OP] = 3,     [AST_UNARY_OP] = 1,
    [AST_CALL] = 3,       [AST_INDEX] = 3,       [AST_SELECTOR] = 1,   [AST_PAREN_GROUP] = 1,
    [AST_IMPORT_ALL] = 1, [AST_IMPORT_PICK] = 3,
};

void node_array_grow(Unit *u)
{
    // In place: a file's nodes are the last array of the node region while it is parsed.
    u->nodes.data = (AstNode *)region_grow_array(REGION_NODES, u->nodes.data, sizeof(AstNode),
                                                 &u->nodes.cap, u->nodes.size + 1);
}

static NodeId ast_new_node(Unit *u, AstNodeKind kind, uint32_t tok, NodeId a, NodeId b, NodeId c)
{
    return node_new(u, kind, tok, a, b, c);
}

static const char *ast_kind_name(AstNodeKind kind)
{
    static const char *const names[AST__COUNT] = {
        [AST_NONE] = "none",
        [AST_MODULE] = "module",
        [AST_STRUCT_DECL] = "struct",
        [AST_FIELD_DECL] = "field",
        [AST_METHOD_DECL] = "method",
        [AST_PARAM] = "param",
        [AST_DEFINE] = "define",
        [AST_VAL_DECL] = "val",
        [AST_TYPE_NAME] = "type",
        [AST_TYPE_SLICE] = "type-slice",
        [AST_TYPE_PTR] = "type-ptr",
        [AST_TYPE_STRUCT] = "type-struct",
        [AST_FN_LIT] = "fn",
        [AST_BLOCK] = "block",
        [AST_RETURN] = "return",
        [AST_ASSIGN] = "assign",
        [AST_BIN_OP] = "binop",
        [AST_UNARY_OP] = "unary",
        [AST_CALL] = "call",
        [AST_INDEX] = "index",
        [AST_SELECTOR] = "select",
        [AST_PAREN_GROUP] = "group",
        [AST_IDENT] = "ident",
        [AST_NUMBER_LIT] = "number",
        [AST_STRING_LIT] = "string",
        [AST_IMPORT] = "import",
        [AST_IMPORT_ALL] = "import-all",
        [AST_IMPORT_PICK] = "import-pick",
        [AST_IMPORT_ITEM] = "item",
    };
    return kind < AST__COUNT && names[kind] ? names[kind] : "?";
}

NodeId ast_import_root(const Unit *u, NodeId expr)
{
    while (expr)
    {
        const AstNode *n = NODE(u, expr);
        switch (n->kind)
        {
        case AST_IMPORT:
            return expr;
        case AST_SELECTOR:
        case AST_IMPORT_ALL:
        case AST_IMPORT_PICK:
            expr = n->a;
            break;
        default:
            return 0;
        }
    }
    return 0;
}

// ---- text rendering ------------------------------------------------------------------------

typedef struct
{
    char *buf;
    size_t cap;
    size_t len;
} Str;

static void str_put(Str *s, const char *text, size_t n)
{
    if (s->len + 1 >= s->cap)
    {
        return;
    }
    size_t room = s->cap - 1 - s->len;
    if (n > room)
    {
        n = room;
    }
    memcpy(s->buf + s->len, text, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void str_puts(Str *s, const char *text)
{
    str_put(s, text, strlen(text));
}

static void str_type(Str *s, const Unit *u, NodeId type)
{
    while (type)
    {
        const AstNode *n = NODE(u, type);
        switch (n->kind)
        {
        case AST_TYPE_SLICE:
            str_puts(s, "[]");
            type = n->a;
            continue;
        case AST_TYPE_PTR:
            str_puts(s, "*");
            type = n->a;
            continue;
        case AST_TYPE_STRUCT:
            str_puts(s, "struct {...}");
            return;
        case AST_TYPE_NAME:
        {
            const Token *first = TOK(u, n->tok);
            const Token *last = TOK(u, n->tok + 2u * n->aux);
            str_put(s, first->start, (size_t)((last->start + last->len) - first->start));
            return;
        }
        default:
            str_puts(s, "?");
            return;
        }
    }
}

static char *ast_type_str(const Unit *u, NodeId type, char *buf, size_t cap)
{
    Str s = {buf, cap, 0};
    buf[0] = '\0';
    str_type(&s, u, type);
    return buf;
}

static void str_params(Str *s, const Unit *u, NodeId param)
{
    str_puts(s, "(");
    for (int first = 1; param; param = NODE(u, param)->next, first = 0)
    {
        const AstNode *n = NODE(u, param);
        const Token *name = TOK(u, n->tok);
        if (!first)
        {
            str_puts(s, ", ");
        }
        str_put(s, name->start, (size_t)name->len);
        if (n->a)
        {
            str_puts(s, ": ");
            str_type(s, u, n->a);
        }
    }
    str_puts(s, ")");
}

// `name(params): T`, or `T name(params)` for the type-first (overloadable) form.
static char *ast_sig_str(const Unit *u, NodeId decl, char *buf, size_t cap)
{
    Str s = {buf, cap, 0};
    const AstNode *n = NODE(u, decl);
    const Token *name = TOK(u, n->tok);
    const AstNode *fn = n->kind == AST_DEFINE ? NODE(u, n->a) : n;
    int type_first = (n->flags & AST_F_MUT) != 0;

    buf[0] = '\0';
    if (type_first)
    {
        str_type(&s, u, fn->b);
        str_puts(&s, " ");
    }
    str_put(&s, name->start, (size_t)name->len);
    str_params(&s, u, fn->a);
    if (!type_first && fn->b)
    {
        str_puts(&s, ": ");
        str_type(&s, u, fn->b);
    }
    return buf;
}

// ---- printer: one visitor over the iterative walker ------------------------------------------

static int print_visit(Unit *u, NodeId id, uint32_t depth, void *ctx)
{
    (void)ctx;
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    char text[256];
    Str s = {text, sizeof(text), 0};
    int skip = VISIT_CONTINUE;
    text[0] = '\0';

    printf("%*s%s", (int)depth * 2, "", ast_kind_name((AstNodeKind)n->kind));

    switch (n->kind)
    {
    case AST_MODULE:
        printf(" %.*s", u->name_len, u->name);
        break;
    case AST_FIELD_DECL:
    case AST_PARAM:
        str_type(&s, u, n->a);
        printf(" %.*s: %s", t->len, t->start, text);
        // An inline struct type keeps its members as children; plain types are fully shown.
        if (!n->a || NODE(u, n->a)->kind != AST_TYPE_STRUCT)
        {
            skip = VISIT_SKIP_A;
        }
        break;
    case AST_METHOD_DECL:
        str_params(&s, u, n->a);
        if (n->b)
        {
            str_puts(&s, ": ");
            str_type(&s, u, n->b);
        }
        printf(" %.*s%s", t->len, t->start, text);
        skip = VISIT_SKIP_A | VISIT_SKIP_B;
        break;
    case AST_FN_LIT:
        str_params(&s, u, n->a);
        if (n->b)
        {
            str_puts(&s, " ");
            str_type(&s, u, n->b);
        }
        printf(" %s", text);
        skip = VISIT_SKIP_A | VISIT_SKIP_B;
        break;
    case AST_TYPE_NAME:
    case AST_TYPE_SLICE:
    case AST_TYPE_PTR:
        str_type(&s, u, id);
        printf(" %s", text);
        skip = VISIT_SKIP;
        break;
    case AST_IMPORT_ITEM:
        printf(" %.*s", t->len, t->start);
        if (n->c)
        {
            const Token *alias = TOK(u, n->c);
            printf(" as %.*s", alias->len, alias->start);
        }
        break;
    case AST_IMPORT:
        printf(" %.*s", t->len, t->start);
        if (n->aux)
        {
            printf("  -> %s", cx.units[n->aux - 1].path);
        }
        break;
    case AST_STRUCT_DECL:
    case AST_DEFINE:
    case AST_IDENT:
    case AST_NUMBER_LIT:
    case AST_STRING_LIT:
    case AST_BIN_OP:
    case AST_UNARY_OP:
        printf(" %.*s", t->len, t->start);
        break;
    case AST_SELECTOR:
        printf(" .%.*s", t->len, t->start);
        break;
    default:
        break;
    }

    if (n->flags & AST_F_MUT)
    {
        printf("  [mut]");
    }
    if (n->flags & AST_F_ONELINE)
    {
        printf("  [oneline]");
    }
    if (n->flags & AST_F_SELF)
    {
        printf("  [self]");
    }
    if (n->flags & AST_F_LAZY)
    {
        printf("  [body not parsed]");
    }
    putchar('\n');
    return skip;
}

static void ast_print(Unit *u)
{
    walk_ast_iterative(u, u->root, print_visit, NULL);
}

const AstImpl ast = {
    .new = {.node = ast_new_node},
    .print = ast_print,
    .kind_name = ast_kind_name,
    .type_str = ast_type_str,
    .sig_str = ast_sig_str,
};
