/*
 * hardcode.c — Hardcoded Language Grammar, Ergonomic Rules & Built-In Conventions
 *
 * Location: Core/lib/hardcode/hardcode.c
 */

#include "hardcode.h"
#include "../delegate/delegate.h"
#include <math.h>
#include <string.h>

/* ============================================================================
 * 1. RESERVED KEYWORDS & BUILT-IN TYPE/UNIT WILDCARDS (`$T`, `$any`, `$none`, `real`)
 * ============================================================================ */

/*code section below manages hardcoding reserved control-flow keywords that cannot be used as variable or tag names*/
/*Construct if, or, then, else, default, switch, case, for, jump, run, break, continue, return*/
bool hardcode_is_reserved_kw(const SymTable *tab, const MemArena *mem,
                             const char *name, uint16_t len) {
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
    if (!mem || !tab || !in_node || !hardcode_is_in_op_sym(mem, tab, in_node->ref)) {
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
static int container_kind_from_sym_hc(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
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

bool hardcode_try_eval_variadic_vec(MemArena *mem, SymTable *tab,
                                    const AstNode *n, uint16_t frame_base,
                                    uint8_t cur_ret_type, Value *out_val) {
    if (n->ref >= tab->sym_count || mem->vec_type_id == 0) return false;
    int kind_code = container_kind_from_sym_hc(mem, tab, n->ref);
    if (kind_code < 0) return false;

    bool is_vec_call = (kind_code == 0);
    bool is_map_call = (kind_code == 4 || kind_code == 5);
    bool has_spec    = (n->right != 0 && n->right != MEM_NO_IDX && (n->right & 0x8000u) != 0);

    if (is_map_call) {
        if (mem->map_type_id == 0) return false;
        /* Distinguish 2-arg HOF `map(coll, fn)` from `map(k1, v1, ...)` */
        if (kind_code == 4 && !has_spec && n->sub == 2) {
            const AstNode *l1 = mem_get_ast(mem, n->left);
            Value a0 = eval_ast(mem, tab, l1->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) {
                *out_val = val_void();
                return true;
            }
            uint16_t t0 = val_get_type(mem, a0);
            if (t0 == mem->vec_type_id || t0 == mem->mat_type_id) {
                const AstNode *l2 = mem_get_ast(mem, l1->right);
                Value a1 = eval_ast(mem, tab, l2->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) {
                    *out_val = val_void();
                    return true;
                }
                Value map_args[2] = { a0, a1 };
                *out_val = eval_dispatch_overload(mem, tab, n->ref, FIX_FUNC, 2, map_args);
                return true;
            }
            if ((n->sub & 1u) != 0) {
                mem_err(mem, "map() constructor requires key-value pairs (even argument count)");
                *out_val = val_void();
                return true;
            }
            const AstNode *l2 = mem_get_ast(mem, l1->right);
            Value a1 = eval_ast(mem, tab, l2->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) {
                *out_val = val_void();
                return true;
            }
            uint16_t m_hdr = mem_map_alloc_hdr(mem, 0, 2);
            if (mem->has_error) { *out_val = val_void(); return true; }
            Value mval = val_make_comp(mem, mem->map_type_id, m_hdr, MEM_MAP_HDR_SLOTS);
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_SET, mval, a0, a1);
            *out_val = mem->has_error ? val_void() : mval;
            return true;
        }
        if ((n->sub & 1u) != 0) {
            mem_err(mem, "map()/dict() constructor requires an even number of key-value arguments");
            *out_val = val_void();
            return true;
        }
        uint8_t  locked_tid = has_spec ? (uint8_t)(n->right & 0x3Fu) : 0;
        uint8_t  locked_uid = has_spec ? (uint8_t)((n->right >> 6u) & 0x3Fu) : 0;
        uint16_t hdr0       = (uint16_t)(((uint32_t)locked_uid << 8u) | (uint32_t)locked_tid);
        uint16_t m_hdr      = mem_map_alloc_hdr(mem, hdr0, (uint16_t)(n->sub / 2u));
        if (mem->has_error) { *out_val = val_void(); return true; }
        Value mval = val_make_comp(mem, mem->map_type_id, m_hdr, MEM_MAP_HDR_SLOTS);
        uint16_t cur_link = n->left;
        while (cur_link != MEM_NO_IDX && !mem->has_error) {
            const AstNode *lk = mem_get_ast(mem, cur_link);
            const AstNode *lv = mem_get_ast(mem, lk->right);
            Value kval = eval_ast(mem, tab, lk->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) { *out_val = val_void(); return true; }
            Value vval = eval_ast(mem, tab, lv->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) { *out_val = val_void(); return true; }
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_SET, mval, kval, vval);
            cur_link = lv->right;
        }
        *out_val = mem->has_error ? val_void() : mval;
        return true;
    }

    bool has_active_ctor = false;
    for (uint16_t i = tab->sym_first_ov[n->ref]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        if (tab->overloads[i].fixity == FIX_FUNC && sym_mod_enabled(tab, tab->overloads[i].mod_id)) {
            has_active_ctor = true;
            break;
        }
    }
    if (!has_active_ctor) return false;

    if (!has_spec) {
        if (!is_vec_call && n->sub < 2) return false;
        if (is_vec_call && n->sub == 0) return false;
    }

    uint8_t  locked_tid = has_spec ? (uint8_t)(n->right & 0x3Fu) : 0;
    uint8_t  locked_uid = has_spec ? (uint8_t)((n->right >> 6u) & 0x3Fu) : 0;
    uint16_t hdr0       = (uint16_t)(((uint32_t)locked_uid << 8u) | (uint32_t)locked_tid);

    uint16_t total_args = 0;
    for (uint16_t c = n->left; c != MEM_NO_IDX; c = mem_get_ast(mem, c)->right) {
        total_args++;
        if (total_args > MEM_MAX_F64_POOL) {
            mem_err(mem, "Container constructor argument count exceeds pool capacity");
            *out_val = val_void();
            return true;
        }
    }

    /* Special `vec<T>()` / `vec(cap)` / `vec(count, init)` variations when total_args <= 2 and first arg is unitless real */
    if (is_vec_call && total_args <= 2) {
        if (total_args == 0) {
            uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, 0);
            *out_val = mem->has_error ? val_void()
                                      : val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
            return true;
        }
        const AstNode *l0 = mem_get_ast(mem, n->left);
        Value a0 = eval_ast(mem, tab, l0->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            *out_val = val_void();
            return true;
        }
        if (total_args == 1 && val_get_type(mem, a0) == mem->fast_real_type &&
            val_get_unit(mem, a0) == MEM_UNIT_NONE) {
            int cap_in = 0;
            if (!delegate_val_to_exact_int(mem, tab, a0, "vector capacity", &cap_in)) {
                *out_val = val_void();
                return true;
            }
            if (cap_in < 0 || (uint32_t)cap_in > MEM_MAX_HEAP_SLOTS) {
                mem_err(mem, "Vector capacity out of range (%d)", cap_in);
                *out_val = val_void();
                return true;
            }
            uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, (uint32_t)cap_in);
            *out_val = mem->has_error ? val_void()
                                      : val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
            return true;
        }
        Value a1 = val_void();
        if (total_args == 2) {
            const AstNode *l1 = mem_get_ast(mem, l0->right);
            a1 = eval_ast(mem, tab, l1->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) {
                *out_val = val_void();
                return true;
            }
            if (val_get_type(mem, a0) == mem->fast_real_type &&
                val_get_unit(mem, a0) == MEM_UNIT_NONE) {
                int count = 0;
                if (!delegate_val_to_exact_int(mem, tab, a0, "vector fill count", &count)) {
                    *out_val = val_void();
                    return true;
                }
                if (count < 0 || (uint32_t)count > MEM_MAX_HEAP_SLOTS) {
                    mem_err(mem, "Vector fill count out of range (%d)", count);
                    *out_val = val_void();
                    return true;
                }
                uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, (uint32_t)count);
                if (mem->has_error) {
                    *out_val = val_void();
                    return true;
                }
                Value vhandle = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
                for (int i = 0; i < count && !mem->has_error; ++i) {
                    (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, a1, val_void());
                }
                *out_val = mem->has_error ? val_void() : vhandle;
                return true;
            }
        }
        uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, total_args);
        if (mem->has_error) {
            *out_val = val_void();
            return true;
        }
        Value vhandle = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
        (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, a0, val_void());
        if (total_args == 2 && !mem->has_error) {
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, a1, val_void());
        }
        *out_val = mem->has_error ? val_void() : vhandle;
        return true;
    }

    uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, total_args);
    if (mem->has_error) {
        *out_val = val_void();
        return true;
    }
    Value vhandle = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    uint16_t root_mark = mem_push_temp_root(mem, &vhandle);
    uint16_t cur = n->left;
    while (cur != MEM_NO_IDX && !mem->has_error) {
        const AstNode *link = mem_get_ast(mem, cur);
        Value item = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            mem_pop_temp_roots(mem, root_mark);
            *out_val = val_void();
            return true;
        }
        (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, item, val_void());
        cur = link->right;
    }
    mem_pop_temp_roots(mem, root_mark);
    *out_val = mem->has_error ? val_void() : vhandle;
    return true;
}

/*code section below manages hardcoding O(1) range, nested exclusion range, & stepped-range AST rewriting, evaluation, and membership without allocating a range vector*/
/*Construct a...b, a..<b, a>..b, a>..<b, a..<x...y>..b, a..<x...y>..<z...w>..<b, x in a...b, x in (a...b by step), a...b by step*/
static bool is_direct_vrange_binary(const MemArena *mem, const SymTable *tab,
                                    uint16_t ast_idx, int *out_mode) {
    if (out_mode) *out_mode = 0;
    if (ast_idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, ast_idx);
    if (n->kind != AST_BINARY) return false;
    uint16_t sid = n->ref;
    if (sid >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sid]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == n->sub && sym_mod_enabled(tab, ov->mod_id) &&
            ov->body_ast != MEM_NO_IDX) {
            const AstNode *b = mem_get_ast(mem, ov->body_ast);
            if (b->kind == AST_INTRINSIC && b->sub == INTR_VRANGE) {
                if (out_mode && b->ref != 0 && b->ref != MEM_NO_IDX) {
                    const AstNode *mn = mem_get_ast(mem, b->ref);
                    if (mn->kind == AST_LIT) {
                        *out_mode = (int)mem_read_pool(mem, mn->ref);
                    }
                }
                return true;
            }
        }
    }
    return false;
}

static bool is_vrange_cat_ast(const MemArena *mem, uint16_t ast_idx) {
    if (ast_idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, ast_idx);
    return (n->kind == AST_INTRINSIC && n->sub == INTR_VRANGE_CAT && n->ref == 0);
}

typedef struct {
    uint16_t lo_ast;
    uint16_t hi_ast;
    int      mode;
} VRangeSegAst;

static bool collect_vrange_segs(const MemArena *mem, const SymTable *tab,
                                uint16_t ast_idx, VRangeSegAst *segs,
                                uint8_t *cnt, uint8_t max_segs) {
    if (ast_idx == MEM_NO_IDX) return false;
    int mode = 0;
    if (is_direct_vrange_binary(mem, tab, ast_idx, &mode)) {
        if (*cnt >= max_segs) return false;
        const AstNode *n = mem_get_ast(mem, ast_idx);
        segs[*cnt].lo_ast = n->left;
        segs[*cnt].hi_ast = n->right;
        segs[*cnt].mode   = mode;
        (*cnt)++;
        return true;
    }
    if (is_vrange_cat_ast(mem, ast_idx)) {
        const AstNode *n = mem_get_ast(mem, ast_idx);
        return collect_vrange_segs(mem, tab, n->left, segs, cnt, max_segs) &&
               collect_vrange_segs(mem, tab, n->right, segs, cnt, max_segs);
    }
    return false;
}

static bool extract_vrange_expr_info(const MemArena *mem, const SymTable *tab,
                                     uint16_t ast_idx, VRangeSegAst *segs,
                                     uint8_t *out_seg_cnt, uint8_t max_segs,
                                     uint16_t *out_step_ast) {
    if (out_seg_cnt)  *out_seg_cnt  = 0;
    if (out_step_ast) *out_step_ast = MEM_NO_IDX;
    if (ast_idx == MEM_NO_IDX) return false;

    uint8_t cnt = 0;
    if (collect_vrange_segs(mem, tab, ast_idx, segs, &cnt, max_segs) && cnt > 0) {
        if (out_seg_cnt) *out_seg_cnt = cnt;
        return true;
    }

    const AstNode *n = mem_get_ast(mem, ast_idx);
    if (n->kind == AST_BINARY && n->ref < tab->sym_count) {
        bool is_by_op = false;
        for (uint16_t i = tab->sym_first_ov[n->ref]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
            const OpOverload *ov = &tab->overloads[i];
            if (ov->fixity == n->sub && sym_mod_enabled(tab, ov->mod_id) &&
                ov->body_ast != MEM_NO_IDX) {
                const AstNode *b = mem_get_ast(mem, ov->body_ast);
                if (b->kind == AST_INTRINSIC && b->sub == INTR_VRANGE_STEP) {
                    is_by_op = true;
                    break;
                }
            }
        }
        if (is_by_op) {
            cnt = 0;
            if (collect_vrange_segs(mem, tab, n->left, segs, &cnt, max_segs) && cnt > 0) {
                if (out_seg_cnt)  *out_seg_cnt  = cnt;
                if (out_step_ast) *out_step_ast = n->right;
                return true;
            }
        }
    }
    return false;
}

static bool is_pending_skip_node(const MemArena *mem, uint16_t ast_idx) {
    if (ast_idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, ast_idx);
    return (n->kind == AST_INTRINSIC && n->sub == INTR_VRANGE_CAT && n->ref == 0xFFFFu);
}

uint16_t hardcode_rewrite_range_chain(Parser *P, OpFixity fix, uint16_t sym_id,
                                      uint16_t left, uint16_t right) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    uint16_t raw_node = mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, sym_id, left, right);
    if (raw_node == MEM_NO_IDX || mem->has_error) return raw_node;

    int cur_mode = 0;
    if (!is_direct_vrange_binary(mem, tab, raw_node, &cur_mode)) {
        return raw_node;
    }

    int left_mode = 0;
    bool left_is_vrange  = is_direct_vrange_binary(mem, tab, left, &left_mode);
    bool left_is_pending = is_pending_skip_node(mem, left);
    bool left_is_cat     = !left_is_pending && is_vrange_cat_ast(mem, left);

    if (!left_is_vrange && !left_is_pending && !left_is_cat) {
        return raw_node;
    }

    /* Case 1: `left` is an open skip-start segment (`a..<s1` or `a>..<s1`) */
    if (left_is_vrange && (left_mode & 1) != 0) {
        if ((cur_mode & 2) == 0) {
            /* `(a..<s1) ... e1` -> pending skip node (ref == 0xFFFFu) */
            return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0xFFFFu,
                                 left, right);
        }
        /* Otherwise `(a..<b) >.. c` is a chained multi-segment range without skipping `b`:
           segment 1 is `a..<b`, segment 2 is `b>..c` (or `b>..<c`). */
        const AstNode *ln = mem_get_ast(mem, left);
        uint16_t b_ast    = ln->right;
        uint16_t seg2_ast = mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, sym_id, b_ast, right);
        return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0, left, seg2_ast);
    }

    /* Case 2: `left` is a pending skip node `(prefix_seg, e1)` closed by `>..` or `>..<` */
    if (left_is_pending) {
        const AstNode *ln = mem_get_ast(mem, left);
        uint16_t prefix_seg = ln->left;
        uint16_t e1_ast     = ln->right;
        if ((cur_mode & 2) == 0) {
            mem_err(mem, "Expected '>..' or '>..<' to close nested exclusion range");
            return MEM_NO_IDX;
        }
        uint16_t next_seg = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                          sym_id, e1_ast, right);
        return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                             prefix_seg, next_seg);
    }

    /* Case 3: `left` is a completed `INTR_VRANGE_CAT` (`cat(prev, seg_k)`) */
    if (left_is_cat) {
        const AstNode *cat_n = mem_get_ast(mem, left);
        if (is_pending_skip_node(mem, cat_n->right)) {
            const AstNode *rn = mem_get_ast(mem, cat_n->right);
            uint16_t prev_cat = mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                              cat_n->left, rn->left);
            uint16_t ek_ast   = rn->right;
            if ((cur_mode & 2) == 0) {
                mem_err(mem, "Expected '>..' or '>..<' to close nested exclusion range");
                return MEM_NO_IDX;
            }
            uint16_t next_seg = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                              sym_id, ek_ast, right);
            return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                 prev_cat, next_seg);
        }
        int r_mode = 0;
        if (is_direct_vrange_binary(mem, tab, cat_n->right, &r_mode)) {
            if ((r_mode & 1) != 0 && (cur_mode & 2) == 0) {
                /* `cat(prev, e1 >..< s2) ... e2` -> pending skip node on right child */
                uint16_t pend = mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0xFFFFu,
                                              cat_n->right, right);
                return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                     cat_n->left, pend);
            }
            if ((r_mode & 1) != 0 && (cur_mode & 2) != 0) {
                /* `cat(prev, a..<b) >.. c` */
                const AstNode *rn = mem_get_ast(mem, cat_n->right);
                uint16_t b_ast    = rn->right;
                uint16_t seg2_ast = mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, sym_id, b_ast, right);
                return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                     left, seg2_ast);
            }
        }
    }

    return raw_node;
}

/*code section below manages hardcoding lazy variable parameter positions for symbolic constructs and pipe operator AST rewriting*/
/*Construct sym_diff(x, ...), sym_integ(x, ...), sym_solve(x, ...), and x |> f / x |> f(a, b) / x |> (a => ...)*/
static bool hc_peek_is_lparen(const Lexer *lex) {
    uint16_t p = lex->pos;
    while (p < lex->len && (lex->src[p] == ' ' || lex->src[p] == '\t' || lex->src[p] == '\r')) p++;
    return (p < lex->len && lex->src[p] == '(');
}

static bool hc_peek_is_arrow(const Lexer *lex) {
    uint16_t p = lex->pos;
    while (p < lex->len && (lex->src[p] == ' ' || lex->src[p] == '\t' || lex->src[p] == '\r')) p++;
    return (p + 1 < lex->len && lex->src[p] == '=' && lex->src[p + 1] == '>');
}

static int hc_find_local_idx(const Parser *P, const char *name, uint16_t len) {
    for (int i = (int)P->vis_count - 1; i >= 0; --i) {
        StrSlice s = P->locals[i].name;
        if (s.len == len && memcmp(mem_str_ptr(P->lex.mem, s), name, len) == 0) {
            return i;
        }
    }
    return -1;
}

uint16_t hardcode_parse_pipe_rhs(Parser *P, uint16_t left, int next_min) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    if ((P->lex.cur.kind == TOK_IDENT ||
         (P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) &&
        !hc_peek_is_lparen(&P->lex) && !hc_peek_is_arrow(&P->lex)) {
        const char *fname = P->lex.src + P->lex.cur.off;
        uint16_t    flen  = P->lex.cur.len;
        if (hc_find_local_idx(P, fname, flen) < 0) {
            int fn_sid = sym_find(tab, mem, fname, flen);
            if (fn_sid >= 0) {
                lex_next(&P->lex);
                uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, MEM_NO_IDX);
                return mem_alloc_ast(mem, AST_CALL, 1, (uint16_t)fn_sid, link, MEM_NO_IDX);
            }
        }
    }
    uint16_t rhs_pipe = parser_parse_expr(P, next_min);
    if (mem->has_error || rhs_pipe == MEM_NO_IDX) return MEM_NO_IDX;
    const AstNode *rn = mem_get_ast(mem, rhs_pipe);
    if (rn->kind == AST_CALL) {
        uint16_t head_link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, rn->left);
        AstNode *mut_call  = mem_get_ast_mut(mem, rhs_pipe);
        mut_call->left     = head_link;
        mut_call->sub++;
        return rhs_pipe;
    }
    if (rn->kind == AST_FN_CALL) {
        uint16_t head_link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, rn->right);
        AstNode *mut_fcall = mem_get_ast_mut(mem, rhs_pipe);
        mut_fcall->right   = head_link;
        mut_fcall->sub++;
        return rhs_pipe;
    }
    uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, MEM_NO_IDX);
    return mem_alloc_ast(mem, AST_FN_CALL, 1, 0, rhs_pipe, link);
}

bool hardcode_eval_in_vrange_fast(MemArena *mem, SymTable *tab,
                                  const AstNode *n, uint16_t frame_base,
                                  uint8_t cur_ret_type, Value *out_val) {
    if (!hardcode_is_in_op_sym(mem, tab, n->ref)) return false;
    VRangeSegAst segs[8];
    uint8_t      seg_cnt  = 0;
    uint16_t     step_ast = MEM_NO_IDX;
    if (!extract_vrange_expr_info(mem, tab, n->right, segs, &seg_cnt, 8, &step_ast)) {
        return false;
    }
    Value item = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) {
        *out_val = val_void();
        return true;
    }
    Value vs = (step_ast != MEM_NO_IDX) ? eval_ast(mem, tab, step_ast, frame_base, cur_ret_type) : val_void();
    if (mem->has_error || mem->jump_active != JUMP_NONE) {
        *out_val = val_void();
        return true;
    }

    for (uint8_t s_i = 0; s_i < seg_cnt; ++s_i) {
        int vrange_mode = segs[s_i].mode;
        Value va = eval_ast(mem, tab, segs[s_i].lo_ast, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            *out_val = val_void();
            return true;
        }
        Value vb = eval_ast(mem, tab, segs[s_i].hi_ast, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            *out_val = val_void();
            return true;
        }
        uint8_t elem_uid = MEM_UNIT_NONE;
        if (!delegate_check_additive_unit_compat(mem, tab, "...", 3, va, vb, &elem_uid)) {
            *out_val = val_void();
            return true;
        }
        if (step_ast == MEM_NO_IDX) {
            int lo = 0, hi = 0;
            if (!delegate_val_to_exact_int(mem, tab, va, "range start", &lo) ||
                !delegate_val_to_exact_int(mem, tab, vb, "range end",   &hi)) {
                *out_val = val_void();
                return true;
            }
            if (val_get_type(mem, item) != mem->fast_real_type || val_get_unit(mem, item) != elem_uid) {
                continue;
            }
            double ditem = val_get_f64(mem, item);
            double ritem = round(ditem);
            if (fabs(ditem - ritem) > 1e-9 || ritem < -2147483647.0 || ritem > 2147483647.0) {
                continue;
            }
            long long k    = (long long)ritem;
            long long step = (lo <= hi) ? 1LL : -1LL;
            long long llo  = (long long)lo + ((vrange_mode & 2) ? step : 0LL);
            long long lhi  = (long long)hi - ((vrange_mode & 1) ? step : 0LL);
            bool hit = (step > 0) ? (k >= llo && k <= lhi) : (k <= llo && k >= lhi);
            if (hit) {
                *out_val = val_make_f64(mem, mem->fast_real_type, 1.0);
                return true;
            }
        } else {
            double da    = val_get_f64(mem, va);
            double db    = val_get_f64(mem, vb);
            double dstep = val_get_f64(mem, vs);
            if (!isfinite(dstep) || fabs(dstep) <= 1e-15) {
                mem_err(mem, "Range step cannot be zero");
                *out_val = val_void();
                return true;
            }
            if (val_get_type(mem, item) != mem->fast_real_type || val_get_unit(mem, item) != elem_uid) {
                continue;
            }
            double ditem = val_get_f64(mem, item);
            double eps   = 1e-11 * (1.0 + fabs(da) + fabs(db) + fabs(dstep));
            double start_v = (vrange_mode & 2) ? (da + dstep) : da;
            bool in_bounds = false;
            if (dstep > 0.0) {
                bool lo_ok = (ditem >= start_v - eps);
                bool hi_ok = (vrange_mode & 1) ? (ditem < db - eps) : (ditem <= db + eps);
                in_bounds  = lo_ok && hi_ok;
            } else {
                bool lo_ok = (ditem <= start_v + eps);
                bool hi_ok = (vrange_mode & 1) ? (ditem > db + eps) : (ditem >= db - eps);
                in_bounds  = lo_ok && hi_ok;
            }
            if (in_bounds) {
                double q  = (ditem - start_v) / dstep;
                double rq = floor(q + 0.5);
                if (rq >= 0.0 && fabs(q - rq) <= 1e-9) {
                    *out_val = val_make_f64(mem, mem->fast_real_type, 1.0);
                    return true;
                }
            }
        }
    }
    *out_val = val_make_f64(mem, mem->fast_real_type, 0.0);
    return true;
}

/*code section below manages hardcoding range, nested exclusion range, stepped-range, and collection iteration loops (with optional index variable) and vector comprehensions*/
/*Construct (i in a...b) { ... }, (i in a..<x...y>..b) { ... }, (i in a...b by s) { ... }, (idx, x in vec) { ... }, [expr for idx, x in iter]*/
Value hardcode_eval_for_in_ast(MemArena *mem, SymTable *tab,
                               const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type) {
    uint8_t  var_slot   = n->sub;
    uint16_t iter_ast   = n->left;
    bool     is_comp    = (n->ref & 0x8000u) != 0;
    bool     has_idx    = (n->ref & 0x4000u) != 0;
    uint8_t  idx_slot   = (uint8_t)((n->ref >> 8u) & 0x3Fu);
    uint16_t raw_lbl    = (uint16_t)(n->ref & 0x00FFu);
    uint8_t  loop_label = (raw_lbl > 0) ? (uint8_t)(raw_lbl - 1) : 0xFFu;

    uint16_t body_ast = n->right;
    uint16_t cond_ast = MEM_NO_IDX;
    Value    comp_vec = val_void();

    if (is_comp) {
        const AstNode *pair = mem_get_ast(mem, n->right);
        body_ast = pair->left;
        cond_ast = pair->right;
        uint16_t hdr_idx = mem_vec_alloc_hdr(mem, 0, 8);
        if (mem->has_error) return val_void();
        comp_vec = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    }

    Value    last  = is_comp ? comp_vec : val_make_f64(mem, mem->fast_real_type, 0.0);
    uint32_t guard = 0;

    /* Fast path: `(i in a...b [by step]) { ... }` or multi-segment `(i in 1..<25...75>..100) { ... }` without allocating range vector */
    VRangeSegAst segs[8];
    uint8_t      seg_cnt  = 0;
    uint16_t     step_ast = MEM_NO_IDX;
    if (extract_vrange_expr_info(mem, tab, iter_ast, segs, &seg_cnt, 8, &step_ast)) {
        Value vs = (step_ast != MEM_NO_IDX) ? eval_ast(mem, tab, step_ast, frame_base, cur_ret_type) : val_void();
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

        long long global_idx = 0;
        bool broke_loop = false;

        for (uint8_t s_i = 0; s_i < seg_cnt && !mem->has_error && !broke_loop; ++s_i) {
            int vrange_mode = segs[s_i].mode;
            Value va = eval_ast(mem, tab, segs[s_i].lo_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value vb = eval_ast(mem, tab, segs[s_i].hi_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

            uint8_t u = MEM_UNIT_NONE;
            if (!delegate_check_additive_unit_compat(mem, tab, "...", 3, va, vb, &u)) {
                return val_void();
            }

            double da   = val_get_f64(mem, va);
            double db   = val_get_f64(mem, vb);
            double step = (da <= db) ? 1.0 : -1.0;

            if (step_ast == MEM_NO_IDX) {
                int lo = 0, hi = 0;
                if (!delegate_val_to_exact_int(mem, tab, va, "range start", &lo) ||
                    !delegate_val_to_exact_int(mem, tab, vb, "range end",   &hi)) {
                    return val_void();
                }
                da   = (double)lo;
                db   = (double)hi;
                step = (lo <= hi) ? 1.0 : -1.0;
            } else {
                uint8_t us = val_get_unit(mem, vs);
                if (us != MEM_UNIT_NONE && us != u) {
                    mem_err(mem, "Unit mismatch in stepped range 'by'");
                    return val_void();
                }
                step = val_get_f64(mem, vs);
                if (!isfinite(step) || fabs(step) <= 1e-15) {
                    mem_err(mem, "Range step cannot be zero");
                    return val_void();
                }
            }

            double    eps       = 1e-11 * (1.0 + fabs(da) + fabs(db) + fabs(step));
            long long start_k   = (vrange_mode & 2) ? 1LL : 0LL;
            double    first_val = da + (double)start_k * step;
            long long cnt_ll    = 0;

            if (step > 0.0) {
                bool ok = (vrange_mode & 1) ? (first_val < db - eps) : (first_val <= db + eps);
                if (ok) cnt_ll = (long long)floor(((db - first_val) + ((vrange_mode & 1) ? -eps : eps)) / step) + 1LL;
            } else {
                bool ok = (vrange_mode & 1) ? (first_val > db + eps) : (first_val >= db - eps);
                if (ok) cnt_ll = (long long)floor(((first_val - db) + ((vrange_mode & 1) ? -eps : eps)) / (-step)) + 1LL;
            }

            for (long long iter_i = 0; iter_i < cnt_ll && !mem->has_error; ++iter_i) {
                double cur_v = da + (double)(start_k + iter_i) * step;
                if (fabs(cur_v) < 1e-15) cur_v = 0.0;
                if (has_idx) {
                    mem_set_slot(mem, frame_base, idx_slot,
                                 val_make_f64(mem, mem->fast_real_type, (double)global_idx));
                }
                global_idx++;
                mem_set_slot(mem, frame_base, var_slot,
                             val_make_f64_unit(mem, mem->fast_real_type, u, cur_v));
                if (cond_ast != MEM_NO_IDX) {
                    Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
                }
                Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
                if (is_comp) {
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
                } else {
                    last = item_res;
                    if (mem->jump_active == JUMP_CONTINUE_ANON ||
                        (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                        mem->jump_active = JUMP_NONE;
                        continue;
                    }
                    if (mem->jump_active == JUMP_BREAK_ANON ||
                        (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                        mem->jump_active = JUMP_NONE;
                        if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                            last = mem->jump_val;
                            mem->jump_val = val_void();
                        }
                        broke_loop = true;
                        break;
                    }
                    if (mem->jump_active != JUMP_NONE) {
                        broke_loop = true;
                        break;
                    }
                }
                if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                    mem->heap_active_count >= 64u) {
                    Value roots[2] = { last, comp_vec };
                    (void)mem_compact_scratch(mem, roots, 2);
                    last     = roots[0];
                    comp_vec = roots[1];
                }
                if (++guard > 1000000u) {
                    mem_err(mem, "Loop iteration limit exceeded");
                    broke_loop = true;
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    /* Collection path: `(v in vec) { ... }`, `(x in mat) { ... }`, `(c in str) { ... }`, or `[expr for v in coll]` */
    Value coll = eval_ast(mem, tab, iter_ast, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    uint16_t coll_tid = val_get_type(mem, coll);

    if (coll_tid == mem->vec_type_id && mem->vec_type_id != 0) {
        for (uint32_t idx = 0; !mem->has_error; ++idx) {
            uint16_t hdr_idx  = val_get_pool_idx(coll);
            uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
            uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
            uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
            uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                                ? tab->types[elem_tid].flat_size : 1;
            if (idx >= len) break;

            Value item = delegate_vec_read_elem(mem, tab, elem_tid, elem_uid,
                                                data_idx, idx * (uint32_t)stride);
            if (elem_tid != 0 && elem_tid < tab->type_count &&
                !tab->types[elem_tid].is_consuming &&
                elem_tid != mem->vec_type_id && elem_tid != mem->mat_type_id &&
                elem_tid != mem->fn_type_id) {
                item = val_clone_comp(mem, item, stride);
            }
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot,
                             val_make_f64(mem, mem->fast_real_type, (double)idx));
            }
            mem_set_slot(mem, frame_base, var_slot, item);
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
            if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                mem->heap_active_count >= 64u) {
                Value roots[3] = { coll, last, comp_vec };
                (void)mem_compact_scratch(mem, roots, 3);
                coll     = roots[0];
                last     = roots[1];
                comp_vec = roots[2];
            }
            if (++guard > 1000000u) {
                mem_err(mem, "Loop iteration limit exceeded");
                break;
            }
        }
        return is_comp ? comp_vec : last;
    }

    if (coll_tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, coll, &rank, &tot_len, &data_idx, dims);
        uint8_t u = val_get_unit(mem, coll);
        for (uint16_t idx = 0; idx < tot_len && !mem->has_error; ++idx) {
            Value item = val_make_f64_unit(mem, mem->fast_real_type, u,
                                           mem_data_read(mem, data_idx, idx));
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot,
                             val_make_f64(mem, mem->fast_real_type, (double)idx));
            }
            mem_set_slot(mem, frame_base, var_slot, item);
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    if (coll_tid == mem->str_type_id && mem->str_type_id != 0) {
        StrSlice s = val_get_str(mem, coll);
        for (uint16_t idx = 0; idx < s.len && !mem->has_error; ++idx) {
            char ch = mem_str_ptr(mem, s)[idx];
            Value item = val_make_str(mem, mem->str_type_id, mem_intern_str(mem, &ch, 1));
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot,
                             val_make_f64(mem, mem->fast_real_type, (double)idx));
            }
            mem_set_slot(mem, frame_base, var_slot, item);
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    if (coll_tid == mem->map_type_id && mem->map_type_id != 0) {
        uint16_t hdr_idx = val_get_pool_idx(coll);
        for (uint16_t idx = 0; !mem->has_error; ++idx) {
            uint16_t len      = (uint16_t)mem_read_pool(mem, hdr_idx + 1);
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            if (idx >= len) break;

            StrSlice kslice = mem_map_entry_key(mem, data_idx, idx);
            Value    kval   = val_make_str(mem, mem->str_type_id, kslice);
            Value    vval   = mem_map_entry_val(mem, data_idx, idx);
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot, kval);
                mem_set_slot(mem, frame_base, var_slot, vval);
            } else {
                mem_set_slot(mem, frame_base, var_slot, kval);
            }
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    mem_err(mem, "Loop target after 'in' must be a range (a...b / a..<b / a>..b / a>..<b), vector ($vec), matrix ($mat), map ($map), or string ($str)");
    return val_void();
}

/*code section below manages hardcoding multi-variable scalar broadcast and struct/vector/matrix destructuring*/
/*Construct x, y := 5 ; r1, r2 := quad_roots(1, -5, 6) ; a, b, c := vec(1, 2, 3)*/
Value hardcode_eval_unpack_elem_ast(MemArena *mem, const SymTable *tab,
                                    const AstNode *n, uint16_t frame_base) {
    uint8_t elem_idx = n->sub;
    uint8_t tot_vars = (uint8_t)n->ref;
    uint8_t tmp_slot = (uint8_t)n->left;
    Value   src      = mem_get_slot(mem, frame_base, tmp_slot);
    uint16_t tid     = val_get_type(mem, src);
    uint8_t  uid     = val_get_unit(mem, src);

    if (tid == 0 || tid >= tab->type_count) return val_void();

    /* 1. Consuming scalar (`real`, `iota`, `str`): broadcast to all variables (`x, y := 5`) */
    if (tab->types[tid].is_consuming) {
        return src;
    }

    /* 2. Dynamic vector (`$vec`): destructure element `elem_idx` (`a, b, c := vec(10, 20, 30)`) */
    if (tid == mem->vec_type_id && mem->vec_type_id != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(src);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1;
        if (elem_idx >= len) {
            mem_err(mem, "Cannot destructure %u variables from vector of length %u",
                    (unsigned)tot_vars, (unsigned)len);
            return val_void();
        }
        return delegate_vec_read_elem(mem, tab, elem_tid, elem_uid,
                                      data_idx, (uint32_t)elem_idx * (uint32_t)stride);
    }

    /* 3. Matrix (`$mat`): destructure element `elem_idx` (`a, b, c := [10, 20, 30]`) */
    if (tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, src, &rank, &tot_len, &data_idx, dims);
        if (elem_idx >= tot_len) {
            mem_err(mem, "Cannot destructure %u variables from matrix of length %u",
                    (unsigned)tot_vars, (unsigned)tot_len);
            return val_void();
        }
        return val_make_f64_unit(mem, mem->fast_real_type, uid,
                                 mem_data_read(mem, data_idx, elem_idx));
    }

    /* 4. Flat C-style struct (`$vec2`, `$vec3`, `$complex`, `$circle`): destructure field `elem_idx` */
    const TypeDef *td = &tab->types[tid];
    if (elem_idx >= td->field_count) {
        char tbuf[32];
        sym_type_name(tab, mem, tid, tbuf, sizeof(tbuf));
        mem_err(mem, "Cannot destructure %u variables from '%s' (has %u fields)",
                (unsigned)tot_vars, tbuf, (unsigned)td->field_count);
        return val_void();
    }
    const FieldDef *fd = &tab->fields[td->start_idx + elem_idx];
    uint16_t f_pidx    = (uint16_t)(val_get_pool_idx(src) + fd->flat_offset);
    if (!tab->types[fd->type_id].is_consuming) {
        return val_with_unit(mem,
                             val_make_comp(mem, fd->type_id, f_pidx, tab->types[fd->type_id].flat_size),
                             uid);
    }
    return val_make_f64_unit(mem, fd->type_id ? fd->type_id : mem->fast_real_type, uid,
                             mem_read_pool(mem, f_pidx));
}

/*code section below manages hardcoding pattern-matched 'switch' case evaluation (exact values, ranges, struct/vector destructuring)*/
/*Construct switch (expr) { case pat if guard : { ... } else : { ... } }*/
Value hardcode_eval_switch_eq_ast(MemArena *mem, SymTable *tab,
                                  const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type) {
    /* sub == 2: Pattern variable binding (`slot` in `n->ref`, value expr in `n->left`), returns 1.0 */
    if (n->sub == 2) {
        Value fval = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
        mem_set_slot(mem, frame_base, (uint8_t)n->ref, fval);
        return val_make_f64(mem, mem->fast_real_type, 1.0);
    }

    Value a = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    uint16_t ta = val_get_type(mem, a);
    uint8_t  ua = val_get_unit(mem, a);

    /* sub == 1: Struct type check (`ta == n->ref`) */
    if (n->sub == 1) {
        return val_make_f64(mem, mem->fast_real_type, (ta == n->ref) ? 1.0 : 0.0);
    }

    /* sub == 3: Vector / 1D matrix length check (`len == n->ref`) */
    if (n->sub == 3) {
        bool ok = false;
        if (ta == mem->vec_type_id && mem->vec_type_id != 0) {
            uint16_t hdr_idx = val_get_pool_idx(a);
            uint16_t len     = (uint16_t)mem_read_pool(mem, hdr_idx + 1);
            ok = (len == n->ref);
        } else if (ta == mem->mat_type_id && mem->mat_type_id != 0) {
            uint8_t  rank = 0;
            uint16_t tot_len = 0, dims[4] = { 0 };
            if (delegate_mat_get_info(mem, a, &rank, &tot_len, NULL, dims) && rank == 1) {
                ok = (tot_len == n->ref);
            }
        }
        return val_make_f64(mem, mem->fast_real_type, ok ? 1.0 : 0.0);
    }

    /* sub == 0: Fast path for range & nested exclusion range patterns `case a...b:` / `case 1..<25...75>..100:` */
    VRangeSegAst sw_segs[8];
    uint8_t      sw_seg_cnt = 0;
    if (collect_vrange_segs(mem, tab, n->right, sw_segs, &sw_seg_cnt, 8) && sw_seg_cnt > 0) {
        if (ta == 0 || ta >= tab->type_count || !tab->types[ta].is_consuming || ta == mem->str_type_id) {
            return val_make_f64(mem, mem->fast_real_type, 0.0);
        }
        double da = val_get_f64(mem, a);
        for (uint8_t s_i = 0; s_i < sw_seg_cnt; ++s_i) {
            int sw_vrange_mode = sw_segs[s_i].mode;
            Value vlo = eval_ast(mem, tab, sw_segs[s_i].lo_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value vhi = eval_ast(mem, tab, sw_segs[s_i].hi_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

            uint8_t ulo = val_get_unit(mem, vlo);
            uint8_t uhi = val_get_unit(mem, vhi);
            if ((ulo != MEM_UNIT_NONE && ua != ulo) || (uhi != MEM_UNIT_NONE && ua != uhi)) {
                continue;
            }
            double dlo = val_get_f64(mem, vlo);
            double dhi = val_get_f64(mem, vhi);
            bool in_rng = false;
            if (dlo <= dhi) {
                bool lo_ok = (sw_vrange_mode & 2) ? (da > dlo + 1e-12) : (da >= dlo - 1e-12);
                bool hi_ok = (sw_vrange_mode & 1) ? (da < dhi - 1e-12) : (da <= dhi + 1e-12);
                in_rng = lo_ok && hi_ok;
            } else {
                bool lo_ok = (sw_vrange_mode & 2) ? (da < dlo - 1e-12) : (da <= dlo + 1e-12);
                bool hi_ok = (sw_vrange_mode & 1) ? (da > dhi + 1e-12) : (da >= dhi - 1e-12);
                in_rng = lo_ok && hi_ok;
            }
            if (in_rng) {
                return val_make_f64(mem, mem->fast_real_type, 1.0);
            }
        }
        return val_make_f64(mem, mem->fast_real_type, 0.0);
    }

    Value b = eval_ast(mem, tab, n->right, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    uint16_t tb = val_get_type(mem, b);
    uint8_t  ub = val_get_unit(mem, b);

    bool eq = false;
    if (ta == mem->str_type_id && tb == mem->str_type_id && mem->str_type_id != 0) {
        eq = mem_slice_eq(mem, val_get_str(mem, a), val_get_str(mem, b));
    } else if (tb == mem->vec_type_id && ta != mem->vec_type_id && mem->vec_type_id != 0) {
        int in_sid = sym_find(tab, mem, "in", 2);
        if (in_sid >= 0) {
            Value in_args[2] = { a, b };
            Value vin_res = eval_dispatch_overload(mem, tab, (uint16_t)in_sid, FIX_INFIX_LEFT, 2, in_args);
            eq = (!mem->has_error && fabs(val_get_f64(mem, vin_res)) > 1e-15);
        }
    } else if (ta == tb && ua == ub) {
        if (ta > 0 && ta < tab->type_count && !tab->types[ta].is_consuming) {
            uint16_t sz = tab->types[ta].flat_size;
            uint16_t pa = val_get_pool_idx(a);
            uint16_t pb = val_get_pool_idx(b);
            eq = true;
            for (uint16_t k = 0; k < sz; ++k) {
                if (fabs(mem_read_pool(mem, pa + k) - mem_read_pool(mem, pb + k)) > 1e-12) {
                    eq = false;
                    break;
                }
            }
        } else {
            eq = (fabs(val_get_f64(mem, a) - val_get_f64(mem, b)) <= 1e-12);
        }
    }
    return val_make_f64(mem, mem->fast_real_type, eq ? 1.0 : 0.0);
}
