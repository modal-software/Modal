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

// ---- arms ------------------------------------------------------------------------------------
//
// An arm is `pattern, pattern: result`. A pattern is a literal (a number, `-` and a number, a
// string, true / false), a range of numbers (`0..9`, `10..`) or a name, which takes any value and
// binds it. A block whose first line is an arm is all arms. A literal followed by `:` was never a
// statement, so this is new syntax that no existing program had.

static int is_bool_token(const Token *t)
{
    return t->kind == TOK_IDENTIFIER && ((t->len == 4 && memcmp(TSTART(t), "true", 4) == 0) ||
                                         (t->len == 5 && memcmp(TSTART(t), "false", 5) == 0));
}

int arm_at(const Token *t)
{
    const Token *k = t->kind == TOK_MINUS && t[1].kind == TOK_NUMBER ? t + 1 : t;
    if (k->kind != TOK_NUMBER && k->kind != TOK_STRING && !is_bool_token(k))
    {
        return 0;
    }
    return k[1].kind == TOK_COLON || k[1].kind == TOK_COMMA ||
           (k->kind == TOK_NUMBER && k[1].kind == TOK_DOTDOT);
}

static NodeId parse_pattern(Parser *p)
{
    Unit *u = p->unit;
    const Token *t = p->current;
    if (t->kind == TOK_IDENTIFIER && !is_bool_token(t))
    {
        parser_advance(p);
        return node_new(u, AST_IDENT, tok_idx(p, t), 0, 0);
    }
    int negative = t->kind == TOK_MINUS && t[1].kind == TOK_NUMBER;
    const Token *lit = negative ? t + 1 : t;
    if (lit->kind != TOK_NUMBER && lit->kind != TOK_STRING && !is_bool_token(lit))
    {
        parser_error_at(p, t, "expected a pattern: a number, a range (`0..9`), a string, true, false or a name");
        return 0;
    }
    if (negative)
    {
        parser_advance(p);
    }
    parser_advance(p);
    uint16_t aux = (uint16_t)(negative ? PAT_NEG : 0);
    if (lit->kind == TOK_STRING)
    {
        aux = PAT_TEXT;
    }
    else if (lit->kind == TOK_NUMBER && p->current->kind == TOK_DOTDOT)
    {
        parser_advance(p);
        aux |= PAT_RANGE;
        int neg_hi = p->current->kind == TOK_MINUS && p->current[1].kind == TOK_NUMBER;
        if (p->current->kind == TOK_NUMBER || neg_hi)
        {
            if (neg_hi)
            {
                parser_advance(p);
                aux |= PAT_NEG_HI;
            }
            parser_advance(p);
        }
        else
        {
            aux |= PAT_OPEN;
        }
    }
    NodeId n = node_new(u, AST_PATTERN, tok_idx(p, lit), 0, 0);
    NODE(u, n)->aux = aux;
    return n;
}

static NodeId parse_arm(Parser *p)
{
    Unit *u = p->unit;
    const Token *first = p->current;
    NodeList pats = list_begin();
    for (;;)
    {
        NodeId pat = parse_pattern(p);
        if (!pat)
        {
            list_abort(&pats);
            return 0;
        }
        node_list_push(u, &pats, pat);
        if (!parser_match(p, TOK_COMMA))
        {
            break;
        }
    }
    if (!parser_consume(p, TOK_COLON, "expected ':' after the arm's pattern, then its result"))
    {
        list_abort(&pats);
        return 0;
    }
    NodeId head = list_finish(u, &pats);
    NodeId result = p->current->kind == TOK_LBRACE ? parse_block(p) : parse_statement(p);
    if (!result)
    {
        return 0;
    }
    return node_new(u, AST_ARM, tok_idx(p, first), head, result);
}

NodeId parse_arm_block(Parser *p, NodeId subject)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    if (!parser_consume(p, TOK_LBRACE, "expected '{' to start the arms") || !parser_enter(p))
    {
        return 0;
    }
    int saved_paren_depth = p->paren_depth;
    p->paren_depth = 0;
    NodeList arms = list_begin();
    while (p->current->kind != TOK_RBRACE && p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }
        NodeId arm = parse_arm(p);
        if (!arm || p->panic)
        {
            parser_synchronize(p);
            continue;
        }
        node_list_push(u, &arms, arm);
        if (!at_statement_end(p))
        {
            parser_error_at(p, p->current, "expected a newline or ';' between arms");
            parser_synchronize(p);
        }
    }
    p->paren_depth = saved_paren_depth;
    parser_leave(p);
    if (!parser_consume(p, TOK_RBRACE, "expected '}' after the arms"))
    {
        list_abort(&arms);
        return 0;
    }
    NodeId head = list_finish(u, &arms);
    if (!head)
    {
        parser_error_at(p, open, "a match needs at least one arm");
        return 0;
    }
    return node_new(u, AST_MATCH, tok_idx(p, open), subject, head);
}

NodeId parse_block(Parser *p)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    if (open->kind == TOK_LBRACE && arm_at(open + 1)) // a block of arms: they match the parameters
    {
        NodeId match = parse_arm_block(p, 0);
        if (!match)
        {
            return 0;
        }
        NodeList one = list_begin();
        node_list_push(u, &one, match);
        return node_new(u, AST_BLOCK, tok_idx(p, open), list_finish(u, &one), 0);
    }

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
