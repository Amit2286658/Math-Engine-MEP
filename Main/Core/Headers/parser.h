/*
 * parser.h — Standalone Pratt Parser & AST Builder Module
 *
 * Purpose:
 *   Consumes tokens from `Lexer` and builds compact 8-byte `AstNode` trees in
 *   `MemArena` using operator binding powers, lexical `{ ... }` scopes,
 *   module-scoped variables (`pub`/`priv`, `:=` mutable vs `=`/`const` immutable),
 *   `if ... then ... else` chains, `switch` blocks, `cond ? x : y` ternary
 *   expressions, and `Tag : { ... }` labeled scopes from `SymTable`.
 */

#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARSER_MAX_LOCALS      64
#define PARSER_MAX_EXPR_DEPTH 128

/* Bit flags stored in `AstNode.ref` on `AST_SLOT_LOAD` */
#define AST_SLOT_UNDEF_ITER_BIT 0x4000u /* Variable auto-allocated on LHS of `in` */
#define AST_SLOT_DUAL_UNIT_BIT  0x8000u /* Variable on RHS of `in` also matches unit sym */
#define AST_SLOT_SYM_MASK       0x3FFFu

/* Bit flag stored in `AstNode.sub` on `AST_SLOT_LOAD` when declaring a local `const` */
#define AST_SLOT_CONST_DECL_BIT 0x80u
#define AST_SLOT_IDX_MASK       0x7Fu

typedef struct {
    StrSlice name;
    uint16_t type_id;
    uint8_t  slot;
    uint8_t  scope_depth;
    uint8_t  is_loop_iter;
    uint8_t  is_const;
} LocalVar;

typedef struct {
    StrSlice name;
    uint16_t body_ast;
    bool     defined;
} LabelDef;

typedef struct {
    Lexer      lex;
    bool       in_ext_body;
    bool       is_mod_scope;      /* True when top-level (`scope_depth == 0`) binds to `SymTable.vars` */
    uint16_t   cur_mod;           /* Active module ID for privacy & module-variable binding            */
    uint8_t    pending_var_flags; /* Modifier flags (`SYM_VAR_*`) from `pub`/`priv`/`const`/`mut`      */
    bool       has_pending_mods;
    LocalVar   locals[PARSER_MAX_LOCALS];
    uint16_t   slot_types[PARSER_MAX_LOCALS];
    uint8_t    vis_count;
    uint8_t    local_count;
    uint8_t    param_count;
    uint8_t    scope_depth;
    uint8_t    in_ternary_then;
    uint8_t    in_cond_expr;
    uint8_t    in_paren_expr;
    uint8_t    in_symbolic_expr;
    uint16_t   expr_depth;
    bool       last_was_block;
    LabelDef   labels[MEM_MAX_LABELS];
    uint8_t    label_count;
} Parser;

void     parser_init(Parser *P, MemArena *mem, SymTable *tab,
                     const char *src, uint16_t len, uint16_t start_pos,
                     bool in_ext_body, bool stop_at_newline);

int      parser_add_local(Parser *P, const char *name, uint16_t len, uint16_t type_id);
uint16_t parser_parse_expr(Parser *P, int min_prec);
bool     parser_check_labels(Parser *P);

#ifdef __cplusplus
}
#endif

#endif /* PARSER_H */