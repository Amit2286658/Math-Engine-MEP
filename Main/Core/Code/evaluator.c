/*
 * evaluator.c — Standalone AST Evaluator & Multiple-Dispatch Engine
 *
 * Routes:
 *   - All `@<intrinsic>` operations -> `Core/lib/delegate/delegate.h`
 *   - Hardcoded ergonomic AST nodes (`AST_FOR_IN`, `AST_UNPACK_ELEM`,
 *     `AST_SWITCH_EQ`, variadic `vec`/`list`, additive/rel operator rules)
 *     -> `Core/lib/hardcode/hardcode.h`
 */

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

/* ============================================================================
 * 1. TAGGED SCOPE (`Tag : { ... }`) & SEQUENCE JUMP/BREAK RESOLUTION
 * ============================================================================ */

static void scan_all_labels_in_tree(MemArena *mem, uint16_t node_idx) {
    if (node_idx == MEM_NO_IDX) return;
    const AstNode *n = mem_get_ast(mem, node_idx);
    if (n->kind == AST_TAGGED_SCOPE && n->sub < MEM_MAX_LABELS) {
        mem->label_ast[n->sub] = n->left;
    }
    if (n->kind != AST_LIT && n->kind != AST_CONST_SYM &&
        n->kind != AST_SLOT_LOAD && n->kind != AST_JUMP &&
        n->kind != AST_RUN_TAG && n->kind != AST_BREAK &&
        n->kind != AST_CONTINUE && n->kind != AST_RETURN &&
        n->kind != AST_LAMBDA && n->kind != AST_UNPACK_ELEM) {
        scan_all_labels_in_tree(mem, n->left);
        if (n->kind != AST_FIELD_LOAD) {
            scan_all_labels_in_tree(mem, n->right);
        }
    }
}

static int stmt_direct_label_id(const MemArena *mem, const SymTable *tab, uint16_t stmt_idx) {
    if (stmt_idx == MEM_NO_IDX) return -1;
    const AstNode *n = mem_get_ast(mem, stmt_idx);
    if (n->kind == AST_TAGGED_SCOPE) {
        return (int)n->sub;
    }
    if (n->kind == AST_BINARY &&
        sym_active_meta_tag(tab, n->ref, (OpFixity)n->sub) == INTR_STORE &&
        n->right != MEM_NO_IDX) {
        const AstNode *rhs = mem_get_ast(mem, n->right);
        if (rhs->kind == AST_TAGGED_SCOPE) {
            return (int)rhs->sub;
        }
    }
    return -1;
}

static void collect_seq_stmts(const MemArena *mem, const SymTable *tab,
                              uint16_t node_idx, uint16_t *stmts, uint16_t *count, uint16_t cap) {
    if (node_idx == MEM_NO_IDX || *count >= cap) return;
    const AstNode *n = mem_get_ast(mem, node_idx);
    if (n->kind == AST_BINARY &&
        sym_active_meta_tag(tab, n->ref, (OpFixity)n->sub) == INTR_SEQ) {
        collect_seq_stmts(mem, tab, n->left, stmts, count, cap);
        collect_seq_stmts(mem, tab, n->right, stmts, count, cap);
        return;
    }
    stmts[(*count)++] = node_idx;
}

static Value eval_seq_chain(MemArena *mem, SymTable *tab,
                            uint16_t seq_root, uint16_t frame_base, uint8_t cur_ret_type) {
    uint16_t stmts[64];
    uint16_t count = 0;
    collect_seq_stmts(mem, tab, seq_root, stmts, &count, 64);

    uint16_t stmt_for_label[MEM_MAX_LABELS];
    for (uint8_t i = 0; i < MEM_MAX_LABELS; ++i) {
        stmt_for_label[i] = MEM_NO_IDX;
    }

    for (uint16_t i = 0; i < count; ++i) {
        scan_all_labels_in_tree(mem, stmts[i]);
        int lid = stmt_direct_label_id(mem, tab, stmts[i]);
        if (lid >= 0 && lid < MEM_MAX_LABELS) {
            stmt_for_label[lid] = i;
        }
    }

    Value last = val_void();
    uint16_t pc = 0;
    uint32_t guard = 0;

    while (pc < count && !mem->has_error) {
        Value step_val = eval_ast(mem, tab, stmts[pc], frame_base, cur_ret_type);
        if (mem->has_error) return val_void();

        if (mem->jump_active == JUMP_GOTO) {
            uint8_t target = mem->jump_target_id;
            if (target < MEM_MAX_LABELS && stmt_for_label[target] != MEM_NO_IDX) {
                mem->jump_active = JUMP_NONE;
                pc = stmt_for_label[target];
                if (++guard > 1000000u) {
                    mem_err(mem, "Jump loop iteration limit exceeded");
                    return val_void();
                }
                continue;
            }
            return val_void();
        }
        if (mem->jump_active != JUMP_NONE) {
            if (val_get_type(mem, step_val) != MEM_TYPE_VOID) {
                last = step_val;
            }
            return last;
        }
        last = step_val;
        pc++;
    }
    return last;
}

/* ============================================================================
 * 2. MULTIPLE DISPATCH (ENTITY TYPES + UNIT TYPES + GENERIC `T`)
 * ============================================================================ */

static Value eval_dispatch_overload_in_mod(MemArena *mem, SymTable *tab,
                                           uint16_t sym_id, OpFixity fixity,
                                           uint8_t argc, const Value *args,
                                           uint16_t filter_mod) {
    uint8_t arg_types[SYM_MAX_OV_PARAMS] = { 0 };
    uint8_t arg_units[SYM_MAX_OV_PARAMS] = { 0 };
    for (uint8_t i = 0; i < argc && i < SYM_MAX_OV_PARAMS; ++i) {
        arg_types[i] = (uint8_t)val_get_type(mem, args[i]);
        arg_units[i] = val_get_unit(mem, args[i]);
    }

    const OpOverload *matched = NULL;
    int      best_score        = -1;
    uint8_t  matched_bound_T   = MEM_UNIT_NONE;
    bool     matched_has_T     = false;
    bool     type_only_matched = false;
    uint16_t priv_mod          = MEM_NO_IDX;

    uint16_t first_ov = (sym_id < tab->sym_count) ? tab->sym_first_ov[sym_id] : MEM_NO_IDX;
    for (uint16_t i = first_ov; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != (uint8_t)fixity) continue;
        if (ov->param_count != argc) continue;
        if (filter_mod != MEM_NO_IDX && ov->mod_id != filter_mod) continue;
        if (!sym_mod_enabled(tab, ov->mod_id)) continue;
        if ((ov->reserved & SYM_OV_PRIVATE) != 0 && ov->mod_id != mem->cur_mod) {
            priv_mod = ov->mod_id;
            continue;
        }

        bool type_ok = true;
        int  score   = 0;
        for (uint8_t p = 0; p < argc; ++p) {
            if (ov->param_types[p] == arg_types[p]) {
                score += 4;
            } else if (ov->param_types[p] == SYM_TYPE_ANY) {
                score += 1;
            } else {
                type_ok = false;
                break;
            }
        }
        if (!type_ok) continue;
        type_only_matched = true;

        bool    unit_ok     = true;
        bool    has_bound_T = false;
        uint8_t bound_T     = MEM_UNIT_NONE;

        for (uint8_t p = 0; p < argc; ++p) {
            uint8_t pu = ov->param_units[p];
            uint8_t au = arg_units[p];
            if (pu == SYM_UNIT_OMITTED || pu == 0) {
                score += 1;
            } else if (pu == SYM_UNIT_GENERIC) {
                score += 2;
                if (!has_bound_T) {
                    has_bound_T = true;
                    bound_T     = au;
                } else if (au != bound_T) {
                    if (delegate_is_zero_unitless_scalar(mem, tab, args[p])) {
                        /* universal 0 compatible with bound_T */
                    } else if (bound_T == MEM_UNIT_NONE && p > 0 &&
                               delegate_is_zero_unitless_scalar(mem, tab, args[0])) {
                        bound_T = au;
                    } else {
                        unit_ok = false;
                        break;
                    }
                }
            } else if (pu == SYM_UNIT_LOCK_NONE) {
                if (au != MEM_UNIT_NONE) { unit_ok = false; break; }
                score += 8;
            } else {
                if (au != pu) { unit_ok = false; break; }
                score += 8;
            }
        }
        if (!unit_ok) continue;

        if (score >= best_score) {
            best_score      = score;
            matched         = ov;
            matched_has_T   = has_bound_T;
            matched_bound_T = bound_T;
        }
    }

    if (!matched && fixity == FIX_INFIX_LEFT && argc == 2 &&
        arg_types[0] > 0 && arg_types[0] < tab->type_count &&
        arg_types[1] > 0 && arg_types[1] < tab->type_count) {
        uint8_t eq_intr = 0;
        for (uint16_t i = first_ov; i != MEM_NO_IDX; i = tab->ov_next[i]) {
            const OpOverload *ov = &tab->overloads[i];
            if (ov->fixity == FIX_INFIX_LEFT && ov->param_count == 2 &&
                sym_mod_enabled(tab, ov->mod_id) && ov->body_ast != MEM_NO_IDX) {
                const AstNode *rn = mem_get_ast(mem, ov->body_ast);
                if (rn->kind == AST_INTRINSIC && (rn->sub == INTR_EQ || rn->sub == INTR_NE)) {
                    eq_intr = rn->sub;
                    break;
                }
            }
        }
        if (eq_intr != 0) {
            uint16_t tid0 = arg_types[0];
            uint16_t tid1 = arg_types[1];
            const TypeDef *td0 = &tab->types[tid0];
            const TypeDef *td1 = &tab->types[tid1];
            bool is_enum0 = (td0->is_consuming && td0->parser_intr == INTR_PARSE_I64);
            bool is_enum1 = (td1->is_consuming && td1->parser_intr == INTR_PARSE_I64);
            if ((is_enum0 || is_enum1) &&
                (tid0 == tid1 || tid0 == mem->fast_real_type || tid1 == mem->fast_real_type)) {
                bool eq = (arg_units[0] == arg_units[1]) &&
                          (fabs(val_get_f64(mem, args[0]) - val_get_f64(mem, args[1])) <= 1e-12);
                double r = (eq_intr == INTR_EQ) ? (eq ? 1.0 : 0.0) : (eq ? 0.0 : 1.0);
                return val_make_f64(mem, mem->fast_real_type, r);
            }
            uint16_t tid = tid0;
            const TypeDef *td = td0;
            if (tid0 == tid1 && !td->is_consuming && td->parser_intr == INTR_PARSE_I64 && td->field_count > 0) {
                uint16_t pa = val_get_pool_idx(args[0]);
                uint16_t pb = val_get_pool_idx(args[1]);
                int tag_a = (int)llround(mem_read_pool(mem, pa));
                int tag_b = (int)llround(mem_read_pool(mem, pb));
                bool eq = (arg_units[0] == arg_units[1]) && (tag_a == tag_b);
                if (eq) {
                    uint8_t i = 0;
                    while (i < td->field_count) {
                        uint16_t vfd_i = (uint16_t)(td->start_idx + i);
                        const FieldDef *vfd = &tab->fields[vfd_i];
                        if (vfd->type_id == tid) {
                            int ctag = (int)(vfd->flat_offset & 0xFFu);
                            uint8_t pcnt = (uint8_t)(vfd->flat_offset >> 8u);
                            if (ctag == tag_a) {
                                for (uint8_t pi = 0; eq && pi < pcnt; ++pi) {
                                    const FieldDef *pfd = &tab->fields[vfd_i + 1u + pi];
                                    uint16_t fa = (uint16_t)(pa + pfd->flat_offset);
                                    uint16_t fb = (uint16_t)(pb + pfd->flat_offset);
                                    if (pfd->type_id == mem->str_type_id && mem->str_type_id != 0) {
                                        uint32_t pka = (uint32_t)mem_read_pool(mem, fa);
                                        uint32_t pkb = (uint32_t)mem_read_pool(mem, fb);
                                        StrSlice sa = { (uint16_t)(pka >> 16u), (uint16_t)(pka & 0xFFFFu) };
                                        StrSlice sb = { (uint16_t)(pkb >> 16u), (uint16_t)(pkb & 0xFFFFu) };
                                        if (!mem_slice_eq(mem, sa, sb)) eq = false;
                                    } else {
                                        uint16_t fsz = (pfd->type_id < tab->type_count) ? tab->types[pfd->type_id].flat_size : 1u;
                                        for (uint16_t k = 0; k < fsz; ++k) {
                                            if (fabs(mem_read_pool(mem, (uint16_t)(fa + k)) -
                                                     mem_read_pool(mem, (uint16_t)(fb + k))) > 1e-12) {
                                                eq = false;
                                                break;
                                            }
                                        }
                                    }
                                }
                                break;
                            }
                            i = (uint8_t)(i + 1u + pcnt);
                        } else {
                            i++;
                        }
                    }
                }
                double r = (eq_intr == INTR_EQ) ? (eq ? 1.0 : 0.0) : (eq ? 0.0 : 1.0);
                return val_make_f64(mem, mem->fast_real_type, r);
            }
            if (tid0 == tid1 && !td->is_consuming && tid != mem->vec_type_id && tid != mem->mat_type_id &&
                tid != mem->map_type_id && tid != mem->fn_type_id) {
                uint16_t pa = val_get_pool_idx(args[0]);
                uint16_t pb = val_get_pool_idx(args[1]);
                bool eq = (arg_units[0] == arg_units[1]);
                for (uint8_t fi = 0; eq && fi < td->field_count; ++fi) {
                    const FieldDef *fd = &tab->fields[td->start_idx + fi];
                    uint16_t fa = (uint16_t)(pa + fd->flat_offset);
                    uint16_t fb = (uint16_t)(pb + fd->flat_offset);
                    if (fd->type_id == mem->str_type_id && mem->str_type_id != 0) {
                        uint32_t pka = (uint32_t)mem_read_pool(mem, fa);
                        uint32_t pkb = (uint32_t)mem_read_pool(mem, fb);
                        StrSlice sa = { (uint16_t)(pka >> 16u), (uint16_t)(pka & 0xFFFFu) };
                        StrSlice sb = { (uint16_t)(pkb >> 16u), (uint16_t)(pkb & 0xFFFFu) };
                        if (!mem_slice_eq(mem, sa, sb)) eq = false;
                    } else {
                        uint16_t fsz = (fd->type_id < tab->type_count) ? tab->types[fd->type_id].flat_size : 1u;
                        for (uint16_t k = 0; k < fsz; ++k) {
                            if (fabs(mem_read_pool(mem, (uint16_t)(fa + k)) -
                                     mem_read_pool(mem, (uint16_t)(fb + k))) > 1e-12) {
                                eq = false;
                                break;
                            }
                        }
                    }
                }
                double r = (eq_intr == INTR_EQ) ? (eq ? 1.0 : 0.0) : (eq ? 0.0 : 1.0);
                return val_make_f64(mem, mem->fast_real_type, r);
            }
        }
    }

    if (!matched) {
        StrSlice s = tab->syms[sym_id];
        if (!type_only_matched && priv_mod != MEM_NO_IDX && priv_mod < tab->mod_count) {
            StrSlice ms = sym_mod_name(tab, priv_mod);
            mem_err(mem, "Function or operator '%.*s' is private to module '[%.*s]'",
                    (int)s.len, mem_str_ptr(mem, s),
                    (int)ms.len, mem_str_ptr(mem, ms));
            return val_void();
        }
        char t1[32] = "", t2[32] = "", u1[32] = "", u2[32] = "";
        if (argc >= 1) {
            sym_type_name(tab, mem, arg_types[0], t1, sizeof(t1));
            sym_unit_name(tab, mem, arg_units[0], u1, sizeof(u1));
        }
        if (argc >= 2) {
            sym_type_name(tab, mem, arg_types[1], t2, sizeof(t2));
            sym_unit_name(tab, mem, arg_units[1], u2, sizeof(u2));
        }
        if (type_only_matched) {
            if (argc == 2) {
                mem_err(mem, "Unit mismatch for '%.*s' with (%s [%s], %s [%s])",
                        (int)s.len, mem_str_ptr(mem, s), t1, u1, t2, u2);
            } else {
                mem_err(mem, "Unit mismatch for '%.*s' with (%s [%s])",
                        (int)s.len, mem_str_ptr(mem, s), t1, u1);
            }
        } else {
            if (argc == 2) {
                mem_err(mem, "No active overload for '%.*s' with types (%s, %s)",
                        (int)s.len, mem_str_ptr(mem, s), t1, t2);
            } else {
                mem_err(mem, "No active overload for '%.*s' with type (%s)",
                        (int)s.len, mem_str_ptr(mem, s), t1);
            }
        }
        return val_void();
    }

    bool is_rel_op      = false;
    bool has_common_u   = false;
    uint8_t common_unit = MEM_UNIT_NONE;
    if (argc == 2 &&
        (matched->param_units[0] == SYM_UNIT_OMITTED || matched->param_units[0] == SYM_UNIT_GENERIC || matched->param_units[0] == 0) &&
        (matched->param_units[1] == SYM_UNIT_OMITTED || matched->param_units[1] == SYM_UNIT_GENERIC || matched->param_units[1] == 0) &&
        hardcode_is_additive_or_rel_sym(mem, tab, sym_id, fixity, &is_rel_op)) {
        StrSlice s = tab->syms[sym_id];
        if (!delegate_check_additive_unit_compat(mem, tab, mem_str_ptr(mem, s), (int)s.len,
                                                 args[0], args[1], &common_unit)) {
            return val_void();
        }
        has_common_u = !is_rel_op;
    }

    uint16_t frame_base = mem_push_frame(mem, matched->local_count);
    if (mem->has_error) return val_void();

    for (uint8_t i = 0; i < matched->local_count; ++i) {
        if (i < argc) {
            uint16_t tid = val_get_type(mem, args[i]);
            if (tid != 0 && tid < tab->type_count &&
                !tab->types[tid].is_consuming &&
                tid != mem->vec_type_id && tid != mem->mat_type_id && tid != mem->fn_type_id && tid != mem->map_type_id) {
                mem_set_slot(mem, frame_base, i, val_clone_comp(mem, args[i], tab->types[tid].flat_size));
            } else {
                mem_set_slot(mem, frame_base, i, args[i]);
            }
            if (matched->param_types[i] != SYM_TYPE_ANY && matched->param_types[i] != 0) {
                mem->slot_locked_type[frame_base + i] = matched->param_types[i];
            }
            if (matched->param_units[i] != SYM_UNIT_OMITTED &&
                matched->param_units[i] != SYM_UNIT_GENERIC &&
                matched->param_units[i] != 0) {
                mem->slot_locked_unit[frame_base + i] = matched->param_units[i];
            }
        } else {
            mem_set_slot(mem, frame_base, i, val_make_f64(mem, mem->fast_real_type, 0.0));
        }
    }

    uint16_t saved_labels[MEM_MAX_LABELS];
    memcpy(saved_labels, mem->label_ast, sizeof(saved_labels));
    for (uint8_t i = 0; i < MEM_MAX_LABELS; ++i) {
        mem->label_ast[i] = MEM_NO_IDX;
    }
    scan_all_labels_in_tree(mem, matched->body_ast);

    uint16_t saved_mod     = mem->cur_mod;
    uint16_t saved_tab_mod = tab->cur_mod;
    mem->cur_mod = matched->mod_id;
    tab->cur_mod = matched->mod_id;

    uint16_t ov_idx = (uint16_t)(matched - tab->overloads);
    if (ov_idx < 2048u && mem->ov_bc_entry[ov_idx] == 0) {
        bool is_perm = (matched->body_ast < mem->perm_ast_count);
        uint16_t bc_pc = bc_compile_ast(mem, tab, matched->body_ast, is_perm);
        mem->ov_bc_entry[ov_idx] = (bc_pc != MEM_NO_IDX) ? (uint16_t)(bc_pc + 1u) : 0xFFFFu;
    }
    Value result;
    if (ov_idx < 2048u && mem->ov_bc_entry[ov_idx] > 0 && mem->ov_bc_entry[ov_idx] != 0xFFFFu) {
        result = bc_exec(mem, tab, (uint16_t)(mem->ov_bc_entry[ov_idx] - 1u), frame_base, matched->ret_type);
    } else {
        result = eval_ast(mem, tab, matched->body_ast, frame_base, matched->ret_type);
    }
    if (mem->jump_active == JUMP_RETURN) {
        result = mem->jump_val;
        mem->jump_active = JUMP_NONE;
    } else if (mem->jump_active == JUMP_THROW) {
        /* Preserve JUMP_THROW and jump_val for enclosing try/catch block */
    } else if (mem->jump_active != JUMP_NONE && !mem->has_error) {
        mem->jump_active = JUMP_NONE;
        mem_err(mem, "Unhandled jump, break, or continue outside active loop/scope");
    }

    mem->cur_mod = saved_mod;
    tab->cur_mod = saved_tab_mod;

    memcpy(mem->label_ast, saved_labels, sizeof(saved_labels));
    mem_pop_frame(mem, frame_base);
    if (mem->has_error) return val_void();
    if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
        mem->heap_active_count >= 64u) {
        (void)mem_compact_scratch(mem, &result, 1);
    }

    if (matched->ret_unit == SYM_UNIT_LOCK_NONE) {
        result = val_with_unit(mem, result, MEM_UNIT_NONE);
    } else if (matched->ret_unit == SYM_UNIT_GENERIC) {
        if (matched_has_T) {
            result = val_with_unit(mem, result, matched_bound_T);
        } else if (has_common_u) {
            result = val_with_unit(mem, result, common_unit);
        }
    } else if (matched->ret_unit != SYM_UNIT_OMITTED && matched->ret_unit != 0) {
        result = val_with_unit(mem, result, matched->ret_unit);
    } else if (has_common_u && val_get_unit(mem, result) == MEM_UNIT_NONE) {
        result = val_with_unit(mem, result, common_unit);
    }

    return result;
}

/* ============================================================================
 * 3. SUBSCRIPT INDEXING & SLICING (`v[i]`, `v[1..3]`, `m[r, c]`, `m[0..1, 1..2]`)
 * ============================================================================ */

Value eval_dispatch_overload(MemArena *mem, SymTable *tab,
                               uint16_t sym_id, OpFixity fixity,
                               uint8_t argc, const Value *args) {
    return eval_dispatch_overload_in_mod(mem, tab, sym_id, fixity, argc, args, MEM_NO_IDX);
}

static Value eval_index_op(MemArena *mem, SymTable *tab,
                           const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type,
                           bool is_write, Value rhs) {
    Value base = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    if (is_write && mem_val_is_immutable(mem, base)) {
        mem_err(mem, "Cannot mutate immutable constant container");
        return val_void();
    }

    uint8_t argc = n->sub;
    Value   iv_args[4] = { 0 };
    bool    has_slice_arg = false;
    uint16_t cur = n->right;
    for (uint8_t i = 0; i < argc && cur != MEM_NO_IDX; ++i) {
        const AstNode *link = mem_get_ast(mem, cur);
        iv_args[i] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
        uint16_t itid = val_get_type(mem, iv_args[i]);
        if ((itid == mem->vec_type_id && mem->vec_type_id != 0) ||
            (itid == mem->mat_type_id && mem->mat_type_id != 0)) {
            has_slice_arg = true;
        }
        cur = link->right;
    }

    uint16_t base_tid  = val_get_type(mem, base);
    uint8_t  base_unit = val_get_unit(mem, base);

    /* A0. String (`$str`) Indexing & Substring Slicing -> Delegate to `@vget` (`INTR_VGET`) */
    if (base_tid == mem->str_type_id && mem->str_type_id != 0) {
        if (is_write) {
            mem_err(mem, "Strings are immutable and do not support index assignment");
            return val_void();
        }
        if (argc != 1) {
            mem_err(mem, "String subscript expects a single index or range slice");
            return val_void();
        }
        if (has_slice_arg) {
            int at_sid = sym_find(tab, mem, "at", 2);
            if (at_sid >= 0) {
                Value at_args[2] = { base, iv_args[0] };
                return eval_dispatch_overload(mem, tab, (uint16_t)at_sid, FIX_FUNC, 2, at_args);
            }
        }
        return delegate_eval_vec_intrinsic(mem, tab, INTR_VGET, base, iv_args[0], val_void());
    }

    /* 0b. Dynamic Map (`$map`) Subscript (`m["key"]`, `m["key"] = val`, `m["key"] += val`) */
    if (base_tid == mem->map_type_id && mem->map_type_id != 0) {
        if (argc != 1) {
            mem_err(mem, "Map subscript expects a single key");
            return val_void();
        }
        if (!is_write) {
            return delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_GET, base, iv_args[0], val_void());
        } else {
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_SET, base, iv_args[0], rhs);
            return rhs;
        }
    }

    /* A. Dynamic Vector (`$vec`) Slicing (`v[1..3]`, `v[1...3]`, `v[vec(0, 2)]`) */
    if (has_slice_arg && base_tid == mem->vec_type_id && mem->vec_type_id != 0 && argc == 1) {
        uint16_t hdr_idx  = val_get_pool_idx(base);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1;

        uint16_t slice_idx[128];
        uint16_t slice_cnt = 0;
        if (!delegate_extract_slice_indices(mem, tab, iv_args[0], (int)len, slice_idx, &slice_cnt, 128)) {
            return val_void();
        }

        if (!is_write) {
            uint16_t out_hdr = mem_vec_alloc_hdr(mem, elem_tid, slice_cnt);
            if (mem->has_error) return val_void();
            mem_write_pool(mem, out_hdr + 0, (double)(raw_hdr0 & (uint16_t)~MEM_HDR_IMMUTABLE_BIT));
            if (slice_cnt > 0) {
                if (!mem_vec_reserve(mem, out_hdr, stride, slice_cnt)) return val_void();
                uint16_t out_data = (uint16_t)mem_read_pool(mem, out_hdr + 3);
                for (uint16_t i = 0; i < slice_cnt; ++i) {
                    Value item = delegate_vec_read_elem(mem, tab, elem_tid, elem_uid,
                                                        data_idx, (uint32_t)slice_idx[i] * (uint32_t)stride);
                    delegate_vec_write_elem(mem, tab, elem_tid, out_data, (uint32_t)i * (uint32_t)stride, item);
                }
                mem_write_pool(mem, out_hdr + 1, (double)slice_cnt);
            }
            return val_make_comp(mem, mem->vec_type_id, out_hdr, MEM_VEC_HDR_SLOTS);
        } else {
            uint16_t rhs_tid = val_get_type(mem, rhs);
            if (rhs_tid == mem->vec_type_id && mem->vec_type_id != 0) {
                uint16_t rhdr = val_get_pool_idx(rhs);
                uint16_t rraw = (uint16_t)mem_read_pool(mem, rhdr + 0);
                uint16_t rtid = (uint16_t)(rraw & 0xFFu);
                uint8_t  ruid = (uint8_t)((rraw >> 8) & 0x7Fu);
                uint32_t rlen = (uint32_t)mem_read_pool(mem, rhdr + 1);
                uint16_t rdat = (uint16_t)mem_read_pool(mem, rhdr + 3);
                if (rlen != slice_cnt) {
                    mem_err(mem, "Vector slice assignment length mismatch (%u vs %u)",
                            (unsigned)slice_cnt, (unsigned)rlen);
                    return val_void();
                }
                if (elem_tid != 0 && (elem_tid != rtid || elem_uid != ruid)) {
                    char exp_t[32], got_t[32];
                    sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                    sym_type_name(tab, mem, rtid, got_t, sizeof(got_t));
                    mem_err(mem, "Vector type mismatch: cannot set %s in vec<%s>", got_t, exp_t);
                    return val_void();
                }
                for (uint16_t i = 0; i < slice_cnt; ++i) {
                    Value item = delegate_vec_read_elem(mem, tab, rtid, ruid, rdat, (uint32_t)i * (uint32_t)stride);
                    delegate_vec_write_elem(mem, tab, elem_tid,
                                            data_idx, (uint32_t)slice_idx[i] * (uint32_t)stride, item);
                }
            } else {
                uint8_t rhs_uid = val_get_unit(mem, rhs);
                if (elem_tid != 0 && (elem_tid != rhs_tid || elem_uid != rhs_uid)) {
                    char exp_t[32], got_t[32];
                    sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                    sym_type_name(tab, mem, rhs_tid, got_t, sizeof(got_t));
                    mem_err(mem, "Vector type mismatch: cannot set %s in vec<%s>", got_t, exp_t);
                    return val_void();
                }
                for (uint16_t i = 0; i < slice_cnt; ++i) {
                    delegate_vec_write_elem(mem, tab, elem_tid,
                                            data_idx, (uint32_t)slice_idx[i] * (uint32_t)stride, rhs);
                }
            }
            return rhs;
        }
    }

    /* B. Matrix (`$mat`) Slicing (`M[0..1, 1..2]`, `M[0..1]`, `M[1, 0..2]`) */
    if (has_slice_arg && base_tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, base, &rank, &tot_len, &data_idx, dims);
        if (argc == 0 || argc > rank) {
            mem_err(mem, "Matrix subscript has %u indices, but matrix rank is %u",
                    (unsigned)argc, (unsigned)rank);
            return val_void();
        }

        uint16_t ax_idx[4][64];
        uint16_t ax_cnt[4]     = { 1, 1, 1, 1 };
        bool     ax_is_slc[4]  = { false, false, false, false };
        uint16_t ax_stride[4]  = { 1, 1, 1, 1 };

        for (int a = (int)rank - 2; a >= 0; --a) {
            ax_stride[a] = (uint16_t)(ax_stride[a + 1] * dims[a + 1]);
        }

        for (uint8_t a = 0; a < rank; ++a) {
            if (a < argc) {
                uint16_t itid = val_get_type(mem, iv_args[a]);
                if ((itid == mem->vec_type_id && mem->vec_type_id != 0) ||
                    (itid == mem->mat_type_id && mem->mat_type_id != 0)) {
                    if (!delegate_extract_slice_indices(mem, tab, iv_args[a], (int)dims[a],
                                               ax_idx[a], &ax_cnt[a], 64)) {
                        return val_void();
                    }
                    ax_is_slc[a] = true;
                } else {
                    int k = 0;
                    if (!delegate_val_to_exact_int(mem, tab, iv_args[a], "subscript index", &k)) {
                        return val_void();
                    }
                    if (k < 0) k += (int)dims[a];
                    if (k < 0 || k >= (int)dims[a]) {
                        mem_err(mem, "Matrix index %d out of bounds on axis %u (size=%u)",
                                k, (unsigned)a, (unsigned)dims[a]);
                        return val_void();
                    }
                    ax_idx[a][0] = (uint16_t)k;
                    ax_cnt[a]    = 1;
                    ax_is_slc[a] = false;
                }
            } else {
                ax_cnt[a]    = dims[a];
                ax_is_slc[a] = true;
                for (uint16_t k = 0; k < dims[a] && k < 64; ++k) {
                    ax_idx[a][k] = k;
                }
            }
        }

        uint16_t out_dims[4] = { 0, 0, 0, 0 };
        uint8_t  out_rank    = 0;
        for (uint8_t a = 0; a < rank; ++a) {
            if (ax_is_slc[a]) {
                out_dims[out_rank++] = ax_cnt[a];
            }
        }
        if (out_rank == 0) {
            out_rank    = 1;
            out_dims[0] = 1;
        }

        if (!is_write) {
            uint16_t out_tot = 0, out_data = 0;
            uint16_t out_hdr = mem_mat_alloc_hdr(mem, out_rank, out_dims, &out_tot, &out_data);
            if (mem->has_error) return val_void();

            uint16_t dst = 0;
            for (uint16_t i0 = 0; i0 < ax_cnt[0]; ++i0) {
                uint16_t off0 = (uint16_t)(ax_idx[0][i0] * ax_stride[0]);
                for (uint16_t i1 = 0; i1 < (rank > 1 ? ax_cnt[1] : 1); ++i1) {
                    uint16_t off1 = (uint16_t)(off0 + (rank > 1 ? ax_idx[1][i1] * ax_stride[1] : 0));
                    for (uint16_t i2 = 0; i2 < (rank > 2 ? ax_cnt[2] : 1); ++i2) {
                        uint16_t off2 = (uint16_t)(off1 + (rank > 2 ? ax_idx[2][i2] * ax_stride[2] : 0));
                        for (uint16_t i3 = 0; i3 < (rank > 3 ? ax_cnt[3] : 1); ++i3) {
                            uint16_t src_off = (uint16_t)(off2 + (rank > 3 ? ax_idx[3][i3] : 0));
                            mem_data_write(mem, out_data, dst++,
                                           mem_data_read(mem, data_idx, src_off));
                        }
                    }
                }
            }
            return val_with_unit(mem, val_make_comp(mem, mem->mat_type_id, out_hdr, MEM_MAT_HDR_SLOTS), base_unit);
        } else {
            double fill_val = val_get_f64(mem, rhs);
            for (uint16_t i0 = 0; i0 < ax_cnt[0]; ++i0) {
                uint16_t off0 = (uint16_t)(ax_idx[0][i0] * ax_stride[0]);
                for (uint16_t i1 = 0; i1 < (rank > 1 ? ax_cnt[1] : 1); ++i1) {
                    uint16_t off1 = (uint16_t)(off0 + (rank > 1 ? ax_idx[1][i1] * ax_stride[1] : 0));
                    for (uint16_t i2 = 0; i2 < (rank > 2 ? ax_cnt[2] : 1); ++i2) {
                        uint16_t off2 = (uint16_t)(off1 + (rank > 2 ? ax_idx[2][i2] * ax_stride[2] : 0));
                        for (uint16_t i3 = 0; i3 < (rank > 3 ? ax_cnt[3] : 1); ++i3) {
                            uint16_t dst_off = (uint16_t)(off2 + (rank > 3 ? ax_idx[3][i3] : 0));
                            mem_data_write(mem, data_idx, dst_off, fill_val);
                        }
                    }
                }
            }
            return rhs;
        }
    }

    /* C. Standard Scalar Subscript Indexing */
    int idx_k[4] = { 0, 0, 0, 0 };
    for (uint8_t i = 0; i < argc; ++i) {
        if (!delegate_val_to_exact_int(mem, tab, iv_args[i], "subscript index", &idx_k[i])) return val_void();
    }

    if (base_tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, base, &rank, &tot_len, &data_idx, dims);

        if (argc == 0 || argc > rank) {
            mem_err(mem, "Matrix subscript has %u indices, but matrix rank is %u",
                    (unsigned)argc, (unsigned)rank);
            return val_void();
        }

        uint32_t prefix_off = 0;
        for (uint8_t a = 0; a < argc; ++a) {
            int dim_a = (int)dims[a];
            int ka    = idx_k[a];
            if (ka < 0) ka += dim_a;
            if (ka < 0 || ka >= dim_a) {
                mem_err(mem, "Matrix index %d out of bounds on axis %u (size=%d)",
                        idx_k[a], (unsigned)a, dim_a);
                return val_void();
            }
            prefix_off = prefix_off * (uint32_t)dim_a + (uint32_t)ka;
        }

        uint32_t sub_len = 1;
        for (uint8_t a = argc; a < rank; ++a) {
            sub_len *= (uint32_t)dims[a];
        }
        uint32_t elem_off = prefix_off * sub_len;

        if (argc == rank) {
            if (!is_write) {
                return val_make_f64_unit(mem, mem->fast_real_type, base_unit,
                                         mem_data_read(mem, data_idx, elem_off));
            } else {
                mem_data_write(mem, data_idx, elem_off, val_get_f64(mem, rhs));
                return rhs;
            }
        } else {
            uint8_t sub_rank = (uint8_t)(rank - argc);
            if (!is_write) {
                uint16_t sub_dims[4] = { 0, 0, 0, 0 };
                for (uint8_t i = 0; i < sub_rank && i < 4; ++i) {
                    sub_dims[i] = dims[argc + i];
                }
                if (mem_is_heap_idx(data_idx)) {
                    uint16_t sub_tot = 0, sub_dat = 0;
                    uint16_t view_hdr = mem_mat_alloc_hdr(mem, sub_rank, sub_dims, &sub_tot, &sub_dat);
                    if (mem->has_error) return val_void();
                    for (uint32_t i = 0; i < sub_len; ++i) {
                        mem_data_write(mem, sub_dat, i, mem_data_read(mem, data_idx, elem_off + i));
                    }
                    return val_with_unit(mem,
                                         val_make_comp(mem, mem->mat_type_id, view_hdr, MEM_MAT_HDR_SLOTS),
                                         base_unit);
                }
                uint16_t target_data_idx = (uint16_t)(data_idx + elem_off);
                uint16_t view_hdr = mem_alloc_pool(mem, MEM_MAT_HDR_SLOTS);
                if (mem->has_error) return val_void();
                mem_write_pool(mem, view_hdr + 0, (double)sub_rank);
                mem_write_pool(mem, view_hdr + 1, (double)sub_len);
                mem_write_pool(mem, view_hdr + 2, (double)target_data_idx);
                mem_write_pool(mem, view_hdr + 3, (double)mem->fast_real_type);
                for (uint8_t i = 0; i < 4; ++i) {
                    mem_write_pool(mem, (uint16_t)(view_hdr + 4u + i),
                                   (i < sub_rank) ? (double)dims[argc + i] : 0.0);
                }
                return val_with_unit(mem,
                                     val_make_comp(mem, mem->mat_type_id, view_hdr, MEM_MAT_HDR_SLOTS),
                                     base_unit);
            } else {
                uint8_t  rhs_rank = 0;
                uint16_t rhs_tot = 0, rhs_dat = 0, rhs_dims[4] = { 0 };
                if (!delegate_mat_get_info(mem, rhs, &rhs_rank, &rhs_tot, &rhs_dat, rhs_dims) ||
                    rhs_rank != sub_rank || rhs_tot != sub_len) {
                    mem_err(mem, "Sub-matrix assignment shape mismatch");
                    return val_void();
                }
                for (uint32_t i = 0; i < sub_len; ++i) {
                    mem_data_write(mem, data_idx, elem_off + i, mem_data_read(mem, rhs_dat, i));
                }
                return rhs;
            }
        }
    }

    if (base_tid == mem->vec_type_id && mem->vec_type_id != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(base);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1;

        int k = idx_k[0];
        if (k < 0) k += (int)len;
        if (k < 0 || k >= (int)len) {
            mem_err(mem, "Vector index %d out of bounds (len=%u)", idx_k[0], (unsigned)len);
            return val_void();
        }
        uint32_t elem_off  = (uint32_t)k * (uint32_t)stride;
        uint16_t elem_pidx = (uint16_t)(data_idx + elem_off);

        if (argc == 1) {
            if (!is_write) {
                return delegate_vec_read_elem(mem, tab, elem_tid, elem_uid, data_idx, elem_off);
            } else {
                uint16_t item_tid = val_get_type(mem, rhs);
                uint8_t  item_uid = val_get_unit(mem, rhs);
                if (elem_tid != 0 && elem_tid != SYM_TYPE_ANY && (elem_tid != item_tid || elem_uid != item_uid)) {
                    char exp_t[32], got_t[32];
                    sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                    sym_type_name(tab, mem, item_tid, got_t, sizeof(got_t));
                    mem_err(mem, "Vector type mismatch: cannot set '%s' in vec<%s>", got_t, exp_t);
                    return val_void();
                }
                delegate_vec_write_elem(mem, tab, elem_tid, data_idx, elem_off, rhs);
                return rhs;
            }
        }
        base_tid  = elem_tid;
        base_unit = elem_uid;
        uint16_t cur_pidx = elem_pidx;
        uint16_t cur_tid  = base_tid;
        for (uint8_t a = 1; a < argc; ++a) {
            if (cur_tid == 0 || cur_tid >= tab->type_count ||
                tab->types[cur_tid].is_consuming || tab->types[cur_tid].field_count == 0) {
                mem_err(mem, "Cannot subscript scalar vector element");
                return val_void();
            }
            const TypeDef *td = &tab->types[cur_tid];
            int fk = idx_k[a];
            if (fk < 0) fk += (int)td->field_count;
            if (fk < 0 || fk >= (int)td->field_count) {
                mem_err(mem, "Struct/vector field index %d out of bounds (fields=%u)",
                        idx_k[a], (unsigned)td->field_count);
                return val_void();
            }
            const FieldDef *fd = &tab->fields[td->start_idx + (uint16_t)fk];
            cur_pidx = (uint16_t)(cur_pidx + fd->flat_offset);
            cur_tid  = fd->type_id;
        }
        if (!is_write) {
            if (!tab->types[cur_tid].is_consuming) {
                return val_with_unit(mem, val_make_comp(mem, cur_tid, cur_pidx, tab->types[cur_tid].flat_size),
                                     base_unit);
            }
            return val_make_f64_unit(mem, cur_tid ? cur_tid : mem->fast_real_type, base_unit,
                                     mem_read_pool(mem, cur_pidx));
        } else {
            mem_write_pool(mem, cur_pidx, val_get_f64(mem, rhs));
            return rhs;
        }
    }

    if (base_tid > 0 && base_tid < tab->type_count && !tab->types[base_tid].is_consuming) {
        uint16_t cur_pidx = val_get_pool_idx(base);
        uint16_t cur_tid  = base_tid;
        for (uint8_t a = 0; a < argc; ++a) {
            if (cur_tid == 0 || cur_tid >= tab->type_count ||
                tab->types[cur_tid].is_consuming || tab->types[cur_tid].field_count == 0) {
                mem_err(mem, "Cannot subscript scalar field with '[...]'");
                return val_void();
            }
            const TypeDef *td = &tab->types[cur_tid];
            int fk = idx_k[a];
            if (fk < 0) fk += (int)td->field_count;
            if (fk < 0 || fk >= (int)td->field_count) {
                mem_err(mem, "Vector/struct index %d out of bounds (fields=%u)",
                        idx_k[a], (unsigned)td->field_count);
                return val_void();
            }
            const FieldDef *fd = &tab->fields[td->start_idx + (uint16_t)fk];
            cur_pidx = (uint16_t)(cur_pidx + fd->flat_offset);
            cur_tid  = fd->type_id;
        }

        if (!is_write) {
            if (cur_tid != 0 && cur_tid < tab->type_count && !tab->types[cur_tid].is_consuming) {
                return val_with_unit(mem,
                                     val_make_comp(mem, cur_tid, cur_pidx, tab->types[cur_tid].flat_size),
                                     base_unit);
            }
            if (cur_tid == mem->str_type_id && mem->str_type_id != 0) {
                uint32_t packed = (uint32_t)mem_read_pool(mem, cur_pidx);
                StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
                return val_make_str(mem, cur_tid, s);
            }
            return val_make_f64_unit(mem, cur_tid ? cur_tid : mem->fast_real_type, base_unit,
                                     mem_read_pool(mem, cur_pidx));
        } else {
            if (cur_tid != 0 && cur_tid < tab->type_count && !tab->types[cur_tid].is_consuming) {
                uint16_t fsize   = tab->types[cur_tid].flat_size;
                uint16_t src_idx = val_get_pool_idx(rhs);
                for (uint16_t i = 0; i < fsize; ++i) {
                    mem_write_pool(mem, cur_pidx + i, mem_read_pool(mem, src_idx + i));
                }
            } else if (cur_tid == mem->str_type_id && mem->str_type_id != 0) {
                StrSlice s = val_get_str(mem, rhs);
                mem_write_pool(mem, cur_pidx, (double)(((uint32_t)s.off << 16u) | (uint32_t)s.len));
            } else {
                mem_write_pool(mem, cur_pidx, val_get_f64(mem, rhs));
            }
            return rhs;
        }
    }

    mem_err(mem, "Cannot subscript non-indexable type with '[...]'");
    return val_void();
}

static Value eval_mat_lit_ast(MemArena *mem, SymTable *tab,
                              const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type) {
    if (mem->mat_type_id == 0) {
        mem_err(mem, "Multidimensional matrix type ($mat) is not loaded");
        return val_void();
    }
    uint8_t count = n->sub;
    if (count == 0) {
        uint16_t hdr_idx = mem_alloc_pool(mem, MEM_MAT_HDR_SLOTS);
        if (mem->has_error) return val_void();
        for (uint8_t i = 0; i < MEM_MAT_HDR_SLOTS; ++i) mem_write_pool(mem, hdr_idx + i, 0.0);
        mem_write_pool(mem, hdr_idx + 0, 1.0);
        return val_make_comp(mem, mem->mat_type_id, hdr_idx, MEM_MAT_HDR_SLOTS);
    }

    Value items[64];
    memset(items, 0, sizeof(items));
    uint8_t actual = 0;
    uint16_t cur = n->left;
    while (cur != MEM_NO_IDX && actual < 64) {
        const AstNode *link = mem_get_ast(mem, cur);
        items[actual] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
        actual++;
        cur = link->right;
    }
    if (actual == 0) return val_void();

    uint16_t first_tid = val_get_type(mem, items[0]);
    uint8_t  common_u  = val_get_unit(mem, items[0]);

    if (first_tid == mem->mat_type_id) {
        uint8_t  sub_rank = 0;
        uint16_t sub_tot = 0, sub_dat0 = 0, sub_dims[4] = { 0 };
        delegate_mat_get_info(mem, items[0], &sub_rank, &sub_tot, &sub_dat0, sub_dims);
        if (sub_rank >= 4) {
            mem_err(mem, "Maximum matrix rank (4D) exceeded");
            return val_void();
        }
        for (uint8_t i = 1; i < actual; ++i) {
            uint8_t  r_i = 0;
            uint16_t t_i = 0, d_i = 0, dims_i[4] = { 0 };
            if (!delegate_mat_get_info(mem, items[i], &r_i, &t_i, &d_i, dims_i) || r_i != sub_rank) {
                mem_err(mem, "Matrix rows/sub-tensors must have matching dimensions");
                return val_void();
            }
            for (uint8_t k = 0; k < sub_rank; ++k) {
                if (dims_i[k] != sub_dims[k]) {
                    mem_err(mem, "Matrix rows/sub-tensors must have matching dimensions");
                    return val_void();
                }
            }
        }
        uint16_t dims[4] = { actual, 0, 0, 0 };
        for (uint8_t k = 0; k < sub_rank && k < 3; ++k) {
            dims[k + 1] = sub_dims[k];
        }
        uint16_t tot_len = 0, data_idx = 0;
        uint16_t hdr_idx = mem_mat_alloc_hdr(mem, (uint8_t)(sub_rank + 1), dims, &tot_len, &data_idx);
        if (mem->has_error) return val_void();

        for (uint8_t i = 0; i < actual; ++i) {
            uint16_t d_i = 0;
            uint16_t dummy_dims[4];
            delegate_mat_get_info(mem, items[i], NULL, NULL, &d_i, dummy_dims);
            for (uint16_t k = 0; k < sub_tot; ++k) {
                mem_data_write(mem, data_idx, (uint32_t)i * (uint32_t)sub_tot + k,
                               mem_data_read(mem, d_i, k));
            }
        }
        return val_with_unit(mem, val_make_comp(mem, mem->mat_type_id, hdr_idx, MEM_MAT_HDR_SLOTS), common_u);
    }

    bool has_non_num = false;
    bool mixed_types = false;
    for (uint8_t i = 0; i < actual; ++i) {
        uint16_t tid = val_get_type(mem, items[i]);
        if (tid != first_tid) mixed_types = true;
        if (tid == 0 || tid >= tab->type_count || !tab->types[tid].is_consuming || tid == mem->str_type_id) {
            has_non_num = true;
        }
    }
    if (has_non_num) {
        if (mem->vec_type_id == 0) {
            mem_err(mem, "Matrix elements must be numeric scalars or sub-matrices");
            return val_void();
        }
        uint16_t elem_tid = mixed_types ? (uint16_t)SYM_TYPE_ANY : first_tid;
        uint16_t stride   = (elem_tid != SYM_TYPE_ANY && elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1u;
        uint16_t saved_tr = mem->temp_root_count;
        for (uint8_t i = 0; i < actual; ++i) {
            (void)mem_push_temp_root(mem, &items[i]);
        }
        uint16_t hdr_idx = mem_vec_alloc_hdr(mem, elem_tid, actual);
        if (mem->has_error || !mem_vec_reserve(mem, hdr_idx, stride, actual)) {
            mem_pop_temp_roots(mem, saved_tr);
            return val_void();
        }
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
        for (uint8_t i = 0; i < actual; ++i) {
            delegate_vec_write_elem(mem, tab, elem_tid, data_idx, (uint32_t)i * (uint32_t)stride, items[i]);
        }
        mem_write_pool(mem, (uint16_t)(hdr_idx + 1), (double)actual);
        mem_pop_temp_roots(mem, saved_tr);
        return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    }
    uint16_t dims[4] = { actual, 0, 0, 0 };
    uint16_t tot_len = 0, data_idx = 0;
    uint16_t hdr_idx = mem_mat_alloc_hdr(mem, 1, dims, &tot_len, &data_idx);
    if (mem->has_error) return val_void();
    for (uint8_t i = 0; i < actual; ++i) {
        mem_data_write(mem, data_idx, i, val_get_f64(mem, items[i]));
    }
    return val_with_unit(mem, val_make_comp(mem, mem->mat_type_id, hdr_idx, MEM_MAT_HDR_SLOTS), common_u);
}

/* ============================================================================
 * 3b. SANDBOXED FILE I/O & RFC 8259 JSON PARSER / SERIALIZER
 * ============================================================================ */

static bool eval_resolve_io_path(MemArena *mem, Value path_val, char *out_buf, size_t cap) {
    char raw[220];
    if (!val_as_cstr(mem, path_val, raw, sizeof(raw)) || raw[0] == '\0') {
        mem_err(mem, "File path must be a non-empty string");
        return false;
    }
    if (raw[0] != '/' && mem->import_dir[0] != '\0') {
        snprintf(out_buf, cap, "%s/%s", mem->import_dir, raw);
    } else {
        snprintf(out_buf, cap, "%s", raw);
    }
    return true;
}

static Value eval_file_io_op(MemArena *mem, SymTable *tab, uint8_t sub_op, Value a, Value b) {
    char path[512];
    if (!eval_resolve_io_path(mem, a, path, sizeof(path))) return val_void();

    if (sub_op == 241u) { /* @file_exists(path) */
        FILE *fp = fopen(path, "rb");
        if (fp) {
            fclose(fp);
            return val_make_f64(mem, mem->fast_real_type, 1.0);
        }
        return val_make_f64(mem, mem->fast_real_type, 0.0);
    }

    if (sub_op == 238u) { /* @file_read(path) */
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            mem_err(mem, "Cannot open file '%s' for reading", path);
            return val_void();
        }
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            mem_err(mem, "Failed to seek file '%s'", path);
            return val_void();
        }
        long sz = ftell(fp);
        if (sz < 0 || sz > 28000L) {
            fclose(fp);
            mem_err(mem, "File '%s' size (%ld bytes) exceeds string pool limit", path, sz);
            return val_void();
        }
        rewind(fp);
        char *buf = (char *)malloc((size_t)sz + 1u);
        if (!buf) {
            fclose(fp);
            mem_err(mem, "Out of memory reading file '%s'", path);
            return val_void();
        }
        size_t n = fread(buf, 1, (size_t)sz, fp);
        fclose(fp);
        buf[n] = '\0';
        StrSlice s = mem_intern_str(mem, buf, (uint16_t)n);
        free(buf);
        if (mem->has_error) return val_void();
        return val_make_str(mem, mem->str_type_id, s);
    }

    if (sub_op == 239u || sub_op == 240u) { /* @file_write / @file_append */
        const char *mode = (sub_op == 240u) ? "ab" : "wb";
        FILE *fp = fopen(path, mode);
        if (!fp) {
            mem_err(mem, "Cannot open file '%s' for writing", path);
            return val_void();
        }
        const char *data_ptr = NULL;
        size_t data_len = 0;
        char fmt_buf[1024];
        if (mem->str_type_id != 0 && val_get_type(mem, b) == mem->str_type_id) {
            StrSlice s = val_get_str(mem, b);
            data_ptr = mem_str_ptr(mem, s);
            data_len = s.len;
        } else {
            Value sv = delegate_eval_intrinsic(mem, tab, INTR_TOSTR, b, val_void(), val_void(), 0);
            if (mem->has_error) { fclose(fp); return val_void(); }
            StrSlice s = val_get_str(mem, sv);
            size_t clen = (s.len < sizeof(fmt_buf) - 1u) ? s.len : (sizeof(fmt_buf) - 1u);
            memcpy(fmt_buf, mem_str_ptr(mem, s), clen);
            fmt_buf[clen] = '\0';
            data_ptr = fmt_buf;
            data_len = clen;
        }
        size_t written = (data_len > 0) ? fwrite(data_ptr, 1, data_len, fp) : 0u;
        fclose(fp);
        if (written != data_len) {
            mem_err(mem, "Incomplete write to file '%s'", path);
            return val_void();
        }
        return val_make_f64(mem, mem->fast_real_type, (double)written);
    }
    return val_void();
}

static void json_skip_ws(const char *s, size_t len, size_t *pos) {
    while (*pos < len) {
        char c = s[*pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') (*pos)++;
        else break;
    }
}

static int json_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool json_parse_string_slice(MemArena *mem, const char *s, size_t len,
                                    size_t *pos, StrSlice *out_slice) {
    if (*pos >= len || s[*pos] != '"') {
        mem_err(mem, "JSON parse error: expected '\"' at offset %u", (unsigned)*pos);
        return false;
    }
    (*pos)++;
    char stack_buf[2048];
    char *buf = stack_buf;
    size_t cap = sizeof(stack_buf);
    size_t out_len = 0;

    while (*pos < len) {
        char c = s[(*pos)++];
        if (c == '"') {
            *out_slice = mem_intern_str(mem, buf, (uint16_t)out_len);
            if (buf != stack_buf) free(buf);
            return !mem->has_error;
        }
        if (out_len + 8u >= cap) {
            size_t new_cap = cap * 2u;
            if (new_cap > 28000u) {
                if (buf != stack_buf) free(buf);
                mem_err(mem, "JSON string literal exceeds maximum length");
                return false;
            }
            char *nb = (char *)malloc(new_cap);
            if (!nb) {
                if (buf != stack_buf) free(buf);
                mem_err(mem, "Out of memory parsing JSON string");
                return false;
            }
            memcpy(nb, buf, out_len);
            if (buf != stack_buf) free(buf);
            buf = nb;
            cap = new_cap;
        }
        if (c == '\\') {
            if (*pos >= len) break;
            char esc = s[(*pos)++];
            switch (esc) {
                case '"':  buf[out_len++] = '"';  break;
                case '\\': buf[out_len++] = '\\'; break;
                case '/':  buf[out_len++] = '/';  break;
                case 'b':  buf[out_len++] = '\b'; break;
                case 'f':  buf[out_len++] = '\f'; break;
                case 'n':  buf[out_len++] = '\n'; break;
                case 'r':  buf[out_len++] = '\r'; break;
                case 't':  buf[out_len++] = '\t'; break;
                case 'u': {
                    if (*pos + 4u > len) {
                        if (buf != stack_buf) free(buf);
                        mem_err(mem, "JSON parse error: incomplete \\uXXXX escape");
                        return false;
                    }
                    int d0 = json_hex_digit(s[*pos + 0]);
                    int d1 = json_hex_digit(s[*pos + 1]);
                    int d2 = json_hex_digit(s[*pos + 2]);
                    int d3 = json_hex_digit(s[*pos + 3]);
                    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) {
                        if (buf != stack_buf) free(buf);
                        mem_err(mem, "JSON parse error: invalid hex in \\uXXXX escape");
                        return false;
                    }
                    (*pos) += 4u;
                    uint32_t cp = ((uint32_t)d0 << 12u) | ((uint32_t)d1 << 8u) |
                                  ((uint32_t)d2 << 4u)  | (uint32_t)d3;
                    if (cp < 0x80u) {
                        buf[out_len++] = (char)cp;
                    } else if (cp < 0x800u) {
                        buf[out_len++] = (char)(0xC0u | (cp >> 6u));
                        buf[out_len++] = (char)(0x80u | (cp & 0x3Fu));
                    } else {
                        buf[out_len++] = (char)(0xE0u | (cp >> 12u));
                        buf[out_len++] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
                        buf[out_len++] = (char)(0x80u | (cp & 0x3Fu));
                    }
                    break;
                }
                default:
                    buf[out_len++] = esc;
                    break;
            }
        } else {
            buf[out_len++] = c;
        }
    }
    if (buf != stack_buf) free(buf);
    mem_err(mem, "JSON parse error: unterminated string literal");
    return false;
}

static Value json_parse_val_rec(MemArena *mem, SymTable *tab, const char *s,
                                size_t len, size_t *pos, uint8_t depth) {
    if (depth > 24u) {
        mem_err(mem, "JSON nesting depth limit (24) exceeded");
        return val_void();
    }
    json_skip_ws(s, len, pos);
    if (*pos >= len) {
        mem_err(mem, "JSON parse error: unexpected end of input");
        return val_void();
    }
    char c = s[*pos];

    /* 1. String */
    if (c == '"') {
        StrSlice sl = { 0, 0 };
        if (!json_parse_string_slice(mem, s, len, pos, &sl)) return val_void();
        return val_make_str(mem, mem->str_type_id, sl);
    }

    /* 2. Object -> $map */
    if (c == '{') {
        (*pos)++;
        uint16_t hdr_idx = mem_map_alloc_hdr(mem, 0, 4);
        if (mem->has_error) return val_void();
        Value mval = val_make_comp(mem, mem->map_type_id, hdr_idx, MEM_MAP_HDR_SLOTS);
        uint16_t saved_tr = mem_push_temp_root(mem, &mval);

        json_skip_ws(s, len, pos);
        if (*pos < len && s[*pos] == '}') {
            (*pos)++;
            mem_pop_temp_roots(mem, saved_tr);
            return mval;
        }
        while (*pos < len && !mem->has_error) {
            json_skip_ws(s, len, pos);
            StrSlice kslice = { 0, 0 };
            if (!json_parse_string_slice(mem, s, len, pos, &kslice)) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }
            Value kval = val_make_str(mem, mem->str_type_id, kslice);
            (void)mem_push_temp_root(mem, &kval);

            json_skip_ws(s, len, pos);
            if (*pos >= len || s[*pos] != ':') {
                mem_pop_temp_roots(mem, saved_tr);
                mem_err(mem, "JSON parse error: expected ':' after object key at offset %u", (unsigned)*pos);
                return val_void();
            }
            (*pos)++;

            Value vval = json_parse_val_rec(mem, tab, s, len, pos, (uint8_t)(depth + 1));
            if (mem->has_error) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }
            (void)delegate_eval_intrinsic(mem, tab, INTR_MAP_SET, mval, kval, vval, 0);
            mem->temp_root_count = (uint16_t)(saved_tr + 1u);
            if (mem->has_error) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }

            json_skip_ws(s, len, pos);
            if (*pos < len && s[*pos] == ',') {
                (*pos)++;
                continue;
            }
            if (*pos < len && s[*pos] == '}') {
                (*pos)++;
                mem_pop_temp_roots(mem, saved_tr);
                return mval;
            }
            mem_pop_temp_roots(mem, saved_tr);
            mem_err(mem, "JSON parse error: expected ',' or '}' in object at offset %u", (unsigned)*pos);
            return val_void();
        }
        mem_pop_temp_roots(mem, saved_tr);
        mem_err(mem, "JSON parse error: unterminated object '{'");
        return val_void();
    }

    /* 3. Array -> $vec */
    if (c == '[') {
        (*pos)++;
        json_skip_ws(s, len, pos);
        if (*pos < len && s[*pos] == ']') {
            (*pos)++;
            uint16_t hdr_idx = mem_vec_alloc_hdr(mem, 0, 0);
            if (mem->has_error) return val_void();
            return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
        }

        Value items[256];
        uint16_t cnt = 0;
        uint16_t saved_tr = mem->temp_root_count;
        uint16_t common_tid = 0;
        bool     mixed_types = false;

        while (*pos < len && !mem->has_error) {
            if (cnt >= 256u) {
                mem_pop_temp_roots(mem, saved_tr);
                mem_err(mem, "JSON array exceeds 256 elements");
                return val_void();
            }
            items[cnt] = json_parse_val_rec(mem, tab, s, len, pos, (uint8_t)(depth + 1));
            if (mem->has_error) {
                mem_pop_temp_roots(mem, saved_tr);
                return val_void();
            }
            (void)mem_push_temp_root(mem, &items[cnt]);
            uint16_t it_tid = val_get_type(mem, items[cnt]);
            if (cnt == 0) {
                common_tid = it_tid;
            } else if (it_tid != common_tid) {
                mixed_types = true;
            }
            cnt++;

            json_skip_ws(s, len, pos);
            if (*pos < len && s[*pos] == ',') {
                (*pos)++;
                continue;
            }
            if (*pos < len && s[*pos] == ']') {
                (*pos)++;
                uint16_t elem_tid = mixed_types ? (uint16_t)SYM_TYPE_ANY : common_tid;
                uint16_t stride   = (elem_tid != SYM_TYPE_ANY && elem_tid > 0 && elem_tid < tab->type_count)
                                    ? tab->types[elem_tid].flat_size : 1u;
                uint16_t hdr_idx  = mem_vec_alloc_hdr(mem, elem_tid, cnt);
                if (mem->has_error || !mem_vec_reserve(mem, hdr_idx, stride, cnt)) {
                    mem_pop_temp_roots(mem, saved_tr);
                    return val_void();
                }
                uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
                for (uint16_t i = 0; i < cnt; ++i) {
                    delegate_vec_write_elem(mem, tab, elem_tid, data_idx, (uint32_t)i * (uint32_t)stride, items[i]);
                }
                mem_write_pool(mem, (uint16_t)(hdr_idx + 1), (double)cnt);
                mem_pop_temp_roots(mem, saved_tr);
                return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
            }
            mem_pop_temp_roots(mem, saved_tr);
            mem_err(mem, "JSON parse error: expected ',' or ']' in array at offset %u", (unsigned)*pos);
            return val_void();
        }
        mem_pop_temp_roots(mem, saved_tr);
        mem_err(mem, "JSON parse error: unterminated array '['");
        return val_void();
    }

    /* 4. Booleans & null */
    if (*pos + 4u <= len && memcmp(s + *pos, "true", 4) == 0) {
        (*pos) += 4u;
        return val_make_f64(mem, mem->fast_real_type, 1.0);
    }
    if (*pos + 5u <= len && memcmp(s + *pos, "false", 5) == 0) {
        (*pos) += 5u;
        return val_make_f64(mem, mem->fast_real_type, 0.0);
    }
    if (*pos + 4u <= len && memcmp(s + *pos, "null", 4) == 0) {
        (*pos) += 4u;
        return val_make_f64(mem, mem->fast_real_type, (double)NAN);
    }

    /* 5. Number */
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *endp = NULL;
        double d = strtod(s + *pos, &endp);
        if (endp && endp > s + *pos) {
            *pos = (size_t)(endp - s);
            return val_make_f64(mem, mem->fast_real_type, d);
        }
    }

    mem_err(mem, "JSON parse error: unexpected token near '%.12s' at offset %u",
            s + *pos, (unsigned)*pos);
    return val_void();
}

static Value eval_json_parse_op(MemArena *mem, SymTable *tab, Value a) {
    if (mem->str_type_id == 0 || val_get_type(mem, a) != mem->str_type_id) {
        mem_err(mem, "json_parse() expects a JSON string ($str)");
        return val_void();
    }
    StrSlice sl = val_get_str(mem, a);
    char *copy = (char *)malloc((size_t)sl.len + 1u);
    if (!copy) {
        mem_err(mem, "Out of memory in json_parse()");
        return val_void();
    }
    if (sl.len > 0) memcpy(copy, mem_str_ptr(mem, sl), sl.len);
    copy[sl.len] = '\0';

    size_t pos = 0;
    Value res = json_parse_val_rec(mem, tab, copy, sl.len, &pos, 0);
    if (!mem->has_error) {
        json_skip_ws(copy, sl.len, &pos);
        if (pos < sl.len) {
            mem_err(mem, "JSON parse error: trailing characters at offset %u", (unsigned)pos);
            free(copy);
            return val_void();
        }
    }
    free(copy);
    return res;
}

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} JsonBuf;

static void jbuf_append_raw(JsonBuf *jb, const char *s, size_t n) {
    if (!jb->buf || n == 0) return;
    if (jb->len + n + 1u > jb->cap) {
        size_t new_cap = jb->cap * 2u;
        while (new_cap < jb->len + n + 1u) new_cap *= 2u;
        if (new_cap > 28000u) return;
        char *nb = (char *)realloc(jb->buf, new_cap);
        if (!nb) return;
        jb->buf = nb;
        jb->cap = new_cap;
    }
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
}

static void jbuf_append_cstr(JsonBuf *jb, const char *s) {
    jbuf_append_raw(jb, s, strlen(s));
}

static void jbuf_append_indent(JsonBuf *jb, int spaces) {
    for (int i = 0; i < spaces; ++i) jbuf_append_raw(jb, " ", 1u);
}

static void jbuf_append_escaped_str(JsonBuf *jb, const char *s, uint16_t slen) {
    jbuf_append_raw(jb, "\"", 1u);
    for (uint16_t i = 0; i < slen; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  jbuf_append_raw(jb, "\\\"", 2u); break;
            case '\\': jbuf_append_raw(jb, "\\\\", 2u); break;
            case '\b': jbuf_append_raw(jb, "\\b", 2u);  break;
            case '\f': jbuf_append_raw(jb, "\\f", 2u);  break;
            case '\n': jbuf_append_raw(jb, "\\n", 2u);  break;
            case '\r': jbuf_append_raw(jb, "\\r", 2u);  break;
            case '\t': jbuf_append_raw(jb, "\\t", 2u);  break;
            default:
                if (c < 0x20u) {
                    char uesc[8];
                    snprintf(uesc, sizeof(uesc), "\\u%04x", (unsigned)c);
                    jbuf_append_raw(jb, uesc, 6u);
                } else {
                    char ch = (char)c;
                    jbuf_append_raw(jb, &ch, 1u);
                }
                break;
        }
    }
    jbuf_append_raw(jb, "\"", 1u);
}

static void json_format_val_rec(MemArena *mem, const SymTable *tab, Value v,
                                int indent_step, int cur_indent, JsonBuf *jb, uint8_t depth) {
    if (depth > 20u) {
        jbuf_append_cstr(jb, "null");
        return;
    }
    uint16_t tid = val_get_type(mem, v);
    if (tid == MEM_TYPE_VOID) {
        jbuf_append_cstr(jb, "null");
        return;
    }
    if (tid == mem->str_type_id && tid != 0) {
        StrSlice s = val_get_str(mem, v);
        jbuf_append_escaped_str(jb, mem_str_ptr(mem, s), s.len);
        return;
    }
    if (tid == mem->vec_type_id && tid != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(v);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 0));
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8u) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
        uint16_t stride   = (elem_tid != SYM_TYPE_ANY && elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1u;
        if (len == 0) {
            jbuf_append_cstr(jb, "[]");
            return;
        }
        jbuf_append_cstr(jb, "[");
        for (uint32_t i = 0; i < len; ++i) {
            if (i > 0) jbuf_append_cstr(jb, ",");
            if (indent_step > 0) {
                jbuf_append_cstr(jb, "\n");
                jbuf_append_indent(jb, cur_indent + indent_step);
            } else if (i > 0) {
                /* compact JSON: no extra space or optional space */
            }
            Value ev = delegate_vec_read_elem(mem, tab, elem_tid, elem_uid, data_idx, i * (uint32_t)stride);
            json_format_val_rec(mem, tab, ev, indent_step, cur_indent + indent_step, jb, (uint8_t)(depth + 1));
        }
        if (indent_step > 0) {
            jbuf_append_cstr(jb, "\n");
            jbuf_append_indent(jb, cur_indent);
        }
        jbuf_append_cstr(jb, "]");
        return;
    }
    if (tid == mem->map_type_id && tid != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(v);
        uint16_t len      = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
        if (len == 0) {
            jbuf_append_cstr(jb, "{}");
            return;
        }
        jbuf_append_cstr(jb, "{");
        for (uint16_t i = 0; i < len; ++i) {
            if (i > 0) jbuf_append_cstr(jb, ",");
            if (indent_step > 0) {
                jbuf_append_cstr(jb, "\n");
                jbuf_append_indent(jb, cur_indent + indent_step);
            }
            StrSlice ks = mem_map_entry_key(mem, data_idx, i);
            Value    ev = mem_map_entry_val(mem, data_idx, i);
            jbuf_append_escaped_str(jb, mem_str_ptr(mem, ks), ks.len);
            jbuf_append_cstr(jb, indent_step > 0 ? ": " : ":");
            json_format_val_rec(mem, tab, ev, indent_step, cur_indent + indent_step, jb, (uint8_t)(depth + 1));
        }
        if (indent_step > 0) {
            jbuf_append_cstr(jb, "\n");
            jbuf_append_indent(jb, cur_indent);
        }
        jbuf_append_cstr(jb, "}");
        return;
    }
    if (tid == mem->mat_type_id && tid != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, v, &rank, &tot_len, &data_idx, dims);
        if (rank <= 1) {
            jbuf_append_cstr(jb, "[");
            for (uint16_t i = 0; i < tot_len; ++i) {
                if (i > 0) jbuf_append_cstr(jb, ",");
                char nbuf[48];
                snprintf(nbuf, sizeof(nbuf), "%.15g", mem_data_read(mem, data_idx, i));
                jbuf_append_cstr(jb, nbuf);
            }
            jbuf_append_cstr(jb, "]");
        } else {
            uint16_t rows = dims[0];
            uint16_t cols = dims[1];
            jbuf_append_cstr(jb, "[");
            for (uint16_t r = 0; r < rows; ++r) {
                if (r > 0) jbuf_append_cstr(jb, ",");
                jbuf_append_cstr(jb, "[");
                for (uint16_t c = 0; c < cols; ++c) {
                    if (c > 0) jbuf_append_cstr(jb, ",");
                    char nbuf[48];
                    snprintf(nbuf, sizeof(nbuf), "%.15g", mem_data_read(mem, data_idx, (uint32_t)r * cols + c));
                    jbuf_append_cstr(jb, nbuf);
                }
                jbuf_append_cstr(jb, "]");
            }
            jbuf_append_cstr(jb, "]");
        }
        return;
    }
    if (tid > 0 && tid < tab->type_count && !tab->types[tid].is_consuming) {
        const TypeDef *td = &tab->types[tid];
        uint16_t pidx = val_get_pool_idx(v);
        if (td->parser_intr == INTR_PARSE_I64 && td->field_count > 0) {
            int v_tag = (int)llround(mem_read_pool(mem, pidx));
            uint8_t scan = 0;
            while (scan < td->field_count) {
                uint16_t vfd_i = (uint16_t)(td->start_idx + scan);
                const FieldDef *vfd = &tab->fields[vfd_i];
                if (vfd->type_id == tid) {
                    int cur_tag  = (int)(vfd->flat_offset & 0xFFu);
                    uint8_t pcnt = (uint8_t)(vfd->flat_offset >> 8u);
                    if (cur_tag == v_tag) {
                        if (pcnt == 0) {
                            jbuf_append_escaped_str(jb, mem_str_ptr(mem, vfd->name), vfd->name.len);
                            return;
                        }
                        jbuf_append_cstr(jb, "{\"$variant\":");
                        jbuf_append_escaped_str(jb, mem_str_ptr(mem, vfd->name), vfd->name.len);
                        for (uint8_t pi = 0; pi < pcnt; ++pi) {
                            const FieldDef *pfd = &tab->fields[vfd_i + 1u + pi];
                            jbuf_append_cstr(jb, ",");
                            jbuf_append_escaped_str(jb, mem_str_ptr(mem, pfd->name), pfd->name.len);
                            jbuf_append_cstr(jb, ":");
                            uint16_t fpidx = (uint16_t)(pidx + pfd->flat_offset);
                            Value fv;
                            if (pfd->type_id == mem->str_type_id && mem->str_type_id != 0) {
                                uint32_t pk = (uint32_t)mem_read_pool(mem, fpidx);
                                StrSlice fs = { (uint16_t)(pk >> 16u), (uint16_t)(pk & 0xFFFFu) };
                                fv = val_make_str(mem, pfd->type_id, fs);
                            } else if (!tab->types[pfd->type_id].is_consuming) {
                                fv = val_make_comp(mem, pfd->type_id, fpidx, tab->types[pfd->type_id].flat_size);
                            } else {
                                fv = val_make_f64(mem, pfd->type_id, mem_read_pool(mem, fpidx));
                            }
                            json_format_val_rec(mem, tab, fv, indent_step, cur_indent + indent_step, jb, (uint8_t)(depth + 1));
                        }
                        jbuf_append_cstr(jb, "}");
                        return;
                    }
                    scan = (uint8_t)(scan + 1u + pcnt);
                } else {
                    scan++;
                }
            }
        }
        /* Regular $struct -> JSON object {"field": val, ...} */
        jbuf_append_cstr(jb, "{");
        for (uint8_t i = 0; i < td->field_count; ++i) {
            if (i > 0) jbuf_append_cstr(jb, ",");
            if (indent_step > 0) {
                jbuf_append_cstr(jb, "\n");
                jbuf_append_indent(jb, cur_indent + indent_step);
            }
            const FieldDef *fd = &tab->fields[td->start_idx + i];
            jbuf_append_escaped_str(jb, mem_str_ptr(mem, fd->name), fd->name.len);
            jbuf_append_cstr(jb, indent_step > 0 ? ": " : ":");
            uint16_t fpidx = (uint16_t)(pidx + fd->flat_offset);
            Value fv;
            if (fd->type_id == mem->str_type_id && mem->str_type_id != 0) {
                uint32_t pk = (uint32_t)mem_read_pool(mem, fpidx);
                StrSlice fs = { (uint16_t)(pk >> 16u), (uint16_t)(pk & 0xFFFFu) };
                fv = val_make_str(mem, fd->type_id, fs);
            } else if (!tab->types[fd->type_id].is_consuming) {
                fv = val_make_comp(mem, fd->type_id, fpidx, tab->types[fd->type_id].flat_size);
            } else {
                fv = val_make_f64(mem, fd->type_id, mem_read_pool(mem, fpidx));
            }
            json_format_val_rec(mem, tab, fv, indent_step, cur_indent + indent_step, jb, (uint8_t)(depth + 1));
        }
        if (indent_step > 0 && td->field_count > 0) {
            jbuf_append_cstr(jb, "\n");
            jbuf_append_indent(jb, cur_indent);
        }
        jbuf_append_cstr(jb, "}");
        return;
    }
    if (tid > 0 && tid < tab->type_count && tab->types[tid].parser_intr == INTR_PARSE_I64) {
        const TypeDef *td = &tab->types[tid];
        int v_tag = (int)llround(val_get_f64(mem, v));
        for (uint8_t i = 0; i < td->field_count; ++i) {
            const FieldDef *fd = &tab->fields[td->start_idx + i];
            if ((int)(int16_t)fd->flat_offset == v_tag) {
                jbuf_append_escaped_str(jb, mem_str_ptr(mem, fd->name), fd->name.len);
                return;
            }
        }
    }
    double d = val_get_f64(mem, v);
    if (!isfinite(d)) {
        jbuf_append_cstr(jb, "null");
    } else {
        char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "%.15g", d);
        jbuf_append_cstr(jb, nbuf);
    }
}

static Value eval_json_stringify_op(MemArena *mem, const SymTable *tab, Value a, Value b) {
    int indent_step = 0;
    if (val_get_type(mem, b) != MEM_TYPE_VOID) {
        double id = val_get_f64(mem, b);
        if (isfinite(id) && id > 0.0) {
            indent_step = (id > 8.0) ? 8 : (int)id;
        }
    }
    JsonBuf jb;
    jb.cap = 512u;
    jb.len = 0u;
    jb.buf = (char *)malloc(jb.cap);
    if (!jb.buf) {
        mem_err(mem, "Out of memory in json_stringify()");
        return val_void();
    }
    jb.buf[0] = '\0';
    json_format_val_rec(mem, tab, a, indent_step, 0, &jb, 0);
    StrSlice s = mem_intern_str(mem, jb.buf, (uint16_t)jb.len);
    free(jb.buf);
    if (mem->has_error) return val_void();
    return val_make_str(mem, mem->str_type_id, s);
}

/* ============================================================================
 * 4. RECURSIVE AST EVALUATOR
 * ============================================================================ */

static Value eval_ast_inner(MemArena *mem, SymTable *tab,
                            uint16_t node_idx, uint16_t frame_base, uint8_t cur_ret_type);

Value eval_ast(MemArena *mem, SymTable *tab,
               uint16_t node_idx, uint16_t frame_base, uint8_t cur_ret_type) {
    if (mem->has_error || mem->jump_active != JUMP_NONE || node_idx == MEM_NO_IDX) return val_void();
    if (++mem->eval_steps > MEM_MAX_EVAL_STEPS) {
        mem_err(mem, "Evaluation step budget exceeded (max %u steps)", (unsigned)MEM_MAX_EVAL_STEPS);
        return val_void();
    }
    if (mem->eval_depth >= MEM_MAX_EVAL_DEPTH) {
        mem_err(mem, "Evaluation recursion depth limit exceeded (max %d)", MEM_MAX_EVAL_DEPTH);
        return val_void();
    }
    mem->eval_depth++;
    Value res = eval_ast_inner(mem, tab, node_idx, frame_base, cur_ret_type);
    mem->eval_depth--;
    return res;
}

static Value eval_ast_inner(MemArena *mem, SymTable *tab,
                            uint16_t node_idx, uint16_t frame_base, uint8_t cur_ret_type) {
    const AstNode *n = mem_get_ast(mem, node_idx);

    switch ((AstKind)n->kind) {
        case AST_LIT: {
            uint16_t tid = n->sub;
            if (tid == MEM_TYPE_VOID) return val_void();
            if (tid == mem->str_type_id && tid != 0) {
                StrSlice s = { n->left, n->right };
                return val_make_str(mem, tid, s);
            }
            return val_make_f64(mem, tid, mem_read_pool(mem, n->ref));
        }

        case AST_CONST_SYM: {
            uint16_t sym_id     = n->ref;
            uint16_t caller_mod = (n->right != MEM_NO_IDX && n->right < tab->mod_count)
                                  ? n->right : mem->cur_mod;
            if ((n->sub & SYM_VAR_QUAL_MOD) != 0 && n->right < tab->mod_count) {
                uint16_t qmod = n->right;
                caller_mod    = mem->cur_mod;
                int own_idx = sym_find_own_var(tab, sym_id, qmod);
                if (own_idx >= 0) {
                    const ModVarDef *mv = &sym_vars(tab)[own_idx];
                    if ((mv->flags & SYM_VAR_PRIVATE) != 0 && qmod != caller_mod) {
                        StrSlice s  = tab->syms[sym_id];
                        StrSlice ms = sym_mod_name(tab, qmod);
                        mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                                (int)s.len, mem_str_ptr(mem, s),
                                (int)ms.len, mem_str_ptr(mem, ms));
                        return val_void();
                    }
                    return mv->val;
                }
                for (uint8_t i = 0; i < tab->const_count; ++i) {
                    if (tab->consts[i].sym_id == sym_id && tab->consts[i].mod_id == qmod) {
                        if ((tab->consts[i].reserved & SYM_VAR_PRIVATE) != 0 && qmod != caller_mod) {
                            StrSlice s  = tab->syms[sym_id];
                            StrSlice ms = sym_mod_name(tab, qmod);
                            mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                                    (int)s.len, mem_str_ptr(mem, s),
                                    (int)ms.len, mem_str_ptr(mem, ms));
                            return val_void();
                        }
                        return tab->const_vals[tab->consts[i].val_idx];
                    }
                }
                StrSlice s  = tab->syms[sym_id];
                StrSlice ms = sym_mod_name(tab, qmod);
                mem_err(mem, "Undefined symbol or constant '%.*s' in module '[%.*s]'",
                        (int)s.len, mem_str_ptr(mem, s),
                        (int)ms.len, mem_str_ptr(mem, ms));
                return val_void();
            }
            uint16_t priv_mod   = MEM_NO_IDX;
            int mv_idx = sym_find_var(tab, sym_id, caller_mod, &priv_mod);
            if (mv_idx >= 0) {
                return sym_vars(tab)[mv_idx].val;
            }
            if (priv_mod == MEM_NO_IDX) {
                for (uint8_t i = 0; i < tab->const_count; ++i) {
                    if (tab->consts[i].sym_id == sym_id && sym_mod_enabled(tab, tab->consts[i].mod_id)) {
                        return tab->const_vals[tab->consts[i].val_idx];
                    }
                }
            }
            StrSlice s = tab->syms[sym_id];
            if (priv_mod != MEM_NO_IDX && priv_mod < tab->mod_count) {
                StrSlice ms = sym_mod_name(tab, priv_mod);
                mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                        (int)s.len, mem_str_ptr(mem, s),
                        (int)ms.len, mem_str_ptr(mem, ms));
            } else {
                mem_err(mem, "Constant '%.*s' is disabled or undefined",
                        (int)s.len, mem_str_ptr(mem, s));
            }
            return val_void();
        }

        case AST_SLOT_LOAD: {
            if ((n->ref & 0x4000u) != 0) {
                uint16_t var_sym = (uint16_t)(n->ref & 0x3FFFu);
                if (var_sym < tab->sym_count) {
                    StrSlice vs = tab->syms[var_sym];
                    mem_err(mem, "Undefined symbol or constant '%.*s'",
                            (int)vs.len, mem_str_ptr(mem, vs));
                } else {
                    mem_err(mem, "Undefined variable in expression");
                }
                return val_void();
            }
            uint8_t slot_idx = (uint8_t)(n->sub & AST_SLOT_IDX_MASK);
            return mem_get_slot(mem, frame_base, slot_idx);
        }

        case AST_FIELD_LOAD: {
            Value base = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            uint16_t base_pidx = val_get_pool_idx(base);
            uint8_t  base_unit = val_get_unit(mem, base);
            uint16_t ftid      = n->sub;
            uint16_t foff      = n->ref;
            if (ftid != 0 && ftid < tab->type_count && !tab->types[ftid].is_consuming) {
                return val_with_unit(mem, val_make_comp(mem, ftid, base_pidx + foff, tab->types[ftid].flat_size),
                                     base_unit);
            }
            if (ftid == mem->str_type_id && mem->str_type_id != 0) {
                uint32_t packed = (uint32_t)mem_read_pool(mem, base_pidx + foff);
                StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
                return val_make_str(mem, ftid, s);
            }
            return val_make_f64_unit(mem, ftid ? ftid : mem->fast_real_type, base_unit,
                                     mem_read_pool(mem, base_pidx + foff));
        }

        case AST_UNARY: {
            Value arg = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            return eval_dispatch_overload(mem, tab, n->ref, (OpFixity)n->sub, 1, &arg);
        }

        case AST_BINARY: {
            uint8_t mtag = sym_active_meta_tag(tab, n->ref, (OpFixity)n->sub);

            if (mtag == INTR_SEQ) {
                return eval_seq_chain(mem, tab, node_idx, frame_base, cur_ret_type);
            }

            if (mtag == INTR_STORE) {
                Value rhs = eval_ast(mem, tab, n->right, frame_base, cur_ret_type);
                const AstNode *rn_chk = mem_get_ast(mem, n->right);
                if ((mem->jump_active == JUMP_RETURN || mem->jump_active == JUMP_BREAK_ANON) &&
                    rn_chk && ((rn_chk->kind == AST_BINARY &&
                                sym_active_meta_tag(tab, rn_chk->ref, (OpFixity)rn_chk->sub) == INTR_SEQ) ||
                               rn_chk->kind == AST_RETURN || rn_chk->kind == AST_BREAK) &&
                    val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                    rhs = mem->jump_val;
                    mem->jump_val    = val_void();
                    mem->jump_active = JUMP_NONE;
                }
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                const AstNode *lhs = mem_get_ast(mem, n->left);
                if (lhs->kind == AST_SLOT_LOAD) {
                    uint8_t  slot_idx      = (uint8_t)(lhs->sub & AST_SLOT_IDX_MASK);
                    bool     is_const_decl = (lhs->sub & AST_SLOT_CONST_DECL_BIT) != 0;
                    uint16_t abs_slot      = (uint16_t)(frame_base + slot_idx);
                    if (abs_slot >= MEM_MAX_CALL_STACK) {
                        mem_err(mem, "Stack slot index %u out of bounds", (unsigned)abs_slot);
                        return val_void();
                    }
                    if (mem->slot_is_const[abs_slot] != 0 && !is_const_decl) {
                        mem_err(mem, "Cannot reassign or mutate immutable constant");
                        return val_void();
                    }
                    if (lhs->left != 0 && lhs->left != MEM_NO_IDX) {
                        mem->slot_locked_type[abs_slot] = (uint8_t)lhs->left;
                    }
                    if (lhs->right != 0 && lhs->right != MEM_NO_IDX) {
                        mem->slot_locked_unit[abs_slot] = (uint8_t)lhs->right;
                        if ((uint8_t)lhs->right < SYM_UNIT_LOCK_NONE &&
                            val_get_type(mem, rhs) != MEM_TYPE_VOID &&
                            val_get_unit(mem, rhs) == MEM_UNIT_NONE) {
                            rhs = val_with_unit(mem, rhs, (uint8_t)lhs->right);
                        }
                    }

                    uint8_t req_type = mem->slot_locked_type[abs_slot];
                    if (req_type != 0 && req_type != SYM_TYPE_ANY) {
                        uint16_t rhs_type = val_get_type(mem, rhs);
                        if (rhs_type != req_type) {
                            char exp_t[32], got_t[32];
                            sym_type_name(tab, mem, req_type, exp_t, sizeof(exp_t));
                            sym_type_name(tab, mem, rhs_type, got_t, sizeof(got_t));
                            mem_err(mem, "Type mismatch: variable holds concrete type '%s', cannot assign '%s'",
                                    exp_t, got_t);
                            return val_void();
                        }
                    }

                    uint8_t req_unit = mem->slot_locked_unit[abs_slot];
                    if (req_unit != 0 && req_unit != SYM_UNIT_GENERIC && req_unit != SYM_UNIT_OMITTED) {
                        uint8_t expected_u = (req_unit == SYM_UNIT_LOCK_NONE) ? MEM_UNIT_NONE : req_unit;
                        uint8_t rhs_unit   = val_get_unit(mem, rhs);
                        if (rhs_unit != expected_u) {
                            char exp_u[32], got_u[32];
                            sym_unit_name(tab, mem, expected_u, exp_u, sizeof(exp_u));
                            sym_unit_name(tab, mem, rhs_unit,   got_u, sizeof(got_u));
                            mem_err(mem, "Unit mismatch: variable holds concrete unit '%s', cannot assign '%s'",
                                    exp_u, got_u);
                            return val_void();
                        }
                    }

                    mem_set_slot(mem, frame_base, slot_idx, rhs);
                    if (is_const_decl) {
                        mem->slot_is_const[abs_slot] = 1u;
                        mem_val_mark_immutable(mem, rhs);
                    }
                    /* Sync recursive self-binding or active-frame upvalue mutation to closure HeapBlocks */
                    if (mem->heap_active_count > 0) {
                        bool is_colon_eq = false;
                        (void)hardcode_is_plain_store_sym(mem, tab, n->ref, &is_colon_eq);
                        uint16_t self_hdr = (mem->fn_type_id != 0 && val_get_type(mem, rhs) == mem->fn_type_id)
                                            ? val_get_pool_idx(rhs) : MEM_NO_IDX;
                        for (uint16_t hid = 0; hid < MEM_MAX_HEAP_BLOCKS; ++hid) {
                            HeapBlock *hb = &mem->heap_blocks[hid];
                            if (hb->in_use == 2u && hb->data != NULL && hb->cap_slots >= 3u) {
                                uint32_t meta    = (uint32_t)hb->data[hb->cap_slots - 1u];
                                uint16_t orig_fb = (uint16_t)(meta >> 8u);
                                uint8_t  s_start = (uint8_t)(meta & 0xFFu);
                                if (orig_fb == frame_base && slot_idx < s_start &&
                                    (!is_colon_eq || hb->owner_hdr == self_hdr)) {
                                    memcpy(&hb->data[slot_idx], &rhs, sizeof(Value));
                                    uint32_t lpack = (uint32_t)mem->slot_locked_type[abs_slot]
                                                   | ((uint32_t)mem->slot_locked_unit[abs_slot] << 8u)
                                                   | ((uint32_t)mem->slot_is_const[abs_slot] << 16u);
                                    hb->data[s_start + slot_idx] = (double)lpack;
                                }
                            }
                        }
                    }
                    return rhs;
                } else if (lhs->kind == AST_CONST_SYM) {
                    uint16_t sym_id    = lhs->ref;
                    uint8_t  flags     = lhs->sub;
                    uint16_t lock_type = (lhs->left != MEM_NO_IDX) ? (uint16_t)(lhs->left & 0xFFu) : 0;
                    uint8_t  lock_unit = (lhs->left != MEM_NO_IDX) ? (uint8_t)((lhs->left >> 8u) & 0xFFu) : 0;
                    uint16_t mod_id    = (lhs->right != MEM_NO_IDX && lhs->right < tab->mod_count)
                                         ? lhs->right : mem->cur_mod;
                    bool     is_decl   = (flags & SYM_VAR_REASSIGN_ONLY) == 0;
                    uint8_t  clean_fl  = (uint8_t)(flags & ~SYM_VAR_QUAL_MOD);
                    (void)clean_fl;
                    if (sym_set_or_add_var(tab, mem, sym_id, mod_id, rhs, lock_type, lock_unit, flags, is_decl) < 0) {
                        return val_void();
                    }
                    int mv_idx = sym_find_own_var(tab, sym_id, mod_id);
                    return (mv_idx >= 0) ? sym_vars(tab)[mv_idx].val : rhs;
                } else if (lhs->kind == AST_INDEX || lhs->kind == AST_FIELD_LOAD) {
                    const AstNode *base_n = lhs;
                    while (base_n->kind == AST_INDEX || base_n->kind == AST_FIELD_LOAD) {
                        if (base_n->kind == AST_FIELD_LOAD && base_n->right != MEM_NO_IDX) {
                            uint16_t abs_s = (uint16_t)(frame_base + (base_n->right & AST_SLOT_IDX_MASK));
                            if (abs_s < MEM_MAX_CALL_STACK && mem->slot_is_const[abs_s] != 0) {
                                mem_err(mem, "Cannot mutate field of immutable constant");
                                return val_void();
                            }
                            base_n = NULL;
                            break;
                        }
                        if (base_n->left == MEM_NO_IDX) { base_n = NULL; break; }
                        base_n = mem_get_ast(mem, base_n->left);
                    }
                    if (base_n) {
                        if (base_n->kind == AST_SLOT_LOAD) {
                            uint16_t abs_s = (uint16_t)(frame_base + (base_n->sub & AST_SLOT_IDX_MASK));
                            if (abs_s < MEM_MAX_CALL_STACK && mem->slot_is_const[abs_s] != 0) {
                                mem_err(mem, "Cannot mutate immutable constant");
                                return val_void();
                            }
                        } else if (base_n->kind == AST_CONST_SYM) {
                            uint16_t cmod = (base_n->right != MEM_NO_IDX && base_n->right < tab->mod_count)
                                            ? base_n->right : mem->cur_mod;
                            int mv_idx = sym_find_var(tab, base_n->ref, cmod, NULL);
                            if (mv_idx >= 0 && (sym_vars(tab)[mv_idx].flags & SYM_VAR_MUTABLE) == 0) {
                                StrSlice vs = tab->syms[base_n->ref];
                                mem_err(mem, "Cannot mutate immutable constant '%.*s'",
                                        (int)vs.len, mem_str_ptr(mem, vs));
                                return val_void();
                            }
                        }
                    }

                    if (lhs->kind == AST_INDEX) {
                        return eval_index_op(mem, tab, lhs, frame_base, cur_ret_type, true, rhs);
                    }
                    Value target = (lhs->right != MEM_NO_IDX)
                                   ? mem_get_slot(mem, frame_base, (uint8_t)(lhs->right & AST_SLOT_IDX_MASK))
                                   : eval_ast(mem, tab, lhs->left, frame_base, cur_ret_type);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    uint16_t base_pidx = val_get_pool_idx(target);
                    uint16_t foff      = lhs->ref;
                    uint16_t ftid      = lhs->sub;
                    uint16_t rhs_tid   = val_get_type(mem, rhs);
                    if (ftid != 0 && rhs_tid != ftid) {
                        char exp_t[32], got_t[32];
                        sym_type_name(tab, mem, ftid, exp_t, sizeof(exp_t));
                        sym_type_name(tab, mem, rhs_tid, got_t, sizeof(got_t));
                        mem_err(mem, "Struct field expects '%s', got '%s'", exp_t, got_t);
                        return val_void();
                    }
                    if (ftid != 0 && ftid < tab->type_count && !tab->types[ftid].is_consuming) {
                        uint16_t fsize   = tab->types[ftid].flat_size;
                        uint16_t src_idx = val_get_pool_idx(rhs);
                        for (uint16_t i = 0; i < fsize; ++i) {
                            mem_write_pool(mem, (uint16_t)(base_pidx + foff + i),
                                           mem_read_pool(mem, (uint16_t)(src_idx + i)));
                        }
                    } else if (ftid == mem->str_type_id && mem->str_type_id != 0) {
                        StrSlice s = val_get_str(mem, rhs);
                        mem_write_pool(mem, base_pidx + foff,
                                       (double)(((uint32_t)s.off << 16u) | (uint32_t)s.len));
                    } else {
                        mem_write_pool(mem, base_pidx + foff, val_get_f64(mem, rhs));
                    }
                    return rhs;
                }
                mem_err(mem, "Left side of assignment operator must be a variable or struct field");
                return val_void();
            }

            if (mtag == INTR_WHILE || mtag == INTR_DO_WHILE) {
                bool is_do_while = (mtag == INTR_DO_WHILE);
                uint16_t cond_idx = is_do_while ? n->right : n->left;
                uint16_t body_idx = is_do_while ? n->left  : n->right;
                Value last = val_make_f64(mem, mem->fast_real_type, 0.0);
                uint32_t guard = 0;
                while (!mem->has_error) {
                    if (!is_do_while) {
                        Value cond = eval_ast(mem, tab, cond_idx, frame_base, cur_ret_type);
                        if (mem->has_error || mem->jump_active != JUMP_NONE ||
                            fabs(val_get_f64(mem, cond)) <= 1e-15) {
                            break;
                        }
                    }
                    last = eval_ast(mem, tab, body_idx, frame_base, cur_ret_type);
                    if (mem->jump_active == JUMP_CONTINUE_ANON) {
                        mem->jump_active = JUMP_NONE;
                        if (++guard > 1000000u) {
                            mem_err(mem, "Loop iteration limit exceeded");
                            break;
                        }
                        if (is_do_while) {
                            Value cond = eval_ast(mem, tab, cond_idx, frame_base, cur_ret_type);
                            if (mem->has_error || mem->jump_active != JUMP_NONE ||
                                fabs(val_get_f64(mem, cond)) <= 1e-15) {
                                break;
                            }
                        }
                        continue;
                    }
                    if (mem->jump_active == JUMP_BREAK_ANON) {
                        if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                            last = mem->jump_val;
                        }
                        mem->jump_active = JUMP_NONE;
                        break;
                    }
                    if (mem->jump_active != JUMP_NONE) {
                        if (mem->jump_active == JUMP_CONTINUE_TAG) {
                            mem->jump_val = last;
                        }
                        break;
                    }
                    if (is_do_while) {
                        Value cond = eval_ast(mem, tab, cond_idx, frame_base, cur_ret_type);
                        if (mem->has_error || mem->jump_active != JUMP_NONE ||
                            fabs(val_get_f64(mem, cond)) <= 1e-15) {
                            break;
                        }
                    }
                    if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                        mem->heap_active_count >= 64u) {
                        (void)mem_compact_scratch(mem, &last, 1);
                    }
                    if (++guard > 1000000u) {
                        mem_err(mem, "Loop iteration limit exceeded");
                        break;
                    }
                }
                return last;
            }

            if (mtag == INTR_CAND) {
                Value lv = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, lv)) <= 1e-15) {
                    return val_make_f64(mem, mem->fast_real_type, 0.0);
                }
                Value rv = eval_ast(mem, tab, n->right, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                return val_make_f64(mem, mem->fast_real_type,
                                    (fabs(val_get_f64(mem, rv)) > 1e-15) ? 1.0 : 0.0);
            }

            if (mtag == INTR_COR) {
                Value lv = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, lv)) > 1e-15) {
                    return val_make_f64(mem, mem->fast_real_type, 1.0);
                }
                Value rv = eval_ast(mem, tab, n->right, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                return val_make_f64(mem, mem->fast_real_type,
                                    (fabs(val_get_f64(mem, rv)) > 1e-15) ? 1.0 : 0.0);
            }

            Value in_rng_res = val_void();
            if (hardcode_eval_in_vrange_fast(mem, tab, n, frame_base, cur_ret_type, &in_rng_res)) {
                return in_rng_res;
            }

            Value args[2];
            args[0] = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            args[1] = eval_ast(mem, tab, n->right, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            uint16_t dual_unit_sym = MEM_NO_IDX;
            if (hardcode_should_use_unit_for_in_rhs(mem, tab, n, args[0], args[1], &dual_unit_sym)) {
                Value one_val = val_make_f64(mem, mem->fast_real_type, 1.0);
                args[1] = eval_dispatch_overload(mem, tab, dual_unit_sym, FIX_POSTFIX, 1, &one_val);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            }
            Value bin_res = eval_dispatch_overload(mem, tab, n->ref, (OpFixity)n->sub, 2, args);
            if (!mem->has_error && hardcode_is_in_op_sym(mem, tab, n->ref) &&
                !hardcode_ast_is_bare_unit_rhs(mem, n->right) && dual_unit_sym == MEM_NO_IDX &&
                val_get_unit(mem, args[0]) != MEM_UNIT_NONE &&
                val_get_unit(mem, args[0]) == val_get_unit(mem, args[1])) {
                bin_res = val_with_unit(mem, bin_res, MEM_UNIT_NONE);
            }
            return bin_res;
        }

        case AST_TERNARY: {
            if (n->sub == 2) {
                /* Structured `try { ... } catch (e) { ... }` block */
                uint16_t saved_sp     = mem->sp;
                uint16_t saved_troots = mem->temp_root_count;
                uint16_t saved_mod    = mem->cur_mod;
                uint16_t saved_tmod   = tab->cur_mod;

                Value try_res = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
                if (!mem->has_error && mem->jump_active != JUMP_THROW) {
                    return try_res;
                }

                Value err_val = val_void();
                if (mem->jump_active == JUMP_THROW &&
                    val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                    err_val = mem->jump_val;
                } else {
                    char saved_msg[256];
                    snprintf(saved_msg, sizeof(saved_msg), "%s",
                             mem->err_msg[0] ? mem->err_msg : "Runtime error");
                    mem_clear_err(mem);
                    err_val = val_from_cstr(mem, saved_msg);
                }

                mem_clear_err(mem);
                mem->jump_active     = JUMP_NONE;
                mem->jump_val        = val_void();
                mem->sp              = saved_sp;
                mem->temp_root_count = saved_troots;
                mem->cur_mod         = saved_mod;
                tab->cur_mod         = saved_tmod;

                const AstNode *pair = mem_get_ast(mem, n->right);
                if (pair->left != MEM_NO_IDX) {
                    const AstNode *cvar = mem_get_ast(mem, pair->left);
                    if (cvar->kind == AST_SLOT_LOAD) {
                        uint8_t c_slot = (uint8_t)(cvar->sub & AST_SLOT_IDX_MASK);
                        mem_set_slot(mem, frame_base, c_slot, err_val);
                    }
                }
                return eval_ast(mem, tab, pair->right, frame_base, cur_ret_type);
            }
            Value c = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            const AstNode *pair = mem_get_ast(mem, n->right);
            Value branch_res = val_void();
            if (fabs(val_get_f64(mem, c)) > 1e-15) {
                branch_res = eval_ast(mem, tab, pair->left, frame_base, cur_ret_type);
            } else if (pair->right != MEM_NO_IDX) {
                branch_res = eval_ast(mem, tab, pair->right, frame_base, cur_ret_type);
            }
            if (n->sub == 1 && mem->jump_active == JUMP_BREAK_ANON) {
                if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                    branch_res = mem->jump_val;
                    mem->jump_val = val_void();
                }
                mem->jump_active = JUMP_NONE;
            }
            return branch_res;
        }

        case AST_SWITCH_EQ:
            return hardcode_eval_switch_eq_ast(mem, tab, n, frame_base, cur_ret_type);

        case AST_TAGGED_SCOPE: {
            uint8_t label_id = n->sub;
            if (label_id < MEM_MAX_LABELS) {
                mem->label_ast[label_id] = n->left;
            }
            Value res = val_void();
            uint32_t guard = 0;
            while (!mem->has_error) {
                res = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
                if ((mem->jump_active == JUMP_GOTO || mem->jump_active == JUMP_CONTINUE_TAG) &&
                    mem->jump_target_id == label_id) {
                    bool was_continue = (mem->jump_active == JUMP_CONTINUE_TAG);
                    mem->jump_active = JUMP_NONE;
                    if (was_continue && val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        res = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    if (++guard > 1000000u) {
                        mem_err(mem, "Loop iteration limit exceeded in tagged scope");
                        return val_void();
                    }
                    if (was_continue && n->left != MEM_NO_IDX) {
                        const AstNode *ln = mem_get_ast(mem, n->left);
                        if (ln->kind == AST_BINARY) {
                            uint8_t lm = sym_active_meta_tag(tab, ln->ref, (OpFixity)ln->sub);
                            bool l_do = (lm == INTR_DO_WHILE);
                            if (l_do) {
                                Value cval = eval_ast(mem, tab, ln->right, frame_base, cur_ret_type);
                                if (mem->has_error || mem->jump_active != JUMP_NONE ||
                                    fabs(val_get_f64(mem, cval)) <= 1e-15) {
                                    break;
                                }
                            }
                        }
                    }
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == label_id)) {
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        res = mem->jump_val;
                    }
                    mem->jump_active = JUMP_NONE;
                    break;
                }
                break;
            }
            return res;
        }

        case AST_JUMP: {
            mem->jump_active    = JUMP_GOTO;
            mem->jump_target_id = n->sub;
            return val_void();
        }

        case AST_BREAK: {
            Value bval = (n->left != MEM_NO_IDX)
                         ? eval_ast(mem, tab, n->left, frame_base, cur_ret_type)
                         : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            mem->jump_val = bval;
            if (n->sub == 0xFFu) {
                mem->jump_active = JUMP_BREAK_ANON;
            } else {
                mem->jump_active    = JUMP_BREAK_TAG;
                mem->jump_target_id = n->sub;
            }
            return bval;
        }

        case AST_CONTINUE: {
            if (n->sub == 0xFFu) {
                mem->jump_active = JUMP_CONTINUE_ANON;
            } else {
                mem->jump_active    = JUMP_CONTINUE_TAG;
                mem->jump_target_id = n->sub;
            }
            return val_void();
        }

        case AST_RETURN: {
            if (n->sub == 1) {
                Value tval = (n->left != MEM_NO_IDX)
                             ? eval_ast(mem, tab, n->left, frame_base, cur_ret_type)
                             : val_from_cstr(mem, "Unhandled exception");
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                uint16_t ttid = val_get_type(mem, tval);
                if (ttid == mem->str_type_id && mem->str_type_id != 0) {
                    StrSlice s = val_get_str(mem, tval);
                    mem_err(mem, "%.*s", (int)s.len, mem_str_ptr(mem, s));
                } else if (ttid == mem->fast_real_type) {
                    mem_err(mem, "Uncaught exception: %.15g", val_get_f64(mem, tval));
                } else {
                    char tname[32];
                    sym_type_name(tab, mem, ttid, tname, sizeof(tname));
                    mem_err(mem, "Uncaught exception of type '%s'", tname);
                }
                mem->jump_val    = tval;
                mem->jump_active = JUMP_THROW;
                return val_void();
            }
            Value rval = (n->left != MEM_NO_IDX)
                         ? eval_ast(mem, tab, n->left, frame_base, cur_ret_type)
                         : val_make_f64(mem, mem->fast_real_type, 0.0);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            mem->jump_val    = rval;
            mem->jump_active = JUMP_RETURN;
            return rval;
        }

        case AST_LAMBDA: {
            if (mem->fn_type_id == 0) {
                mem_err(mem, "First-class function type ($fn) is not loaded");
                return val_void();
            }
            uint16_t hdr_idx = mem_alloc_pool(mem, MEM_FN_HDR_SLOTS);
            if (mem->has_error) return val_void();
            uint8_t  slot_start   = (uint8_t)(n->left & 0xFFu);
            uint32_t packed_slots = (uint32_t)n->left | ((uint32_t)n->right << 16);
            uint32_t mod_tag = (mem->cur_mod != MEM_NO_IDX) ? ((uint32_t)(mem->cur_mod + 1u) << 8u) : 0u;
            uint32_t packed_p1 = ((uint32_t)n->sub & 0xFFu) | mod_tag;
            uint32_t fbase_and_env = (uint32_t)frame_base;
            if (slot_start > 0) {
                uint16_t env_hid = mem_heap_alloc_block(mem, hdr_idx, (uint32_t)slot_start * 2u + 1u);
                if (mem->has_error || env_hid == MEM_NO_IDX) return val_void();
                uint16_t hid = (uint16_t)(env_hid & MEM_HEAP_ID_MASK);
                HeapBlock *hb = &mem->heap_blocks[hid];
                hb->in_use = 2u; /* Active-frame lexical closure environment */
                for (uint8_t k = 0; k < slot_start; ++k) {
                    uint16_t abs_s = (uint16_t)(frame_base + k);
                    Value sv = (abs_s < MEM_MAX_CALL_STACK) ? mem->stack[abs_s] : val_void();
                    memcpy(&hb->data[k], &sv, sizeof(Value));
                    uint32_t lpack = 0;
                    if (abs_s < MEM_MAX_CALL_STACK) {
                        lpack = (uint32_t)mem->slot_locked_type[abs_s]
                              | ((uint32_t)mem->slot_locked_unit[abs_s] << 8u)
                              | ((uint32_t)mem->slot_is_const[abs_s] << 16u);
                    }
                    hb->data[slot_start + k] = (double)lpack;
                }
                hb->data[(uint32_t)slot_start * 2u] = (double)(((uint32_t)frame_base << 8u) | (uint32_t)slot_start);
                fbase_and_env = ((uint32_t)frame_base << 16u) | (uint32_t)env_hid;
            }
            mem_write_pool(mem, hdr_idx + 0, (double)n->ref);
            mem_write_pool(mem, hdr_idx + 1, (double)packed_p1);
            mem_write_pool(mem, hdr_idx + 2, (double)packed_slots);
            mem_write_pool(mem, hdr_idx + 3, (double)fbase_and_env);
            return val_make_comp(mem, mem->fn_type_id, hdr_idx, MEM_FN_HDR_SLOTS);
        }

        case AST_FN_CALL: {
            Value fn_val = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
            const AstNode *fn_node = mem_get_ast(mem, n->left);
            if ((mem->jump_active == JUMP_RETURN || mem->jump_active == JUMP_BREAK_ANON) &&
                fn_node && ((fn_node->kind == AST_BINARY &&
                             sym_active_meta_tag(tab, fn_node->ref, (OpFixity)fn_node->sub) == INTR_SEQ) ||
                            fn_node->kind == AST_RETURN || fn_node->kind == AST_BREAK) &&
                val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                fn_val = mem->jump_val;
                mem->jump_val    = val_void();
                mem->jump_active = JUMP_NONE;
            }
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            uint16_t saved_tr = mem_push_temp_root(mem, &fn_val);
            Value args[16];
            uint8_t argc = 0;
            uint16_t cur = n->right;
            while (cur != MEM_NO_IDX && argc < 16) {
                const AstNode *link = mem_get_ast(mem, cur);
                args[argc] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) {
                    mem_pop_temp_roots(mem, saved_tr);
                    return val_void();
                }
                (void)mem_push_temp_root(mem, &args[argc]);
                argc++;
                cur = link->right;
            }
            Value call_res = delegate_call_fn(mem, tab, fn_val, argc, args);
            mem_pop_temp_roots(mem, saved_tr);
            return call_res;
        }

        case AST_RUN_TAG: {
            uint8_t label_id = n->sub;
            if (label_id >= MEM_MAX_LABELS || mem->label_ast[label_id] == MEM_NO_IDX) {
                mem_err(mem, "Cannot reuse uninitialized or unknown scope tag");
                return val_void();
            }
            uint16_t target_ast = mem->label_ast[label_id];
            Value res = val_void();
            uint32_t guard = 0;
            while (!mem->has_error) {
                res = eval_ast(mem, tab, target_ast, frame_base, cur_ret_type);
                if ((mem->jump_active == JUMP_GOTO || mem->jump_active == JUMP_CONTINUE_TAG) &&
                    mem->jump_target_id == label_id) {
                    mem->jump_active = JUMP_NONE;
                    if (++guard > 1000000u) {
                        mem_err(mem, "Loop iteration limit exceeded in reused tagged scope");
                        return val_void();
                    }
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == label_id)) {
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        res = mem->jump_val;
                    }
                    mem->jump_active = JUMP_NONE;
                    break;
                }
                break;
            }
            return res;
        }

        case AST_CALL: {
            uint8_t mtag = sym_active_meta_tag(tab, n->ref, FIX_FUNC);
            if (delegate_is_sym_meta(mtag)) {
                return delegate_eval_sym_meta(mem, tab, mtag, n, frame_base, cur_ret_type);
            }
            if (mtag == INTR_IF && n->sub == 3) {
                const AstNode *l1 = mem_get_ast(mem, n->left);
                const AstNode *l2 = mem_get_ast(mem, l1->right);
                const AstNode *l3 = mem_get_ast(mem, l2->right);
                Value c = eval_ast(mem, tab, l1->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, c)) > 1e-15) {
                    return eval_ast(mem, tab, l2->left, frame_base, cur_ret_type);
                } else {
                    return eval_ast(mem, tab, l3->left, frame_base, cur_ret_type);
                }
            }

            Value var_vec = val_void();
            if (hardcode_try_eval_variadic_vec(mem, tab, n, frame_base, cur_ret_type, &var_vec)) {
                return var_vec;
            }

            Value args[16];
            uint8_t argc = 0;
            uint16_t cur = n->left;
            while (cur != MEM_NO_IDX && argc < 16) {
                const AstNode *link = mem_get_ast(mem, cur);
                args[argc++] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                cur = link->right;
            }
            uint16_t filter_mod = (n->right != MEM_NO_IDX && (n->right & AST_CALL_QUAL_MOD_BIT) != 0)
                                  ? (uint16_t)(n->right & 0x3FFFu) : MEM_NO_IDX;
            return eval_dispatch_overload_in_mod(mem, tab, n->ref, FIX_FUNC, argc, args, filter_mod);
        }

        case AST_INTRINSIC: {
            /* Phase 5 & Phase 6 Extended Built-In Opcodes (246..252) */
            if (n->sub == 246u || n->sub == 247u) {
                /* print(...) / println(...) */
                bool add_nl = (n->sub == 247u);
                uint16_t cur = n->left;
                uint8_t idx = 0;
                Value last_v = val_make_f64(mem, mem->fast_real_type, 0.0);
                while (cur != MEM_NO_IDX && idx < 16u) {
                    const AstNode *link = mem_get_ast(mem, cur);
                    Value v = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    if (idx > 0) mem_out_append(mem, " ", 1u);
                    if (mem->str_type_id != 0 && val_get_type(mem, v) == mem->str_type_id) {
                        StrSlice s = val_get_str(mem, v);
                        mem_out_append(mem, mem_str_ptr(mem, s), s.len);
                    } else {
                        Value sv = delegate_eval_intrinsic(mem, tab, INTR_TOSTR, v, val_void(), val_void(), 0);
                        if (mem->has_error) return val_void();
                        StrSlice s = val_get_str(mem, sv);
                        mem_out_append(mem, mem_str_ptr(mem, s), s.len);
                    }
                    last_v = v;
                    idx++;
                    cur = link->right;
                }
                if (add_nl) mem_out_append(mem, "\n", 1u);
                return last_v;
            }
            if (n->sub == 248u) {
                /* assert(cond[, msg]) — lazy evaluation of msg */
                const AstNode *l1 = mem_get_ast(mem, n->left);
                Value cond_v = eval_ast(mem, tab, l1->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cond_v)) > 1e-15) {
                    return val_make_f64(mem, mem->fast_real_type, 1.0);
                }
                if (l1->right != MEM_NO_IDX) {
                    const AstNode *l2 = mem_get_ast(mem, l1->right);
                    Value msg_v = eval_ast(mem, tab, l2->left, frame_base, cur_ret_type);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    char mbuf[120];
                    if (val_as_cstr(mem, msg_v, mbuf, sizeof(mbuf)) && mbuf[0] != '\0') {
                        mem_err(mem, "%s", mbuf);
                    } else {
                        mem_err(mem, "Assertion failed");
                    }
                } else {
                    mem_err(mem, "Assertion failed");
                }
                return val_void();
            }
            if (n->sub == 249u) {
                /* clock_ms() */
                double ms = (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
                return val_make_f64(mem, mem->fast_real_type, ms);
            }
            if (n->sub == 250u || n->sub == 251u) {
                /* import "path.math" / import mod_name */
                StrSlice s = { n->left, n->right };
                Value imp_res = val_void();
                (void)ext_import_module_or_file(mem, tab, mem_str_ptr(mem, s), s.len,
                                                (n->sub == 250u), &imp_res);
                return imp_res;
            }
            if (n->sub == 252u) {
                /* bc_steps() */
                return val_make_f64(mem, mem->fast_real_type, (double)mem->bc_exec_steps);
            }
            if (n->sub == 253u) {
                /* bc_disasm(fn_name) */
                Value nv = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                char fnbuf[64];
                if (!val_as_cstr(mem, nv, fnbuf, sizeof(fnbuf)) || fnbuf[0] == '\0') {
                    mem_err(mem, "bc_disasm() expects a function name string");
                    return val_void();
                }
                char dbuf[1024];
                (void)bc_disasm_func(mem, tab, fnbuf, (uint16_t)strlen(fnbuf), dbuf, sizeof(dbuf));
                if (mem->has_error) return val_void();
                return val_from_cstr(mem, dbuf);
            }
            Value a = (n->left  != MEM_NO_IDX) ? eval_ast(mem, tab, n->left,  frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value b = (n->right != MEM_NO_IDX) ? eval_ast(mem, tab, n->right, frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value c = (n->ref   != MEM_NO_IDX && n->ref != 0)
                      ? eval_ast(mem, tab, n->ref, frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            if (n->sub >= 238u && n->sub <= 241u) {
                return eval_file_io_op(mem, tab, n->sub, a, b);
            }
            if (n->sub == 242u) {
                return eval_json_parse_op(mem, tab, a);
            }
            if (n->sub == 243u) {
                return eval_json_stringify_op(mem, tab, a, b);
            }
            return delegate_eval_intrinsic(mem, tab, (IntrinsicId)n->sub, a, b, c, cur_ret_type);
        }

        case AST_EXTERN: {
            Value args[16];
            uint8_t argc = 0;
            uint16_t cur = n->left;
            while (cur != MEM_NO_IDX && argc < 16) {
                const AstNode *link = mem_get_ast(mem, cur);
                args[argc++] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                cur = link->right;
            }
            const ExternDef *ext = sym_find_extern(tab, mem, n->ref);
            if (!ext || !ext->fn) {
                StrSlice es = tab->syms[n->ref];
                const char *ep = mem_str_ptr(mem, es);
                Value a0 = (argc >= 1) ? args[0] : val_void();
                Value a1 = (argc >= 2) ? args[1] : val_void();
                if (es.len == 9 && memcmp(ep, "file_read", 9) == 0) {
                    return eval_file_io_op(mem, tab, 238u, a0, a1);
                }
                if (es.len == 10 && memcmp(ep, "file_write", 10) == 0) {
                    return eval_file_io_op(mem, tab, 239u, a0, a1);
                }
                if (es.len == 11 && memcmp(ep, "file_append", 11) == 0) {
                    return eval_file_io_op(mem, tab, 240u, a0, a1);
                }
                if (es.len == 11 && memcmp(ep, "file_exists", 11) == 0) {
                    return eval_file_io_op(mem, tab, 241u, a0, a1);
                }
                if (es.len == 10 && memcmp(ep, "json_parse", 10) == 0) {
                    return eval_json_parse_op(mem, tab, a0);
                }
                if (es.len == 14 && memcmp(ep, "json_stringify", 14) == 0) {
                    return eval_json_stringify_op(mem, tab, a0, a1);
                }
                /* No host implementation registered yet: degrade to a loud
                 * STRING result instead of failing the whole expression, so
                 * extension patches (currency, location services, etc.) stay
                 * evaluable while the host app implements the extern. */
                StrSlice s = tab->syms[n->ref];
                char name[40];
                snprintf(name, sizeof(name), "%.*s", (int)s.len,
                         mem_str_ptr(mem, s));
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "unavailable: host extern '@%s' not implemented yet",
                         name);
                Value out = val_from_cstr(mem, msg);
                if (!mem->has_error) return out;
                mem_err(mem, "Unregistered host extern '@%.*s'",
                        (int)s.len, mem_str_ptr(mem, s));
                return val_void();
            }
            Value out = val_void();
            if (!ext->fn(mem, tab, args, argc, ext->user_ctx, &out)) {
                if (!mem->has_error) {
                    StrSlice s = tab->syms[n->ref];
                    mem_err(mem, "Host extern '@%.*s' failed",
                            (int)s.len, mem_str_ptr(mem, s));
                }
                return val_void();
            }
            return out;
        }

        case AST_CONSTRUCT: {
            uint16_t st_tid = n->ref;
            const TypeDef *td = &tab->types[st_tid];
            if ((n->sub & 0x80u) != 0 && !td->is_consuming && td->parser_intr == INTR_PARSE_I64) {
                uint8_t v_tag = (uint8_t)((n->sub >> 4u) & 0x07u);
                uint8_t pcnt  = (uint8_t)(n->sub & 0x0Fu);
                uint16_t pidx = mem_alloc_pool(mem, td->flat_size);
                if (mem->has_error) return val_void();
                for (uint16_t k = 0; k < td->flat_size; ++k) {
                    mem_write_pool(mem, (uint16_t)(pidx + k), 0.0);
                }
                mem_write_pool(mem, pidx, (double)v_tag);

                /* Locate variant descriptor in tab->fields */
                uint16_t vfd_i = td->start_idx;
                uint8_t  scan  = 0;
                while (scan < td->field_count) {
                    const FieldDef *fd = &tab->fields[td->start_idx + scan];
                    if (fd->type_id == st_tid) {
                        uint8_t ctag = (uint8_t)(fd->flat_offset & 0xFFu);
                        uint8_t ccnt = (uint8_t)(fd->flat_offset >> 8u);
                        if (ctag == v_tag) {
                            vfd_i = (uint16_t)(td->start_idx + scan);
                            break;
                        }
                        scan = (uint8_t)(scan + 1u + ccnt);
                    } else {
                        scan++;
                    }
                }

                uint16_t cur_link = n->left;
                uint8_t  pi       = 0;
                while (cur_link != MEM_NO_IDX && pi < pcnt) {
                    const AstNode  *link = mem_get_ast(mem, cur_link);
                    const FieldDef *pfd  = &tab->fields[vfd_i + 1u + pi];
                    Value fval = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    uint16_t fval_tid = val_get_type(mem, fval);
                    if (fval_tid != pfd->type_id) {
                        char exp_t[32], got_t[32];
                        sym_type_name(tab, mem, pfd->type_id, exp_t, sizeof(exp_t));
                        sym_type_name(tab, mem, fval_tid, got_t, sizeof(got_t));
                        mem_err(mem, "Enum variant field '%.*s' expects %s, got %s",
                                (int)pfd->name.len, mem_str_ptr(mem, pfd->name), exp_t, got_t);
                        return val_void();
                    }
                    uint16_t dst_off = (uint16_t)(pidx + pfd->flat_offset);
                    if (!tab->types[fval_tid].is_consuming) {
                        uint16_t sub_sz  = tab->types[fval_tid].flat_size;
                        uint16_t sub_idx = val_get_pool_idx(fval);
                        for (uint16_t k = 0; k < sub_sz; ++k) {
                            mem_write_pool(mem, (uint16_t)(dst_off + k), mem_read_pool(mem, (uint16_t)(sub_idx + k)));
                        }
                    } else if (fval_tid == mem->str_type_id && mem->str_type_id != 0) {
                        StrSlice s = val_get_str(mem, fval);
                        mem_write_pool(mem, dst_off, (double)(((uint32_t)s.off << 16u) | (uint32_t)s.len));
                    } else {
                        mem_write_pool(mem, dst_off, val_get_f64(mem, fval));
                    }
                    cur_link = link->right;
                    pi++;
                }
                return val_make_comp(mem, st_tid, pidx, td->flat_size);
            }
            if (n->sub != td->field_count) {
                char st_name[32];
                sym_type_name(tab, mem, st_tid, st_name, sizeof(st_name));
                mem_err(mem, "Struct %s expects %u argument(s), got %u",
                        st_name, (unsigned)td->field_count, (unsigned)n->sub);
                return val_void();
            }
            uint16_t pidx = mem_alloc_pool(mem, td->flat_size);
            if (mem->has_error) return val_void();

            uint16_t cur_link = n->left;
            uint16_t slot_cur = 0;
            uint8_t  f_idx    = 0;
            uint8_t  comp_unit = MEM_UNIT_NONE;
            while (cur_link != MEM_NO_IDX && f_idx < td->field_count && slot_cur < td->flat_size) {
                const AstNode  *link = mem_get_ast(mem, cur_link);
                const FieldDef *fd   = &tab->fields[td->start_idx + f_idx];
                Value fval = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

                uint16_t fval_tid = val_get_type(mem, fval);
                if (fval_tid != fd->type_id) {
                    char st_name[32], exp_t[32], got_t[32];
                    sym_type_name(tab, mem, st_tid, st_name, sizeof(st_name));
                    sym_type_name(tab, mem, fd->type_id, exp_t, sizeof(exp_t));
                    sym_type_name(tab, mem, fval_tid, got_t, sizeof(got_t));
                    mem_err(mem, "Struct %s field %.*s expects %s, got %s",
                            st_name, (int)fd->name.len, mem_str_ptr(mem, fd->name), exp_t, got_t);
                    return val_void();
                }

                uint8_t fu = val_get_unit(mem, fval);
                if (fu != MEM_UNIT_NONE && comp_unit == MEM_UNIT_NONE) {
                    comp_unit = fu;
                }

                if (fval_tid != 0 && fval_tid < tab->type_count && !tab->types[fval_tid].is_consuming) {
                    uint16_t sub_sz  = tab->types[fval_tid].flat_size;
                    uint16_t sub_idx = val_get_pool_idx(fval);
                    for (uint16_t k = 0; k < sub_sz && slot_cur < td->flat_size; ++k) {
                        mem_write_pool(mem, pidx + slot_cur++, mem_read_pool(mem, sub_idx + k));
                    }
                } else if (fval_tid == mem->str_type_id && mem->str_type_id != 0) {
                    StrSlice s = val_get_str(mem, fval);
                    mem_write_pool(mem, pidx + slot_cur++,
                                   (double)(((uint32_t)s.off << 16u) | (uint32_t)s.len));
                } else {
                    mem_write_pool(mem, pidx + slot_cur++, val_get_f64(mem, fval));
                }
                cur_link = link->right;
                f_idx++;
            }
            return val_with_unit(mem, val_make_comp(mem, st_tid, pidx, td->flat_size), comp_unit);
        }

        case AST_MAT_LIT:
            return eval_mat_lit_ast(mem, tab, n, frame_base, cur_ret_type);

        case AST_INDEX:
            return eval_index_op(mem, tab, n, frame_base, cur_ret_type, false, val_void());

        case AST_FOR_IN:
            return hardcode_eval_for_in_ast(mem, tab, n, frame_base, cur_ret_type);

        case AST_UNPACK_ELEM:
            return hardcode_eval_unpack_elem_ast(mem, tab, n, frame_base);

        default:
            mem_err(mem, "Corrupt AST node kind %u", (unsigned)n->kind);
            return val_void();
    }
}


/* ============================================================================
 * 5. PHASE 6+: STACK BYTECODE COMPILER, RANGE/CALL VM & DISASSEMBLER
 * ============================================================================ */

static bool ast_seq_has_label_or_goto(const MemArena *mem, const SymTable *tab, uint16_t idx) {
    if (idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_TAGGED_SCOPE || n->kind == AST_JUMP || n->kind == AST_RUN_TAG) {
        return true;
    }
    if (n->kind == AST_BINARY &&
        sym_active_meta_tag(tab, n->ref, (OpFixity)n->sub) == INTR_SEQ) {
        return ast_seq_has_label_or_goto(mem, tab, n->left) ||
               ast_seq_has_label_or_goto(mem, tab, n->right);
    }
    return false;
}

static uint16_t bc_emit(MemArena *mem, BcOp op, uint8_t a, uint16_t b,
                        uint16_t imm16, uint16_t aux16, bool *out_ok) {
    if (!*out_ok || mem->bc_count >= MEM_MAX_BC_INSTS) {
        *out_ok = false;
        return 0;
    }
    uint16_t idx = mem->bc_count++;
    BcInst *inst = &mem->bc_code[idx];
    inst->op    = (uint8_t)op;
    inst->a     = a;
    inst->b     = b;
    inst->imm16 = imm16;
    inst->aux16 = aux16;
    return idx;
}

static BcOp bc_classify_bin_op(const MemArena *mem, const SymTable *tab,
                               uint16_t sym_id, OpFixity fix) {
    if (sym_id >= tab->sym_count) return BC_NOP;
    if (hardcode_is_in_op_sym(mem, tab, sym_id)) return BC_NOP;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != (uint8_t)fix || ov->param_count != 2 ||
            !sym_mod_enabled(tab, ov->mod_id) || ov->body_ast == MEM_NO_IDX) continue;
        if (ov->param_types[0] != mem->fast_real_type ||
            ov->param_types[1] != mem->fast_real_type) continue;
        const AstNode *rn = mem_get_ast(mem, ov->body_ast);
        if (rn->kind == AST_INTRINSIC) {
            switch ((IntrinsicId)rn->sub) {
                case INTR_ADD: return BC_ADD;
                case INTR_SUB: return BC_SUB;
                case INTR_MUL: return BC_MUL;
                case INTR_DIV: return BC_DIV;
                case INTR_MOD: return BC_MOD;
                case INTR_POW: return BC_POW;
                case INTR_EQ:  return BC_EQ;
                case INTR_NE:  return BC_NE;
                case INTR_LT:  return BC_LT;
                case INTR_LE:  return BC_LE;
                case INTR_GT:  return BC_GT;
                case INTR_GE:  return BC_GE;
                default: break;
            }
        }
    }
    return BC_NOP;
}

static BcOp bc_classify_unary_op(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return BC_NOP;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != (uint8_t)FIX_PREFIX || ov->param_count != 1 ||
            !sym_mod_enabled(tab, ov->mod_id) || ov->body_ast == MEM_NO_IDX) continue;
        if (ov->param_types[0] != mem->fast_real_type) continue;
        const AstNode *rn = mem_get_ast(mem, ov->body_ast);
        if (rn->kind == AST_INTRINSIC) {
            if (rn->sub == INTR_NEG)  return BC_NEG;
            if (rn->sub == INTR_CNOT) return BC_NOT;
        }
    }
    return BC_NOP;
}

/* Detect single-segment range `lo...hi` / `lo..<hi` / `lo>..hi` / `lo>..<hi` [by step] */
static bool bc_extract_single_range(const MemArena *mem, const SymTable *tab,
                                    uint16_t iter_ast, uint16_t *out_lo, uint16_t *out_hi,
                                    uint16_t *out_step, uint8_t *out_mode) {
    if (iter_ast == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, iter_ast);
    if (n->kind != AST_BINARY) return false;

    uint16_t step_ast = MEM_NO_IDX;
    /* Check if outer operator is `by` (`INTR_VRANGE_STEP`) */
    if (n->ref < tab->sym_count) {
        for (uint16_t i = tab->sym_first_ov[n->ref]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
            const OpOverload *ov = &tab->overloads[i];
            if (ov->fixity == n->sub && ov->param_count == 2 &&
                sym_mod_enabled(tab, ov->mod_id) && ov->body_ast != MEM_NO_IDX) {
                const AstNode *rn = mem_get_ast(mem, ov->body_ast);
                if (rn->kind == AST_INTRINSIC && rn->sub == INTR_VRANGE_STEP) {
                    step_ast = n->right;
                    if (n->left == MEM_NO_IDX) return false;
                    n = mem_get_ast(mem, n->left);
                    if (n->kind != AST_BINARY) return false;
                    break;
                }
            }
        }
    }

    if (n->ref >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[n->ref]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == n->sub && ov->param_count == 2 &&
            sym_mod_enabled(tab, ov->mod_id) && ov->body_ast != MEM_NO_IDX) {
            const AstNode *rn = mem_get_ast(mem, ov->body_ast);
            if (rn->kind == AST_INTRINSIC && rn->sub == INTR_VRANGE &&
                rn->left != MEM_NO_IDX && rn->right != MEM_NO_IDX &&
                mem_get_ast(mem, rn->left)->kind == AST_SLOT_LOAD &&
                mem_get_ast(mem, rn->right)->kind == AST_SLOT_LOAD) {
                uint8_t mode = 0;
                if (rn->ref != MEM_NO_IDX && mem_get_ast(mem, rn->ref)->kind == AST_LIT) {
                    mode = (uint8_t)mem_read_pool(mem, mem_get_ast(mem, rn->ref)->ref);
                }
                *out_lo   = n->left;
                *out_hi   = n->right;
                *out_step = step_ast;
                *out_mode = mode;
                return true;
            }
        }
    }
    return false;
}

static void bc_compile_node(MemArena *mem, const SymTable *tab, uint16_t node_idx, bool *ok) {
    if (!*ok || node_idx == MEM_NO_IDX) return;
    const AstNode *n = mem_get_ast(mem, node_idx);

    switch ((AstKind)n->kind) {
        case AST_LIT:
            if (n->sub != mem->str_type_id && n->sub != MEM_TYPE_VOID) {
                (void)bc_emit(mem, BC_PUSH_F64, n->sub, 0, n->ref, 0, ok);
            } else {
                (void)bc_emit(mem, BC_PUSH_LIT, 0, 0, node_idx, 0, ok);
            }
            return;

        case AST_SLOT_LOAD:
            if ((n->ref & 0x4000u) == 0) {
                uint8_t s = (uint8_t)(n->sub & AST_SLOT_IDX_MASK);
                (void)bc_emit(mem, BC_LOAD_SLOT, s, 0, 0, 0, ok);
            } else {
                (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            }
            return;

        case AST_UNARY:
            if ((OpFixity)n->sub == FIX_PREFIX) {
                BcOp uop = bc_classify_unary_op(mem, tab, n->ref);
                if (uop != BC_NOP) {
                    bc_compile_node(mem, tab, n->left, ok);
                    (void)bc_emit(mem, uop, 0, n->ref, 0, node_idx, ok);
                    return;
                }
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;

        case AST_BINARY: {
            uint8_t mtag = sym_active_meta_tag(tab, n->ref, (OpFixity)n->sub);
            if (mtag == INTR_SEQ) {
                if (!ast_seq_has_label_or_goto(mem, tab, node_idx)) {
                    bc_compile_node(mem, tab, n->left, ok);
                    (void)bc_emit(mem, BC_POP, 0, 0, 0, 0, ok);
                    bc_compile_node(mem, tab, n->right, ok);
                    return;
                }
                (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
                return;
            }
            if (mtag == INTR_STORE) {
                const AstNode *lhs = mem_get_ast(mem, n->left);
                const AstNode *rn  = mem_get_ast(mem, n->right);
                bool rhs_is_seq_jump = rn &&
                    ((rn->kind == AST_BINARY &&
                      sym_active_meta_tag(tab, rn->ref, (OpFixity)rn->sub) == INTR_SEQ) ||
                     rn->kind == AST_RETURN || rn->kind == AST_BREAK);
                if (!rhs_is_seq_jump &&
                    lhs->kind == AST_SLOT_LOAD &&
                    (lhs->sub & AST_SLOT_CONST_DECL_BIT) == 0 &&
                    lhs->left == 0 && lhs->right == 0) {
                    uint8_t s = (uint8_t)(lhs->sub & AST_SLOT_IDX_MASK);
                    bc_compile_node(mem, tab, n->right, ok);
                    (void)bc_emit(mem, BC_STORE_SLOT, s, n->ref, n->right, node_idx, ok);
                    return;
                }
                (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
                return;
            }
            if (mtag == INTR_WHILE) {
                (void)bc_emit(mem, BC_PUSH_F64, mem->fast_real_type, 0, MEM_NO_IDX, 0, ok);
                uint16_t enter_i = bc_emit(mem, BC_LOOP_GUARD, 1u, 0, 0, 0, ok);
                uint16_t cond_pc = mem->bc_count;
                bc_compile_node(mem, tab, n->left, ok);
                uint16_t jout_i  = bc_emit(mem, BC_JMP_FALSE, 0, 0, 0, 0, ok);
                bc_compile_node(mem, tab, n->right, ok);
                (void)bc_emit(mem, BC_LOOP_GUARD, 0u, 0, cond_pc, 0, ok);
                (void)bc_emit(mem, BC_JMP, 0, 0, cond_pc, 0, ok);
                uint16_t exit_pc = mem->bc_count;
                (void)bc_emit(mem, BC_LOOP_GUARD, 2u, 0, 0, 0, ok);
                if (*ok) {
                    mem->bc_code[jout_i].imm16  = exit_pc;
                    mem->bc_code[enter_i].imm16 = cond_pc;
                    mem->bc_code[enter_i].aux16 = (uint16_t)(exit_pc + 1u);
                }
                return;
            }
            if (mtag == INTR_DO_WHILE) {
                (void)bc_emit(mem, BC_PUSH_F64, mem->fast_real_type, 0, MEM_NO_IDX, 0, ok);
                uint16_t enter_i = bc_emit(mem, BC_LOOP_GUARD, 1u, 0, 0, 0, ok);
                uint16_t body_pc = mem->bc_count;
                bc_compile_node(mem, tab, n->left, ok);
                uint16_t cond_pc = mem->bc_count;
                (void)bc_emit(mem, BC_LOOP_GUARD, 0u, 0, cond_pc, 0, ok);
                bc_compile_node(mem, tab, n->right, ok);
                (void)bc_emit(mem, BC_JMP_TRUE, 0, 0, body_pc, 0, ok);
                uint16_t exit_pc = mem->bc_count;
                (void)bc_emit(mem, BC_LOOP_GUARD, 2u, 0, 0, 0, ok);
                if (*ok) {
                    mem->bc_code[enter_i].imm16 = cond_pc;
                    mem->bc_code[enter_i].aux16 = (uint16_t)(exit_pc + 1u);
                }
                return;
            }
            if (mtag == INTR_CAND) {
                bc_compile_node(mem, tab, n->left, ok);
                uint16_t jfalse = bc_emit(mem, BC_JMP_FALSE, 0, 0, 0, 0, ok);
                bc_compile_node(mem, tab, n->right, ok);
                uint16_t jtrue  = bc_emit(mem, BC_JMP_TRUE, 0, 0, 0, 0, ok);
                uint16_t f_pc   = mem->bc_count;
                (void)bc_emit(mem, BC_PUSH_F64, mem->fast_real_type, 0, MEM_NO_IDX, 0, ok);
                uint16_t jend   = bc_emit(mem, BC_JMP, 0, 0, 0, 0, ok);
                uint16_t t_pc   = mem->bc_count;
                (void)bc_emit(mem, BC_PUSH_F64, mem->fast_real_type, 0, 0xFFFEu, 0, ok);
                uint16_t end_pc = mem->bc_count;
                if (*ok) {
                    mem->bc_code[jfalse].imm16 = f_pc;
                    mem->bc_code[jtrue].imm16  = t_pc;
                    mem->bc_code[jend].imm16   = end_pc;
                }
                return;
            }
            if (mtag == INTR_COR) {
                bc_compile_node(mem, tab, n->left, ok);
                uint16_t jtrue1 = bc_emit(mem, BC_JMP_TRUE, 0, 0, 0, 0, ok);
                bc_compile_node(mem, tab, n->right, ok);
                uint16_t jtrue2 = bc_emit(mem, BC_JMP_TRUE, 0, 0, 0, 0, ok);
                (void)bc_emit(mem, BC_PUSH_F64, mem->fast_real_type, 0, MEM_NO_IDX, 0, ok);
                uint16_t jend   = bc_emit(mem, BC_JMP, 0, 0, 0, 0, ok);
                uint16_t t_pc   = mem->bc_count;
                (void)bc_emit(mem, BC_PUSH_F64, mem->fast_real_type, 0, 0xFFFEu, 0, ok);
                uint16_t end_pc = mem->bc_count;
                if (*ok) {
                    mem->bc_code[jtrue1].imm16 = t_pc;
                    mem->bc_code[jtrue2].imm16 = t_pc;
                    mem->bc_code[jend].imm16   = end_pc;
                }
                return;
            }
            BcOp bop = bc_classify_bin_op(mem, tab, n->ref, (OpFixity)n->sub);
            if (bop != BC_NOP) {
                bc_compile_node(mem, tab, n->left, ok);
                bc_compile_node(mem, tab, n->right, ok);
                (void)bc_emit(mem, bop, (uint8_t)n->sub, n->ref, 0, node_idx, ok);
                return;
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;
        }

        case AST_FOR_IN: {
            bool is_comp      = (n->ref & 0x8000u) != 0;
            uint16_t raw_lbl  = (uint16_t)(n->ref & 0x00FFu);
            uint16_t lo_ast   = MEM_NO_IDX, hi_ast = MEM_NO_IDX, step_ast = MEM_NO_IDX;
            uint8_t  r_mode   = 0;
            if (!is_comp && raw_lbl == 0 &&
                bc_extract_single_range(mem, tab, n->left, &lo_ast, &hi_ast, &step_ast, &r_mode)) {
                bc_compile_node(mem, tab, lo_ast, ok);
                bc_compile_node(mem, tab, hi_ast, ok);
                if (step_ast != MEM_NO_IDX) {
                    bc_compile_node(mem, tab, step_ast, ok);
                } else {
                    (void)bc_emit(mem, BC_PUSH_LIT, 0, 0, MEM_NO_IDX, 0, ok);
                }
                uint16_t packed_meta = (uint16_t)((n->ref & 0x7F00u) | (r_mode & 0x0Fu));
                uint16_t init_i  = bc_emit(mem, BC_FOR_INIT, n->sub, packed_meta, 0, 0, ok);
                uint16_t body_pc = mem->bc_count;
                bc_compile_node(mem, tab, n->right, ok);
                uint16_t step_pc = mem->bc_count;
                uint16_t step_i  = bc_emit(mem, BC_FOR_STEP, n->sub, 0, body_pc, 0, ok);
                uint16_t exit_pc = mem->bc_count;
                if (*ok) {
                    mem->bc_code[init_i].imm16 = step_pc;
                    mem->bc_code[init_i].aux16 = exit_pc;
                    mem->bc_code[step_i].aux16 = exit_pc;
                }
                return;
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;
        }

        case AST_CALL: {
            uint8_t mtag = sym_active_meta_tag(tab, n->ref, FIX_FUNC);
            if (!delegate_is_sym_meta(mtag) && mtag != INTR_IF &&
                n->right == MEM_NO_IDX &&
                !hardcode_is_vec_ctor_sym(mem, tab, n->ref) &&
                !hardcode_is_map_ctor_sym(mem, tab, n->ref) &&
                n->sub <= 8u) {
                uint16_t cur = n->left;
                uint8_t argc = 0;
                while (cur != MEM_NO_IDX && argc < 8u) {
                    const AstNode *link = mem_get_ast(mem, cur);
                    bc_compile_node(mem, tab, link->left, ok);
                    argc++;
                    cur = link->right;
                }
                uint16_t filter_mod = (n->right != MEM_NO_IDX && (n->right & AST_CALL_QUAL_MOD_BIT) != 0)
                                      ? (uint16_t)(n->right & 0x3FFFu) : MEM_NO_IDX;
                (void)bc_emit(mem, BC_CALL, argc, n->ref, filter_mod, node_idx, ok);
                return;
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;
        }

        case AST_FN_CALL: {
            const AstNode *fn_node = mem_get_ast(mem, n->left);
            bool fn_is_seq_jump = fn_node &&
                ((fn_node->kind == AST_BINARY &&
                  sym_active_meta_tag(tab, fn_node->ref, (OpFixity)fn_node->sub) == INTR_SEQ) ||
                 fn_node->kind == AST_RETURN || fn_node->kind == AST_BREAK);
            if (!fn_is_seq_jump && n->sub <= 8u) {
                bc_compile_node(mem, tab, n->left, ok);
                uint16_t cur = n->right;
                uint8_t argc = 0;
                while (cur != MEM_NO_IDX && argc < 8u) {
                    const AstNode *link = mem_get_ast(mem, cur);
                    bc_compile_node(mem, tab, link->left, ok);
                    argc++;
                    cur = link->right;
                }
                (void)bc_emit(mem, BC_FN_CALL, argc, 0, 0, node_idx, ok);
                return;
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;
        }

        case AST_INTRINSIC: {
            if (n->sub < 246u) {
                uint16_t mask = 0;
                if (n->left != MEM_NO_IDX) {
                    bc_compile_node(mem, tab, n->left, ok);
                    mask |= 1u;
                }
                if (n->right != MEM_NO_IDX) {
                    bc_compile_node(mem, tab, n->right, ok);
                    mask |= 2u;
                }
                if (n->ref != MEM_NO_IDX && n->ref != 0) {
                    bc_compile_node(mem, tab, n->ref, ok);
                    mask |= 4u;
                }
                (void)bc_emit(mem, BC_INTRINSIC, n->sub, mask, 0, node_idx, ok);
                return;
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;
        }

        case AST_TERNARY:
            if (n->sub == 0 && n->right != MEM_NO_IDX) {
                const AstNode *pair = mem_get_ast(mem, n->right);
                bc_compile_node(mem, tab, n->left, ok);
                uint16_t jelse = bc_emit(mem, BC_JMP_FALSE, 0, 0, 0, 0, ok);
                bc_compile_node(mem, tab, pair->left, ok);
                uint16_t jend  = bc_emit(mem, BC_JMP, 0, 0, 0, 0, ok);
                uint16_t else_pc = mem->bc_count;
                if (pair->right != MEM_NO_IDX) {
                    bc_compile_node(mem, tab, pair->right, ok);
                } else {
                    (void)bc_emit(mem, BC_PUSH_LIT, 0, 0, MEM_NO_IDX, 0, ok);
                }
                uint16_t end_pc = mem->bc_count;
                if (*ok) {
                    mem->bc_code[jelse].imm16 = else_pc;
                    mem->bc_code[jend].imm16  = end_pc;
                }
                return;
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;

        case AST_RETURN:
            if (n->sub == 0) {
                if (n->left != MEM_NO_IDX) {
                    bc_compile_node(mem, tab, n->left, ok);
                } else {
                    (void)bc_emit(mem, BC_PUSH_F64, mem->fast_real_type, 0, MEM_NO_IDX, 0, ok);
                }
                (void)bc_emit(mem, BC_RET, 0, 0, 0, 0, ok);
                return;
            }
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;

        default:
            (void)bc_emit(mem, BC_EVAL_AST, 0, 0, node_idx, 0, ok);
            return;
    }
}

uint16_t bc_compile_ast(MemArena *mem, const SymTable *tab, uint16_t root_ast, bool is_perm) {
    if (!mem || !tab || root_ast == MEM_NO_IDX) return MEM_NO_IDX;
    uint16_t start_pc = mem->bc_count;
    bool ok = true;
    bc_compile_node(mem, tab, root_ast, &ok);
    (void)bc_emit(mem, BC_HALT, 0, 0, 0, 0, &ok);
    if (!ok) {
        mem->bc_count = start_pc;
        return MEM_NO_IDX;
    }
    if (is_perm && mem->bc_count > mem->perm_bc_count) {
        mem->perm_bc_count = mem->bc_count;
    }
    return start_pc;
}

static inline bool bc_val_is_plain_real(const MemArena *mem, Value v, double *out_d) {
    if ((v >> 48u) < 0xFFF8u) {
        memcpy(out_d, &v, sizeof(double));
        return true;
    }
    if (val_get_type(mem, v) == mem->fast_real_type &&
        val_get_unit(mem, v) == MEM_UNIT_NONE &&
        mem->fast_real_type != 0) {
        *out_d = val_get_f64(mem, v);
        return true;
    }
    return false;
}

static inline Value bc_double_to_val(MemArena *mem, double d) {
    if (d != d) {
        uint64_t qnan = 0x7FF8000000000000ULL;
        return qnan;
    }
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    if ((u & 0xFFF8000000000000ULL) == 0xFFF8000000000000ULL) {
        return val_make_f64(mem, mem->fast_real_type, d);
    }
    return u;
}

typedef struct {
    uint16_t  cond_pc;
    uint16_t  exit_pc;
    uint8_t   base_vsp;
    uint8_t   is_for_range;
    uint8_t   var_slot;
    uint8_t   idx_slot;
    uint8_t   has_idx;
    uint8_t   unit_id;
    uint32_t  guard;
    long long iter_i;
    long long cnt_ll;
    long long start_k;
    double    da;
    double    step;
} BcLoopFrame;

Value bc_exec(MemArena *mem, SymTable *tab, uint16_t start_pc,
              uint16_t frame_base, uint8_t cur_ret_type) {
    if (mem->has_error || mem->jump_active != JUMP_NONE || start_pc >= mem->bc_count) {
        return val_void();
    }
    if (mem->eval_depth >= MEM_MAX_EVAL_DEPTH) {
        mem_err(mem, "Evaluation recursion depth limit exceeded (max %d)", MEM_MAX_EVAL_DEPTH);
        return val_void();
    }
    mem->eval_depth++;

    Value       vstack[64];
    uint8_t     vsp = 0;
    BcLoopFrame loops[8];
    uint8_t     loop_depth = 0;
    uint16_t    pc = start_pc;

    while (pc < mem->bc_count && !mem->has_error) {
        if (++mem->eval_steps > MEM_MAX_EVAL_STEPS) {
            mem_err(mem, "Evaluation step budget exceeded (max %u steps)", (unsigned)MEM_MAX_EVAL_STEPS);
            break;
        }
        mem->bc_exec_steps++;
        const BcInst *inst = &mem->bc_code[pc++];

        switch ((BcOp)inst->op) {
            case BC_NOP:
                break;

            case BC_PUSH_F64: {
                double d = 0.0;
                if (inst->imm16 == 0xFFFEu) d = 1.0;
                else if (inst->imm16 != MEM_NO_IDX) d = mem_read_pool(mem, inst->imm16);
                if (vsp < 64u) {
                    vstack[vsp++] = (inst->a == mem->fast_real_type)
                                    ? bc_double_to_val(mem, d)
                                    : val_make_f64(mem, inst->a, d);
                }
                break;
            }

            case BC_PUSH_LIT: {
                Value v = (inst->imm16 != MEM_NO_IDX)
                          ? eval_ast(mem, tab, inst->imm16, frame_base, cur_ret_type)
                          : val_void();
                if (vsp < 64u) vstack[vsp++] = v;
                break;
            }

            case BC_LOAD_SLOT: {
                uint16_t abs_s = (uint16_t)(frame_base + inst->a);
                Value v = (abs_s < MEM_MAX_CALL_STACK) ? mem->stack[abs_s] : val_void();
                if (vsp < 64u) vstack[vsp++] = v;
                break;
            }

            case BC_STORE_SLOT: {
                if (vsp == 0) break;
                Value rhs = vstack[vsp - 1];
                uint8_t  slot_idx = inst->a;
                uint16_t abs_slot = (uint16_t)(frame_base + slot_idx);
                if (abs_slot < MEM_MAX_CALL_STACK &&
                    mem->slot_locked_type[abs_slot] == 0 &&
                    mem->slot_locked_unit[abs_slot] == 0 &&
                    mem->slot_is_const[abs_slot] == 0 &&
                    mem->heap_active_count == 0) {
                    mem->stack[abs_slot] = rhs;
                } else {
                    if (abs_slot >= MEM_MAX_CALL_STACK) {
                        mem_err(mem, "Stack slot index %u out of bounds", (unsigned)abs_slot);
                        break;
                    }
                    if (mem->slot_is_const[abs_slot] != 0) {
                        mem_err(mem, "Cannot reassign or mutate immutable constant");
                        break;
                    }
                    uint8_t req_t = mem->slot_locked_type[abs_slot];
                    if (req_t != 0 && req_t != SYM_TYPE_ANY && val_get_type(mem, rhs) != req_t) {
                        char exp_t[32], got_t[32];
                        sym_type_name(tab, mem, req_t, exp_t, sizeof(exp_t));
                        sym_type_name(tab, mem, val_get_type(mem, rhs), got_t, sizeof(got_t));
                        mem_err(mem, "Type mismatch: variable holds concrete type '%s', cannot assign '%s'",
                                exp_t, got_t);
                        break;
                    }
                    uint8_t req_u = mem->slot_locked_unit[abs_slot];
                    if (req_u != 0 && req_u != SYM_UNIT_GENERIC && req_u != SYM_UNIT_OMITTED) {
                        uint8_t exp_u = (req_u == SYM_UNIT_LOCK_NONE) ? MEM_UNIT_NONE : req_u;
                        if (val_get_unit(mem, rhs) != exp_u) {
                            char eu[32], gu[32];
                            sym_unit_name(tab, mem, exp_u, eu, sizeof(eu));
                            sym_unit_name(tab, mem, val_get_unit(mem, rhs), gu, sizeof(gu));
                            mem_err(mem, "Unit mismatch: variable holds concrete unit '%s', cannot assign '%s'",
                                    eu, gu);
                            break;
                        }
                    }
                    mem->stack[abs_slot] = rhs;
                    if (mem->heap_active_count > 0) {
                        bool is_colon_eq = false;
                        (void)hardcode_is_plain_store_sym(mem, tab, inst->b, &is_colon_eq);
                        uint16_t self_hdr = (mem->fn_type_id != 0 && val_get_type(mem, rhs) == mem->fn_type_id)
                                            ? val_get_pool_idx(rhs) : MEM_NO_IDX;
                        for (uint16_t hid = 0; hid < MEM_MAX_HEAP_BLOCKS; ++hid) {
                            HeapBlock *hb = &mem->heap_blocks[hid];
                            if (hb->in_use == 2u && hb->data != NULL && hb->cap_slots >= 3u) {
                                uint32_t meta    = (uint32_t)hb->data[hb->cap_slots - 1u];
                                uint16_t orig_fb = (uint16_t)(meta >> 8u);
                                uint8_t  s_start = (uint8_t)(meta & 0xFFu);
                                if (orig_fb == frame_base && slot_idx < s_start &&
                                    (!is_colon_eq || hb->owner_hdr == self_hdr)) {
                                    memcpy(&hb->data[slot_idx], &rhs, sizeof(Value));
                                    uint32_t lpack = (uint32_t)mem->slot_locked_type[abs_slot]
                                                   | ((uint32_t)mem->slot_locked_unit[abs_slot] << 8u)
                                                   | ((uint32_t)mem->slot_is_const[abs_slot] << 16u);
                                    hb->data[s_start + slot_idx] = (double)lpack;
                                }
                            }
                        }
                    }
                }
                break;
            }

            case BC_POP:
                if (vsp > 0) vsp--;
                break;

            case BC_NEG:
            case BC_NOT: {
                if (vsp == 0) break;
                Value va = vstack[vsp - 1];
                double da = 0.0;
                if (bc_val_is_plain_real(mem, va, &da)) {
                    if (inst->op == BC_NEG) {
                        vstack[vsp - 1] = bc_double_to_val(mem, -da);
                        break;
                    }
                    if (fabs(da) <= 1e-15) {
                        vstack[vsp - 1] = bc_double_to_val(mem, 1.0);
                        break;
                    }
                    if (fabs(da - 1.0) <= 1e-15) {
                        vstack[vsp - 1] = bc_double_to_val(mem, 0.0);
                        break;
                    }
                }
                vstack[vsp - 1] = eval_dispatch_overload(mem, tab, inst->b, FIX_PREFIX, 1, &va);
                break;
            }

            case BC_ADD:
            case BC_SUB:
            case BC_MUL:
            case BC_DIV:
            case BC_MOD:
            case BC_POW:
            case BC_EQ:
            case BC_NE:
            case BC_LT:
            case BC_LE:
            case BC_GT:
            case BC_GE: {
                if (vsp < 2u) break;
                Value vb = vstack[--vsp];
                Value va = vstack[vsp - 1];
                double da = 0.0, db = 0.0;
                if (bc_val_is_plain_real(mem, va, &da) && bc_val_is_plain_real(mem, vb, &db)) {
                    double r = 0.0;
                    bool handled = true;
                    switch ((BcOp)inst->op) {
                        case BC_ADD: r = da + db; break;
                        case BC_SUB: r = da - db; break;
                        case BC_MUL: r = da * db; break;
                        case BC_DIV:
                            if (db == 0.0) { mem_err(mem, "Division by zero"); handled = false; }
                            else r = da / db;
                            break;
                        case BC_MOD:
                            if (db == 0.0) { mem_err(mem, "Division by zero in modulo"); handled = false; }
                            else r = fmod(da, db);
                            break;
                        case BC_POW: r = pow(da, db); break;
                        case BC_LT:  r = (da <  db) ? 1.0 : 0.0; break;
                        case BC_LE:  r = (da <= db) ? 1.0 : 0.0; break;
                        case BC_GT:  r = (da >  db) ? 1.0 : 0.0; break;
                        case BC_GE:  r = (da >= db) ? 1.0 : 0.0; break;
                        case BC_EQ: {
                            if (isnan(da) || isnan(db)) r = 0.0;
                            else if (isinf(da) || isinf(db) || da == 0.0 || db == 0.0) r = (da == db) ? 1.0 : 0.0;
                            else r = (fabs(da - db) <= 1e-14 * fmax(fabs(da), fabs(db))) ? 1.0 : 0.0;
                            break;
                        }
                        case BC_NE: {
                            if (isnan(da) || isnan(db)) r = 1.0;
                            else if (isinf(da) || isinf(db) || da == 0.0 || db == 0.0) r = (da != db) ? 1.0 : 0.0;
                            else r = (fabs(da - db) > 1e-14 * fmax(fabs(da), fabs(db))) ? 1.0 : 0.0;
                            break;
                        }
                        default: handled = false; break;
                    }
                    if (mem->has_error) break;
                    if (handled) {
                        vstack[vsp - 1] = bc_double_to_val(mem, r);
                        break;
                    }
                }
                Value bargs[2] = { va, vb };
                vstack[vsp - 1] = eval_dispatch_overload(mem, tab, inst->b, (OpFixity)inst->a, 2, bargs);
                break;
            }

            case BC_JMP:
                pc = inst->imm16;
                break;

            case BC_JMP_FALSE: {
                Value c = (vsp > 0) ? vstack[--vsp] : val_void();
                if (fabs(val_get_f64(mem, c)) <= 1e-15) {
                    pc = inst->imm16;
                }
                break;
            }

            case BC_JMP_TRUE: {
                Value c = (vsp > 0) ? vstack[--vsp] : val_void();
                if (fabs(val_get_f64(mem, c)) > 1e-15) {
                    pc = inst->imm16;
                }
                break;
            }

            case BC_LOOP_GUARD: {
                if (inst->a == 1u) {
                    if (loop_depth < 8u) {
                        memset(&loops[loop_depth], 0, sizeof(loops[0]));
                        loops[loop_depth].cond_pc  = inst->imm16;
                        loops[loop_depth].exit_pc  = inst->aux16;
                        loops[loop_depth].base_vsp = vsp;
                        loop_depth++;
                    }
                } else if (inst->a == 2u) {
                    if (loop_depth > 0) loop_depth--;
                } else {
                    if (loop_depth > 0) {
                        uint8_t bvsp = loops[loop_depth - 1].base_vsp;
                        if (vsp > bvsp && bvsp > 0) {
                            vstack[bvsp - 1] = vstack[vsp - 1];
                            vsp = bvsp;
                        }
                        if (++loops[loop_depth - 1].guard > 1000000u) {
                            mem_err(mem, "Loop iteration limit exceeded");
                            break;
                        }
                    }
                    if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                        mem->heap_active_count >= 64u) {
                        (void)mem_compact_scratch(mem, vstack, vsp);
                    }
                }
                break;
            }

            case BC_FOR_INIT: {
                if (vsp < 3u) break;
                Value vs = vstack[--vsp];
                Value vb = vstack[--vsp];
                Value va = vstack[--vsp];
                vstack[vsp++] = bc_double_to_val(mem, 0.0); /* Initial `last` on stack */

                uint8_t  var_slot   = inst->a;
                bool     has_idx    = (inst->b & 0x4000u) != 0;
                uint8_t  idx_slot   = (uint8_t)((inst->b >> 8u) & 0x3Fu);
                int      vmode      = (int)(inst->b & 0x0Fu);
                uint8_t  u          = MEM_UNIT_NONE;
                if (!delegate_check_additive_unit_compat(mem, tab, "...", 3, va, vb, &u)) {
                    break;
                }
                double da   = val_get_f64(mem, va);
                double db   = val_get_f64(mem, vb);
                double step = (da <= db) ? 1.0 : -1.0;
                if (val_get_type(mem, vs) == MEM_TYPE_VOID) {
                    int lo = 0, hi = 0;
                    if (!delegate_val_to_exact_int(mem, tab, va, "range start", &lo) ||
                        !delegate_val_to_exact_int(mem, tab, vb, "range end",   &hi)) {
                        break;
                    }
                    da   = (double)lo;
                    db   = (double)hi;
                    step = (lo <= hi) ? 1.0 : -1.0;
                } else {
                    uint8_t us = val_get_unit(mem, vs);
                    if (us != MEM_UNIT_NONE && us != u) {
                        mem_err(mem, "Unit mismatch in stepped range 'by'");
                        break;
                    }
                    step = val_get_f64(mem, vs);
                    if (!isfinite(step) || fabs(step) <= 1e-15) {
                        mem_err(mem, "Range step cannot be zero");
                        break;
                    }
                }
                double    eps       = 1e-11 * (1.0 + fabs(da) + fabs(db) + fabs(step));
                long long start_k   = (vmode & 2) ? 1LL : 0LL;
                double    first_val = da + (double)start_k * step;
                long long cnt_ll    = 0;
                if (step > 0.0) {
                    bool ok_b = (vmode & 1) ? (first_val < db - eps) : (first_val <= db + eps);
                    if (ok_b) cnt_ll = (long long)floor(((db - first_val) + ((vmode & 1) ? -eps : eps)) / step) + 1LL;
                } else {
                    bool ok_b = (vmode & 1) ? (first_val > db + eps) : (first_val >= db - eps);
                    if (ok_b) cnt_ll = (long long)floor(((first_val - db) + ((vmode & 1) ? -eps : eps)) / (-step)) + 1LL;
                }
                if (cnt_ll <= 0 || loop_depth >= 8u) {
                    pc = inst->aux16;
                    break;
                }
                BcLoopFrame *lf  = &loops[loop_depth++];
                memset(lf, 0, sizeof(*lf));
                lf->cond_pc      = inst->imm16; /* step_pc for `continue` */
                lf->exit_pc      = inst->aux16;
                lf->base_vsp     = vsp;
                lf->is_for_range = 1u;
                lf->var_slot     = var_slot;
                lf->idx_slot     = idx_slot;
                lf->has_idx      = has_idx ? 1u : 0u;
                lf->unit_id      = u;
                lf->iter_i       = 0;
                lf->cnt_ll       = cnt_ll;
                lf->start_k      = start_k;
                lf->da           = da;
                lf->step         = step;

                double cur_v = da + (double)start_k * step;
                if (fabs(cur_v) < 1e-15) cur_v = 0.0;
                if (has_idx) {
                    mem->stack[frame_base + idx_slot] = bc_double_to_val(mem, 0.0);
                }
                mem->stack[frame_base + var_slot] = (u == MEM_UNIT_NONE)
                    ? bc_double_to_val(mem, cur_v)
                    : val_make_f64_unit(mem, mem->fast_real_type, u, cur_v);
                break;
            }

            case BC_FOR_STEP: {
                if (loop_depth == 0) break;
                BcLoopFrame *lf = &loops[loop_depth - 1];
                if (vsp > lf->base_vsp && lf->base_vsp > 0) {
                    vstack[lf->base_vsp - 1] = vstack[vsp - 1];
                    vsp = lf->base_vsp;
                }
                if (++lf->guard > 1000000u) {
                    mem_err(mem, "Loop iteration limit exceeded");
                    break;
                }
                if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                    mem->heap_active_count >= 64u) {
                    (void)mem_compact_scratch(mem, vstack, vsp);
                }
                lf->iter_i++;
                if (lf->iter_i >= lf->cnt_ll) {
                    loop_depth--;
                    pc = inst->aux16;
                    break;
                }
                double cur_v = lf->da + (double)(lf->start_k + lf->iter_i) * lf->step;
                if (fabs(cur_v) < 1e-15) cur_v = 0.0;
                if (lf->has_idx) {
                    mem->stack[frame_base + lf->idx_slot] = bc_double_to_val(mem, (double)lf->iter_i);
                }
                mem->stack[frame_base + lf->var_slot] = (lf->unit_id == MEM_UNIT_NONE)
                    ? bc_double_to_val(mem, cur_v)
                    : val_make_f64_unit(mem, mem->fast_real_type, lf->unit_id, cur_v);
                pc = inst->imm16;
                break;
            }

            case BC_CALL: {
                uint8_t argc = inst->a;
                if (vsp < argc) break;
                Value cargs[8];
                for (int k = (int)argc - 1; k >= 0; --k) {
                    cargs[k] = vstack[--vsp];
                }
                uint16_t saved_tr = mem->temp_root_count;
                for (uint8_t k = 0; k < vsp; ++k) {
                    (void)mem_push_temp_root(mem, &vstack[k]);
                }
                for (uint8_t k = 0; k < argc; ++k) {
                    (void)mem_push_temp_root(mem, &cargs[k]);
                }
                Value cres = eval_dispatch_overload_in_mod(mem, tab, inst->b, FIX_FUNC, argc, cargs, inst->imm16);
                mem_pop_temp_roots(mem, saved_tr);
                if (vsp < 64u) vstack[vsp++] = cres;
                break;
            }

            case BC_FN_CALL: {
                uint8_t argc = inst->a;
                if (vsp < (uint8_t)(argc + 1u)) break;
                Value cargs[8];
                for (int k = (int)argc - 1; k >= 0; --k) {
                    cargs[k] = vstack[--vsp];
                }
                Value fn_val = vstack[--vsp];
                uint16_t saved_tr = mem->temp_root_count;
                for (uint8_t k = 0; k < vsp; ++k) {
                    (void)mem_push_temp_root(mem, &vstack[k]);
                }
                (void)mem_push_temp_root(mem, &fn_val);
                for (uint8_t k = 0; k < argc; ++k) {
                    (void)mem_push_temp_root(mem, &cargs[k]);
                }
                Value cres = delegate_call_fn(mem, tab, fn_val, argc, cargs);
                mem_pop_temp_roots(mem, saved_tr);
                if (vsp < 64u) vstack[vsp++] = cres;
                break;
            }

            case BC_INTRINSIC: {
                uint16_t mask = inst->b;
                Value c = (mask & 4u) ? (vsp > 0 ? vstack[--vsp] : val_void()) : val_void();
                Value b = (mask & 2u) ? (vsp > 0 ? vstack[--vsp] : val_void()) : val_void();
                Value a = (mask & 1u) ? (vsp > 0 ? vstack[--vsp] : val_void()) : val_void();
                uint16_t saved_tr = mem->temp_root_count;
                for (uint8_t k = 0; k < vsp; ++k) {
                    (void)mem_push_temp_root(mem, &vstack[k]);
                }
                Value ires;
                if (inst->a >= 238u && inst->a <= 241u) {
                    ires = eval_file_io_op(mem, tab, inst->a, a, b);
                } else if (inst->a == 242u) {
                    ires = eval_json_parse_op(mem, tab, a);
                } else if (inst->a == 243u) {
                    ires = eval_json_stringify_op(mem, tab, a, b);
                } else {
                    ires = delegate_eval_intrinsic(mem, tab, (IntrinsicId)inst->a, a, b, c, cur_ret_type);
                }
                mem_pop_temp_roots(mem, saved_tr);
                if (vsp < 64u) vstack[vsp++] = ires;
                break;
            }

            case BC_EVAL_AST: {
                uint16_t saved_tr = mem->temp_root_count;
                for (uint8_t k = 0; k < vsp; ++k) {
                    (void)mem_push_temp_root(mem, &vstack[k]);
                }
                Value v = eval_ast(mem, tab, inst->imm16, frame_base, cur_ret_type);
                mem_pop_temp_roots(mem, saved_tr);
                if (vsp < 64u) vstack[vsp++] = v;
                break;
            }

            case BC_RET: {
                Value rval = (vsp > 0) ? vstack[vsp - 1] : val_make_f64(mem, mem->fast_real_type, 0.0);
                mem->jump_val    = rval;
                mem->jump_active = JUMP_RETURN;
                mem->eval_depth--;
                return rval;
            }

            case BC_HALT: {
                mem->eval_depth--;
                return (vsp > 0) ? vstack[vsp - 1] : val_void();
            }
        }

        if (mem->jump_active != JUMP_NONE) {
            if (mem->jump_active == JUMP_CONTINUE_ANON && loop_depth > 0) {
                mem->jump_active = JUMP_NONE;
                BcLoopFrame *lf = &loops[loop_depth - 1];
                vsp = lf->base_vsp;
                if (lf->is_for_range) {
                    pc = lf->cond_pc; /* jumps directly to BC_FOR_STEP */
                    continue;
                }
                if (++lf->guard > 1000000u) {
                    mem_err(mem, "Loop iteration limit exceeded");
                    break;
                }
                pc = lf->cond_pc;
                continue;
            }
            if (mem->jump_active == JUMP_BREAK_ANON && loop_depth > 0) {
                mem->jump_active = JUMP_NONE;
                vsp = loops[loop_depth - 1].base_vsp;
                if (vsp > 0 && val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                    vstack[vsp - 1] = mem->jump_val;
                    mem->jump_val   = val_void();
                }
                loop_depth--;
                pc = loops[loop_depth].exit_pc;
                continue;
            }
            break;
        }
    }

    mem->eval_depth--;
    return (vsp > 0) ? vstack[vsp - 1] : val_void();
}

static const char *bc_op_name(BcOp op) {
    switch (op) {
        case BC_NOP:        return "NOP";
        case BC_PUSH_F64:   return "PUSH_F64";
        case BC_PUSH_LIT:   return "PUSH_LIT";
        case BC_LOAD_SLOT:  return "LOAD_SLOT";
        case BC_STORE_SLOT: return "STORE_SLOT";
        case BC_POP:        return "POP";
        case BC_ADD:        return "ADD";
        case BC_SUB:        return "SUB";
        case BC_MUL:        return "MUL";
        case BC_DIV:        return "DIV";
        case BC_MOD:        return "MOD";
        case BC_POW:        return "POW";
        case BC_NEG:        return "NEG";
        case BC_EQ:         return "EQ";
        case BC_NE:         return "NE";
        case BC_LT:         return "LT";
        case BC_LE:         return "LE";
        case BC_GT:         return "GT";
        case BC_GE:         return "GE";
        case BC_NOT:        return "NOT";
        case BC_JMP:        return "JMP";
        case BC_JMP_FALSE:  return "JMP_FALSE";
        case BC_JMP_TRUE:   return "JMP_TRUE";
        case BC_LOOP_GUARD: return "LOOP_GUARD";
        case BC_FOR_INIT:   return "FOR_INIT";
        case BC_FOR_STEP:   return "FOR_STEP";
        case BC_CALL:       return "CALL";
        case BC_FN_CALL:    return "FN_CALL";
        case BC_INTRINSIC:  return "INTRINSIC";
        case BC_EVAL_AST:   return "EVAL_AST";
        case BC_RET:        return "RET";
        case BC_HALT:       return "HALT";
        default:            return "UNKNOWN";
    }
}

size_t bc_disasm_func(MemArena *mem, const SymTable *tab, const char *fn_name, uint16_t fn_len,
                      char *out_buf, size_t buf_cap) {
    if (!out_buf || buf_cap == 0) return 0;
    out_buf[0] = '\0';
    if (!mem || !tab || !fn_name || fn_len == 0) return 0;

    int sid = sym_find(tab, mem, fn_name, fn_len);
    if (sid < 0) {
        mem_err(mem, "Unknown function '%.*s' in bc_disasm", (int)fn_len, fn_name);
        return 0;
    }

    int target_ov = -1;
    for (uint16_t i = tab->sym_first_ov[sid]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == FIX_FUNC && sym_mod_enabled(tab, ov->mod_id) && ov->body_ast != MEM_NO_IDX) {
            target_ov = (int)i;
            break;
        }
    }
    if (target_ov < 0 || target_ov >= 2048) {
        mem_err(mem, "No active function overload found for '%.*s'", (int)fn_len, fn_name);
        return 0;
    }

    const OpOverload *ov = &tab->overloads[target_ov];
    if (mem->ov_bc_entry[target_ov] == 0) {
        bool is_perm = (ov->body_ast < mem->perm_ast_count);
        uint16_t pc = bc_compile_ast(mem, tab, ov->body_ast, is_perm);
        mem->ov_bc_entry[target_ov] = (pc != MEM_NO_IDX) ? (uint16_t)(pc + 1u) : 0xFFFFu;
    }
    if (mem->ov_bc_entry[target_ov] == 0 || mem->ov_bc_entry[target_ov] == 0xFFFFu) {
        mem_err(mem, "Failed to compile bytecode for '%.*s'", (int)fn_len, fn_name);
        return 0;
    }

    uint16_t start_pc = (uint16_t)(mem->ov_bc_entry[target_ov] - 1u);
    size_t used = 0;
    for (uint16_t pc = start_pc; pc < mem->bc_count && used + 1 < buf_cap; ++pc) {
        const BcInst *inst = &mem->bc_code[pc];
        int n = 0;
        if (inst->op == BC_PUSH_F64) {
            double d = (inst->imm16 == 0xFFFEu) ? 1.0
                       : ((inst->imm16 != MEM_NO_IDX) ? mem_read_pool(mem, inst->imm16) : 0.0);
            n = snprintf(out_buf + used, buf_cap - used, "%04u: %-10s %.15g\n",
                         (unsigned)(pc - start_pc), bc_op_name((BcOp)inst->op), d);
        } else if (inst->op == BC_LOAD_SLOT || inst->op == BC_STORE_SLOT) {
            n = snprintf(out_buf + used, buf_cap - used, "%04u: %-10s s%u\n",
                         (unsigned)(pc - start_pc), bc_op_name((BcOp)inst->op), (unsigned)inst->a);
        } else if (inst->op == BC_CALL && inst->b < tab->sym_count) {
            StrSlice s = tab->syms[inst->b];
            n = snprintf(out_buf + used, buf_cap - used, "%04u: %-10s %.*s/%u\n",
                         (unsigned)(pc - start_pc), bc_op_name((BcOp)inst->op),
                         (int)s.len, mem_str_ptr(mem, s), (unsigned)inst->a);
        } else if (inst->op == BC_JMP || inst->op == BC_JMP_FALSE || inst->op == BC_JMP_TRUE) {
            n = snprintf(out_buf + used, buf_cap - used, "%04u: %-10s -> %04u\n",
                         (unsigned)(pc - start_pc), bc_op_name((BcOp)inst->op),
                         (unsigned)(inst->imm16 - start_pc));
        } else {
            n = snprintf(out_buf + used, buf_cap - used, "%04u: %s\n",
                         (unsigned)(pc - start_pc), bc_op_name((BcOp)inst->op));
        }
        if (n > 0) {
            used += ((size_t)n < buf_cap - used) ? (size_t)n : (buf_cap - used - 1);
        }
        if (inst->op == BC_HALT) break;
    }
    return used;
}
