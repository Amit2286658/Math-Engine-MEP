/*
 * hardcode_parse.c — Hardcoded Grammar Lookahead, Wildcard Resolution & Container Type Args
 */
#include "hardcode_internal.h"

/* ============================================================================
 * 1. RESERVED KEYWORDS & BUILT-IN TYPE/UNIT WILDCARDS (`$T`, `$any`, `$none`, `real`)
 * ============================================================================ */

/*code section below manages hardcoding reserved control-flow keywords that cannot be used as variable or tag names*/
/*Construct if, or, then, else, default, switch, case, for, jump, run, break, continue, return*/
bool hardcode_is_reserved_kw(const SymTable *tab, const MemArena *mem,
                             const char *name, uint16_t len) {
    if ((len == 3 && memcmp(name, "try", 3) == 0) ||
        (len == 5 && (memcmp(name, "catch", 5) == 0 || memcmp(name, "throw", 5) == 0))) {
        return true;
    }
    int sid = sym_find(tab, mem, name, len);
    if (sid < 0) return false;
    uint8_t m = sym_active_meta_tag(tab, (uint16_t)sid, FIX_FUNC);
    return (m == INTR_IF            ||
            m == INTR_OR_BRANCH     ||
            m == INTR_ELSE_BRANCH   ||
            m == INTR_SWITCH_BLOCK  ||
            m == INTR_CASE_BRANCH   ||
            m == INTR_FOR_KW        ||
            m == INTR_JUMP_TAG      ||
            m == INTR_RUN_TAG       ||
            m == INTR_BREAK_LOOP    ||
            m == INTR_CONTINUE_LOOP ||
            m == INTR_RETURN_FN);
}

/*code section below manages hardcoding generic entity/unit wildcards and default real type fallback*/
/*Construct $T, $any, $none, default :real*/
uint16_t hardcode_resolve_wildcard_type(const SymTable *tab, const MemArena *mem,
                                        const char *name, uint16_t len) {
    if (!tab || !mem) return MEM_TYPE_VOID;
    for (uint16_t i = 1; i < tab->type_count; ++i) {
        if (!sym_mod_enabled(tab, tab->types[i].mod_id)) continue;
        if (tab->types[i].parser_intr == INTR_ANY_TYPE) {
            StrSlice s = tab->types[i].name;
            if (s.len == len && memcmp(mem_str_ptr(mem, s), name, len) == 0) {
                return SYM_TYPE_ANY;
            }
        }
    }
    return MEM_TYPE_VOID;
}

uint8_t hardcode_resolve_wildcard_unit(const SymTable *tab, const MemArena *mem,
                                       const char *name, uint16_t len) {
    if (!tab || !mem) return MEM_UNIT_NONE;
    for (uint16_t i = 1; i < tab->type_count; ++i) {
        if (!sym_mod_enabled(tab, tab->types[i].mod_id)) continue;
        StrSlice s = tab->types[i].name;
        if (s.len == len && memcmp(mem_str_ptr(mem, s), name, len) == 0) {
            if (tab->types[i].parser_intr == INTR_ANY_TYPE)  return SYM_UNIT_GENERIC;
            if (tab->types[i].parser_intr == INTR_NONE_UNIT) return SYM_UNIT_LOCK_NONE;
        }
    }
    return MEM_UNIT_NONE;
}

uint16_t hardcode_default_real_type(const MemArena *mem) {
    return mem->fast_real_type ? mem->fast_real_type : 1;
}

/* ============================================================================
 * 2. DEFAULT PARAMETER OVERLOAD WRAPPER SYNTHESIS (`param = default_expr`)
 * ============================================================================ */

/*code section below manages hardcoding automatic overload wrapper synthesis for functions with default parameters*/
/*Construct fn(x, param = default_expr)*/
static int find_active_meta_sym_hc(const SymTable *tab, uint8_t target_meta) {
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->meta_tag == target_meta && sym_mod_enabled(tab, ov->mod_id)) {
            return (int)ov->sym_id;
        }
    }
    return -1;
}

void hardcode_synthesize_default_overloads(MemArena *mem, SymTable *tab,
                                           const char *src, uint16_t sym_id,
                                           uint16_t cur_mod, uint16_t base_ov_idx,
                                           uint8_t pcount, uint8_t req_count,
                                           const StrSlice *pnames,
                                           const uint16_t *ptypes,
                                           const uint8_t  *punits,
                                           const uint16_t *def_start,
                                           const uint16_t *def_end,
                                           uint16_t rtid, uint8_t ruid) {
    int store_sym = find_active_meta_sym_hc(tab, INTR_STORE);
    int seq_sym   = find_active_meta_sym_hc(tab, INTR_SEQ);
    if (store_sym < 0 || seq_sym < 0) {
        mem_err(mem, "Default parameters require active assignment and sequence operators");
        return;
    }

    OpOverload *base_ov  = &tab->overloads[base_ov_idx];
    uint16_t full_body   = base_ov->body_ast;
    uint8_t  full_locals = base_ov->local_count;
    uint16_t def_asts[SYM_MAX_OV_PARAMS];
    memset(def_asts, 0xFF, sizeof(def_asts));

    for (uint8_t k = req_count; k < pcount; ++k) {
        Parser DP;
        parser_init(&DP, mem, tab, src, def_end[k], def_start[k], true, false);
        DP.param_count = pcount;
        for (uint8_t i = 0; i < pcount; ++i) {
            parser_add_local(&DP, mem_str_ptr(mem, pnames[i]), pnames[i].len, ptypes[i]);
        }
        if (DP.local_count < full_locals) {
            DP.local_count = full_locals;
        }
        lex_next(&DP.lex);
        def_asts[k] = parser_parse_expr(&DP, 0);
        if (mem->has_error || def_asts[k] == MEM_NO_IDX) return;
        if (DP.local_count > full_locals) {
            full_locals = DP.local_count;
        }
    }
    base_ov->local_count = full_locals;

    for (uint8_t m = req_count; m < pcount; ++m) {
        uint16_t wrapper_ast = full_body;
        for (int k = (int)pcount - 1; k >= (int)m; --k) {
            uint16_t slot_k  = mem_alloc_ast(mem, AST_SLOT_LOAD, (uint8_t)k, 0, 0, 0);
            uint16_t store_k = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_RIGHT,
                                             (uint16_t)store_sym, slot_k, def_asts[k]);
            wrapper_ast      = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                             (uint16_t)seq_sym, store_k, wrapper_ast);
        }
        if (mem->has_error) return;

        int existing_dov = -1;
        if (sym_id < tab->sym_count) {
            for (uint16_t oi = tab->sym_first_ov[sym_id]; oi != MEM_NO_IDX; oi = tab->ov_next[oi]) {
                const OpOverload *cand = &tab->overloads[oi];
                if (cand->mod_id != cur_mod || cand->fixity != FIX_FUNC || cand->param_count != m) continue;
                bool same = true;
                for (uint8_t pi = 0; pi < m; ++pi) {
                    if (cand->param_types[pi] != (uint8_t)ptypes[pi] || cand->param_units[pi] != punits[pi]) {
                        same = false;
                        break;
                    }
                }
                if (same) { existing_dov = (int)oi; break; }
            }
        }

        uint16_t dov_idx = 0;
        if (existing_dov >= 0) {
            dov_idx = (uint16_t)existing_dov;
        } else {
            if (tab->overload_count >= SYM_MAX_OVERLOADS) {
                mem_err(mem, "Overload table overflow (max %d)", SYM_MAX_OVERLOADS);
                return;
            }
            dov_idx = tab->overload_count++;
        }
        OpOverload *dov  = &tab->overloads[dov_idx];
        memset(dov, 0, sizeof(*dov));
        dov->sym_id      = sym_id;
        dov->mod_id      = cur_mod;
        dov->fixity      = FIX_FUNC;
        dov->prec        = 0;
        dov->param_count = m;
        dov->local_count = full_locals;
        dov->ret_type    = (uint8_t)rtid;
        dov->ret_unit    = ruid;
        dov->meta_tag    = base_ov->meta_tag;
        dov->reserved    = base_ov->reserved;
        for (uint8_t i = 0; i < m; ++i) {
            dov->param_types[i] = (uint8_t)ptypes[i];
            dov->param_units[i] = punits[i];
        }
        dov->body_ast = wrapper_ast;
        if (existing_dov < 0) {
            sym_link_overload(tab, dov_idx);
        }
    }
}

/* ============================================================================
 * 3. PARSER HARDCODED ERGONOMICS (NEWLINE `;`, CONTEXT-AWARE `in`, LAMBDAS)
 * ============================================================================ */

static uint16_t skip_raw_ws_hc(const char *src, uint16_t p, uint16_t len) {
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

static uint8_t token_func_meta_hc(const Parser *P, Token t) {
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

/*code section below manages hardcoding newline as a default ';' statement separator when a new statement begins*/
/*Construct \n (newline statement separator)*/
bool hardcode_can_start_newline_stmt(const Parser *P) {
    SymTable *tab = P->lex.tab;
    Token t = P->lex.cur;

    if (!t.had_newline) return false;
    if (t.kind == TOK_EOF    ||
        t.kind == TOK_RPAREN ||
        t.kind == TOK_RBRACE ||
        t.kind == TOK_RBRACK ||
        t.kind == TOK_COMMA) {
        return false;
    }

    uint8_t kw = token_func_meta_hc(P, t);
    if (kw == INTR_OR_BRANCH || kw == INTR_ELSE_BRANCH || kw == INTR_CASE_BRANCH) {
        return false;
    }
    if (t.len == 5 && memcmp(P->lex.src + t.off, "catch", 5) == 0) {
        return false;
    }

    if (t.kind == TOK_VAL    ||
        t.kind == TOK_IDENT  ||
        t.kind == TOK_LPAREN ||
        t.kind == TOK_LBRACE ||
        t.kind == TOK_LBRACK ||
        t.kind == TOK_AT) {
        return true;
    }

    if (t.kind == TOK_OP) {
        if (sym_active_meta_tag(tab, t.sym_id, FIX_INFIX_LEFT) == INTR_SEQ) {
            return true;
        }
        if (sym_get_active_prec(tab, t.sym_id, FIX_PREFIX) >= 0) {
            return true;
        }
        if (lex_is_ident_start((unsigned char)P->lex.src[t.off])) {
            return true;
        }
    }
    return false;
}

/*code section below manages hardcoding the check for active postfix physical unit operators*/
/*Construct <expr> <unit> (e.g. 25km, 50m, 90deg)*/
bool hardcode_sym_is_postfix_unit(const SymTable *tab, uint16_t sym_id) {
    if (!tab || sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == FIX_POSTFIX && sym_mod_enabled(tab, ov->mod_id) &&
            ov->ret_unit != 0 && ov->ret_unit < SYM_UNIT_LOCK_NONE) {
            return true;
        }
    }
    return false;
}

/*code section below manages hardcoding the detection of the 'for' and 'in' operator/keyword symbols*/
/*Construct [expr for x in iter] and <lhs> in <rhs>*/
bool hardcode_is_for_kw(const MemArena *mem, const SymTable *tab,
                        const char *name, uint16_t len) {
    if (!mem || !tab || !name || len == 0) return false;
    int sid = sym_find(tab, mem, name, len);
    if (sid < 0) return false;
    return sym_active_meta_tag(tab, (uint16_t)sid, FIX_FUNC) == INTR_FOR_KW;
}

bool hardcode_is_in_op_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
    if (!mem || !tab || sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if ((ov->fixity == FIX_INFIX_LEFT || ov->fixity == FIX_INFIX_RIGHT) &&
            sym_mod_enabled(tab, ov->mod_id) && ov->body_ast != MEM_NO_IDX) {
            const AstNode *b = mem_get_ast(mem, ov->body_ast);
            if (b && b->kind == AST_INTRINSIC && b->sub == INTR_MAP_HAS) {
                return true;
            }
        }
    }
    return false;
}

bool hardcode_is_into_op_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
    if (!mem || !tab || sym_id >= tab->sym_count) return false;
    StrSlice nm = tab->syms[sym_id];
    return (nm.len == 4 && memcmp(mem_str_ptr(mem, nm), "into", 4) == 0);
}

bool hardcode_is_in_kw(const MemArena *mem, const SymTable *tab,
                       const char *name, uint16_t len) {
    if (!mem || !tab || !name || len == 0) return false;
    int sid = sym_find(tab, mem, name, len);
    if (sid < 0) return false;
    return hardcode_is_in_op_sym(mem, tab, (uint16_t)sid);
}

/*code section below manages hardcoding the detection of bare unit conversion RHS on 'in' ("25km in m")*/
/*Construct <expr> in <unit>*/
bool hardcode_ast_is_bare_unit_rhs(const MemArena *mem, uint16_t ast_idx) {
    if (!mem || ast_idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, ast_idx);
    return (n && n->kind == AST_UNARY && n->sub == (uint8_t)FIX_POSTFIX && n->right == 1);
}

/*code section below manages hardcoding undefined iterator variable validation on the LHS of 'in' when not followed by a loop body*/
/*Construct (i in 1...5) { ... } vs i in 1...5 { ... }*/
bool hardcode_check_undef_in_lhs(MemArena *mem, const SymTable *tab, uint16_t in_ast_idx) {
    if (!mem || !tab || in_ast_idx == MEM_NO_IDX) return true;
    const AstNode *in_node = mem_get_ast(mem, in_ast_idx);
    if (!in_node || in_node->kind != AST_BINARY ||
        !hardcode_is_in_op_sym(mem, tab, in_node->ref)) {
        return true;
    }
    const AstNode *lhs = mem_get_ast(mem, in_node->left);
    if (lhs && lhs->kind == AST_SLOT_LOAD && (lhs->ref & AST_SLOT_UNDEF_ITER_BIT) != 0) {
        uint16_t var_sym = (uint16_t)(lhs->ref & AST_SLOT_SYM_MASK);
        if (var_sym < tab->sym_count) {
            StrSlice vs = tab->syms[var_sym];
            mem_err(mem, "Undefined symbol or constant '%.*s'",
                    (int)vs.len, mem_str_ptr(mem, vs));
        } else {
            mem_err(mem, "Undefined variable on left side of 'in'");
        }
        return false;
    }
    return true;
}

/*code section below manages hardcoding the differentiating "25km in m" when "m" is also declared a variable or vector*/
/*Construct <expr> in <var_or_unit>*/
bool hardcode_should_use_unit_for_in_rhs(const MemArena *mem, const SymTable *tab,
                                         const AstNode *in_node, Value lhs_val,
                                         Value rhs_val, uint16_t *out_unit_sym) {
    if (out_unit_sym) *out_unit_sym = MEM_NO_IDX;
    if (!mem || !tab || !in_node || !hardcode_is_into_op_sym(mem, tab, in_node->ref)) {
        return false;
    }
    const AstNode *rn = mem_get_ast(mem, in_node->right);
    if (!rn) return false;
    uint16_t unit_sym = MEM_NO_IDX;
    if (rn->kind == AST_SLOT_LOAD && (rn->ref & AST_SLOT_DUAL_UNIT_BIT) != 0) {
        unit_sym = (uint16_t)(rn->ref & AST_SLOT_SYM_MASK);
    } else if (rn->kind == AST_CONST_SYM && (rn->left & AST_SLOT_DUAL_UNIT_BIT) != 0) {
        unit_sym = (uint16_t)(rn->left & AST_SLOT_SYM_MASK);
    } else {
        return false;
    }
    if (unit_sym >= tab->sym_count) return false;

    uint8_t unit_uid = MEM_UNIT_NONE;
    for (uint16_t i = tab->sym_first_ov[unit_sym]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == FIX_POSTFIX && sym_mod_enabled(tab, ov->mod_id) &&
            ov->ret_unit != 0 && ov->ret_unit < SYM_UNIT_LOCK_NONE) {
            unit_uid = ov->ret_unit;
            break;
        }
    }
    if (unit_uid == MEM_UNIT_NONE) return false;

    uint8_t lhs_uid = val_get_unit(mem, lhs_val);
    if (lhs_uid == MEM_UNIT_NONE || lhs_uid != unit_uid) {
        return false;
    }

    uint16_t r_tid = val_get_type(mem, rhs_val);
    if (mem->vec_type_id != 0 && r_tid == mem->vec_type_id) {
        uint16_t hdr_idx  = val_get_pool_idx(rhs_val);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
        uint16_t len      = (uint16_t)mem_read_pool(mem, hdr_idx + 1);
        if (len > 0 && elem_uid == lhs_uid) {
            /* `km` is a vector whose elements have the exact same unit as `lhs` -> membership test */
            return false;
        }
        if (out_unit_sym) *out_unit_sym = unit_sym;
        return true;
    }
    if (mem->mat_type_id != 0 && r_tid == mem->mat_type_id) {
        if (val_get_unit(mem, rhs_val) == lhs_uid) {
            return false;
        }
        if (out_unit_sym) *out_unit_sym = unit_sym;
        return true;
    }

    /* Variable holds a scalar (cannot be a collection for membership), so convert to `1 <unit>` */
    if (out_unit_sym) *out_unit_sym = unit_sym;
    return true;
}

/*code section below manages hardcoding lookahead for parenthesized lambda parameter lists*/
/*Construct (x, y, ...) => expr, (x, y = 10) => expr, () => expr, f(x, y = 10) := expr, f(x, y) = expr*/
static uint16_t hc_skip_default_param_expr(const char *src, uint16_t p, uint16_t len) {
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
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            p++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) break;
            depth--;
            p++;
            continue;
        }
        if (depth == 0 && (c == ',' || c == ';' || c == '\n')) {
            break;
        }
        p++;
    }
    return p;
}

static bool hc_scan_param_list_parens(const char *src, uint16_t p, uint16_t len, uint16_t *out_after_rparen) {
    p = skip_raw_ws_hc(src, p, len);
    if (p >= len || src[p] != '(') return false;
    p = skip_raw_ws_hc(src, (uint16_t)(p + 1), len);
    if (p < len && src[p] == ')') {
        if (out_after_rparen) *out_after_rparen = skip_raw_ws_hc(src, (uint16_t)(p + 1), len);
        return true;
    }
    for (uint8_t k = 0; k < MEM_FN_MAX_PARAMS; ++k) {
        if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;
        while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
        p = skip_raw_ws_hc(src, p, len);
        if (p < len && src[p] == ':' && !(p + 1 < len && src[p + 1] == '=')) {
            p = skip_raw_ws_hc(src, (uint16_t)(p + 1), len);
            if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;
            while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
            p = skip_raw_ws_hc(src, p, len);
        }
        if (p < len && src[p] == '=' &&
            !(p + 1 < len && (src[p + 1] == '=' || src[p + 1] == '>'))) {
            p = skip_raw_ws_hc(src, (uint16_t)(p + 1), len);
            p = hc_skip_default_param_expr(src, p, len);
            p = skip_raw_ws_hc(src, p, len);
        }
        if (p < len && src[p] == ',') {
            p = skip_raw_ws_hc(src, (uint16_t)(p + 1), len);
            continue;
        }
        break;
    }
    if (p >= len || src[p] != ')') return false;
    if (out_after_rparen) *out_after_rparen = skip_raw_ws_hc(src, (uint16_t)(p + 1), len);
    return true;
}

bool hardcode_is_paren_lambda(const Parser *P) {
    if (P->lex.cur.kind != TOK_LPAREN) return false;
    uint16_t after = 0;
    if (!hc_scan_param_list_parens(P->lex.src, P->lex.cur.off, P->lex.len, &after)) {
        return false;
    }
    return (after + 1 < P->lex.len && P->lex.src[after] == '=' && P->lex.src[after + 1] == '>');
}

static bool hc_scan_param_paren_to_colon_eq(const char *src, uint16_t p, uint16_t len) {
    uint16_t after = 0;
    if (!hc_scan_param_list_parens(src, p, len, &after)) return false;
    if (after + 1 < len && src[after] == ':' && src[after + 1] == '=') return true;
    if (after < len && src[after] == '=' &&
        !(after + 1 < len && (src[after + 1] == '=' || src[after + 1] == '>'))) {
        return true;
    }
    return false;
}

bool hardcode_is_inline_fn_def(const Parser *P) {
    if (P->lex.cur.kind != TOK_LPAREN || P->lex.cur.had_newline) return false;
    if (P->lex.mem->fn_type_id == 0) return false;
    int ce_sid = sym_find(P->lex.tab, P->lex.mem, ":=", 2);
    if (ce_sid < 0 || sym_active_meta_tag(P->lex.tab, (uint16_t)ce_sid, FIX_INFIX_RIGHT) != INTR_STORE) {
        return false;
    }
    return hc_scan_param_paren_to_colon_eq(P->lex.src, P->lex.cur.off, P->lex.len);
}

bool hardcode_rhs_starts_lambda(const Parser *P) {
    const char *src = P->lex.src;
    uint16_t    len = P->lex.len;
    uint16_t    p   = skip_raw_ws_hc(src, P->lex.pos, len);
    if (p >= len) return false;
    if (src[p] == '(') {
        uint16_t after = 0;
        if (!hc_scan_param_list_parens(src, p, len, &after)) return false;
        return (after + 1 < len && src[after] == '=' && src[after + 1] == '>');
    }
    if (lex_is_ident_start((unsigned char)src[p])) {
        while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
        p = skip_raw_ws_hc(src, p, len);
        return (p + 1 < len && src[p] == '=' && src[p + 1] == '>');
    }
    return false;
}

/*code section below manages hardcoding lookahead for vector comprehensions inside '[ ... ]'*/
/*Construct [expr for var in iterable (if cond)], [expr for idx, var in iterable (if cond)]*/
static bool hc_match_in_kw_at(const MemArena *mem, const SymTable *tab,
                              const char *src, uint16_t p, uint16_t len) {
    if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;
    uint16_t istart = p;
    while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
    return hardcode_is_in_kw(mem, tab, src + istart, (uint16_t)(p - istart));
}

bool hardcode_scan_comprehension_var(const Parser *P,
                                     const char **out_idx_name, uint16_t *out_idx_len,
                                     const char **out_vname, uint16_t *out_vlen) {
    if (out_idx_name) *out_idx_name = NULL;
    if (out_idx_len)  *out_idx_len  = 0;
    const MemArena *mem = P->lex.mem;
    const SymTable *tab = P->lex.tab;
    const char     *src = P->lex.src;
    uint16_t        len = P->lex.len;
    uint16_t        p   = P->lex.cur.off;
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
        if (c == '#') {
            while (p < len && src[p] != '\n') p++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            p++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) break;
            depth--;
            p++;
            continue;
        }
        if (depth == 0 && (c == ',' || c == ';')) {
            return false;
        }
        if (depth == 0 && lex_is_ident_start((unsigned char)c)) {
            uint16_t id_start = p;
            while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
            uint16_t id_len = (uint16_t)(p - id_start);
            if (hardcode_is_for_kw(mem, tab, src + id_start, id_len)) {
                uint16_t q = skip_raw_ws_hc(src, p, len);
                if (q < len && lex_is_ident_start((unsigned char)src[q])) {
                    uint16_t vstart = q;
                    while (q < len && lex_is_ident_cont((unsigned char)src[q])) q++;
                    uint16_t vlen = (uint16_t)(q - vstart);
                    q = skip_raw_ws_hc(src, q, len);
                    if (hc_match_in_kw_at(mem, tab, src, q, len)) {
                        if (out_vname) *out_vname = src + vstart;
                        if (out_vlen)  *out_vlen  = vlen;
                        return true;
                    }
                    if (q < len && src[q] == ',') {
                        uint16_t r = skip_raw_ws_hc(src, (uint16_t)(q + 1), len);
                        if (r < len && lex_is_ident_start((unsigned char)src[r])) {
                            uint16_t v2start = r;
                            while (r < len && lex_is_ident_cont((unsigned char)src[r])) r++;
                            uint16_t v2len = (uint16_t)(r - v2start);
                            r = skip_raw_ws_hc(src, r, len);
                            if (hc_match_in_kw_at(mem, tab, src, r, len)) {
                                if (out_idx_name) *out_idx_name = src + vstart;
                                if (out_idx_len)  *out_idx_len  = vlen;
                                if (out_vname)    *out_vname    = src + v2start;
                                if (out_vlen)     *out_vlen     = v2len;
                                return true;
                            }
                        }
                    }
                }
            }
            continue;
        }
        p++;
    }
    return false;
}

/*code section below manages hardcoding same-line implicit multiplication rules*/
/*Construct 2x, 2π, 3(x + 1), (a + b)(c + d), 2 sin(x)*/
bool hardcode_can_start_implicit_mul_rhs(const Parser *P) {
    SymTable *tab = P->lex.tab;
    Token t = P->lex.cur;

    /* Never implicitly multiply across a line break */
    if (t.had_newline) return false;

    if (t.kind == TOK_LPAREN) {
        return !hardcode_is_paren_lambda(P);
    }
    if (t.kind == TOK_IDENT) {
        uint8_t kw = token_func_meta_hc(P, t);
        if (kw != 0 && (kw & SYM_META_LAZY_MASK) == 0 && !delegate_is_sym_meta(kw)) {
            return false;
        }
        /* Do not treat missing semicolon before `y := ...`, `f(x) := ...`, `Tag : ...`, `i in ...`, or `x => ...` as implicit mul */
        uint16_t p = skip_raw_ws_hc(P->lex.src, P->lex.pos, P->lex.len);
        if (p < P->lex.len && (P->lex.src[p] == ':' || P->lex.src[p] == '=')) {
            return false;
        }
        if (p < P->lex.len && P->lex.src[p] == '(' &&
            hc_scan_param_paren_to_colon_eq(P->lex.src, p, P->lex.len)) {
            return false;
        }
        if (p + 1 < P->lex.len && P->lex.src[p + 1] == '=' &&
            (P->lex.src[p] == '+' || P->lex.src[p] == '-' || P->lex.src[p] == '*' ||
             P->lex.src[p] == '/' || P->lex.src[p] == '%' || P->lex.src[p] == '^')) {
            return false;
        }
        if (hc_match_in_kw_at(P->lex.mem, tab, P->lex.src, p, P->lex.len)) {
            return false;
        }
        return true;
    }
    if (t.kind == TOK_OP) {
        /* Prefix-only non-word operator like `√` in `2 √16` */
        if (sym_get_active_prec(tab, t.sym_id, FIX_POSTFIX) < 0 &&
            sym_get_active_prec(tab, t.sym_id, FIX_INFIX_LEFT) < 0 &&
            sym_get_active_prec(tab, t.sym_id, FIX_INFIX_RIGHT) < 0 &&
            sym_get_active_prec(tab, t.sym_id, FIX_PREFIX) >= 0 &&
            !((P->lex.src[t.off] >= 'a' && P->lex.src[t.off] <= 'z') ||
              (P->lex.src[t.off] >= 'A' && P->lex.src[t.off] <= 'Z') ||
              P->lex.src[t.off] == '_')) {
            return true;
        }
    }
    return false;
}

/*code section below manages hardcoding plain assignment (':=', '=') vs compound assignment ('+=', '-=', etc.) detection*/
/*Construct :=, =, +=, -=, *=, /=, %=, ^=*/
bool hardcode_is_plain_store_sym(const MemArena *mem, const SymTable *tab,
                                 uint16_t sym_id, bool *out_is_colon_eq) {
    if (out_is_colon_eq) *out_is_colon_eq = false;
    if (!mem || !tab || sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != FIX_INFIX_RIGHT || !sym_mod_enabled(tab, ov->mod_id) ||
            ov->body_ast == MEM_NO_IDX) continue;
        const AstNode *root = mem_get_ast(mem, ov->body_ast);
        if (root && root->kind == AST_INTRINSIC && root->sub == INTR_STORE &&
            root->right != MEM_NO_IDX && mem_get_ast(mem, root->right)->kind == AST_SLOT_LOAD) {
            if (out_is_colon_eq) *out_is_colon_eq = (root->ref != MEM_NO_IDX);
            return true;
        }
    }
    return false;
}

/*code section below manages hardcoding relational comparison operator detection for distributed '&' and '|' comparisons*/
/*Construct (x & y == 1 | 2), case > 10:*/
static bool sym_has_direct_intr_hc(const MemArena *mem, const SymTable *tab,
                                   uint16_t sym_id, bool want_rel) {
    if (sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->param_count != 2 || !sym_mod_enabled(tab, ov->mod_id) ||
            ov->body_ast == MEM_NO_IDX) continue;
        const AstNode *root = mem_get_ast(mem, ov->body_ast);
        if (root->kind == AST_INTRINSIC) {
            if (want_rel && delegate_is_rel_intrinsic((IntrinsicId)root->sub)) return true;
            if (!want_rel && ov->fixity == FIX_INFIX_LEFT &&
                (root->sub == INTR_ADD || root->sub == INTR_SUB)) return true;
        }
    }
    return false;
}

bool hardcode_is_rel_comparison_op(const MemArena *mem, const SymTable *tab,
                                   uint16_t sym_id) {
    bool is_rel = false;
    (void)hardcode_is_additive_or_rel_sym(mem, tab, sym_id, FIX_INFIX_LEFT, &is_rel);
    return is_rel;
}

/*code section below manages hardcoding additive and relational unit compatibility checks*/
/*Construct +, -, plus, minus, ==, !=, <, <=, >, >=, ~*/
bool hardcode_is_additive_or_rel_sym(const MemArena *mem, const SymTable *tab,
                                     uint16_t sym_id, OpFixity fixity, bool *out_is_rel) {
    if (out_is_rel) *out_is_rel = false;
    if (fixity != FIX_INFIX_LEFT && fixity != FIX_INFIX_RIGHT) return false;
    if (sym_id >= tab->sym_count) return false;

    if (sym_has_direct_intr_hc(mem, tab, sym_id, false)) {
        return (fixity == FIX_INFIX_LEFT);
    }
    if (sym_has_direct_intr_hc(mem, tab, sym_id, true)) {
        if (out_is_rel) *out_is_rel = true;
        return true;
    }

    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != (uint8_t)fixity || ov->param_count != 2 ||
            !sym_mod_enabled(tab, ov->mod_id) || ov->body_ast == MEM_NO_IDX) continue;
        const AstNode *root = mem_get_ast(mem, ov->body_ast);
        if (root->kind == AST_BINARY && root->ref != sym_id) {
            const AstNode *ln = mem_get_ast(mem, root->left);
            const AstNode *rn = mem_get_ast(mem, root->right);
            if (ln->kind == AST_SLOT_LOAD && rn->kind == AST_SLOT_LOAD &&
                sym_has_direct_intr_hc(mem, tab, root->ref, false)) {
                return true;
            }
            if (ln->kind == AST_INTRINSIC && ln->sub == INTR_ABS &&
                sym_has_direct_intr_hc(mem, tab, root->ref, true)) {
                if (out_is_rel) *out_is_rel = true;
                return true;
            }
        }
    }
    return false;
}

/* ============================================================================
 * 4. EVALUATOR HARDCODED ERGONOMICS (GENERIC `vec<T>`/`list<T>`/`stack<T>`/`queue<T>`,
 *    `AST_FOR_IN`, DESTRUCTURING, `switch`)
 * ============================================================================ */

/*code section below manages hardcoding parameterized container type syntax and variadic constructors*/
/*Construct vec<T>(...), list<T>(...), stack<T>(...), queue<T>(...), vec(...), list(...), stack(...), queue(...)*/
int container_kind_from_sym_hc(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return -1;
    bool has_vnew    = false;
    bool has_vfill2  = false;
    bool has_map_new = false;
    bool has_hof_map = false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != FIX_FUNC || !sym_mod_enabled(tab, ov->mod_id) ||
            ov->body_ast == MEM_NO_IDX) continue;
        const AstNode *root = mem_get_ast(mem, ov->body_ast);
        if (root->kind == AST_INTRINSIC) {
            if (root->sub == INTR_VNEW) has_vnew = true;
            else if (root->sub == INTR_VFILL && ov->param_count == 2) has_vfill2 = true;
            else if (root->sub == INTR_MAP_NEW) has_map_new = true;
        }
        if (ov->param_count == 2 && mem->fn_type_id != 0 &&
            ov->param_types[1] == mem->fn_type_id) {
            has_hof_map = true;
        }
    }
    if (has_map_new) return has_hof_map ? 4 : 5;
    if (has_vnew)    return has_vfill2 ? 0 : 1;
    return -1;
}

static int container_kind_from_name_hc(const MemArena *mem, const SymTable *tab,
                                       const char *name, uint16_t len) {
    int sid = sym_find(tab, mem, name, len);
    if (sid < 0) return -1;
    return container_kind_from_sym_hc(mem, tab, (uint16_t)sid);
}

bool hardcode_is_vec_ctor_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
    int ck = container_kind_from_sym_hc(mem, tab, sym_id);
    return (ck == 0 || ck == 1);
}

bool hardcode_is_map_ctor_sym(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
    int ck = container_kind_from_sym_hc(mem, tab, sym_id);
    return (ck == 4 || ck == 5);
}

static uint16_t resolve_container_elem_type_hc(const SymTable *tab, const MemArena *mem,
                                               const char *name, uint16_t len) {
    int ck = container_kind_from_name_hc(mem, tab, name, len);
    if ((ck == 0 || ck == 1) && mem->vec_type_id != 0) {
        return mem->vec_type_id;
    }
    if ((ck == 4 || ck == 5) && mem->map_type_id != 0) {
        return mem->map_type_id;
    }
    return sym_find_type(tab, mem, name, len);
}

bool hardcode_try_parse_container_type_arg(Parser *P, const char *id_str, uint16_t id_len,
                                           uint16_t *out_spec) {
    if (out_spec) *out_spec = 0;
    int kind_code = container_kind_from_name_hc(P->lex.mem, P->lex.tab, id_str, id_len);
    if (kind_code < 0) return false;

    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;

    if (P->lex.cur.had_newline || P->lex.cur.kind != TOK_OP) return false;
    StrSlice op_s = tab->syms[P->lex.cur.sym_id];
    if (op_s.len != 1 || mem_str_ptr(mem, op_s)[0] != '<') return false;

    lex_next(&P->lex); /* consume '<' */
    if (P->lex.cur.kind != TOK_IDENT &&
        !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
        mem_err(mem, "Expected type or unit name inside '%.*s<...>'", (int)id_len, id_str);
        return false;
    }
    const char *s1_ptr = P->lex.src + P->lex.cur.off;
    uint16_t    s1_len = P->lex.cur.len;
    lex_next(&P->lex);

    uint8_t locked_tid = 0;
    uint8_t locked_uid = 0;

    if (P->lex.cur.kind == TOK_COMMA) {
        lex_next(&P->lex); /* consume ',' */
        if (P->lex.cur.kind != TOK_IDENT &&
            !(P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) {
            mem_err(mem, "Expected unit name after ',' in '%.*s<...>'", (int)id_len, id_str);
            return false;
        }
        const char *s2_ptr = P->lex.src + P->lex.cur.off;
        uint16_t    s2_len = P->lex.cur.len;
        lex_next(&P->lex);

        uint16_t tid = resolve_container_elem_type_hc(tab, mem, s1_ptr, s1_len);
        if (tid == MEM_TYPE_VOID) {
            mem_err(mem, "Unknown element type '%.*s' in '%.*s<...>'",
                    (int)s1_len, s1_ptr, (int)id_len, id_str);
            return false;
        }
        uint8_t uid = sym_find_or_add_unit(tab, mem, s2_ptr, s2_len);
        if (mem->has_error) return false;
        locked_tid = (tid == SYM_TYPE_ANY) ? 0 : (uint8_t)tid;
        locked_uid = (uid == SYM_UNIT_GENERIC || uid == SYM_UNIT_OMITTED || uid == SYM_UNIT_LOCK_NONE) ? 0 : uid;
    } else {
        uint16_t tid = resolve_container_elem_type_hc(tab, mem, s1_ptr, s1_len);
        if (tid == SYM_TYPE_ANY) {
            locked_tid = 0;
            locked_uid = 0;
        } else if (tid != MEM_TYPE_VOID) {
            locked_tid = (uint8_t)tid;
            locked_uid = 0;
        } else {
            uint8_t uid = sym_find_unit(tab, mem, s1_ptr, s1_len);
            if (uid == MEM_UNIT_NONE) {
                mem_err(mem, "Unknown element type '%.*s' in '%.*s<...>'",
                        (int)s1_len, s1_ptr, (int)id_len, id_str);
                return false;
            }
            locked_tid = 0;
            locked_uid = (uid == SYM_UNIT_GENERIC || uid == SYM_UNIT_OMITTED || uid == SYM_UNIT_LOCK_NONE) ? 0 : uid;
        }
    }

    if (P->lex.cur.kind != TOK_OP ||
        tab->syms[P->lex.cur.sym_id].len != 1 ||
        mem_str_ptr(mem, tab->syms[P->lex.cur.sym_id])[0] != '>') {
        mem_err(mem, "Expected closing '>' after element type in '%.*s<...>'", (int)id_len, id_str);
        return false;
    }
    lex_next(&P->lex); /* consume '>' */

    if (P->lex.cur.kind != TOK_LPAREN) {
        mem_err(mem, "Expected '(' after '%.*s<...>'", (int)id_len, id_str);
        return false;
    }

    if (out_spec) {
        *out_spec = (uint16_t)(0x8000u |
                               (((uint32_t)kind_code & 0x07u) << 12u) |
                               (((uint32_t)locked_uid & 0x3Fu) << 6u) |
                               ((uint32_t)locked_tid & 0x3Fu));
    }
    return true;
}

