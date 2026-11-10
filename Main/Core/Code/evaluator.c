/*
 * evaluator.c — Core AST Evaluation Tree-Walk Dispatcher
 */
#include "eval_internal.h"

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
            uint16_t ftid      = sym_unpack_outer_type(mem, n->sub);
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
                        uint16_t rtid = val_get_type(mem, rhs);
                        if ((uint8_t)lhs->right < SYM_UNIT_LOCK_NONE &&
                            rtid != MEM_TYPE_VOID &&
                            rtid != mem->vec_type_id && rtid != mem->map_type_id &&
                            val_get_unit(mem, rhs) == MEM_UNIT_NONE) {
                            rhs = val_with_unit(mem, rhs, (uint8_t)lhs->right);
                        }
                    }

                    uint8_t req_type = mem->slot_locked_type[abs_slot];
                    uint8_t req_unit = mem->slot_locked_unit[abs_slot];
                    if (req_unit != 0 && req_unit < SYM_UNIT_LOCK_NONE) {
                        uint16_t rtid = val_get_type(mem, rhs);
                        if (rtid != MEM_TYPE_VOID &&
                            rtid != mem->vec_type_id && rtid != mem->map_type_id &&
                            val_get_unit(mem, rhs) == MEM_UNIT_NONE) {
                            rhs = val_with_unit(mem, rhs, req_unit);
                        }
                    }
                    if (!sym_validate_and_stamp_lock(tab, mem, MEM_NO_IDX, NULL, req_type, req_unit, &rhs)) {
                        return val_void();
                    }

                    mem_set_slot(mem, frame_base, slot_idx, rhs);
                    if (is_const_decl) {
                        mem->slot_is_const[abs_slot] = 1u;
                        mem_val_mark_immutable(mem, rhs);
                    }
                    /* Sync recursive self-binding or active-frame upvalue mutation to closure HeapBlocks */
                    if (mem->active_closure_count > 0 || mem->closure_list_overflow) {
                        bool is_colon_eq = false;
                        (void)hardcode_is_plain_store_sym(mem, tab, n->ref, &is_colon_eq);
                        uint16_t self_hdr = (mem->fn_type_id != 0 && val_get_type(mem, rhs) == mem->fn_type_id)
                                            ? val_get_pool_idx(rhs) : MEM_NO_IDX;
                        uint16_t lim = mem->closure_list_overflow ? MEM_MAX_HEAP_BLOCKS : mem->active_closure_count;
                        for (uint16_t ci = 0; ci < lim; ++ci) {
                            uint16_t hid = mem->closure_list_overflow ? ci : mem->active_closure_hids[ci];
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
                    uint16_t raw_ftid  = lhs->sub;
                    if (raw_ftid != 0) {
                        if (!sym_validate_and_stamp_lock(tab, mem, MEM_NO_IDX, "Struct field", raw_ftid, 0, &rhs)) {
                            return val_void();
                        }
                    }
                    uint16_t ftid = sym_unpack_outer_type(mem, raw_ftid);
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
            if (!mem->has_error && hardcode_is_into_op_sym(mem, tab, n->ref) &&
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
                if (mem->active_closure_count < MEM_MAX_ACTIVE_CLOSURES) {
                    mem->active_closure_hids[mem->active_closure_count++] = (uint8_t)hid;
                } else {
                    mem->closure_list_overflow = 1u;
                }
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
            IntrinsicId intr = (IntrinsicId)n->sub;
            if (intr == INTR_PRINT || intr == INTR_PRINTLN) {
                bool add_nl = (intr == INTR_PRINTLN);
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
            if (intr == INTR_ASSERT) {
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
            if (intr == INTR_CLOCK_MS) {
                double ms = (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
                return val_make_f64(mem, mem->fast_real_type, ms);
            }
            if (intr == INTR_IMPORT) {
                StrSlice s = { n->left, n->right };
                Value imp_res = val_void();
                (void)ext_import_module_or_file(mem, tab, mem_str_ptr(mem, s), s.len,
                                                (n->ref != 0), &imp_res);
                return imp_res;
            }
            Value a = (n->left  != MEM_NO_IDX) ? eval_ast(mem, tab, n->left,  frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value b = (n->right != MEM_NO_IDX) ? eval_ast(mem, tab, n->right, frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value c = (n->ref   != MEM_NO_IDX && n->ref != 0)
                      ? eval_ast(mem, tab, n->ref, frame_base, cur_ret_type) : val_void();
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            if (intr == INTR_FILE_READ || intr == INTR_FILE_WRITE ||
                intr == INTR_FILE_APPEND || intr == INTR_FILE_EXISTS) {
                return eval_file_io_op(mem, tab, intr, a, b);
            }
            if (intr == INTR_JSON_PARSE) {
                return eval_json_parse_op(mem, tab, a);
            }
            if (intr == INTR_JSON_STRINGIFY) {
                return eval_json_stringify_op(mem, tab, a, b);
            }
            return delegate_eval_intrinsic(mem, tab, intr, a, b, c, cur_ret_type);
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

                if (!sym_validate_and_stamp_lock(tab, mem, MEM_NO_IDX, NULL, fd->type_id, 0, &fval)) {
                    mem_clear_err(mem);
                    uint16_t fval_tid_err = val_get_type(mem, fval);
                    char st_name[32], exp_t[32], got_t[32];
                    sym_type_name(tab, mem, st_tid, st_name, sizeof(st_name));
                    sym_type_name(tab, mem, fd->type_id, exp_t, sizeof(exp_t));
                    if (fval_tid_err == mem->vec_type_id && fval_tid_err != 0) {
                        uint16_t vraw = (uint16_t)mem_read_pool(mem, val_get_pool_idx(fval));
                        uint16_t et   = (uint16_t)(vraw & 0xFFu);
                        sym_type_name(tab, mem, et ? sym_pack_container_type(mem, fval_tid_err, et) : fval_tid_err,
                                      got_t, sizeof(got_t));
                    } else if (fval_tid_err == mem->map_type_id && fval_tid_err != 0) {
                        uint16_t mraw = (uint16_t)mem_read_pool(mem, val_get_pool_idx(fval));
                        uint16_t et   = (uint16_t)(mraw & 0xFFu);
                        sym_type_name(tab, mem, et ? sym_pack_container_type(mem, fval_tid_err, et) : fval_tid_err,
                                      got_t, sizeof(got_t));
                    } else {
                        sym_type_name(tab, mem, fval_tid_err, got_t, sizeof(got_t));
                    }
                    mem_err(mem, "Struct %s field %.*s expects %s, got %s",
                            st_name, (int)fd->name.len, mem_str_ptr(mem, fd->name), exp_t, got_t);
                    return val_void();
                }

                uint16_t fval_tid = val_get_type(mem, fval);
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
