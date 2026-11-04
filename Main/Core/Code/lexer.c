/*
 * lexer.c — Standalone UTF-8 & Dynamic Pattern-Driven Lexer Implementation
 */

#include "lexer.h"
#include "../lib/delegate/delegate.h"
#include <stdlib.h>
#include <string.h>

bool lex_is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

bool lex_is_ident_cont(unsigned char c) {
    return lex_is_ident_start(c) || c == '_' || (c >= '0' && c <= '9');
}

static bool is_ascii_word(const char *s, uint16_t len) {
    if (len == 0) return false;
    unsigned char c0 = (unsigned char)s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) return false;
    for (uint16_t i = 1; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

static bool ident_continues_at(const char *src, uint16_t p, uint16_t len) {
    if (p >= len) return false;
    unsigned char c = (unsigned char)src[p];
    if (lex_is_ident_start(c) || (c >= '0' && c <= '9')) return true;
    if (c == '_') {
        if (p + 1 >= len) return false;
        unsigned char nc = (unsigned char)src[p + 1];
        return lex_is_ident_start(nc) || (nc >= '0' && nc <= '9');
    }
    return false;
}

static int match_single_atom(const MemArena *mem, const PatElem *pe,
                             const char *src, uint16_t pos, uint16_t len) {
    if (pos >= len) return -1;
    unsigned char c = (unsigned char)src[pos];
    switch (pe->kind) {
        case PAT_DIGIT:
            return (c >= '0' && c <= '9') ? 1 : -1;
        case PAT_ALPHA:
            return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') ? 1 : -1;
        case PAT_LIT: {
            if (pe->chars.len == 0 || pos + pe->chars.len > len) return -1;
            if (memcmp(src + pos, mem_str_ptr(mem, pe->chars), pe->chars.len) == 0) {
                return (int)pe->chars.len;
            }
            return -1;
        }
        case PAT_CHARS: {
            const char *cset = mem_str_ptr(mem, pe->chars);
            for (uint16_t i = 0; i < pe->chars.len; ++i) {
                if (src[pos] == cset[i]) return 1;
            }
            return -1;
        }
        case PAT_NOT_CHARS: {
            const char *excl = mem_str_ptr(mem, pe->chars);
            bool excl_quote = false;
            for (uint16_t i = 0; i < pe->chars.len; ++i) {
                if (excl[i] == '"') excl_quote = true;
                if (src[pos] == excl[i]) return -1;
            }
            if (excl_quote && src[pos] == '\\' && pos + 1 < len) {
                return 2;
            }
            int ulen = 1;
            if ((c & 0xE0) == 0xC0) ulen = 2;
            else if ((c & 0xF0) == 0xE0) ulen = 3;
            else if ((c & 0xF8) == 0xF0) ulen = 4;
            return (pos + ulen <= len) ? ulen : 1;
        }
        default:
            return -1;
    }
}

static bool group_has_top_alt(const SymTable *tab, uint16_t start, uint16_t end) {
    uint16_t depth = 0;
    for (uint16_t i = start; i < end; ++i) {
        uint8_t k = tab->pat_elems[i].kind;
        if (k == PAT_GROUP_START) depth++;
        else if (k == PAT_GROUP_END && depth > 0) depth--;
        else if (k == PAT_ALT && depth == 0) return true;
    }
    return false;
}

static int match_pat_seq(const MemArena *mem, const SymTable *tab,
                         uint16_t p_idx, uint16_t p_end,
                         const char *src, uint16_t pos, uint16_t len);

static int match_group_body(const MemArena *mem, const SymTable *tab,
                            uint16_t g_start, uint16_t g_end,
                            const char *src, uint16_t pos, uint16_t len) {
    if (!group_has_top_alt(tab, g_start, g_end)) {
        return match_pat_seq(mem, tab, g_start, g_end, src, pos, len);
    }
    int best_end = -1;
    uint16_t b_start = g_start;
    uint16_t depth   = 0;
    for (uint16_t i = g_start; i <= g_end; ++i) {
        bool is_split = (i == g_end);
        if (!is_split) {
            uint8_t k = tab->pat_elems[i].kind;
            if (k == PAT_GROUP_START) depth++;
            else if (k == PAT_GROUP_END && depth > 0) depth--;
            else if (k == PAT_ALT && depth == 0) is_split = true;
        }
        if (is_split) {
            int sub_end = match_pat_seq(mem, tab, b_start, i, src, pos, len);
            if (sub_end > best_end) best_end = sub_end;
            b_start = (uint16_t)(i + 1);
        }
    }
    return best_end;
}

static int match_pat_seq(const MemArena *mem, const SymTable *tab,
                         uint16_t p_idx, uint16_t p_end,
                         const char *src, uint16_t pos, uint16_t len) {
    if (p_idx >= p_end) return (int)pos;
    const PatElem *pe = &tab->pat_elems[p_idx];

    if (pe->kind == PAT_GROUP_START) {
        uint16_t depth = 1;
        uint16_t g_end = p_idx + 1;
        while (g_end < p_end && depth > 0) {
            if (tab->pat_elems[g_end].kind == PAT_GROUP_START) depth++;
            else if (tab->pat_elems[g_end].kind == PAT_GROUP_END) depth--;
            if (depth > 0) g_end++;
        }
        uint8_t quant = (g_end < p_end) ? tab->pat_elems[g_end].quant : QUANT_ONE;
        uint16_t next_p = (g_end < p_end) ? (g_end + 1) : p_end;

        if (quant == QUANT_OPT) {
            int sub_end = match_group_body(mem, tab, p_idx + 1, g_end, src, pos, len);
            if (sub_end >= 0) {
                int rem = match_pat_seq(mem, tab, next_p, p_end, src, (uint16_t)sub_end, len);
                if (rem >= 0) return rem;
            }
            return match_pat_seq(mem, tab, next_p, p_end, src, pos, len);
        } else if (quant == QUANT_ONE) {
            int sub_end = match_group_body(mem, tab, p_idx + 1, g_end, src, pos, len);
            if (sub_end < 0) return -1;
            return match_pat_seq(mem, tab, next_p, p_end, src, (uint16_t)sub_end, len);
        } else {
            uint16_t ends[64];
            int count = 0;
            if (quant == QUANT_STAR) ends[count++] = pos;
            uint16_t cur = pos;
            int matched_iters = 0;
            while (cur < len) {
                int sub_end = match_group_body(mem, tab, p_idx + 1, g_end, src, cur, len);
                if (sub_end <= (int)cur) break;
                cur = (uint16_t)sub_end;
                matched_iters++;
                if (count < 64) {
                    ends[count++] = cur;
                } else {
                    ends[count - 1] = cur;
                }
            }
            if (matched_iters == 0 && quant == QUANT_PLUS) return -1;
            int rem_full = match_pat_seq(mem, tab, next_p, p_end, src, cur, len);
            if (rem_full >= 0) return rem_full;
            for (int i = count - 1; i >= 0; --i) {
                if (ends[i] == cur) continue;
                int rem = match_pat_seq(mem, tab, next_p, p_end, src, ends[i], len);
                if (rem >= 0) return rem;
            }
            return -1;
        }
    }

    uint8_t quant = pe->quant;
    uint16_t next_p = p_idx + 1;

    if (quant == QUANT_ONE) {
        int step = match_single_atom(mem, pe, src, pos, len);
        if (step < 0) return -1;
        return match_pat_seq(mem, tab, next_p, p_end, src, pos + (uint16_t)step, len);
    } else if (quant == QUANT_OPT) {
        int step = match_single_atom(mem, pe, src, pos, len);
        if (step > 0) {
            int rem = match_pat_seq(mem, tab, next_p, p_end, src, pos + (uint16_t)step, len);
            if (rem >= 0) return rem;
        }
        return match_pat_seq(mem, tab, next_p, p_end, src, pos, len);
    } else {
        uint16_t ends[64];
        int count = 0;
        if (quant == QUANT_STAR) ends[count++] = pos;
        uint16_t cur = pos;
        int matched_iters = 0;
        while (cur < len) {
            int step = match_single_atom(mem, pe, src, cur, len);
            if (step <= 0) break;
            cur = (uint16_t)(cur + (uint16_t)step);
            matched_iters++;
            if (count < 64) {
                ends[count++] = cur;
            } else {
                ends[count - 1] = cur;
            }
        }
        if (matched_iters == 0 && quant == QUANT_PLUS) return -1;
        int rem_full = match_pat_seq(mem, tab, next_p, p_end, src, cur, len);
        if (rem_full >= 0) return rem_full;
        for (int i = count - 1; i >= 0; --i) {
            if (ends[i] == cur) continue;
            int rem = match_pat_seq(mem, tab, next_p, p_end, src, ends[i], len);
            if (rem >= 0) return rem;
        }
        return -1;
    }
}

static bool next_line_starts_branch_kw(const Lexer *lex, uint16_t nl_pos) {
    uint16_t p = nl_pos;
    while (p < lex->len) {
        char c = lex->src[p];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p++;
        else if (c == '#') {
            while (p < lex->len && lex->src[p] != '\n') p++;
        } else break;
    }
    if (p >= lex->len || !lex_is_ident_start((unsigned char)lex->src[p])) return false;
    uint16_t start = p;
    while (p < lex->len && lex_is_ident_cont((unsigned char)lex->src[p])) p++;
    uint16_t kw_len = (uint16_t)(p - start);
    if (kw_len == 5 && memcmp(lex->src + start, "catch", 5) == 0) return true;
    int sid = sym_find(lex->tab, lex->mem, lex->src + start, kw_len);
    if (sid < 0) return false;
    uint8_t m = sym_active_meta_tag(lex->tab, (uint16_t)sid, FIX_FUNC);
    return (m == INTR_OR_BRANCH || m == INTR_ELSE_BRANCH);
}

static bool skip_ws_and_comments(Lexer *lex, bool *out_had_nl) {
    bool saw_nl = false;
    while (lex->pos < lex->len) {
        char c = lex->src[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lex->pos++;
        } else if (c == '\n') {
            saw_nl = true;
            if (lex->stop_at_newline && lex->paren_depth == 0 &&
                !next_line_starts_branch_kw(lex, (uint16_t)(lex->pos + 1))) {
                if (out_had_nl) *out_had_nl = saw_nl;
                return true;
            }
            lex->pos++;
        } else if (c == '#') {
            while (lex->pos < lex->len && lex->src[lex->pos] != '\n') lex->pos++;
        } else {
            break;
        }
    }
    if (out_had_nl) *out_had_nl = saw_nl;
    return false;
}

void lex_init(Lexer *lex, MemArena *mem, SymTable *tab,
              const char *src, uint16_t len, uint16_t start_pos, bool stop_at_newline) {
    memset(lex, 0, sizeof(*lex));
    lex->mem             = mem;
    lex->tab             = tab;
    lex->src             = src;
    lex->len             = len;
    lex->pos             = start_pos;
    lex->stop_at_newline = stop_at_newline;
}

void lex_next(Lexer *lex) {
    MemArena       *mem = lex->mem;
    const SymTable *tab = lex->tab;
    bool had_nl = false;

    if (skip_ws_and_comments(lex, &had_nl)) {
        memset(&lex->cur, 0, sizeof(lex->cur));
        lex->cur.off         = lex->pos;
        lex->cur.kind        = TOK_EOF;
        lex->cur.had_newline = had_nl;
        return;
    }
    memset(&lex->cur, 0, sizeof(lex->cur));
    lex->cur.off         = lex->pos;
    lex->cur.had_newline = had_nl;

    if (lex->pos >= lex->len) {
        lex->cur.kind = TOK_EOF;
        return;
    }

    char c = lex->src[lex->pos];

    /* 1. Private C intrinsic / host extern hook '@' (only valid inside extension bodies) */
    if (c == '@') {
        lex->cur.kind = TOK_AT;
        lex->cur.len  = 1;
        lex->pos++;
        return;
    }

    /* 1b. Interpolated string literal `$"..."` or `f"..."` */
    if ((c == '$' || c == 'f') && lex->pos + 1 < lex->len &&
        lex->src[lex->pos + 1] == '"' && mem->str_type_id != 0) {
        uint16_t p = (uint16_t)(lex->pos + 2);
        int brace_depth = 0;
        while (p < lex->len) {
            char ch = lex->src[p];
            if (ch == '\\' && p + 1 < lex->len) {
                p = (uint16_t)(p + 2);
                continue;
            }
            if (ch == '{') {
                brace_depth++;
                p++;
                continue;
            }
            if (ch == '}' && brace_depth > 0) {
                brace_depth--;
                p++;
                continue;
            }
            if (ch == '"' && brace_depth == 0) {
                p++;
                break;
            }
            p++;
        }
        uint16_t q_start = (uint16_t)(lex->pos + 1);
        uint16_t q_len   = (uint16_t)(p - q_start);
        lex->cur.kind = TOK_VAL;
        lex->cur.len  = (uint16_t)(p - lex->pos);
        lex->cur.val  = delegate_construct_token(mem, tab, mem->str_type_id, lex->src, q_start, q_len);
        lex->pos      = p;
        lex->last_was_expr = true;
        return;
    }

    /* 2. Active Consuming Types with non-empty patterns (Longest Match wins) */
    int best_type_len = 0;
    uint16_t best_type_id = MEM_TYPE_VOID;

    for (uint16_t tid = 1; tid < tab->type_count; ++tid) {
        const TypeDef *td = &tab->types[tid];
        if (!td->is_consuming || td->field_count == 0 || !sym_mod_enabled(tab, td->mod_id)) continue;
        if ((td->reserved & SYM_TYPE_PRIVATE) != 0 && td->mod_id != tab->cur_mod) continue;

        uint16_t p_start = td->start_idx;
        uint16_t p_end   = p_start + td->field_count;

        int match_end = match_pat_seq(mem, tab, p_start, p_end, lex->src, lex->pos, lex->len);
        if (match_end > (int)lex->pos) {
            int mlen = match_end - (int)lex->pos;
            if (mlen > best_type_len) {
                best_type_len = mlen;
                best_type_id  = tid;
            }
        }
    }

    if (best_type_len > 0) {
        lex->cur.kind = TOK_VAL;
        lex->cur.len  = (uint16_t)best_type_len;
        lex->cur.val  = delegate_construct_token(mem, tab, best_type_id, lex->src, lex->pos, (uint16_t)best_type_len);
        lex->pos += (uint16_t)best_type_len;
        lex->last_was_expr = true;
        return;
    }

    /* 3. Active Syntax Delimiters (`(`, `)`, `{`, `}`, `[`, `]`, `,`) & Active Operators */
    int best_sym_len = 0;
    uint16_t best_sym_id = 0;
    uint8_t best_syntax_tag = 0;

    for (uint16_t sid = 0; sid < tab->sym_count; ++sid) {
        StrSlice s = tab->syms[sid];
        if (s.len == 0 || lex->pos + s.len > lex->len) continue;
        const char *s_ptr = mem_str_ptr(mem, s);
        if (s_ptr[0] != lex->src[lex->pos]) continue;
        uint8_t syn_tag = sym_get_active_syntax(tab, sid);
        bool    is_op   = sym_has_active_op(tab, sid);
        if (!syn_tag && !is_op) continue;
        if (memcmp(lex->src + lex->pos, s_ptr, s.len) == 0) {
            if (is_ascii_word(s_ptr, s.len)) {
                if (ident_continues_at(lex->src, (uint16_t)(lex->pos + s.len), lex->len)) {
                    continue;
                }
            }
            if ((int)s.len > best_sym_len) {
                best_sym_len    = (int)s.len;
                best_sym_id     = sid;
                best_syntax_tag = syn_tag;
            }
        }
    }

    /* Disambiguate `5--3` / `5++3`: when `--` or `++` (prefix/postfix only, no infix)
     * follows an expression and is immediately followed on the same line by a primary
     * operand starter (digit, `.digit`, identifier, `(`, or `√`), split off the leading
     * binary `-` or `+` so `5--3` parses as `5 - (-3)` while `6-- + 1` stays postfix `--`. */
    if (best_sym_len >= 2 && best_syntax_tag == 0 && lex->last_was_expr) {
        const char *op_s = mem_str_ptr(mem, tab->syms[best_sym_id]);
        bool all_minus = true, all_plus = true;
        for (int k = 0; k < best_sym_len; ++k) {
            if (op_s[k] != '-') all_minus = false;
            if (op_s[k] != '+') all_plus  = false;
        }
        if ((all_minus || all_plus) &&
            sym_get_active_prec(tab, best_sym_id, FIX_INFIX_LEFT) < 0 &&
            sym_get_active_prec(tab, best_sym_id, FIX_INFIX_RIGHT) < 0) {
            uint16_t look = (uint16_t)(lex->pos + (uint16_t)best_sym_len);
            while (look < lex->len && (lex->src[look] == ' ' || lex->src[look] == '\t')) look++;
            if (look < lex->len) {
                unsigned char nc = (unsigned char)lex->src[look];
                bool rhs_starts_primary = (nc >= '0' && nc <= '9') ||
                                          (nc == '.' && look + 1 < lex->len &&
                                           lex->src[look + 1] >= '0' && lex->src[look + 1] <= '9') ||
                                          nc == '(' ||
                                          lex_is_ident_start(nc);
                if (rhs_starts_primary) {
                    char single_ch = all_minus ? '-' : '+';
                    int single_sid = sym_find(tab, mem, &single_ch, 1);
                    if (single_sid >= 0 && sym_has_active_op(tab, (uint16_t)single_sid)) {
                        best_sym_len = 1;
                        best_sym_id  = (uint16_t)single_sid;
                    }
                }
            }
        }
    }

    if (best_sym_len > 0) {
        lex->cur.len    = (uint16_t)best_sym_len;
        lex->cur.sym_id = best_sym_id;
        lex->pos += (uint16_t)best_sym_len;

        if (best_syntax_tag == INTR_LPAREN) {
            lex->paren_depth++;
            lex->cur.kind = TOK_LPAREN;
            lex->last_was_expr = false;
        } else if (best_syntax_tag == INTR_RPAREN) {
            if (lex->paren_depth > 0) lex->paren_depth--;
            lex->cur.kind = TOK_RPAREN;
            lex->last_was_expr = true;
        } else if (best_syntax_tag == INTR_LBRACE) {
            lex->paren_depth++;
            lex->cur.kind = TOK_LBRACE;
            lex->last_was_expr = false;
        } else if (best_syntax_tag == INTR_RBRACE) {
            if (lex->paren_depth > 0) lex->paren_depth--;
            lex->cur.kind = TOK_RBRACE;
            lex->last_was_expr = true;
        } else if (best_syntax_tag == INTR_LBRACK) {
            lex->paren_depth++;
            lex->cur.kind = TOK_LBRACK;
            lex->last_was_expr = false;
        } else if (best_syntax_tag == INTR_RBRACK) {
            if (lex->paren_depth > 0) lex->paren_depth--;
            lex->cur.kind = TOK_RBRACK;
            lex->last_was_expr = true;
        } else if (best_syntax_tag == INTR_COMMA) {
            lex->cur.kind = TOK_COMMA;
            lex->last_was_expr = false;
        } else {
            lex->cur.kind = TOK_OP;
            bool has_infix = (sym_get_active_prec(tab, best_sym_id, FIX_INFIX_LEFT) >= 0) ||
                             (sym_get_active_prec(tab, best_sym_id, FIX_INFIX_RIGHT) >= 0);
            bool has_post  = (sym_get_active_prec(tab, best_sym_id, FIX_POSTFIX) >= 0);
            lex->last_was_expr = (has_post && !has_infix && lex->last_was_expr);
        }
        return;
    }

    /* 4. Identifiers (trailing '_' is reserved for the postfix '_' operator) */
    if (lex_is_ident_start((unsigned char)c)) {
        uint16_t start = lex->pos;
        while (lex->pos < lex->len && ident_continues_at(lex->src, lex->pos, lex->len)) {
            if ((unsigned char)lex->src[lex->pos] >= 0x80 && lex->pos > start) {
                bool sym_starts_here = false;
                for (uint16_t sid = 0; sid < tab->sym_count; ++sid) {
                    StrSlice s = tab->syms[sid];
                    if (s.len == 0 || lex->pos + s.len > lex->len) continue;
                    const char *s_ptr = mem_str_ptr(mem, s);
                    if (s_ptr[0] != lex->src[lex->pos]) continue;
                    if (!sym_has_active_op(tab, sid) && !sym_get_active_syntax(tab, sid)) continue;
                    if (memcmp(lex->src + lex->pos, s_ptr, s.len) == 0) {
                        sym_starts_here = true;
                        break;
                    }
                }
                if (sym_starts_here) break;
            }
            lex->pos++;
        }
        lex->cur.kind = TOK_IDENT;
        lex->cur.len  = lex->pos - start;
        lex->last_was_expr = true;
        return;
    }

    mem_err(mem, "Unknown token or undefined entity near '%.*s'",
            (int)(lex->len - lex->pos < 12 ? lex->len - lex->pos : 12), lex->src + lex->pos);
    lex->cur.kind = TOK_EOF;
}