/*
 * parser.c — Standalone Pratt Parser & AST Builder Implementation
 *
 * Supports:
 *   - Default newline `;` statement separator both inside and outside `{ ... }` / `( ... )`
 *     for one-statement-per-line writing, while keeping `;` compulsory when placing
 *     multiple statements on a single line
 *   - Lexical `{ ... }` scope blocks (value-returning and void/isolated blocks)
 *   - Module-scoped vs. local-scoped variables, privacy (`pub` / `priv`), and
 *     mutability (`:=` / `mut` mutable vs. `=` / `const` immutable)
 *   - Multi-branch `if ... or ... else` statements (one `if`, many `or` else-if
 *     branches, optional single `else`, plus legacy `if(c, t, e)`)
 *   - Pattern-matched `switch (expr) { ... }` blocks (exact values, `a..b` / `a...b`
 *     ranges, relational guards `case > 10:`, struct destructuring `case vec2(0, y):`,
 *     vector destructuring `case [0, y]:`, and optional `if <cond>` guards)
 *   - Standard right-associative ternary `cond ? x : y`
 *   - Colon-tagged scopes/loops `Tag : { ... }`, `Tag : (cond) ?? { ... }`, and
 *     `Tag : (i in 1...5) { ... }` with `jump(Tag)`, `run(Tag)`, `break(Tag)`,
 *     `break(Tag, val)`, `break val`, `continue`, `continue(Tag)`, and `return val`
 *   - Keywordless range/collection loops `(i in 1...5) { ... }`, `i in 1...5 { ... }`,
 *     `(v in vec) { ... }` as well as `loop (i in ...) { ... }` and `loop (cond) { ... }`
 *   - Parameterized container constructors `vec<T>(...)`, `list<T>(...)`,
 *     `stack<T>(...)`, `queue<T>(...)`
 *   - Vector comprehensions `[x^2 for x in 1...5]` and `[x^2 for x in 1...10 if x % 2 == 0]`
 *   - First-class anonymous lambdas `(x) => x^2`, `x => x^2`, `(a, b) => a + b`, `() => 42`
 *     and direct `$fn` call syntax `f(x)` / `((x) => x^2)(5)`
 *   - Implicit multiplication (`2x`, `2π`, `3(x + 1)`, `(x + 1)(x - 1)`, `2 sin(x)`)
 *   - Uniform Function Call / dot-chaining syntax (`v.push(10)`, `v.map(x => x^2)`)
 *   - Multi-variable declarations (`x, y := 5`), struct/vector/matrix destructuring
 *     (`r1, r2 := quad_roots(1, -5, 6)`), and compound assignments (`+=`, `-=`, etc.)
 *   - Concrete Entity Type & Concrete Unit Type variable annotations
 *   - Reserved keyword guards for variables and scope tags
 */

#include "parser.h"
#include "../lib/delegate/delegate.h"
#include "../lib/hardcode/hardcode.h"
#include <string.h>

static bool is_var_modifier_kw(const char *s, uint16_t len) {
    return (len == 3 && (memcmp(s, "pub", 3) == 0 || memcmp(s, "mut", 3) == 0)) ||
           (len == 4 && memcmp(s, "priv", 4) == 0) ||
           (len == 5 && memcmp(s, "const", 5) == 0);
}

static int find_local_idx(const Parser *P, const char *name, uint16_t len) {
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

static uint8_t parser_alloc_anon_slot(Parser *P, uint16_t type_id) {
    if (P->local_count >= PARSER_MAX_LOCALS) {
        mem_err(P->lex.mem, "Too many local variables (max %d)", PARSER_MAX_LOCALS);
        return 0;
    }
    uint8_t slot = P->local_count++;
    P->slot_types[slot] = type_id;
    return slot;
}

static int find_label_id(const Parser *P, const char *name, uint16_t len) {
    for (uint8_t i = 0; i < P->label_count; ++i) {
        StrSlice s = P->labels[i].name;
        if (s.len == len && memcmp(mem_str_ptr(P->lex.mem, s), name, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int parser_get_or_add_label(Parser *P, const char *name, uint16_t len) {
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

static bool sym_has_visible_func_ov(const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == FIX_FUNC && sym_ov_is_visible(tab, ov)) {
            return true;
        }
    }
    return false;
}

static uint8_t token_func_meta(const Parser *P, Token t) {
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

static uint8_t cur_op_meta(const Parser *P) {
    if (P->lex.cur.kind != TOK_OP) return 0;
    uint8_t m = sym_active_meta_tag(P->lex.tab, P->lex.cur.sym_id, FIX_INFIX_LEFT);
    if (!m) m = sym_active_meta_tag(P->lex.tab, P->lex.cur.sym_id, FIX_INFIX_RIGHT);
    return m;
}

static uint16_t skip_raw_ws(const char *src, uint16_t p, uint16_t len) {
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

static bool lex_at_is_syntax(const Lexer *lex, uint16_t p, uint8_t target_syn) {
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

static bool lex_peek_is_lbrace(const Lexer *lex) {
    return lex_at_is_syntax(lex, lex->pos, INTR_LBRACE);
}

static bool lex_peek_is_lparen(const Lexer *lex) {
    return lex_at_is_syntax(lex, lex->pos, INTR_LPAREN);
}

static bool lex_match_in_kw_at(const Parser *P, uint16_t p) {
    if (p >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[p])) return false;
    uint16_t istart = p;
    while (p < P->lex.len && lex_is_ident_cont((unsigned char)P->lex.src[p])) p++;
    return hardcode_is_in_kw(P->lex.mem, P->lex.tab, P->lex.src + istart, (uint16_t)(p - istart));
}

static bool lex_peek_is_ident_in(const Parser *P) {
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

static bool lex_is_two_var_in(const Parser *P) {
    const Lexer *lex = &P->lex;
    uint16_t p = skip_raw_ws(lex->src, lex->pos, lex->len);
    if (p >= lex->len || !lex_is_ident_start((unsigned char)lex->src[p])) return false;
    while (p < lex->len && lex_is_ident_cont((unsigned char)lex->src[p])) p++;
    p = skip_raw_ws(lex->src, p, lex->len);
    return lex_match_in_kw_at(P, p);
}

static bool lex_peek_is_branch_kw(const Parser *P) {
    uint16_t p = skip_raw_ws(P->lex.src, P->lex.pos, P->lex.len);
    if (p >= P->lex.len || !lex_is_ident_start((unsigned char)P->lex.src[p])) return false;
    uint16_t start = p;
    while (p < P->lex.len && lex_is_ident_cont((unsigned char)P->lex.src[p])) p++;
    int sid = sym_find(P->lex.tab, P->lex.mem, P->lex.src + start, p - start);
    if (sid < 0) return false;
    uint8_t m = sym_active_meta_tag(P->lex.tab, (uint16_t)sid, FIX_FUNC);
    return (m == INTR_OR_BRANCH || m == INTR_ELSE_BRANCH);
}

static int find_active_meta_sym(const SymTable *tab, uint8_t target_meta) {
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->meta_tag == target_meta && sym_ov_is_visible(tab, ov)) {
            return (int)ov->sym_id;
        }
    }
    return -1;
}

static bool lex_is_multi_lhs(const Parser *P) {
    const Lexer    *lex = &P->lex;
    const SymTable *tab = lex->tab;
    const MemArena *mem = lex->mem;
    uint16_t p = lex->pos;

    while (p < lex->len) {
        p = skip_raw_ws(lex->src, p, lex->len);
        if (p >= lex->len || !lex_is_ident_start((unsigned char)lex->src[p])) {
            return false;
        }
        while (p < lex->len && lex_is_ident_cont((unsigned char)lex->src[p])) {
            p++;
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

static uint16_t resolve_annot_type(const SymTable *tab, const MemArena *mem,
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

static bool sym_has_non_lazy_func_ov(const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == FIX_FUNC && sym_ov_is_visible(tab, ov) &&
            (ov->meta_tag & SYM_META_LAZY_MASK) == 0) {
            return true;
        }
    }
    return false;
}

static int find_active_sub_sym(const MemArena *mem, const SymTable *tab) {
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        const OpOverload *ov = &tab->overloads[i];
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

static bool parse_var_type_unit_annot(Parser *P, uint8_t *out_lock_type, uint8_t *out_lock_unit) {
    MemArena       *mem = P->lex.mem;
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

    /* Optional `<T>` on container variable type annotation `v : vec<real> := ...` */
    if (P->lex.cur.kind == TOK_OP &&
        tab->syms[P->lex.cur.sym_id].len == 1 &&
        mem_str_ptr(mem, tab->syms[P->lex.cur.sym_id])[0] == '<') {
        lex_next(&P->lex); /* consume '<' */
        while (!mem->has_error && P->lex.cur.kind != TOK_EOF) {
            if (P->lex.cur.kind == TOK_OP &&
                tab->syms[P->lex.cur.sym_id].len == 1 &&
                mem_str_ptr(mem, tab->syms[P->lex.cur.sym_id])[0] == '>') {
                lex_next(&P->lex);
                break;
            }
            lex_next(&P->lex);
        }
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

        uint16_t tid = resolve_annot_type(tab, mem, s1_ptr, s1_len);
        if (tid == MEM_TYPE_VOID) {
            mem_err(mem, "Unknown entity type '%.*s' in variable declaration", (int)s1_len, s1_ptr);
            return false;
        }
        uint8_t uid = sym_find_or_add_unit(tab, mem, s2_ptr, s2_len);
        *out_lock_type = (tid == SYM_TYPE_ANY) ? 0 : (uint8_t)tid;
        *out_lock_unit = (uid == SYM_UNIT_GENERIC) ? 0 : uid;
        return !mem->has_error;
    }

    uint16_t tid = resolve_annot_type(tab, mem, s1_ptr, s1_len);
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

static uint16_t build_store_or_compound(Parser *P, uint16_t op_sym_id,
                                        uint16_t lhs_ast, uint16_t rhs_ast) {
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    if (lhs_ast == MEM_NO_IDX || rhs_ast == MEM_NO_IDX) return MEM_NO_IDX;

    bool is_colon_eq = false;
    if (hardcode_is_plain_store_sym(mem, tab, op_sym_id, &is_colon_eq)) {
        const AstNode *ln = mem_get_ast(mem, lhs_ast);
        const AstNode *rn = mem_get_ast(mem, rhs_ast);
        if (ln->kind == AST_SLOT_LOAD && ln->left == 0) {
            uint8_t sidx = (uint8_t)(ln->sub & AST_SLOT_IDX_MASK);
            if (rn->kind == AST_CONSTRUCT) {
                P->slot_types[sidx] = rn->ref;
            } else if (rn->kind == AST_LAMBDA && mem->fn_type_id != 0) {
                P->slot_types[sidx] = mem->fn_type_id;
            }
        } else if (ln->kind == AST_CONST_SYM) {
            int own_idx = sym_find_own_var(tab, ln->ref, P->cur_mod);
            if (own_idx >= 0) {
                ModVarDef *mv = &sym_vars_mut(tab)[own_idx];
                if (rn->kind == AST_CONSTRUCT) {
                    mv->lock_type = (uint16_t)((mv->lock_type & 0xFFu) | ((uint16_t)rn->ref << 8u));
                } else if (rn->kind == AST_LAMBDA && mem->fn_type_id != 0) {
                    mv->lock_type = (uint16_t)((mv->lock_type & 0xFFu) | ((uint32_t)mem->fn_type_id << 8u));
                }
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

static uint16_t build_distributed_comp(Parser *P, OpFixity fix, uint16_t comp_sym,
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

static uint16_t parse_field_step(Parser *P, uint16_t left) {
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

static uint16_t parse_mat_literal(Parser *P) {
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

static uint16_t parse_index_step(Parser *P, uint16_t left) {
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

static uint16_t parse_fn_call_step(Parser *P, uint16_t fn_ast) {
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

static uint16_t parse_scope_block(Parser *P) {
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

static uint16_t parse_branch_body(Parser *P) {
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

static uint16_t parse_lambda_from_paren(Parser *P) {
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

static uint16_t parse_lambda_from_ident(Parser *P, const char *id_str, uint16_t id_len) {
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

static uint16_t resolve_var_for_assign(Parser *P, const char *name, uint16_t len,
                                       uint16_t op_sym_id, uint8_t lock_type, uint8_t lock_unit);

static uint16_t parse_inline_fn_def(Parser *P, const char *fn_name, uint16_t fn_len) {
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

static uint16_t parse_if_construct(Parser *P, uint16_t if_sym_id) {
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

                if ((P->lex.cur.kind == TOK_IDENT || tok_is_underscore(P, P->lex.cur)) &&
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

            if ((P->lex.cur.kind == TOK_IDENT || tok_is_underscore(P, P->lex.cur)) &&
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

static uint16_t parse_switch_construct(Parser *P) {
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
        if (kw_meta == INTR_ELSE_BRANCH) {
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

static uint16_t parse_tag_op(Parser *P, AstKind kind, const char *op_name) {
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
    return true;
}

static uint16_t parse_break_construct(Parser *P) {
    MemArena *mem = P->lex.mem;
    if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN) {
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

static uint16_t parse_continue_construct(Parser *P) {
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

static uint16_t parse_return_construct(Parser *P) {
    MemArena *mem = P->lex.mem;
    if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN) {
        lex_next(&P->lex);
        if (P->lex.cur.kind == TOK_RPAREN) {
            lex_next(&P->lex);
            return mem_alloc_ast(mem, AST_RETURN, 0, 0, MEM_NO_IDX, MEM_NO_IDX);
        }
        uint16_t val_ast = parser_parse_expr(P, 0);
        if (mem->has_error || val_ast == MEM_NO_IDX) return MEM_NO_IDX;
        if (P->lex.cur.kind != TOK_RPAREN) {
            mem_err(mem, "Expected ')' after 'return(...)'");
            return MEM_NO_IDX;
        }
        lex_next(&P->lex);
        return mem_alloc_ast(mem, AST_RETURN, 0, 0, val_ast, MEM_NO_IDX);
    }

    if (can_follow_jump_value_expr(P)) {
        uint16_t val_ast = parser_parse_expr(P, 2);
        return mem_alloc_ast(mem, AST_RETURN, 0, 0, val_ast, MEM_NO_IDX);
    }

    return mem_alloc_ast(mem, AST_RETURN, 0, 0, MEM_NO_IDX, MEM_NO_IDX);
}

static uint16_t resolve_var_for_assign(Parser *P, const char *name, uint16_t len,
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

static uint16_t parse_nud(Parser *P) {
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    Token t = P->lex.cur;
    P->last_was_block = false;

    /* 1. Consumed Literal Value ($real, $str) */
    if (t.kind == TOK_VAL) {
        lex_next(&P->lex);
        uint16_t tid = val_get_type(mem, t.val);
        if (tid == mem->str_type_id && tid != 0) {
            StrSlice s = val_get_str(mem, t.val);
            return mem_alloc_ast(mem, AST_LIT, (uint8_t)tid, 0, s.off, s.len);
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

        /* 6a. Control-flow constructs bound in `Library/bootstrap.math` */
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
    MemArena       *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    uint16_t left = parse_nud(P);
    if (mem->has_error || left == MEM_NO_IDX) return MEM_NO_IDX;

    while (!mem->has_error) {
        /* Direct call on lambda or function expression: `((x) => x^2)(5)` */
        if (!P->lex.cur.had_newline && P->lex.cur.kind == TOK_LPAREN && 95 >= min_prec &&
            (mem_get_ast(mem, left)->kind == AST_LAMBDA ||
             mem_get_ast(mem, left)->kind == AST_FN_CALL)) {
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
                }

                uint16_t right          = MEM_NO_IDX;
                bool     is_in_op       = hardcode_is_in_op_sym(mem, tab, t.sym_id);
                bool     rhs_unparen    = (P->lex.cur.kind != TOK_LPAREN);
                int      dual_unit_sid  = -1;

                if (is_in_op && (P->lex.cur.kind == TOK_OP || P->lex.cur.kind == TOK_IDENT)) {
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
                    right = parser_parse_expr(P, next_min);
                    if (is_in_op && rhs_unparen && dual_unit_sid >= 0 && right != MEM_NO_IDX) {
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
