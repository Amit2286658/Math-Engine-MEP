/*
 * parser_postfix.c — UFCS Field Steps, Matrix Literals, Indexing, Calls, Blocks & Lambdas
 */
#include "parser_internal.h"


uint16_t parse_field_step(Parser *P, uint16_t left) {
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;

    if (P->lex.cur.kind != TOK_IDENT &&
        !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
        mem_err(mem, "Expected struct field or method name after '.'");
        return MEM_NO_IDX;
    }
    const char *fname = P->lex.src + P->lex.cur.off;
    uint16_t flen     = P->lex.cur.len;
    lex_next(&P->lex);

    /* Check if `left` is a struct whose field `fname` is a `$fn` closure (`obj.cb(args)`) */
    if (P->lex.cur.kind == TOK_LPAREN && mem->fn_type_id != 0) {
        const AstNode *lnode_p = mem_get_ast(mem, left);
        uint16_t ctype    = MEM_TYPE_VOID;
        uint16_t b_ast    = left;
        uint16_t b_slot   = MEM_NO_IDX;
        uint16_t p_off    = 0;
        if (lnode_p->kind == AST_SLOT_LOAD) {
            b_slot = (uint8_t)(lnode_p->sub & AST_SLOT_IDX_MASK);
            ctype  = P->slot_types[b_slot];
        } else if (lnode_p->kind == AST_CONST_SYM) {
            uint16_t cmod = (lnode_p->right != MEM_NO_IDX && lnode_p->right < tab->mod_count)
                            ? lnode_p->right : P->cur_mod;
            int mv_i = sym_find_var(tab, lnode_p->ref, cmod, NULL);
            if (mv_i >= 0) {
                const ModVarDef *mv = &sym_vars(tab)[mv_i];
                ctype = val_get_type(mem, mv->val);
                if (ctype == MEM_TYPE_VOID) {
                    ctype = (mv->lock_type & 0xFFu) ? (uint16_t)(mv->lock_type & 0xFFu)
                                                    : (uint16_t)((mv->lock_type >> 8u) & 0xFFu);
                }
            }
        } else if (lnode_p->kind == AST_CONSTRUCT) {
            ctype = lnode_p->ref;
        } else if (lnode_p->kind == AST_FIELD_LOAD) {
            ctype  = lnode_p->sub;
            p_off  = lnode_p->ref;
            b_ast  = lnode_p->left;
            b_slot = lnode_p->right;
        }
        uint16_t f_tid = MEM_TYPE_VOID, f_off = 0;
        bool is_fn_field = false;
        if (ctype != MEM_TYPE_VOID && ctype < tab->type_count &&
            !tab->types[ctype].is_consuming && tab->types[ctype].field_count > 0) {
            if (sym_resolve_field(tab, mem, ctype, fname, flen, &f_tid, &f_off) &&
                f_tid == mem->fn_type_id) {
                is_fn_field = true;
            }
        } else if (ctype == MEM_TYPE_VOID && find_local_idx(P, fname, flen) < 0) {
            int f_sid = sym_find(tab, mem, fname, flen);
            bool has_fn_or_var = (f_sid >= 0) &&
                (sym_has_visible_func_ov(tab, (uint16_t)f_sid) ||
                 sym_find_var(tab, (uint16_t)f_sid, P->cur_mod, NULL) >= 0);
            if (!has_fn_or_var) {
                for (uint16_t tid = 1; tid < tab->type_count; ++tid) {
                    if (!tab->types[tid].is_consuming && tab->types[tid].field_count > 0 &&
                        sym_mod_enabled(tab, tab->types[tid].mod_id)) {
                        if (sym_resolve_field(tab, mem, tid, fname, flen, &f_tid, &f_off) &&
                            f_tid == mem->fn_type_id) {
                            is_fn_field = true;
                            break;
                        }
                    }
                }
            }
        }
        if (is_fn_field) {
            uint16_t fload = left;
            if (lnode_p->kind == AST_FIELD_LOAD) {
                AstNode *mut = mem_get_ast_mut(mem, left);
                mut->sub = (uint8_t)f_tid;
                mut->ref = (uint16_t)(p_off + f_off);
            } else {
                fload = mem_alloc_ast(mem, AST_FIELD_LOAD, (uint8_t)f_tid,
                                      (uint16_t)(p_off + f_off), b_ast, b_slot);
            }
            return parse_fn_call_step(P, fload);
        }
    }

    /* Uniform Function Call Syntax (UFCS): `base.fn(a, b)` -> `fn(base, a, b)` */
    if (P->lex.cur.kind == TOK_LPAREN) {
        lex_next(&P->lex); /* consume '(' */
        uint8_t saved_ternary = P->in_ternary_then;
        P->in_ternary_then = 0;

        uint16_t head = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, MEM_NO_IDX);
        uint16_t tail = head;
        uint8_t  argc = 1;

        if (P->lex.cur.kind != TOK_RPAREN) {
            while (!mem->has_error) {
                uint16_t arg  = parser_parse_expr(P, 0);
                uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, arg, MEM_NO_IDX);
                if (head == MEM_NO_IDX) head = link;
                else mem_get_ast_mut(mem, tail)->right = link;
                tail = link;
                argc++;
                if (P->lex.cur.kind == TOK_COMMA) {
                    lex_next(&P->lex);
                } else {
                    break;
                }
            }
        }
        P->in_ternary_then = saved_ternary;
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected ')' after arguments to '.%.*s(...)'", (int)flen, fname);
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);

        int vidx = find_local_idx(P, fname, flen);
        if (vidx >= 0) {
            uint8_t slot = P->locals[vidx].slot;
            if (mem->fn_type_id != 0 && P->slot_types[slot] == mem->fn_type_id) {
                uint16_t fn_load = mem_alloc_ast(mem, AST_SLOT_LOAD, slot, 0, 0, 0);
                return mem_alloc_ast(mem, AST_FN_CALL, argc, 0, fn_load, head);
            }
        }
        int sym_id = sym_find(tab, mem, fname, flen);
        if (sym_id < 0) {
            mem_err(mem, "Undefined method or function '%.*s'", (int)flen, fname);
            return MEM_NO_IDX;
        }
        uint16_t priv_mod = MEM_NO_IDX;
        int mv_idx = sym_find_var(tab, (uint16_t)sym_id, P->cur_mod, &priv_mod);
        if (mv_idx >= 0) {
            const ModVarDef *mv = &sym_vars(tab)[mv_idx];
            uint16_t mv_tid = val_get_type(mem, mv->val);
            if (mv_tid == MEM_TYPE_VOID) {
                mv_tid = (mv->lock_type & 0xFFu) ? (uint16_t)(mv->lock_type & 0xFFu)
                                                 : (uint16_t)((mv->lock_type >> 8u) & 0xFFu);
            }
            if ((mem->fn_type_id != 0 && mv_tid == mem->fn_type_id) ||
                !sym_has_visible_func_ov(tab, (uint16_t)sym_id)) {
                uint16_t fn_load = mem_alloc_ast(mem, AST_CONST_SYM, 0, (uint16_t)sym_id, MEM_NO_IDX, P->cur_mod);
                return mem_alloc_ast(mem, AST_FN_CALL, argc, 0, fn_load, head);
            }
        } else if (priv_mod != MEM_NO_IDX && !sym_has_visible_func_ov(tab, (uint16_t)sym_id)) {
            StrSlice pmod_s = sym_mod_name(tab, priv_mod);
            mem_err(mem, "Function or variable '%.*s' is private to module '[%.*s]'",
                    (int)flen, fname, (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
            return MEM_NO_IDX;
        }
        return mem_alloc_ast(mem, AST_CALL, argc, (uint16_t)sym_id, head, MEM_NO_IDX);
    }

    const AstNode *lnode = mem_get_ast(mem, left);
    uint16_t cur_type    = MEM_TYPE_VOID;
    uint16_t base_ast    = left;
    uint16_t base_slot   = MEM_NO_IDX;
    uint16_t prev_off    = 0;

    if (lnode->kind == AST_SLOT_LOAD) {
        base_slot = (uint8_t)(lnode->sub & AST_SLOT_IDX_MASK);
        cur_type  = P->slot_types[base_slot];
    } else if (lnode->kind == AST_CONST_SYM) {
        int mv_idx = sym_find_var(tab, lnode->ref, P->cur_mod, NULL);
        if (mv_idx >= 0) {
            const ModVarDef *mv = &sym_vars(tab)[mv_idx];
            cur_type = val_get_type(mem, mv->val);
            if (cur_type == MEM_TYPE_VOID) {
                cur_type = (mv->lock_type & 0xFFu) ? (uint16_t)(mv->lock_type & 0xFFu)
                                                   : (uint16_t)((mv->lock_type >> 8u) & 0xFFu);
            }
        }
    } else if (lnode->kind == AST_CONSTRUCT) {
        cur_type  = lnode->ref;
    } else if (lnode->kind == AST_FIELD_LOAD) {
        cur_type  = lnode->sub;
        prev_off  = lnode->ref;
        base_ast  = lnode->left;
        base_slot = lnode->right;
    }

    uint16_t ftype = MEM_TYPE_VOID;
    uint16_t foff  = 0;
    bool resolved  = false;

    if (cur_type != MEM_TYPE_VOID && cur_type < tab->type_count &&
        !tab->types[cur_type].is_consuming && tab->types[cur_type].field_count > 0) {
        resolved = sym_resolve_field(tab, mem, cur_type, fname, flen, &ftype, &foff);
    }
    if (!resolved) {
        for (uint16_t tid = 1; tid < tab->type_count; ++tid) {
            if (!tab->types[tid].is_consuming && tab->types[tid].field_count > 0 &&
                sym_mod_enabled(tab, tab->types[tid].mod_id) &&
                ((tab->types[tid].reserved & SYM_TYPE_PRIVATE) == 0 || tab->types[tid].mod_id == P->cur_mod)) {
                if (sym_resolve_field(tab, mem, tid, fname, flen, &ftype, &foff)) {
                    resolved = true;
                    break;
                }
            }
        }
    }
    if (!resolved) {
        mem_err(mem, "Unknown struct field '%.*s'", (int)flen, fname);
        return MEM_NO_IDX;
    }

    if (lnode->kind == AST_FIELD_LOAD) {
        AstNode *mut = mem_get_ast_mut(mem, left);
        mut->sub = (uint8_t)ftype;
        mut->ref = prev_off + foff;
        return left;
    }

    return mem_alloc_ast(mem, AST_FIELD_LOAD, (uint8_t)ftype, prev_off + foff, base_ast, base_slot);
}

/* ============================================================================
 * 2b. MATRIX LITERAL `[...]`, COMPREHENSIONS `[expr for x in ...]` & `v[i]`
 * ============================================================================ */

static bool cur_is_seq_semicolon(const Parser *P) {
    if (P->lex.cur.kind != TOK_OP) return false;
    return sym_active_meta_tag(P->lex.tab, P->lex.cur.sym_id, FIX_INFIX_LEFT) == INTR_SEQ;
}

uint16_t parse_mat_literal(Parser *P) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;

    lex_next(&P->lex); /* consume '[' */

    uint8_t saved_ternary = P->in_ternary_then;
    P->in_ternary_then = 0;

    if (P->lex.cur.kind == TOK_RBRACK) {
        P->in_ternary_then = saved_ternary;
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_MAT_LIT, 0, 0, MEM_NO_IDX, MEM_NO_IDX);
    }

    const char *comp_idx_name = NULL;
    uint16_t    comp_idx_len  = 0;
    const char *comp_vname    = NULL;
    uint16_t    comp_vlen     = 0;
    if (hardcode_scan_comprehension_var(P, &comp_idx_name, &comp_idx_len, &comp_vname, &comp_vlen)) {
        uint8_t saved_vis = P->vis_count;
        P->scope_depth++;
        int idx_slot = -1;
        if (comp_idx_name != NULL && comp_idx_len > 0) {
            idx_slot = parser_add_local(P, comp_idx_name, comp_idx_len, mem->fast_real_type);
            if (mem->has_error) {
                P->vis_count = saved_vis;
                P->scope_depth--;
                P->in_ternary_then = saved_ternary;
                return MEM_NO_IDX;
            }
        }
        int var_slot = parser_add_local(P, comp_vname, comp_vlen, mem->fast_real_type);
        if (mem->has_error) {
            P->vis_count = saved_vis;
            P->scope_depth--;
            P->in_ternary_then = saved_ternary;
            return MEM_NO_IDX;
        }

        uint16_t map_expr = parser_parse_expr(P, 2);
        if (mem->has_error || map_expr == MEM_NO_IDX) {
            P->vis_count = saved_vis;
            P->scope_depth--;
            P->in_ternary_then = saved_ternary;
            return MEM_NO_IDX;
        }

        if (!hardcode_is_for_kw(mem, tab, P->lex.src + P->lex.cur.off, P->lex.cur.len)) {
            P->vis_count = saved_vis;
            P->scope_depth--;
            P->in_ternary_then = saved_ternary;
            mem_err(mem, "Expected 'for' in vector comprehension");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex); /* consume 'for' */

        if (idx_slot >= 0) {
            lex_next(&P->lex); /* consume index variable name */
            if (P->lex.cur.kind != TOK_COMMA) {
                P->vis_count = saved_vis;
                P->scope_depth--;
                P->in_ternary_then = saved_ternary;
                mem_err(mem, "Expected ',' after index variable in comprehension");
                return MEM_NO_IDX;
            }
            lex_next(&P->lex); /* consume ',' */
        }
        lex_next(&P->lex); /* consume loop variable name */

        if (!hardcode_is_in_kw(mem, tab, P->lex.src + P->lex.cur.off, P->lex.cur.len)) {
            P->vis_count = saved_vis;
            P->scope_depth--;
            P->in_ternary_then = saved_ternary;
            mem_err(mem, "Expected 'in' after comprehension variable '%.*s'", (int)comp_vlen, comp_vname);
            return MEM_NO_IDX;
        }
        lex_next(&P->lex); /* consume 'in' */

        uint16_t iter_ast = parser_parse_expr(P, 2);
        if (mem->has_error || iter_ast == MEM_NO_IDX) {
            P->vis_count = saved_vis;
            P->scope_depth--;
            P->in_ternary_then = saved_ternary;
            return MEM_NO_IDX;
        }

        /* Optional `if <filter_cond>` clause inside `[expr for x in iter if cond]` */
        uint16_t cond_ast = MEM_NO_IDX;
        if (token_func_meta(P, P->lex.cur) == INTR_IF) {
            lex_next(&P->lex); /* consume 'if' */
            cond_ast = parser_parse_expr(P, 2);
            if (mem->has_error || cond_ast == MEM_NO_IDX) {
                P->vis_count = saved_vis;
                P->scope_depth--;
                P->in_ternary_then = saved_ternary;
                return MEM_NO_IDX;
            }
        }

        P->vis_count = saved_vis;
        P->scope_depth--;
        P->in_ternary_then = saved_ternary;

        if (P->lex.cur.kind != TOK_RBRACK) {
            mem_err(mem, "Expected closing ']' after vector comprehension");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
        uint16_t for_ref = 0x8000u;
        if (idx_slot >= 0) {
            for_ref |= (uint16_t)(0x4000u | (((uint16_t)idx_slot & 0x3Fu) << 8u));
        }
        uint16_t pair_ast = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, map_expr, cond_ast);
        return mem_alloc_ast(mem, AST_FOR_IN, (uint8_t)var_slot, for_ref, iter_ast, pair_ast);
    }

    uint16_t row0_head = MEM_NO_IDX;
    uint16_t row0_tail = MEM_NO_IDX;
    uint8_t  row0_cnt  = 0;

    while (!mem->has_error) {
        uint16_t item = parser_parse_expr(P, 2);
        if (mem->has_error || item == MEM_NO_IDX) {
            P->in_ternary_then = saved_ternary;
            return MEM_NO_IDX;
        }
        uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, item, MEM_NO_IDX);
        if (row0_head == MEM_NO_IDX) row0_head = link;
        else mem_get_ast_mut(mem, row0_tail)->right = link;
        row0_tail = link;
        row0_cnt++;

        if (P->lex.cur.kind == TOK_COMMA) {
            lex_next(&P->lex);
            if (P->lex.cur.kind == TOK_RBRACK) break;
        } else {
            break;
        }
    }

    if (cur_is_seq_semicolon(P)) {
        uint16_t r0_ast    = mem_alloc_ast(mem, AST_MAT_LIT, row0_cnt, 0, row0_head, MEM_NO_IDX);
        uint16_t rows_head = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, r0_ast, MEM_NO_IDX);
        uint16_t rows_tail = rows_head;
        uint8_t  rows_cnt  = 1;

        while (!mem->has_error && cur_is_seq_semicolon(P)) {
            lex_next(&P->lex);
            if (P->lex.cur.kind == TOK_RBRACK) break;

            uint16_t rk_head = MEM_NO_IDX;
            uint16_t rk_tail = MEM_NO_IDX;
            uint8_t  rk_cnt  = 0;
            while (!mem->has_error) {
                uint16_t item = parser_parse_expr(P, 2);
                if (mem->has_error || item == MEM_NO_IDX) {
                    P->in_ternary_then = saved_ternary;
                    return MEM_NO_IDX;
                }
                uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, item, MEM_NO_IDX);
                if (rk_head == MEM_NO_IDX) rk_head = link;
                else mem_get_ast_mut(mem, rk_tail)->right = link;
                rk_tail = link;
                rk_cnt++;

                if (P->lex.cur.kind == TOK_COMMA) {
                    lex_next(&P->lex);
                } else {
                    break;
                }
            }
            uint16_t rk_ast  = mem_alloc_ast(mem, AST_MAT_LIT, rk_cnt, 0, rk_head, MEM_NO_IDX);
            uint16_t rk_link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, rk_ast, MEM_NO_IDX);
            mem_get_ast_mut(mem, rows_tail)->right = rk_link;
            rows_tail = rk_link;
            rows_cnt++;
        }

        P->in_ternary_then = saved_ternary;
        if (P->lex.cur.kind != TOK_RBRACK) {
            mem_err(mem, "Expected closing ']' after matrix literal");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_MAT_LIT, rows_cnt, 0, rows_head, MEM_NO_IDX);
    }

    P->in_ternary_then = saved_ternary;
    if (P->lex.cur.kind != TOK_RBRACK) {
        mem_err(mem, "Expected closing ']' after matrix literal");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex);
    return mem_alloc_ast(mem, AST_MAT_LIT, row0_cnt, 0, row0_head, MEM_NO_IDX);
}

uint16_t parse_index_step(Parser *P, uint16_t left) {
    MemArena *mem = P->lex.mem;
    lex_next(&P->lex); /* consume '[' */

    uint8_t saved_ternary = P->in_ternary_then;
    P->in_ternary_then = 0;

    if (P->lex.cur.kind == TOK_RBRACK) {
        P->in_ternary_then = saved_ternary;
        mem_err(mem, "Empty subscript '[]' is not allowed");
        return MEM_NO_IDX;
    }

    uint16_t idx_head = MEM_NO_IDX;
    uint16_t idx_tail = MEM_NO_IDX;
    uint8_t  argc     = 0;

    while (!mem->has_error) {
        if (argc >= 4) {
            P->in_ternary_then = saved_ternary;
            mem_err(mem, "Maximum 4 subscript indices supported");
            return MEM_NO_IDX;
        }
        uint16_t idx_expr = parser_parse_expr(P, 2);
        if (mem->has_error || idx_expr == MEM_NO_IDX) {
            P->in_ternary_then = saved_ternary;
            return MEM_NO_IDX;
        }
        uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, idx_expr, MEM_NO_IDX);
        if (idx_head == MEM_NO_IDX) idx_head = link;
        else mem_get_ast_mut(mem, idx_tail)->right = link;
        idx_tail = link;
        argc++;

        if (P->lex.cur.kind == TOK_COMMA) {
            lex_next(&P->lex);
        } else {
            break;
        }
    }

    P->in_ternary_then = saved_ternary;
    if (P->lex.cur.kind != TOK_RBRACK) {
        mem_err(mem, "Expected closing ']' after subscript index");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex);
    return mem_alloc_ast(mem, AST_INDEX, argc, 0, left, idx_head);
}

uint16_t parse_fn_call_step(Parser *P, uint16_t fn_ast) {
    MemArena *mem = P->lex.mem;
    lex_next(&P->lex); /* consume '(' */

    uint8_t saved_ternary = P->in_ternary_then;
    P->in_ternary_then = 0;

    uint16_t head = MEM_NO_IDX;
    uint16_t tail = MEM_NO_IDX;
    uint8_t  argc = 0;

    if (P->lex.cur.kind != TOK_RPAREN) {
        while (!mem->has_error) {
            uint16_t arg  = parser_parse_expr(P, 0);
            uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, arg, MEM_NO_IDX);
            if (head == MEM_NO_IDX) head = link;
            else mem_get_ast_mut(mem, tail)->right = link;
            tail = link;
            argc++;
            if (P->lex.cur.kind == TOK_COMMA) {
                lex_next(&P->lex);
            } else {
                break;
            }
        }
    }
    P->in_ternary_then = saved_ternary;
    if (P->lex.cur.kind != TOK_RPAREN) {
        mem_err(mem, "Expected ')' after function call arguments");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex);
    return mem_alloc_ast(mem, AST_FN_CALL, argc, 0, fn_ast, head);
}

/* ============================================================================
 * 3. LEXICAL SCOPE BLOCKS `{ ... }`, LAMBDAS `=>`, `if/or/else`, `switch`, LOOPS
 * ============================================================================ */

uint16_t parse_scope_block(Parser *P) {
    MemArena *mem = P->lex.mem;
    if (P->lex.cur.kind != TOK_LBRACE) {
        mem_err(mem, "Expected '{' to open local scope block");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex);

    uint8_t saved_vis     = P->vis_count;
    uint8_t saved_ternary = P->in_ternary_then;
    P->scope_depth++;
    P->in_ternary_then = 0;

    while (cur_op_meta(P) == INTR_SEQ) {
        lex_next(&P->lex);
    }

    uint16_t body = MEM_NO_IDX;
    if (P->lex.cur.kind == TOK_RBRACE) {
        body = mem_alloc_ast(mem, AST_LIT, MEM_TYPE_VOID, 0, 0, 0);
    } else {
        body = parser_parse_expr(P, 0);
    }

    while (cur_op_meta(P) == INTR_SEQ) {
        lex_next(&P->lex);
    }

    if (P->lex.cur.kind != TOK_RBRACE) {
        mem_err(mem, "Expected closing '}' for local scope block");
    } else {
        lex_next(&P->lex);
    }

    P->vis_count       = saved_vis;
    P->scope_depth--;
    P->in_ternary_then = saved_ternary;
    P->last_was_block  = true;
    return body;
}

uint16_t parse_branch_body(Parser *P) {
    if (P->lex.cur.kind == TOK_LBRACE) {
        return parse_scope_block(P);
    }
    return parser_parse_expr(P, 16);
}

static void record_lambda_src(Parser *P, uint16_t body_ast, uint16_t start_off) {
    MemArena *mem = P->lex.mem;
    if (mem->fn_src_count >= 64) return;
    uint16_t end_off = (P->lex.cur.kind == TOK_EOF) ? P->lex.pos : P->lex.cur.off;
    while (end_off > start_off) {
        char c = P->lex.src[end_off - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') end_off--;
        else break;
    }
    if (end_off > start_off) {
        StrSlice s = mem_intern_str(mem, P->lex.src + start_off, (uint16_t)(end_off - start_off));
        if (!mem->has_error) {
            mem->fn_body_ast[mem->fn_src_count]  = body_ast;
            mem->fn_src_slice[mem->fn_src_count] = s;
            mem->fn_src_count++;
        }
    }
}

/* Pre-scan parameter identifiers inside `(...)` so parameter slots are allocated contiguously */
static void prescan_and_alloc_param_slots(Parser *P, uint16_t paren_content_off) {
    const char *src = P->lex.src;
    uint16_t    len = P->lex.len;
    uint16_t    p   = skip_raw_ws(src, paren_content_off, len);
    for (uint8_t k = 0; k < MEM_FN_MAX_PARAMS && p < len && src[p] != ')'; ++k) {
        if (!lex_is_ident_start((unsigned char)src[p])) break;
        uint16_t id_start = p;
        while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
        uint16_t id_len = (uint16_t)(p - id_start);
        (void)parser_add_local(P, src + id_start, id_len, P->lex.mem->fast_real_type);
        if (P->lex.mem->has_error) return;
        /* Skip optional :Type and = default_expr to next top-level ',' or ')' */
        int depth = 0;
        while (p < len) {
            char c = src[p];
            if (c == '"') {
                p++;
                while (p < len && src[p] != '"') {
                    if (src[p] == '\\' && p + 1 < len) p++;
                    p++;
                }
                if (p < len) p++;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') { depth++; p++; continue; }
            if (c == ')' || c == ']' || c == '}') {
                if (depth == 0) break;
                depth--; p++; continue;
            }
            if (depth == 0 && c == ',') {
                p = skip_raw_ws(src, (uint16_t)(p + 1), len);
                break;
            }
            p++;
        }
    }
}

static bool parse_fn_param_list_in_scope(Parser *P, uint8_t *out_pcnt, uint16_t *out_def_head) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    *out_pcnt     = 0;
    *out_def_head = MEM_NO_IDX;

    prescan_and_alloc_param_slots(P, P->lex.cur.off);
    if (mem->has_error) return false;

    uint16_t def_head = MEM_NO_IDX;
    uint16_t def_tail = MEM_NO_IDX;
    uint8_t  pcnt     = 0;
    bool     seen_def = false;

    if (P->lex.cur.kind != TOK_RPAREN) {
        while (!mem->has_error) {
            if (pcnt >= MEM_FN_MAX_PARAMS) {
                mem_err(mem, "Functions and lambdas support at most %u parameters",
                        (unsigned)MEM_FN_MAX_PARAMS);
                return false;
            }
            if (P->lex.cur.kind != TOK_IDENT &&
                !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                mem_err(mem, "Expected parameter name in function/lambda header");
                return false;
            }
            const char *pname = P->lex.src + P->lex.cur.off;
            uint16_t    plen  = P->lex.cur.len;
            uint16_t    ptype = mem->fast_real_type;
            lex_next(&P->lex);

            if (cur_op_meta(P) == INTR_COLON) {
                lex_next(&P->lex);
                if (P->lex.cur.kind != TOK_IDENT &&
                    !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                    mem_err(mem, "Expected type name after ':' in parameter");
                    return false;
                }
                uint16_t tid = sym_find_type(tab, mem, P->lex.src + P->lex.cur.off, P->lex.cur.len);
                if (tid == MEM_TYPE_VOID) {
                    mem_err(mem, "Unknown parameter type '%.*s'",
                            (int)P->lex.cur.len, P->lex.src + P->lex.cur.off);
                    return false;
                }
                ptype = (tid == SYM_TYPE_ANY) ? mem->fast_real_type : tid;
                lex_next(&P->lex);
            }

            int s = parser_add_local(P, pname, plen, ptype);
            if (mem->has_error) return false;
            P->slot_types[s] = ptype;

            if (P->lex.cur.kind == TOK_OP &&
                tab->syms[P->lex.cur.sym_id].len == 1 &&
                mem_str_ptr(mem, tab->syms[P->lex.cur.sym_id])[0] == '=') {
                seen_def = true;
                lex_next(&P->lex); /* consume '=' */
                uint16_t def_ast = parser_parse_expr(P, 2);
                if (mem->has_error || def_ast == MEM_NO_IDX) return false;
                uint16_t dlink = mem_alloc_ast(mem, AST_ARG_LINK, pcnt, (uint16_t)s, def_ast, MEM_NO_IDX);
                if (def_head == MEM_NO_IDX) def_head = dlink;
                else mem_get_ast_mut(mem, def_tail)->right = dlink;
                def_tail = dlink;
            } else if (seen_def) {
                mem_err(mem, "Non-default parameter '%.*s' cannot follow a default parameter",
                        (int)plen, pname);
                return false;
            }

            pcnt++;
            if (P->lex.cur.kind == TOK_COMMA) {
                lex_next(&P->lex);
            } else {
                break;
            }
        }
    }

    *out_pcnt     = pcnt;
    *out_def_head = def_head;
    return !mem->has_error;
}

uint16_t parse_lambda_from_paren(Parser *P) {
    MemArena *mem = P->lex.mem;
    uint16_t lam_start  = P->lex.cur.off;
    lex_next(&P->lex); /* consume '(' */

    uint8_t  saved_vis  = P->vis_count;
    uint8_t  slot_start = P->local_count;
    P->scope_depth++;

    uint8_t  pcnt     = 0;
    uint16_t def_head = MEM_NO_IDX;
    if (!parse_fn_param_list_in_scope(P, &pcnt, &def_head)) {
        P->vis_count = saved_vis;
        P->scope_depth--;
        return MEM_NO_IDX;
    }

    if (P->lex.cur.kind != TOK_RPAREN) {
        P->vis_count = saved_vis;
        P->scope_depth--;
        mem_err(mem, "Expected ')' after lambda parameters");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex); /* consume ')' */

    if (cur_op_meta(P) != INTR_LAMBDA) {
        P->vis_count = saved_vis;
        P->scope_depth--;
        mem_err(mem, "Expected '=>' after lambda parameter list");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex); /* consume '=>' */

    uint16_t body_ast = (P->lex.cur.kind == TOK_LBRACE)
                        ? parse_scope_block(P)
                        : parser_parse_expr(P, 8);
    uint8_t total_slots = (P->local_count > slot_start)
                          ? (uint8_t)(P->local_count - slot_start) : 0u;
    while (P->vis_count > saved_vis && P->locals[P->vis_count - 1].scope_depth >= P->scope_depth) {
        P->vis_count--;
    }
    P->scope_depth--;
    P->last_was_block = false;
    if (mem->has_error || body_ast == MEM_NO_IDX) return MEM_NO_IDX;

    record_lambda_src(P, body_ast, lam_start);
    uint16_t slot_range = (uint16_t)(((uint32_t)slot_start & 0xFFu) |
                                     (((uint32_t)total_slots & 0xFFu) << 8u));
    return mem_alloc_ast(mem, AST_LAMBDA, pcnt, body_ast, slot_range, def_head);
}

uint16_t parse_lambda_from_ident(Parser *P, const char *id_str, uint16_t id_len) {
    MemArena *mem = P->lex.mem;
    lex_next(&P->lex); /* consume '=>' */

    uint8_t saved_vis  = P->vis_count;
    uint8_t slot_start = P->local_count;
    P->scope_depth++;
    (void)parser_add_local(P, id_str, id_len, mem->fast_real_type);
    if (mem->has_error) {
        P->vis_count = saved_vis;
        P->scope_depth--;
        return MEM_NO_IDX;
    }

    uint16_t body_ast = (P->lex.cur.kind == TOK_LBRACE)
                        ? parse_scope_block(P)
                        : parser_parse_expr(P, 8);
    uint8_t total_slots = (P->local_count > slot_start)
                          ? (uint8_t)(P->local_count - slot_start) : 1u;
    P->vis_count = saved_vis;
    P->scope_depth--;
    P->last_was_block = false;
    if (mem->has_error || body_ast == MEM_NO_IDX) return MEM_NO_IDX;

    record_lambda_src(P, body_ast, (uint16_t)(id_str - P->lex.src));
    uint16_t slot_range = (uint16_t)(((uint32_t)slot_start & 0xFFu) |
                                     (((uint32_t)total_slots & 0xFFu) << 8u));
    return mem_alloc_ast(mem, AST_LAMBDA, 1u, body_ast, slot_range, MEM_NO_IDX);
}

uint16_t parse_inline_fn_def(Parser *P, const char *fn_name, uint16_t fn_len) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    int ce_sid = sym_find(tab, mem, ":=", 2);
    if (ce_sid < 0) {
        mem_err(mem, "Assignment operator ':=' is not active");
        return MEM_NO_IDX;
    }
    uint16_t lhs_ast = resolve_var_for_assign(P, fn_name, fn_len, (uint16_t)ce_sid, 0, 0);
    if (mem->has_error || lhs_ast == MEM_NO_IDX) return MEM_NO_IDX;
    if (mem->fn_type_id != 0) {
        const AstNode *ln = mem_get_ast(mem, lhs_ast);
        if (ln->kind == AST_SLOT_LOAD) {
            P->slot_types[ln->sub & AST_SLOT_IDX_MASK] = mem->fn_type_id;
        } else if (ln->kind == AST_CONST_SYM) {
            /* Pre-register module function variable so recursive calls inside body resolve cleanly */
            int own_idx = sym_find_own_var(tab, ln->ref, P->cur_mod);
            if (own_idx < 0) {
                own_idx = sym_set_or_add_var(tab, mem, ln->ref, P->cur_mod, val_void(),
                                             0, 0,
                                             (uint8_t)(ln->sub | SYM_VAR_MUTABLE), true);
            }
            if (own_idx >= 0) {
                ModVarDef *mv = &sym_vars_mut(tab)[own_idx];
                mv->lock_type = (uint16_t)((mv->lock_type & 0xFFu) | ((uint32_t)mem->fn_type_id << 8u));
            }
        }
    }

    uint16_t paren_start = P->lex.cur.off;
    lex_next(&P->lex); /* consume '(' */

    uint8_t  saved_vis  = P->vis_count;
    uint8_t  slot_start = P->local_count;
    P->scope_depth++;

    uint8_t  pcnt     = 0;
    uint16_t def_head = MEM_NO_IDX;
    if (!parse_fn_param_list_in_scope(P, &pcnt, &def_head)) {
        P->vis_count = saved_vis;
        P->scope_depth--;
        return MEM_NO_IDX;
    }

    if (P->lex.cur.kind != TOK_RPAREN) {
        P->vis_count = saved_vis;
        P->scope_depth--;
        mem_err(mem, "Expected ')' after function parameters");
        return MEM_NO_IDX;
    }
    uint16_t paren_end = (uint16_t)(P->lex.cur.off + P->lex.cur.len);
    lex_next(&P->lex); /* consume ')' */

    if (P->lex.cur.kind != TOK_OP ||
        !hardcode_is_plain_store_sym(mem, tab, P->lex.cur.sym_id, NULL)) {
        P->vis_count = saved_vis;
        P->scope_depth--;
        mem_err(mem, "Expected ':=' or '=' after function header");
        return MEM_NO_IDX;
    }
    uint16_t op_sym_id = P->lex.cur.sym_id;
    lex_next(&P->lex); /* consume ':=' or '=' */

    uint16_t body_start = P->lex.cur.off;
    uint16_t body_ast = (P->lex.cur.kind == TOK_LBRACE)
                        ? parse_scope_block(P)
                        : parser_parse_expr(P, 5);
    uint8_t total_slots = (P->local_count > slot_start)
                          ? (uint8_t)(P->local_count - slot_start) : 0u;
    while (P->vis_count > saved_vis && P->locals[P->vis_count - 1].scope_depth >= P->scope_depth) {
        P->vis_count--;
    }
    P->scope_depth--;
    P->last_was_block = false;
    if (mem->has_error || body_ast == MEM_NO_IDX) return MEM_NO_IDX;

    if (mem->fn_src_count < 64) {
        uint16_t end_off = (P->lex.cur.kind == TOK_EOF) ? P->lex.pos : P->lex.cur.off;
        while (end_off > body_start) {
            char c = P->lex.src[end_off - 1];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') end_off--;
            else break;
        }
        uint16_t plen = (uint16_t)(paren_end - paren_start);
        uint16_t blen = (end_off > body_start) ? (uint16_t)(end_off - body_start) : 0u;
        char lbuf[384];
        if (blen > 0 && (size_t)plen + 4u + (size_t)blen < sizeof(lbuf)) {
            memcpy(lbuf, P->lex.src + paren_start, plen);
            memcpy(lbuf + plen, " => ", 4);
            memcpy(lbuf + plen + 4, P->lex.src + body_start, blen);
            StrSlice s = mem_intern_str(mem, lbuf, (uint16_t)(plen + 4u + blen));
            if (!mem->has_error) {
                mem->fn_body_ast[mem->fn_src_count]  = body_ast;
                mem->fn_src_slice[mem->fn_src_count] = s;
                mem->fn_src_count++;
            }
        }
    }

    uint16_t slot_range = (uint16_t)(((uint32_t)slot_start & 0xFFu) |
                                     (((uint32_t)total_slots & 0xFFu) << 8u));
    uint16_t lam_ast    = mem_alloc_ast(mem, AST_LAMBDA, pcnt, body_ast, slot_range, def_head);
    return build_store_or_compound(P, op_sym_id, lhs_ast, lam_ast);
}

