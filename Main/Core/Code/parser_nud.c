/*
 * parser_nud.c — String Interpolation & Pratt Prefix/Atom (NUD) Dispatcher
 */
#include "parser_internal.h"

static uint16_t make_str_lit_node(MemArena *mem, const char *s, uint16_t len) {
    StrSlice sl = mem_intern_str(mem, s, len);
    if (mem->has_error) return MEM_NO_IDX;
    return mem_alloc_ast(mem, AST_LIT, (uint8_t)mem->str_type_id, 0, sl.off, sl.len);
}

static uint16_t parse_interpolated_string(Parser *P, StrSlice s, bool force_interp) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    if (s.len == 0 || s.len >= 1020u) {
        return mem_alloc_ast(mem, AST_LIT, (uint8_t)mem->str_type_id, 0, s.off, s.len);
    }
    char raw[1024];
    memcpy(raw, mem_str_ptr(mem, s), s.len);
    raw[s.len] = '\0';

    /* Quick scan to check if there is at least one valid `{expr}` placeholder */
    bool has_placeholder = false;
    bool has_escaped_brace = false;
    for (uint16_t i = 0; i < s.len; ++i) {
        if (raw[i] == '{' && i + 1 < s.len && raw[i + 1] == '{') {
            has_escaped_brace = true;
            i++;
            continue;
        }
        if (raw[i] == '}' && i + 1 < s.len && raw[i + 1] == '}') {
            has_escaped_brace = true;
            i++;
            continue;
        }
        if (raw[i] == '{') {
            uint16_t j = (uint16_t)(i + 1);
            while (j < s.len && (raw[j] == ' ' || raw[j] == '\t')) j++;
            if (j < s.len && raw[j] != '}' && raw[j] != ':') {
                unsigned char fc = (unsigned char)raw[j];
                bool starts_expr = force_interp || lex_is_ident_start(fc) || fc == '(';
                if (starts_expr) {
                    int depth = 1;
                    uint16_t k = j;
                    while (k < s.len && depth > 0) {
                        if (raw[k] == '{') depth++;
                        else if (raw[k] == '}') depth--;
                        k++;
                    }
                    if (depth == 0) {
                        has_placeholder = true;
                        break;
                    }
                }
            }
        }
    }

    if (!has_placeholder) {
        if (force_interp && has_escaped_brace) {
            char unesc[1024];
            uint16_t ulen = 0;
            for (uint16_t i = 0; i < s.len; ++i) {
                if ((raw[i] == '{' || raw[i] == '}') && i + 1 < s.len && raw[i + 1] == raw[i]) {
                    unesc[ulen++] = raw[i++];
                } else {
                    unesc[ulen++] = raw[i];
                }
            }
            return make_str_lit_node(mem, unesc, ulen);
        }
        return mem_alloc_ast(mem, AST_LIT, (uint8_t)mem->str_type_id, 0, s.off, s.len);
    }

    uint16_t acc_ast = MEM_NO_IDX;
    char     lit_buf[1024];
    uint16_t lit_len = 0;
    uint16_t pos     = 0;

    while (pos < s.len && !mem->has_error) {
        if (raw[pos] == '{' && pos + 1 < s.len && raw[pos + 1] == '{') {
            lit_buf[lit_len++] = '{';
            pos = (uint16_t)(pos + 2);
            continue;
        }
        if (raw[pos] == '}' && pos + 1 < s.len && raw[pos + 1] == '}') {
            lit_buf[lit_len++] = '}';
            pos = (uint16_t)(pos + 2);
            continue;
        }
        if (raw[pos] == '{') {
            uint16_t b_start = (uint16_t)(pos + 1);
            int depth = 1;
            uint16_t k = b_start;
            while (k < s.len && depth > 0) {
                if (raw[k] == '"') {
                    k++;
                    while (k < s.len && raw[k] != '"') {
                        if (raw[k] == '\\' && k + 1 < s.len) k = (uint16_t)(k + 2);
                        else k++;
                    }
                    if (k < s.len) k++;
                    continue;
                }
                if (raw[k] == '{') depth++;
                else if (raw[k] == '}') depth--;
                if (depth > 0) k++;
            }
            if (depth != 0) {
                mem_err(mem, "Unclosed '{' in interpolated string");
                return MEM_NO_IDX;
            }
            uint16_t b_end = k;
            uint16_t ws = b_start;
            while (ws < b_end && (raw[ws] == ' ' || raw[ws] == '\t')) ws++;
            unsigned char fc = (ws < b_end) ? (unsigned char)raw[ws] : 0;
            bool is_interp = (ws < b_end && fc != '}' && fc != ':' &&
                              (force_interp || lex_is_ident_start(fc) || fc == '('));
            if (!is_interp) {
                while (pos <= b_end) lit_buf[lit_len++] = raw[pos++];
                continue;
            }

            if (lit_len > 0) {
                uint16_t lnode = make_str_lit_node(mem, lit_buf, lit_len);
                if (mem->has_error) return MEM_NO_IDX;
                acc_ast = (acc_ast == MEM_NO_IDX)
                          ? lnode
                          : mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_ADD, 0, acc_ast, lnode);
                lit_len = 0;
            }

            /* Split `{expr[:spec]}` at top-level format colon `:` */
            uint16_t colon_pos = b_end;
            int p_depth = 0;
            int q_depth = 0;
            for (uint16_t m = ws; m < b_end; ++m) {
                char mc = raw[m];
                if (mc == '"') {
                    m++;
                    while (m < b_end && raw[m] != '"') {
                        if (raw[m] == '\\' && m + 1 < b_end) m++;
                        m++;
                    }
                    continue;
                }
                if (mc == '(' || mc == '[' || mc == '{') { p_depth++; continue; }
                if (mc == ')' || mc == ']' || mc == '}') { if (p_depth > 0) p_depth--; continue; }
                if (p_depth == 0 && mc == '?') { q_depth++; continue; }
                if (p_depth == 0 && mc == ':') {
                    if (m + 1 < b_end && (raw[m + 1] == ':' || raw[m + 1] == '=')) {
                        m++;
                        continue;
                    }
                    if (q_depth > 0) {
                        q_depth--;
                        continue;
                    }
                    colon_pos = m;
                    break;
                }
            }

            uint16_t expr_len = (uint16_t)(colon_pos - ws);
            Parser subP;
            parser_init(&subP, mem, tab, raw + ws, expr_len, 0, false, P->in_ext_body);
            subP.cur_mod      = P->cur_mod;
            subP.is_mod_scope = P->is_mod_scope;
            subP.scope_depth  = P->scope_depth;
            subP.local_count  = P->local_count;
            subP.vis_count    = P->vis_count;
            memcpy(subP.locals,     P->locals,     sizeof(P->locals));
            memcpy(subP.slot_types, P->slot_types, sizeof(P->slot_types));

            lex_next(&subP.lex);
            uint16_t expr_ast = parser_parse_expr(&subP, 0);
            if (mem->has_error || expr_ast == MEM_NO_IDX) return MEM_NO_IDX;
            P->local_count = subP.local_count;
            memcpy(P->slot_types, subP.slot_types, sizeof(P->slot_types));

            uint16_t seg_ast = MEM_NO_IDX;
            if (colon_pos < b_end) {
                char fmt_tmpl[64];
                uint16_t sp_len = (uint16_t)(b_end - (colon_pos + 1));
                if (sp_len > 56u) sp_len = 56u;
                snprintf(fmt_tmpl, sizeof(fmt_tmpl), "{:%.*s}", (int)sp_len, raw + colon_pos + 1);
                uint16_t tmpl_ast = make_str_lit_node(mem, fmt_tmpl, (uint16_t)strlen(fmt_tmpl));
                if (mem->has_error) return MEM_NO_IDX;
                seg_ast = mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_STR_FMT, MEM_NO_IDX, tmpl_ast, expr_ast);
            } else {
                seg_ast = mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_TOSTR, 0, expr_ast, MEM_NO_IDX);
            }
            acc_ast = (acc_ast == MEM_NO_IDX)
                      ? seg_ast
                      : mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_ADD, 0, acc_ast, seg_ast);
            pos = (uint16_t)(b_end + 1);
            continue;
        }
        lit_buf[lit_len++] = raw[pos++];
    }

    if (lit_len > 0 || acc_ast == MEM_NO_IDX) {
        uint16_t lnode = make_str_lit_node(mem, lit_buf, lit_len);
        if (mem->has_error) return MEM_NO_IDX;
        acc_ast = (acc_ast == MEM_NO_IDX)
                  ? lnode
                  : mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_ADD, 0, acc_ast, lnode);
    }
    return acc_ast;
}

uint16_t parse_nud(Parser *P) {
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    Token t = P->lex.cur;
    P->last_was_block = false;

    /* 1. Consumed Literal Value ($real, $str, or $"..." / f"..." interpolated string) */
    if (t.kind == TOK_VAL) {
        bool force_interp = (t.len >= 3 &&
                             (P->lex.src[t.off] == '$' || P->lex.src[t.off] == 'f') &&
                             P->lex.src[t.off + 1] == '"');
        lex_next(&P->lex);
        uint16_t tid = val_get_type(mem, t.val);
        if (tid == mem->str_type_id && tid != 0) {
            StrSlice s = val_get_str(mem, t.val);
            return parse_interpolated_string(P, s, force_interp);
        } else {
            uint16_t pidx = mem_alloc_pool(mem, 1);
            if (mem->has_error) return MEM_NO_IDX;
            mem_write_pool(mem, pidx, val_get_f64(mem, t.val));
            return mem_alloc_ast(mem, AST_LIT, (uint8_t)tid, pidx, 0, 0);
        }
    }

    /* 2. Parenthesized Expression `( ... )`, Lambda `(x, y) => ...`, OR Loop `(i in 1...5) { ... }` */
    if (t.kind == TOK_LPAREN) {
        if (hardcode_is_paren_lambda(P)) {
            return parse_lambda_from_paren(P);
        }
        lex_next(&P->lex);
        uint8_t saved_ternary = P->in_ternary_then;
        P->in_ternary_then = 0;
        P->in_paren_expr++;
        while (cur_op_meta(P) == INTR_SEQ) {
            lex_next(&P->lex);
        }
        uint16_t inner = parser_parse_expr(P, 0);
        while (cur_op_meta(P) == INTR_SEQ) {
            lex_next(&P->lex);
        }
        P->in_paren_expr--;
        P->in_ternary_then = saved_ternary;
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected closing delimiter ')'");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);

        if (inner != MEM_NO_IDX && !mem->has_error) {
            const AstNode *in_node = mem_get_ast(mem, inner);
            if (in_node && in_node->kind == AST_BINARY &&
                hardcode_is_in_op_sym(mem, tab, in_node->ref)) {
                const AstNode *lhs_node = mem_get_ast(mem, in_node->left);
                if (lhs_node && lhs_node->kind == AST_SLOT_LOAD &&
                    !hardcode_ast_is_bare_unit_rhs(mem, in_node->right) &&
                    (P->lex.cur.kind == TOK_LBRACE || cur_op_meta(P) == INTR_WHILE)) {
                    uint8_t  var_slot = (uint8_t)(lhs_node->sub & AST_SLOT_IDX_MASK);
                    uint16_t idx_tag  = lhs_node->left;
                    uint16_t for_ref  = ((idx_tag & 0x4000u) != 0)
                                        ? (uint16_t)(0x4000u | ((idx_tag & 0x3Fu) << 8u))
                                        : 0;
                    uint16_t iter_ast = in_node->right;
                    mem_get_ast_mut(mem, in_node->left)->ref &= (uint16_t)~AST_SLOT_UNDEF_ITER_BIT;
                    if (cur_op_meta(P) == INTR_WHILE) {
                        lex_next(&P->lex); /* consume optional '??' */
                    }
                    uint16_t body_ast = parse_branch_body(P);
                    if (mem->has_error || body_ast == MEM_NO_IDX) return MEM_NO_IDX;
                    P->last_was_block = true;
                    return mem_alloc_ast(mem, AST_FOR_IN, var_slot, for_ref, iter_ast, body_ast);
                }
                if (!hardcode_check_undef_in_lhs(mem, tab, inner)) {
                    return MEM_NO_IDX;
                }
            }
        }
        return inner;
    }

    /* 3. Local Scope Block `{ stmt1; stmt2; expr }` or `{}` */
    if (t.kind == TOK_LBRACE) {
        return parse_scope_block(P);
    }

    /* 3b. Matrix Literal `[1, 2, 3]` or Vector Comprehension `[x^2 for x in 1...5]` */
    if (t.kind == TOK_LBRACK) {
        return parse_mat_literal(P);
    }

    /* 4. Private C Intrinsic `@add(a, b)` OR Host Extern Callback `@fx_rate(from, to)` */
    if (t.kind == TOK_AT) {
        if (!P->in_ext_body) {
            mem_err(mem, "Private '@' intrinsics/externs cannot be called outside extensions");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
        if (P->lex.cur.kind != TOK_IDENT &&
            !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
            mem_err(mem, "Expected intrinsic or extern name after '@'");
            return MEM_NO_IDX;
        }
        const char *iname = P->lex.src + P->lex.cur.off;
        uint16_t    ilen  = P->lex.cur.len;
        IntrinsicId intr  = sym_lookup_intrinsic(iname, ilen);
        lex_next(&P->lex);

        if (P->lex.cur.kind != TOK_LPAREN) {
            mem_err(mem, "Expected '(' after '@%.*s'", (int)ilen, iname);
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);

        if (intr != INTR_NONE) {
            uint16_t arg1 = MEM_NO_IDX;
            uint16_t arg2 = MEM_NO_IDX;
            uint16_t arg3 = MEM_NO_IDX;
            if (P->lex.cur.kind != TOK_RPAREN) {
                arg1 = parser_parse_expr(P, 0);
                if (P->lex.cur.kind == TOK_COMMA) {
                    lex_next(&P->lex);
                    arg2 = parser_parse_expr(P, 0);
                    if (P->lex.cur.kind == TOK_COMMA) {
                        lex_next(&P->lex);
                        arg3 = parser_parse_expr(P, 0);
                    }
                }
            }
            if (P->lex.cur.kind != TOK_RPAREN) {
                mem_err(mem, "Expected ')' after intrinsic arguments");
            } else {
                lex_next(&P->lex);
            }
            return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)intr, arg3, arg1, arg2);
        }

        StrSlice ext_slice  = mem_intern_str(mem, iname, ilen);
        uint16_t ext_sym_id = sym_intern(tab, mem, ext_slice);
        uint16_t head = MEM_NO_IDX;
        uint16_t tail = MEM_NO_IDX;
        uint8_t  argc = 0;

        if (P->lex.cur.kind != TOK_RPAREN) {
            while (1) {
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
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected ')' after arguments to '@%.*s'", (int)ilen, iname);
        } else {
            lex_next(&P->lex);
        }
        return mem_alloc_ast(mem, AST_EXTERN, argc, ext_sym_id, head, MEM_NO_IDX);
    }

    /* 5. Prefix Unary Operator (`-x`, `√x`, `_"42"`, `not x`, `!x`, `++x`, `+++x`) */
    if (t.kind == TOK_OP) {
        if (sym_active_meta_tag(tab, t.sym_id, FIX_FUNC) == INTR_OR_BRANCH) {
            t.kind = TOK_IDENT;
        } else {
            int prec = sym_get_active_prec(tab, t.sym_id, FIX_PREFIX);
            bool is_word_local = lex_is_ident_start((unsigned char)P->lex.src[t.off]) &&
                                 (find_local_idx(P, P->lex.src + t.off, t.len) >= 0 ||
                                  sym_find_var(tab, t.sym_id, P->cur_mod, NULL) >= 0 ||
                                  prec < 0);
            if (!is_word_local) {
                if (prec < 0) {
                    uint16_t priv_mod = MEM_NO_IDX;
                    if (sym_has_priv_ov(tab, t.sym_id, FIX_PREFIX, &priv_mod)) {
                        StrSlice s = tab->syms[t.sym_id];
                        StrSlice pmod_s = sym_mod_name(tab, priv_mod);
                        mem_err(mem, "Operator '%.*s' is private to module '[%.*s]'",
                                (int)s.len, mem_str_ptr(mem, s),
                                (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
                        return MEM_NO_IDX;
                    }
                    StrSlice s = tab->syms[t.sym_id];
                    mem_err(mem, "Operator '%.*s' has no active prefix definition",
                            (int)s.len, mem_str_ptr(mem, s));
                    return MEM_NO_IDX;
                }
                lex_next(&P->lex);
                uint16_t operand = parser_parse_expr(P, prec);
                return mem_alloc_ast(mem, AST_UNARY, (uint8_t)FIX_PREFIX, t.sym_id, operand, MEM_NO_IDX);
            }
            t.kind = TOK_IDENT;
        }
    }

    /* 6. Identifier, Variable Modifier (`pub`, `priv`, `const`, `mut`), Lambda `x => ...`,
          Control Construct, Tagged Scope, Typed Variable Declaration, or Function Call */
    if (t.kind == TOK_IDENT) {
        const char *id_str = P->lex.src + t.off;
        uint16_t id_len    = t.len;

        /* 5.9 Consume optional declaration modifiers `pub`, `priv`, `const`, `mut` */
        while (is_var_modifier_kw(id_str, id_len)) {
            uint16_t peek_p = skip_raw_ws(P->lex.src, P->lex.pos, P->lex.len);
            if (peek_p >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[peek_p])) {
                mem_err(mem, "Cannot use reserved keyword '%.*s' as a variable name",
                        (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            P->has_pending_mods = true;
            if (id_len == 4 && memcmp(id_str, "priv", 4) == 0) {
                P->pending_var_flags |= SYM_VAR_PRIVATE;
            } else if (id_len == 3 && memcmp(id_str, "pub", 3) == 0) {
                P->pending_var_flags &= (uint8_t)~SYM_VAR_PRIVATE;
            } else if (id_len == 5 && memcmp(id_str, "const", 5) == 0) {
                P->pending_var_flags |= SYM_VAR_EXPLICIT_CONST;
                P->pending_var_flags &= (uint8_t)~SYM_VAR_MUTABLE;
            } else if (id_len == 3 && memcmp(id_str, "mut", 3) == 0) {
                P->pending_var_flags |= SYM_VAR_MUTABLE;
                P->pending_var_flags &= (uint8_t)~SYM_VAR_EXPLICIT_CONST;
            }
            lex_next(&P->lex);
            t = P->lex.cur;
            if (t.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[t.off])) {
                t.kind = TOK_IDENT;
            }
            if (t.kind != TOK_IDENT) {
                mem_err(mem, "Expected variable or function identifier after declaration modifier");
                return MEM_NO_IDX;
            }
            id_str = P->lex.src + t.off;
            id_len = t.len;
        }

        /* 5.95 Check for Module-Qualified Access `mod::member` */
        if (P->lex.pos + 2 <= P->lex.len &&
            P->lex.src[P->lex.pos] == ':' && P->lex.src[P->lex.pos + 1] == ':') {
            if (P->has_pending_mods) {
                P->pending_var_flags = 0;
                P->has_pending_mods  = false;
                mem_err(mem, "Declaration modifiers cannot be applied to module-qualified symbol '%.*s::...'",
                        (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            int qmod = sym_find_mod_slice(tab, mem, id_str, id_len);
            if (qmod < 0) {
                /* Check for Type-Qualified Method or Enum Variant Access `Type::method(...)` / `Enum::Variant` */
                uint16_t qtid = sym_find_type(tab, mem, id_str, id_len);
                if (qtid != MEM_TYPE_VOID && qtid < tab->type_count) {
                    P->lex.pos = (uint16_t)(P->lex.pos + 2);
                    lex_next(&P->lex);
                    Token mt = P->lex.cur;
                    if (mt.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[mt.off])) {
                        mt.kind = TOK_IDENT;
                    }
                    if (mt.kind != TOK_IDENT) {
                        mem_err(mem, "Expected method or variant identifier after '%.*s::'", (int)id_len, id_str);
                        return MEM_NO_IDX;
                    }
                    const char *m_str = P->lex.src + mt.off;
                    uint16_t    m_len = mt.len;
                    lex_next(&P->lex);

                    if (P->lex.cur.kind == TOK_LPAREN) {
                        lex_next(&P->lex);
                        uint16_t head = MEM_NO_IDX, tail = MEM_NO_IDX;
                        uint8_t argc = 0;
                        if (P->lex.cur.kind != TOK_RPAREN) {
                            while (!mem->has_error) {
                                uint16_t arg = parser_parse_expr(P, 0);
                                uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, arg, MEM_NO_IDX);
                                if (head == MEM_NO_IDX) head = link;
                                else mem_get_ast_mut(mem, tail)->right = link;
                                tail = link;
                                argc++;
                                if (P->lex.cur.kind == TOK_COMMA) lex_next(&P->lex);
                                else break;
                            }
                        }
                        if (P->lex.cur.kind != TOK_RPAREN) {
                            mem_err(mem, "Expected ')' after '%.*s::%.*s(...)'", (int)id_len, id_str, (int)m_len, m_str);
                            return MEM_NO_IDX;
                        }
                        lex_next(&P->lex);
                        int msid = sym_find(tab, mem, m_str, m_len);
                        if (msid >= 0 && sym_has_visible_func_ov(tab, (uint16_t)msid)) {
                            return mem_alloc_ast(mem, AST_CALL, argc, (uint16_t)msid, head, MEM_NO_IDX);
                        }
                    }
                    mem_err(mem, "Undefined method or variant '%.*s' on type '%.*s'",
                            (int)m_len, m_str, (int)id_len, id_str);
                    return MEM_NO_IDX;
                }
                mem_err(mem, "Unknown or unloaded module '[%.*s]'", (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            if (!sym_mod_enabled(tab, (uint16_t)qmod)) {
                mem_err(mem, "Module '[%.*s]' is disabled", (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            uint16_t qual_mod = (uint16_t)qmod;
            P->lex.pos = (uint16_t)(P->lex.pos + 2);
            if (P->lex.pos >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[P->lex.pos])) {
                mem_err(mem, "Expected symbol identifier after '%.*s::'", (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            lex_next(&P->lex);
            Token mt = P->lex.cur;
            if (mt.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[mt.off])) {
                mt.kind = TOK_IDENT;
            }
            if (mt.kind != TOK_IDENT) {
                mem_err(mem, "Expected symbol identifier after '%.*s::'", (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            const char *m_str = P->lex.src + mt.off;
            uint16_t    m_len = mt.len;
            int         msid  = sym_find(tab, mem, m_str, m_len);
            lex_next(&P->lex);

            /* 1) Type/unit annotated qualified assignment `mod::v : real, usd := 1` */
            if (P->in_ternary_then == 0 && cur_op_meta(P) == INTR_COLON) {
                uint8_t ltype = 0, lunit = 0;
                if (!parse_var_type_unit_annot(P, &ltype, &lunit)) return MEM_NO_IDX;
                if (P->lex.cur.kind != TOK_OP ||
                    !hardcode_is_plain_store_sym(mem, tab, P->lex.cur.sym_id, NULL)) {
                    mem_err(mem, "Expected ':=' or '=' after variable type annotation");
                    return MEM_NO_IDX;
                }
                return resolve_qual_mod_assign(P, qual_mod, m_str, m_len, P->lex.cur.sym_id, ltype, lunit);
            }

            /* 2) Qualified assignment or compound assignment `mod::v = 10`, `mod::v += 5`, `mod::v := 10` */
            if (P->in_symbolic_expr == 0 && P->lex.cur.kind == TOK_OP &&
                sym_active_meta_tag(tab, P->lex.cur.sym_id, FIX_INFIX_RIGHT) == INTR_STORE) {
                return resolve_qual_mod_assign(P, qual_mod, m_str, m_len, P->lex.cur.sym_id, 0, 0);
            }

            /* 3) Qualified function or `$fn` call `mod::fn(args...)` */
            if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN) {
                lex_next(&P->lex); /* consume '(' */
                uint16_t head = MEM_NO_IDX;
                uint16_t tail = MEM_NO_IDX;
                uint8_t  argc = 0;
                if (P->lex.cur.kind != TOK_RPAREN) {
                    while (!mem->has_error) {
                        if (argc >= 16) {
                            mem_err(mem, "Max 16 arguments supported in function call");
                            return MEM_NO_IDX;
                        }
                        uint16_t arg = parser_parse_expr(P, 0);
                        if (mem->has_error || arg == MEM_NO_IDX) return MEM_NO_IDX;
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
                if (P->lex.cur.kind != TOK_RPAREN) {
                    mem_err(mem, "Expected ')' after arguments to '%.*s::%.*s'",
                            (int)id_len, id_str, (int)m_len, m_str);
                    return MEM_NO_IDX;
                }
                lex_next(&P->lex); /* consume ')' */

                if (msid >= 0) {
                    bool is_priv_fn = false;
                    if (sym_has_func_ov_in_mod(tab, (uint16_t)msid, qual_mod, P->cur_mod, &is_priv_fn)) {
                        uint16_t qspec = (uint16_t)(AST_CALL_QUAL_MOD_BIT | (qual_mod & 0x3FFFu));
                        return mem_alloc_ast(mem, AST_CALL, argc, (uint16_t)msid, head, qspec);
                    }
                    if (is_priv_fn) {
                        mem_err(mem, "Function '%.*s' is private to module '[%.*s]'",
                                (int)m_len, m_str, (int)id_len, id_str);
                        return MEM_NO_IDX;
                    }
                    int own_v = sym_find_own_var(tab, (uint16_t)msid, qual_mod);
                    if (own_v >= 0) {
                        const ModVarDef *mv = &sym_vars(tab)[own_v];
                        if ((mv->flags & SYM_VAR_PRIVATE) != 0 && qual_mod != P->cur_mod) {
                            mem_err(mem, "Function or variable '%.*s' is private to module '[%.*s]'",
                                    (int)m_len, m_str, (int)id_len, id_str);
                            return MEM_NO_IDX;
                        }
                        uint16_t fn_load = mem_alloc_ast(mem, AST_CONST_SYM, SYM_VAR_QUAL_MOD,
                                                         (uint16_t)msid, MEM_NO_IDX, qual_mod);
                        return mem_alloc_ast(mem, AST_FN_CALL, argc, 0, fn_load, head);
                    }
                }
                mem_err(mem, "Undefined function '%.*s' in module '[%.*s]'",
                        (int)m_len, m_str, (int)id_len, id_str);
                return MEM_NO_IDX;
            }

            /* 4) Qualified constant / variable read `mod::CONST`, `mod::var` */
            if (msid >= 0) {
                int own_v = sym_find_own_var(tab, (uint16_t)msid, qual_mod);
                if (own_v >= 0) {
                    const ModVarDef *mv = &sym_vars(tab)[own_v];
                    if ((mv->flags & SYM_VAR_PRIVATE) != 0 && qual_mod != P->cur_mod) {
                        mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                                (int)m_len, m_str, (int)id_len, id_str);
                        return MEM_NO_IDX;
                    }
                    return mem_alloc_ast(mem, AST_CONST_SYM, SYM_VAR_QUAL_MOD,
                                         (uint16_t)msid, MEM_NO_IDX, qual_mod);
                }
                for (uint8_t ci = 0; ci < tab->const_count; ++ci) {
                    if (tab->consts[ci].sym_id == (uint16_t)msid && tab->consts[ci].mod_id == qual_mod) {
                        if ((tab->consts[ci].reserved & SYM_VAR_PRIVATE) != 0 && qual_mod != P->cur_mod) {
                            mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                                    (int)m_len, m_str, (int)id_len, id_str);
                            return MEM_NO_IDX;
                        }
                        return mem_alloc_ast(mem, AST_CONST_SYM, SYM_VAR_QUAL_MOD,
                                             (uint16_t)msid, MEM_NO_IDX, qual_mod);
                    }
                }
            }
            mem_err(mem, "Undefined symbol or constant '%.*s' in module '[%.*s]'",
                    (int)m_len, m_str, (int)id_len, id_str);
            return MEM_NO_IDX;
        }

        int sym_id      = sym_find(tab, mem, id_str, id_len);
        uint8_t fn_meta = (sym_id >= 0) ? sym_active_meta_tag(tab, (uint16_t)sym_id, FIX_FUNC) : 0;

        lex_next(&P->lex);

        /* 6.0 Reserved keyword guard */
        if (sym_is_reserved_kw(tab, mem, id_str, id_len)) {
            if (P->in_ternary_then == 0 && cur_op_meta(P) == INTR_COLON) {
                if (lex_peek_is_lbrace(&P->lex) || lex_peek_is_lparen(&P->lex)) {
                    mem_err(mem, "Cannot use reserved keyword '%.*s' as a scope tag",
                            (int)id_len, id_str);
                } else {
                    mem_err(mem, "Cannot use reserved keyword '%.*s' as a variable name",
                            (int)id_len, id_str);
                }
                return MEM_NO_IDX;
            }
            if ((P->lex.cur.kind == TOK_OP &&
                 sym_active_meta_tag(tab, P->lex.cur.sym_id, FIX_INFIX_RIGHT) == INTR_STORE) ||
                cur_op_meta(P) == INTR_LAMBDA ||
                (P->lex.cur.kind == TOK_COMMA && lex_is_multi_lhs(P))) {
                mem_err(mem, "Cannot use reserved keyword '%.*s' as a variable name",
                        (int)id_len, id_str);
                return MEM_NO_IDX;
            }
        }

        /* 6.1 Single-parameter unparenthesized lambda `x => expr` */
        if (cur_op_meta(P) == INTR_LAMBDA) {
            return parse_lambda_from_ident(P, id_str, id_len);
        }

        /* 6a. Control-flow constructs (`try`/`catch`/`throw` & `Library/bootstrap.math` constructs) */
        if (is_try_kw(id_str, id_len)) {
            return parse_try_construct(P);
        }
        if (is_catch_kw(id_str, id_len)) {
            mem_err(mem, "Unexpected 'catch' without preceding 'try' block");
            return MEM_NO_IDX;
        }
        if (is_throw_kw(id_str, id_len)) {
            return parse_throw_construct(P);
        }
        if (id_len == 6 && memcmp(id_str, "import", 6) == 0 &&
            (P->lex.cur.kind == TOK_VAL || P->lex.cur.kind == TOK_IDENT || P->lex.cur.kind == TOK_LPAREN)) {
            bool has_paren = (P->lex.cur.kind == TOK_LPAREN);
            if (has_paren) lex_next(&P->lex);
            StrSlice spec = { 0, 0 };
            uint16_t is_quoted_path = 0;
            if (P->lex.cur.kind == TOK_VAL && mem->str_type_id != 0 &&
                val_get_type(mem, P->lex.cur.val) == mem->str_type_id) {
                spec           = val_get_str(mem, P->lex.cur.val);
                is_quoted_path = 1u;
                lex_next(&P->lex);
            } else if (P->lex.cur.kind == TOK_IDENT ||
                       (P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                spec           = mem_intern_str(mem, P->lex.src + P->lex.cur.off, P->lex.cur.len);
                is_quoted_path = 0u;
                lex_next(&P->lex);
            } else {
                mem_err(mem, "Expected file path string or module name after 'import'");
                return MEM_NO_IDX;
            }
            if (has_paren) {
                if (P->lex.cur.kind != TOK_RPAREN) {
                    mem_err(mem, "Expected ')' after import argument");
                    return MEM_NO_IDX;
                }
                lex_next(&P->lex);
            }
            return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_IMPORT, is_quoted_path, spec.off, spec.len);
        }
        if (fn_meta == INTR_IF) {
            return parse_if_construct(P, (uint16_t)sym_id);
        }
        if (fn_meta == INTR_SWITCH_BLOCK) {
            return parse_switch_construct(P);
        }
        if (fn_meta == INTR_JUMP_TAG) {
            return parse_tag_op(P, AST_JUMP, "jump");
        }
        if (fn_meta == INTR_RUN_TAG) {
            return parse_tag_op(P, AST_RUN_TAG, "run");
        }
        if (fn_meta == INTR_BREAK_LOOP) {
            return parse_break_construct(P);
        }
        if (fn_meta == INTR_CONTINUE_LOOP) {
            return parse_continue_construct(P);
        }
        if (fn_meta == INTR_RETURN_FN) {
            return parse_return_construct(P);
        }
        if (fn_meta == INTR_OR_BRANCH) {
            mem_err(mem, "Unexpected 'or'/'then' without preceding 'if' block");
            return MEM_NO_IDX;
        }
        if (fn_meta == INTR_ELSE_BRANCH) {
            mem_err(mem, "Unexpected 'else' without preceding 'if' or 'switch' block");
            return MEM_NO_IDX;
        }
        if (fn_meta == INTR_CASE_BRANCH) {
            mem_err(mem, "Unexpected 'case' outside of 'switch' block");
            return MEM_NO_IDX;
        }
        if (fn_meta == INTR_FOR_KW) {
            mem_err(mem, "Unexpected 'for' outside of '[expr for x in ...]' comprehension");
            return MEM_NO_IDX;
        }

        /* 6b. Colon-Tagged Scope or Loop (`Tag : { ... }`, `Tag : (i in 1...5) { ... }`, `Tag : i in 1...5 { ... }`) */
        if (P->in_ternary_then == 0 && cur_op_meta(P) == INTR_COLON &&
            (lex_peek_is_lbrace(&P->lex) || lex_peek_is_lparen(&P->lex) || lex_peek_is_ident_in(P))) {
            bool is_brace_scope = lex_peek_is_lbrace(&P->lex);
            lex_next(&P->lex);
            int label_id = parser_get_or_add_label(P, id_str, id_len);
            if (mem->has_error) return MEM_NO_IDX;
            if (P->labels[label_id].defined) {
                mem_err(mem, "Duplicate scope tag '%.*s' in the same function/expression",
                        (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            P->labels[label_id].defined = true;
            uint16_t body = is_brace_scope ? parse_scope_block(P) : parser_parse_expr(P, 2);
            if (is_brace_scope && body != MEM_NO_IDX && !mem->has_error &&
                 (cur_op_meta(P) == INTR_WHILE || cur_op_meta(P) == INTR_DO_WHILE)) {
                uint16_t op_sym = P->lex.cur.sym_id;
                int do_sym = find_active_meta_sym(tab, INTR_DO_WHILE);
                if (do_sym >= 0) op_sym = (uint16_t)do_sym;
                lex_next(&P->lex);
                uint16_t cond_ast = parser_parse_expr(P, 4);
                if (mem->has_error || cond_ast == MEM_NO_IDX) return MEM_NO_IDX;
                body = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT, op_sym, body, cond_ast);
            }
            if (body != MEM_NO_IDX && mem_get_ast(mem, body)->kind == AST_FOR_IN) {
                mem_get_ast_mut(mem, body)->ref |= (uint16_t)(label_id + 1);
            }
            P->labels[label_id].body_ast = body;
            P->last_was_block = true;
            return mem_alloc_ast(mem, AST_TAGGED_SCOPE, (uint8_t)label_id, 0, body, MEM_NO_IDX);
        }

        /* 6c. Single-Variable Concrete Type/Unit Declaration (`x : real := 5`,
               `x : real, distance := 50m`, `x : distance := 50m`) */
        if (P->in_ternary_then == 0 && cur_op_meta(P) == INTR_COLON) {
            uint8_t lock_type = 0, lock_unit = 0;
            if (!parse_var_type_unit_annot(P, &lock_type, &lock_unit)) {
                return MEM_NO_IDX;
            }
            if (P->lex.cur.kind != TOK_OP ||
                !hardcode_is_plain_store_sym(mem, tab, P->lex.cur.sym_id, NULL)) {
                mem_err(mem, "Expected ':=' or '=' after variable type annotation");
                return MEM_NO_IDX;
            }
            return resolve_var_for_assign(P, id_str, id_len, P->lex.cur.sym_id, lock_type, lock_unit);
        }

        /* 6d-iter. Two-Variable Indexed Loop Header (`(i, x in v) { ... }` or `i, x in v { ... }`) */
        if (P->lex.cur.kind == TOK_COMMA && lex_is_two_var_in(P)) {
            lex_next(&P->lex); /* consume ',' */
            if (P->lex.cur.kind != TOK_IDENT &&
                !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                mem_err(mem, "Expected second variable name after ',' in 'i, x in ...'");
                return MEM_NO_IDX;
            }
            const char *v2_str = P->lex.src + P->lex.cur.off;
            uint16_t    v2_len = P->lex.cur.len;
            lex_next(&P->lex); /* consume second variable name */

            int idx_slot = parser_add_local(P, id_str, id_len, mem->fast_real_type);
            if (mem->has_error) return MEM_NO_IDX;
            P->locals[P->vis_count - 1].is_loop_iter = 1;

            int val_slot = parser_add_local(P, v2_str, v2_len, mem->fast_real_type);
            if (mem->has_error) return MEM_NO_IDX;
            P->locals[P->vis_count - 1].is_loop_iter = 1;

            StrSlice vslice  = mem_intern_str(mem, v2_str, v2_len);
            uint16_t var_sym = sym_intern(tab, mem, vslice);
            uint16_t idx_tag = (uint16_t)(0x4000u | ((uint16_t)idx_slot & 0x3Fu));
            return mem_alloc_ast(mem, AST_SLOT_LOAD, (uint8_t)val_slot,
                                 (uint16_t)(AST_SLOT_UNDEF_ITER_BIT | (var_sym & AST_SLOT_SYM_MASK)),
                                 idx_tag, 0);
        }

        /* 6d. Multi-Variable Declaration / Destructuring / Compound Assignment */
        if (P->lex.cur.kind == TOK_COMMA && lex_is_multi_lhs(P)) {
            const char *var_names[8];
            uint16_t    var_lens[8];
            uint8_t     var_cnt = 0;

            var_names[var_cnt] = id_str;
            var_lens[var_cnt]  = id_len;
            var_cnt++;

            while (P->lex.cur.kind == TOK_COMMA && var_cnt < 8) {
                lex_next(&P->lex);
                if (P->lex.cur.kind != TOK_IDENT &&
                    !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                    mem_err(mem, "Expected variable name after ',' in multi-variable assignment");
                    return MEM_NO_IDX;
                }
                var_names[var_cnt] = P->lex.src + P->lex.cur.off;
                var_lens[var_cnt]  = P->lex.cur.len;
                var_cnt++;
                lex_next(&P->lex);
            }

            uint8_t lock_type = 0, lock_unit = 0;
            if (cur_op_meta(P) == INTR_COLON) {
                if (!parse_var_type_unit_annot(P, &lock_type, &lock_unit)) {
                    return MEM_NO_IDX;
                }
            }

            if (P->lex.cur.kind != TOK_OP ||
                sym_active_meta_tag(tab, P->lex.cur.sym_id, FIX_INFIX_RIGHT) != INTR_STORE) {
                mem_err(mem, "Expected assignment operator after multi-variable list");
                return MEM_NO_IDX;
            }
            uint16_t op_sym_id = P->lex.cur.sym_id;
            int prec = sym_get_active_prec(tab, op_sym_id, FIX_INFIX_RIGHT);
            lex_next(&P->lex);

            uint8_t saved_mod_flags = P->pending_var_flags;
            bool    saved_had_mods  = P->has_pending_mods;
            uint16_t slot_asts[8];
            for (uint8_t i = 0; i < var_cnt; ++i) {
                P->pending_var_flags = saved_mod_flags;
                P->has_pending_mods  = saved_had_mods;
                slot_asts[i] = resolve_var_for_assign(P, var_names[i], var_lens[i],
                                                      op_sym_id, lock_type, lock_unit);
                if (mem->has_error) return MEM_NO_IDX;
            }

            uint16_t rhs = parser_parse_expr(P, prec >= 0 ? prec : 5);
            if (mem->has_error || rhs == MEM_NO_IDX) return MEM_NO_IDX;

            int seq_sym = find_active_meta_sym(tab, INTR_SEQ);
            if (hardcode_is_plain_store_sym(mem, tab, op_sym_id, NULL) && seq_sym >= 0) {
                uint8_t  tmp_slot = parser_alloc_anon_slot(P, mem->fast_real_type);
                uint16_t tmp_load = mem_alloc_ast(mem, AST_SLOT_LOAD, tmp_slot, 0, 0, 0);
                uint16_t acc      = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_RIGHT,
                                                  op_sym_id, tmp_load, rhs);
                for (uint8_t i = 0; i < var_cnt; ++i) {
                    uint16_t unpack_i = mem_alloc_ast(mem, AST_UNPACK_ELEM, i, var_cnt, tmp_slot, 0);
                    uint16_t store_i  = build_store_or_compound(P, op_sym_id, slot_asts[i], unpack_i);
                    acc = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                        (uint16_t)seq_sym, acc, store_i);
                }
                return acc;
            } else {
                uint16_t acc = build_store_or_compound(P, op_sym_id, slot_asts[0], rhs);
                for (uint8_t i = 1; i < var_cnt; ++i) {
                    uint16_t step = build_store_or_compound(P, op_sym_id, slot_asts[i], rhs);
                    acc = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                        (uint16_t)seq_sym, acc, step);
                }
                return acc;
            }
        }

        /* 6e-fn. Inline Named Function Declaration `f(x, y) := expr` -> desugars to `$fn` closure */
        if (hardcode_is_inline_fn_def(P)) {
            return parse_inline_fn_def(P, id_str, id_len);
        }

        /* 6e. Single Local Variable, Parameter Slot, or Module Variable Assignment */
        int vidx = find_local_idx(P, id_str, id_len);
        bool is_assign = (P->in_symbolic_expr == 0 && P->lex.cur.kind == TOK_OP &&
                          sym_active_meta_tag(tab, P->lex.cur.sym_id, FIX_INFIX_RIGHT) == INTR_STORE);
        if (is_assign) {
            uint16_t lhs_ast = resolve_var_for_assign(P, id_str, id_len, P->lex.cur.sym_id, 0, 0);
            if (lhs_ast != MEM_NO_IDX && mem->fn_type_id != 0 && hardcode_rhs_starts_lambda(P)) {
                const AstNode *ln = mem_get_ast(mem, lhs_ast);
                if (ln->kind == AST_SLOT_LOAD) {
                    P->slot_types[ln->sub & AST_SLOT_IDX_MASK] = mem->fn_type_id;
                } else if (ln->kind == AST_CONST_SYM) {
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
            return lhs_ast;
        }
        if (P->has_pending_mods) {
            P->pending_var_flags = 0;
            P->has_pending_mods  = false;
            mem_err(mem, "Expected ':=' or '=' after declaration modifier on '%.*s'",
                    (int)id_len, id_str);
            return MEM_NO_IDX;
        }
        if (vidx >= 0) {
            uint8_t  slot      = P->locals[vidx].slot;
            uint16_t slot_load = mem_alloc_ast(mem, AST_SLOT_LOAD, slot, 0, 0, 0);
            if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN) {
                bool is_fn_var = (mem->fn_type_id != 0 && P->slot_types[slot] == mem->fn_type_id);
                if (!is_fn_var && lex_at_is_syntax(&P->lex, P->lex.pos, INTR_RPAREN)) {
                    is_fn_var = true; /* `f()` with 0 args is unambiguously a function call */
                }
                if (is_fn_var) {
                    return parse_fn_call_step(P, slot_load);
                }
            }
            return slot_load;
        }

        /* 6e-mod. Module-level `$fn` variable call `f(x)` when `f` is in `tab->vars` */
        if (sym_id >= 0 && !P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN) {
            uint16_t priv_mod = MEM_NO_IDX;
            int mv_idx = sym_find_var(tab, (uint16_t)sym_id, P->cur_mod, &priv_mod);
            if (mv_idx >= 0) {
                const ModVarDef *mv = &sym_vars(tab)[mv_idx];
                uint16_t mv_tid = val_get_type(mem, mv->val);
                if (mv_tid == MEM_TYPE_VOID) {
                    mv_tid = (mv->lock_type & 0xFFu) ? (uint16_t)(mv->lock_type & 0xFFu)
                                                     : (uint16_t)((mv->lock_type >> 8u) & 0xFFu);
                }
                bool is_fn_mvar = (mem->fn_type_id != 0 && mv_tid == mem->fn_type_id);
                if (!is_fn_mvar && !sym_has_visible_func_ov(tab, (uint16_t)sym_id) &&
                    sym_find_type(tab, mem, id_str, id_len) == MEM_TYPE_VOID) {
                    is_fn_mvar = true;
                }
                if (is_fn_mvar) {
                    uint16_t mod_load = mem_alloc_ast(mem, AST_CONST_SYM, 0,
                                                      (uint16_t)sym_id, MEM_NO_IDX, P->cur_mod);
                    return parse_fn_call_step(P, mod_load);
                }
            }
        }

        /* 6e-iter. Undefined variable on the left side of an iterator operator (`i in 1...100`) */
        if (P->lex.cur.kind == TOK_OP && hardcode_is_in_op_sym(mem, tab, P->lex.cur.sym_id)) {
            bool is_active_var = false;
            if (sym_id >= 0 && sym_find_var(tab, (uint16_t)sym_id, P->cur_mod, NULL) >= 0) {
                is_active_var = true;
            }
            if (!is_active_var) {
                int slot = parser_add_local(P, id_str, id_len, mem->fast_real_type);
                if (mem->has_error) return MEM_NO_IDX;
                P->locals[P->vis_count - 1].is_loop_iter = 1;
                StrSlice vslice  = P->locals[P->vis_count - 1].name;
                uint16_t var_sym = sym_intern(tab, mem, vslice);
                return mem_alloc_ast(mem, AST_SLOT_LOAD, (uint8_t)slot,
                                     (uint16_t)(AST_SLOT_UNDEF_ITER_BIT | (var_sym & AST_SLOT_SYM_MASK)),
                                     0, 0);
            }
        }

        /* 6f-0. Parameterized container constructor (`vec<T>(...)`, `list<T>(...)`, `stack<T>(...)`, `queue<T>(...)`) */
        uint16_t container_type_spec = MEM_NO_IDX;
        {
            uint16_t parsed_spec = 0;
            if (hardcode_try_parse_container_type_arg(P, id_str, id_len, &parsed_spec)) {
                container_type_spec = parsed_spec;
            } else if (mem->has_error) {
                return MEM_NO_IDX;
            }
        }

        /* 6f. Call syntax `name(arg1, arg2, ...)` -> Struct, Function, or `Tag()` reuse */
        if (P->lex.cur.kind == TOK_LPAREN) {
            lex_next(&P->lex);
            uint8_t saved_ternary = P->in_ternary_then;
            P->in_ternary_then = 0;

            uint16_t head = MEM_NO_IDX;
            uint16_t tail = MEM_NO_IDX;
            uint8_t  argc = 0;

            bool use_lazy_call = false;
            if (sym_id >= 0 && (fn_meta & SYM_META_LAZY_MASK) != 0) {
                if (!sym_has_non_lazy_func_ov(tab, (uint16_t)sym_id)) {
                    use_lazy_call = true;
                } else {
                    bool first_is_ident = (P->lex.cur.kind == TOK_IDENT ||
                                           (P->lex.cur.kind == TOK_OP &&
                                            lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off])));
                    bool first_is_fn_local = false;
                    if (first_is_ident && mem->fn_type_id != 0) {
                        int lidx = find_local_idx(P, P->lex.src + P->lex.cur.off, P->lex.cur.len);
                        if (lidx >= 0 && P->slot_types[P->locals[lidx].slot] == mem->fn_type_id) {
                            first_is_fn_local = true;
                        } else {
                            int fsid = sym_find(tab, mem, P->lex.src + P->lex.cur.off, P->lex.cur.len);
                            if (fsid >= 0) {
                                int fmv = sym_find_var(tab, (uint16_t)fsid, P->cur_mod, NULL);
                                if (fmv >= 0 && val_get_type(mem, sym_vars(tab)[fmv].val) == mem->fn_type_id) {
                                    first_is_fn_local = true;
                                }
                            }
                        }
                    }
                    use_lazy_call = (first_is_ident && !first_is_fn_local);
                }
            }

            bool parse_rest = (P->lex.cur.kind != TOK_RPAREN);
            if (use_lazy_call && parse_rest) {
                uint8_t v_cnt = (uint8_t)((fn_meta >> 3u) & 0x07u);
                uint8_t e_cnt = (uint8_t)(fn_meta & 0x07u);
                bool    is_eq = ((fn_meta & SYM_META_LAZY_EQ_BIT) != 0);

                uint16_t z_pidx = mem_alloc_pool(mem, 1);
                if (mem->has_error) {
                    P->in_ternary_then = saved_ternary;
                    return MEM_NO_IDX;
                }
                mem_write_pool(mem, z_pidx, 0.0);
                uint16_t zero_lit = mem_alloc_ast(mem, AST_LIT, mem->fast_real_type, z_pidx, 0, 0);

                const char *vnames[4];
                uint16_t    vlens[4];
                for (uint8_t v = 0; v < v_cnt; ++v) {
                    if (P->lex.cur.kind != TOK_IDENT &&
                        !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                        P->in_ternary_then = saved_ternary;
                        mem_err(mem, "Variable argument to '%.*s' must be an identifier (e.g. x)",
                                (int)id_len, id_str);
                        return MEM_NO_IDX;
                    }
                    vnames[v] = P->lex.src + P->lex.cur.off;
                    vlens[v]  = P->lex.cur.len;
                    lex_next(&P->lex);
                    uint16_t zlink = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, zero_lit, MEM_NO_IDX);
                    if (head == MEM_NO_IDX) head = zlink;
                    else mem_get_ast_mut(mem, tail)->right = zlink;
                    tail = zlink;
                    argc++;
                    if (P->lex.cur.kind != TOK_COMMA) {
                        P->in_ternary_then = saved_ternary;
                        mem_err(mem, "Calculus/solver construct '%.*s' requires an expression argument",
                                (int)id_len, id_str);
                        return MEM_NO_IDX;
                    }
                    lex_next(&P->lex);
                }

                uint8_t saved_vis  = P->vis_count;
                uint8_t slot_start = P->local_count;
                P->scope_depth++;
                for (uint8_t v = 0; v < v_cnt; ++v) {
                    (void)parser_add_local(P, vnames[v], vlens[v], mem->fast_real_type);
                    if (mem->has_error) {
                        P->vis_count       = saved_vis;
                        P->scope_depth--;
                        P->in_ternary_then = saved_ternary;
                        return MEM_NO_IDX;
                    }
                }

                uint16_t expr_asts[4];
                for (uint8_t e = 0; e < e_cnt; ++e) {
                    if (e > 0) {
                        if (P->lex.cur.kind != TOK_COMMA) {
                            P->vis_count       = saved_vis;
                            P->scope_depth--;
                            P->in_ternary_then = saved_ternary;
                            mem_err(mem, "Expected ',' before expression argument in '%.*s'",
                                    (int)id_len, id_str);
                            return MEM_NO_IDX;
                        }
                        lex_next(&P->lex);
                    }
                    uint16_t east = parser_parse_expr(P, 0);
                    if (mem->has_error || east == MEM_NO_IDX) {
                        P->vis_count       = saved_vis;
                        P->scope_depth--;
                        P->in_ternary_then = saved_ternary;
                        return MEM_NO_IDX;
                    }
                    if (is_eq && east != MEM_NO_IDX) {
                        const AstNode *en = mem_get_ast(mem, east);
                        if (en->kind == AST_BINARY && en->ref < tab->sym_count &&
                            hardcode_is_rel_comparison_op(mem, tab, en->ref)) {
                            int sub_sid = find_active_sub_sym(mem, tab);
                            if (sub_sid >= 0) {
                                east = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                                     (uint16_t)sub_sid, en->left, en->right);
                            }
                        }
                    }
                    expr_asts[e] = east;
                }

                uint8_t total_slots = (P->local_count > slot_start)
                                      ? (uint8_t)(P->local_count - slot_start) : v_cnt;
                P->vis_count = saved_vis;
                P->scope_depth--;

                uint16_t slot_range = (uint16_t)(((uint32_t)slot_start & 0xFFu) |
                                                 (((uint32_t)total_slots & 0xFFu) << 8u));
                for (uint8_t e = 0; e < e_cnt; ++e) {
                    uint16_t lam  = mem_alloc_ast(mem, AST_LAMBDA, v_cnt, expr_asts[e], slot_range, MEM_NO_IDX);
                    uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, lam, MEM_NO_IDX);
                    if (head == MEM_NO_IDX) head = link;
                    else mem_get_ast_mut(mem, tail)->right = link;
                    tail = link;
                    argc++;
                }

                if (P->lex.cur.kind == TOK_COMMA) {
                    lex_next(&P->lex);
                    parse_rest = true;
                } else {
                    parse_rest = false;
                }
            }

            bool is_sym_call = delegate_is_sym_meta(fn_meta);
            if (is_sym_call) P->in_symbolic_expr++;
            if (parse_rest) {
                while (1) {
                    uint16_t arg = MEM_NO_IDX;
                    bool is_var_p = is_sym_call && ((fn_meta == INTR_SYM_EVAL) ? (argc == 1) : (fn_meta != INTR_SYM_SIMPLIFY && argc == 0));
                    if (is_var_p && is_sym_call &&
                        !(P->lex.cur.kind == TOK_IDENT ||
                          (P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off])))) {
                        is_var_p = false; /* allow string literal in sym_diff */
                    }
                    if (is_var_p) {
                        if (P->lex.cur.kind != TOK_IDENT &&
                            !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                            if (is_sym_call) P->in_symbolic_expr--;
                            mem_err(mem, "Variable argument to '%.*s' must be an identifier (e.g. x)",
                                    (int)id_len, id_str);
                            return MEM_NO_IDX;
                        }
                        const char *vname = P->lex.src + P->lex.cur.off;
                        uint16_t    vlen  = P->lex.cur.len;
                        int vslot = parser_add_local(P, vname, vlen, mem->fast_real_type);
                        if (mem->has_error) {
                            if (is_sym_call) P->in_symbolic_expr--;
                            return MEM_NO_IDX;
                        }
                        lex_next(&P->lex);
                        arg = mem_alloc_ast(mem, AST_SLOT_LOAD, (uint8_t)vslot, 0, 0, 0);
                    } else {
                        arg = parser_parse_expr(P, 0);
                    }
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
            if (is_sym_call) P->in_symbolic_expr--;
            P->in_ternary_then = saved_ternary;
            if (P->lex.cur.kind != TOK_RPAREN) {
                mem_err(mem, "Expected ')' after arguments to '%.*s'", (int)id_len, id_str);
            } else {
                lex_next(&P->lex);
            }

            if (argc == 0 && container_type_spec == MEM_NO_IDX) {
                int lid = find_label_id(P, id_str, id_len);
                if (lid >= 0) {
                    return mem_alloc_ast(mem, AST_RUN_TAG, (uint8_t)lid, 0, MEM_NO_IDX, MEM_NO_IDX);
                }
            }

            uint16_t st_tid = sym_find_type(tab, mem, id_str, id_len);
            if (container_type_spec == MEM_NO_IDX &&
                st_tid != MEM_TYPE_VOID && st_tid < tab->type_count &&
                !tab->types[st_tid].is_consuming &&
                (tab->types[st_tid].parser_intr == INTR_NONE ||
                 tab->types[st_tid].parser_intr == INTR_COMPLEX_TYPE)) {
                return mem_alloc_ast(mem, AST_CONSTRUCT, argc, st_tid, head, MEM_NO_IDX);
            }

            /* Variadic & lazy scripting intrinsics: print, println, assert, clock_ms, typeof */
            if (container_type_spec == MEM_NO_IDX && (!P->in_ext_body || sym_id < 0 || !sym_has_visible_func_ov(tab, (uint16_t)sym_id))) {
                if (id_len == 5 && memcmp(id_str, "print", 5) == 0) {
                    return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_PRINT, argc, head, 0);
                }
                if (id_len == 7 && memcmp(id_str, "println", 7) == 0) {
                    return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_PRINTLN, argc, head, 1);
                }
                if (id_len == 6 && memcmp(id_str, "assert", 6) == 0 && (argc == 1 || argc == 2)) {
                    return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_ASSERT, argc, head, 0);
                }
                if (id_len == 8 && memcmp(id_str, "clock_ms", 8) == 0 && argc == 0) {
                    return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_CLOCK_MS, 0, MEM_NO_IDX, MEM_NO_IDX);
                }
                if (id_len == 6 && memcmp(id_str, "typeof", 6) == 0 && argc == 1 && head != MEM_NO_IDX) {
                    return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_TYPE_NAME, 0,
                                         mem_get_ast(mem, head)->left, MEM_NO_IDX);
                }
            }

            if (sym_id < 0 || !sym_has_visible_func_ov(tab, (uint16_t)sym_id)) {
                if (sym_id >= 0) {
                    uint16_t priv_mod = MEM_NO_IDX;
                    if (sym_has_priv_ov(tab, (uint16_t)sym_id, FIX_FUNC, &priv_mod)) {
                        StrSlice pmod_s = sym_mod_name(tab, priv_mod);
                        mem_err(mem, "Function '%.*s' is private to module '[%.*s]'",
                                (int)id_len, id_str, (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
                        return MEM_NO_IDX;
                    }
                    int mv_idx = sym_find_var(tab, (uint16_t)sym_id, P->cur_mod, &priv_mod);
                    if (mv_idx < 0 && priv_mod != MEM_NO_IDX) {
                        StrSlice pmod_s = sym_mod_name(tab, priv_mod);
                        mem_err(mem, "Function '%.*s' is private to module '[%.*s]'",
                                (int)id_len, id_str, (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
                        return MEM_NO_IDX;
                    }
                }
                /* Check if `id_str` is defined later in the same script as a mutually recursive function */
                uint16_t scan_p = P->lex.cur.off;
                bool found_later_fn = false;
                while (scan_p + id_len < P->lex.len) {
                    if (memcmp(P->lex.src + scan_p, id_str, id_len) == 0 &&
                        (scan_p == 0 || !lex_is_ident_cont((unsigned char)P->lex.src[scan_p - 1])) &&
                        !lex_is_ident_cont((unsigned char)P->lex.src[scan_p + id_len])) {
                        uint16_t q = skip_raw_ws(P->lex.src, (uint16_t)(scan_p + id_len), P->lex.len);
                        if (q < P->lex.len && (P->lex.src[q] == '(' || P->lex.src[q] == ':' || P->lex.src[q] == '=')) {
                            found_later_fn = true;
                            break;
                        }
                    }
                    scan_p++;
                }
                if (found_later_fn && mem->fn_type_id != 0) {
                    if (P->in_ext_body) {
                        StrSlice fslice = mem_intern_str(mem, id_str, id_len);
                        uint16_t fsid   = sym_intern(tab, mem, fslice);
                        return mem_alloc_ast(mem, AST_CALL, argc, fsid, head, MEM_NO_IDX);
                    }
                    if (P->is_mod_scope) {
                        StrSlice fslice = mem_intern_str(mem, id_str, id_len);
                        uint16_t fsid   = sym_intern(tab, mem, fslice);
                        int f_idx = sym_set_or_add_var(tab, mem, fsid, P->cur_mod, val_void(),
                                                       0, 0, SYM_VAR_MUTABLE, true);
                        if (f_idx >= 0) {
                            sym_vars_mut(tab)[f_idx].lock_type = (uint16_t)((uint32_t)mem->fn_type_id << 8u);
                        }
                        uint16_t fn_load = mem_alloc_ast(mem, AST_CONST_SYM, 0, fsid, MEM_NO_IDX, P->cur_mod);
                        return mem_alloc_ast(mem, AST_FN_CALL, argc, 0, fn_load, head);
                    } else if (P->vis_count < PARSER_MAX_LOCALS && P->local_count < PARSER_MAX_LOCALS) {
                        uint8_t fwd_slot = P->local_count++;
                        /* Insert at bottom of P->locals with scope_depth = 0 so outer scope sees it */
                        memmove(&P->locals[1], &P->locals[0], (size_t)P->vis_count * sizeof(LocalVar));
                        P->vis_count++;
                        P->locals[0].name         = mem_intern_str(mem, id_str, id_len);
                        P->locals[0].type_id      = mem->fn_type_id;
                        P->locals[0].slot         = fwd_slot;
                        P->locals[0].scope_depth  = 0;
                        P->locals[0].is_loop_iter = 0;
                        P->locals[0].is_const     = 0;
                        P->slot_types[fwd_slot]   = mem->fn_type_id;
                        if (!P->in_ext_body && fwd_slot < 64u) {
                            mem->local_slot_names[fwd_slot] = P->locals[0].name;
                        }
                        uint16_t fn_load = mem_alloc_ast(mem, AST_SLOT_LOAD, fwd_slot, 0, 0, 0);
                        return mem_alloc_ast(mem, AST_FN_CALL, argc, 0, fn_load, head);
                    }
                }
                mem_err(mem, "Undefined function or struct '%.*s'", (int)id_len, id_str);
                return MEM_NO_IDX;
            }
            if (container_type_spec == MEM_NO_IDX && fn_meta == INTR_FCALL &&
                argc >= 1 && head != MEM_NO_IDX) {
                const AstNode *first_link = mem_get_ast(mem, head);
                return mem_alloc_ast(mem, AST_FN_CALL, (uint8_t)(argc - 1), 0,
                                     first_link->left, first_link->right);
            }
            return mem_alloc_ast(mem, AST_CALL, argc, (uint16_t)sym_id, head, container_type_spec);
        }

        /* 6g. Module Variable or Named Constant (`π`, `e`, `g0`, `c0`, user module vars) */
        if (sym_id >= 0 && P->in_symbolic_expr == 0) {
            uint16_t priv_mod = MEM_NO_IDX;
            int mv_idx = sym_find_var(tab, (uint16_t)sym_id, P->cur_mod, &priv_mod);
            if (mv_idx >= 0) {
                return mem_alloc_ast(mem, AST_CONST_SYM, 0, (uint16_t)sym_id, MEM_NO_IDX, P->cur_mod);
            }
            if (priv_mod != MEM_NO_IDX) {
                StrSlice pmod_s = sym_mod_name(tab, priv_mod);
                mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                        (int)id_len, id_str, (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
                return MEM_NO_IDX;
            }
        } else if (sym_id >= 0 && P->in_symbolic_expr > 0) {
            for (uint8_t i = 0; i < tab->const_count; ++i) {
                if (tab->consts[i].sym_id == (uint16_t)sym_id &&
                    sym_mod_enabled(tab, tab->consts[i].mod_id) &&
                    ((tab->consts[i].reserved & SYM_VAR_PRIVATE) == 0 || tab->consts[i].mod_id == P->cur_mod)) {
                    return mem_alloc_ast(mem, AST_CONST_SYM, 0, (uint16_t)sym_id, MEM_NO_IDX, P->cur_mod);
                }
            }
        }

        if (P->in_symbolic_expr > 0) {
            int sslot = parser_add_local(P, id_str, id_len, mem->fast_real_type);
            if (mem->has_error) return MEM_NO_IDX;
            return mem_alloc_ast(mem, AST_SLOT_LOAD, (uint8_t)sslot, 0, 0, 0);
        }
        mem_err(mem, "Undefined symbol or constant '%.*s'", (int)id_len, id_str);
        return MEM_NO_IDX;
    }

    mem_err(mem, "Unexpected token in expression");
    return MEM_NO_IDX;
}

