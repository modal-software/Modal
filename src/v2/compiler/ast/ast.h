// ast.h (v2): compact, index-linked AST.
//
// A node is 24 bytes and lives in one flat array per file. Children are referenced by index
// (NodeId), never by pointer, so the array can sit in the arena and the tree stays valid no
// matter where it is stored. Lists (statements, members, params, args) are chained through
// `next`. Source text is reached through `tok`, an index into the unit's token stream.
#ifndef MODAL_V2_AST_H
#define MODAL_V2_AST_H

#include <stddef.h>
#include <stdint.h>

typedef struct Unit Unit;
typedef uint32_t NodeId; // 0 = no node

typedef enum
{
    AST_NONE,
    //                  tok          a            b            c
    AST_MODULE,      // -            decls
    AST_STRUCT_DECL, // name         members
    AST_FIELD_DECL,  // name         type         init
    AST_METHOD_DECL, // name         params       signature (AST_SIG: return type, body)
    AST_PARAM,       // name         type
    AST_DEFINE,      // name         value                               name := value
    AST_VAL_DECL,    // 'val'        items        source                 val (a as b): import(x)
    AST_TYPE_NAME,   // first ident  (aux = number of extra `.segment`s)
    AST_TYPE_SLICE,  // '['          element
    AST_TYPE_ARRAY,  // '['          element, length (an expression: a literal or a constant's name)
    AST_TYPE_PTR,    // '*'          pointee
    AST_TYPE_STRUCT, // 'struct'     members
    AST_FN_LIT,      // '('          params       signature (AST_SIG: return type, body)
    AST_BLOCK,       // '{'          statements
    AST_RETURN,      // 'return'     value
    AST_ASSIGN,      // '='          target       value
    AST_BIN_OP,      // operator     left         right
    AST_UNARY_OP,    // operator     operand
    AST_CALL,        // '(' / string callee       args
    AST_INDEX,       // '['          target       index
    AST_SELECTOR,    // member name  target
    AST_PAREN_GROUP, // '('          expr
    AST_IDENT,       // name
    AST_NUMBER_LIT,  // literal
    AST_STRING_LIT,  // literal
    AST_IMPORT,      // module arg   (aux = target unit id + 1, 0 = external; set by the linker)
    AST_IMPORT_ALL,  // '*'          chain                               import(x).*
    AST_IMPORT_PICK, // '('          chain        items                  import(x).(a, b as c)
    AST_IMPORT_ITEM, // name                                   alias TOKEN index (0 = none)
    AST_SIG, // return type, body (a node, or the TOK_BODY token of a body not parsed yet)
    AST_SITE, // the callee's name: a `site` the compiler filled, `file:line:col: <the call>`
    // Arms (docs/std.md §8): `value { pattern: result ... }`, or a function body made of arms,
    // which match its parameters (a = 0). Tried in order; the first that matches gives the result.
    AST_MATCH,   // '{'          subject (0: the parameters)   arms
    AST_ARM,     // first token  patterns     result (an expression, a statement or a block)
    AST_PATTERN, // literal      (aux: PAT_*, as a pattern parameter's)  -- a name is an AST_IDENT
    AST__COUNT,
} AstNodeKind;

enum
{
    AST_F_MUT = 1u << 0,     // type-first declaration: mutable field / overloadable method
    AST_F_ONELINE = 1u << 1, // method body is `: expression`
    AST_F_SELF = 1u << 2,    // callable whose first parameter is `self`
    // The body has not been parsed yet: `c` holds the TOKEN index of its '{', not a node.
    // It is parsed, checked, emitted and discarded one function at a time (see check.c).
    AST_F_LAZY = 1u << 3,
    // A parameter named exactly like its struct type (`arena: arena`): a context parameter,
    // a reference that calls fill in by themselves from the caller's value of that name.
    AST_F_CONTEXT = 1u << 4,
    // A type-first parameter with a literal where its name would be (`u8 fact(u8 0): 1`): a
    // pattern. Its token is the literal (the first one of a range); aux: PAT_* below.
    AST_F_PATTERN = 1u << 5,
    // The last element of the list it belongs to. A list's elements are consecutive nodes, so
    // the next sibling of `id` is `id + 1` unless this is set: no link is stored (16-byte nodes).
    AST_F_LAST = 1u << 6,
    // A print or write call whose bytes go to the program's `out` (sema/check.c,
    // route_output): its first argument is that `out`, and its ref is the `put` method.
    AST_F_SINK = 1u << 7,
    // On an AST_FIELD_DECL: `b` is where it is placed (`@ n`), not an initializer. In a struct,
    // its offset; at module level, the address of a device (memory-mapped registers).
    AST_F_PLACED = 1u << 7,
};

// A pattern parameter's aux (AST_F_PATTERN). A range `lo..hi` is tokens lo, `..`, hi (each with
// its `-` before it when negative); `lo..` has no hi and takes every value from lo up.
enum
{
    PAT_NEG = 1u << 0,    // the literal (or the range's low end) is negative
    PAT_RANGE = 1u << 1,  // `lo..hi`, both ends included
    PAT_NEG_HI = 1u << 2, // the high end is negative
    PAT_OPEN = 1u << 3,   // `lo..`: no high end
    PAT_TEXT = 1u << 4,   // a string literal (`[]u8 "help"`)
};

typedef struct AstNode
{
    uint8_t kind;
    uint8_t flags;
    uint16_t aux;
    uint32_t tok;
    NodeId a, b; // children; a list child is the id of its first element
} AstNode;

_Static_assert(sizeof(AstNode) == 16, "AstNode must stay 16 bytes");

typedef struct
{
    AstNode *data;
    size_t size;
    size_t cap;
} NodeVec;

#define NODE(u, id) (&(u)->nodes.data[(id)])
#define TOK(u, i) (&(u)->tokens.data[(i)])
// The next element of the list `id` is in, or 0 after the last.
#define NEXT(u, id) ((NODE(u, (id))->flags & AST_F_LAST) ? (NodeId)0 : (NodeId)((id) + 1))

// Which of a/b/c hold child nodes, per kind (bit 0 = a, bit 1 = b, bit 2 = c).
extern const uint8_t ast_child_mask[AST__COUNT];

typedef struct
{
    NodeId (*node)(Unit *u, AstNodeKind kind, uint32_t tok, NodeId a, NodeId b);
} AstConstructor;

typedef struct
{
    AstConstructor new;
    void (*print)(Unit *u);
    const char *(*kind_name)(AstNodeKind kind);
    // Renders a type node as source-like text into buf; returns buf.
    char *(*type_str)(const Unit *u, NodeId type, char *buf, size_t cap);
    // Renders the signature of a method declaration or function binding; returns buf.
    char *(*sig_str)(const Unit *u, NodeId decl, char *buf, size_t cap);
} AstImpl;

extern const AstImpl ast;

// If `expr` is an import chain (import(x), import(x).a.b, import(x).*, import(x).(a, b)),
// returns its AST_IMPORT node; otherwise 0.
NodeId ast_import_root(const Unit *u, NodeId expr);

static inline int is_self_token(const char *s, int len)
{
    return len == 4 && s[0] == 's' && s[1] == 'e' && s[2] == 'l' && s[3] == 'f';
}

#endif // MODAL_V2_AST_H
