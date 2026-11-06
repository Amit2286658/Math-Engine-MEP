/*
 * parser_internal.h — Internal helpers shared across parser_*.c compilation units
 */
#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

#include "parser.h"
#include "../lib/delegate/delegate.h"
#include "../lib/hardcode/hardcode.h"
#include <string.h>
#include <stdio.h>

bool is_var_modifier_kw(const char *s, uint16_t len);
bool is_try_kw(const char *s, uint16_t len);
bool is_catch_kw(const char *s, uint16_t len);
bool is_throw_kw(const char *s, uint16_t len);
int find_local_idx(const Parser *P, const char *name, uint16_t len);
uint8_t parser_alloc_anon_slot(Parser *P, uint16_t type_id);
int find_label_id(const Parser *P, const char *name, uint16_t len);
int parser_get_or_add_label(Parser *P, const char *name, uint16_t len);
bool sym_has_visible_func_ov(const SymTable *tab, uint16_t sym_id);
bool sym_has_func_ov_in_mod(const SymTable *tab, uint16_t sym_id, uint16_t mod_id,
                            uint16_t caller_mod, bool *out_is_priv);
uint8_t token_func_meta(const Parser *P, Token t);
uint8_t cur_op_meta(const Parser *P);
uint16_t skip_raw_ws(const char *src, uint16_t p, uint16_t len);
bool lex_at_is_syntax(const Lexer *lex, uint16_t p, uint8_t target_syn);
bool lex_peek_is_lbrace(const Lexer *lex);
bool lex_peek_is_lparen(const Lexer *lex);
bool lex_peek_is_ident_in(const Parser *P);
bool lex_is_two_var_in(const Parser *P);
bool lex_peek_is_branch_kw(const Parser *P);
int find_active_meta_sym(const SymTable *tab, uint8_t target_meta);
bool lex_is_multi_lhs(const Parser *P);
bool sym_has_non_lazy_func_ov(const SymTable *tab, uint16_t sym_id);
int find_active_sub_sym(const MemArena *mem, const SymTable *tab);
bool parse_var_type_unit_annot(Parser *P, uint8_t *out_lock_type, uint8_t *out_lock_unit);
bool ast_is_fn_expr_rec(const MemArena *mem, const SymTable *tab,
                        const Parser *P, uint16_t idx, uint8_t depth);
uint16_t build_store_or_compound(Parser *P, uint16_t op_sym_id,
                                 uint16_t lhs_ast, uint16_t rhs_ast);
uint16_t build_distributed_comp(Parser *P, OpFixity fix, uint16_t comp_sym,
                                uint16_t left, uint16_t right);

uint16_t parse_field_step(Parser *P, uint16_t left);
uint16_t parse_mat_literal(Parser *P);
uint16_t parse_index_step(Parser *P, uint16_t left);
uint16_t parse_fn_call_step(Parser *P, uint16_t fn_ast);
uint16_t parse_scope_block(Parser *P);
uint16_t parse_branch_body(Parser *P);
uint16_t parse_lambda_from_paren(Parser *P);
uint16_t parse_lambda_from_ident(Parser *P, const char *id_str, uint16_t id_len);
uint16_t parse_inline_fn_def(Parser *P, const char *fn_name, uint16_t fn_len);

uint16_t parse_if_construct(Parser *P, uint16_t if_sym_id);
uint16_t parse_switch_construct(Parser *P);
uint16_t parse_tag_op(Parser *P, AstKind kind, const char *op_name);
uint16_t parse_break_construct(Parser *P);
uint16_t parse_continue_construct(Parser *P);
uint16_t parse_return_construct(Parser *P);
uint16_t parse_throw_construct(Parser *P);
uint16_t parse_try_construct(Parser *P);
uint16_t resolve_qual_mod_assign(Parser *P, uint16_t qual_mod,
                                 const char *name, uint16_t len,
                                 uint16_t op_sym_id,
                                 uint8_t lock_type, uint8_t lock_unit);
uint16_t resolve_var_for_assign(Parser *P, const char *name, uint16_t len,
                                uint16_t op_sym_id,
                                uint8_t lock_type, uint8_t lock_unit);

uint16_t parse_nud(Parser *P);

#endif /* PARSER_INTERNAL_H */
