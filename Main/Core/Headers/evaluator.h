/*
 * evaluator.h — Standalone AST Evaluator & Private C Intrinsics Engine
 *
 * Purpose:
 *   Evaluates AST trees stored in `MemArena` using `SymTable` for type-based
 *   operator/function multiple dispatch and private `@` intrinsic execution.
 */

#ifndef EVALUATOR_H
#define EVALUATOR_H

#include "mem_arena.h"
#include "sym_table.h"

#ifdef __cplusplus
extern "C" {
#endif

Value eval_ast(MemArena *mem, SymTable *tab,
               uint16_t node_idx, uint16_t frame_base, uint8_t cur_ret_type);
Value eval_dispatch_overload(MemArena *mem, SymTable *tab,
                             uint16_t sym_id, OpFixity fixity,
                             uint8_t argc, const Value *args);


#ifdef __cplusplus
}
#endif

#endif /* EVALUATOR_H */