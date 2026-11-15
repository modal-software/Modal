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
    // The earlier `value: { arms }`: say what a match is now.
    if (e && p->current->kind == TOK_COLON && (p->current[1].kind == TOK_LBRACE || p->current[1].kind == TOK_BODY) &&
        !p->current[1].nl)
    {
        parser_error_at(p, p->current, "a match is written `if value { arms }`: `if c { 48..57: ...,  _: ... }`");
        return 0;
    }
    return e;
}

// ---- prefix handlers -------------------------------------------------------------------------

static NodeId nud_if(Parser *p)
{
    return parse_if(p, 1);
}

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

// `point{ x, y: e }`: a struct literal. Each member is `name: value`, or `name` alone for
// `name: name` (a local or parameter of the member's name). Members are separated by `,` or a
// newline.
static NodeId parse_struct_literal(Parser *p, const Token *type)
{
    Unit *u = p->unit;
    NodeList inits = list_begin();
    const Token *open = p->current;
    parser_advance(p); // '{'
    p->paren_depth++;
    int saved_lit = p->no_literal;
    p->no_literal = 0;
    while (p->current->kind != TOK_RBRACE && p->current->kind != TOK_EOF)
    {
        const Token *name = p->current;
        if (!parser_consume(p, TOK_IDENTIFIER, "expected a member name in the literal (`point{ x, y: 0 }`)"))
        {
            break;
        }
        NodeId value;
        uint16_t written = 0;
        if (parser_match(p, TOK_COLON))
        {
            value = parse_precedence(p, PREC_ASSIGN);
            written = 1;
        }
        else
        {
            value = node_new(u, AST_IDENT, tok_idx(p, name), 0, 0); // `x`: the name x's value
        }
        if (!value)
        {
            break;
        }
        NodeId init = node_new(u, AST_FIELD_INIT, tok_idx(p, name), value, 0);
        NODE(u, init)->aux = written;
        node_list_push(u, &inits, init);
        if (!parser_match(p, TOK_COMMA) && p->current->kind != TOK_RBRACE && !p->current->nl)
        {
            parser_error_at(p, p->current, "expected ',' or a new line between the members of a literal");
            break;
        }
    }
    p->paren_depth--;
    p->no_literal = saved_lit;
    if (p->panic || !parser_consume(p, TOK_RBRACE, "expected '}' to close the literal"))
    {
        list_abort(&inits);
        return 0;
    }
    (void)open;
    return node_new(u, AST_STRUCT_LIT, tok_idx(p, type), list_finish(u, &inits), 0);
}

static NodeId nud_ident(Parser *p)
{
    parser_advance(p);
    const Token *name = p->previous;
    const Token *next = p->current;
    // `point{`: the brace right after the name (`if c {` is a value then its block)
    if (next->kind == TOK_LBRACE && !next->nl && !p->no_literal && name->off + name->len == next->off)
    {
        return parse_struct_literal(p, name);
    }
    return node_new(p->unit, AST_IDENT, tok_idx(p, name), 0, 0);
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
    if (!open->match || open == p->cond_open) // `if (c > 3) { ... }`: the value, grouped
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
    if (after->kind == TOK_COLON && !after[1].nl)
    {
        // `(x: u64): u64 { ... }`, as a function is written, or `(k: u64): k * limit`, one line
        // whose value is the result (its type, the expression's). An arm `(0, _): acc` is read as
        // an arm before any expression is parsed.
        return 1;
    }
    return after->kind == TOK_IDENTIFIER ||
           (after->kind == TOK_LBRACKET && after[1].kind == TOK_RBRACKET);
}

// After a literal's `)` and `:`: a result type then its body (`u64 {`), not an expression.
static int lit_result_type_at(const Token *t)
{
    t += t->kind == TOK_STAR ? 1 : t->kind == TOK_LBRACKET && t[1].kind == TOK_RBRACKET ? 2 : 0;
    return t->kind == TOK_IDENTIFIER && !t[1].nl && is_body_start(t + 1);
}

// (params) [T] { body }   or   (params): expression
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
    if (p->current->kind == TOK_COLON && !lit_result_type_at(p->current + 1))
    {
        parser_advance(p); // `(k: u64): k * limit`: one line, the result is its value
        NodeId body = parse_expression(p);
        if (!body)
        {
            return 0;
        }
        NodeId fn = node_new_fn(u, AST_FN_LIT, tok_idx(p, open), params, 0, body);
        NODE(u, fn)->flags |= (uint8_t)(AST_F_ONELINE | (has_self ? AST_F_SELF : 0));
        return fn;
    }
    if (!is_body_start(p->current))
    {
        parser_match(p, TOK_COLON); // `(x: u64): u64 { ... }` or `(x: u64) u64 { ... }`
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
        NodeId arg;
        const Token *t = p->current;
        if (t->kind == TOK_IDENTIFIER && t[1].kind == TOK_COLON && !t[2].nl)
        {
            // `x: x * 2`: a literal of one parameter, x, whose type is the one the callee's
            // `function(T): U` gives it (context.md §61)
            parser_advance(p);
            parser_advance(p);
            NodeId param = node_new(u, AST_PARAM, tok_idx(p, t), 0, 0);
            NodeId body = parse_precedence(p, PREC_ASSIGN);
            if (!body)
            {
                break;
            }
            arg = node_new_fn(u, AST_FN_LIT, tok_idx(p, t), param, 0, body);
            NODE(u, arg)->flags |= AST_F_ONELINE;
        }
        else
        {
            arg = parse_precedence(p, PREC_ASSIGN);
        }
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

// A stage without a pattern, applied to `value`: a name alone is a function given the value
// (`x.(f)` is `f(x)`); anything else runs after it, and its value goes on (`x.(f())`). The
// sequence is a match whose only arm is `_`.
static NodeId stage_continue(Parser *p, NodeId value, NodeId e)
{
    Unit *u = p->unit;
    if (NODE(u, e)->kind == AST_IDENT)
    {
        NODE(u, value)->flags |= AST_F_LAST; // the one argument
        return node_new(u, AST_CALL, NODE(u, e)->tok, e, value);
    }
    NodeId any = node_new(u, AST_PATTERN, NODE(u, e)->tok, 0, 0);
    NODE(u, any)->aux = PAT_ANY;
    NodeId arm = node_new(u, AST_ARM, NODE(u, e)->tok, any, e);
    return node_new(u, AST_MATCH, NODE(u, e)->tok, value, arm);
}

// value.( ... ): a pipe stage (std.md §14). Arms are tried on the chain's head: every pattern stage
// of a chain is an arm of one match, so `n.(0..10: a).(_: b)` is `if n { 0..10: a, _: b }`, and a
// stage is partial (what no arm takes goes on). A stage without a pattern continues the arm opened
// last, or, before any arm, the value itself.
static NodeId led_stage(Parser *p, NodeId left)
{
    Unit *u = p->unit;
    const Token *open = p->current;
    parser_advance(p); // '('
    p->paren_depth++;
    if (arm_at(p->current))
    {
        int count = 0;
        NodeId arms = parse_stage_arms(p, &count);
        p->paren_depth--;
        if (!arms || !parser_consume(p, TOK_RPAREN, "expected ')' after the stage's arms"))
        {
            return 0;
        }
        AstNode *m = NODE(u, left);
        if (m->kind == AST_MATCH && (m->aux & MATCH_STAGE)) // the chain's head again: more arms of it
        {
            NodeId old = m->b;
            NodeList all = list_begin();
            for (NodeId a = old; a; a = NEXT(u, a))
            {
                node_list_push(u, &all, a);
            }
            for (NodeId a = arms; a; a = NEXT(u, a))
            {
                node_list_push(u, &all, a);
            }
            NODE(u, left)->b = list_finish(u, &all);
            NODE(u, left)->aux = (uint16_t)((NODE(u, left)->aux & 0xff) | ((count & 0xff) << 8));
            return left;
        }
        NodeId match = node_new(u, AST_MATCH, tok_idx(p, open), left, arms);
        NODE(u, match)->aux = (uint16_t)(MATCH_STAGE | MATCH_PARTIAL | ((count & 0xff) << 8));
        return match;
    }
    NodeId e = parse_precedence(p, PREC_NONE);
    p->paren_depth--;
    if (!e || !parser_consume(p, TOK_RPAREN, "expected ')' after the stage"))
    {
        return 0;
    }
    AstNode *m = NODE(u, left);
    if (m->kind == AST_MATCH && (m->aux & MATCH_STAGE) && MATCH_OPEN(m->aux))
    {
        if (MATCH_OPEN(m->aux) != 1)
        {
            parser_error_at(p, open, "this stage would continue several arms: put it inside the arm it is for");
            return 0;
        }
        NodeId last = m->b;
        while (NEXT(u, last))
        {
            last = NEXT(u, last);
        }
        NODE(u, last)->b = stage_continue(p, NODE(u, last)->b, e);
        return left;
    }
    return stage_continue(p, left, e);
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

    if (t->kind == TOK_LPAREN && !ast_import_root(u, left))
    {
        return led_stage(p, left);
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
    [TOK_IF] = {nud_if, NULL, PREC_NONE},
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
