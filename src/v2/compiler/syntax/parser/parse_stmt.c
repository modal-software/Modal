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
    case TOK_IF:
        return parse_if(p, 0);
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
// or `_`, every value, or a name that exists (a constant, a local, a parameter): the arm takes what
// equals it. Several values are matched with parentheses, one pattern each: `(0, _): acc`; a lone
// `_` takes every value of all of them. An arm binds no name: inside it the value keeps its own name. A match is `if value { arms }`; a function body whose first line is an
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

// A name as a pattern: an existing value the arm compares with (never a new variable).
static int is_name_token(const Token *t)
{
    return t->kind == TOK_IDENTIFIER && !is_any_token(t) && !is_bool_token(t);
}

static int is_builtin_type(const Token *t)
{
    static const char *const names[] = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "int", "bool", "nil"};
    for (size_t i = 0; t->kind == TOK_IDENTIFIER && i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strlen(names[i]) == t->len && memcmp(names[i], TSTART(t), t->len) == 0)
        {
            return 1;
        }
    }
    return 0;
}

// `name: T = v`, `name: u8`, `name: []T ...`, `name: *T ...`, `name: struct`: a local declared, not
// an arm. (An arm whose result is an assignment after a name pattern reads the same: give it
// braces, `limit: { x = 1 }`.)
static int declaration_shape(const Token *t)
{
    const Token *k = t + 2;
    return (k->kind == TOK_IDENTIFIER && k[1].kind == TOK_EQ) || is_builtin_type(k) ||
           (k->kind == TOK_LBRACKET && (k[1].kind == TOK_RBRACKET || ((k[1].kind == TOK_NUMBER || k[1].kind == TOK_IDENTIFIER) && k[2].kind == TOK_RBRACKET))) ||
           k->kind == TOK_STAR || k->kind == TOK_STRUCT;
}

// One pattern's tokens from t: how many (0: not a pattern).
static int pattern_len(const Token *t)
{
    if (t->kind == TOK_MINUS && is_name_token(t + 1) && t[2].kind == TOK_DOTDOT) // `-limit..`
    {
        return 1 + pattern_len(t + 1);
    }
    if (is_name_token(t) && t[1].kind == TOK_DOTDOT) // `lo..`, `lo..hi`, `lo..9`
    {
        const Token *h = t + 2;
        return 2 + (is_name_token(h) || h->kind == TOK_NUMBER ? 1
                    : h->kind == TOK_MINUS && (h[1].kind == TOK_NUMBER || is_name_token(h + 1)) ? 2 : 0);
    }
    if (is_any_token(t) || is_bool_token(t) || t->kind == TOK_STRING || is_name_token(t))
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
        if (t[k].kind == TOK_MINUS && (t[k + 1].kind == TOK_NUMBER || is_name_token(&t[k + 1])))
        {
            k++;
        }
        if (t[k].kind == TOK_NUMBER || is_name_token(&t[k]))
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
    return n && t[n].kind == TOK_COLON && !(is_name_token(t) && declaration_shape(t));
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
    // A range with a constant's name at an end (`lo..hi`, `0..limit`, `limit..`): its ends are
    // kept as expressions, valued by the checker.
    {
        const Token *k = t;
        int named = is_name_token(k) || (k->kind == TOK_MINUS && is_name_token(k + 1));
        int lo_len = is_name_token(k) ? 1 : (k->kind == TOK_MINUS && (k[1].kind == TOK_NUMBER || named)) ? 2
                                          : k->kind == TOK_NUMBER;
        if (lo_len && k[lo_len].kind == TOK_DOTDOT)
        {
            const Token *h = k + lo_len + 1;
            int hi_named = is_name_token(h) || (h->kind == TOK_MINUS && is_name_token(h + 1));
            if (named || hi_named)
            {
                NodeId ends[2] = {0, 0};
                for (int e = 0; e < 2; e++)
                {
                    const Token *at = p->current;
                    if (e == 1)
                    {
                        parser_advance(p); // `..`
                        at = p->current;
                        if (!is_name_token(at) && at->kind != TOK_NUMBER &&
                            !(at->kind == TOK_MINUS && (at[1].kind == TOK_NUMBER || is_name_token(at + 1))))
                        {
                            break; // open: `limit..`
                        }
                    }
                    if (is_name_token(at))
                    {
                        parser_advance(p);
                        ends[e] = node_new(u, AST_IDENT, tok_idx(p, at), 0, 0);
                    }
                    else if (at->kind == TOK_MINUS)
                    {
                        parser_advance(p);
                        parser_advance(p);
                        NodeId inner = node_new(u, is_name_token(at + 1) ? AST_IDENT : AST_NUMBER_LIT, tok_idx(p, at + 1), 0, 0);
                        ends[e] = node_new(u, AST_UNARY_OP, tok_idx(p, at), inner, 0);
                    }
                    else
                    {
                        parser_advance(p);
                        ends[e] = node_new(u, AST_NUMBER_LIT, tok_idx(p, at), 0, 0);
                    }
                }
                NodeId n = node_new(u, AST_PATTERN, tok_idx(p, t), ends[0], ends[1]);
                NODE(u, n)->aux = (uint16_t)(PAT_RANGE | PAT_ENDS | (ends[1] ? 0 : PAT_OPEN));
                return n;
            }
        }
    }
    if (is_name_token(t)) // an existing value: the arm takes what equals it
    {
        parser_advance(p);
        NodeId ident = node_new(u, AST_IDENT, tok_idx(p, t), 0, 0);
        NodeId n = node_new(u, AST_PATTERN, tok_idx(p, t), ident, 0);
        NODE(u, n)->aux = PAT_NAME;
        return n;
    }
    int negative = t->kind == TOK_MINUS && t[1].kind == TOK_NUMBER;
    const Token *lit = negative ? t + 1 : t;
    if (lit->kind != TOK_NUMBER && lit->kind != TOK_STRING && !is_bool_token(lit))
    {
        parser_error_at(p, t, "expected a pattern: a number, a range (`0..9`), a string, true, false, a name "
                              "or _");
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
    int saved_paren_depth = p->paren_depth, saved_if_then = p->if_then;
    p->paren_depth = 0;
    p->if_then = 0;
    NodeList arms = list_begin();
    int count = 0;
    while (p->current->kind != TOK_RBRACE && p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }
        const Token *t = p->current;
        if (count && !arm_at(t) && t->kind != TOK_LPAREN && !(t->kind == TOK_IDENTIFIER && t[1].kind == TOK_COLON) &&
            (t->kind == TOK_IDENTIFIER || t->kind == TOK_RETURN || t->kind == TOK_IF || t->kind == TOK_LBRACE))
        {
            // A second statement under an arm: one statement each, or braces.
            parser_error_at(p, t, "this line is not an arm: an arm with several statements takes braces, "
                                  "`50: { a()  b() }`");
            parser_synchronize(p);
            continue;
        }
        count++;
        NodeId arm = parse_arm(p);
        if (!arm || p->panic)
        {
            parser_synchronize(p);
            continue;
        }
        node_list_push(u, &arms, arm);
        if (!parser_match(p, TOK_COMMA) && !at_statement_end(p))
        {
            parser_error_at(p, p->current, "expected a ',' or a newline between arms");
            parser_synchronize(p);
        }
    }
    p->paren_depth = saved_paren_depth;
    p->if_then = saved_if_then;
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
//
// if value { arms }: a match (std.md §8). The block holds arms when its first line is one (a pattern
// then `:`), which no statement can start with. As a value (`word := if c { ... }`) only this form
// is accepted: an `if` with a condition gives no value yet.
static NodeId parse_branch(Parser *p)
{
    return p->current->kind == TOK_LBRACE ? parse_block(p) : parse_statement(p);
}

NodeId parse_if(Parser *p, int value)
{
    Unit *u = p->unit;
    const Token *t = p->current;
    parser_advance(p);
    const Token *saved_open = p->cond_open;
    p->cond_open = p->current->kind == TOK_LPAREN ? p->current : NULL;
    int saved_lit = p->no_literal;
    p->no_literal = 1;
    NodeId cond = parse_expression(p);
    p->cond_open = saved_open;
    p->no_literal = saved_lit;
    if (!cond)
    {
        return 0;
    }
    if (p->current->kind == TOK_COMMA) // `if a, b { (0, _): ... }`: several values, one column each
    {
        NodeList values = list_begin();
        node_list_push(u, &values, cond);
        while (parser_match(p, TOK_COMMA))
        {
            p->no_literal = 1;
            NodeId v = parse_expression(p);
            p->no_literal = saved_lit;
            if (!v)
            {
                list_abort(&values);
                return 0;
            }
            node_list_push(u, &values, v);
        }
        if (!(p->current->kind == TOK_LBRACE && arm_at(p->current + 1)))
        {
            list_abort(&values);
            parser_error_at(p, p->current, "several values are matched by arms: `if a, b { (0, _): ...,  _: ... }`");
            return 0;
        }
        NodeId match = parse_arm_block(p, list_finish(u, &values));
        if (match && !value)
        {
            NODE(u, match)->aux |= MATCH_STATEMENT;
        }
        return match;
    }
    if (p->current->kind == TOK_LBRACE && arm_at(p->current + 1))
    {
        NodeId subject = NODE(u, cond)->kind == AST_PAREN_GROUP ? NODE(u, cond)->a : cond; // `if (c) {`
        NodeId match = parse_arm_block(p, subject);
        if (match && p->current->kind == TOK_ELSE && !p->if_then)
        {
            parser_error_at(p, p->current, "arms take no 'else': end them with `_: ...`, which takes the rest");
            return 0;
        }
        if (match && !value) // a statement: it may leave values untaken (std.md §8)
        {
            NODE(u, match)->aux |= MATCH_STATEMENT;
        }
        return match; // (an `else` here belongs to the `if` this one is the branch of)
    }
    if (p->current->kind == TOK_BODY)
    {
        // At module level the scanner keeps every `{` as a function body, not yet tokenized.
        parser_error_at(p, p->current,
                        "a match at module level is not supported yet: write it in a function "
                        "(`R pick(T n) { 0: ...,  _: ... }`) and bind the call (`x := pick(v)`)");
        return 0;
    }
    if (value)
    {
        parser_error_at(p, t, "an 'if' used as a value takes arms: `if c { 0: ...,  _: ... }`");
        return 0;
    }
    p->if_then++;
    NodeId then = parse_branch(p);
    p->if_then--;
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
    int saved_if_then = p->if_then;         // an `else` inside the braces is theirs
    p->paren_depth = 0;
    p->if_then = 0;

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
    p->if_then = saved_if_then;
    parser_leave(p);
    if (!parser_consume(p, TOK_RBRACE, "expected '}' at the end of the block"))
    {
        list_abort(&stmts);
        return 0;
    }
    NodeId head = list_finish(u, &stmts);
    return node_new(u, AST_BLOCK, tok_idx(p, open), head, 0);
}
