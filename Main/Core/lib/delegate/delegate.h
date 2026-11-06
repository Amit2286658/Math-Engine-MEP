/*
 * delegate.h — Internal C `@` Intrinsic Delegate Registry & Execution Engine
 *
 * Location: Core/lib/delegate/delegate.h
 *
 * Purpose:
 *   Isolates all C-backed `@<intrinsic>` operations that extension modules
 *   delegate to via `@` syntax (`@add`, `@sub`, `@sin`, `@vnew`, `@vpush`,
 *   `@vshift`, `@vrange`, `@vin`, `@fcall`, `@mzero`, `@mmul`,
 *   `@mdet`, `@minv`, `@sym_diff`, `@sym_simplify`, `@tostr`,
 *   `@fromstr`, `@f64`, `@lparen`, `@seq`, `@store`, etc.).
 *
 *   Distinct from:
 *     - `Library/`           : External text/binary extension definitions
 *     - `Core/lib/hardcode/` : Hardcoded parser/evaluator ergonomic rules
 */

#ifndef CORE_DELEGATE_H
#define CORE_DELEGATE_H

#include "../../Headers/sym_table.h"
#include "../../Headers/evaluator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*Below section is the C's native lookup resolver and enum-order-independent predicates for operator/function delegates @<intrinsic>*/
/*Construct @f64, @lparen, @seq, @store, @add, @sin, @vnew, @mmul, @sym_diff, @tostr, @fromstr*/
IntrinsicId delegate_lookup_intrinsic(const char *s, uint16_t len);
uint32_t    delegate_intrinsic_table_hash(void);
bool        delegate_is_meta_intrinsic(IntrinsicId intr);
bool        delegate_is_sym_meta(uint8_t mtag);
bool        delegate_is_rel_intrinsic(IntrinsicId intr);
bool        delegate_is_vec_or_map_intrinsic(IntrinsicId intr);
bool        delegate_is_map_intrinsic(IntrinsicId intr);
bool        delegate_is_mat_intrinsic(IntrinsicId intr);
Value       delegate_construct_token(MemArena *mem, const SymTable *tab,
                                     uint16_t type_id, const char *src,
                                     uint16_t off, uint16_t len);
bool        delegate_extract_slice_indices(MemArena *mem, const SymTable *tab,
                                           Value idx_val, int dim_len,
                                           uint16_t *out_indices, uint16_t *out_count,
                                           uint16_t max_cap);

/*Below section is the C's native scalar, additive unit compatibility, and compound SI dimensional algebra helpers*/
/*Construct unit-checked arithmetic/comparison, compound unit cancellation & dimensional algebra (*, /, ^, √, cbrt), and integer index/dimension checks*/
bool    delegate_is_zero_unitless_scalar(const MemArena *mem, const SymTable *tab, Value v);
bool    delegate_check_additive_unit_compat(MemArena *mem, const SymTable *tab,
                                            const char *op_name, int op_len,
                                            Value a, Value b, uint8_t *out_unit);
uint8_t delegate_resolve_mul_unit(const MemArena *mem, const SymTable *tab,
                                  uint8_t ua, uint8_t ub);
uint8_t delegate_resolve_div_unit(const MemArena *mem, const SymTable *tab,
                                  uint8_t ua, uint8_t ub);
uint8_t delegate_resolve_pow_unit(const MemArena *mem, const SymTable *tab,
                                  uint8_t ua, uint8_t ub, double exp_val);
uint8_t delegate_resolve_root_unit(const MemArena *mem, const SymTable *tab,
                                   uint8_t ua, int root_n);
bool    delegate_val_to_exact_int(MemArena *mem, const SymTable *tab, Value v,
                                  const char *ctx, int *out_int);
Value   delegate_eval_range_step(MemArena *mem, const SymTable *tab,
                                 Value va, Value vb, Value vstep, int mode);

/*Below section is the C's native closure invocation engine for operator/function delegates @lambda and @fcall*/
/*Construct (x) => expr, f(x), call(f, x)*/
Value delegate_call_fn(MemArena *mem, SymTable *tab,
                       Value fn_val, uint8_t argc, const Value *args);

/*Below section is the C's native for operator/function delegates @vnew, @vfill, @vpush, @vpop, @vshift, @vget, @vset, @vlen, @vcap, @vresize, @vshrink, @vclear, @vrange, @vin, @vsort, @map_new..@map_del*/
/*Construct vec<T>(), list<T>(), stack<T>(), queue<T>(), map(), dict(), push, pop, enqueue, dequeue, .., ..., ..<, <.., in, into*/
void  delegate_vec_write_elem(MemArena *mem, const SymTable *tab,
                              uint16_t elem_tid, uint16_t data_idx, uint32_t offset, Value item);
Value delegate_vec_read_elem(MemArena *mem, const SymTable *tab,
                             uint16_t elem_tid, uint8_t elem_uid, uint16_t data_idx, uint32_t offset);
Value delegate_eval_vec_intrinsic(MemArena *mem, SymTable *tab,
                                  IntrinsicId intr, Value a, Value b, Value c);

/*Below section is the C's native for operator/function delegates @mzero, @mrows, @mcols, @mrank, @mlen, @mshape, @madd, @msub, @mscal, @mmul, @mtrans, @mdet, @minv, @mlinsolve, @mreshape, @meigvals, @meigvecs, @msvd*/
/*Construct [a, b ; c, d], A + B, A - B, A * B, s * A, det(A), transpose(A), inv(A), linsolve(A, b), eigvals, eigvecs, svd*/
bool  delegate_mat_get_info(const MemArena *mem, Value v,
                            uint8_t *out_rank, uint16_t *out_total_len,
                            uint16_t *out_data_idx, uint16_t dims[4]);
Value delegate_eval_mat_intrinsic(MemArena *mem, const SymTable *tab,
                                  IntrinsicId intr, Value a, Value b, Value c);

/*Below section is the C's native top-level dispatcher for operator/function delegates @add..@err, @tostr, @fromstr*/
/*Construct +, -, *, /, %, ^, √, sin..tanh, ==..~, &&..nand, type_of, unit_of, x_, _s, error*/
void  delegate_format_mat_at(const MemArena *mem, uint16_t hdr_idx, bool high_prec,
                             char *buf, size_t cap);
void  delegate_format_value(const MemArena *mem, const SymTable *tab,
                            Value val, char *buf, size_t buf_cap);
Value delegate_eval_intrinsic(MemArena *mem, SymTable *tab,
                              IntrinsicId intr, Value a, Value b, Value c, uint8_t ret_type);

/*Below section is the C's native for symbolic AST algebra delegates @sym_diff, @sym_simplify, @sym_eval, @sym_taylor*/
/*Construct sym_diff, sym_diff_n, sym_simplify, simplify, sym_eval, sym_taylor*/
Value delegate_eval_sym_meta(MemArena *mem, SymTable *tab,
                             uint8_t mtag, const AstNode *n,
                             uint16_t frame_base, uint8_t cur_ret_type);

#ifdef __cplusplus
}
#endif

#endif /* CORE_DELEGATE_H */
