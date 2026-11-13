// parse_expr.c (v2): Pratt expression engine.
//
// Each token kind has at most one prefix handler (NUD), one infix handler (LED) and an infix
// precedence, held in a table indexed by TokenKind. The core loop is
//
//     left = nud();  while (prec < precedence(current)) left = led(left);
//
// so a chain of operators costs one loop iteration each instead of one call per grammar level.
#include "v2/compiler/syntax/parser/parser.h"

typedef enum
{
    PREC_NONE,
    PREC_ASSIGN, // =
    PREC_OR,     // or
    PREC_AND,    // and
    PREC_CMP,    // == != < > <= >=
    PREC_TERM,   // + -
    PREC_FACTOR, // * /
    PREC_UNARY,  // - ! & *
    PREC_CALL,   // . () [] "string"
} Precedence;

typedef NodeId (*NudFn)(Parser *p);
typedef NodeId (*LedFn)(Parser *p, NodeId left);

typedef struct
{
    NudFn nud;
    LedFn led;
    uint8_t prec;
} ParseRule;

static const ParseRule rules[TOK__COUNT];

static NodeId parse_precedence(Parser *p, int prec)
{
    const ParseRule *rule = &rules[p->current->kind];
    if (!rule->nud)
    {
        parser_error_at(p, p->current, "expected an expression");
        return 0;
    }
    if (!parser_enter(p))
    {
        return 0;
    }

    NodeId left = rule->nud(p);
    while (left)
    {
        const Token *t = p->current;
        rule = &rules[t->kind];
        if (!rule->led || rule->prec <= prec)
        {
            break;
        }
        // A newline ends the expression, except inside ( ) / [ ] and before a leading '.'.
        if ((t->nl) && p->paren_depth == 0 && t->kind != TOK_DOT)
        {
            break;
        }
        left = rule->led(p, left);
    }

    parser_leave(p);
    return left;
}

NodeId parse_expression(Parser *p)
{
    NodeId e = parse_precedence(p, PREC_NONE);
    // `value: { arms }`: `:` takes the value, the arms match it.
    if (e && p->current->kind == TOK_COLON && p->current[1].kind == TOK_LBRACE && !p->current[1].nl)
    {
        parser_advance(p);
        return parse_arm_block(p, e);
    }
    if (e && p->current->kind == TOK_COLON && p->current[1].kind == TOK_BODY && !p->current[1].nl)
    {
        // At module level the scanner keeps every `{` as a function body, not yet tokenized.
        parser_error_at(p, p->current + 1,
                        "a match at module level is not supported yet: write it in a function "
                        "(`R pick(T n) { 0: ...  _: ... }`) and bind the call (`x := pick(v)`)");
        return 0;
    }
    return e;
}

// ---- prefix handlers -------------------------------------------------------------------------

static NodeId nud_number(Parser *p)
{
    parser_advance(p);
    return node_new(p->unit, AST_NUMBER_LIT, tok_idx(p, p->previous), 0, 0);
}

static NodeId nud_string(Parser *p)
{
    parser_advance(p);
    return node_new(p->unit, AST_STRING_LIT, tok_idx(p, p->previous), 0, 0);
}

static NodeId nud_ident(Parser *p)
{
    parser_advance(p);
    return node_new(p->unit, AST_IDENT, tok_idx(p, p->previous), 0, 0);
}

static NodeId nud_unary(Parser *p)
{
    const Token *op = p->current;
    parser_advance(p);
    NodeId operand = parse_precedence(p, PREC_UNARY);
    return operand ? node_new(p->unit, AST_UNARY_OP, tok_idx(p, op), operand, 0) : 0;
}

// `(` opens either a grouped expression or a function literal `(params) [T] { ... }`.
// The scanner stored the index of the matching `)`, so one look at the token after it decides
// which, in O(1) and without parsing anything twice.
static int paren_opens_fn_literal(const Parser *p, const Token *open)
{
    if (!open->match)
    {
        return 0;
    }
    const Token *after = TOK(p->unit, open->match) + 1;
    if (is_body_start(after))
    {
        return 1;
    }
    if (after->nl)
    {
        return 0;
    }
    return after->kind == TOK_IDENTIFIER ||
           (after->kind == TOK_LBRACKET && after[1].kind == TOK_RBRACKET);
}

// (params) [T] { body }
static NodeId parse_fn_literal(Parser *p)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    int has_self = 0, deferred = 0;
    NodeId ret = 0;
    NodeId params = parse_params(p, &has_self);
    if (p->panic)
    {
        return 0;
    }
    if (!is_body_start(p->current))
    {
        ret = parse_type(p);
        if (!ret)
        {
            return 0;
        }
    }
    NodeId body = parse_body(p, &deferred);
    if (!body)
    {
        return 0;
    }
    NodeId fn = node_new_fn(u, AST_FN_LIT, tok_idx(p, open), params, ret, body);
    NODE(u, fn)->flags |= (uint8_t)((has_self ? AST_F_SELF : 0) | (deferred ? AST_F_LAZY : 0));
    NODE(u, NODE(u, fn)->b)->flags |= deferred ? AST_F_LAZY : 0; // its signature node knows too
    return fn;
}

static NodeId nud_paren(Parser *p)
{
    Unit *u = p->unit;
    const Token *open = p->current;

    if (paren_opens_fn_literal(p, open))
    {
        return parse_fn_literal(p);
    }

    parser_advance(p);
    p->paren_depth++;
    NodeId inner = parse_precedence(p, PREC_NONE);
    p->paren_depth--;
    if (!inner || !parser_consume(p, TOK_RPAREN, "expected ')' to close the group"))
    {
        return 0;
    }
    return node_new(u, AST_PAREN_GROUP, tok_idx(p, open), inner, 0);
}

// import("std")   import(std)   -- a comptime macro; selectors are applied by led_dot.
static NodeId nud_import(Parser *p)
{
    parser_advance(p);
    if (!parser_consume(p, TOK_LPAREN, "expected '(' after import"))
    {
        return 0;
    }
    const Token *arg = p->current;
    if (arg->kind != TOK_STRING && arg->kind != TOK_IDENTIFIER)
    {
        parser_error_at(p, arg, "import takes a module name: import(\"std\") or import(std)");
        return 0;
    }
    parser_advance(p);
    if (!parser_consume(p, TOK_RPAREN, "expected ')' after the module name"))
    {
        return 0;
    }
    return node_new(p->unit, AST_IMPORT, tok_idx(p, arg), 0, 0);
}

// ---- infix handlers --------------------------------------------------------------------------

static NodeId led_binary(Parser *p, NodeId left)
{
    const Token *op = p->current;
    parser_advance(p);
    NodeId right = parse_precedence(p, rules[op->kind].prec); // left-associative
    return right ? node_new(p->unit, AST_BIN_OP, tok_idx(p, op), left, right) : 0;
}

static NodeId led_assign(Parser *p, NodeId left)
{
    const Token *op = p->current;
    parser_advance(p);
    NodeId value = parse_precedence(p, PREC_ASSIGN - 1); // right-associative
    return value ? node_new(p->unit, AST_ASSIGN, tok_idx(p, op), left, value) : 0;
}

static NodeId led_call(Parser *p, NodeId left)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    NodeList args = list_begin();

    parser_advance(p);
    p->paren_depth++;
    while (p->current->kind != TOK_RPAREN && p->current->kind != TOK_EOF)
    {
        NodeId arg = parse_precedence(p, PREC_ASSIGN);
        if (!arg)
        {
            break;
        }
        node_list_push(u, &args, arg);
        if (!parser_match(p, TOK_COMMA))
        {
            break;
        }
    }
    p->paren_depth--;

    if (p->panic || !parser_consume(p, TOK_RPAREN, "expected ')' after the arguments"))
    {
        list_abort(&args);
        return 0;
    }
    NodeId head = list_finish(u, &args);
    return node_new(u, AST_CALL, tok_idx(p, open), left, head);
}

// Lua-style call with a single string argument: print "hello"
static NodeId led_string_call(Parser *p, NodeId left)
{
    Unit *u = p->unit;
    uint8_t kind = NODE(u, left)->kind;

    if (kind != AST_IDENT && kind != AST_SELECTOR)
    {
        parser_error_at(p, p->current, "unexpected string after an expression");
        return 0;
    }
    NodeId arg = nud_string(p);
    uint32_t tok = NODE(u, arg)->tok;
    NodeList args = list_begin(); // a list of one: arguments are always a list
    node_list_push(u, &args, arg);
    NodeId head = list_finish(u, &args);
    return node_new(u, AST_CALL, tok, left, head);
}

static NodeId led_index(Parser *p, NodeId left)
{
    const Token *open = p->current;
    parser_advance(p);
    p->paren_depth++;
    NodeId index = parse_precedence(p, PREC_NONE);
    p->paren_depth--;
    if (!index || !parser_consume(p, TOK_RBRACKET, "expected ']' after the index"))
    {
        return 0;
    }
    return node_new(p->unit, AST_INDEX, tok_idx(p, open), left, index);
}

// left.name   and, on an import chain only:   left.*   left.(a, b as c)
static NodeId led_dot(Parser *p, NodeId left)
{
    Unit *u = p->unit;
    uint8_t left_kind = NODE(u, left)->kind;
    parser_advance(p); // '.'
    const Token *t = p->current;

    if (left_kind == AST_IMPORT_ALL || left_kind == AST_IMPORT_PICK)
    {
        parser_error_at(p, t, "nothing can follow `.*` or an import list");
        return 0;
    }

    if (t->kind == TOK_IDENTIFIER)
    {
        parser_advance(p);
        return node_new(u, AST_SELECTOR, tok_idx(p, t), left, 0);
    }

    if (t->kind == TOK_STAR || t->kind == TOK_LPAREN)
    {
        if (!ast_import_root(u, left))
        {
            parser_error_at(p, t, "`.*` and `.(...)` are only valid on an import");
            return 0;
        }
        parser_advance(p);
        if (t->kind == TOK_STAR)
        {
            return node_new(u, AST_IMPORT_ALL, tok_idx(p, t), left, 0);
        }

        p->paren_depth++;
        NodeId items = parse_import_items(p, t);
        p->paren_depth--;
        if (!parser_consume(p, TOK_RPAREN, "expected ')' to close the import list"))
        {
            return 0;
        }
        return node_new(u, AST_IMPORT_PICK, tok_idx(p, t), left, items);
    }

    parser_error_at(p, t, "expected a member name after '.'");
    return 0;
}

// ---- dispatch table --------------------------------------------------------------------------

static const ParseRule rules[TOK__COUNT] = {
    [TOK_NUMBER] = {nud_number, NULL, PREC_NONE},
    [TOK_STRING] = {nud_string, led_string_call, PREC_CALL},
    [TOK_IDENTIFIER] = {nud_ident, NULL, PREC_NONE},
    [TOK_WRITE] = {nud_ident, NULL, PREC_NONE}, // write(...) parses as an ordinary call
    [TOK_IMPORT] = {nud_import, NULL, PREC_NONE},
    [TOK_LPAREN] = {nud_paren, led_call, PREC_CALL},
    [TOK_LBRACKET] = {NULL, led_index, PREC_CALL},
    [TOK_DOT] = {NULL, led_dot, PREC_CALL},
    [TOK_EQ] = {NULL, led_assign, PREC_ASSIGN},
    [TOK_OR] = {NULL, led_binary, PREC_OR},
    [TOK_AND] = {NULL, led_binary, PREC_AND},
    [TOK_CMP] = {NULL, led_binary, PREC_CMP},
    [TOK_NEQ] = {NULL, led_binary, PREC_CMP},
    [TOK_LT] = {NULL, led_binary, PREC_CMP},
    [TOK_GT] = {NULL, led_binary, PREC_CMP},
    [TOK_LE] = {NULL, led_binary, PREC_CMP},
    [TOK_GE] = {NULL, led_binary, PREC_CMP},
    [TOK_PLUS] = {NULL, led_binary, PREC_TERM},
    [TOK_MINUS] = {nud_unary, led_binary, PREC_TERM},
    [TOK_STAR] = {nud_unary, led_binary, PREC_FACTOR},
    [TOK_SLASH] = {NULL, led_binary, PREC_FACTOR},
    [TOK_BANG] = {nud_unary, NULL, PREC_NONE},
    [TOK_AMP] = {nud_unary, NULL, PREC_NONE},
};
