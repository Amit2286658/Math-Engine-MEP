/*
 * parser_core.c — Parser State, Local Slots, Labels, Lookahead & Assignment AST Builders
 */
#include "parser_internal.h"

bool is_var_modifier_kw(const char *s, uint16_t len) {
    return (len == 3 && (memcmp(s, "pub", 3) == 0 || memcmp(s, "mut", 3) == 0)) ||
           (len == 4 && memcmp(s, "priv", 4) == 0) ||
           (len == 5 && memcmp(s, "const", 5) == 0);
}

bool is_try_kw(const char *s, uint16_t len) {
    return (len == 3 && memcmp(s, "try", 3) == 0);
}

bool is_catch_kw(const char *s, uint16_t len) {
    return (len == 5 && memcmp(s, "catch", 5) == 0);
}

bool is_throw_kw(const char *s, uint16_t len) {
    return (len == 5 && memcmp(s, "throw", 5) == 0);
}

int find_local_idx(const Parser *P, const char *name, uint16_t len) {
    for (int i = (int)P->vis_count - 1; i >= 0; --i) {
        StrSlice s = P->locals[i].name;
        if (s.len == len && memcmp(mem_str_ptr(P->lex.mem, s), name, len) == 0) {
            return i;
        }
    }
    return -1;
}

int parser_add_local(Parser *P, const char *name, uint16_t len, uint16_t type_id) {
    if (sym_is_reserved_kw(P->lex.tab, P->lex.mem, name, len) || is_var_modifier_kw(name, len)) {
        mem_err(P->lex.mem, "Cannot use reserved keyword '%.*s' as a variable name",
                (int)len, name);
        return 0;
    }
    int existing = find_local_idx(P, name, len);
    if (existing >= 0 && P->locals[existing].scope_depth == P->scope_depth) {
        return (int)P->locals[existing].slot;
    }
    if (P->vis_count >= PARSER_MAX_LOCALS || P->local_count >= PARSER_MAX_LOCALS) {
        mem_err(P->lex.mem, "Too many local variables (max %d)", PARSER_MAX_LOCALS);
        return 0;
    }
    uint8_t slot = P->local_count++;
    uint8_t vidx = P->vis_count++;
    P->locals[vidx].name         = mem_intern_str(P->lex.mem, name, len);
    P->locals[vidx].type_id      = type_id;
    P->locals[vidx].slot         = slot;
    P->locals[vidx].scope_depth  = P->scope_depth;
    P->locals[vidx].is_loop_iter = 0;
    P->locals[vidx].is_const     = 0;
    P->slot_types[slot]          = type_id;
    if (!P->in_ext_body && slot < 64u) {
        P->lex.mem->local_slot_names[slot] = P->locals[vidx].name;
    }
    return (int)slot;
}

uint8_t parser_alloc_anon_slot(Parser *P, uint16_t type_id) {
    if (P->local_count >= PARSER_MAX_LOCALS) {
        mem_err(P->lex.mem, "Too many local variables (max %d)", PARSER_MAX_LOCALS);
        return 0;
    }
    uint8_t slot = P->local_count++;
    P->slot_types[slot] = type_id;
    return slot;
}

int find_label_id(const Parser *P, const char *name, uint16_t len) {
    for (uint8_t i = 0; i < P->label_count; ++i) {
        StrSlice s = P->labels[i].name;
        if (s.len == len && memcmp(mem_str_ptr(P->lex.mem, s), name, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int parser_get_or_add_label(Parser *P, const char *name, uint16_t len) {
    if (sym_is_reserved_kw(P->lex.tab, P->lex.mem, name, len) || is_var_modifier_kw(name, len)) {
        mem_err(P->lex.mem, "Cannot use reserved keyword '%.*s' as a scope tag",
                (int)len, name);
        return 0;
    }
    int existing = find_label_id(P, name, len);
    if (existing >= 0) return existing;
    if (P->label_count >= MEM_MAX_LABELS) {
        mem_err(P->lex.mem, "Too many scope tags (max %d)", MEM_MAX_LABELS);
        return 0;
    }
    uint8_t id = P->label_count++;
    P->labels[id].name     = mem_intern_str(P->lex.mem, name, len);
    P->labels[id].body_ast = MEM_NO_IDX;
    P->labels[id].defined  = false;
    return (int)id;
}

bool parser_check_labels(Parser *P) {
    if (P->lex.mem->has_error) return false;
    for (uint8_t i = 0; i < P->label_count; ++i) {
        if (!P->labels[i].defined) {
            StrSlice s = P->labels[i].name;
            mem_err(P->lex.mem, "Undefined scope tag '%.*s'",
                    (int)s.len, mem_str_ptr(P->lex.mem, s));
            return false;
        }
    }
    return true;
}

void parser_init(Parser *P, MemArena *mem, SymTable *tab,
                 const char *src, uint16_t len, uint16_t start_pos,
                 bool in_ext_body, bool stop_at_newline) {
    memset(P, 0, sizeof(*P));
    lex_init(&P->lex, mem, tab, src, len, start_pos, stop_at_newline);
    P->in_ext_body = in_ext_body;
    P->cur_mod     = tab ? tab->cur_mod : 0;
}

/* ============================================================================
 * 1. TOKEN, OPERATOR & META-INTRINSIC INSPECTION HELPERS
 * ============================================================================ */

bool sym_has_visible_func_ov(const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        const OpOverload *ov = &sym_overloads(tab)[i];
        if (ov->fixity == FIX_FUNC && sym_ov_is_visible(tab, ov)) {
            return true;
        }
    }
    return false;
}

bool sym_has_func_ov_in_mod(const SymTable *tab, uint16_t sym_id, uint16_t mod_id,
                                   uint16_t caller_mod, bool *out_is_priv) {
    if (out_is_priv) *out_is_priv = false;
    if (sym_id >= tab->sym_count || !sym_mod_enabled(tab, mod_id)) return false;
    bool saw_priv = false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        const OpOverload *ov = &sym_overloads(tab)[i];
        if (ov->fixity != FIX_FUNC || ov->mod_id != mod_id) continue;
        if ((ov->reserved & SYM_OV_PRIVATE) != 0 && mod_id != caller_mod) {
            saw_priv = true;
            continue;
        }
        return true;
    }
    if (out_is_priv) *out_is_priv = saw_priv;
    return false;
}

uint8_t token_func_meta(const Parser *P, Token t) {
    SymTable *tab = P->lex.tab;
    const MemArena *mem = P->lex.mem;
    if (t.kind == TOK_OP) {
        return sym_active_meta_tag(tab, t.sym_id, FIX_FUNC);
    }
    if (t.kind == TOK_IDENT) {
        int sid = sym_find(tab, mem, P->lex.src + t.off, t.len);
        if (sid >= 0) {
            return sym_active_meta_tag(tab, (uint16_t)sid, FIX_FUNC);
        }
    }
    return 0;
}

uint8_t cur_op_meta(const Parser *P) {
    if (P->lex.cur.kind != TOK_OP) return 0;
    uint8_t m = sym_active_meta_tag(P->lex.tab, P->lex.cur.sym_id, FIX_INFIX_LEFT);
    if (!m) m = sym_active_meta_tag(P->lex.tab, P->lex.cur.sym_id, FIX_INFIX_RIGHT);
    return m;
}

uint16_t skip_raw_ws(const char *src, uint16_t p, uint16_t len) {
    while (p < len) {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p++;
        } else if (c == '#') {
            while (p < len && src[p] != '\n') p++;
        } else {
            break;
        }
    }
    return p;
}

bool lex_at_is_syntax(const Lexer *lex, uint16_t p, uint8_t target_syn) {
    p = skip_raw_ws(lex->src, p, lex->len);
    for (uint16_t sid = 0; sid < lex->tab->sym_count; ++sid) {
        if (sym_get_active_syntax(lex->tab, sid) != target_syn) continue;
        StrSlice s = lex->tab->syms[sid];
        if (s.len > 0 && p + s.len <= lex->len &&
            memcmp(lex->src + p, mem_str_ptr(lex->mem, s), s.len) == 0) {
            return true;
        }
    }
    return false;
}

bool lex_peek_is_lbrace(const Lexer *lex) {
    return lex_at_is_syntax(lex, lex->pos, INTR_LBRACE);
}

bool lex_peek_is_lparen(const Lexer *lex) {
    return lex_at_is_syntax(lex, lex->pos, INTR_LPAREN);
}

static bool lex_match_in_kw_at(const Parser *P, uint16_t p) {
    if (p >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[p])) return false;
    uint16_t istart = p;
    while (p < P->lex.len && lex_is_ident_cont((unsigned char)P->lex.src[p])) p++;
    return hardcode_is_in_kw(P->lex.mem, P->lex.tab, P->lex.src + istart, (uint16_t)(p - istart));
}

bool lex_peek_is_ident_in(const Parser *P) {
    uint16_t p = skip_raw_ws(P->lex.src, P->lex.pos, P->lex.len);
    if (p >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[p])) return false;
    while (p < P->lex.len && lex_is_ident_cont((unsigned char)P->lex.src[p])) p++;
    p = skip_raw_ws(P->lex.src, p, P->lex.len);
    if (p < P->lex.len && P->lex.src[p] == ',') {
        p = skip_raw_ws(P->lex.src, (uint16_t)(p + 1), P->lex.len);
        if (p >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[p])) return false;
        while (p < P->lex.len && lex_is_ident_cont((unsigned char)P->lex.src[p])) p++;
        p = skip_raw_ws(P->lex.src, p, P->lex.len);
    }
    return lex_match_in_kw_at(P, p);
}

bool lex_is_two_var_in(const Parser *P) {
    const Lexer *lex = &P->lex;
    uint16_t p = skip_raw_ws(lex->src, lex->pos, lex->len);
    if (p >= lex->len || !lex_is_ident_start((unsigned char)lex->src[p])) return false;
    while (p < lex->len && lex_is_ident_cont((unsigned char)lex->src[p])) p++;
    p = skip_raw_ws(lex->src, p, lex->len);
    return lex_match_in_kw_at(P, p);
}

bool lex_peek_is_branch_kw(const Parser *P) {
    uint16_t p = skip_raw_ws(P->lex.src, P->lex.pos, P->lex.len);
    if (p >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[p])) return false;
    uint16_t start = p;
    while (p < P->lex.len && lex_is_ident_cont((unsigned char)P->lex.src[p])) p++;
    uint16_t kw_len = (uint16_t)(p - start);
    if (is_catch_kw(P->lex.src + start, kw_len)) return true;
    int sid = sym_find(P->lex.tab, P->lex.mem, P->lex.src + start, kw_len);
    if (sid < 0) return false;
    uint8_t m = sym_active_meta_tag(P->lex.tab, (uint16_t)sid, FIX_FUNC);
    return (m == INTR_OR_BRANCH || m == INTR_ELSE_BRANCH);
}

int find_active_meta_sym(const SymTable *tab, uint8_t target_meta) {
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        const OpOverload *ov = &sym_overloads(tab)[i];
        if (ov->meta_tag == target_meta && sym_ov_is_visible(tab, ov)) {
            return (int)ov->sym_id;
        }
    }
    return -1;
}

bool lex_is_multi_lhs(const Parser *P) {
    const Lexer    *lex = &P->lex;
    const SymTable *tab = lex->tab;
    const MemArena *mem = lex->mem;
    uint16_t p = lex->pos;

    while (p < lex->len) {
        p = skip_raw_ws(lex->src, p, lex->len);
        if (p >= lex->len || !lex_is_ident_start((unsigned char)lex->src[p])) {
            return false;
        }
        uint16_t id_s = p;
        while (p < lex->len && lex_is_ident_cont((unsigned char)lex->src[p])) {
            p++;
        }
        uint16_t id_l = (uint16_t)(p - id_s);
        if (sym_is_reserved_kw(tab, mem, lex->src + id_s, id_l) ||
            is_var_modifier_kw(lex->src + id_s, id_l) ||
            (id_l == 7 && memcmp(lex->src + id_s, "default", 7) == 0)) {
            return false;
        }
        p = skip_raw_ws(lex->src, p, lex->len);

        bool matched_comma = false;
        for (uint16_t sid = 0; sid < tab->sym_count; ++sid) {
            if (sym_get_active_syntax(tab, sid) != INTR_COMMA) continue;
            StrSlice s = tab->syms[sid];
            if (s.len > 0 && p + s.len <= lex->len &&
                memcmp(lex->src + p, mem_str_ptr(mem, s), s.len) == 0) {
                p += s.len;
                matched_comma = true;
                break;
            }
        }
        if (!matched_comma) break;
    }

    int best_len = 0;
    uint16_t best_sid = 0;
    for (uint16_t sid = 0; sid < tab->sym_count; ++sid) {
        if (!sym_has_active_op(tab, sid)) continue;
        StrSlice s = tab->syms[sid];
        if (s.len > 0 && p + s.len <= lex->len &&
            memcmp(lex->src + p, mem_str_ptr(mem, s), s.len) == 0) {
            if ((int)s.len > best_len) {
                best_len = (int)s.len;
                best_sid = sid;
            }
        }
    }
    if (best_len == 0) return false;
    uint8_t m = sym_active_meta_tag(tab, best_sid, FIX_INFIX_RIGHT);
    return (m == INTR_STORE || m == INTR_COLON);
}

uint16_t resolve_annot_type_ext(const SymTable *tab, const MemArena *mem,
                                const char *name, uint16_t len) {
    int sid = sym_find(tab, mem, name, len);
    if (sid >= 0) {
        if (mem->vec_type_id != 0 && hardcode_is_vec_ctor_sym(mem, tab, (uint16_t)sid)) {
            return mem->vec_type_id;
        }
        if (mem->map_type_id != 0 && hardcode_is_map_ctor_sym(mem, tab, (uint16_t)sid)) {
            return mem->map_type_id;
        }
    }
    return sym_find_type(tab, mem, name, len);
}

bool sym_has_non_lazy_func_ov(const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        const OpOverload *ov = &sym_overloads(tab)[i];
        if (ov->fixity == FIX_FUNC && sym_ov_is_visible(tab, ov) &&
            (ov->meta_tag & SYM_META_LAZY_MASK) == 0) {
            return true;
        }
    }
    return false;
}

int find_active_sub_sym(const MemArena *mem, const SymTable *tab) {
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        const OpOverload *ov = &sym_overloads(tab)[i];
        if (ov->fixity == FIX_INFIX_LEFT && ov->param_count == 2 &&
            sym_ov_is_visible(tab, ov) && ov->body_ast != MEM_NO_IDX) {
            const AstNode *root = mem_get_ast(mem, ov->body_ast);
            if (root->kind == AST_INTRINSIC && root->sub == INTR_SUB) {
                return (int)ov->sym_id;
            }
        }
    }
    return -1;
}

bool parse_var_type_unit_annot(Parser *P, uint8_t *out_lock_type, uint8_t *out_lock_unit) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    *out_lock_type = 0;
    *out_lock_unit = 0;

    lex_next(&P->lex); /* consume ':' */
    if (P->lex.cur.kind != TOK_IDENT &&
        !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
        mem_err(mem, "Expected type or unit name after ':' in variable declaration");
        return false;
    }
    const char *s1_ptr = P->lex.src + P->lex.cur.off;
    uint16_t    s1_len = P->lex.cur.len;
    lex_next(&P->lex);

    /* Parameterized `<T>`, `<T, U>`, or `<U>` on container variable type annotation `v : vec<vec2> := ...` */
    if (P->lex.cur.kind == TOK_OP &&
        tab->syms[P->lex.cur.sym_id].len == 1 &&
        mem_str_ptr(mem, tab->syms[P->lex.cur.sym_id])[0] == '<') {
        uint16_t outer_tid = resolve_annot_type_ext(tab, mem, s1_ptr, s1_len);
        if (outer_tid == MEM_TYPE_VOID ||
            (outer_tid != mem->vec_type_id && outer_tid != mem->map_type_id && outer_tid != mem->mat_type_id)) {
            mem_err(mem, "Type '%.*s' does not take '<...>' element type parameters", (int)s1_len, s1_ptr);
            return false;
        }
        lex_next(&P->lex); /* consume '<' */
        if (P->lex.cur.kind != TOK_IDENT &&
            !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
            mem_err(mem, "Expected element type or unit name inside '%.*s<...>'", (int)s1_len, s1_ptr);
            return false;
        }
        const char *e1_ptr = P->lex.src + P->lex.cur.off;
        uint16_t    e1_len = P->lex.cur.len;
        lex_next(&P->lex);

        uint16_t elem_tid = 0;
        uint8_t  elem_uid = 0;
        if (P->lex.cur.kind == TOK_COMMA) {
            lex_next(&P->lex); /* consume ',' inside '<...>' */
            if (P->lex.cur.kind != TOK_IDENT &&
                !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                mem_err(mem, "Expected unit name after ',' inside '%.*s<...>'", (int)s1_len, s1_ptr);
                return false;
            }
            const char *e2_ptr = P->lex.src + P->lex.cur.off;
            uint16_t    e2_len = P->lex.cur.len;
            lex_next(&P->lex);

            elem_tid = resolve_annot_type_ext(tab, mem, e1_ptr, e1_len);
            if (elem_tid == MEM_TYPE_VOID) {
                mem_err(mem, "Unknown element type '%.*s' in '%.*s<...>'",
                        (int)e1_len, e1_ptr, (int)s1_len, s1_ptr);
                return false;
            }
            elem_uid = sym_find_or_add_unit(tab, mem, e2_ptr, e2_len);
            if (mem->has_error) return false;
        } else {
            elem_tid = resolve_annot_type_ext(tab, mem, e1_ptr, e1_len);
            if (elem_tid == MEM_TYPE_VOID) {
                elem_uid = sym_find_unit(tab, mem, e1_ptr, e1_len);
                if (elem_uid == MEM_UNIT_NONE) {
                    mem_err(mem, "Unknown element type '%.*s' in '%.*s<...>'",
                            (int)e1_len, e1_ptr, (int)s1_len, s1_ptr);
                    return false;
                }
                elem_tid = mem->fast_real_type;
            }
        }

        if (P->lex.cur.kind != TOK_OP ||
            tab->syms[P->lex.cur.sym_id].len != 1 ||
            mem_str_ptr(mem, tab->syms[P->lex.cur.sym_id])[0] != '>') {
            mem_err(mem, "Expected closing '>' in '%.*s<...>'", (int)s1_len, s1_ptr);
            return false;
        }
        lex_next(&P->lex); /* consume '>' */

        if (outer_tid == mem->mat_type_id && elem_tid != 0 &&
            elem_tid != SYM_TYPE_ANY && elem_tid != mem->fast_real_type) {
            mem_err(mem, "Matrix type 'mat<...>' only supports 'real' elements");
            return false;
        }

        if (P->lex.cur.kind == TOK_COMMA) {
            lex_next(&P->lex); /* optional trailing `, unit` after `vec<real>` */
            if (P->lex.cur.kind != TOK_IDENT &&
                !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
                mem_err(mem, "Expected unit name after ',' in variable type annotation");
                return false;
            }
            elem_uid = sym_find_or_add_unit(tab, mem, P->lex.src + P->lex.cur.off, P->lex.cur.len);
            lex_next(&P->lex);
            if (mem->has_error) return false;
        }

        *out_lock_type = sym_pack_container_type(mem, outer_tid, elem_tid);
        *out_lock_unit = (elem_uid == SYM_UNIT_GENERIC || elem_uid == SYM_UNIT_OMITTED) ? 0 : elem_uid;
        return !mem->has_error;
    }

    if (P->lex.cur.kind == TOK_COMMA) {
        lex_next(&P->lex); /* consume ',' */
        if (P->lex.cur.kind != TOK_IDENT &&
            !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
            mem_err(mem, "Expected unit name after ',' in variable type annotation");
            return false;
        }
        const char *s2_ptr = P->lex.src + P->lex.cur.off;
        uint16_t    s2_len = P->lex.cur.len;
        lex_next(&P->lex);

        uint16_t tid = resolve_annot_type_ext(tab, mem, s1_ptr, s1_len);
        if (tid == MEM_TYPE_VOID) {
            mem_err(mem, "Unknown entity type '%.*s' in variable declaration", (int)s1_len, s1_ptr);
            return false;
        }
        uint8_t uid = sym_find_or_add_unit(tab, mem, s2_ptr, s2_len);
        *out_lock_type = (tid == SYM_TYPE_ANY) ? 0 : (uint8_t)tid;
        *out_lock_unit = (uid == SYM_UNIT_GENERIC) ? 0 : uid;
        return !mem->has_error;
    }

    uint16_t tid = resolve_annot_type_ext(tab, mem, s1_ptr, s1_len);
    if (tid == SYM_TYPE_ANY) {
        *out_lock_type = 0;
        *out_lock_unit = 0;
        return true;
    }
    if (tid != MEM_TYPE_VOID) {
        *out_lock_type = (uint8_t)tid;
        *out_lock_unit = 0;
        return true;
    }

    uint8_t uid = sym_find_or_add_unit(tab, mem, s1_ptr, s1_len);
    *out_lock_type = 0;
    *out_lock_unit = (uid == SYM_UNIT_GENERIC) ? 0 : uid;
    return !mem->has_error;
}

bool ast_is_fn_expr_rec(const MemArena *mem, const SymTable *tab,
                               const Parser *P, uint16_t idx, uint8_t depth) {
    if (idx == MEM_NO_IDX || depth > 12 || mem->fn_type_id == 0) return false;
    const AstNode *n = mem_get_ast(mem, idx);
    switch ((AstKind)n->kind) {
        case AST_LAMBDA:
        case AST_FN_CALL:
            return true;
        case AST_SLOT_LOAD:
            if (P != NULL) {
                uint8_t s = (uint8_t)(n->sub & AST_SLOT_IDX_MASK);
                return (sym_unpack_outer_type(mem, P->slot_types[s]) == mem->fn_type_id);
            }
            return false;
        case AST_CONST_SYM: {
            uint16_t cmod = (n->right != MEM_NO_IDX && n->right < tab->mod_count)
                            ? n->right : (P ? P->cur_mod : 0);
            int mv_idx = ((n->sub & SYM_VAR_QUAL_MOD) != 0)
                         ? sym_find_own_var(tab, n->ref, cmod)
                         : sym_find_var(tab, n->ref, cmod, NULL);
            if (mv_idx >= 0) {
                const ModVarDef *mv = &sym_vars(tab)[mv_idx];
                uint16_t mv_tid = val_get_type(mem, mv->val);
                if (mv_tid == MEM_TYPE_VOID) {
                    mv_tid = (mv->lock_type & 0xFFu) ? (uint16_t)(mv->lock_type & 0xFFu)
                                                     : (uint16_t)((mv->lock_type >> 8u) & 0xFFu);
                    mv_tid = sym_unpack_outer_type(mem, mv_tid);
                }
                return (mv_tid == mem->fn_type_id);
            }
            return false;
        }
        case AST_FIELD_LOAD:
            return (sym_unpack_outer_type(mem, n->sub) == mem->fn_type_id);
        case AST_RETURN:
            return (n->sub == 0 && ast_is_fn_expr_rec(mem, tab, P, n->left, (uint8_t)(depth + 1)));
        case AST_BREAK:
            return ast_is_fn_expr_rec(mem, tab, P, n->left, (uint8_t)(depth + 1));
        case AST_TAGGED_SCOPE:
            return ast_is_fn_expr_rec(mem, tab, P, n->left, (uint8_t)(depth + 1));
        case AST_TERNARY: {
            if (n->sub == 2) {
                const AstNode *pair = mem_get_ast(mem, n->right);
                return ast_is_fn_expr_rec(mem, tab, P, n->left, (uint8_t)(depth + 1)) ||
                       ast_is_fn_expr_rec(mem, tab, P, pair->right, (uint8_t)(depth + 1));
            }
            const AstNode *pair = mem_get_ast(mem, n->right);
            return ast_is_fn_expr_rec(mem, tab, P, pair->left, (uint8_t)(depth + 1)) ||
                   ast_is_fn_expr_rec(mem, tab, P, pair->right, (uint8_t)(depth + 1));
        }
        case AST_BINARY: {
            uint8_t mtag = sym_active_meta_tag(tab, n->ref, (OpFixity)n->sub);
            if (mtag == INTR_SEQ) {
                return ast_is_fn_expr_rec(mem, tab, P, n->right, (uint8_t)(depth + 1)) ||
                       ast_is_fn_expr_rec(mem, tab, P, n->left, (uint8_t)(depth + 1));
            }
            if (mtag == INTR_STORE) {
                return ast_is_fn_expr_rec(mem, tab, P, n->right, (uint8_t)(depth + 1));
            }
            return false;
        }
        case AST_CALL: {
            if (n->ref >= tab->sym_count) return false;
            for (uint16_t i = tab->sym_first_ov[n->ref]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
                const OpOverload *ov = &sym_overloads(tab)[i];
                if (ov->fixity != FIX_FUNC || ov->param_count != n->sub ||
                    !sym_mod_enabled(tab, ov->mod_id)) continue;
                if (sym_unpack_outer_type(mem, ov->ret_type) == mem->fn_type_id) return true;
                if (ov->body_ast != MEM_NO_IDX &&
                    ast_is_fn_expr_rec(mem, tab, NULL, ov->body_ast, (uint8_t)(depth + 1))) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

static uint16_t ast_infer_type_rec(const Parser *P, uint16_t idx, uint8_t depth) {
    const MemArena *mem = P->lex.mem;
    const SymTable *tab = P->lex.tab;
    if (idx == MEM_NO_IDX || depth > 8) return 0;
    const AstNode *rn = mem_get_ast(mem, idx);

    if (rn->kind == AST_CONSTRUCT) return rn->ref;
    if (rn->kind == AST_FIELD_LOAD && rn->sub != 0) return rn->sub;
    if (rn->kind == AST_LIT && rn->sub != 0) return rn->sub;
    if (ast_is_fn_expr_rec(mem, tab, P, idx, 0)) return mem->fn_type_id;

    if (rn->kind == AST_SLOT_LOAD) {
        uint8_t s = (uint8_t)(rn->sub & AST_SLOT_IDX_MASK);
        if (s < PARSER_MAX_LOCALS) return P->slot_types[s];
        return 0;
    }

    if (rn->kind == AST_CONST_SYM) {
        uint16_t cmod = (rn->right != MEM_NO_IDX && rn->right < tab->mod_count)
                        ? rn->right : P->cur_mod;
        int mv_idx = ((rn->sub & SYM_VAR_QUAL_MOD) != 0)
                     ? sym_find_own_var(tab, rn->ref, cmod)
                     : sym_find_var(tab, rn->ref, cmod, NULL);
        if (mv_idx >= 0) {
            const ModVarDef *mv = &sym_vars(tab)[mv_idx];
            uint8_t l_low = (uint8_t)(mv->lock_type & 0xFFu);
            uint8_t l_hi  = (uint8_t)((mv->lock_type >> 8u) & 0xFFu);
            if (sym_type_is_parameterized(l_low)) return l_low;
            if (sym_type_is_parameterized(l_hi))  return l_hi;
            if (l_low != 0) return l_low;
            if (l_hi != 0)  return l_hi;
            uint16_t vtid = val_get_type(mem, mv->val);
            if ((vtid == mem->vec_type_id && mem->vec_type_id != 0) ||
                (vtid == mem->map_type_id && mem->map_type_id != 0)) {
                uint16_t raw0 = (uint16_t)mem_read_pool(mem, (uint16_t)(val_get_pool_idx(mv->val) + 0));
                uint16_t etid = (uint16_t)(raw0 & 0xFFu);
                return (etid != 0) ? sym_pack_container_type(mem, vtid, etid) : vtid;
            }
            return vtid;
        }
        return 0;
    }

    if (rn->kind == AST_INDEX && rn->sub == 1) {
        uint16_t base_t = ast_infer_type_rec(P, rn->left, (uint8_t)(depth + 1));
        uint16_t elem_t = sym_unpack_elem_type(mem, base_t);
        if (elem_t != 0 && elem_t != SYM_TYPE_ANY) return elem_t;
        return 0;
    }

    if (rn->kind == AST_MAT_LIT) {
        if (rn->sub > 0 && rn->left != MEM_NO_IDX) {
            uint16_t first_item = mem_get_ast(mem, rn->left)->left;
            uint16_t it_t       = ast_infer_type_rec(P, first_item, (uint8_t)(depth + 1));
            uint16_t it_outer   = sym_unpack_outer_type(mem, it_t);
            if (it_outer != 0 && it_outer != mem->mat_type_id && it_outer != mem->fast_real_type) {
                return sym_pack_container_type(mem, mem->vec_type_id, it_outer);
            }
        }
        return mem->mat_type_id;
    }

    if (rn->kind == AST_FOR_IN && (rn->ref & 0x8000u) != 0) {
        const AstNode *pair = mem_get_ast(mem, rn->right);
        uint16_t map_t     = ast_infer_type_rec(P, pair->left, (uint8_t)(depth + 1));
        uint16_t map_outer = sym_unpack_outer_type(mem, map_t);
        if (map_outer != 0 && map_outer != SYM_TYPE_ANY) {
            return sym_pack_container_type(mem, mem->vec_type_id, map_outer);
        }
        return mem->vec_type_id;
    }

    if (rn->kind == AST_BINARY) {
        uint8_t mtag = sym_active_meta_tag(tab, rn->ref, (OpFixity)rn->sub);
        if (mtag == INTR_SEQ || mtag == INTR_STORE) {
            return ast_infer_type_rec(P, rn->right, (uint8_t)(depth + 1));
        }
        if (hardcode_is_into_op_sym(mem, tab, rn->ref)) {
            uint16_t rhs_t = ast_infer_type_rec(P, rn->right, (uint8_t)(depth + 1));
            if (sym_unpack_outer_type(mem, rhs_t) == mem->vec_type_id) {
                uint16_t item_t = sym_unpack_outer_type(mem, ast_infer_type_rec(P, rn->left, (uint8_t)(depth + 1)));
                if (item_t != 0 && item_t != SYM_TYPE_ANY && sym_unpack_elem_type(mem, rhs_t) == 0) {
                    return sym_pack_container_type(mem, mem->vec_type_id, item_t);
                }
                return rhs_t;
            }
        }
    }

    if (rn->kind == AST_CALL && rn->ref < tab->sym_count) {
        bool has_spec = (rn->right != 0 && rn->right != MEM_NO_IDX && (rn->right & 0x8000u) != 0);
        if (has_spec) {
            uint8_t  raw_ltid = (uint8_t)(rn->right & 0x7Fu);
            uint16_t elem_t   = (raw_ltid == SYM_ELEM_ANY) ? SYM_TYPE_ANY
                                : (raw_ltid != 0 ? raw_ltid : mem->fast_real_type);
            if (hardcode_is_map_ctor_sym(mem, tab, rn->ref)) {
                return sym_pack_container_type(mem, mem->map_type_id, elem_t);
            }
            if (hardcode_is_vec_ctor_sym(mem, tab, rn->ref)) {
                return sym_pack_container_type(mem, mem->vec_type_id, elem_t);
            }
        }
        if (hardcode_is_vec_ctor_sym(mem, tab, rn->ref)) {
            if (rn->sub == 0) return mem->vec_type_id;
            const AstNode *l0 = (rn->left != MEM_NO_IDX) ? mem_get_ast(mem, rn->left) : NULL;
            if (l0) {
                const AstNode *a0_n = mem_get_ast(mem, l0->left);
                if (rn->sub == 1 && a0_n && a0_n->kind == AST_LIT && a0_n->sub == mem->fast_real_type) {
                    return mem->vec_type_id; /* `vec(cap)` */
                }
                if (rn->sub == 2 && l0->right != MEM_NO_IDX && a0_n &&
                    a0_n->kind == AST_LIT && a0_n->sub == mem->fast_real_type) {
                    const AstNode *l1 = mem_get_ast(mem, l0->right);
                    uint16_t t1 = sym_unpack_outer_type(mem, ast_infer_type_rec(P, l1->left, (uint8_t)(depth + 1)));
                    if (t1 != 0 && t1 != SYM_TYPE_ANY) {
                        return sym_pack_container_type(mem, mem->vec_type_id, t1);
                    }
                }
                uint16_t t0 = sym_unpack_outer_type(mem, ast_infer_type_rec(P, l0->left, (uint8_t)(depth + 1)));
                if (t0 != 0 && t0 != SYM_TYPE_ANY) {
                    return sym_pack_container_type(mem, mem->vec_type_id, t0);
                }
            }
            return mem->vec_type_id;
        }
        if (hardcode_is_map_ctor_sym(mem, tab, rn->ref)) {
            if (rn->sub == 0) return mem->map_type_id;
            if ((rn->sub & 1u) == 0 && rn->left != MEM_NO_IDX) {
                const AstNode *lk = mem_get_ast(mem, rn->left);
                const AstNode *lv = (lk->right != MEM_NO_IDX) ? mem_get_ast(mem, lk->right) : NULL;
                if (lv) {
                    uint16_t tv = sym_unpack_outer_type(mem, ast_infer_type_rec(P, lv->left, (uint8_t)(depth + 1)));
                    if (tv != 0 && tv != SYM_TYPE_ANY && tv != mem->fn_type_id) {
                        return sym_pack_container_type(mem, mem->map_type_id, tv);
                    }
                }
            }
        }

        uint16_t arg0_t = 0;
        if (rn->sub >= 1 && rn->left != MEM_NO_IDX) {
            arg0_t = ast_infer_type_rec(P, mem_get_ast(mem, rn->left)->left, (uint8_t)(depth + 1));
        }

        uint16_t fallback_ret = 0;
        for (uint16_t i = tab->sym_first_ov[rn->ref]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
            const OpOverload *ov = &sym_overloads(tab)[i];
            if (ov->fixity != FIX_FUNC || ov->param_count != rn->sub ||
                !sym_mod_enabled(tab, ov->mod_id) || ov->ret_type == 0) continue;

            if (rn->sub >= 1 && arg0_t != 0) {
                uint16_t p0_t = ov->param_types[0];
                if (p0_t == arg0_t && ov->ret_type != SYM_TYPE_ANY) {
                    return ov->ret_type;
                }
                if (sym_unpack_outer_type(mem, p0_t) == sym_unpack_outer_type(mem, arg0_t)) {
                    if (ov->ret_type == SYM_TYPE_ANY) {
                        uint16_t elem_t = sym_unpack_elem_type(mem, arg0_t);
                        if (elem_t != 0 && elem_t != SYM_TYPE_ANY) return elem_t;
                    } else if (ov->ret_type == mem->vec_type_id &&
                               sym_unpack_outer_type(mem, arg0_t) == mem->vec_type_id &&
                               ov->param_count == 1) {
                        return arg0_t;
                    }
                }
            }
            if (ov->ret_type != SYM_TYPE_ANY && fallback_ret == 0) {
                fallback_ret = ov->ret_type;
            }
        }
        return fallback_ret;
    }
    return 0;
}

uint16_t ast_infer_struct_or_fn_type(const Parser *P, uint16_t idx) {
    return ast_infer_type_rec(P, idx, 0);
}

void parser_try_infer_container_push(Parser *P, uint16_t vec_ast, uint16_t item_ast) {
    if (!P || vec_ast == MEM_NO_IDX || item_ast == MEM_NO_IDX) return;
    const MemArena *mem = P->lex.mem;
    SymTable       *tab = P->lex.tab;
    if (mem->vec_type_id == 0) return;

    uint16_t item_t = sym_unpack_outer_type(mem, ast_infer_struct_or_fn_type(P, item_ast));
    if (item_t == 0 || item_t == SYM_TYPE_ANY) return;

    const AstNode *vn = mem_get_ast(mem, vec_ast);
    if (vn->kind == AST_SLOT_LOAD) {
        uint8_t sidx = (uint8_t)(vn->sub & AST_SLOT_IDX_MASK);
        if (sidx < PARSER_MAX_LOCALS && P->slot_types[sidx] == mem->vec_type_id) {
            P->slot_types[sidx] = sym_pack_container_type(mem, mem->vec_type_id, item_t);
        }
    } else if (vn->kind == AST_CONST_SYM) {
        uint16_t tmod = (vn->right != MEM_NO_IDX && vn->right < tab->mod_count)
                        ? vn->right : P->cur_mod;
        int own_idx = sym_find_own_var(tab, vn->ref, tmod);
        if (own_idx >= 0) {
            ModVarDef *mv = &sym_vars_mut(tab)[own_idx];
            uint8_t l_low = (uint8_t)(mv->lock_type & 0xFFu);
            uint8_t l_hi  = (uint8_t)((mv->lock_type >> 8u) & 0xFFu);
            if (!sym_type_is_parameterized(l_low) &&
                (l_hi == 0 || l_hi == mem->vec_type_id)) {
                uint8_t packed = sym_pack_container_type(mem, mem->vec_type_id, item_t);
                mv->lock_type = (uint16_t)(((uint16_t)packed << 8u) | (uint16_t)l_low);
            }
        }
    }
}

uint16_t build_store_or_compound(Parser *P, uint16_t op_sym_id,
                                        uint16_t lhs_ast, uint16_t rhs_ast) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    if (lhs_ast == MEM_NO_IDX || rhs_ast == MEM_NO_IDX) return MEM_NO_IDX;

    bool is_colon_eq = false;
    if (hardcode_is_plain_store_sym(mem, tab, op_sym_id, &is_colon_eq)) {
        const AstNode *ln = mem_get_ast(mem, lhs_ast);
        uint16_t inferred_t = ast_infer_struct_or_fn_type(P, rhs_ast);
        if (ln->kind == AST_SLOT_LOAD) {
            uint8_t sidx = (uint8_t)(ln->sub & AST_SLOT_IDX_MASK);
            if (inferred_t != 0) {
                if (ln->left == 0 ||
                    (!sym_type_is_parameterized(P->slot_types[sidx]) &&
                     sym_type_is_parameterized(inferred_t) &&
                     sym_unpack_outer_type(mem, P->slot_types[sidx]) == sym_unpack_outer_type(mem, inferred_t))) {
                    P->slot_types[sidx] = inferred_t;
                }
            }
        } else if (ln->kind == AST_CONST_SYM && inferred_t != 0) {
            uint16_t tmod = (ln->right != MEM_NO_IDX && ln->right < tab->mod_count)
                            ? ln->right : P->cur_mod;
            int own_idx = sym_find_own_var(tab, ln->ref, tmod);
            if (own_idx >= 0) {
                ModVarDef *mv = &sym_vars_mut(tab)[own_idx];
                mv->lock_type = (uint16_t)((uint32_t)(mv->lock_type & 0xFFu) | ((uint32_t)(uint16_t)inferred_t << 8u));
            }
        }
        return mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_RIGHT, op_sym_id, lhs_ast, rhs_ast);
    }

    StrSlice op_s   = tab->syms[op_sym_id];
    const char *ptr = mem_str_ptr(mem, op_s);
    if (op_s.len < 2 || ptr[op_s.len - 1] != '=') {
        mem_err(mem, "Invalid compound assignment operator '%.*s'", (int)op_s.len, ptr);
        return MEM_NO_IDX;
    }
    int base_sym  = sym_find(tab, mem, ptr, (uint16_t)(op_s.len - 1));
    int store_sym = find_active_meta_sym(tab, INTR_STORE);
    if (base_sym < 0 || store_sym < 0) {
        mem_err(mem, "Base operator for '%.*s' is not active", (int)op_s.len, ptr);
        return MEM_NO_IDX;
    }
    OpFixity base_fix = (sym_get_active_prec(tab, (uint16_t)base_sym, FIX_INFIX_RIGHT) >= 0)
                        ? FIX_INFIX_RIGHT : FIX_INFIX_LEFT;
    uint16_t comb_rhs = mem_alloc_ast(mem, AST_BINARY, (uint8_t)base_fix,
                                      (uint16_t)base_sym, lhs_ast, rhs_ast);
    return mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_RIGHT,
                         (uint16_t)store_sym, lhs_ast, comb_rhs);
}

static uint8_t ast_cand_cor_meta(const MemArena *mem, const SymTable *tab,
                                 uint16_t idx, uint16_t *out_sym) {
    if (idx == MEM_NO_IDX) return 0;
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind != AST_BINARY) return 0;
    uint8_t m = sym_active_meta_tag(tab, n->ref, (OpFixity)n->sub);
    if (m == INTR_CAND || m == INTR_COR) {
        if (out_sym) *out_sym = n->ref;
        return m;
    }
    return 0;
}

static bool ast_cnot_meta(const MemArena *mem, const SymTable *tab,
                          uint16_t idx, uint16_t *out_sym) {
    if (idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind != AST_UNARY || n->sub != (uint8_t)FIX_PREFIX) return false;
    uint8_t m = sym_active_meta_tag(tab, n->ref, FIX_PREFIX);
    if (m == INTR_CNOT) {
        if (out_sym) *out_sym = n->ref;
        return true;
    }
    return false;
}

uint16_t build_distributed_comp(Parser *P, OpFixity fix, uint16_t comp_sym,
                                       uint16_t left, uint16_t right) {
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    uint16_t comb_sym = 0;

    if (ast_cand_cor_meta(mem, tab, left, &comb_sym) != 0) {
        uint16_t ll = mem_get_ast(mem, left)->left;
        uint16_t lr = mem_get_ast(mem, left)->right;
        uint16_t c1 = build_distributed_comp(P, fix, comp_sym, ll, right);
        uint16_t c2 = build_distributed_comp(P, fix, comp_sym, lr, right);
        return mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT, comb_sym, c1, c2);
    }
    if (ast_cnot_meta(mem, tab, left, &comb_sym)) {
        uint16_t inner = mem_get_ast(mem, left)->left;
        uint16_t c1 = build_distributed_comp(P, fix, comp_sym, inner, right);
        return mem_alloc_ast(mem, AST_UNARY, (uint8_t)FIX_PREFIX, comb_sym, c1, MEM_NO_IDX);
    }
    if (ast_cand_cor_meta(mem, tab, right, &comb_sym) != 0) {
        uint16_t rl = mem_get_ast(mem, right)->left;
        uint16_t rr = mem_get_ast(mem, right)->right;
        uint16_t c1 = build_distributed_comp(P, fix, comp_sym, left, rl);
        uint16_t c2 = build_distributed_comp(P, fix, comp_sym, left, rr);
        return mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT, comb_sym, c1, c2);
    }
    if (ast_cnot_meta(mem, tab, right, &comb_sym)) {
        uint16_t inner = mem_get_ast(mem, right)->left;
        uint16_t c1 = build_distributed_comp(P, fix, comp_sym, left, inner);
        return mem_alloc_ast(mem, AST_UNARY, (uint8_t)FIX_PREFIX, comb_sym, c1, MEM_NO_IDX);
    }
    return mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, comp_sym, left, right);
}

/* ============================================================================
 * 2. STRUCT FIELD RESOLUTION (`.field`) & UFCS METHOD CHAINING (`.fn(...)`)
 * ============================================================================ */

uint16_t parse_fn_call_step(Parser *P, uint16_t fn_ast);
