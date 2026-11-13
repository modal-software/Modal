// parse_stmt.c (v2): blocks and statements. A statement ends at a newline, a ';' or the '}'.
// A statement that declares a local (`name: T = v`, `T name`) is parsed by parse_local.
#include "v2/compiler/syntax/parser/parser.h"

static int at_statement_end(const Parser *p)
{
    TokenKind k = p->current->kind;
    return k == TOK_RBRACE || k == TOK_SEMICOLON || k == TOK_EOF || (p->current->nl);
}

static NodeId parse_if(Parser *p);

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
    case TOK_IF:
        return parse_if(p);
    case TOK_ELSE:
        parser_error_at(p, t, "'else' without an 'if' before it");
        return 0;
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

// ---- arms (docs/std.md §8) ------------------------------------------------------------------------
//
// An arm is `pattern: result`. `:` takes a value, as everywhere (only `=` assigns). A pattern is a
// literal (a number, `-` and a number, a string, true, false), a range of numbers (`0..9`, `10..`)
// or `_`, every value. Several values are matched with parentheses, one pattern each:
// `(0, _): acc`; a lone `_` takes every value of all of them. An arm binds no name: inside it the
// value keeps its own name. A match is `value: { arms }`; a function body whose first line is an
// arm is arms on the parameters.

static int is_bool_token(const Token *t)
{
    return t->kind == TOK_IDENTIFIER && ((t->len == 4 && memcmp(TSTART(t), "true", 4) == 0) ||
                                         (t->len == 5 && memcmp(TSTART(t), "false", 5) == 0));
}

static int is_any_token(const Token *t)
{
    return t->kind == TOK_IDENTIFIER && t->len == 1 && TSTART(t)[0] == '_';
}

// One pattern's tokens from t: how many (0: not a pattern).
static int pattern_len(const Token *t)
{
    if (is_any_token(t) || is_bool_token(t) || t->kind == TOK_STRING)
    {
        return 1;
    }
    int k = 0;
    if (t[k].kind == TOK_MINUS && t[k + 1].kind == TOK_NUMBER)
    {
        k++;
    }
    if (t[k].kind != TOK_NUMBER)
    {
        return 0;
    }
    k++;
    if (t[k].kind == TOK_DOTDOT)
    {
        k++;
        if (t[k].kind == TOK_MINUS && t[k + 1].kind == TOK_NUMBER)
        {
            k++;
        }
        if (t[k].kind == TOK_NUMBER)
        {
            k++;
        }
    }
    return k;
}

int arm_at(const Token *t)
{
    if (t->kind == TOK_LPAREN) // (p, p, ...):
    {
        const Token *k = t + 1;
        for (;;)
        {
            int n = pattern_len(k);
            if (!n)
            {
                return 0;
            }
            k += n;
            if (k->kind == TOK_COMMA)
            {
                k++;
                continue;
            }
            return k->kind == TOK_RPAREN && k[1].kind == TOK_COLON;
        }
    }
    int n = pattern_len(t);
    return n && t[n].kind == TOK_COLON;
}

static NodeId parse_pattern(Parser *p)
{
    Unit *u = p->unit;
    const Token *t = p->current;
    if (is_any_token(t))
    {
        parser_advance(p);
        NodeId n = node_new(u, AST_PATTERN, tok_idx(p, t), 0, 0);
        NODE(u, n)->aux = PAT_ANY;
        return n;
    }
    int negative = t->kind == TOK_MINUS && t[1].kind == TOK_NUMBER;
    const Token *lit = negative ? t + 1 : t;
    if (lit->kind != TOK_NUMBER && lit->kind != TOK_STRING && !is_bool_token(lit))
    {
        parser_error_at(p, t, t->kind == TOK_IDENTIFIER
                                  ? "a pattern is a literal, a range or _, not a name: inside the arm, use "
                                    "the value's own name"
                                  : "expected a pattern: a number, a range (`0..9`), a string, true, false or _");
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
    int grouped = parser_match(p, TOK_LPAREN);
    for (;;)
    {
        NodeId pat = parse_pattern(p);
        if (!pat)
        {
            list_abort(&pats);
            return 0;
        }
        node_list_push(u, &pats, pat);
        if (!grouped || !parser_match(p, TOK_COMMA))
        {
            break;
        }
    }
    if (grouped && !parser_consume(p, TOK_RPAREN, "expected ')' after the arm's patterns"))
    {
        list_abort(&pats);
        return 0;
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
    NodeId arm = node_new(u, AST_ARM, tok_idx(p, first), head, result);
    NODE(u, arm)->aux = (uint16_t)grouped;
    return arm;
}

// `{ arms }`, matching `subject` (0: the parameters of the function whose body it is).
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

// if cond branch [else branch]: a branch is a block or one statement. `else` may start the next
// line; `else if ...` chains.
static NodeId parse_branch(Parser *p)
{
    return p->current->kind == TOK_LBRACE ? parse_block(p) : parse_statement(p);
}

static NodeId parse_if(Parser *p)
{
    Unit *u = p->unit;
    const Token *t = p->current;
    parser_advance(p);
    NodeId cond = parse_expression(p);
    if (!cond)
    {
        return 0;
    }
    NodeId then = parse_branch(p);
    if (!then)
    {
        return 0;
    }
    NodeId other = 0;
    if (p->current->kind == TOK_ELSE)
    {
        parser_advance(p);
        other = parse_branch(p);
        if (!other)
        {
            return 0;
        }
    }
    NodeId branches = node_new(u, AST_ARM, tok_idx(p, t), then, other);
    return node_new(u, AST_IF, tok_idx(p, t), cond, branches);
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
