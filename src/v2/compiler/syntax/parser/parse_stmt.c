// parse_stmt.c (v2): blocks and statements. A statement ends at a newline, a ';' or the '}'.
// A statement that declares a local (`name: T = v`, `T name`) is parsed by parse_local.
#include "v2/compiler/syntax/parser/parser.h"

static int at_statement_end(const Parser *p)
{
    TokenKind k = p->current->kind;
    return k == TOK_RBRACE || k == TOK_SEMICOLON || k == TOK_EOF || (p->current->nl);
}

NodeId parse_statement(Parser *p)
{
    const Token *t = p->current;

    switch (t->kind)
    {
    case TOK_RETURN:
    {
        parser_advance(p);
        NodeId value = 0;
        if (!at_statement_end(p))
        {
            value = parse_expression(p);
            if (!value)
            {
                return 0;
            }
        }
        return node_new(p->unit, AST_RETURN, tok_idx(p, t), value, 0);
    }
    case TOK_LBRACE:
        return parse_block(p);
    case TOK_IDENTIFIER:
        if (t[1].kind == TOK_DEFINE)
        {
            return parse_define(p);
        }
        return parse_expression(p);
    default:
        return parse_expression(p);
    }
}

NodeId parse_block(Parser *p)
{
    Unit *u = p->unit;
    const Token *open = p->current;

    if (!parser_consume(p, TOK_LBRACE, "expected '{' to start a block") || !parser_enter(p))
    {
        return 0;
    }
    NodeList stmts = list_begin();
    int saved_paren_depth = p->paren_depth; // a block inside ( ) gets newline rules back
    p->paren_depth = 0;

    while (p->current->kind != TOK_RBRACE && p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }

        if (local_decl_at(p->current))
        {
            parse_local(p, &stmts);
        }
        else
        {
            NodeId stmt = parse_statement(p);
            if (!p->panic)
            {
                node_list_push(u, &stmts, stmt);
            }
        }
        if (p->panic)
        {
            parser_synchronize(p);
            continue;
        }

        if (!at_statement_end(p))
        {
            parser_error_at(p, p->current, "expected a newline or ';' between statements");
            parser_synchronize(p);
        }
    }

    p->paren_depth = saved_paren_depth;
    parser_leave(p);
    if (!parser_consume(p, TOK_RBRACE, "expected '}' at the end of the block"))
    {
        list_abort(&stmts);
        return 0;
    }
    NodeId head = list_finish(u, &stmts);
    return node_new(u, AST_BLOCK, tok_idx(p, open), head, 0);
}
