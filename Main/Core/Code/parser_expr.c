/*
 * parser_expr.c — Pratt Infix/Postfix (LED) Precedence Climbing Expression Loop
 */
#include "parser_internal.h"

static uint16_t parser_parse_expr_inner(Parser *P, int min_prec);

uint16_t parser_parse_expr(Parser *P, int min_prec) {
    if (P->expr_depth >= PARSER_MAX_EXPR_DEPTH) {
        mem_err(P->lex.mem, "Expression nesting depth limit exceeded (max %d)", PARSER_MAX_EXPR_DEPTH);
        return MEM_NO_IDX;
    }
    P->expr_depth++;
    uint16_t res = parser_parse_expr_inner(P, min_prec);
    P->expr_depth--;
    return res;
}

static uint16_t parser_parse_expr_inner(Parser *P, int min_prec) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    uint16_t left = parse_nud(P);
    if (mem->has_error || left == MEM_NO_IDX) return MEM_NO_IDX;

    while (!mem->has_error) {
        /* Direct call on lambda, scope block, or function-returning expression:
           `((x) => x^2)(5)`, `{ scale := 10 ; (x) => x * scale }(5)`, `make_adder(10)(5)` */
        if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN && 95 >= min_prec &&
            (mem_get_ast(mem, left)->kind == AST_LAMBDA ||
             mem_get_ast(mem, left)->kind == AST_FN_CALL ||
             mem_get_ast(mem, left)->kind == AST_INDEX ||
             ast_is_fn_expr_rec(mem, tab, P, left, 0))) {
            left = parse_fn_call_step(P, left);
            P->last_was_block = false;
            continue;
        }
        /* Postfix subscript `[` only binds on the SAME line (`!had_newline`) */
        if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LBRACK && 95 >= min_prec) {
            left = parse_index_step(P, left);
            P->last_was_block = false;
            continue;
        }
        if (P->lex.cur.kind == TOK_OP) {
            Token t = P->lex.cur;

            int post_prec = sym_get_active_prec(tab, t.sym_id, FIX_POSTFIX);
            int in_l_prec = sym_get_active_prec(tab, t.sym_id, FIX_INFIX_LEFT);
            int in_r_prec = sym_get_active_prec(tab, t.sym_id, FIX_INFIX_RIGHT);
            int pref_prec = sym_get_active_prec(tab, t.sym_id, FIX_PREFIX);

            /* Postfix unary operators only bind on the SAME line (`!t.had_newline`) */
            if (!t.had_newline && post_prec >= min_prec) {
                bool can_be_infix = (in_l_prec >= 0 || in_r_prec >= 0);
                bool use_postfix  = !can_be_infix;
                if (can_be_infix && post_prec >= in_l_prec && post_prec >= in_r_prec) {
                    use_postfix = true;
                }
                if (use_postfix) {
                    lex_next(&P->lex);
                    P->last_was_block = false;
                    left = mem_alloc_ast(mem, AST_UNARY, (uint8_t)FIX_POSTFIX, t.sym_id, left, MEM_NO_IDX);
                    continue;
                }
            }

            /* If a newline preceded a prefix-capable operator (like `-x`, `+x`, `++x`, `!x`),
               do NOT bind it as infix to the previous line; let it start a new statement! */
            bool blocked_by_newline_prefix = (t.had_newline && pref_prec >= 0);

            if (!blocked_by_newline_prefix && (in_l_prec >= min_prec || in_r_prec >= min_prec)) {
                OpFixity fix = (in_r_prec > in_l_prec) ? FIX_INFIX_RIGHT : FIX_INFIX_LEFT;
                int prec     = (fix == FIX_INFIX_RIGHT) ? in_r_prec : in_l_prec;
                int next_min = (fix == FIX_INFIX_RIGHT) ? prec : (prec + 1);
                uint8_t mtag = sym_active_meta_tag(tab, t.sym_id, fix);

                if (mtag == INTR_COLON || mtag == INTR_LAMBDA) {
                    break;
                }

                if (mtag == INTR_TERNARY) {
                    lex_next(&P->lex);
                    P->in_ternary_then++;
                    uint16_t then_ast = parser_parse_expr(P, prec);
                    P->in_ternary_then--;
                    if (mem->has_error || then_ast == MEM_NO_IDX) return MEM_NO_IDX;

                    if (cur_op_meta(P) != INTR_COLON) {
                        mem_err(mem, "Expected ':' in ternary '? :' expression");
                        return MEM_NO_IDX;
                    }
                    lex_next(&P->lex);
                    uint16_t else_ast = parser_parse_expr(P, prec);
                    if (mem->has_error || else_ast == MEM_NO_IDX) return MEM_NO_IDX;

                    uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, then_ast, else_ast);
                    left = mem_alloc_ast(mem, AST_TERNARY, 0, 0, left, link);
                    P->last_was_block = false;
                    continue;
                }

                bool lhs_was_block = P->last_was_block;
                lex_next(&P->lex);
                bool rhs_starts_brace = (P->lex.cur.kind == TOK_LBRACE);
                if (mtag == INTR_FIELD) {
                    left = parse_field_step(P, left);
                    P->last_was_block = false;
                    continue;
                }
                if (mtag == INTR_PIPE) {
                    left = hardcode_parse_pipe_rhs(P, left, next_min);
                    if (mem->has_error || left == MEM_NO_IDX) return MEM_NO_IDX;
                    P->last_was_block = false;
                    continue;
                }
                if (mtag == INTR_SEQ) {
                    while (cur_op_meta(P) == INTR_SEQ) {
                        lex_next(&P->lex);
                    }
                    if (P->lex.cur.kind == TOK_RPAREN || P->lex.cur.kind == TOK_RBRACE ||
                        P->lex.cur.kind == TOK_RBRACK || P->lex.cur.kind == TOK_EOF) {
                        break;
                    }
                    uint8_t kw = token_func_meta(P, P->lex.cur);
                    if (kw == INTR_OR_BRANCH || kw == INTR_ELSE_BRANCH || kw == INTR_CASE_BRANCH) {
                        break;
                    }
                    if (is_catch_kw(P->lex.src + P->lex.cur.off, P->lex.cur.len)) {
                        break;
                    }
                }

                uint16_t right          = MEM_NO_IDX;
                bool     is_in_op       = hardcode_is_in_op_sym(mem, tab, t.sym_id);
                bool     is_into_op     = hardcode_is_into_op_sym(mem, tab, t.sym_id);
                bool     rhs_unparen    = (P->lex.cur.kind != TOK_LPAREN);
                int      dual_unit_sid  = -1;

                if (is_into_op && (P->lex.cur.kind == TOK_OP || P->lex.cur.kind == TOK_IDENT)) {
                    const char *ustr = P->lex.src + P->lex.cur.off;
                    uint16_t    ulen = P->lex.cur.len;
                    if (!lex_peek_is_lparen(&P->lex)) {
                        int usid = sym_find(tab, mem, ustr, ulen);
                        if (usid >= 0 && hardcode_sym_is_postfix_unit(tab, (uint16_t)usid)) {
                            bool is_bound_var = (find_local_idx(P, ustr, ulen) >= 0) ||
                                                (sym_find_var(tab, (uint16_t)usid, P->cur_mod, NULL) >= 0);
                            if (!is_bound_var) {
                                lex_next(&P->lex);
                                uint16_t pidx = mem_alloc_pool(mem, 1);
                                mem_write_pool(mem, pidx, 1.0);
                                uint16_t one_ast = mem_alloc_ast(mem, AST_LIT, mem->fast_real_type, pidx, 0, 0);
                                right = mem_alloc_ast(mem, AST_UNARY, (uint8_t)FIX_POSTFIX,
                                                      (uint16_t)usid, one_ast, 1);
                            } else {
                                dual_unit_sid = usid;
                            }
                        }
                    }
                }
                if (right == MEM_NO_IDX) {
                    right = parser_parse_expr(P, (mtag == INTR_STORE) ? 2 : next_min);
                    if (is_into_op && rhs_unparen && dual_unit_sid >= 0 && right != MEM_NO_IDX) {
                        AstKind rk = (AstKind)mem_get_ast(mem, right)->kind;
                        if (rk == AST_SLOT_LOAD) {
                            mem_get_ast_mut(mem, right)->ref |=
                                (uint16_t)(AST_SLOT_DUAL_UNIT_BIT | ((uint16_t)dual_unit_sid & AST_SLOT_SYM_MASK));
                        } else if (rk == AST_CONST_SYM) {
                            mem_get_ast_mut(mem, right)->left =
                                (uint16_t)(AST_SLOT_DUAL_UNIT_BIT | ((uint16_t)dual_unit_sid & AST_SLOT_SYM_MASK));
                        }
                    }
                }
                if (is_into_op && right != MEM_NO_IDX) {
                    parser_try_infer_container_push(P, right, left);
                }
                if (mtag == INTR_STORE) {
                    if (P->in_symbolic_expr > 0) {
                        left = mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, t.sym_id, left, right);
                    } else {
                        left = build_store_or_compound(P, t.sym_id, left, right);
                    }
                    continue;
                }
                if (hardcode_is_rel_comparison_op(mem, tab, t.sym_id)) {
                    left = build_distributed_comp(P, fix, t.sym_id, left, right);
                    continue;
                }
                uint16_t bin_sym = t.sym_id;
                if (mtag == INTR_WHILE && lhs_was_block && !rhs_starts_brace) {
                    int do_sym = find_active_meta_sym(tab, INTR_DO_WHILE);
                    if (do_sym >= 0) bin_sym = (uint16_t)do_sym;
                }
                left = hardcode_rewrite_range_chain(P, fix, bin_sym, left, right);
                if (is_in_op && left != MEM_NO_IDX && !mem->has_error) {
                    const AstNode *in_node  = mem_get_ast(mem, left);
                    const AstNode *lhs_node = mem_get_ast(mem, in_node->left);
                    if (lhs_node && lhs_node->kind == AST_SLOT_LOAD &&
                        !hardcode_ast_is_bare_unit_rhs(mem, in_node->right) &&
                        (cur_op_meta(P) == INTR_WHILE ||
                         (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LBRACE && P->in_cond_expr == 0))) {
                        uint8_t  var_slot = (uint8_t)(lhs_node->sub & AST_SLOT_IDX_MASK);
                        uint16_t idx_tag  = lhs_node->left;
                        uint16_t for_ref  = ((idx_tag & 0x4000u) != 0)
                                            ? (uint16_t)(0x4000u | ((idx_tag & 0x3Fu) << 8u))
                                            : 0;
                        uint16_t iter_ast = in_node->right;
                        uint16_t iter_t   = ast_infer_struct_or_fn_type(P, iter_ast);
                        uint16_t elem_t   = sym_unpack_elem_type(mem, iter_t);
                        if (elem_t != MEM_TYPE_VOID && elem_t != SYM_TYPE_ANY) {
                            P->slot_types[var_slot] = elem_t;
                            for (uint8_t li = 0; li < P->vis_count; ++li) {
                                if (P->locals[li].slot == var_slot) {
                                    P->locals[li].type_id = elem_t;
                                    break;
                                }
                            }
                        }
                        mem_get_ast_mut(mem, in_node->left)->ref &= (uint16_t)~AST_SLOT_UNDEF_ITER_BIT;
                        if (cur_op_meta(P) == INTR_WHILE) {
                            lex_next(&P->lex); /* consume optional '??' */
                        }
                        uint16_t body_ast = parse_branch_body(P);
                        if (mem->has_error || body_ast == MEM_NO_IDX) return MEM_NO_IDX;
                        P->last_was_block = true;
                        left = mem_alloc_ast(mem, AST_FOR_IN, var_slot, for_ref, iter_ast, body_ast);
                        continue;
                    }
                    if (!(P->in_paren_expr > 0 && min_prec == 0 && P->lex.cur.kind == TOK_RPAREN)) {
                        if (!hardcode_check_undef_in_lhs(mem, tab, left)) {
                            return MEM_NO_IDX;
                        }
                    }
                }
                continue;
            }
        }

        /* Implicit multiplication on the SAME line: `2x`, `2π`, `3(x + 1)`, `(x + 1)(x - 1)`, `2 sin(x)` */
        if (!P->last_was_block && 60 >= min_prec && hardcode_can_start_implicit_mul_rhs(P)) {
            int mul_sym = sym_find(tab, mem, "*", 1);
            if (mul_sym >= 0 && sym_get_active_prec(tab, (uint16_t)mul_sym, FIX_INFIX_LEFT) >= 0) {
                uint16_t right = parser_parse_expr(P, 65);
                if (mem->has_error || right == MEM_NO_IDX) return MEM_NO_IDX;
                left = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                     (uint16_t)mul_sym, left, right);
                P->last_was_block = false;
                continue;
            }
        }

        /* Default Newline `;` Operator (`\n` acts as `@seq` for one-statement-per-line writing,
           both inside and outside `{...}` / `(...)` scopes, while keeping explicit `;` compulsory
           when placing multiple statements on a single line). */
        if (min_prec <= 1 && hardcode_can_start_newline_stmt(P)) {
            int seq_sym = find_active_meta_sym(tab, INTR_SEQ);
            if (seq_sym >= 0) {
                P->last_was_block = false;
                uint16_t right = parser_parse_expr(P, 2);
                if (mem->has_error || right == MEM_NO_IDX) return MEM_NO_IDX;
                left = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                     (uint16_t)seq_sym, left, right);
                continue;
            }
        }

        break;
    }
    return left;
}
