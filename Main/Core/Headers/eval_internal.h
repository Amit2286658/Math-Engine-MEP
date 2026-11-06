/*
 * eval_internal.h — Internal helpers shared across evaluator compilation units
 */
#ifndef EVAL_INTERNAL_H
#define EVAL_INTERNAL_H

#include "evaluator.h"
#include "../compile/compiler.h"
#include "../lib/delegate/delegate.h"
#include "../lib/hardcode/hardcode.h"
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

Value eval_seq_chain(MemArena *mem, SymTable *tab,
                     uint16_t seq_root, uint16_t frame_base, uint8_t cur_ret_type);
Value eval_dispatch_overload_in_mod(MemArena *mem, SymTable *tab,
                                    uint16_t sym_id, OpFixity fixity,
                                    uint8_t argc, const Value *args,
                                    uint16_t filter_mod);
Value eval_index_op(MemArena *mem, SymTable *tab,
                    const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type,
                    bool is_write, Value rhs);
Value eval_mat_lit_ast(MemArena *mem, SymTable *tab,
                       const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type);
Value eval_file_io_op(MemArena *mem, SymTable *tab, IntrinsicId sub_op, Value a, Value b);
Value eval_json_parse_op(MemArena *mem, SymTable *tab, Value a);
Value eval_json_stringify_op(MemArena *mem, const SymTable *tab, Value a, Value b);

#endif /* EVAL_INTERNAL_H */
