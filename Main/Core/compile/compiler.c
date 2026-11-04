/*
 * compiler.c — In-Memory `.math` Compilation Unit Stager & Top-to-Bottom Module Evaluator
 *
 * Implements Phase 1 (Compilation & Execution) of `.math` modules:
 *   - `ext_build_init` / `ext_build_create` / `ext_build_destroy` / `ext_compile_unit`:
 *     Scans raw `.math` source strings, splits them into `[module]` compilation units,
 *     extracts `<[dep]` module dependencies, and rejects duplicate named modules.
 *   - `ext_eval_slice` / `ext_compile_slice`: Executes `$type`, syntax delimiter,
 *     constant, and function/operator overload declarations alongside top-to-bottom
 *     module variables and statements/expressions for a single module slice.
 */

#include "compiler.h"
#include "parser.h"
#include "hardcode.h"
#include "evaluator.h"
#include "../lib/delegate/delegate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    MemArena   *mem;
    SymTable   *tab;
    const char *src;
    uint16_t    len;
    uint16_t    pos;
    uint16_t    cur_mod;
} ExtLoader;

static uint16_t ext_skip_ws_at(const char *src, uint16_t p, uint16_t len) {
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

static void ext_skip_ws(ExtLoader *L) {
    L->pos = ext_skip_ws_at(L->src, L->pos, L->len);
}

static bool ext_match_char(ExtLoader *L, char expected) {
    ext_skip_ws(L);
    if (L->pos < L->len && L->src[L->pos] == expected) {
        L->pos++;
        return true;
    }
    return false;
}

static StrSlice ext_read_ident(ExtLoader *L) {
    ext_skip_ws(L);
    uint16_t start = L->pos;
    while (L->pos < L->len && lex_is_ident_cont((unsigned char)L->src[L->pos])) {
        L->pos++;
    }
    return mem_intern_str(L->mem, L->src + start, (uint16_t)(L->pos - start));
}

static StrSlice ext_read_quoted(ExtLoader *L) {
    ext_skip_ws(L);
    if (L->pos >= L->len || L->src[L->pos] != '"') {
        mem_err(L->mem, "Expected quoted string symbol in declaration");
        StrSlice empty = { 0, 0 };
        return empty;
    }
    L->pos++;
    char buf[64];
    uint16_t blen = 0;
    while (L->pos < L->len && L->src[L->pos] != '"') {
        if (L->src[L->pos] == '\\' && L->pos + 1 < L->len) {
            L->pos++;
        }
        if (blen >= sizeof(buf) - 1) {
            mem_err(L->mem, "Quoted symbol exceeds maximum length (63 bytes)");
            StrSlice empty = { 0, 0 };
            return empty;
        }
        buf[blen++] = L->src[L->pos];
        L->pos++;
    }
    if (L->pos >= L->len || L->src[L->pos] != '"') {
        mem_err(L->mem, "Unterminated quoted string symbol in declaration");
        StrSlice empty = { 0, 0 };
        return empty;
    }
    L->pos++;
    return mem_intern_str(L->mem, buf, blen);
}

static int ext_find_duplicate_overload(const SymTable *tab, uint16_t sym_id, uint16_t mod_id,
                                       uint8_t fixity, uint8_t pcount,
                                       const uint16_t *ptypes, const uint8_t *punits) {
    if (sym_id >= tab->sym_count) return -1;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->mod_id != mod_id || ov->fixity != fixity || ov->param_count != pcount) continue;
        bool same = true;
        for (uint8_t p = 0; p < pcount; ++p) {
            if (ov->param_types[p] != (uint8_t)ptypes[p] || ov->param_units[p] != punits[p]) {
                same = false;
                break;
            }
        }
        if (same) return (int)i;
    }
    return -1;
}

static bool ext_peek_is_kw_type_decl(const ExtLoader *L, uint16_t pos) {
    if (pos >= L->len || !lex_is_ident_start((unsigned char)L->src[pos])) return false;
    uint16_t p = pos;
    while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
    uint16_t klen = (uint16_t)(p - pos);
    bool is_enum   = (klen == 4 && memcmp(L->src + pos, "enum", 4) == 0);
    bool is_struct = (klen == 6 && memcmp(L->src + pos, "struct", 6) == 0);
    if (!is_enum && !is_struct) return false;
    while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
    if (p >= L->len || !lex_is_ident_start((unsigned char)L->src[p])) return false;
    while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;

    while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
    if (p >= L->len) return false;
    return is_enum ? (L->src[p] == '{' || L->src[p] == '(') : (L->src[p] == '(');
}

static void ext_parse_type_decl(ExtLoader *L, uint8_t decl_flags) {
    MemArena *mem = L->mem;
    SymTable *tab = L->tab;
    if ((decl_flags & (SYM_VAR_MUTABLE | SYM_VAR_EXPLICIT_CONST)) != 0) {
        mem_err(mem, "Modifiers 'const' and 'mut' cannot be applied to '$type' declarations");
        return;
    }
    if (L->pos < L->len && L->src[L->pos] == '$') {
        L->pos++; /* skip '$' */
    }
    StrSlice tname = ext_read_ident(L);
    if (mem->has_error || tname.len == 0) return;

    bool is_enum_kw = (tname.len == 4 && memcmp(mem_str_ptr(mem, tname), "enum", 4) == 0);
    bool is_struct_kw = (tname.len == 6 && memcmp(mem_str_ptr(mem, tname), "struct", 6) == 0);
    if (is_enum_kw || is_struct_kw) {
        uint16_t next_p = L->pos;
        while (next_p < L->len && (L->src[next_p] == ' ' || L->src[next_p] == '\t')) next_p++;
        if (next_p < L->len && lex_is_ident_start((unsigned char)L->src[next_p])) {
            tname = ext_read_ident(L);
            if (mem->has_error || tname.len == 0) return;
        } else {
            is_enum_kw   = false;
            is_struct_kw = false;
        }
    }

    /* Reject duplicate type declarations */
    for (uint16_t i = 1; i < tab->type_count; ++i) {
        if (mem_slice_eq(mem, tab->types[i].name, tname)) {
            mem_err(mem, "Type '$%.*s' is already defined",
                    (int)tname.len, mem_str_ptr(mem, tname));
            return;
        }
    }

    if (tab->type_count >= SYM_MAX_TYPES) {
        mem_err(mem, "Type table overflow (max %d)", SYM_MAX_TYPES);
        return;
    }

    uint16_t tid = tab->type_count++;
    TypeDef *td  = &tab->types[tid];
    memset(td, 0, sizeof(*td));
    td->name     = tname;
    td->mod_id   = L->cur_mod;
    td->reserved = (decl_flags & SYM_VAR_PRIVATE) ? SYM_TYPE_PRIVATE : 0;

    ext_skip_ws(L);
    if (ext_match_char(L, '~')) {
        td->is_consuming = 1;
        td->flat_size    = 1;
        td->start_idx    = tab->pat_elem_count;
        uint8_t elem_cnt = 0;

        while (L->pos < L->len && !mem->has_error) {
            ext_skip_ws(L);
            if (L->pos + 1 < L->len && L->src[L->pos] == '=' && L->src[L->pos + 1] == '>') {
                L->pos = (uint16_t)(L->pos + 2);
                break;
            }
            if (tab->pat_elem_count >= SYM_MAX_PAT_ELEMS) {
                mem_err(mem, "Pattern element pool overflow");
                return;
            }
            PatElem *pe = &tab->pat_elems[tab->pat_elem_count++];
            memset(pe, 0, sizeof(*pe));
            elem_cnt++;

            char c = L->src[L->pos];
            if (c == '\\' && L->pos + 1 < L->len) {
                char esc = L->src[L->pos + 1];
                L->pos = (uint16_t)(L->pos + 2);
                if (esc == 'd') {
                    pe->kind = PAT_DIGIT;
                } else if (esc == 'a') {
                    pe->kind = PAT_ALPHA;
                } else {
                    mem_err(mem, "Invalid pattern escape '\\%c' (expected \\d or \\a)", esc);
                    return;
                }
            } else if (c == '"') {
                pe->kind  = PAT_LIT;
                pe->chars = ext_read_quoted(L);
            } else if (c == '[') {
                bool is_neg = (L->pos + 1 < L->len && L->src[L->pos + 1] == '^');
                L->pos = (uint16_t)(L->pos + (is_neg ? 2 : 1));
                char cset[32];
                uint16_t elen = 0;
                while (L->pos < L->len && L->src[L->pos] != ']') {
                    if (L->src[L->pos] == '\\' && L->pos + 1 < L->len) L->pos++;
                    if (elen >= sizeof(cset) - 1) {
                        mem_err(mem, "Character class exceeds maximum length (31 bytes)");
                        return;
                    }
                    cset[elen++] = L->src[L->pos];
                    L->pos++;
                }
                if (L->pos < L->len && L->src[L->pos] == ']') L->pos++;
                pe->kind  = is_neg ? PAT_NOT_CHARS : PAT_CHARS;
                pe->chars = mem_intern_str(mem, cset, elen);
            } else if (c == '(') {
                L->pos++;
                pe->kind = PAT_GROUP_START;
            } else if (c == ')') {
                L->pos++;
                pe->kind = PAT_GROUP_END;
            } else if (c == '|') {
                L->pos++;
                pe->kind = PAT_ALT;
            } else {
                mem_err(mem, "Invalid pattern syntax near '%c'", c);
                return;
            }

            if (pe->kind != PAT_GROUP_START && L->pos < L->len) {
                char q = L->src[L->pos];
                if (q == '+') { pe->quant = QUANT_PLUS; L->pos++; }
                else if (q == '*') { pe->quant = QUANT_STAR; L->pos++; }
                else if (q == '?') { pe->quant = QUANT_OPT;  L->pos++; }
            }
        }
        td->field_count = elem_cnt;

        if (!ext_match_char(L, '@')) {
            mem_err(mem, "Expected '@parser_intrinsic' after '=>' in consuming type");
            return;
        }
        StrSlice iname = ext_read_ident(L);
        td->parser_intr = (uint8_t)sym_lookup_intrinsic(mem_str_ptr(mem, iname), iname.len);
        if (td->parser_intr == INTR_PARSE_F64 && mem->fast_real_type == 0) {
            mem->fast_real_type = (uint8_t)tid;
        } else if (td->parser_intr == INTR_PARSE_STR_SLICE && mem->str_type_id == 0) {
            mem->str_type_id = (uint8_t)tid;
        } else if (td->parser_intr == INTR_VEC_TYPE) {
            td->is_consuming = 0;
            td->field_count  = 0;
            td->flat_size    = MEM_VEC_HDR_SLOTS;
            if (mem->vec_type_id == 0) {
                mem->vec_type_id = (uint8_t)tid;
            }
        } else if (td->parser_intr == INTR_MAT_TYPE) {
            td->is_consuming = 0;
            td->field_count  = 0;
            td->flat_size    = MEM_MAT_HDR_SLOTS;
            if (mem->mat_type_id == 0) {
                mem->mat_type_id = (uint8_t)tid;
            }
        } else if (td->parser_intr == INTR_FN_TYPE) {
            td->is_consuming = 0;
            td->field_count  = 0;
            td->flat_size    = MEM_FN_HDR_SLOTS;
            if (mem->fn_type_id == 0) {
                mem->fn_type_id = (uint8_t)tid;
            }
        } else if (td->parser_intr == INTR_MAP_TYPE) {
            td->is_consuming = 0;
            td->field_count  = 0;
            td->flat_size    = MEM_MAP_HDR_SLOTS;
            if (mem->map_type_id == 0) {
                mem->map_type_id = (uint8_t)tid;
            }
        } else if (td->parser_intr == INTR_ANY_TYPE || td->parser_intr == INTR_NONE_UNIT) {
            td->is_consuming = 0;
            td->field_count  = 0;
            td->flat_size    = 0;
        }
        if (tid < 64u) mem->type_stride[tid] = (uint8_t)td->flat_size;
    } else if (ext_match_char(L, '(')) {
        td->is_consuming = 0;
        td->start_idx    = tab->field_count;
        uint16_t flat_off = 0;
        uint8_t  fcount   = 0;

        while (L->pos < L->len && !mem->has_error) {
            if (ext_match_char(L, ')')) break;
            StrSlice fname = ext_read_ident(L);
            uint16_t ftid  = mem->fast_real_type;
            if (ext_match_char(L, ':')) {
                StrSlice ftype_s = ext_read_ident(L);
                ftid = sym_find_type(tab, mem, mem_str_ptr(mem, ftype_s), ftype_s.len);
                if (ftid == MEM_TYPE_VOID || ftid == SYM_TYPE_ANY) {
                    mem_err(mem, "Unknown field type '%.*s'", (int)ftype_s.len, mem_str_ptr(mem, ftype_s));
                    return;
                }
            }
            if (ftid == tid) {
                mem_err(mem, "Self-referential struct field '%.*s' in '$%.*s' is not allowed",
                        (int)fname.len, mem_str_ptr(mem, fname),
                        (int)tname.len, mem_str_ptr(mem, tname));
                return;
            }
            if (ftid >= tab->type_count || tab->types[ftid].flat_size == 0) {
                mem_err(mem, "Incomplete or invalid field type in '$%.*s'",
                        (int)tname.len, mem_str_ptr(mem, tname));
                return;
            }
            if (tab->field_count >= SYM_MAX_FIELDS) {
                mem_err(mem, "Struct field table overflow (max %d)", SYM_MAX_FIELDS);
                return;
            }
            FieldDef *fd = &tab->fields[tab->field_count++];
            fd->name        = fname;
            fd->type_id     = ftid;
            fd->flat_offset = flat_off;

            flat_off = (uint16_t)(flat_off + tab->types[ftid].flat_size);
            fcount++;
            ext_match_char(L, ',');
        }
        td->field_count = fcount;
        td->flat_size   = flat_off;
        if (tid < 64u) mem->type_stride[tid] = (uint8_t)flat_off;

        /* Optional same-line `=> @type_intrinsic` tag on struct type (e.g. `$complex(re, im) => @complex_type`) */
        while (L->pos < L->len && (L->src[L->pos] == ' ' || L->src[L->pos] == '\t')) L->pos++;
        if (L->pos + 1 < L->len && L->src[L->pos] == '=' && L->src[L->pos + 1] == '>') {
            L->pos = (uint16_t)(L->pos + 2);
            if (!ext_match_char(L, '@')) {
                mem_err(mem, "Expected '@type_intrinsic' after '=>' in struct type declaration");
                return;
            }
            StrSlice iname = ext_read_ident(L);
            td->parser_intr = (uint8_t)sym_lookup_intrinsic(mem_str_ptr(mem, iname), iname.len);
        }
    } else if (ext_match_char(L, '{') || (is_enum_kw && ext_match_char(L, '('))) {
        char close_ch = (L->src[L->pos - 1] == '(') ? ')' : '}';
        /* Peek ahead to determine if any variant carries payload parameters `Variant(...)` */
        bool has_payload = false;
        {
            uint16_t scan = L->pos;
            int depth = 0;
            while (scan < L->len) {
                char c = L->src[scan];
                if (c == close_ch && depth == 0) break;
                if (c == '(') {
                    has_payload = true;
                    break;
                }
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']' || c == ')') { if (depth > 0) depth--; }
                scan++;
            }
        }

        if (!has_payload) {
            td->is_consuming = 1;
            td->flat_size    = 1;
            td->parser_intr  = INTR_PARSE_I64;
            td->start_idx    = tab->field_count;
            uint8_t vcount   = 0;
            long    next_val = 0;

            while (L->pos < L->len && !mem->has_error) {
                ext_skip_ws(L);
                if (ext_match_char(L, close_ch)) break;
                StrSlice vname = ext_read_ident(L);
                if (mem->has_error || vname.len == 0) {
                    mem_err(mem, "Expected variant identifier in enum '$%.*s'",
                            (int)tname.len, mem_str_ptr(mem, tname));
                    return;
                }
                ext_skip_ws(L);
                if (ext_match_char(L, '=')) {
                    ext_skip_ws(L);
                    char *endp = NULL;
                    long parsed = strtol(L->src + L->pos, &endp, 10);
                    if (endp == L->src + L->pos) {
                        mem_err(mem, "Expected integer value after '=' for enum variant '%.*s'",
                                (int)vname.len, mem_str_ptr(mem, vname));
                        return;
                    }
                    L->pos   = (uint16_t)(endp - L->src);
                    next_val = parsed;
                }
                if (tab->field_count >= SYM_MAX_FIELDS) {
                    mem_err(mem, "Struct/enum field table overflow (max %d)", SYM_MAX_FIELDS);
                    return;
                }
                FieldDef *fd    = &tab->fields[tab->field_count++];
                fd->name        = vname;
                fd->type_id     = tid;
                fd->flat_offset = (uint16_t)(int16_t)next_val;

                uint16_t vsym = sym_intern(tab, mem, vname);
                if (mem->has_error) return;
                Value vval = val_make_f64(mem, tid, (double)next_val);
                uint8_t vflags = (uint8_t)((decl_flags & SYM_VAR_PRIVATE) | SYM_VAR_EXPLICIT_CONST);
                (void)sym_set_or_add_var(tab, mem, vsym, L->cur_mod, vval, tid, 0, vflags, true);
                if (mem->has_error) return;

                next_val++;
                vcount++;
                ext_skip_ws(L);
                if (!ext_match_char(L, ',')) {
                    (void)ext_match_char(L, ';');
                }
            }
            if (vcount == 0 && !mem->has_error) {
                mem_err(mem, "Enum '$%.*s' must declare at least one variant",
                        (int)tname.len, mem_str_ptr(mem, tname));
                return;
            }
            td->field_count = vcount;
            if (tid < 64u) mem->type_stride[tid] = 1u;
        } else {
            /* Tagged Union / Algebraic Data Type (`$enum Result { Ok(val: real), Err(msg: str) }`) */
            td->is_consuming = 0;
            td->parser_intr  = INTR_PARSE_I64;
            td->start_idx    = tab->field_count;
            uint16_t max_payload_slots = 1;
            uint8_t  total_fdefs       = 0;
            uint8_t  v_idx             = 0;
            uint16_t unit_var_syms[16];
            uint8_t  unit_var_tags[16];
            uint8_t  unit_var_cnt      = 0;

            while (L->pos < L->len && !mem->has_error) {
                ext_skip_ws(L);
                if (ext_match_char(L, close_ch)) break;
                StrSlice vname = ext_read_ident(L);
                if (mem->has_error || vname.len == 0) {
                    mem_err(mem, "Expected variant identifier in enum '$%.*s'",
                            (int)tname.len, mem_str_ptr(mem, tname));
                    return;
                }
                if (v_idx >= 15u) {
                    mem_err(mem, "Tagged union '$%.*s' supports at most 15 variants",
                            (int)tname.len, mem_str_ptr(mem, tname));
                    return;
                }
                if (tab->field_count >= SYM_MAX_FIELDS) {
                    mem_err(mem, "Struct/enum field table overflow (max %d)", SYM_MAX_FIELDS);
                    return;
                }
                uint16_t vfd_idx = tab->field_count++;
                total_fdefs++;
                tab->fields[vfd_idx].name    = vname;
                tab->fields[vfd_idx].type_id = tid;

                uint8_t  pcnt  = 0;
                uint16_t p_off = 1;
                uint16_t ptypes[8];
                ext_skip_ws(L);
                if (ext_match_char(L, '(')) {
                    while (L->pos < L->len && !mem->has_error) {
                        ext_skip_ws(L);
                        if (ext_match_char(L, ')')) break;
                        if (pcnt >= 8u) {
                            mem_err(mem, "Enum variant '%.*s' supports at most 8 payload fields",
                                    (int)vname.len, mem_str_ptr(mem, vname));
                            return;
                        }
                        StrSlice pname = ext_read_ident(L);
                        uint16_t ptid  = mem->fast_real_type;
                        ext_skip_ws(L);
                        if (ext_match_char(L, ':')) {
                            StrSlice pts = ext_read_ident(L);
                            ptid = sym_find_type(tab, mem, mem_str_ptr(mem, pts), pts.len);
                            if (ptid == MEM_TYPE_VOID || ptid == SYM_TYPE_ANY) {
                                mem_err(mem, "Unknown payload field type '%.*s' in variant '%.*s'",
                                        (int)pts.len, mem_str_ptr(mem, pts),
                                        (int)vname.len, mem_str_ptr(mem, vname));
                                return;
                            }
                        }
                        if (tab->field_count >= SYM_MAX_FIELDS) {
                            mem_err(mem, "Struct/enum field table overflow (max %d)", SYM_MAX_FIELDS);
                            return;
                        }
                        FieldDef *pfd   = &tab->fields[tab->field_count++];
                        total_fdefs++;
                        pfd->name        = pname;
                        pfd->type_id     = ptid;
                        pfd->flat_offset = p_off;
                        ptypes[pcnt++]   = ptid;
                        p_off = (uint16_t)(p_off + tab->types[ptid].flat_size);
                        ext_skip_ws(L);
                        (void)ext_match_char(L, ',');
                    }
                }
                tab->fields[vfd_idx].flat_offset = (uint16_t)(((uint16_t)pcnt << 8u) | (uint16_t)v_idx);
                if ((uint16_t)(p_off - 1u) > max_payload_slots) {
                    max_payload_slots = (uint16_t)(p_off - 1u);
                }

                uint16_t vsym = sym_intern(tab, mem, vname);
                if (mem->has_error) return;
                if (pcnt == 0) {
                    if (unit_var_cnt < 16u) {
                        unit_var_syms[unit_var_cnt] = vsym;
                        unit_var_tags[unit_var_cnt] = v_idx;
                        unit_var_cnt++;
                    }
                } else {
                    /* Synthesize constructor overload `VariantName(p0, ...) -> EnumT` */
                    if (tab->overload_count < SYM_MAX_OVERLOADS) {
                        uint16_t head = MEM_NO_IDX;
                        uint16_t tail = MEM_NO_IDX;
                        for (uint8_t pi = 0; pi < pcnt; ++pi) {
                            uint16_t sload = mem_alloc_ast(mem, AST_SLOT_LOAD, pi, 0, 0, 0);
                            uint16_t link  = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, sload, MEM_NO_IDX);
                            if (head == MEM_NO_IDX) head = link;
                            else mem_get_ast_mut(mem, tail)->right = link;
                            tail = link;
                        }
                        uint8_t c_sub = (uint8_t)(0x80u | ((v_idx & 0x07u) << 4u) | (pcnt & 0x0Fu));
                        uint16_t body_ast = mem_alloc_ast(mem, AST_CONSTRUCT, c_sub, tid, head, MEM_NO_IDX);
                        uint16_t ov_idx   = tab->overload_count++;
                        OpOverload *ov    = &tab->overloads[ov_idx];
                        memset(ov, 0, sizeof(*ov));
                        ov->sym_id      = vsym;
                        ov->mod_id      = L->cur_mod;
                        ov->fixity      = FIX_FUNC;
                        ov->param_count = pcnt;
                        ov->local_count = pcnt;
                        ov->ret_type    = (uint8_t)tid;
                        ov->ret_unit    = SYM_UNIT_OMITTED;
                        ov->reserved    = (decl_flags & SYM_VAR_PRIVATE) ? SYM_OV_PRIVATE : 0;
                        for (uint8_t pi = 0; pi < pcnt; ++pi) {
                            ov->param_types[pi] = (uint8_t)ptypes[pi];
                            ov->param_units[pi] = SYM_UNIT_OMITTED;
                        }
                        ov->body_ast = body_ast;
                        sym_link_overload(tab, ov_idx);
                    }
                }

                v_idx++;
                ext_skip_ws(L);
                if (!ext_match_char(L, ',')) {
                    (void)ext_match_char(L, ';');
                }
            }
            if (v_idx == 0 && !mem->has_error) {
                mem_err(mem, "Enum '$%.*s' must declare at least one variant",
                        (int)tname.len, mem_str_ptr(mem, tname));
                return;
            }
            td->field_count = total_fdefs;
            td->flat_size   = (uint16_t)(1u + max_payload_slots);
            if (tid < 64u) mem->type_stride[tid] = (uint8_t)td->flat_size;

            for (uint8_t ui = 0; ui < unit_var_cnt; ++ui) {
                uint16_t pidx = mem_alloc_pool(mem, td->flat_size);
                if (mem->has_error) return;
                for (uint16_t k = 0; k < td->flat_size; ++k) {
                    mem_write_pool(mem, (uint16_t)(pidx + k), 0.0);
                }
                mem_write_pool(mem, pidx, (double)unit_var_tags[ui]);
                Value uval = val_make_comp(mem, tid, pidx, td->flat_size);
                uint8_t vflags = (uint8_t)((decl_flags & SYM_VAR_PRIVATE) | SYM_VAR_EXPLICIT_CONST);
                (void)sym_set_or_add_var(tab, mem, unit_var_syms[ui], L->cur_mod, uval, tid, 0, vflags, true);
            }
        }
    } else {
        mem_err(mem, "Expected '~' (consuming/container), '(' (struct), or '{' (enum) in type declaration");
    }
}

static bool ext_resolve_type_and_unit(ExtLoader *L, StrSlice first_id,
                                      bool has_second, StrSlice second_id,
                                      uint16_t *out_type, uint8_t *out_unit) {
    MemArena *mem = L->mem;
    SymTable *tab = L->tab;
    const char *s1 = mem_str_ptr(mem, first_id);

    if (has_second) {
        uint16_t tid = sym_find_type(tab, mem, s1, first_id.len);
        if (tid == MEM_TYPE_VOID) {
            mem_err(mem, "Unknown entity type '%.*s'", (int)first_id.len, s1);
            return false;
        }
        const char *s2 = mem_str_ptr(mem, second_id);
        uint8_t uid = sym_find_or_add_unit(tab, mem, s2, second_id.len);
        *out_type = tid;
        *out_unit = uid;
        return !mem->has_error;
    }

    uint16_t tid = sym_find_type(tab, mem, s1, first_id.len);
    if (tid != MEM_TYPE_VOID) {
        *out_type = tid;
        *out_unit = SYM_UNIT_OMITTED;
        return true;
    }

    uint8_t uid = sym_find_or_add_unit(tab, mem, s1, first_id.len);
    *out_type = SYM_TYPE_ANY;
    *out_unit = uid;
    return !mem->has_error;
}

static bool ext_parse_one_param(ExtLoader *L, StrSlice *out_name,
                                uint16_t *out_type, uint8_t *out_unit) {
    if (!ext_match_char(L, '(')) return false;
    *out_name = ext_read_ident(L);
    if (sym_is_reserved_kw(L->tab, L->mem, mem_str_ptr(L->mem, *out_name), out_name->len)) {
        mem_err(L->mem, "Cannot use reserved keyword '%.*s' as a variable name",
                (int)out_name->len, mem_str_ptr(L->mem, *out_name));
        return false;
    }
    if (!ext_match_char(L, ':')) {
        *out_type = L->mem->fast_real_type ? L->mem->fast_real_type : 1;
        *out_unit = SYM_UNIT_OMITTED;
        if (!ext_match_char(L, ')')) {
            mem_err(L->mem, "Expected ')' after parameter declaration");
            return false;
        }
        return true;
    }
    StrSlice tname = ext_read_ident(L);
    bool has_unit  = false;
    StrSlice uname = { 0, 0 };
    if (ext_match_char(L, ',')) {
        has_unit = true;
        uname    = ext_read_ident(L);
    }
    if (!ext_resolve_type_and_unit(L, tname, has_unit, uname, out_type, out_unit)) {
        return false;
    }
    if (!ext_match_char(L, ')')) {
        mem_err(L->mem, "Expected ')' after parameter declaration");
        return false;
    }
    return true;
}

static bool ext_peek_ident_is_unit_annot(ExtLoader *L) {
    ext_skip_ws(L);
    uint16_t p = L->pos;
    if (p >= L->len || !lex_is_ident_start((unsigned char)L->src[p])) return false;
    uint16_t id_start = p;
    while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
    uint16_t id_len = (uint16_t)(p - id_start);
    while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t' || L->src[p] == '\r' || L->src[p] == '\n')) p++;
    if (p < L->len && (L->src[p] == ':' || L->src[p] == '=')) return false;
    return sym_find_unit(L->tab, L->mem, L->src + id_start, id_len) != MEM_UNIT_NONE;
}

static bool ext_parse_return_spec(ExtLoader *L, uint16_t *out_rtype, uint8_t *out_runit) {
    ext_skip_ws(L);
    if (L->pos < L->len && L->src[L->pos] == '=') {
        *out_rtype = L->mem->fast_real_type ? L->mem->fast_real_type : 1;
        *out_runit = SYM_UNIT_OMITTED;
        return true;
    }
    if (L->pos + 1 < L->len && L->src[L->pos] == '-' && L->src[L->pos + 1] == '>') {
        L->pos = (uint16_t)(L->pos + 2);
    }
    StrSlice rtype_s = ext_read_ident(L);
    bool has_unit    = false;
    StrSlice runit_s = { 0, 0 };
    if (ext_match_char(L, ',')) {
        has_unit = true;
        runit_s  = ext_read_ident(L);
    }
    return ext_resolve_type_and_unit(L, rtype_s, has_unit, runit_s, out_rtype, out_runit);
}

static void ext_skip_default_expr(ExtLoader *L, uint16_t *out_start, uint16_t *out_end) {
    ext_skip_ws(L);
    *out_start = L->pos;
    int depth = 0;
    while (L->pos < L->len) {
        char c = L->src[L->pos];
        if (c == '"') {
            L->pos++;
            while (L->pos < L->len && L->src[L->pos] != '"') {
                if (L->src[L->pos] == '\\' && L->pos + 1 < L->len) L->pos++;
                L->pos++;
            }
            if (L->pos < L->len) L->pos++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) break;
            depth--;
        } else if (c == ',' && depth == 0) {
            break;
        }
        L->pos++;
    }
    *out_end = L->pos;
}

static void ext_compile_body(ExtLoader *L, OpOverload *ov, StrSlice *pnames) {
    MemArena *mem = L->mem;
    SymTable *tab = L->tab;
    ext_skip_ws(L);

    Parser P;
    parser_init(&P, mem, tab, L->src, L->len, L->pos, true, true);
    P.cur_mod     = L->cur_mod;
    P.param_count = ov->param_count;

    for (uint8_t i = 0; i < ov->param_count; ++i) {
        parser_add_local(&P, mem_str_ptr(mem, pnames[i]), pnames[i].len, ov->param_types[i]);
    }

    uint16_t ast_before = mem->ast_count;
    lex_next(&P.lex);
    int min_prec = (P.lex.cur.kind == TOK_LBRACE) ? 0 : 2;
    uint16_t body_root = parser_parse_expr(&P, min_prec);
    if (mem->fn_type_id != 0 && !mem->has_error) {
        for (uint16_t ai = ast_before; ai < mem->ast_count; ++ai) {
            const AstNode *an = mem_get_ast(mem, ai);
            if (an->kind == AST_FN_CALL && an->left != MEM_NO_IDX) {
                const AstNode * target_n = mem_get_ast(mem, an->left);
                if (target_n->kind == AST_SLOT_LOAD && target_n->sub < ov->param_count &&
                    ov->param_types[target_n->sub] == (uint8_t)mem->fast_real_type) {
                    ov->param_types[target_n->sub] = (uint8_t)mem->fn_type_id;
                }
            }
        }
    }
    (void)parser_check_labels(&P);
    ov->body_ast    = body_root;
    ov->local_count = P.local_count;
    ov->meta_tag    = 0;

    L->pos = (P.lex.cur.kind == TOK_EOF) ? P.lex.pos : P.lex.cur.off;

    if (body_root != MEM_NO_IDX) {
        const AstNode *root = mem_get_ast(mem, body_root);
        if (root->kind == AST_INTRINSIC &&
            delegate_is_meta_intrinsic((IntrinsicId)root->sub)) {
            ov->meta_tag = root->sub;
            return;
        }
        if (ov->param_count == 2 && root->kind == AST_BINARY &&
            root->left != MEM_NO_IDX && root->right != MEM_NO_IDX &&
            mem_get_ast(mem, root->left)->kind == AST_SLOT_LOAD && mem_get_ast(mem, root->left)->sub == 0 &&
            mem_get_ast(mem, root->right)->kind == AST_SLOT_LOAD && mem_get_ast(mem, root->right)->sub == 1) {
            uint16_t target_sym = root->ref;
            if (target_sym < tab->sym_count) {
                for (uint16_t i = tab->sym_first_ov[target_sym]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
                    const OpOverload *target = &tab->overloads[i];
                    if (target != ov &&
                        target->param_count == 2 &&
                        target->param_types[0] == ov->param_types[0] &&
                        target->param_types[1] == ov->param_types[1] &&
                        target->param_units[0] == ov->param_units[0] &&
                        target->param_units[1] == ov->param_units[1] &&
                        target->ret_unit       == ov->ret_unit) {
                        ov->body_ast    = target->body_ast;
                        ov->local_count = target->local_count;
                        ov->meta_tag    = 1;
                        break;
                    }
                }
            }
        }
    }
}

static bool ext_body_starts_with_sym_intrinsic(ExtLoader *L) {
    ext_skip_ws(L);
    return (L->pos + 5 <= L->len && memcmp(L->src + L->pos, "@sym_", 5) == 0);
}

static void ext_parse_sym_decl_ex(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags,
                                  bool allow_redef, uint16_t assoc_type_id);

static void ext_parse_sym_decl(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags, bool allow_redef) {
    ext_parse_sym_decl_ex(L, sym_id, decl_flags, allow_redef, 0);
}

static void ext_parse_sym_decl_ex(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags,
                                  bool allow_redef, uint16_t assoc_type_id) {
    MemArena *mem = L->mem;
    SymTable *tab = L->tab;
    ext_skip_ws(L);

    /* 1. Syntax delimiter binding (`"(" => @lparen`, `"{" => @lbrace`, `"," => @comma`) */
    if (L->pos + 1 < L->len && L->src[L->pos] == '=' && L->src[L->pos + 1] == '>') {
        L->pos = (uint16_t)(L->pos + 2);
        if (!ext_match_char(L, '@')) {
            mem_err(mem, "Expected '@syntax_intrinsic' after '=>'");
            return;
        }
        StrSlice iname = ext_read_ident(L);
        IntrinsicId intr = sym_lookup_intrinsic(mem_str_ptr(mem, iname), iname.len);
        if (intr != INTR_LPAREN && intr != INTR_RPAREN &&
            intr != INTR_LBRACE && intr != INTR_RBRACE &&
            intr != INTR_LBRACK && intr != INTR_RBRACK &&
            intr != INTR_COMMA) {
            mem_err(mem, "Unknown syntax delimiter intrinsic '@%.*s'",
                     (int)iname.len, mem_str_ptr(mem, iname));
            return;
        }
        if (tab->overload_count >= SYM_MAX_OVERLOADS) {
            mem_err(mem, "Overload table overflow (max %d)", SYM_MAX_OVERLOADS);
            return;
        }
        uint16_t ov_idx = tab->overload_count++;
        OpOverload *ov  = &tab->overloads[ov_idx];
        memset(ov, 0, sizeof(*ov));
        ov->sym_id   = sym_id;
        ov->mod_id   = L->cur_mod;
        ov->fixity   = FIX_SYNTAX;
        ov->meta_tag = (uint8_t)intr;
        ov->reserved = (decl_flags & SYM_VAR_PRIVATE) ? SYM_OV_PRIVATE : 0;
        sym_link_overload(tab, ov_idx);
        return;
    }

    /* 2. Constant / variable definition (`"g0" : real = 9.80665`, `"gx" : real, distance = 5m`, or `g0 = 9.80665`) */
    if (ext_match_char(L, ':') || ext_match_char(L, '=')) {
        bool already_matched_eq = (L->src[L->pos - 1] == '=');
        uint16_t rtid      = mem->fast_real_type ? mem->fast_real_type : 1;
        uint8_t  ruid      = SYM_UNIT_OMITTED;
        uint16_t lock_type = 0;
        if (!already_matched_eq) {
            if (L->pos < L->len && L->src[L->pos] == '=') {
                L->pos++;
                decl_flags |= SYM_VAR_MUTABLE;
            } else {
                if (!ext_parse_return_spec(L, &rtid, &ruid)) return;
                if (rtid != SYM_TYPE_ANY) lock_type = rtid;
                else rtid = mem->fast_real_type ? mem->fast_real_type : 1;
                ext_skip_ws(L);
                if (L->pos + 1 < L->len && L->src[L->pos] == ':' && L->src[L->pos + 1] == '=') {
                    L->pos = (uint16_t)(L->pos + 2);
                    decl_flags |= SYM_VAR_MUTABLE;
                } else if (!ext_match_char(L, '=')) {
                    mem_err(mem, "Expected '=' or ':=' in constant/variable definition");
                    return;
                }
            }
        }
        if ((decl_flags & SYM_VAR_MUTABLE) == 0) {
            decl_flags |= SYM_VAR_EXPLICIT_CONST;
        }
        ext_skip_ws(L);
        Parser P;
        parser_init(&P, mem, tab, L->src, L->len, L->pos, false, true);
        P.cur_mod      = L->cur_mod;
        P.is_mod_scope = true;
        lex_next(&P.lex);
        uint16_t c_ast = parser_parse_expr(&P, 2);
        L->pos = (P.lex.cur.kind == TOK_EOF) ? P.lex.pos : P.lex.cur.off;
        if (mem->has_error || c_ast == MEM_NO_IDX) return;

        uint16_t fbase = (P.local_count > 0) ? mem_push_frame(mem, P.local_count) : 0;
        if (mem->has_error) return;
        Value ev = eval_ast(mem, tab, c_ast, fbase, (uint8_t)rtid);
        if (P.local_count > 0) mem_pop_frame(mem, fbase);
        if (mem->has_error) return;

        uint8_t lock_unit = (ruid != SYM_UNIT_OMITTED && ruid != SYM_UNIT_GENERIC) ? ruid : 0;
        if (lock_unit != 0 && lock_unit < SYM_UNIT_LOCK_NONE && val_get_unit(mem, ev) == MEM_UNIT_NONE) {
            ev = val_with_unit(mem, ev, lock_unit);
        }
        (void)sym_set_or_add_var(tab, mem, sym_id, L->cur_mod, ev, lock_type, lock_unit, decl_flags, true);
        return;
    }

    /* 3. Function or Prefix Operator Declaration (`"fn"(...)` or `fn(...)`) */
    if ((decl_flags & (SYM_VAR_MUTABLE | SYM_VAR_EXPLICIT_CONST)) != 0) {
        mem_err(mem, "Modifiers 'const' and 'mut' cannot be applied to function or operator declarations");
        return;
    }
    if (!ext_match_char(L, '(')) {
        mem_err(mem, "Expected '(', ':', '=', or '=>' after symbol in declaration");
        return;
    }
    StrSlice pnames[SYM_MAX_OV_PARAMS];
    uint16_t ptypes[SYM_MAX_OV_PARAMS];
    uint8_t  punits[SYM_MAX_OV_PARAMS];
    bool     has_def[SYM_MAX_OV_PARAMS];
    uint16_t def_start[SYM_MAX_OV_PARAMS];
    uint16_t def_end[SYM_MAX_OV_PARAMS];
    uint8_t  pcount    = 0;
    uint8_t  req_count = 0;
    bool     seen_def  = false;
    memset(has_def, 0, sizeof(has_def));

    while (L->pos < L->len && !mem->has_error) {
        if (ext_match_char(L, ')')) break;
        if (pcount >= SYM_MAX_OV_PARAMS) {
            mem_err(mem, "Max %d parameters supported", SYM_MAX_OV_PARAMS);
            break;
        }
        pnames[pcount] = ext_read_ident(L);
        if (sym_is_reserved_kw(tab, mem, mem_str_ptr(mem, pnames[pcount]), pnames[pcount].len)) {
            mem_err(mem, "Cannot use reserved keyword '%.*s' as a variable name",
                    (int)pnames[pcount].len, mem_str_ptr(mem, pnames[pcount]));
            break;
        }
        if (ext_match_char(L, ':')) {
            StrSlice tn = ext_read_ident(L);
            bool has_u  = false;
            StrSlice un = { 0, 0 };
            if (ext_match_char(L, ',')) {
                if (ext_peek_ident_is_unit_annot(L)) {
                    has_u = true;
                    un    = ext_read_ident(L);
                }
            }
            if (!ext_resolve_type_and_unit(L, tn, has_u, un, &ptypes[pcount], &punits[pcount])) {
                break;
            }
        } else {
            if (pcount == 0 && assoc_type_id != 0) {
                ptypes[pcount] = assoc_type_id;
            } else {
                ptypes[pcount] = mem->fast_real_type ? mem->fast_real_type : 1;
            }
            punits[pcount] = SYM_UNIT_OMITTED;
        }

        if (ext_match_char(L, '=')) {
            seen_def        = true;
            has_def[pcount] = true;
            ext_skip_default_expr(L, &def_start[pcount], &def_end[pcount]);
        } else {
            if (seen_def) {
                mem_err(mem, "Parameter without default value cannot follow a default parameter");
                break;
            }
            req_count++;
        }
        ext_match_char(L, ',');
        pcount++;
    }
    if (mem->has_error) return;

    ext_skip_ws(L);
    uint8_t fixity = FIX_FUNC;
    uint8_t prec   = 0;

    if (ext_match_char(L, ':')) {
        fixity = FIX_PREFIX;
        char *endp = NULL;
        prec = (uint8_t)strtol(L->src + L->pos, &endp, 10);
        L->pos = (uint16_t)(endp - L->src);
    }

    uint16_t rtid = 0;
    uint8_t  ruid = SYM_UNIT_OMITTED;
    if (!ext_parse_return_spec(L, &rtid, &ruid)) return;
    ext_match_char(L, '=');

    /* Detect lazy bound-variable parameter prefix (`var_x`, `var_t, var_y`, etc.) on `.math` functions */
    uint8_t lazy_mtag = 0;
    if (fixity == FIX_FUNC && pcount >= 2 && !ext_body_starts_with_sym_intrinsic(L)) {
        uint8_t v_cnt = 0;
        while (v_cnt < pcount && v_cnt < 4u &&
               pnames[v_cnt].len > 4u &&
               memcmp(mem_str_ptr(mem, pnames[v_cnt]), "var_", 4) == 0) {
            v_cnt++;
        }
        if (v_cnt >= 1u && v_cnt < pcount) {
            uint8_t e_cnt = (v_cnt <= 2u) ? 1u : (uint8_t)(v_cnt - 1u);
            if ((uint8_t)(v_cnt + e_cnt) > pcount) {
                e_cnt = (uint8_t)(pcount - v_cnt);
            }
            for (uint8_t e = 0; e < e_cnt; ++e) {
                if (mem->fn_type_id != 0) {
                    ptypes[v_cnt + e] = mem->fn_type_id;
                }
            }
            bool is_eq = (pnames[v_cnt].len == 2u &&
                          memcmp(mem_str_ptr(mem, pnames[v_cnt]), "eq", 2) == 0);
            lazy_mtag = (uint8_t)(SYM_META_LAZY_MASK |
                                  (is_eq ? SYM_META_LAZY_EQ_BIT : 0u) |
                                  ((v_cnt & 0x07u) << 3u) |
                                  (e_cnt & 0x07u));
        }
    }

    int dup_idx = ext_find_duplicate_overload(tab, sym_id, L->cur_mod, fixity, pcount, ptypes, punits);
    if (dup_idx >= 0 && !allow_redef) {
        StrSlice s = tab->syms[sym_id];
        mem_err(mem, "Duplicate declaration of '%.*s' in module '[%.*s]'",
                (int)s.len, mem_str_ptr(mem, s),
                (int)sym_mod_name(tab, L->cur_mod).len,
                mem_str_ptr(mem, sym_mod_name(tab, L->cur_mod)));
        return;
    }

    uint16_t ov_idx = 0;
    if (dup_idx >= 0) {
        ov_idx = (uint16_t)dup_idx;
        if (ov_idx < 2048u) mem->ov_bc_entry[ov_idx] = 0;
    } else {
        if (tab->overload_count >= SYM_MAX_OVERLOADS) {
            mem_err(mem, "Overload table overflow (max %d)", SYM_MAX_OVERLOADS);
            return;
        }
        ov_idx = tab->overload_count++;
    }
    OpOverload *ov  = &tab->overloads[ov_idx];
    memset(ov, 0, sizeof(*ov));
    ov->sym_id      = sym_id;
    ov->mod_id      = L->cur_mod;
    ov->fixity      = fixity;
    ov->prec        = prec;
    ov->param_count = pcount;
    ov->ret_type    = (uint8_t)rtid;
    ov->ret_unit    = ruid;
    ov->reserved    = (decl_flags & SYM_VAR_PRIVATE) ? SYM_OV_PRIVATE : 0;
    for (uint8_t i = 0; i < pcount; ++i) {
        ov->param_types[i] = (uint8_t)ptypes[i];
        ov->param_units[i] = punits[i];
    }
    if (dup_idx < 0) {
        sym_link_overload(tab, ov_idx);
    }

    ext_compile_body(L, ov, pnames);
    if (mem->has_error) return;
    if (lazy_mtag != 0 && ov->meta_tag == 0) {
        ov->meta_tag = lazy_mtag;
    }

    /* 4. Synthesize lower-arity overload wrappers for trailing default parameters */
    if (seen_def && fixity == FIX_FUNC && req_count < pcount) {
        hardcode_synthesize_default_overloads(mem, tab, L->src, sym_id, L->cur_mod,
                                              ov_idx, pcount, req_count,
                                              pnames, ptypes, punits,
                                              def_start, def_end, rtid, ruid);
    }
}

/* ============================================================================
 * TOP-LEVEL DECLARATION VS. EXECUTABLE STATEMENT LOOKAHEAD HELPERS
 * ============================================================================ */

static bool ext_is_mod_kw(const char *s, uint16_t len) {
    return (len == 3 && (memcmp(s, "pub", 3) == 0 || memcmp(s, "mut", 3) == 0)) ||
           (len == 4 && memcmp(s, "priv", 4) == 0) ||
           (len == 5 && memcmp(s, "const", 5) == 0);
}

static bool ext_peek_is_mod_header(const char *src, uint16_t pos, uint16_t len,
                                   uint16_t *out_id_start, uint16_t *out_id_len,
                                   uint16_t *out_after_rbrack, bool *out_malformed) {
    if (out_malformed) *out_malformed = false;
    if (pos >= len || src[pos] != '[') return false;
    uint16_t p = (uint16_t)(pos + 1);
    while (p < len && (src[p] == ' ' || src[p] == '\t')) p++;
    if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;

    uint16_t id_start = p;
    while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
    uint16_t id_len = (uint16_t)(p - id_start);
    if (id_len == 0 || id_len >= 32u) return false;
    if (id_len == 3 && memcmp(src + id_start, "for", 3) == 0) return false;

    while (p < len && (src[p] == ' ' || src[p] == '\t')) p++;
    if (p < len && src[p] == ']') {
        uint16_t after = (uint16_t)(p + 1);
        uint16_t q     = after;
        while (q < len && (src[q] == ' ' || src[q] == '\t')) q++;
        if (q >= len || src[q] == '\n' || src[q] == '\r' || src[q] == '#' ||
            (q + 1 < len && src[q] == '<' && src[q + 1] == '[')) {
            if (out_id_start)     *out_id_start     = id_start;
            if (out_id_len)       *out_id_len       = id_len;
            if (out_after_rbrack) *out_after_rbrack = after;
            return true;
        }
        return false;
    }
    if (p >= len || src[p] == '\n' || src[p] == '\r' || src[p] == '#') {
        if (out_malformed) *out_malformed = true;
    }
    return false;
}

static uint16_t ext_peek_modifiers(const ExtLoader *L, uint16_t start_pos,
                                   uint8_t *out_flags, bool *out_has_mods,
                                   bool report_conflicts) {
    *out_flags    = 0;
    *out_has_mods = false;
    bool seen_pub = false, seen_priv = false, seen_const = false, seen_mut = false;
    uint16_t p = start_pos;

    while (p < L->len) {
        uint16_t ws = ext_skip_ws_at(L->src, p, L->len);
        if (ws >= L->len || !lex_is_ident_start((unsigned char)L->src[ws])) break;
        uint16_t q = ws;
        while (q < L->len && lex_is_ident_cont((unsigned char)L->src[q])) q++;
        uint16_t wlen = (uint16_t)(q - ws);
        const char *w = L->src + ws;
        if (!ext_is_mod_kw(w, wlen)) break;

        uint16_t r = ext_skip_ws_at(L->src, q, L->len);
        if (r >= L->len || L->src[r] == ';') break;

        if (wlen == 3 && memcmp(w, "pub", 3) == 0) {
            if (seen_priv && report_conflicts) {
                mem_err(L->mem, "Conflicting visibility modifiers 'pub' and 'priv'");
                return r;
            }
            seen_pub = true;
        } else if (wlen == 4 && memcmp(w, "priv", 4) == 0) {
            if (seen_pub && report_conflicts) {
                mem_err(L->mem, "Conflicting visibility modifiers 'pub' and 'priv'");
                return r;
            }
            seen_priv = true;
            *out_flags |= SYM_VAR_PRIVATE;
        } else if (wlen == 5 && memcmp(w, "const", 5) == 0) {
            if (seen_mut && report_conflicts) {
                mem_err(L->mem, "Conflicting mutability modifiers 'const' and 'mut'");
                return r;
            }
            seen_const = true;
            *out_flags |= SYM_VAR_EXPLICIT_CONST;
        } else if (wlen == 3 && memcmp(w, "mut", 3) == 0) {
            if (seen_const && report_conflicts) {
                mem_err(L->mem, "Conflicting mutability modifiers 'const' and 'mut'");
                return r;
            }
            seen_mut = true;
            *out_flags |= SYM_VAR_MUTABLE;
        }
        *out_has_mods = true;
        p = r;
    }
    return p;
}

static bool ext_peek_is_quoted_decl(const ExtLoader *L, uint16_t pos) {
    if (pos >= L->len || L->src[pos] != '"') return false;
    uint16_t p = (uint16_t)(pos + 1);
    while (p < L->len && L->src[p] != '"') {
        if (L->src[p] == '\\' && p + 1 < L->len) p = (uint16_t)(p + 2);
        else p++;
    }
    if (p >= L->len) return true; /* Unterminated quote: let ext_read_quoted report error */
    p = ext_skip_ws_at(L->src, (uint16_t)(p + 1), L->len);
    if (p >= L->len) return false;
    if (L->src[p] == '(' || L->src[p] == ':') return true;
    if (L->src[p] == '=' && !(p + 1 < L->len && L->src[p + 1] == '=')) return true;
    return false;
}

static bool ext_scan_one_param_parens(const char *src, uint16_t pos, uint16_t len, uint16_t *out_after) {
    uint16_t p = ext_skip_ws_at(src, pos, len);
    if (p >= len || src[p] != '(') return false;
    p = ext_skip_ws_at(src, (uint16_t)(p + 1), len);
    if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;
    while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
    p = ext_skip_ws_at(src, p, len);
    if (p < len && src[p] == ':') {
        p = ext_skip_ws_at(src, (uint16_t)(p + 1), len);
        if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;
        while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
        p = ext_skip_ws_at(src, p, len);
        if (p < len && src[p] == ',') {
            p = ext_skip_ws_at(src, (uint16_t)(p + 1), len);
            if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;
            while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
            p = ext_skip_ws_at(src, p, len);
        }
    }
    if (p >= len || src[p] != ')') return false;
    if (out_after) *out_after = (uint16_t)(p + 1);
    return true;
}

static bool ext_peek_is_op_decl(const ExtLoader *L, uint16_t pos) {
    uint16_t after_p1 = 0;
    if (!ext_scan_one_param_parens(L->src, pos, L->len, &after_p1)) return false;
    uint16_t p = ext_skip_ws_at(L->src, after_p1, L->len);
    if (p >= L->len) return false;

    if (L->src[p] == '"') {
        p++;
        while (p < L->len && L->src[p] != '"') {
            if (L->src[p] == '\\' && p + 1 < L->len) p = (uint16_t)(p + 2);
            else p++;
        }
        if (p >= L->len) return true;
        p++;
    } else if (lex_is_ident_start((unsigned char)L->src[p])) {
        while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
    } else {
        return false;
    }

    p = ext_skip_ws_at(L->src, p, L->len);
    if (p < L->len && L->src[p] == '(') {
        if (!ext_scan_one_param_parens(L->src, p, L->len, &p)) return false;
        p = ext_skip_ws_at(L->src, p, L->len);
    }
    return (p < L->len && L->src[p] == ':' && !(p + 1 < L->len && L->src[p + 1] == '='));
}

static bool ext_scan_decl_param_list(const char *src, uint16_t pos, uint16_t len, uint16_t *out_after_rparen) {
    uint16_t p = ext_skip_ws_at(src, pos, len);
    if (p >= len || src[p] != '(') return false;
    p = ext_skip_ws_at(src, (uint16_t)(p + 1), len);
    if (p < len && src[p] == ')') {
        if (out_after_rparen) *out_after_rparen = (uint16_t)(p + 1);
        return true;
    }
    while (p < len) {
        if (!lex_is_ident_start((unsigned char)src[p])) return false;
        while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
        p = ext_skip_ws_at(src, p, len);
        if (p < len && src[p] == ':' && !(p + 1 < len && src[p + 1] == '=')) {
            p = ext_skip_ws_at(src, (uint16_t)(p + 1), len);
            if (p >= len || !lex_is_ident_start((unsigned char)src[p])) return false;
            while (p < len && lex_is_ident_cont((unsigned char)src[p])) p++;
            p = ext_skip_ws_at(src, p, len);
        }
        if (p < len && src[p] == '=' &&
            !(p + 1 < len && (src[p + 1] == '=' || src[p + 1] == '>'))) {
            p = ext_skip_ws_at(src, (uint16_t)(p + 1), len);
            int depth = 0;
            while (p < len) {
                char c = src[p];
                if (c == '"') {
                    p++;
                    while (p < len && src[p] != '"') {
                        if (src[p] == '\\' && p + 1 < len) p = (uint16_t)(p + 2);
                        else p++;
                    }
                    if (p < len) p++;
                    continue;
                }
                if (c == '(' || c == '[' || c == '{') { depth++; p++; continue; }
                if (c == ')' || c == ']' || c == '}') {
                    if (depth == 0) break;
                    depth--; p++; continue;
                }
                if (depth == 0 && (c == ',' || c == ';' || c == '\n')) break;
                p++;
            }
            p = ext_skip_ws_at(src, p, len);
        }
        if (p < len && src[p] == ',') {
            p = ext_skip_ws_at(src, (uint16_t)(p + 1), len);
            continue;
        }
        break;
    }
    if (p >= len || src[p] != ')') return false;
    if (out_after_rparen) *out_after_rparen = (uint16_t)(p + 1);
    return true;
}

static bool ext_store_op_active(const SymTable *tab) {
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        if (tab->overloads[i].meta_tag == INTR_STORE &&
            sym_mod_enabled(tab, tab->overloads[i].mod_id)) {
            return true;
        }
    }
    return false;
}

static uint16_t ext_skip_opt_fn_kw(const ExtLoader *L, uint16_t pos) {
    if (pos >= L->len) return pos;
    uint16_t p = pos;
    if (L->src[p] == '$') p++;
    if (p + 2 < L->len && L->src[p] == 'f' && L->src[p + 1] == 'n' &&
        !lex_is_ident_cont((unsigned char)L->src[p + 2])) {
        uint16_t q = (uint16_t)(p + 2);
        while (q < L->len && (L->src[q] == ' ' || L->src[q] == '\t')) q++;
        if (q < L->len && lex_is_ident_start((unsigned char)L->src[q])) {
            return q;
        }
    }
    return pos;
}

static bool ext_peek_is_fn_decl(const ExtLoader *L, uint16_t pos) {
    if (pos >= L->len || !lex_is_ident_start((unsigned char)L->src[pos])) return false;
    uint16_t id_start = pos;
    uint16_t p        = pos;
    while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
    uint16_t id_len = (uint16_t)(p - id_start);
    if (ext_is_mod_kw(L->src + id_start, id_len)) {
        return false;
    }
    /* Support `Type::method(...) = ...` method declarations */
    if (p + 2 < L->len && L->src[p] == ':' && L->src[p + 1] == ':' &&
        lex_is_ident_start((unsigned char)L->src[p + 2])) {
        if (sym_find_type(L->tab, L->mem, L->src + id_start, id_len) != MEM_TYPE_VOID) {
            p = (uint16_t)(p + 2);
            while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
        }
    }


    while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
    if (p >= L->len) return false;

    /* Fallback if `[boot_control]` (`=` / `:=`) is not active yet: treat `ident : ... = ...` or `ident = ...` as decl */
    if (L->src[p] != '(') {
        if (sym_is_reserved_kw(L->tab, L->mem, L->src + id_start, id_len)) {
            return false;
        }
        if (!ext_store_op_active(L->tab) && (L->src[p] == ':' || L->src[p] == '=')) {
            return true;
        }
        return false;
    }

    uint16_t after_rparen = 0;
    if (!ext_scan_decl_param_list(L->src, p, L->len, &after_rparen)) return false;
    uint16_t q = ext_skip_ws_at(L->src, after_rparen, L->len);
    if (q >= L->len) return false;
    if (q + 1 < L->len && L->src[q] == '-' && L->src[q + 1] == '>') return true;
    if (L->src[q] == ':' && !(q + 1 < L->len && L->src[q + 1] == '=')) return true;
    if (L->src[q] == '=' && !(q + 1 < L->len && (L->src[q + 1] == '=' || L->src[q + 1] == '>'))) return true;
    return false;
}

static bool ext_peek_is_var_decl(const ExtLoader *L, uint16_t pos) {
    if (pos >= L->len || !lex_is_ident_start((unsigned char)L->src[pos])) return false;
    uint16_t id_start = pos;
    uint16_t p        = pos;
    while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
    uint16_t id_len = (uint16_t)(p - id_start);
    if (ext_is_mod_kw(L->src + id_start, id_len) ||
        sym_is_reserved_kw(L->tab, L->mem, L->src + id_start, id_len)) {
        return false;
    }
    while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
    if (p >= L->len) return false;
    if (L->src[p] == '=' && !(p + 1 < L->len && (L->src[p + 1] == '=' || L->src[p + 1] == '>'))) {
        return true;
    }
    if (L->src[p] == ':') {
        if (p + 1 < L->len && L->src[p + 1] == '=') return true;
        p++;
        while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
        if (p >= L->len || !lex_is_ident_start((unsigned char)L->src[p])) return false;
        while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
        while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
        if (p < L->len && L->src[p] == ',') {
            p++;
            while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
            if (p >= L->len || !lex_is_ident_start((unsigned char)L->src[p])) return false;
            while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
            while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
        }
        if (p < L->len && L->src[p] == '=' &&
            !(p + 1 < L->len && (L->src[p + 1] == '=' || L->src[p + 1] == '>'))) {
            return true;
        }
        if (p + 1 < L->len && L->src[p] == ':' && L->src[p + 1] == '=') {
            return true;
        }
    }
    return false;
}

static bool ext_peek_is_import_decl(const ExtLoader *L, uint16_t pos) {
    if (pos + 6 > L->len) return false;
    if (memcmp(L->src + pos, "import", 6) != 0) return false;
    if (pos + 6 < L->len && lex_is_ident_cont((unsigned char)L->src[pos + 6])) return false;
    uint16_t p = ext_skip_ws_at(L->src, (uint16_t)(pos + 6), L->len);
    if (p >= L->len) return false;
    return (L->src[p] == '"' || L->src[p] == '(' || lex_is_ident_start((unsigned char)L->src[p]));
}

bool ext_import_module_or_file(MemArena *mem, SymTable *tab,
                               const char *spec, uint16_t spec_len,
                               bool is_quoted_path, Value *out_val) {
    if (out_val) *out_val = val_make_f64(mem, mem->fast_real_type, 1.0);
    if (!mem || !tab || !spec || spec_len == 0) {
        if (mem) mem_err(mem, "Empty module or file path in 'import'");
        return false;
    }

    /* 1. If unquoted module identifier matches an already-loaded module, enable it */
    if (!is_quoted_path) {
        int mid = sym_find_mod_slice(tab, mem, spec, spec_len);
        if (mid >= 0) {
            sym_mod_active_mut(tab)[mid] = 1u;
            return true;
        }
    }

    char raw_path[120];
    uint16_t rlen = (spec_len < sizeof(raw_path) - 6u) ? spec_len : (uint16_t)(sizeof(raw_path) - 6u);
    memcpy(raw_path, spec, rlen);
    raw_path[rlen] = '\0';

    bool has_ext = (rlen >= 5 && memcmp(raw_path + rlen - 5, ".math", 5) == 0);
    char with_ext[128];
    if (has_ext) {
        snprintf(with_ext, sizeof(with_ext), "%s", raw_path);
    } else {
        snprintf(with_ext, sizeof(with_ext), "%s.math", raw_path);
    }

    char cands[6][256];
    int  cand_cnt = 0;
    snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s", with_ext);
    if (!has_ext) {
        snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s", raw_path);
    }
    if (mem->import_dir[0] != '\0') {
        snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s/%s", mem->import_dir, with_ext);
        if (!has_ext) {
            snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s/%s", mem->import_dir, raw_path);
        }
    }
    snprintf(cands[cand_cnt++], sizeof(cands[0]), "Library/%s", with_ext);
    snprintf(cands[cand_cnt++], sizeof(cands[0]), "patches/%s", with_ext);

    FILE *fp = NULL;
    const char *chosen = NULL;
    for (int i = 0; i < cand_cnt; ++i) {
        fp = fopen(cands[i], "rb");
        if (fp != NULL) {
            chosen = cands[i];
            break;
        }
    }
    if (!fp || !chosen) {
        mem_err(mem, "Cannot find imported module or file '%.*s'", (int)spec_len, spec);
        return false;
    }

    /* Extract normalized key (last 63 chars of chosen path) for include-once guard */
    size_t clen = strlen(chosen);
    const char *key = (clen > 63u) ? (chosen + (clen - 63u)) : chosen;
    for (uint8_t i = 0; i < mem->imported_file_count; ++i) {
        if (strcmp(mem->imported_files[i], key) == 0) {
            fclose(fp);
            return true; /* Already imported (idempotent / cycle-safe) */
        }
    }
    if (mem->imported_file_count < 16u) {
        snprintf(mem->imported_files[mem->imported_file_count++], 64, "%.63s", key);
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        mem_err(mem, "Failed to seek imported file '%s'", chosen);
        return false;
    }
    long fsz = ftell(fp);
    if (fsz < 0 || fsz > 65534L) {
        fclose(fp);
        mem_err(mem, "Imported file '%s' size (%ld) out of range", chosen, fsz);
        return false;
    }
    rewind(fp);
    char *fbuf = (char *)malloc((size_t)fsz + 1u);
    if (!fbuf) {
        fclose(fp);
        mem_err(mem, "Out of memory reading imported file '%s'", chosen);
        return false;
    }
    size_t nread = fread(fbuf, 1, (size_t)fsz, fp);
    fclose(fp);
    fbuf[nread] = '\0';

    /* Derive default module stem from filename (e.g. "dir/foo.math" -> "foo") */
    const char *slash = strrchr(chosen, '/');
    const char *base  = slash ? (slash + 1) : chosen;
    char stem[32];
    size_t blen = 0;
    while (base[blen] != '\0' && base[blen] != '.' && blen + 1u < sizeof(stem)) {
        stem[blen] = base[blen];
        blen++;
    }
    stem[blen] = '\0';
    if (stem[0] == '\0') snprintf(stem, sizeof(stem), "default");

    uint16_t saved_mem_mod = mem->cur_mod;
    uint16_t saved_tab_mod = tab->cur_mod;
    Value imp_val = val_void();
    bool ok = ext_eval_slice(mem, tab, stem, true, fbuf, (uint16_t)nread, &imp_val);
    free(fbuf);
    mem->cur_mod = saved_mem_mod;
    tab->cur_mod = saved_tab_mod;
    if (ok && !mem->has_error) {
        mem_snapshot_perm(mem);
        if (out_val && val_get_type(mem, imp_val) != MEM_TYPE_VOID) {
            *out_val = imp_val;
        }
        return true;
    }
    return false;
}

static bool ext_is_top_level_decl_at(const ExtLoader *L, uint16_t pos) {
    uint16_t p = ext_skip_ws_at(L->src, pos, L->len);
    if (p >= L->len) return false;

    bool malformed_hdr = false;
    if (L->src[p] == '[' &&
        (ext_peek_is_mod_header(L->src, p, L->len, NULL, NULL, NULL, &malformed_hdr) || malformed_hdr)) {
        return true;
    }
    if (L->src[p] == '<' && p + 1 < L->len && L->src[p + 1] == '[') return true;
    if (L->src[p] == '@') {
        uint16_t q = (uint16_t)(p + 1);
        while (q < L->len && lex_is_ident_cont((unsigned char)L->src[q])) q++;
        uint16_t dlen = (uint16_t)(q - (p + 1));
        if ((dlen == 3 && memcmp(L->src + p + 1, "dim", 3) == 0) ||
            (dlen == 8 && memcmp(L->src + p + 1, "unit_dim", 8) == 0)) {
            return true;
        }
    }

    uint8_t dummy_flags = 0;
    bool    dummy_mods  = false;
    uint16_t am = ext_peek_modifiers(L, p, &dummy_flags, &dummy_mods, false);
    if (am >= L->len) return false;

    (void)ext_peek_is_var_decl;
    uint16_t fn_pos = ext_skip_opt_fn_kw(L, am);
    if (fn_pos != am && ext_peek_is_fn_decl(L, fn_pos)) return true;
    char c = L->src[am];
    if (c == '$') return true;
    if (ext_peek_is_kw_type_decl(L, am))            return true;
    if (ext_peek_is_import_decl(L, am))             return true;
    if (c == '"' && ext_peek_is_quoted_decl(L, am)) return true;
    if (c == '(' && ext_peek_is_op_decl(L, am))     return true;
    if (lex_is_ident_start((unsigned char)c) && ext_peek_is_fn_decl(L, am)) return true;
    return false;
}

static uint16_t ext_find_stmt_chunk_end(const ExtLoader *L) {
    uint16_t p     = L->pos;
    uint16_t len   = L->len;
    int      depth = 0;

    while (p < len) {
        char c = L->src[p];
        if (c == '"') {
            p++;
            while (p < len && L->src[p] != '"') {
                if (L->src[p] == '\\' && p + 1 < len) p = (uint16_t)(p + 2);
                else p++;
            }
            if (p < len) p++;
            continue;
        }
        if (c == '#') {
            while (p < len && L->src[p] != '\n') p++;
            continue;
        }
        if (c == '(' || c == '{' || c == '[') {
            depth++;
            p++;
            continue;
        }
        if (c == ')' || c == '}' || c == ']') {
            if (depth > 0) depth--;
            p++;
            continue;
        }
        if (depth == 0 && (c == '\n' || c == ';')) {
            uint16_t boundary = p;
            p++;
            while (p < len) {
                while (p < len && (L->src[p] == ' ' || L->src[p] == '\t' || L->src[p] == '\r')) p++;
                if (p < len && L->src[p] == '#') {
                    while (p < len && L->src[p] != '\n') p++;
                }
                if (p < len && (L->src[p] == '\n' || L->src[p] == ';')) {
                    p++;
                    continue;
                }
                break;
            }
            if (p < len && ext_is_top_level_decl_at(L, p)) {
                return boundary;
            }
            continue;
        }
        p++;
    }
    return len;
}

/* ============================================================================
 * TOP-TO-BOTTOM MODULE SLICE EVALUATION & COMPILATION (`ext_eval_slice`)
 * ============================================================================ */

bool ext_eval_slice(MemArena *mem, SymTable *tab,
                    const char *default_mod_name, bool allow_redef,
                    const char *slice_src, uint16_t slice_len,
                    Value *out_val) {
    if (out_val) *out_val = val_void();
    if (!mem || !tab) return false;
    mem->sym_tab = tab;

    ExtLoader L;
    memset(&L, 0, sizeof(L));
    L.mem = mem;
    L.tab = tab;
    L.src = slice_src;
    L.len = slice_len;
    L.pos = 0;

    const char *eff_mod = (default_mod_name && default_mod_name[0] != '\0')
                          ? default_mod_name : "default";
    StrSlice def_mod = mem_intern_str(mem, eff_mod, (uint16_t)strlen(eff_mod));
    if (mem->has_error) return false;
    L.cur_mod = sym_find_or_add_mod(tab, mem, def_mod);
    if (mem->has_error) return false;

    uint16_t prev_mem_mod = mem->cur_mod;
    uint16_t prev_tab_mod = tab->cur_mod;
    mem->cur_mod = L.cur_mod;
    tab->cur_mod = L.cur_mod;

    Value last_val      = val_void();
    bool  had_any_token = false;

    while (L.pos < L.len && !mem->has_error) {
        ext_skip_ws(&L);
        while (L.pos < L.len && L.src[L.pos] == ';') {
            L.pos++;
            ext_skip_ws(&L);
        }
        if (L.pos >= L.len) break;
        had_any_token = true;

        /* 1. `[module]` Header */
        if (L.src[L.pos] == '[') {
            uint16_t id_start    = 0;
            uint16_t id_len      = 0;
            uint16_t after_rbrck = 0;
            bool     malformed   = false;
            if (ext_peek_is_mod_header(L.src, L.pos, L.len, &id_start, &id_len, &after_rbrck, &malformed)) {
                StrSlice mname = mem_intern_str(mem, L.src + id_start, id_len);
                if (mem->has_error) break;
                L.cur_mod    = sym_find_or_add_mod(tab, mem, mname);
                mem->cur_mod = L.cur_mod;
                tab->cur_mod = L.cur_mod;
                L.pos        = after_rbrck;
                continue;
            }
            if (malformed) {
                mem_err(mem, "Expected closing ']' in module header");
                break;
            }
        }

        /* 1b. `import "file.math"` or `import mod_name` Directive */
        if (ext_peek_is_import_decl(&L, L.pos)) {
            L.pos = (uint16_t)(L.pos + 6);
            ext_skip_ws(&L);
            bool has_paren = ext_match_char(&L, '(');
            ext_skip_ws(&L);
            StrSlice spec = { 0, 0 };
            bool is_quoted = false;
            if (L.pos < L.len && L.src[L.pos] == '"') {
                is_quoted = true;
                spec = ext_read_quoted(&L);
            } else {
                spec = ext_read_ident(&L);
            }
            if (has_paren) {
                ext_skip_ws(&L);
                if (!ext_match_char(&L, ')')) {
                    mem_err(mem, "Expected ')' after import argument");
                    break;
                }
            }
            if (mem->has_error) break;
            if (!ext_import_module_or_file(mem, tab, mem_str_ptr(mem, spec), spec.len, is_quoted, &last_val)) {
                break;
            }
            continue;
        }

        /* 2. `<[dep]` Dependency Header */
        if (L.src[L.pos] == '<' && L.pos + 1 < L.len && L.src[L.pos + 1] == '[') {
            L.pos = (uint16_t)(L.pos + 2);
            StrSlice dep = ext_read_ident(&L);
            if (!ext_match_char(&L, ']') || dep.len == 0) {
                mem_err(mem, "Expected closing ']' in module dependency");
                break;
            }
            uint16_t dep_id = sym_find_or_add_mod(tab, mem, dep);
            if (mem->has_error) break;
            sym_mod_active_mut(tab)[dep_id] = 1u;
            continue;
        }

        /* 3. `@dim(...)` / `@unit_dim(...)` Physical Dimensional Directive */
        if (L.src[L.pos] == '@') {
            uint16_t q = (uint16_t)(L.pos + 1);
            while (q < L.len && lex_is_ident_cont((unsigned char)L.src[q])) q++;
            uint16_t dlen = (uint16_t)(q - (L.pos + 1));
            if ((dlen == 3 && memcmp(L.src + L.pos + 1, "dim", 3) == 0) ||
                (dlen == 8 && memcmp(L.src + L.pos + 1, "unit_dim", 8) == 0)) {
                L.pos = q;
                if (!ext_match_char(&L, '(')) {
                    mem_err(mem, "Expected '(' after '@dim'");
                    break;
                }
                StrSlice uname = ext_read_ident(&L);
                if (uname.len == 0) {
                    mem_err(mem, "Expected unit group name in '@dim(...)'");
                    break;
                }
                uint8_t uid = sym_find_or_add_unit(tab, mem, mem_str_ptr(mem, uname), uname.len);
                if (mem->has_error || uid == MEM_UNIT_NONE) break;
                UnitDimDef udim;
                memset(&udim, 0, sizeof(udim));
                udim.has_dim = 1;
                int idx = 0;
                while (L.pos < L.len && !mem->has_error && idx < 7) {
                    if (!ext_match_char(&L, ',')) break;
                    ext_skip_ws(&L);
                    char *endp = NULL;
                    long val = strtol(L.src + L.pos, &endp, 10);
                    if (endp == L.src + L.pos) {
                        mem_err(mem, "Expected integer exponent in '@dim(...)'");
                        break;
                    }
                    L.pos = (uint16_t)(endp - L.src);
                    udim.exp[idx++] = (int8_t)val;
                }
                if (mem->has_error) break;
                if (!ext_match_char(&L, ')')) {
                    mem_err(mem, "Expected ')' at end of '@dim(...)'");
                    break;
                }
                tab->unit_dims[uid] = udim;
                mem_snapshot_perm(mem);
                continue;
            }
        }

        /* 4. Peek optional `pub`, `priv`, `const`, `mut` modifiers for declarations */
        uint8_t  decl_flags = 0;
        bool     has_mods   = false;
        uint16_t after_mods = ext_peek_modifiers(&L, L.pos, &decl_flags, &has_mods, true);
        if (mem->has_error) break;

        if (after_mods < L.len) {
            uint16_t fn_pos = ext_skip_opt_fn_kw(&L, after_mods);
            if (fn_pos != after_mods && ext_peek_is_fn_decl(&L, fn_pos)) {
                after_mods = fn_pos;
            }
            char mc = L.src[after_mods];

            /* 4a. `$type`, `$struct`, `$enum`, `struct`, or `enum` Declaration */
            if (mc == '$' || ext_peek_is_kw_type_decl(&L, after_mods)) {
                L.pos = after_mods;
                ext_parse_type_decl(&L, decl_flags);
                if (!mem->has_error) mem_snapshot_perm(mem);
                continue;
            }

            /* 4b. Quoted Symbol Declaration (`"(" => @lparen`, `"g0" : real = ...`, `"sin"(x:real) = ...`) */
            if (mc == '"' && ext_peek_is_quoted_decl(&L, after_mods)) {
                L.pos = after_mods;
                StrSlice sym = ext_read_quoted(&L);
                if (mem->has_error) break;
                uint16_t sym_id = sym_intern(tab, mem, sym);
                if (mem->has_error) break;
                ext_parse_sym_decl(&L, sym_id, decl_flags, allow_redef);
                if (!mem->has_error) mem_snapshot_perm(mem);
                continue;
            }

            /* 4c. Unquoted Function, Type Method (`Type::fn(self) = ...`), or Prefix Operator Declaration */
            if (lex_is_ident_start((unsigned char)mc) && ext_peek_is_fn_decl(&L, after_mods)) {
                L.pos = after_mods;
                StrSlice sym = ext_read_ident(&L);
                if (mem->has_error || sym.len == 0) break;
                uint16_t assoc_tid = 0;
                if (L.pos + 2 <= L.len && L.src[L.pos] == ':' && L.src[L.pos + 1] == ':') {
                    assoc_tid = sym_find_type(tab, mem, mem_str_ptr(mem, sym), sym.len);
                    L.pos = (uint16_t)(L.pos + 2);
                    sym = ext_read_ident(&L);
                    if (mem->has_error || sym.len == 0) break;
                }
                uint16_t sym_id = sym_intern(tab, mem, sym);
                if (mem->has_error) break;
                ext_parse_sym_decl_ex(&L, sym_id, decl_flags, allow_redef, assoc_tid);
                if (!mem->has_error) mem_snapshot_perm(mem);
                continue;
            }

            /* 4d. Postfix or Infix Operator Declaration (`(a:T) "+" (b:T) :<50 = ...`) */
            if (mc == '(' && ext_peek_is_op_decl(&L, after_mods)) {
                if ((decl_flags & (SYM_VAR_MUTABLE | SYM_VAR_EXPLICIT_CONST)) != 0) {
                    mem_err(mem, "Modifiers 'const' and 'mut' cannot be applied to operator declarations");
                    break;
                }
                L.pos = after_mods;
                StrSlice pnames[SYM_MAX_OV_PARAMS];
                uint16_t ptypes[SYM_MAX_OV_PARAMS];
                uint8_t  punits[SYM_MAX_OV_PARAMS];
                if (!ext_parse_one_param(&L, &pnames[0], &ptypes[0], &punits[0])) break;

                ext_skip_ws(&L);
                StrSlice sym = (L.pos < L.len && L.src[L.pos] == '"')
                               ? ext_read_quoted(&L)
                               : ext_read_ident(&L);
                if (mem->has_error || sym.len == 0) break;
                uint16_t sym_id = sym_intern(tab, mem, sym);
                if (mem->has_error) break;
                ext_skip_ws(&L);

                uint8_t pcount = 1;
                uint8_t fixity = FIX_POSTFIX;
                uint8_t prec   = 0;

                if (L.pos < L.len && L.src[L.pos] == '(') {
                    if (!ext_parse_one_param(&L, &pnames[1], &ptypes[1], &punits[1])) break;
                    pcount = 2;
                }

                if (!ext_match_char(&L, ':')) {
                    mem_err(mem, "Expected ':prec', ':<prec', or ':>prec' in operator declaration");
                    break;
                }
                if (pcount == 2) {
                    if (ext_match_char(&L, '<')) fixity = FIX_INFIX_LEFT;
                    else if (ext_match_char(&L, '>')) fixity = FIX_INFIX_RIGHT;
                    else fixity = FIX_INFIX_LEFT;
                } else {
                    fixity = FIX_POSTFIX;
                }

                char *endp = NULL;
                prec = (uint8_t)strtol(L.src + L.pos, &endp, 10);
                L.pos = (uint16_t)(endp - L.src);

                uint16_t rtid = 0;
                uint8_t  ruid = SYM_UNIT_OMITTED;
                if (!ext_parse_return_spec(&L, &rtid, &ruid)) break;
                ext_match_char(&L, '=');

                int dup_idx = ext_find_duplicate_overload(tab, sym_id, L.cur_mod, fixity, pcount, ptypes, punits);
                if (dup_idx >= 0 && !allow_redef) {
                    StrSlice s = tab->syms[sym_id];
                    mem_err(mem, "Duplicate declaration of '%.*s' in module '[%.*s]'",
                            (int)s.len, mem_str_ptr(mem, s),
                            (int)sym_mod_name(tab, L.cur_mod).len,
                            mem_str_ptr(mem, sym_mod_name(tab, L.cur_mod)));
                    break;
                }

                uint16_t ov_idx = 0;
                if (dup_idx >= 0) {
                    ov_idx = (uint16_t)dup_idx;
                } else {
                    if (tab->overload_count >= SYM_MAX_OVERLOADS) {
                        mem_err(mem, "Overload table overflow (max %d)", SYM_MAX_OVERLOADS);
                        break;
                    }
                    ov_idx = tab->overload_count++;
                }
                OpOverload *ov     = &tab->overloads[ov_idx];
                memset(ov, 0, sizeof(*ov));
                ov->sym_id         = sym_id;
                ov->mod_id         = L.cur_mod;
                ov->fixity         = fixity;
                ov->prec           = prec;
                ov->param_count    = pcount;
                ov->ret_type       = (uint8_t)rtid;
                ov->ret_unit       = ruid;
                ov->reserved       = (decl_flags & SYM_VAR_PRIVATE) ? SYM_OV_PRIVATE : 0;
                ov->param_types[0] = (uint8_t)ptypes[0];
                ov->param_units[0] = punits[0];
                if (pcount == 2) {
                    ov->param_types[1] = (uint8_t)ptypes[1];
                    ov->param_units[1] = punits[1];
                }
                if (dup_idx < 0) {
                    sym_link_overload(tab, ov_idx);
                }

                ext_compile_body(&L, ov, pnames);
                if (!mem->has_error) mem_snapshot_perm(mem);
                continue;
            }
        }

        /* 5. Top-to-Bottom Executable Module Statement / Expression Chunk */
        uint16_t chunk_end = ext_find_stmt_chunk_end(&L);
        uint16_t saved_ast = mem->ast_count;

        Parser P;
        parser_init(&P, mem, tab, L.src, chunk_end, L.pos, false, false);
        P.is_mod_scope = true;
        P.cur_mod      = L.cur_mod;

        lex_next(&P.lex);
        uint16_t expr_root = parser_parse_expr(&P, 0);
        if (!mem->has_error && P.lex.cur.kind != TOK_EOF) {
            mem_err(mem, "Unexpected trailing token '%.*s'",
                    (int)P.lex.cur.len, L.src + P.lex.cur.off);
        }
        if (!parser_check_labels(&P) || mem->has_error || expr_root == MEM_NO_IDX) {
            break;
        }
        L.pos = chunk_end;

        uint16_t saved_labels[MEM_MAX_LABELS];
        memcpy(saved_labels, mem->label_ast, sizeof(saved_labels));
        for (uint8_t i = 0; i < MEM_MAX_LABELS; ++i) {
            mem->label_ast[i] = (i < P.label_count) ? P.labels[i].body_ast : MEM_NO_IDX;
        }

        uint16_t frame_base = 0;
        if (P.local_count > 0) {
            frame_base = mem_push_frame(mem, P.local_count);
            if (mem->has_error) {
                memcpy(mem->label_ast, saved_labels, sizeof(saved_labels));
                break;
            }
            for (uint8_t i = 0; i < P.local_count; ++i) {
                mem_set_slot(mem, frame_base, i, val_make_f64(mem, mem->fast_real_type, 0.0));
            }
        }

        mem->eval_steps  = 0;
        mem->eval_depth  = 0;
        mem->call_depth  = 0;
        mem->jump_active = JUMP_NONE;

        Value step_val = eval_ast(mem, tab, expr_root, frame_base, 0);
        bool early_return = false;
        if (mem->jump_active == JUMP_RETURN) {
            step_val         = mem->jump_val;
            mem->jump_val    = val_void();
            mem->jump_active = JUMP_NONE;
            early_return     = true;
        } else if (mem->jump_active == JUMP_THROW) {
            mem->jump_active = JUMP_NONE;
            if (!mem->has_error) {
                mem_err(mem, "Unhandled exception");
            }
        } else if (mem->jump_active != JUMP_NONE && !mem->has_error) {
            mem->jump_active = JUMP_NONE;
            mem_err(mem, "Unhandled jump, break, or continue outside active loop/scope");
        }

        if (P.local_count > 0) {
            mem_pop_frame(mem, frame_base);
        }
        memcpy(mem->label_ast, saved_labels, sizeof(saved_labels));

        if (mem->perm_ast_count <= saved_ast) {
            mem->ast_count = saved_ast;
        }

        if (mem->has_error) break;
        last_val = step_val;
        if (early_return) break;
    }

    if (!had_any_token && !mem->has_error && allow_redef) {
        mem_err(mem, "Empty expression");
    }

    if (mem->has_error) {
        /* Clean up any uninitialized parse-time placeholder variables on error */
        ModVarDef *vars = sym_vars_mut(tab);
        uint16_t w = 0;
        for (uint16_t i = 0; i < tab->var_count; ++i) {
            if (val_get_type(mem, vars[i].val) != MEM_TYPE_VOID) {
                if (w != i) vars[w] = vars[i];
                w++;
            }
        }
        tab->var_count = w;
    }

    mem->cur_mod      = prev_mem_mod;
    tab->cur_mod      = prev_tab_mod;
    mem->last_mod_val = last_val;
    if (out_val) *out_val = last_val;
    return !mem->has_error;
}

bool ext_compile_slice(MemArena *mem, SymTable *tab,
                       const char *slice_src, uint16_t slice_len) {
    return ext_eval_slice(mem, tab, "default", false, slice_src, slice_len, NULL);
}

/* ============================================================================
 * MODULE UNIT STAGING (`ext_build_init`, `ext_build_create`, `ext_compile_unit`)
 * ============================================================================ */

void ext_build_free_heap(ExtBuildCtx *ctx) {
    if (!ctx) return;
    if (ctx->heap_magic == EXT_BUILD_HEAP_MAGIC) {
        ExtCompiledUnit *units = ext_ctx_units_mut(ctx);
        for (uint16_t i = 0; i < ctx->unit_count; ++i) {
            if (units[i].heap_deps != NULL) {
                free(units[i].heap_deps);
                units[i].heap_deps = NULL;
            }
        }
        if (ctx->heap_units != NULL) {
            free(ctx->heap_units);
            ctx->heap_units = NULL;
        }
    }
    ctx->heap_units = NULL;
    ctx->heap_magic = 0;
    ctx->unit_cap   = EXT_SBO_BUILD_UNITS;
}

void ext_build_init(ExtBuildCtx *ctx) {
    if (!ctx) return;
    ext_build_free_heap(ctx);
    ctx->heap_magic = EXT_BUILD_HEAP_MAGIC;
    ctx->unit_cap   = EXT_SBO_BUILD_UNITS;
    ctx->unit_count = 0;
    ctx->pool_used  = 0;
    ctx->has_error  = false;
    ctx->err_msg[0] = '\0';
}

ExtBuildCtx *ext_build_create(void) {
    ExtBuildCtx *ctx = (ExtBuildCtx *)malloc(sizeof(ExtBuildCtx));
    if (ctx) {
        memset(ctx, 0, sizeof(ExtBuildCtx));
        ext_build_init(ctx);
    }
    return ctx;
}

void ext_build_destroy(ExtBuildCtx *ctx) {
    if (!ctx) return;
    ext_build_free_heap(ctx);
    free(ctx);
}

static bool build_fail(MemArena *mem, ExtBuildCtx *ctx, const char *msg) {
    if (mem) mem_err(mem, "%s", msg);
    if (ctx && !ctx->has_error) {
        ctx->has_error = true;
        snprintf(ctx->err_msg, sizeof(ctx->err_msg), "%s", mem ? mem->err_msg : msg);
    }
    return false;
}

static int ext_alloc_unit(MemArena *mem, ExtBuildCtx *ctx) {
    if (ctx->heap_magic != EXT_BUILD_HEAP_MAGIC || ctx->unit_cap == 0) {
        ctx->heap_magic = EXT_BUILD_HEAP_MAGIC;
        ctx->heap_units = NULL;
        ctx->unit_cap   = EXT_SBO_BUILD_UNITS;
    }
    if (ctx->unit_count >= 0xFFFEu) {
        build_fail(mem, ctx, "Compilation unit index overflow");
        return -1;
    }
    if (ctx->unit_count >= ctx->unit_cap) {
        uint32_t new_cap = (uint32_t)ctx->unit_cap * 2u;
        if (new_cap > 0xFFFEu) new_cap = 0xFFFEu;
        ExtCompiledUnit *new_units = (ExtCompiledUnit *)calloc(new_cap, sizeof(ExtCompiledUnit));
        if (!new_units) {
            build_fail(mem, ctx, "Out of memory expanding compilation unit table");
            return -1;
        }
        memcpy(new_units, ext_ctx_units(ctx), (size_t)ctx->unit_count * sizeof(ExtCompiledUnit));
        if (ctx->heap_units != NULL) {
            free(ctx->heap_units);
        }
        ctx->heap_units = new_units;
        ctx->unit_cap   = (uint16_t)new_cap;
    }
    int idx = (int)ctx->unit_count++;
    ExtCompiledUnit *u = &ext_ctx_units_mut(ctx)[idx];
    memset(u, 0, sizeof(*u));
    u->dep_cap = EXT_SBO_UNIT_DEPS;
    return idx;
}

static bool ext_add_unit_dep(MemArena *mem, ExtBuildCtx *ctx, ExtCompiledUnit *u,
                             const char *dname, uint16_t id_len) {
    const char (*deps)[32] = ext_unit_deps(u);
    for (uint16_t d = 0; d < u->dep_count; ++d) {
        if (strcmp(deps[d], dname) == 0) {
            return true;
        }
    }
    if (u->dep_cap == 0) u->dep_cap = EXT_SBO_UNIT_DEPS;
    if (u->dep_count >= 0xFFFEu) {
        return build_fail(mem, ctx, "Module dependency index overflow");
    }
    if (u->dep_count >= u->dep_cap) {
        uint32_t new_cap = (uint32_t)u->dep_cap * 2u;
        if (new_cap > 0xFFFEu) new_cap = 0xFFFEu;
        char (*new_deps)[32] = (char (*)[32])calloc(new_cap, sizeof(*new_deps));
        if (!new_deps) {
            return build_fail(mem, ctx, "Out of memory expanding module dependencies");
        }
        memcpy(new_deps, ext_unit_deps(u), (size_t)u->dep_count * sizeof(*new_deps));
        if (u->heap_deps != NULL) {
            free(u->heap_deps);
        }
        u->heap_deps = new_deps;
        u->dep_cap   = (uint16_t)new_cap;
    }
    memcpy(ext_unit_deps_mut(u)[u->dep_count++], dname, (size_t)id_len + 1u);
    return true;
}

bool ext_compile_unit(MemArena *mem, const SymTable *tab,
                      ExtBuildCtx *ctx, const char *raw_math_text) {
    if (!mem || !tab || !ctx) return false;
    mem_clear_err(mem);
    if (ctx->has_error) {
        mem_err(mem, "%s", ctx->err_msg);
        return false;
    }
    if (!raw_math_text) {
        return build_fail(mem, ctx, "Null extension source string");
    }
    size_t raw_len = strlen(raw_math_text);
    if (raw_len > 65534u) {
        return build_fail(mem, ctx, "Extension source length exceeds maximum (65534 bytes)");
    }
    if (ctx->pool_used + (uint32_t)raw_len + 1u > EXT_STAGE_POOL_CAP) {
        return build_fail(mem, ctx, "Extension staging pool overflow");
    }

    uint32_t base_off = ctx->pool_used;
    memcpy(ctx->src_pool + base_off, raw_math_text, raw_len + 1u);
    ctx->pool_used += (uint32_t)raw_len + 1u;

    const char *src = ctx->src_pool + base_off;
    uint16_t    len = (uint16_t)raw_len;
    uint16_t    pos = 0;

    int  cur_u          = -1;
    int  depth          = 0;
    bool in_pattern     = false;
    bool at_stmt_start  = true;

    const StrSlice *loaded_mods = sym_mod_names(tab);

    while (pos < len) {
        /* 1. Skip horizontal whitespace */
        while (pos < len && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r')) {
            pos++;
        }
        if (pos >= len) break;

        /* 2. Newline resets top-level statement boundary */
        if (src[pos] == '\n') {
            pos++;
            if (depth == 0 && !in_pattern) {
                at_stmt_start = true;
            }
            continue;
        }

        /* 3. Line comment */
        if (src[pos] == '#') {
            while (pos < len && src[pos] != '\n') pos++;
            continue;
        }

        /* 4. Top-level `[module]` header or `<[dep]` dependency */
        if (at_stmt_start && depth == 0 && !in_pattern) {
            if (src[pos] == '[') {
                uint16_t hdr_start   = pos;
                uint16_t id_start    = 0;
                uint16_t id_len      = 0;
                uint16_t after_rbrck = 0;
                bool     malformed   = false;
                if (ext_peek_is_mod_header(src, pos, len, &id_start, &id_len, &after_rbrck, &malformed)) {
                    pos = after_rbrck;
                    char mname[32];
                    memcpy(mname, src + id_start, id_len);
                    mname[id_len] = '\0';
                    bool is_def = (strcmp(mname, "default") == 0);

                    if (!is_def) {
                        /* Reject if module is already loaded in SymTable */
                        for (uint16_t i = 0; i < tab->mod_count; ++i) {
                            if (mem_slice_eq_cstr(mem, loaded_mods[i], mname)) {
                                mem_err(mem, "Module '[%s]' is already loaded", mname);
                                ctx->has_error = true;
                                snprintf(ctx->err_msg, sizeof(ctx->err_msg), "%s", mem->err_msg);
                                return false;
                            }
                        }
                        /* Reject if module is already staged in ExtBuildCtx */
                        const ExtCompiledUnit *staged = ext_ctx_units(ctx);
                        for (uint16_t i = 0; i < ctx->unit_count; ++i) {
                            if (!staged[i].is_default_mod &&
                                strcmp(staged[i].mod_name, mname) == 0) {
                                mem_err(mem, "Module '[%s]' is already loaded", mname);
                                ctx->has_error = true;
                                snprintf(ctx->err_msg, sizeof(ctx->err_msg), "%s", mem->err_msg);
                                return false;
                            }
                        }
                    }

                    if (cur_u >= 0) {
                        ExtCompiledUnit *prev = &ext_ctx_units_mut(ctx)[cur_u];
                        prev->src_len = (uint16_t)((base_off + hdr_start) - prev->src_off);
                    }
                    cur_u = ext_alloc_unit(mem, ctx);
                    if (cur_u < 0) return false;
                    ExtCompiledUnit *u = &ext_ctx_units_mut(ctx)[cur_u];
                    memcpy(u->mod_name, mname, (size_t)id_len + 1u);
                    u->is_default_mod = is_def;
                    u->src_off        = base_off + hdr_start;
                    u->src_len        = 0;
                    at_stmt_start     = true;
                    continue;
                }
                if (malformed) {
                    return build_fail(mem, ctx, "Expected closing ']' in module header");
                }
            }

            if (src[pos] == '<' && pos + 1 < len && src[pos + 1] == '[') {
                uint16_t dep_stmt_start = pos;
                if (cur_u < 0) {
                    cur_u = ext_alloc_unit(mem, ctx);
                    if (cur_u < 0) return false;
                    ExtCompiledUnit *u = &ext_ctx_units_mut(ctx)[cur_u];
                    memcpy(u->mod_name, "default", 8);
                    u->is_default_mod = true;
                    u->src_off        = base_off + dep_stmt_start;
                    u->src_len        = 0;
                }
                pos = (uint16_t)(pos + 2);
                while (pos < len && (src[pos] == ' ' || src[pos] == '\t')) pos++;
                uint16_t id_start = pos;
                while (pos < len && lex_is_ident_cont((unsigned char)src[pos])) pos++;
                uint16_t id_len = (uint16_t)(pos - id_start);
                while (pos < len && (src[pos] == ' ' || src[pos] == '\t')) pos++;

                if (id_len == 0 || id_len >= 32u || pos >= len || src[pos] != ']') {
                    return build_fail(mem, ctx, "Expected closing ']' in module dependency");
                }
                pos++; /* consume ']' */

                char dname[32];
                memcpy(dname, src + id_start, id_len);
                dname[id_len] = '\0';

                ExtCompiledUnit *u = &ext_ctx_units_mut(ctx)[cur_u];
                if (!ext_add_unit_dep(mem, ctx, u, dname, id_len)) {
                    return false;
                }
                at_stmt_start = true;
                continue;
            }
        }

        /* 5. Any declaration/statement before an explicit `[module]` header creates an implicit `[default]` unit */
        if (cur_u < 0) {
            cur_u = ext_alloc_unit(mem, ctx);
            if (cur_u < 0) return false;
            ExtCompiledUnit *u = &ext_ctx_units_mut(ctx)[cur_u];
            memcpy(u->mod_name, "default", 8);
            u->is_default_mod = true;
            u->src_off        = base_off + pos;
            u->src_len        = 0;
        }

        /* 6. Advance over string literals, consuming patterns, and bracketed scopes */
        if (src[pos] == '"') {
            pos++;
            while (pos < len && src[pos] != '"') {
                if (src[pos] == '\\' && pos + 1 < len) pos = (uint16_t)(pos + 2);
                else pos++;
            }
            if (pos >= len || src[pos] != '"') {
                return build_fail(mem, ctx, "Unterminated quoted string symbol in declaration");
            }
            pos++;
            at_stmt_start = false;
        } else if (in_pattern) {
            if (src[pos] == '[') {
                pos = (uint16_t)(pos + 1);
                while (pos < len && src[pos] != ']') {
                    if (src[pos] == '\\' && pos + 1 < len) pos = (uint16_t)(pos + 2);
                    else pos++;
                }
                if (pos < len) pos++;
            } else if (src[pos] == '=' && pos + 1 < len && src[pos + 1] == '>') {
                pos = (uint16_t)(pos + 2);
                in_pattern = false;
            } else {
                pos++;
            }
        } else {
            if (src[pos] == '$' && depth == 0) {
                /* Consuming type `~` only applies inside `$type` declarations */
                pos++;
                while (pos < len && lex_is_ident_cont((unsigned char)src[pos])) pos++;
                while (pos < len && (src[pos] == ' ' || src[pos] == '\t')) pos++;
                if (pos < len && src[pos] == '~') {
                    in_pattern = true;
                    pos++;
                }
                at_stmt_start = false;
            } else if (src[pos] == '(' || src[pos] == '{' || src[pos] == '[') {
                depth++;
                at_stmt_start = false;
                pos++;
            } else if (src[pos] == ')' || src[pos] == '}' || src[pos] == ']') {
                if (depth > 0) depth--;
                at_stmt_start = false;
                pos++;
            } else if (src[pos] == ';' && depth == 0) {
                at_stmt_start = true;
                pos++;
            } else {
                at_stmt_start = false;
                pos++;
            }
        }
    }

    if (cur_u >= 0) {
        ExtCompiledUnit *last = &ext_ctx_units_mut(ctx)[cur_u];
        last->src_len = (uint16_t)((base_off + len) - last->src_off);
    }
    return true;
}
