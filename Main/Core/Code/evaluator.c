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
#include "../lib/delegate/delegate.h"
#include "../lib/hardcode/hardcode.h"
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

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

Value eval_dispatch_overload(MemArena *mem, SymTable *tab,
                               uint16_t sym_id, OpFixity fixity,
                               uint8_t argc, const Value *args) {
    uint8_t arg_types[SYM_MAX_OV_PARAMS] = { 0 };
    uint8_t arg_units[SYM_MAX_OV_PARAMS] = { 0 };
    for (uint8_t i = 0; i < argc && i < SYM_MAX_OV_PARAMS; ++i) {
        arg_types[i] = (uint8_t)val_get_type(mem, args[i]);
        arg_units[i] = val_get_unit(mem, args[i]);
    }

    const OpOverload *matched = NULL;
    int     best_score        = -1;
    uint8_t matched_bound_T   = MEM_UNIT_NONE;
    bool    matched_has_T     = false;
    bool    type_only_matched = false;

    uint16_t first_ov = (sym_id < tab->sym_count) ? tab->sym_first_ov[sym_id] : MEM_NO_IDX;
    for (uint16_t i = first_ov; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != (uint8_t)fixity) continue;
        if (ov->param_count != argc) continue;
        if (!sym_mod_enabled(tab, ov->mod_id)) continue;

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

    if (!matched) {
        StrSlice s = tab->syms[sym_id];
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

    Value result = eval_ast(mem, tab, matched->body_ast, frame_base, matched->ret_type);
    if (mem->jump_active == JUMP_RETURN) {
        result = mem->jump_val;
        mem->jump_active = JUMP_NONE;
    } else if (mem->jump_active != JUMP_NONE && !mem->has_error) {
        mem->jump_active = JUMP_NONE;
        mem_err(mem, "Unhandled jump, break, or continue outside active loop/scope");
    }

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

static Value eval_index_op(MemArena *mem, SymTable *tab,
                           const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type,
                           bool is_write, Value rhs) {
    Value base = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

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
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0xFFu);
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
            mem_write_pool(mem, out_hdr + 0, (double)raw_hdr0);
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
                uint8_t  ruid = (uint8_t)((rraw >> 8) & 0xFFu);
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
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0xFFu);
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
                if (elem_tid != 0 && (elem_tid != item_tid || elem_uid != item_uid)) {
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
            return val_make_f64_unit(mem, cur_tid ? cur_tid : mem->fast_real_type, base_unit,
                                     mem_read_pool(mem, cur_pidx));
        } else {
            if (cur_tid != 0 && cur_tid < tab->type_count && !tab->types[cur_tid].is_consuming) {
                uint16_t fsize   = tab->types[cur_tid].flat_size;
                uint16_t src_idx = val_get_pool_idx(rhs);
                for (uint16_t i = 0; i < fsize; ++i) {
                    mem_write_pool(mem, cur_pidx + i, mem_read_pool(mem, src_idx + i));
                }
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

    for (uint8_t i = 0; i < actual; ++i) {
        uint16_t tid = val_get_type(mem, items[i]);
        if (tid == 0 || tid >= tab->type_count || !tab->types[tid].is_consuming || tid == mem->str_type_id) {
            mem_err(mem, "Matrix elements must be numeric scalars or sub-matrices");
            return val_void();
        }
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
            uint16_t sym_id = n->ref;
            for (uint8_t i = 0; i < tab->const_count; ++i) {
                if (tab->consts[i].sym_id == sym_id && sym_mod_enabled(tab, tab->consts[i].mod_id)) {
                    return tab->const_vals[tab->consts[i].val_idx];
                }
            }
            StrSlice s = tab->syms[sym_id];
            mem_err(mem, "Constant '%.*s' is disabled or undefined", (int)s.len, mem_str_ptr(mem, s));
            return val_void();
        }

        case AST_SLOT_LOAD:
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
            return mem_get_slot(mem, frame_base, n->sub);

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
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                const AstNode *lhs = mem_get_ast(mem, n->left);
                if (lhs->kind == AST_SLOT_LOAD) {
                    uint16_t abs_slot = (uint16_t)(frame_base + lhs->sub);
                    if (abs_slot >= MEM_MAX_CALL_STACK) {
                        mem_err(mem, "Stack slot index %u out of bounds", (unsigned)abs_slot);
                        return val_void();
                    }
                    if (lhs->left != 0 && lhs->left != MEM_NO_IDX) {
                        mem->slot_locked_type[abs_slot] = (uint8_t)lhs->left;
                    }
                    if (lhs->right != 0 && lhs->right != MEM_NO_IDX) {
                        mem->slot_locked_unit[abs_slot] = (uint8_t)lhs->right;
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

                    mem_set_slot(mem, frame_base, lhs->sub, rhs);
                    return rhs;
                } else if (lhs->kind == AST_INDEX) {
                    return eval_index_op(mem, tab, lhs, frame_base, cur_ret_type, true, rhs);
                } else if (lhs->kind == AST_FIELD_LOAD) {
                    Value target       = (lhs->right != MEM_NO_IDX)
                                         ? mem_get_slot(mem, frame_base, (uint8_t)lhs->right)
                                         : eval_ast(mem, tab, lhs->left, frame_base, cur_ret_type);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    uint16_t base_pidx = val_get_pool_idx(target);
                    uint16_t foff      = lhs->ref;
                    uint16_t ftid      = lhs->sub;
                    if (ftid != 0 && ftid < tab->type_count && !tab->types[ftid].is_consuming) {
                        uint16_t fsize   = tab->types[ftid].flat_size;
                        uint16_t src_idx = val_get_pool_idx(rhs);
                        for (uint16_t i = 0; i < fsize; ++i) {
                            mem_write_pool(mem, (uint16_t)(base_pidx + foff + i),
                                           mem_read_pool(mem, (uint16_t)(src_idx + i)));
                        }
                    } else {
                        mem_write_pool(mem, base_pidx + foff, val_get_f64(mem, rhs));
                    }
                    return rhs;
                }
                mem_err(mem, "Left side of assignment operator must be a local variable or struct field");
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
            uint32_t packed_slots = (uint32_t)n->left | ((uint32_t)n->right << 16);
            mem_write_pool(mem, hdr_idx + 0, (double)n->ref);
            mem_write_pool(mem, hdr_idx + 1, (double)n->sub);
            mem_write_pool(mem, hdr_idx + 2, (double)packed_slots);
            mem_write_pool(mem, hdr_idx + 3, (double)frame_base);
            return val_make_comp(mem, mem->fn_type_id, hdr_idx, MEM_FN_HDR_SLOTS);
        }

        case AST_FN_CALL: {
            Value fn_val = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value args[16];
            uint8_t argc = 0;
            uint16_t cur = n->right;
            while (cur != MEM_NO_IDX && argc < 16) {
                const AstNode *link = mem_get_ast(mem, cur);
                args[argc++] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                cur = link->right;
            }
            return delegate_call_fn(mem, tab, fn_val, argc, args);
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
            return eval_dispatch_overload(mem, tab, n->ref, FIX_FUNC, argc, args);
        }

        case AST_INTRINSIC: {
            Value a = (n->left  != MEM_NO_IDX) ? eval_ast(mem, tab, n->left,  frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value b = (n->right != MEM_NO_IDX) ? eval_ast(mem, tab, n->right, frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value c = (n->ref   != MEM_NO_IDX && n->ref != 0)
                      ? eval_ast(mem, tab, n->ref, frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
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
