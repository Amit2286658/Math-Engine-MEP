/*
 * delegate_numeric.c — String/Regex, Numeric, Bitwise, Complex & Numerical Calculus Delegates
 */
#include "delegate.h"
#include "../../Headers/parser.h"
#include "../hardcode/hardcode.h"
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Value delegate_eval_tostr(MemArena *mem, const SymTable *tab, Value val) {
    uint16_t tid = val_get_type(mem, val);
    if (tid == mem->str_type_id && tid != 0) {
        return val;
    }
    char buf[1024];
    delegate_format_value(mem, tab, val, buf, sizeof(buf));
    return val_from_cstr(mem, buf);
}

static Value delegate_eval_fromstr(MemArena *mem, SymTable *tab, Value str_val) {
    if (mem->str_type_id == 0 || val_get_type(mem, str_val) != mem->str_type_id) {
        mem_err(mem, "No compatible type found");
        return val_void();
    }
    StrSlice raw_slice = val_get_str(mem, str_val);
    char raw_buf[512];
    uint16_t copy_len = (raw_slice.len < sizeof(raw_buf) - 1)
                        ? raw_slice.len : (uint16_t)(sizeof(raw_buf) - 1);
    if (copy_len > 0) {
        memcpy(raw_buf, mem_str_ptr(mem, raw_slice), copy_len);
    }
    raw_buf[copy_len] = '\0';

    /* Trim leading and trailing whitespace */
    uint16_t start = 0;
    while (start < copy_len &&
           (raw_buf[start] == ' ' || raw_buf[start] == '\t' ||
            raw_buf[start] == '\r' || raw_buf[start] == '\n')) {
        start++;
    }
    uint16_t end = copy_len;
    while (end > start &&
           (raw_buf[end - 1] == ' ' || raw_buf[end - 1] == '\t' ||
            raw_buf[end - 1] == '\r' || raw_buf[end - 1] == '\n')) {
        end--;
    }

    const char *trimmed = raw_buf + start;
    uint16_t    tlen    = (uint16_t)(end - start);
    if (tlen == 0 || trimmed[0] == '"') {
        mem_err(mem, "No compatible type found for '%.*s'", (int)copy_len, raw_buf);
        return val_void();
    }
    for (uint16_t i = 0; i < tlen; ++i) {
        if (trimmed[i] == '\n') {
            mem_err(mem, "No compatible type found for '%.*s'", (int)copy_len, raw_buf);
            return val_void();
        }
    }

    /* Normalize struct `field=` labels inside `(...)` (e.g. `vec2(x=3, y=4)` -> `vec2(3, 4)`)
       and convert 1D bracketed lists `[a, b, ...]` (without `;` or nested `[` at depth 1) to `list(...)`
       when they represent vectors. */
    char norm_buf[512];
    uint16_t nlen = 0;
    int  pdepth = 0;
    bool at_arg_start = false;
    bool rewrite_1d_vec = false;

    if (tlen >= 2 && trimmed[0] == '{' && trimmed[tlen - 1] == '}') {
        uint16_t inner_p = 1;
        while (inner_p + 1 < tlen && (trimmed[inner_p] == ' ' || trimmed[inner_p] == '\t')) inner_p++;
        if (inner_p + 1 >= tlen) {
            uint16_t mhdr = mem_map_alloc_hdr(mem, 0, 0);
            return mem->has_error ? val_void() : val_make_comp(mem, mem->map_type_id, mhdr, MEM_MAP_HDR_SLOTS);
        }
        char dict_buf[512];
        size_t dw = (size_t)snprintf(dict_buf, sizeof(dict_buf), "dict(");
        bool in_q = false;
        int  d_nest = 0;
        for (uint16_t k = 1; k + 1 < tlen && dw + 2 < sizeof(dict_buf); ++k) {
            char ch = trimmed[k];
            if (ch == '"' && (k == 1 || trimmed[k - 1] != '\\')) in_q = !in_q;
            if (!in_q) {
                if (ch == '(' || ch == '[' || ch == '{') d_nest++;
                else if (ch == ')' || ch == ']' || ch == '}') d_nest--;
                else if (ch == ':' && d_nest == 0) {
                    dict_buf[dw++] = ',';
                    continue;
                }
            }
            dict_buf[dw++] = ch;
        }
        if (dw + 2 < sizeof(dict_buf)) {
            dict_buf[dw++] = ')';
            dict_buf[dw]   = '\0';
            uint16_t saved_sp2 = mem->sp;
            Parser MP;
            parser_init(&MP, mem, tab, dict_buf, (uint16_t)dw, 0, false, false);
            lex_next(&MP.lex);
            uint16_t mroot = parser_parse_expr(&MP, 0);
            if (!mem->has_error && mroot != MEM_NO_IDX && MP.lex.cur.kind == TOK_EOF) {
                Value mres = eval_ast(mem, tab, mroot, saved_sp2, 0);
                if (!mem->has_error && val_get_type(mem, mres) == mem->map_type_id) {
                    return mres;
                }
            }
            mem->sp = saved_sp2;
            mem_clear_err(mem);
        }
    }

    if (tlen >= 2 && trimmed[0] == '[' && trimmed[tlen - 1] == ']') {
        bool has_semi_or_sub = false;
        int  bdepth = 0;
        for (uint16_t i = 1; i + 1 < tlen; ++i) {
            char ch = trimmed[i];
            if (ch == '(' || ch == '{') bdepth++;
            else if (ch == ')' || ch == '}') bdepth--;
            else if (ch == '[' && bdepth == 0) { has_semi_or_sub = true; break; }
            else if (ch == ';' && bdepth == 0) { has_semi_or_sub = true; break; }
            else if (bdepth == 0 && lex_is_ident_start((unsigned char)ch) &&
                     (i == 1 || !lex_is_ident_cont((unsigned char)trimmed[i - 1]))) {
                uint16_t wend = i;
                while (wend + 1 < tlen && lex_is_ident_cont((unsigned char)trimmed[wend])) wend++;
                if (hardcode_is_for_kw(mem, tab, trimmed + i, (uint16_t)(wend - i))) {
                    has_semi_or_sub = true;
                    break;
                }
            }
        }
        if (!has_semi_or_sub) {
            rewrite_1d_vec = true;
        }
    }

    uint16_t i = 0;
    uint16_t limit = tlen;
    if (rewrite_1d_vec) {
        /* Empty `[]` -> `vec()`, non-empty `[a, b]` -> `list(a, b)` */
        uint16_t inner_p = 1;
        while (inner_p + 1 < tlen && (trimmed[inner_p] == ' ' || trimmed[inner_p] == '\t')) inner_p++;
        if (inner_p + 1 >= tlen) {
            memcpy(norm_buf, "vec()", 5);
            nlen = 5;
            i = tlen;
        } else {
            memcpy(norm_buf, "list(", 5);
            nlen = 5;
            pdepth = 1;
            at_arg_start = true;
            i = 1;
            limit = (uint16_t)(tlen - 1);
        }
    }

    while (i < limit && (size_t)nlen + 2u < sizeof(norm_buf)) {
        char c = trimmed[i];
        if (c == '(') {
            pdepth++;
            at_arg_start = true;
            norm_buf[nlen++] = c;
            i++;
            continue;
        }
        if (c == ')') {
            if (pdepth > 0) pdepth--;
            at_arg_start = false;
            norm_buf[nlen++] = c;
            i++;
            continue;
        }
        if (c == ',' && pdepth > 0) {
            at_arg_start = true;
            norm_buf[nlen++] = c;
            i++;
            continue;
        }
        if (pdepth > 0 && at_arg_start) {
            while (i < limit && (trimmed[i] == ' ' || trimmed[i] == '\t')) i++;
            if (i >= limit) break;
            unsigned char uc = (unsigned char)trimmed[i];
            if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z')) {
                uint16_t scan = i;
                while (scan < limit) {
                    unsigned char sc = (unsigned char)trimmed[scan];
                    if ((sc >= 'a' && sc <= 'z') || (sc >= 'A' && sc <= 'Z') ||
                        (sc >= '0' && sc <= '9') || sc == '_') {
                        scan++;
                    } else {
                        break;
                    }
                }
                uint16_t ws = scan;
                while (ws < limit && (trimmed[ws] == ' ' || trimmed[ws] == '\t')) ws++;
                if (ws < limit && trimmed[ws] == '=' &&
                    !(ws + 1 < limit && (trimmed[ws + 1] == '=' || trimmed[ws + 1] == '>'))) {
                    i = (uint16_t)(ws + 1);
                    continue;
                }
            }
            at_arg_start = false;
        }
        norm_buf[nlen++] = trimmed[i++];
    }
    if (rewrite_1d_vec && nlen > 0 && norm_buf[0] == 'l' && (size_t)nlen + 1u < sizeof(norm_buf)) {
        norm_buf[nlen++] = ')';
    }
    norm_buf[nlen] = '\0';

    uint16_t saved_sp = mem->sp;
    Parser P;
    parser_init(&P, mem, tab, norm_buf, nlen, 0, false, false);
    lex_next(&P.lex);

    uint16_t root = MEM_NO_IDX;
    if (!mem->has_error && P.lex.cur.kind != TOK_EOF) {
        root = parser_parse_expr(&P, 0);
    }

    if (mem->has_error || root == MEM_NO_IDX ||
        P.lex.cur.kind != TOK_EOF || P.vis_count != 0) {
        mem->sp = saved_sp;
        mem_clear_err(mem);
        mem_err(mem, "No compatible type found for '%.*s'", (int)copy_len, raw_buf);
        return val_void();
    }

    Value res = eval_ast(mem, tab, root, saved_sp, 0);
    uint16_t rtid = val_get_type(mem, res);
    if (mem->has_error || mem->jump_active != JUMP_NONE ||
        rtid == MEM_TYPE_VOID || rtid == mem->str_type_id) {
        mem->sp          = saved_sp;
        mem->jump_active = JUMP_NONE;
        mem_clear_err(mem);
        mem_err(mem, "No compatible type found for '%.*s'", (int)copy_len, raw_buf);
        return val_void();
    }
    return res;
}

/* ============================================================================
 * 5a. STRING FORMATTING, MANIPULATION & BACKTRACKING REGEX ENGINE DELEGATES
 * ============================================================================ */

static bool rx_match_here(const char *pat, uint16_t plen, const char *txt, uint16_t tlen,
                          uint16_t *out_end);

static bool rx_char_in_class(char c, const char *cls, uint16_t clen) {
    bool neg = (clen > 0 && cls[0] == '^');
    uint16_t i = neg ? 1 : 0;
    bool hit = false;
    while (i < clen) {
        if (cls[i] == '\\' && i + 1 < clen) {
            char esc = cls[i + 1];
            bool m = false;
            if (esc == 'd') m = (c >= '0' && c <= '9');
            else if (esc == 'w') m = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
            else if (esc == 's') m = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
            else m = (c == esc);
            if (m) hit = true;
            i = (uint16_t)(i + 2);
            continue;
        }
        if (i + 2 < clen && cls[i + 1] == '-') {
            if ((unsigned char)c >= (unsigned char)cls[i] &&
                (unsigned char)c <= (unsigned char)cls[i + 2]) {
                hit = true;
            }
            i = (uint16_t)(i + 3);
            continue;
        }
        if (c == cls[i]) hit = true;
        i++;
    }
    return neg ? !hit : hit;
}

static bool rx_atom_matches(const char *pat, uint16_t atom_len, char c) {
    if (atom_len == 1 && pat[0] == '.') return (c != '\n');
    if (atom_len == 2 && pat[0] == '\\') {
        char esc = pat[1];
        if (esc == 'd') return (c >= '0' && c <= '9');
        if (esc == 'D') return !(c >= '0' && c <= '9');
        if (esc == 'w') return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
        if (esc == 'W') return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
        if (esc == 's') return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (esc == 'S') return !(c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (esc == 'n') return (c == '\n');
        if (esc == 't') return (c == '\t');
        return (c == esc);
    }
    if (atom_len >= 2 && pat[0] == '[' && pat[atom_len - 1] == ']') {
        return rx_char_in_class(c, pat + 1, (uint16_t)(atom_len - 2));
    }
    return (atom_len == 1 && pat[0] == c);
}

static uint16_t rx_atom_span(const char *pat, uint16_t plen) {
    if (plen == 0) return 0;
    if (pat[0] == '\\' && plen >= 2) return 2;
    if (pat[0] == '[') {
        uint16_t i = 1;
        while (i < plen && pat[i] != ']') {
            if (pat[i] == '\\' && i + 1 < plen) i++;
            i++;
        }
        return (i < plen) ? (uint16_t)(i + 1) : plen;
    }
    if (pat[0] == '(') {
        int depth = 1;
        uint16_t i = 1;
        while (i < plen && depth > 0) {
            if (pat[i] == '\\' && i + 1 < plen) { i = (uint16_t)(i + 2); continue; }
            if (pat[i] == '(') depth++;
            else if (pat[i] == ')') depth--;
            i++;
        }
        return i;
    }
    return 1;
}

static bool rx_match_here(const char *pat, uint16_t plen, const char *txt, uint16_t tlen,
                          uint16_t *out_end) {
    /* Top-level alternation `branch1|branch2` */
    int depth = 0;
    bool in_cls = false;
    for (uint16_t i = 0; i < plen; ++i) {
        if (pat[i] == '\\' && i + 1 < plen) { i++; continue; }
        if (pat[i] == '[' && !in_cls) { in_cls = true; continue; }
        if (pat[i] == ']' && in_cls)  { in_cls = false; continue; }
        if (!in_cls) {
            if (pat[i] == '(') depth++;
            else if (pat[i] == ')') depth--;
            else if (pat[i] == '|' && depth == 0) {
                uint16_t e1 = 0;
                if (rx_match_here(pat, i, txt, tlen, &e1)) {
                    *out_end = e1;
                    return true;
                }
                return rx_match_here(pat + i + 1, (uint16_t)(plen - i - 1), txt, tlen, out_end);
            }
        }
    }

    if (plen == 0) {
        *out_end = 0;
        return true;
    }
    if (plen == 1 && pat[0] == '$') {
        if (tlen == 0) { *out_end = 0; return true; }
        return false;
    }

    uint16_t alen = rx_atom_span(pat, plen);
    char quant = (alen < plen && (pat[alen] == '*' || pat[alen] == '+' || pat[alen] == '?'))
                 ? pat[alen] : '\0';
    uint16_t rem_off = (quant != '\0') ? (uint16_t)(alen + 1) : alen;
    const char *rem_pat = pat + rem_off;
    uint16_t    rem_len = (uint16_t)(plen - rem_off);

    bool is_group = (alen >= 2 && pat[0] == '(' && pat[alen - 1] == ')');
    const char *g_pat = is_group ? (pat + 1) : pat;
    uint16_t    g_len = is_group ? (uint16_t)(alen - 2) : alen;

    if (quant == '\0') {
        if (is_group) {
            uint16_t g_end = 0;
            if (!rx_match_here(g_pat, g_len, txt, tlen, &g_end)) return false;
            uint16_t r_end = 0;
            if (!rx_match_here(rem_pat, rem_len, txt + g_end, (uint16_t)(tlen - g_end), &r_end)) return false;
            *out_end = (uint16_t)(g_end + r_end);
            return true;
        }
        if (tlen > 0 && rx_atom_matches(pat, alen, txt[0])) {
            uint16_t r_end = 0;
            if (rx_match_here(rem_pat, rem_len, txt + 1, (uint16_t)(tlen - 1), &r_end)) {
                *out_end = (uint16_t)(1 + r_end);
                return true;
            }
        }
        return false;
    }

    if (quant == '?') {
        if (is_group) {
            uint16_t g_end = 0;
            if (rx_match_here(g_pat, g_len, txt, tlen, &g_end)) {
                uint16_t r_end = 0;
                if (rx_match_here(rem_pat, rem_len, txt + g_end, (uint16_t)(tlen - g_end), &r_end)) {
                    *out_end = (uint16_t)(g_end + r_end);
                    return true;
                }
            }
        } else if (tlen > 0 && rx_atom_matches(pat, alen, txt[0])) {
            uint16_t r_end = 0;
            if (rx_match_here(rem_pat, rem_len, txt + 1, (uint16_t)(tlen - 1), &r_end)) {
                *out_end = (uint16_t)(1 + r_end);
                return true;
            }
        }
        return rx_match_here(rem_pat, rem_len, txt, tlen, out_end);
    }

    /* '*' or '+' quantifier (greedy with backtracking) */
    uint16_t starts[128];
    uint16_t scnt = 0;
    if (quant == '*') starts[scnt++] = 0;
    uint16_t cur = 0;
    while (cur < tlen && scnt < 128) {
        uint16_t step = 0;
        if (is_group) {
            if (!rx_match_here(g_pat, g_len, txt + cur, (uint16_t)(tlen - cur), &step) || step == 0) break;
        } else {
            if (!rx_atom_matches(pat, alen, txt[cur])) break;
            step = 1;
        }
        cur = (uint16_t)(cur + step);
        starts[scnt++] = cur;
    }
    for (int k = (int)scnt - 1; k >= 0; --k) {
        uint16_t r_end = 0;
        if (rx_match_here(rem_pat, rem_len, txt + starts[k], (uint16_t)(tlen - starts[k]), &r_end)) {
            *out_end = (uint16_t)(starts[k] + r_end);
            return true;
        }
    }
    return false;
}

static bool rx_search(const char *pat, uint16_t plen, const char *txt, uint16_t tlen,
                      uint16_t *out_start, uint16_t *out_len) {
    if (plen > 0 && pat[0] == '^') {
        uint16_t mend = 0;
        if (rx_match_here(pat + 1, (uint16_t)(plen - 1), txt, tlen, &mend)) {
            if (out_start) *out_start = 0;
            if (out_len)   *out_len   = mend;
            return true;
        }
        return false;
    }
    for (uint16_t i = 0; i <= tlen; ++i) {
        uint16_t mend = 0;
        if (rx_match_here(pat, plen, txt + i, (uint16_t)(tlen - i), &mend)) {
            if (out_start) *out_start = i;
            if (out_len)   *out_len   = mend;
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * 5. SCALAR ARITHMETIC, BOOLEAN LOGIC & TRANSCENDENTAL DELEGATES
 * ============================================================================ */

/*Below section is the C's native top-level dispatcher for operator/function delegates @add..@err, @tostr, @fromstr*/
/*Construct +, -, *, /, %, ^, √, sin..tanh, ==..~, &&..nand, type_of, unit_of, x_, _s, error*/
Value delegate_eval_intrinsic(MemArena *mem, SymTable *tab,
                              IntrinsicId intr, Value a, Value b, Value c, uint8_t ret_type) {
    if (delegate_is_vec_or_map_intrinsic(intr)) {
        return delegate_eval_vec_intrinsic(mem, tab, intr, a, b, c);
    }
    if (delegate_is_mat_intrinsic(intr)) {
        return delegate_eval_mat_intrinsic(mem, tab, intr, a, b, c);
    }

    /*Below section is the C's native for operator delegates @type_name, @unit_name, @tostr, @fromstr, @err*/
    /*Construct type_of(x), elem_type_of(x), type_sig_of(x), unit_of(x), x_ (postfix "_"), _s (prefix "_"), error(msg)*/
    if (intr == INTR_GC_COMPACT) {
        uint16_t reclaimed = mem_compact_scratch(mem, NULL, 0);
        return val_make_f64(mem, mem->fast_real_type, (double)reclaimed);
    }
    if (intr == INTR_POOL_USED) {
        uint16_t used = mem_scratch_pool_used(mem);
        return val_make_f64(mem, mem->fast_real_type, (double)used);
    }
    if (intr == INTR_STR_FMT) {
        char fmt_buf[512];
        if (!val_as_cstr(mem, a, fmt_buf, sizeof(fmt_buf))) {
            mem_err(mem, "fmt() first argument must be a format string");
            return val_void();
        }
        Value fmt_args[16];
        uint8_t n_fargs = 0;
        if (mem->vec_type_id != 0 && val_get_type(mem, b) == mem->vec_type_id &&
            val_get_type(mem, c) == MEM_TYPE_VOID) {
            uint16_t vhdr = val_get_pool_idx(b);
            uint16_t raw0 = (uint16_t)mem_read_pool(mem, vhdr + 0);
            uint16_t etid = (uint16_t)(raw0 & 0xFFu);
            uint8_t  euid = (uint8_t)((raw0 >> 8u) & 0x7Fu);
            uint16_t vlen = (uint16_t)mem_read_pool(mem, vhdr + 1);
            uint16_t vdat = (uint16_t)mem_read_pool(mem, vhdr + 3);
            uint16_t strd = (etid > 0 && etid < tab->type_count) ? tab->types[etid].flat_size : 1;
            for (uint16_t k = 0; k < vlen && n_fargs < 16; ++k) {
                fmt_args[n_fargs++] = delegate_vec_read_elem(mem, tab, etid, euid, vdat, (uint32_t)k * (uint32_t)strd);
            }
        } else {
            if (val_get_type(mem, b) != MEM_TYPE_VOID) fmt_args[n_fargs++] = b;
            if (val_get_type(mem, c) != MEM_TYPE_VOID) fmt_args[n_fargs++] = c;
        }
        char out[1024];
        size_t pos = 0;
        uint8_t auto_idx = 0;
        for (size_t i = 0; fmt_buf[i] != '\0' && pos + 1 < sizeof(out); ) {
            if (fmt_buf[i] == '{' && fmt_buf[i + 1] == '{') {
                out[pos++] = '{'; i += 2; continue;
            }
            if (fmt_buf[i] == '}' && fmt_buf[i + 1] == '}') {
                out[pos++] = '}'; i += 2; continue;
            }
            if (fmt_buf[i] == '{') {
                size_t close = i + 1;
                while (fmt_buf[close] != '\0' && fmt_buf[close] != '}') close++;
                if (fmt_buf[close] == '}') {
                    char spec[64];
                    size_t slen = close - (i + 1);
                    if (slen >= sizeof(spec)) slen = sizeof(spec) - 1;
                    memcpy(spec, fmt_buf + i + 1, slen);
                    spec[slen] = '\0';
                    uint8_t use_idx = auto_idx;
                    const char *colon = strchr(spec, ':');
                    if (colon != NULL) {
                        if (colon > spec) use_idx = (uint8_t)atoi(spec);
                        else auto_idx++;
                    } else if (slen > 0 && spec[0] >= '0' && spec[0] <= '9') {
                        use_idx = (uint8_t)atoi(spec);
                    } else {
                        auto_idx++;
                    }
                    if (use_idx < n_fargs) {
                        Value fv = fmt_args[use_idx];
                        char vbuf[256];
                        if (colon != NULL && colon[1] != '\0') {
                            const char *sp = colon + 1;
                            bool zero_pad = (*sp == '0');
                            int width = atoi(sp);
                            int prec = -1;
                            const char *dot = strchr(sp, '.');
                            if (dot) prec = atoi(dot + 1);
                            char conv = sp[strlen(sp) - 1];
                            if (conv == 'd' || conv == 'i') {
                                if (zero_pad && width > 0) snprintf(vbuf, sizeof(vbuf), "%0*lld", width, (long long)llround(val_get_f64(mem, fv)));
                                else if (width > 0) snprintf(vbuf, sizeof(vbuf), "%*lld", width, (long long)llround(val_get_f64(mem, fv)));
                                else snprintf(vbuf, sizeof(vbuf), "%lld", (long long)llround(val_get_f64(mem, fv)));
                            } else if (conv == 'x' || conv == 'X') {
                                unsigned long long uv = (unsigned long long)llround(val_get_f64(mem, fv));
                                if (conv == 'x') snprintf(vbuf, sizeof(vbuf), "%llx", uv);
                                else snprintf(vbuf, sizeof(vbuf), "%llX", uv);
                            } else if (conv == 'b') {
                                unsigned long long uv = (unsigned long long)llround(val_get_f64(mem, fv));
                                if (uv == 0) { snprintf(vbuf, sizeof(vbuf), "0"); }
                                else {
                                    char btmp[72]; int bi = 0;
                                    while (uv > 0 && bi < 64) { btmp[bi++] = (char)('0' + (uv & 1ULL)); uv >>= 1ULL; }
                                    int oi = 0; while (bi > 0) vbuf[oi++] = btmp[--bi];
                                    vbuf[oi] = '\0';
                                }
                            } else if (conv == 'e') {
                                snprintf(vbuf, sizeof(vbuf), "%.*e", (prec >= 0) ? prec : 6, val_get_f64(mem, fv));
                            } else if (conv == 'g') {
                                snprintf(vbuf, sizeof(vbuf), "%.*g", (prec > 0) ? prec : 6, val_get_f64(mem, fv));
                            } else {
                                snprintf(vbuf, sizeof(vbuf), "%.*f", (prec >= 0) ? prec : 6, val_get_f64(mem, fv));
                            }
                        } else {
                            Value sv = delegate_eval_tostr(mem, tab, fv);
                            val_as_cstr(mem, sv, vbuf, sizeof(vbuf));
                        }
                        size_t vl = strlen(vbuf);
                        if (pos + vl >= sizeof(out)) vl = sizeof(out) - 1 - pos;
                        memcpy(out + pos, vbuf, vl);
                        pos += vl;
                    } else {
                        /* Preserve unconsumed placeholder for chained multi-arg @str_fmt calls */
                        size_t raw_l = (close - i) + 1;
                        if (pos + raw_l < sizeof(out)) {
                            memcpy(out + pos, fmt_buf + i, raw_l);
                            pos += raw_l;
                        }
                    }
                    i = close + 1;
                    continue;
                }
            }
            if (fmt_buf[i] == '%' && fmt_buf[i + 1] != '\0') {
                if (fmt_buf[i + 1] == '%') {
                    out[pos++] = '%'; i += 2; continue;
                }
                size_t j = i + 1;
                while (fmt_buf[j] != '\0' &&
                       (fmt_buf[j] == '-' || fmt_buf[j] == '+' || fmt_buf[j] == '0' ||
                        fmt_buf[j] == '.' || (fmt_buf[j] >= '0' && fmt_buf[j] <= '9'))) {
                    j++;
                }
                char conv = fmt_buf[j];
                if (conv == 'f' || conv == 'g' || conv == 'e' || conv == 'd' || conv == 'i' ||
                    conv == 'x' || conv == 'X' || conv == 's') {
                    char cfmt[32];
                    size_t flen = (j - i);
                    if (flen >= sizeof(cfmt)) flen = sizeof(cfmt) - 1;
                    memcpy(cfmt, fmt_buf + i + 1, flen - 1);
                    cfmt[flen - 1] = '\0';
                    bool zero_pad = (cfmt[0] == '0');
                    int width = atoi(cfmt);
                    int prec = -1;
                    const char *dot = strchr(cfmt, '.');
                    if (dot) prec = atoi(dot + 1);
                    if (auto_idx < n_fargs) {
                        Value fv = fmt_args[auto_idx++];
                        char vbuf[256];
                        if (conv == 's') {
                            Value sv = delegate_eval_tostr(mem, tab, fv);
                            char sraw[200];
                            val_as_cstr(mem, sv, sraw, sizeof(sraw));
                            if (width != 0) snprintf(vbuf, sizeof(vbuf), "%*s", width, sraw);
                            else snprintf(vbuf, sizeof(vbuf), "%s", sraw);
                        } else if (conv == 'd' || conv == 'i') {
                            long long iv = (long long)llround(val_get_f64(mem, fv));
                            if (zero_pad && width > 0) snprintf(vbuf, sizeof(vbuf), "%0*lld", width, iv);
                            else if (width != 0) snprintf(vbuf, sizeof(vbuf), "%*lld", width, iv);
                            else snprintf(vbuf, sizeof(vbuf), "%lld", iv);
                        } else if (conv == 'x' || conv == 'X') {
                            unsigned long long uv = (unsigned long long)llround(val_get_f64(mem, fv));
                            if (conv == 'x') snprintf(vbuf, sizeof(vbuf), "%llx", uv);
                            else snprintf(vbuf, sizeof(vbuf), "%llX", uv);
                        } else if (conv == 'e') {
                            snprintf(vbuf, sizeof(vbuf), "%.*e", (prec >= 0) ? prec : 6, val_get_f64(mem, fv));
                        } else if (conv == 'g') {
                            snprintf(vbuf, sizeof(vbuf), "%.*g", (prec > 0) ? prec : 6, val_get_f64(mem, fv));
                        } else {
                            if (width > 0) snprintf(vbuf, sizeof(vbuf), "%*.*f", width, (prec >= 0) ? prec : 6, val_get_f64(mem, fv));
                            else snprintf(vbuf, sizeof(vbuf), "%.*f", (prec >= 0) ? prec : 6, val_get_f64(mem, fv));
                        }
                        size_t vl = strlen(vbuf);
                        if (pos + vl >= sizeof(out)) vl = sizeof(out) - 1 - pos;
                        memcpy(out + pos, vbuf, vl);
                        pos += vl;
                    } else {
                        size_t raw_l = (j - i) + 1;
                        if (pos + raw_l < sizeof(out)) {
                            memcpy(out + pos, fmt_buf + i, raw_l);
                            pos += raw_l;
                        }
                    }
                    i = j + 1;
                    continue;
                }
            }
            out[pos++] = fmt_buf[i++];
        }
        out[pos] = '\0';
        return val_from_cstr(mem, out);
    }
    if (intr == INTR_REGEX_MATCH || intr == INTR_REGEX_FIND ||
        intr == INTR_REGEX_FINDALL || intr == INTR_REGEX_REPLACE || intr == INTR_REGEX_SPLIT) {
        char txt[512], pat[256];
        if (!val_as_cstr(mem, a, txt, sizeof(txt)) || !val_as_cstr(mem, b, pat, sizeof(pat))) {
            mem_err(mem, "Regex functions require string arguments (text, pattern)");
            return val_void();
        }
        uint16_t tlen = (uint16_t)strlen(txt);
        uint16_t plen = (uint16_t)strlen(pat);
        if (intr == INTR_REGEX_MATCH) {
            uint16_t mstart = 0, mlen = 0;
            bool ok = rx_search(pat, plen, txt, tlen, &mstart, &mlen);
            return val_make_f64(mem, mem->fast_real_type, ok ? 1.0 : 0.0);
        }
        if (intr == INTR_REGEX_FIND) {
            uint16_t mstart = 0, mlen = 0;
            if (rx_search(pat, plen, txt, tlen, &mstart, &mlen)) {
                StrSlice ms = mem_intern_str(mem, txt + mstart, mlen);
                return val_make_str(mem, mem->str_type_id, ms);
            }
            return val_from_cstr(mem, "");
        }
        if (intr == INTR_REGEX_FINDALL) {
            uint16_t hdr = mem_vec_alloc_hdr(mem, mem->str_type_id, 4);
            if (mem->has_error) return val_void();
            uint16_t pos = 0;
            while (pos <= tlen) {
                uint16_t mstart = 0, mlen = 0;
                if (!rx_search(pat, plen, txt + pos, (uint16_t)(tlen - pos), &mstart, &mlen)) break;
                StrSlice ms = mem_intern_str(mem, txt + pos + mstart, mlen);
                Value mv = val_make_str(mem, mem->str_type_id, ms);
                uint16_t cur_len = (uint16_t)mem_read_pool(mem, hdr + 1);
                if (!mem_vec_reserve(mem, hdr, 1, (uint16_t)(cur_len + 1))) return val_void();
                uint16_t dat = (uint16_t)mem_read_pool(mem, hdr + 3);
                delegate_vec_write_elem(mem, tab, mem->str_type_id, dat, cur_len, mv);
                mem_write_pool(mem, hdr + 1, (double)(cur_len + 1));
                pos = (uint16_t)(pos + mstart + (mlen > 0 ? mlen : 1));
            }
            return val_make_comp(mem, mem->vec_type_id, hdr, MEM_VEC_HDR_SLOTS);
        }
        if (intr == INTR_REGEX_SPLIT) {
            uint16_t hdr = mem_vec_alloc_hdr(mem, mem->str_type_id, 4);
            if (mem->has_error) return val_void();
            uint16_t pos = 0;
            while (pos <= tlen) {
                uint16_t mstart = 0, mlen = 0;
                if (!rx_search(pat, plen, txt + pos, (uint16_t)(tlen - pos), &mstart, &mlen) || mlen == 0) {
                    StrSlice ms = mem_intern_str(mem, txt + pos, (uint16_t)(tlen - pos));
                    Value mv = val_make_str(mem, mem->str_type_id, ms);
                    uint16_t cur_len = (uint16_t)mem_read_pool(mem, hdr + 1);
                    if (!mem_vec_reserve(mem, hdr, 1, (uint16_t)(cur_len + 1))) return val_void();
                    uint16_t dat = (uint16_t)mem_read_pool(mem, hdr + 3);
                    delegate_vec_write_elem(mem, tab, mem->str_type_id, dat, cur_len, mv);
                    mem_write_pool(mem, hdr + 1, (double)(cur_len + 1));
                    break;
                }
                StrSlice ms = mem_intern_str(mem, txt + pos, mstart);
                Value mv = val_make_str(mem, mem->str_type_id, ms);
                uint16_t cur_len = (uint16_t)mem_read_pool(mem, hdr + 1);
                if (!mem_vec_reserve(mem, hdr, 1, (uint16_t)(cur_len + 1))) return val_void();
                uint16_t dat = (uint16_t)mem_read_pool(mem, hdr + 3);
                delegate_vec_write_elem(mem, tab, mem->str_type_id, dat, cur_len, mv);
                mem_write_pool(mem, hdr + 1, (double)(cur_len + 1));
                pos = (uint16_t)(pos + mstart + mlen);
            }
            return val_make_comp(mem, mem->vec_type_id, hdr, MEM_VEC_HDR_SLOTS);
        }
        if (intr == INTR_REGEX_REPLACE) {
            char rep[256];
            if (!val_as_cstr(mem, c, rep, sizeof(rep))) rep[0] = '\0';
            size_t rlen = strlen(rep);
            char out[1024];
            size_t out_p = 0;
            uint16_t pos = 0;
            while (pos <= tlen && out_p + 1 < sizeof(out)) {
                uint16_t mstart = 0, mlen = 0;
                if (!rx_search(pat, plen, txt + pos, (uint16_t)(tlen - pos), &mstart, &mlen) || mlen == 0) {
                    size_t rem = (size_t)(tlen - pos);
                    if (out_p + rem >= sizeof(out)) rem = sizeof(out) - 1 - out_p;
                    memcpy(out + out_p, txt + pos, rem);
                    out_p += rem;
                    break;
                }
                size_t pre = mstart;
                if (out_p + pre >= sizeof(out)) pre = sizeof(out) - 1 - out_p;
                memcpy(out + out_p, txt + pos, pre);
                out_p += pre;
                size_t nw = rlen;
                if (out_p + nw >= sizeof(out)) nw = sizeof(out) - 1 - out_p;
                memcpy(out + out_p, rep, nw);
                out_p += nw;
                pos = (uint16_t)(pos + mstart + mlen);
            }
            out[out_p] = '\0';
            return val_from_cstr(mem, out);
        }
    }

    if (intr == INTR_TYPE_NAME) {
        int mode = 0;
        if (val_get_type(mem, b) != MEM_TYPE_VOID) {
            mode = (int)val_get_f64(mem, b);
        }
        uint16_t atid = val_get_type(mem, a);
        char tbuf[96];
        if (mode == 1) {
            /* `elem_type_of(x)`: static element type (inferred default `real` when untyped/empty) */
            uint16_t etid = atid;
            if ((atid == mem->vec_type_id && mem->vec_type_id != 0) ||
                (atid == mem->map_type_id && mem->map_type_id != 0)) {
                uint16_t raw0 = (uint16_t)mem_read_pool(mem, (uint16_t)(val_get_pool_idx(a) + 0));
                etid = (uint16_t)(raw0 & 0xFFu);
                if (etid == 0) etid = mem->fast_real_type;
            } else if (atid == mem->mat_type_id && mem->mat_type_id != 0) {
                etid = mem->fast_real_type;
            }
            if (etid == SYM_ELEM_ANY || etid == SYM_TYPE_ANY) snprintf(tbuf, sizeof(tbuf), "any"); else sym_type_name(tab, mem, etid, tbuf, sizeof(tbuf));
            return val_from_cstr(mem, tbuf);
        }
        if (mode == 2) {
            /* `type_sig_of(x)`: full static type signature (`vec<real>`, `vec<real, length>`, `vec<vec2>`, `map<real>`) */
            if ((atid == mem->vec_type_id && mem->vec_type_id != 0) ||
                (atid == mem->map_type_id && mem->map_type_id != 0)) {
                uint16_t raw0 = (uint16_t)mem_read_pool(mem, (uint16_t)(val_get_pool_idx(a) + 0));
                uint16_t etid = (uint16_t)(raw0 & 0xFFu);
                uint8_t  euid = (uint8_t)((raw0 >> 8u) & 0x7Fu);
                if (etid == 0) etid = mem->fast_real_type;
                char oname[20], ename[20], uname[24];
                sym_type_name(tab, mem, atid, oname, sizeof(oname));
                if (etid == SYM_ELEM_ANY || etid == SYM_TYPE_ANY) snprintf(ename, sizeof(ename), "any"); else sym_type_name(tab, mem, etid, ename, sizeof(ename));
                if (euid != 0 && euid < SYM_UNIT_LOCK_NONE) {
                    sym_unit_name(tab, mem, euid, uname, sizeof(uname));
                    snprintf(tbuf, sizeof(tbuf), "%s<%s, %s>", oname, ename, uname);
                } else {
                    snprintf(tbuf, sizeof(tbuf), "%s<%s>", oname, ename);
                }
                return val_from_cstr(mem, tbuf);
            }
            if (atid == mem->mat_type_id && mem->mat_type_id != 0) {
                uint8_t muid = val_get_unit(mem, a);
                char oname[20], ename[20], uname[24];
                sym_type_name(tab, mem, atid, oname, sizeof(oname));
                sym_type_name(tab, mem, mem->fast_real_type, ename, sizeof(ename));
                if (muid != 0 && muid < SYM_UNIT_LOCK_NONE) {
                    sym_unit_name(tab, mem, muid, uname, sizeof(uname));
                    snprintf(tbuf, sizeof(tbuf), "%s<%s, %s>", oname, ename, uname);
                } else {
                    snprintf(tbuf, sizeof(tbuf), "%s<%s>", oname, ename);
                }
                return val_from_cstr(mem, tbuf);
            }
            uint8_t suid = val_get_unit(mem, a);
            char oname[24], uname[24];
            sym_type_name(tab, mem, atid, oname, sizeof(oname));
            if (suid != 0 && suid < SYM_UNIT_LOCK_NONE) {
                sym_unit_name(tab, mem, suid, uname, sizeof(uname));
                snprintf(tbuf, sizeof(tbuf), "%s, %s", oname, uname);
            } else {
                snprintf(tbuf, sizeof(tbuf), "%s", oname);
            }
            return val_from_cstr(mem, tbuf);
        }
        sym_type_name(tab, mem, atid, tbuf, sizeof(tbuf));
        return val_from_cstr(mem, tbuf);
    }
    if (intr == INTR_UNIT_NAME) {
        uint8_t uid = val_get_unit(mem, a);
        if (uid == MEM_UNIT_NONE) {
            uint16_t atid = val_get_type(mem, a);
            if ((atid == mem->vec_type_id && mem->vec_type_id != 0) ||
                (atid == mem->map_type_id && mem->map_type_id != 0)) {
                uint16_t raw0 = (uint16_t)mem_read_pool(mem, (uint16_t)(val_get_pool_idx(a) + 0));
                uid = (uint8_t)((raw0 >> 8u) & 0x7Fu);
            }
        }
        if (val_get_type(mem, b) != MEM_TYPE_VOID) {
            int ax = 0;
            if (!delegate_val_to_exact_int(mem, tab, b, "SI dimension axis", &ax)) {
                return val_void();
            }
            if (ax < 0) ax += 7;
            if (ax < 0 || ax >= 7) {
                mem_err(mem, "SI dimension axis %d out of bounds (expected 0..6 for m,kg,s,A,K,mol,cd)", ax);
                return val_void();
            }
            int8_t e = (uid < SYM_MAX_UNITS && tab->unit_dims[uid].has_dim)
                       ? tab->unit_dims[uid].exp[ax] : 0;
            return val_make_f64(mem, mem->fast_real_type, (double)e);
        }
        char ubuf[32];
        sym_unit_name(tab, mem, uid, ubuf, sizeof(ubuf));
        return val_from_cstr(mem, ubuf);
    }
    if (intr == INTR_TOSTR) {
        return delegate_eval_tostr(mem, tab, a);
    }
    if (intr == INTR_FROMSTR) {
        return delegate_eval_fromstr(mem, tab, a);
    }
    if (intr == INTR_ERR) {
        char ebuf[120];
        if (val_as_cstr(mem, a, ebuf, sizeof(ebuf)) && ebuf[0] != '\0') {
            mem_err(mem, "%s", ebuf);
        } else {
            mem_err(mem, "Runtime error raised by error()");
        }
        return val_void();
    }

    uint16_t rtid = (ret_type != 0 && ret_type != mem->str_type_id && ret_type < tab->type_count && tab->types[ret_type].is_consuming)
                    ? ret_type
                    : mem->fast_real_type;
    uint8_t  ua   = val_get_unit(mem, a);
    uint8_t  ub   = (val_get_type(mem, b) != MEM_TYPE_VOID) ? val_get_unit(mem, b) : MEM_UNIT_NONE;
    uint8_t  ruid = MEM_UNIT_NONE;

    double da  = val_get_f64(mem, a);
    double db  = (val_get_type(mem, b) != MEM_TYPE_VOID) ? val_get_f64(mem, b) : 0.0;
    double res = 0.0;

    switch (intr) {
        /*Below section is the C's native for operator delegates @add, @sub, @mul, @div, @mod, @neg, @abs, @floor, @ceil, @pow, @sqrt, @cbrt, @exp, @ln, @log10*/
        /*Construct +, -, *, /, %, unary -, abs, floor, ceil, ^, √ / sqrt, cbrt, exp, ln, log10*/
        case INTR_ADD:
            if (val_get_type(mem, a) == mem->str_type_id && val_get_type(mem, b) == mem->str_type_id && mem->str_type_id != 0) {
                StrSlice sa = val_get_str(mem, a);
                StrSlice sb = val_get_str(mem, b);
                uint32_t tot_len = (uint32_t)sa.len + (uint32_t)sb.len;
                if (tot_len >= (uint32_t)MEM_SRC_BUF_CAP) {
                    mem_err(mem, "Concatenated string length (%u) exceeds string arena capacity (%u)",
                            (unsigned)tot_len, (unsigned)MEM_SRC_BUF_CAP);
                    return val_void();
                }
                char stack_sbuf[1024];
                char *sbuf = stack_sbuf;
                if (tot_len >= sizeof(stack_sbuf)) {
                    sbuf = (char *)malloc(tot_len + 1u);
                    if (!sbuf) {
                        mem_err(mem, "Out of memory concatenating strings");
                        return val_void();
                    }
                }
                if (sa.len > 0) memcpy(sbuf, mem_str_ptr(mem, sa), sa.len);
                if (sb.len > 0) memcpy(sbuf + sa.len, mem_str_ptr(mem, sb), sb.len);
                StrSlice sc = mem_intern_str(mem, sbuf, (uint16_t)tot_len);
                if (sbuf != stack_sbuf) free(sbuf);
                if (mem->has_error) return val_void();
                return val_make_str(mem, mem->str_type_id, sc);
            }
            if (!delegate_check_additive_unit_compat(mem, tab, "+", 1, a, b, &ruid)) return val_void();
            res = da + db;
            break;
        case INTR_SUB:
            if (!delegate_check_additive_unit_compat(mem, tab, "-", 1, a, b, &ruid)) return val_void();
            res = da - db;
            break;
        case INTR_MUL:
            ruid = delegate_resolve_mul_unit(mem, tab, ua, ub);
            res = da * db;
            break;
        case INTR_DIV:
            if (db == 0.0) { mem_err(mem, "Division by zero"); return val_void(); }
            ruid = delegate_resolve_div_unit(mem, tab, ua, ub);
            res = da / db;
            break;
        case INTR_MOD:
            if (db == 0.0) { mem_err(mem, "Division by zero in modulo"); return val_void(); }
            if (ua != ub && ub != MEM_UNIT_NONE) {
                if (!delegate_check_additive_unit_compat(mem, tab, "%", 1, a, b, &ruid)) return val_void();
            } else {
                ruid = ua;
            }
            res = fmod(da, db);
            break;
        case INTR_NEG:   ruid = ua; res = -da; break;
        case INTR_ABS:   ruid = ua; res = fabs(da); break;
        case INTR_FLOOR: ruid = ua; res = floor(da); break;
        case INTR_CEIL:  ruid = ua; res = ceil(da); break;
        case INTR_POW:   ruid = delegate_resolve_pow_unit(mem, tab, ua, ub, db); res = pow(da, db); break;
        case INTR_SQRT:  ruid = delegate_resolve_root_unit(mem, tab, ua, 2); res = sqrt(da); break;
        case INTR_CBRT:  ruid = delegate_resolve_root_unit(mem, tab, ua, 3); res = cbrt(da); break;
        case INTR_EXP:   res = exp(da); break;
        case INTR_LN:    res = log(da); break;

        /*Below section is the C's native for operator delegates @sin, @cos, @tan, @asin, @acos, @atan, @atan2, @sinh, @cosh, @tanh*/
        /*Construct sin, cos, tan, asin, acos, atan, atan2, sinh, cosh, tanh*/
        case INTR_SIN:   res = sin(da); break;
        case INTR_COS:   res = cos(da); break;
        case INTR_TAN:   res = tan(da); break;
        case INTR_ASIN:  res = asin(da); break;
        case INTR_ACOS:  res = acos(da); break;
        case INTR_ATAN:  res = atan(da); break;
        case INTR_ATAN2: res = atan2(da, db); break;
        case INTR_ERF:    res = erf(da); break;
        case INTR_GAMMA:  res = tgamma(da); break;
        case INTR_LGAMMA: res = lgamma(da); break;

        /*Below section is the C's native for operator delegates @eq, @ne, @lt, @le, @gt, @ge, @approx, @land, @lor, @cnot, @lnot, @lxor, @lnor, @lnand, @raw*/
        /*Construct ==, !=, <, <=, >, >=, ~, && / and, || / or, !, not, xor / ^^, nor, nand, i (iota)*/
        case INTR_EQ:
            if (val_get_type(mem, a) == mem->str_type_id && val_get_type(mem, b) == mem->str_type_id && mem->str_type_id != 0) {
                res = mem_slice_eq(mem, val_get_str(mem, a), val_get_str(mem, b)) ? 1.0 : 0.0;
                break;
            }
            if (!delegate_check_additive_unit_compat(mem, tab, "==", 2, a, b, NULL)) return val_void();
            if (isnan(da) || isnan(db)) {
                res = 0.0;
            } else if (isinf(da) || isinf(db)) {
                res = (da == db) ? 1.0 : 0.0;
            } else if (da == 0.0 || db == 0.0) {
                res = (da == db) ? 1.0 : 0.0;
            } else {
                double scale = fmax(fabs(da), fabs(db));
                res = (fabs(da - db) <= 1e-14 * scale) ? 1.0 : 0.0;
            }
            break;
        case INTR_NE:
            if (val_get_type(mem, a) == mem->str_type_id && val_get_type(mem, b) == mem->str_type_id && mem->str_type_id != 0) {
                res = mem_slice_eq(mem, val_get_str(mem, a), val_get_str(mem, b)) ? 0.0 : 1.0;
                break;
            }
            if (!delegate_check_additive_unit_compat(mem, tab, "!=", 2, a, b, NULL)) return val_void();
            if (isnan(da) || isnan(db)) {
                res = 1.0;
            } else if (isinf(da) || isinf(db)) {
                res = (da != db) ? 1.0 : 0.0;
            } else if (da == 0.0 || db == 0.0) {
                res = (da != db) ? 1.0 : 0.0;
            } else {
                double scale = fmax(fabs(da), fabs(db));
                res = (fabs(da - db) > 1e-14 * scale) ? 1.0 : 0.0;
            }
            break;
        case INTR_LT:
        case INTR_LE:
        case INTR_GT:
        case INTR_GE:
            if (val_get_type(mem, a) == mem->str_type_id && val_get_type(mem, b) == mem->str_type_id && mem->str_type_id != 0) {
                StrSlice sa = val_get_str(mem, a);
                StrSlice sb = val_get_str(mem, b);
                uint16_t ml = (sa.len < sb.len) ? sa.len : sb.len;
                int cmp = (ml > 0) ? memcmp(mem_str_ptr(mem, sa), mem_str_ptr(mem, sb), ml) : 0;
                if (cmp == 0) cmp = (int)sa.len - (int)sb.len;
                if (intr == INTR_LT)      res = (cmp <  0) ? 1.0 : 0.0;
                else if (intr == INTR_LE) res = (cmp <= 0) ? 1.0 : 0.0;
                else if (intr == INTR_GT) res = (cmp >  0) ? 1.0 : 0.0;
                else                      res = (cmp >= 0) ? 1.0 : 0.0;
                break;
            }
            if (intr == INTR_LT) {
                if (!delegate_check_additive_unit_compat(mem, tab, "<", 1, a, b, NULL)) return val_void();
                res = (da <  db) ? 1.0 : 0.0;
            } else if (intr == INTR_LE) {
                if (!delegate_check_additive_unit_compat(mem, tab, "<=", 2, a, b, NULL)) return val_void();
                res = (da <= db) ? 1.0 : 0.0;
            } else if (intr == INTR_GT) {
                if (!delegate_check_additive_unit_compat(mem, tab, ">", 1, a, b, NULL)) return val_void();
                res = (da >  db) ? 1.0 : 0.0;
            } else {
                if (!delegate_check_additive_unit_compat(mem, tab, ">=", 2, a, b, NULL)) return val_void();
                res = (da >= db) ? 1.0 : 0.0;
            }
            break;
        case INTR_CNOT:
            if (fabs(da) <= 1e-15) {
                res = 1.0;
            } else if (fabs(da - 1.0) <= 1e-15) {
                res = 0.0;
            } else {
                mem_err(mem, "Operand %.6g out of range for boolean negation '!': expected 0 or 1", da);
                return val_void();
            }
            break;
        case INTR_BAND:
        case INTR_BOR:
        case INTR_BXOR:
        case INTR_BNOT:
        case INTR_BSHL:
        case INTR_BSHR: {
            if (ua != MEM_UNIT_NONE || (intr != INTR_BNOT && ub != MEM_UNIT_NONE)) {
                mem_err(mem, "Bitwise operation requires unitless integer operands");
                return val_void();
            }
            double ra = floor(da + 0.5);
            if (!isfinite(da) || da < -9007199254740991.0 || da > 9007199254740991.0 || fabs(da - ra) > 1e-9) {
                mem_err(mem, "Bitwise operation requires an exact integer operand (got %.6g)", da);
                return val_void();
            }
            int64_t ia = (int64_t)ra;
            if (intr == INTR_BNOT) {
                res = (double)(~ia);
                break;
            }
            double rb = floor(db + 0.5);
            if (!isfinite(db) || db < -9007199254740991.0 || db > 9007199254740991.0 || fabs(db - rb) > 1e-9) {
                mem_err(mem, "Bitwise operation requires an exact integer operand (got %.6g)", db);
                return val_void();
            }
            int64_t ib = (int64_t)rb;
            if (intr == INTR_BSHL || intr == INTR_BSHR) {
                if (ib < 0 || ib > 52) {
                    mem_err(mem, "Bit shift count %lld out of range (0..52)", (long long)ib);
                    return val_void();
                }
                res = (intr == INTR_BSHL)
                      ? (double)((int64_t)((uint64_t)ia << (unsigned)ib))
                      : (double)(ia >> (unsigned)ib);
            } else if (intr == INTR_BAND) {
                res = (double)(ia & ib);
            } else if (intr == INTR_BOR) {
                res = (double)(ia | ib);
            } else {
                res = (double)(ia ^ ib);
            }
            break;
        }
        case INTR_RAW:
            if (mem->str_type_id != 0 && val_get_type(mem, a) == mem->str_type_id) return a;
            ruid = (val_get_type(mem, b) != MEM_TYPE_VOID) ? ub : ua;
            res  = da;
            break;
        default:         mem_err(mem, "Unhandled intrinsic %d", (int)intr); return val_void();
    }
    return val_make_f64_unit(mem, rtid, ruid, res);
}

/* ============================================================================
 * 6. CALCULUS, SOLVER & EXTREMA DELEGATES (`@calc_diff` .. `@calc_fmax`)
 * ============================================================================ */

/*Below section is the C's native for operator/function delegates @calc_diff, @calc_diff2, @calc_diffn, @calc_integ, @calc_lim, @calc_lim_l, @calc_lim_r, @calc_sigma, @calc_prod, @calc_solve, @calc_fmin, @calc_fmax*/
/*Construct diff, diff2, diff_n, integrate, ntg, lim, lim_left, lim_right, sigma, prod, solve, fmin, fmax*/

/* ============================================================================
 * 6a. SYMBOLIC DIFFERENTIATION, ALGEBRAIC SIMPLIFICATION & TAYLOR EXPANSION
 * ============================================================================ */
