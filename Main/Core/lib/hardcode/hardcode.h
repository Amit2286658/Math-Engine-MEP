/*
 * hardcode.h — Hardcoded Language Grammar, Ergonomic Rules & Built-In Conventions
 *
 * Location: Core/lib/hardcode/hardcode.h
 *
 * Purpose:
 *   Centralizes all hardcoded language ergonomics and structural conventions:
 *     1. Reserved control-flow keywords (`if`, `or`, `then`, `else`, `default`,
 *        `switch`, `case`, `for`, `jump`, `run`, `break`, `continue`, `return`)
 *     2. Built-in generic entity/unit wildcards (`$T`, `$any`, `$none`) and
 *        default `:real` parameter type fallback
 *     3. Automatic synthesis of lower-arity overloads for default parameters
 *     4. Newline `\n` as a default `;` statement separator
 *     5. Same-line implicit multiplication (`2x`, `2π`, `3(x + 1)`, `2 sin(x)`)
 *     6. Context-aware `in` operator disambiguation (`25km in m` vs `x in vec`
 *        vs `(i in 1...5) { ... }` / `[x^2 for x in 1...5]`)
 *     7. Lookahead for parenthesized lambdas `(x, y) => expr` and comprehensions
 *     8. Generic container constructors `vec<T>(...)`, `list<T>(...)`,
 *        `stack<T>(...)`, `queue<T>(...)` and variadic construction
 *     9. O(1) range/collection `AST_FOR_IN` loops, `AST_UNPACK_ELEM` destructuring,
 *        and pattern-matched `AST_SWITCH_EQ` evaluation
 */

#ifndef HARDCODE_H
#define HARDCODE_H

#include "../../Headers/mem_arena.h"
#include "../../Headers/sym_table.h"
#include "../../Headers/lexer.h"
#include "../../Headers/parser.h"
#include "../../Headers/evaluator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1. Reserved Keywords & Built-In Type/Unit Wildcards (`$T`, `$any`, `$none`, `real`) */
bool     hardcode_is_reserved_kw(const SymTable *tab, const MemArena *mem,
                                 const char *name, uint16_t len);
uint16_t hardcode_resolve_wildcard_type(const SymTable *tab, const MemArena *mem,
                                        const char *name, uint16_t len);
uint8_t  hardcode_resolve_wildcard_unit(const SymTable *tab, const MemArena *mem,
                                        const char *name, uint16_t len);
uint16_t hardcode_default_real_type(const MemArena *mem);

/* 2. Default Parameter Overload Wrapper Synthesis (`param = default_expr`) */
void     hardcode_synthesize_default_overloads(MemArena *mem, SymTable *tab,
                                               const char *src, uint16_t sym_id,
                                               uint16_t cur_mod, uint16_t base_ov_idx,
                                               uint8_t pcount, uint8_t req_count,
                                               const StrSlice *pnames,
                                               const uint16_t *ptypes,
                                               const uint8_t  *punits,
                                               const uint16_t *def_start,
                                               const uint16_t *def_end,
                                               uint16_t rtid, uint8_t ruid);

/* 3. Parser Hardcoded Ergonomics (Newline `;`, Implicit `*`, `in`, Lambdas, Comprehensions) */
bool     hardcode_can_start_newline_stmt(const Parser *P);
bool     hardcode_can_start_implicit_mul_rhs(const Parser *P);
bool     hardcode_is_paren_lambda(const Parser *P);
bool     hardcode_is_inline_fn_def(const Parser *P);
bool     hardcode_rhs_starts_lambda(const Parser *P);
bool     hardcode_scan_comprehension_var(const Parser *P,
                                         const char **out_idx_name, uint16_t *out_idx_len,
                                         const char **out_vname, uint16_t *out_vlen);
bool     hardcode_is_for_kw(const MemArena *mem, const SymTable *tab,
                            const char *name, uint16_t len);
bool     hardcode_is_in_kw(const MemArena *mem, const SymTable *tab,
                           const char *name, uint16_t len);
bool     hardcode_is_plain_store_sym(const MemArena *mem, const SymTable *tab,
                                     uint16_t sym_id, bool *out_is_colon_eq);
bool     hardcode_is_rel_comparison_op(const MemArena *mem, const SymTable *tab,
                                       uint16_t sym_id);
bool     hardcode_is_additive_or_rel_sym(const MemArena *mem, const SymTable *tab,
                                         uint16_t sym_id, OpFixity fixity, bool *out_is_rel);
bool     hardcode_sym_is_postfix_unit(const SymTable *tab, uint16_t sym_id);
bool     hardcode_is_in_op_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id);
bool     hardcode_is_into_op_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id);
bool     hardcode_ast_is_bare_unit_rhs(const MemArena *mem, uint16_t ast_idx);
bool     hardcode_check_undef_in_lhs(MemArena *mem, const SymTable *tab, uint16_t in_ast_idx);
bool     hardcode_should_use_unit_for_in_rhs(const MemArena *mem, const SymTable *tab,
                                             const AstNode *in_node, Value lhs_val,
                                             Value rhs_val, uint16_t *out_unit_sym);
bool     hardcode_is_vec_ctor_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id);
bool     hardcode_is_map_ctor_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id);
bool     hardcode_try_parse_container_type_arg(Parser *P, const char *id_str, uint16_t id_len,
                                               uint16_t *out_spec);
uint16_t hardcode_parse_pipe_rhs(Parser *P, uint16_t left, int next_min);
uint16_t hardcode_rewrite_range_chain(Parser *P, OpFixity fix, uint16_t sym_id, uint16_t left, uint16_t right);

/* 4. Evaluator Hardcoded Ergonomics (Variadic `vec`/`list`/`map`, `in` Range, `AST_FOR_IN`, Unpack, `switch`) */
bool     hardcode_try_eval_variadic_vec(MemArena *mem, SymTable *tab,
                                        const AstNode *n, uint16_t frame_base,
                                        uint8_t cur_ret_type, Value *out_val);
bool     hardcode_eval_in_vrange_fast(MemArena *mem, SymTable *tab,
                                      const AstNode *n, uint16_t frame_base,
                                      uint8_t cur_ret_type, Value *out_val);
Value    hardcode_eval_for_in_ast(MemArena *mem, SymTable *tab,
                                  const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type);
Value    hardcode_eval_unpack_elem_ast(MemArena *mem, const SymTable *tab,
                                       const AstNode *n, uint16_t frame_base);
Value    hardcode_eval_switch_eq_ast(MemArena *mem, SymTable *tab,
                                     const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type);

#ifdef __cplusplus
}
#endif

#endif /* HARDCODE_H */
