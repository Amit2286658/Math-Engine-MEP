/*
 * parser_control.c — Conditional Branches, Pattern-Matched Switch, Jumps, Exceptions & Var Resolution
 */
#include "parser_internal.h"

uint16_t parse_if_construct(Parser *P, uint16_t if_sym_id) {
    MemArena *mem = P->lex.mem;
    uint16_t cond1 = MEM_NO_IDX;

    if (P->lex.cur.kind == TOK_LPAREN) {
        lex_next(&P->lex);
        P->in_cond_expr++;
        cond1 = parser_parse_expr(P, 0);
        P->in_cond_expr--;
        if (P->lex.cur.kind == TOK_COMMA) {
            lex_next(&P->lex);
            uint16_t then_val = parser_parse_expr(P, 0);
            if (P->lex.cur.kind != TOK_COMMA) {
                mem_err(mem, "Expected ',' before else-branch in 3-argument if(cond, then, else)");
                return MEM_NO_IDX;
            }
            lex_next(&P->lex);
            uint16_t else_val = parser_parse_expr(P, 0);
            if (P->lex.cur.kind != TOK_RPAREN) {
                mem_err(mem, "Expected ')' after if(cond, then, else)");
            } else {
                lex_next(&P->lex);
            }
            uint16_t l3 = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, else_val, MEM_NO_IDX);
            uint16_t l2 = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, then_val, l3);
            uint16_t l1 = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, cond1, l2);
            return mem_alloc_ast(mem, AST_CALL, 3, if_sym_id, l1, MEM_NO_IDX);
        }
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected ')' after 'if' condition");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
    } else {
        P->in_cond_expr++;
        cond1 = parser_parse_expr(P, 16);
        P->in_cond_expr--;
    }

    if (mem->has_error || cond1 == MEM_NO_IDX) return MEM_NO_IDX;

    uint16_t conds[32];
    uint16_t bodies[32];
    uint8_t  branch_cnt = 0;

    conds[branch_cnt]  = cond1;
    bodies[branch_cnt] = parse_branch_body(P);
    branch_cnt++;

    uint16_t else_body = MEM_NO_IDX;
    bool     seen_else = false;

    while (!mem->has_error) {
        if (cur_op_meta(P) == INTR_SEQ && lex_peek_is_branch_kw(P)) {
            lex_next(&P->lex);
        }

        uint8_t kw_meta = token_func_meta(P, P->lex.cur);
        if (kw_meta == INTR_OR_BRANCH) {
            if (seen_else) {
                mem_err(mem, "'or' branch cannot appear after 'else' block");
                return MEM_NO_IDX;
            }
            if (branch_cnt >= 32) {
                mem_err(mem, "Too many 'or' branches in 'if' statement (max 32)");
                return MEM_NO_IDX;
            }
            lex_next(&P->lex);

            if (P->lex.cur.kind == TOK_LBRACE) {
                mem_err(mem, "Expected condition after 'or' (use 'else { ... }' for the fallback branch)");
                return MEM_NO_IDX;
            }

            uint16_t c_next = MEM_NO_IDX;
            if (P->lex.cur.kind == TOK_LPAREN) {
                lex_next(&P->lex);
                P->in_cond_expr++;
                c_next = parser_parse_expr(P, 0);
                P->in_cond_expr--;
                if (P->lex.cur.kind != TOK_RPAREN) {
                    mem_err(mem, "Expected ')' after 'or' condition");
                    return MEM_NO_IDX;
                }
                lex_next(&P->lex);
            } else {
                P->in_cond_expr++;
                c_next = parser_parse_expr(P, 16);
                P->in_cond_expr--;
            }

            uint16_t b_next = parse_branch_body(P);
            conds[branch_cnt]  = c_next;
            bodies[branch_cnt] = b_next;
            branch_cnt++;
            continue;
        }

        if (kw_meta == INTR_ELSE_BRANCH) {
            if (seen_else) {
                mem_err(mem, "Only one 'else' block is allowed in an 'if' statement");
                return MEM_NO_IDX;
            }
            lex_next(&P->lex);

            if (token_func_meta(P, P->lex.cur) == INTR_IF) {
                mem_err(mem, "Only one 'if' is allowed per chain; use 'or (cond) { ... }' for else-if branches");
                return MEM_NO_IDX;
            }

            else_body = parse_branch_body(P);
            seen_else = true;
            continue;
        }

        break;
    }

    uint16_t acc = else_body;
    for (int i = (int)branch_cnt - 1; i >= 0; --i) {
        uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, bodies[i], acc);
        acc = mem_alloc_ast(mem, AST_TERNARY, 0, 0, conds[i], link);
    }
    P->last_was_block = true;
    return acc;
}

static bool tok_is_underscore(const Parser *P, Token t) {
    return (t.len == 1 && P->lex.src[t.off] == '_' &&
            (t.kind == TOK_IDENT || t.kind == TOK_OP));
}

static bool tok_is_bindable_ident(const Parser *P, Token t) {
    if (t.kind == TOK_IDENT || tok_is_underscore(P, t)) return true;
    if (t.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[t.off]) &&
        sym_get_active_prec(P->lex.tab, t.sym_id, FIX_PREFIX) < 0) {
        return true;
    }
    return false;
}

static bool is_pattern_bind_ident(const Parser *P, const char *name, uint16_t len, uint8_t end_syn) {
    const MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;

    if (!lex_at_is_syntax(&P->lex, P->lex.pos, INTR_COMMA) &&
        !lex_at_is_syntax(&P->lex, P->lex.pos, end_syn)) {
        return false;
    }
    if (len == 1 && name[0] == '_') return true;
    if (find_local_idx(P, name, len) >= 0) return false;
    int sid = sym_find(tab, mem, name, len);
    if (sid >= 0) {
        if (sym_find_var(tab, (uint16_t)sid, P->cur_mod, NULL) >= 0) {
            return false;
        }
    }
    return true;
}

static uint16_t parse_single_switch_pattern(Parser *P, uint16_t slot_ast, uint8_t sw_slot,
                                            int cand_sym) {
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;

    /* 1. Relational guard pattern: `case > 10 :`, `case <= 0 :`, `case != 5 :`, `case ~ 3.14 :` */
    if (P->lex.cur.kind == TOK_OP && hardcode_is_rel_comparison_op(mem, tab, P->lex.cur.sym_id)) {
        uint16_t op_sym = P->lex.cur.sym_id;
        lex_next(&P->lex);
        P->in_ternary_then++;
        uint16_t rhs_ast = parser_parse_expr(P, 35);
        P->in_ternary_then--;
        if (mem->has_error || rhs_ast == MEM_NO_IDX) return MEM_NO_IDX;
        return mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT, op_sym, slot_ast, rhs_ast);
    }

    /* 2. Struct destructuring pattern: `case vec2(0, y) :`, `case vec3(x, 0, z) :` */
    if (P->lex.cur.kind == TOK_IDENT && lex_peek_is_lparen(&P->lex)) {
        const char *tname = P->lex.src + P->lex.cur.off;
        uint16_t    tlen  = P->lex.cur.len;
        uint16_t    st_tid = sym_find_type(tab, mem, tname, tlen);
        if (st_tid != MEM_TYPE_VOID && st_tid < tab->type_count &&
            !tab->types[st_tid].is_consuming && tab->types[st_tid].parser_intr == INTR_NONE &&
            tab->types[st_tid].field_count > 0) {
            const TypeDef *td = &tab->types[st_tid];
            lex_next(&P->lex); /* consume struct type name */
            lex_next(&P->lex); /* consume '(' */

            uint16_t cond = mem_alloc_ast(mem, AST_SWITCH_EQ, 1, st_tid, slot_ast, MEM_NO_IDX);
            uint8_t  f_idx = 0;

            while (!mem->has_error && P->lex.cur.kind != TOK_RPAREN && f_idx < td->field_count) {
                const FieldDef *fd = &tab->fields[td->start_idx + f_idx];
                uint16_t field_ast = mem_alloc_ast(mem, AST_FIELD_LOAD, (uint8_t)fd->type_id,
                                                   fd->flat_offset, slot_ast, sw_slot);
                uint16_t step_cond = MEM_NO_IDX;

                if (tok_is_bindable_ident(P, P->lex.cur) &&
                    is_pattern_bind_ident(P, P->lex.src + P->lex.cur.off, P->lex.cur.len, INTR_RPAREN)) {
                    const char *vname = P->lex.src + P->lex.cur.off;
                    uint16_t    vlen  = P->lex.cur.len;
                    lex_next(&P->lex);
                    if (!(vlen == 1 && vname[0] == '_')) {
                        int pslot = parser_add_local(P, vname, vlen, fd->type_id);
                        if (mem->has_error) return MEM_NO_IDX;
                        step_cond = mem_alloc_ast(mem, AST_SWITCH_EQ, 2, (uint16_t)pslot, field_ast, MEM_NO_IDX);
                    }
                } else {
                    P->in_ternary_then++;
                    uint16_t f_expr = parser_parse_expr(P, 11);
                    P->in_ternary_then--;
                    if (mem->has_error || f_expr == MEM_NO_IDX) return MEM_NO_IDX;
                    step_cond = mem_alloc_ast(mem, AST_SWITCH_EQ, 0, 0, field_ast, f_expr);
                }

                if (step_cond != MEM_NO_IDX && cand_sym >= 0) {
                    cond = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                         (uint16_t)cand_sym, cond, step_cond);
                }
                f_idx++;
                if (P->lex.cur.kind == TOK_COMMA) {
                    lex_next(&P->lex);
                } else {
                    break;
                }
            }

            if (P->lex.cur.kind != TOK_RPAREN) {
                mem_err(mem, "Expected ')' after struct pattern in 'case'");
                return MEM_NO_IDX;
            }
            lex_next(&P->lex);
            return cond;
        }
    }

    /* 3. Vector / 1D Matrix destructuring pattern: `case [0, y] :`, `case [a, b] :` */
    if (P->lex.cur.kind == TOK_LBRACK) {
        lex_next(&P->lex); /* consume '[' */
        uint16_t elem_checks[16];
        uint8_t  elem_cnt = 0;

        while (!mem->has_error && P->lex.cur.kind != TOK_RBRACK && elem_cnt < 16) {
            uint16_t k_pidx = mem_alloc_pool(mem, 1);
            if (mem->has_error) return MEM_NO_IDX;
            mem_write_pool(mem, k_pidx, (double)elem_cnt);
            uint16_t k_lit    = mem_alloc_ast(mem, AST_LIT, mem->fast_real_type, k_pidx, 0, 0);
            uint16_t k_link   = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, k_lit, MEM_NO_IDX);
            uint16_t elem_ast = mem_alloc_ast(mem, AST_INDEX, 1, 0, slot_ast, k_link);

            if (tok_is_bindable_ident(P, P->lex.cur) &&
                is_pattern_bind_ident(P, P->lex.src + P->lex.cur.off, P->lex.cur.len, INTR_RBRACK)) {
                const char *vname = P->lex.src + P->lex.cur.off;
                uint16_t    vlen  = P->lex.cur.len;
                lex_next(&P->lex);
                if (vlen == 1 && vname[0] == '_') {
                    elem_checks[elem_cnt] = MEM_NO_IDX;
                } else {
                    int pslot = parser_add_local(P, vname, vlen, mem->fast_real_type);
                    if (mem->has_error) return MEM_NO_IDX;
                    elem_checks[elem_cnt] = mem_alloc_ast(mem, AST_SWITCH_EQ, 2, (uint16_t)pslot, elem_ast, MEM_NO_IDX);
                }
            } else {
                P->in_ternary_then++;
                uint16_t e_expr = parser_parse_expr(P, 11);
                P->in_ternary_then--;
                if (mem->has_error || e_expr == MEM_NO_IDX) return MEM_NO_IDX;
                elem_checks[elem_cnt] = mem_alloc_ast(mem, AST_SWITCH_EQ, 0, 0, elem_ast, e_expr);
            }

            elem_cnt++;
            if (P->lex.cur.kind == TOK_COMMA) {
                lex_next(&P->lex);
            } else {
                break;
            }
        }

        if (P->lex.cur.kind != TOK_RBRACK) {
            mem_err(mem, "Expected ']' after vector pattern in 'case'");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);

        uint16_t cond = mem_alloc_ast(mem, AST_SWITCH_EQ, 3, elem_cnt, slot_ast, MEM_NO_IDX);
        for (uint8_t k = 0; k < elem_cnt; ++k) {
            if (elem_checks[k] != MEM_NO_IDX && cand_sym >= 0) {
                cond = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                     (uint16_t)cand_sym, cond, elem_checks[k]);
            }
        }
        return cond;
    }

    /* 4. Wildcard `_` or Standard Value / Range Pattern (`case 1..5 :`, `case 42 :`) */
    if (tok_is_underscore(P, P->lex.cur) &&
        (lex_at_is_syntax(&P->lex, P->lex.pos, INTR_COMMA) ||
         cur_op_meta(P) == INTR_COLON ||
         (P->lex.pos < P->lex.len && P->lex.src[skip_raw_ws(P->lex.src, P->lex.pos, P->lex.len)] == ':'))) {
        lex_next(&P->lex);
        uint16_t one_pidx = mem_alloc_pool(mem, 1);
        if (mem->has_error) return MEM_NO_IDX;
        mem_write_pool(mem, one_pidx, 1.0);
        return mem_alloc_ast(mem, AST_LIT, mem->fast_real_type, one_pidx, 0, 0);
    }

    P->in_ternary_then++;
    uint16_t v_ast = parser_parse_expr(P, 11);
    P->in_ternary_then--;
    if (mem->has_error || v_ast == MEM_NO_IDX) return MEM_NO_IDX;
    return mem_alloc_ast(mem, AST_SWITCH_EQ, 0, 0, slot_ast, v_ast);
}

uint16_t parse_switch_construct(Parser *P) {
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;

    int store_sym = find_active_meta_sym(tab, INTR_STORE);
    int seq_sym   = find_active_meta_sym(tab, INTR_SEQ);
    int cand_sym  = find_active_meta_sym(tab, INTR_CAND);
    if (store_sym < 0 || seq_sym < 0) {
        mem_err(mem, "Switch requires active assignment and sequence operators in [boot_control]");
        return MEM_NO_IDX;
    }

    uint16_t target_expr = MEM_NO_IDX;
    if (P->lex.cur.kind == TOK_LPAREN) {
        lex_next(&P->lex);
        P->in_cond_expr++;
        target_expr = parser_parse_expr(P, 0);
        P->in_cond_expr--;
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected ')' after 'switch' expression");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
    } else {
        P->in_cond_expr++;
        target_expr = parser_parse_expr(P, 2);
        P->in_cond_expr--;
    }

    if (P->lex.cur.kind != TOK_LBRACE) {
        mem_err(mem, "Expected '{' to open 'switch' block");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex);

    uint8_t  sw_slot  = parser_alloc_anon_slot(P, mem->fast_real_type);
    uint16_t slot_ast = mem_alloc_ast(mem, AST_SLOT_LOAD, sw_slot, 0, 0, 0);
    uint16_t init_ast = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_RIGHT,
                                      (uint16_t)store_sym, slot_ast, target_expr);

    uint16_t conds[32];
    uint16_t bodies[32];
    uint8_t  case_cnt     = 0;
    uint16_t default_body = MEM_NO_IDX;
    bool     seen_default = false;

    while (!mem->has_error && P->lex.cur.kind != TOK_RBRACE && P->lex.cur.kind != TOK_EOF) {
        while (cur_op_meta(P) == INTR_SEQ || P->lex.cur.kind == TOK_COMMA) {
            lex_next(&P->lex);
        }
        if (P->lex.cur.kind == TOK_RBRACE || P->lex.cur.kind == TOK_EOF) break;

        uint8_t kw_meta = token_func_meta(P, P->lex.cur);
        bool is_default_kw = (P->lex.cur.kind == TOK_IDENT && P->lex.cur.len == 7 &&
                              memcmp(P->lex.src + P->lex.cur.off, "default", 7) == 0);
        if (kw_meta == INTR_ELSE_BRANCH || is_default_kw) {
            if (seen_default) {
                mem_err(mem, "Only one 'else'/'default' branch is allowed in a 'switch' block");
                return MEM_NO_IDX;
            }
            lex_next(&P->lex);
            if (cur_op_meta(P) == INTR_COLON) {
                lex_next(&P->lex);
            }
            default_body = parse_branch_body(P);
            seen_default = true;
            continue;
        }

        if (kw_meta == INTR_CASE_BRANCH) {
            lex_next(&P->lex);
        }

        uint8_t case_saved_vis = P->vis_count;
        P->scope_depth++;

        uint16_t case_conds[8];
        uint8_t  val_cnt = 0;
        while (!mem->has_error && val_cnt < 8) {
            uint16_t c_ast = parse_single_switch_pattern(P, slot_ast, sw_slot, cand_sym);
            if (mem->has_error || c_ast == MEM_NO_IDX) {
                P->vis_count = case_saved_vis;
                P->scope_depth--;
                return MEM_NO_IDX;
            }
            case_conds[val_cnt++] = c_ast;
            if (P->lex.cur.kind == TOK_COMMA) {
                lex_next(&P->lex);
            } else {
                break;
            }
        }

        /* Optional `if <guard_expr>` clause on `case ... if <guard> :` */
        if (token_func_meta(P, P->lex.cur) == INTR_IF) {
            lex_next(&P->lex); /* consume 'if' */
            P->in_ternary_then++;
            uint16_t guard_ast = parser_parse_expr(P, 11);
            P->in_ternary_then--;
            if (mem->has_error || guard_ast == MEM_NO_IDX) {
                P->vis_count = case_saved_vis;
                P->scope_depth--;
                return MEM_NO_IDX;
            }
            if (cand_sym >= 0) {
                for (uint8_t k = 0; k < val_cnt; ++k) {
                    case_conds[k] = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                                  (uint16_t)cand_sym, case_conds[k], guard_ast);
                }
            }
        }

        if (cur_op_meta(P) == INTR_COLON) {
            lex_next(&P->lex);
        }

        uint16_t body_ast = parse_branch_body(P);
        P->vis_count = case_saved_vis;
        P->scope_depth--;

        for (uint8_t k = 0; k < val_cnt; ++k) {
            if (case_cnt >= 32) {
                mem_err(mem, "Too many cases in 'switch' block (max 32)");
                return MEM_NO_IDX;
            }
            conds[case_cnt]  = case_conds[k];
            bodies[case_cnt] = body_ast;
            case_cnt++;
        }
    }

    if (P->lex.cur.kind != TOK_RBRACE) {
        mem_err(mem, "Expected closing '}' for 'switch' block");
    } else {
        lex_next(&P->lex);
    }

    uint16_t acc = default_body;
    for (int i = (int)case_cnt - 1; i >= 0; --i) {
        uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, bodies[i], acc);
        acc = mem_alloc_ast(mem, AST_TERNARY, (uint8_t)(i == 0 ? 1 : 0), 0, conds[i], link);
    }
    if (acc == MEM_NO_IDX) {
        acc = mem_alloc_ast(mem, AST_LIT, MEM_TYPE_VOID, 0, 0, 0);
    }

    P->last_was_block = true;
    return mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                         (uint16_t)seq_sym, init_ast, acc);
}

uint16_t parse_tag_op(Parser *P, AstKind kind, const char *op_name) {
    MemArena *mem = P->lex.mem;
    if (P->lex.cur.kind != TOK_LPAREN) {
        mem_err(mem, "Expected '(' after '%s'", op_name);
        return MEM_NO_IDX;
    }
    lex_next(&P->lex);
    if (P->lex.cur.kind != TOK_IDENT &&
        !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
        mem_err(mem, "Expected scope tag name inside '%s(...)'", op_name);
        return MEM_NO_IDX;
    }
    const char *tname = P->lex.src + P->lex.cur.off;
    uint16_t    tlen  = P->lex.cur.len;
    int label_id = parser_get_or_add_label(P, tname, tlen);
    if (mem->has_error) return MEM_NO_IDX;
    lex_next(&P->lex);

    if (P->lex.cur.kind != TOK_RPAREN) {
        mem_err(mem, "Expected ')' after scope tag in '%s(%.*s)'", op_name, (int)tlen, tname);
        return MEM_NO_IDX;
    }
    lex_next(&P->lex);
    return mem_alloc_ast(mem, (uint8_t)kind, (uint8_t)label_id, 0, MEM_NO_IDX, MEM_NO_IDX);
}

static bool can_follow_jump_value_expr(const Parser *P) {
    if (P->lex.cur.had_newline) return false;
    TokKind k = P->lex.cur.kind;
    if (k == TOK_EOF || k == TOK_RBRACE || k == TOK_RPAREN || k == TOK_RBRACK || k == TOK_COMMA) {
        return false;
    }
    if (cur_op_meta(P) == INTR_SEQ || cur_op_meta(P) == INTR_COLON) {
        return false;
    }
    uint8_t kw = token_func_meta(P, P->lex.cur);
    if (kw == INTR_OR_BRANCH || kw == INTR_ELSE_BRANCH || kw == INTR_CASE_BRANCH) {
        return false;
    }
    if (is_catch_kw(P->lex.src + P->lex.cur.off, P->lex.cur.len)) {
        return false;
    }
    return true;
}

uint16_t parse_break_construct(Parser *P) {
    MemArena *mem = P->lex.mem;
    if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN && !hardcode_is_paren_lambda(P)) {
        lex_next(&P->lex);
        if (P->lex.cur.kind == TOK_RPAREN) {
            lex_next(&P->lex);
            return mem_alloc_ast(mem, AST_BREAK, 0xFFu, 0, MEM_NO_IDX, MEM_NO_IDX);
        }
        /* Check if the first token inside `break(...)` is a scope/loop tag followed by `)` or `,` */
        if ((P->lex.cur.kind == TOK_IDENT ||
             (P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) &&
            (lex_at_is_syntax(&P->lex, P->lex.pos, INTR_RPAREN) ||
             lex_at_is_syntax(&P->lex, P->lex.pos, INTR_COMMA))) {
            const char *tname = P->lex.src + P->lex.cur.off;
            uint16_t    tlen  = P->lex.cur.len;
            bool is_tag = (find_label_id(P, tname, tlen) >= 0) ||
                          (find_local_idx(P, tname, tlen) < 0 &&
                           sym_find(P->lex.tab, mem, tname, tlen) < 0) ||
                          lex_at_is_syntax(&P->lex, P->lex.pos, INTR_COMMA);
            if (is_tag) {
                int label_id = parser_get_or_add_label(P, tname, tlen);
                if (mem->has_error) return MEM_NO_IDX;
                lex_next(&P->lex);
                uint16_t val_ast = MEM_NO_IDX;
                if (P->lex.cur.kind == TOK_COMMA) {
                    lex_next(&P->lex);
                    val_ast = parser_parse_expr(P, 0);
                    if (mem->has_error || val_ast == MEM_NO_IDX) return MEM_NO_IDX;
                }
                if (P->lex.cur.kind != TOK_RPAREN) {
                    mem_err(mem, "Expected ')' after 'break(%.*s...)'", (int)tlen, tname);
                    return MEM_NO_IDX;
                }
                lex_next(&P->lex);
                if (val_ast == MEM_NO_IDX && can_follow_jump_value_expr(P)) {
                    val_ast = parser_parse_expr(P, 2);
                }
                return mem_alloc_ast(mem, AST_BREAK, (uint8_t)label_id, 0, val_ast, MEM_NO_IDX);
            }
        }
        /* Otherwise `break(expr)` is a value-returning anonymous break */
        uint16_t val_ast = parser_parse_expr(P, 0);
        if (mem->has_error || val_ast == MEM_NO_IDX) return MEM_NO_IDX;
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected ')' after break value expression");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_BREAK, 0xFFu, 0, val_ast, MEM_NO_IDX);
    }

    if (can_follow_jump_value_expr(P)) {
        if (P->lex.cur.kind == TOK_IDENT && token_func_meta(P, P->lex.cur) == 0 &&
            find_label_id(P, P->lex.src + P->lex.cur.off, P->lex.cur.len) >= 0) {
            int label_id = find_label_id(P, P->lex.src + P->lex.cur.off, P->lex.cur.len);
            lex_next(&P->lex);
            uint16_t val_ast = MEM_NO_IDX;
            if (can_follow_jump_value_expr(P)) {
                val_ast = parser_parse_expr(P, 2);
            }
            return mem_alloc_ast(mem, AST_BREAK, (uint8_t)label_id, 0, val_ast, MEM_NO_IDX);
        }
        uint16_t val_ast = parser_parse_expr(P, 2);
        return mem_alloc_ast(mem, AST_BREAK, 0xFFu, 0, val_ast, MEM_NO_IDX);
    }

    return mem_alloc_ast(mem, AST_BREAK, 0xFFu, 0, MEM_NO_IDX, MEM_NO_IDX);
}

uint16_t parse_continue_construct(Parser *P) {
    MemArena *mem = P->lex.mem;
    if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN) {
        lex_next(&P->lex);
        if (P->lex.cur.kind == TOK_RPAREN) {
            lex_next(&P->lex);
            return mem_alloc_ast(mem, AST_CONTINUE, 0xFFu, 0, MEM_NO_IDX, MEM_NO_IDX);
        }
        if (P->lex.cur.kind != TOK_IDENT &&
            !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
            mem_err(mem, "Expected loop tag name inside 'continue(...)'");
            return MEM_NO_IDX;
        }
        const char *tname = P->lex.src + P->lex.cur.off;
        uint16_t    tlen  = P->lex.cur.len;
        int label_id = parser_get_or_add_label(P, tname, tlen);
        if (mem->has_error) return MEM_NO_IDX;
        lex_next(&P->lex);
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected ')' after tag in 'continue(%.*s)'", (int)tlen, tname);
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_CONTINUE, (uint8_t)label_id, 0, MEM_NO_IDX, MEM_NO_IDX);
    }

    if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_IDENT &&
        token_func_meta(P, P->lex.cur) == 0 &&
        find_label_id(P, P->lex.src + P->lex.cur.off, P->lex.cur.len) >= 0) {
        int label_id = find_label_id(P, P->lex.src + P->lex.cur.off, P->lex.cur.len);
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_CONTINUE, (uint8_t)label_id, 0, MEM_NO_IDX, MEM_NO_IDX);
    }

    return mem_alloc_ast(mem, AST_CONTINUE, 0xFFu, 0, MEM_NO_IDX, MEM_NO_IDX);
}

uint16_t parse_return_construct(Parser *P) {
    MemArena *mem = P->lex.mem;
    if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN &&
        !hardcode_is_paren_lambda(P) &&
        lex_at_is_syntax(&P->lex, P->lex.pos, INTR_RPAREN)) {
        lex_next(&P->lex);
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_RETURN, 0, 0, MEM_NO_IDX, MEM_NO_IDX);
    }

    if (can_follow_jump_value_expr(P)) {
        uint16_t val_ast = parser_parse_expr(P, 2);
        return mem_alloc_ast(mem, AST_RETURN, 0, 0, val_ast, MEM_NO_IDX);
    }

    return mem_alloc_ast(mem, AST_RETURN, 0, 0, MEM_NO_IDX, MEM_NO_IDX);
}

uint16_t parse_throw_construct(Parser *P) {
    MemArena *mem = P->lex.mem;
    if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN &&
        !hardcode_is_paren_lambda(P) &&
        lex_at_is_syntax(&P->lex, P->lex.pos, INTR_RPAREN)) {
        lex_next(&P->lex);
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_RETURN, 1, 0, MEM_NO_IDX, MEM_NO_IDX);
    }

    if (can_follow_jump_value_expr(P)) {
        uint16_t val_ast = parser_parse_expr(P, 2);
        return mem_alloc_ast(mem, AST_RETURN, 1, 0, val_ast, MEM_NO_IDX);
    }

    mem_err(mem, "Expected exception value or message after 'throw'");
    return MEM_NO_IDX;
}

uint16_t parse_try_construct(Parser *P) {
    MemArena *mem = P->lex.mem;
    uint16_t try_body = parse_branch_body(P);
    if (mem->has_error || try_body == MEM_NO_IDX) return MEM_NO_IDX;

    if (cur_op_meta(P) == INTR_SEQ && lex_peek_is_branch_kw(P)) {
        lex_next(&P->lex);
    }

    if ((P->lex.cur.kind != TOK_IDENT && P->lex.cur.kind != TOK_OP) ||
        !is_catch_kw(P->lex.src + P->lex.cur.off, P->lex.cur.len)) {
        mem_err(mem, "Expected 'catch' block after 'try' block");
        return MEM_NO_IDX;
    }
    lex_next(&P->lex); /* consume 'catch' */

    uint8_t  saved_vis     = P->vis_count;
    uint16_t catch_var_ast = MEM_NO_IDX;
    P->scope_depth++;

    if (P->lex.cur.kind == TOK_LPAREN) {
        lex_next(&P->lex); /* consume '(' */
        if (P->lex.cur.kind != TOK_RPAREN) {
            if (P->lex.cur.kind != TOK_IDENT &&
                !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                P->vis_count = saved_vis;
                P->scope_depth--;
                mem_err(mem, "Expected error variable identifier inside 'catch(...)'");
                return MEM_NO_IDX;
            }
            const char *ename = P->lex.src + P->lex.cur.off;
            uint16_t    elen  = P->lex.cur.len;
            lex_next(&P->lex);

            uint8_t c_ltype = 0, c_lunit = 0;
            if (cur_op_meta(P) == INTR_COLON) {
                if (!parse_var_type_unit_annot(P, &c_ltype, &c_lunit)) {
                    P->vis_count = saved_vis;
                    P->scope_depth--;
                    return MEM_NO_IDX;
                }
            }
            uint16_t init_tid = c_ltype ? c_ltype : (mem->str_type_id ? mem->str_type_id : mem->fast_real_type);
            int c_slot = parser_add_local(P, ename, elen, init_tid);
            if (mem->has_error) {
                P->vis_count = saved_vis;
                P->scope_depth--;
                return MEM_NO_IDX;
            }
            catch_var_ast = mem_alloc_ast(mem, AST_SLOT_LOAD, (uint8_t)c_slot, 0, c_ltype, c_lunit);
        }
        if (P->lex.cur.kind != TOK_RPAREN) {
            P->vis_count = saved_vis;
            P->scope_depth--;
            mem_err(mem, "Expected ')' after 'catch(...)' parameter");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex); /* consume ')' */
    } else if ((P->lex.cur.kind == TOK_IDENT ||
                (P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) &&
               lex_peek_is_lbrace(&P->lex)) {
        const char *ename = P->lex.src + P->lex.cur.off;
        uint16_t    elen  = P->lex.cur.len;
        lex_next(&P->lex);
        uint16_t init_tid = mem->str_type_id ? mem->str_type_id : mem->fast_real_type;
        int c_slot = parser_add_local(P, ename, elen, init_tid);
        if (mem->has_error) {
            P->vis_count = saved_vis;
            P->scope_depth--;
            return MEM_NO_IDX;
        }
        catch_var_ast = mem_alloc_ast(mem, AST_SLOT_LOAD, (uint8_t)c_slot, 0, 0, 0);
    }

    uint16_t catch_body = parse_branch_body(P);
    P->vis_count = saved_vis;
    P->scope_depth--;
    if (mem->has_error || catch_body == MEM_NO_IDX) return MEM_NO_IDX;

    uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, catch_var_ast, catch_body);
    P->last_was_block = true;
    return mem_alloc_ast(mem, AST_TERNARY, 2, 0, try_body, link);
}

uint16_t resolve_qual_mod_assign(Parser *P, uint16_t qual_mod,
                                        const char *name, uint16_t len,
                                        uint16_t op_sym_id,
                                        uint8_t lock_type, uint8_t lock_unit) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    if (sym_is_reserved_kw(tab, mem, name, len) || is_var_modifier_kw(name, len)) {
        mem_err(mem, "Cannot use reserved keyword '%.*s' as a variable name",
                (int)len, name);
        return MEM_NO_IDX;
    }

    bool is_colon_eq = false;
    bool is_plain    = hardcode_is_plain_store_sym(mem, tab, op_sym_id, &is_colon_eq);

    StrSlice vslice = mem_intern_str(mem, name, len);
    uint16_t sym_id = sym_intern(tab, mem, vslice);
    if (mem->has_error) return MEM_NO_IDX;

    int own_idx = sym_find_own_var(tab, sym_id, qual_mod);
    if (own_idx >= 0) {
        const ModVarDef *mv = &sym_vars(tab)[own_idx];
        if ((mv->flags & SYM_VAR_PRIVATE) != 0 && qual_mod != P->cur_mod) {
            StrSlice pmod_s = sym_mod_name(tab, qual_mod);
            mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                    (int)len, name, (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
            return MEM_NO_IDX;
        }
        if ((mv->flags & SYM_VAR_MUTABLE) == 0) {
            mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                    (int)len, name);
            return MEM_NO_IDX;
        }
    }

    uint8_t ast_flags = SYM_VAR_QUAL_MOD;
    if (is_plain) {
        if (is_colon_eq) {
            ast_flags |= SYM_VAR_MUTABLE;
            if (own_idx < 0) {
                (void)sym_set_or_add_var(tab, mem, sym_id, qual_mod, val_void(),
                                         lock_type, lock_unit, ast_flags, true);
                if (mem->has_error) return MEM_NO_IDX;
            }
        } else if (own_idx >= 0) {
            ast_flags |= SYM_VAR_REASSIGN_ONLY;
        } else if (qual_mod == P->cur_mod && P->is_mod_scope && P->scope_depth == 0) {
            ast_flags |= SYM_VAR_EXPLICIT_CONST;
            (void)sym_set_or_add_var(tab, mem, sym_id, qual_mod, val_void(),
                                     lock_type, lock_unit, ast_flags, true);
            if (mem->has_error) return MEM_NO_IDX;
        } else {
            StrSlice qmod_s = sym_mod_name(tab, qual_mod);
            mem_err(mem, "Undefined variable '%.*s' in module '[%.*s]'",
                    (int)len, name, (int)qmod_s.len, mem_str_ptr(mem, qmod_s));
            return MEM_NO_IDX;
        }
    } else {
        if (own_idx < 0) {
            StrSlice qmod_s = sym_mod_name(tab, qual_mod);
            mem_err(mem, "Cannot apply compound assignment to undeclared variable '%.*s::%.*s'",
                    (int)qmod_s.len, mem_str_ptr(mem, qmod_s), (int)len, name);
            return MEM_NO_IDX;
        }
        ast_flags |= (uint8_t)(SYM_VAR_REASSIGN_ONLY | SYM_VAR_COMPOUND_OP);
    }

    uint16_t packed_lock = (uint16_t)(((uint16_t)lock_type & 0xFFu) |
                                      (((uint16_t)lock_unit & 0xFFu) << 8u));
    return mem_alloc_ast(mem, AST_CONST_SYM, ast_flags, sym_id, packed_lock, qual_mod);
}

uint16_t resolve_var_for_assign(Parser *P, const char *name, uint16_t len,
                                       uint16_t op_sym_id,
                                       uint8_t lock_type, uint8_t lock_unit) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    if (sym_is_reserved_kw(tab, mem, name, len) || is_var_modifier_kw(name, len)) {
        mem_err(mem, "Cannot use reserved keyword '%.*s' as a variable name",
                (int)len, name);
        return MEM_NO_IDX;
    }

    uint8_t mod_flags = P->pending_var_flags;
    bool    had_mods  = P->has_pending_mods;
    P->pending_var_flags = 0;
    P->has_pending_mods  = false;

    bool is_colon_eq = false;
    bool is_plain    = hardcode_is_plain_store_sym(mem, tab, op_sym_id, &is_colon_eq);

    /* Case A: Module-level variable or constant (`P->is_mod_scope && P->scope_depth == 0`) */
    if (P->is_mod_scope && P->scope_depth == 0) {
        StrSlice vslice = mem_intern_str(mem, name, len);
        uint16_t sym_id = sym_intern(tab, mem, vslice);
        if (mem->has_error) return MEM_NO_IDX;

        uint8_t ast_flags = (uint8_t)(mod_flags & (SYM_VAR_PRIVATE | SYM_VAR_EXPLICIT_CONST));
        if (is_plain) {
            if ((mod_flags & SYM_VAR_EXPLICIT_CONST) != 0) {
                ast_flags |= SYM_VAR_EXPLICIT_CONST;
            } else if (is_colon_eq || (mod_flags & SYM_VAR_MUTABLE) != 0) {
                ast_flags |= SYM_VAR_MUTABLE;
            } else {
                /* Plain `=` at module top level:
                 * - If `name` is already a visible mutable variable and no `pub`/`priv`/`: type`
                 *   was given, mutate that variable (`SYM_VAR_REASSIGN_ONLY`).
                 * - Otherwise, define an immutable module constant (or fail if reassigning a constant). */
                uint16_t priv_mod = MEM_NO_IDX;
                int vis_idx = sym_find_var(tab, sym_id, P->cur_mod, &priv_mod);
                if (vis_idx >= 0 && !had_mods && lock_type == 0 && lock_unit == 0) {
                    ast_flags |= SYM_VAR_REASSIGN_ONLY;
                } else if (priv_mod != MEM_NO_IDX && !had_mods && lock_type == 0 && lock_unit == 0) {
                    ast_flags |= SYM_VAR_REASSIGN_ONLY;
                } else {
                    ast_flags |= SYM_VAR_EXPLICIT_CONST;
                }
            }
        } else {
            ast_flags |= (uint8_t)(SYM_VAR_REASSIGN_ONLY | SYM_VAR_COMPOUND_OP);
        }

        if ((ast_flags & SYM_VAR_REASSIGN_ONLY) == 0) {
            int own_idx = sym_find_own_var(tab, sym_id, P->cur_mod);
            if (own_idx >= 0) {
                ModVarDef *mv = &sym_vars_mut(tab)[own_idx];
                if ((mv->flags & SYM_VAR_MUTABLE) == 0) {
                    mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                            (int)len, name);
                    return MEM_NO_IDX;
                }
            } else {
                (void)sym_set_or_add_var(tab, mem, sym_id, P->cur_mod, val_void(),
                                         lock_type, lock_unit, ast_flags, true);
                if (mem->has_error) return MEM_NO_IDX;
            }
        } else {
            uint16_t priv_mod = MEM_NO_IDX;
            int vis_idx = sym_find_var(tab, sym_id, P->cur_mod, &priv_mod);
            if (vis_idx >= 0 && (sym_vars(tab)[vis_idx].flags & SYM_VAR_MUTABLE) == 0) {
                mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                        (int)len, name);
                return MEM_NO_IDX;
            }
            if (vis_idx < 0 && priv_mod != MEM_NO_IDX) {
                StrSlice pmod_s = sym_mod_name(tab, priv_mod);
                mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                        (int)len, name, (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
                return MEM_NO_IDX;
            }
            if (vis_idx < 0 && (ast_flags & SYM_VAR_COMPOUND_OP) != 0) {
                mem_err(mem, "Cannot apply compound assignment to undeclared variable '%.*s'",
                        (int)len, name);
                return MEM_NO_IDX;
            }
        }

        uint16_t packed_lock = (uint16_t)(((uint16_t)lock_type & 0xFFu) |
                                          (((uint16_t)lock_unit & 0xFFu) << 8u));
        return mem_alloc_ast(mem, AST_CONST_SYM, ast_flags, sym_id, packed_lock, P->cur_mod);
    }

    /* Case B: Local block/function scope (`P->scope_depth > 0`) or stateless one-shot `eng_eval` */
    if ((mod_flags & SYM_VAR_PRIVATE) != 0 && P->scope_depth > 0) {
        mem_err(mem, "Modifier 'priv' is only valid on module-level declarations");
        return MEM_NO_IDX;
    }

    int vidx = find_local_idx(P, name, len);

    if (is_plain) {
        /* If plain `=` (not `:=`) and not a local variable, check if it targets a module variable/constant */
        if (!is_colon_eq && !had_mods && vidx < 0) {
            int sid = sym_find(tab, mem, name, len);
            if (sid >= 0) {
                uint16_t priv_mod = MEM_NO_IDX;
                int mv_idx = sym_find_var(tab, (uint16_t)sid, P->cur_mod, &priv_mod);
                if (mv_idx >= 0 || priv_mod != MEM_NO_IDX) {
                    uint16_t packed_lock = (uint16_t)(((uint16_t)lock_type & 0xFFu) |
                                                      (((uint16_t)lock_unit & 0xFFu) << 8u));
                    return mem_alloc_ast(mem, AST_CONST_SYM, SYM_VAR_REASSIGN_ONLY,
                                         (uint16_t)sid, packed_lock, P->cur_mod);
                }
            }
        }

        /* At scope_depth == 0 in stateless eval, prevent `:=` from overwriting an immutable constant of cur_mod */
        if (P->scope_depth == 0) {
            int sid = sym_find(tab, mem, name, len);
            if (sid >= 0) {
                int own_idx = sym_find_own_var(tab, (uint16_t)sid, P->cur_mod);
                if (own_idx >= 0 && (sym_vars(tab)[own_idx].flags & SYM_VAR_MUTABLE) == 0) {
                    mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                            (int)len, name);
                    return MEM_NO_IDX;
                }
            }
        }

        uint8_t slot = 0;
        uint16_t init_tid = lock_type ? lock_type : mem->fast_real_type;
        if (vidx < 0 || (is_colon_eq && P->scope_depth > P->locals[vidx].scope_depth)) {
            slot = (uint8_t)parser_add_local(P, name, len, init_tid);
            if (mem->has_error) return MEM_NO_IDX;
            vidx = find_local_idx(P, name, len);
        } else {
            if (P->locals[vidx].is_const) {
                mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                        (int)len, name);
                return MEM_NO_IDX;
            }
            P->locals[vidx].is_loop_iter = 0;
            slot = P->locals[vidx].slot;
            if (lock_type != 0) {
                P->slot_types[slot] = lock_type;
            }
        }
        uint8_t sub_byte = slot;
        if ((mod_flags & SYM_VAR_EXPLICIT_CONST) != 0 && vidx >= 0) {
            P->locals[vidx].is_const = 1;
            sub_byte |= AST_SLOT_CONST_DECL_BIT;
        }
        return mem_alloc_ast(mem, AST_SLOT_LOAD, sub_byte, 0, lock_type, lock_unit);
    }

    if (vidx >= 0) {
        if (P->locals[vidx].is_const) {
            mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                    (int)len, name);
            return MEM_NO_IDX;
        }
        return mem_alloc_ast(mem, AST_SLOT_LOAD, P->locals[vidx].slot, 0, lock_type, lock_unit);
    }

    /* Compound assignment (`+=`, `-=`, etc.) when not in `P->locals`: check module variables */
    int sid = sym_find(tab, mem, name, len);
    if (sid >= 0) {
        uint16_t priv_mod = MEM_NO_IDX;
        int mv_idx = sym_find_var(tab, (uint16_t)sid, P->cur_mod, &priv_mod);
        if (mv_idx >= 0 || priv_mod != MEM_NO_IDX) {
            uint16_t packed_lock = (uint16_t)(((uint16_t)lock_type & 0xFFu) |
                                              (((uint16_t)lock_unit & 0xFFu) << 8u));
            return mem_alloc_ast(mem, AST_CONST_SYM,
                                 (uint8_t)(SYM_VAR_REASSIGN_ONLY | SYM_VAR_COMPOUND_OP),
                                 (uint16_t)sid, packed_lock, P->cur_mod);
        }
    }

    mem_err(mem, "Cannot apply compound assignment to undeclared variable '%.*s'",
            (int)len, name);
    return MEM_NO_IDX;
}

/* ============================================================================
 * 4. PRATT PARSER NUD & LED DRIVERS
 * ============================================================================ */


