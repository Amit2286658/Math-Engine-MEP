/*
 * eval_dispatch.c — Tagged Scope Jump Resolution & Multiple-Dispatch Overload Engine
 */
#include "eval_internal.h"

/* ============================================================================
 * 1. TAGGED SCOPE (`Tag : { ... }`) & SEQUENCE JUMP/BREAK RESOLUTION
 * ============================================================================ */

static void scan_all_labels_in_tree_d(MemArena *mem, uint16_t node_idx, int depth) {
    if (node_idx == MEM_NO_IDX || depth > 64) return;
    const AstNode *n = mem_get_ast(mem, node_idx);
    if (n->kind == AST_TAGGED_SCOPE && n->sub < MEM_MAX_LABELS) {
        mem->label_ast[n->sub] = n->left;
    }
    if (n->kind != AST_LIT && n->kind != AST_CONST_SYM &&
        n->kind != AST_SLOT_LOAD && n->kind != AST_JUMP &&
        n->kind != AST_RUN_TAG && n->kind != AST_BREAK &&
        n->kind != AST_CONTINUE && n->kind != AST_RETURN &&
        n->kind != AST_LAMBDA && n->kind != AST_UNPACK_ELEM) {
        scan_all_labels_in_tree_d(mem, n->left, depth + 1);
        if (n->kind != AST_FIELD_LOAD && n->kind != AST_UNARY && n->kind != AST_MAT_LIT) {
            scan_all_labels_in_tree_d(mem, n->right, depth + 1);
        }
    }
}
static void scan_all_labels_in_tree(MemArena *mem, uint16_t node_idx) { scan_all_labels_in_tree_d(mem, node_idx, 0); }

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

Value eval_seq_chain(MemArena *mem, SymTable *tab,
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

Value eval_dispatch_overload_in_mod(MemArena *mem, SymTable *tab,
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
    for (uint16_t i = first_ov; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        const OpOverload *ov = &sym_overloads(tab)[i];
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
        for (uint16_t i = first_ov; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
            const OpOverload *ov = &sym_overloads(tab)[i];
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
            uint16_t tid  = tid0;
            const TypeDef *td = &tab->types[tid0];
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

    Value result = eval_ast(mem, tab, matched->body_ast, frame_base, matched->ret_type);
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

