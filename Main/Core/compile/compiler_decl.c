/*
 * compiler_decl.c — Declaration Scanners & Type/Symbol/Operator Overload Parsers
 */
#include "compiler_internal.h"

uint16_t ext_skip_ws_at(const char *src, uint16_t p, uint16_t len) {
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

void ext_skip_ws(ExtLoader *L) {
    L->pos = ext_skip_ws_at(L->src, L->pos, L->len);
}

bool ext_match_char(ExtLoader *L, char expected) {
    ext_skip_ws(L);
    if (L->pos < L->len && L->src[L->pos] == expected) {
        L->pos++;
        return true;
    }
    return false;
}

StrSlice ext_read_ident(ExtLoader *L) {
    ext_skip_ws(L);
    uint16_t start = L->pos;
    while (L->pos < L->len && lex_is_ident_cont((unsigned char)L->src[L->pos])) {
        L->pos++;
    }
    return mem_intern_str(L->mem, L->src + start, (uint16_t)(L->pos - start));
}

StrSlice ext_read_quoted(ExtLoader *L) {
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

int ext_find_duplicate_overload(const SymTable *tab, uint16_t sym_id, uint16_t mod_id,
                                       uint8_t fixity, uint8_t pcount,
                                       const uint16_t *ptypes, const uint8_t *punits) {
    if (sym_id >= tab->sym_count) return -1;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        const OpOverload *ov = &sym_overloads(tab)[i];
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

bool ext_peek_is_kw_type_decl(const ExtLoader *L, uint16_t pos) {
    if (pos >= L->len || !lex_is_ident_start((unsigned char)L->src[pos])) return false;
    uint16_t p = pos;
    while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
    uint16_t klen = (uint16_t)(p - pos);
    if (klen != 6 || memcmp(L->src + pos, "struct", 6) != 0) return false;
    while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
    if (p >= L->len || !lex_is_ident_start((unsigned char)L->src[p])) return false;
    while (p < L->len && lex_is_ident_cont((unsigned char)L->src[p])) p++;
    while (p < L->len && (L->src[p] == ' ' || L->src[p] == '\t')) p++;
    return (p < L->len && L->src[p] == '(');
}

void ext_parse_type_decl(ExtLoader *L, uint8_t decl_flags) {
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

    if (tname.len == 6 && memcmp(mem_str_ptr(mem, tname), "struct", 6) == 0) {
        uint16_t next_p = L->pos;
        while (next_p < L->len && (L->src[next_p] == ' ' || L->src[next_p] == '\t')) next_p++;
        if (next_p < L->len && lex_is_ident_start((unsigned char)L->src[next_p])) {
            tname = ext_read_ident(L);
            if (mem->has_error || tname.len == 0) return;
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
    } else {
        mem_err(mem, "Expected '~' (consuming/container) or '(' (struct) in type declaration");
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

bool ext_parse_one_param(ExtLoader *L, StrSlice *out_name,
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

bool ext_parse_return_spec(ExtLoader *L, uint16_t *out_rtype, uint8_t *out_runit) {
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

void ext_compile_body(ExtLoader *L, OpOverload *ov, StrSlice *pnames) {
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
                for (uint16_t i = tab->sym_first_ov[target_sym]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
                    const OpOverload *target = &sym_overloads(tab)[i];
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

void ext_parse_sym_decl_ex(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags,
                                  bool allow_redef, uint16_t assoc_type_id);

void ext_parse_sym_decl(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags, bool allow_redef) {
    ext_parse_sym_decl_ex(L, sym_id, decl_flags, allow_redef, 0);
}

void ext_parse_sym_decl_ex(ExtLoader *L, uint16_t sym_id, uint8_t decl_flags,
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
        if (tab->overload_count >= SYM_MAX_OVERLOADS ||
            !sym_ensure_ov_cap(tab, mem, (uint16_t)(tab->overload_count + 1u))) {
            return;
        }
        uint16_t ov_idx = tab->overload_count++;
        OpOverload *ov  = &sym_overloads_mut(tab)[ov_idx];
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
    } else {
        if (tab->overload_count >= SYM_MAX_OVERLOADS ||
            !sym_ensure_ov_cap(tab, mem, (uint16_t)(tab->overload_count + 1u))) {
            return;
        }
        ov_idx = tab->overload_count++;
    }
    OpOverload *ov  = &sym_overloads_mut(tab)[ov_idx];
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

bool ext_peek_is_mod_header(const char *src, uint16_t pos, uint16_t len,
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

uint16_t ext_peek_modifiers(const ExtLoader *L, uint16_t start_pos,
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

bool ext_peek_is_quoted_decl(const ExtLoader *L, uint16_t pos) {
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

bool ext_peek_is_op_decl(const ExtLoader *L, uint16_t pos) {
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
        if (sym_overloads(tab)[i].meta_tag == INTR_STORE &&
            sym_mod_enabled(tab, sym_overloads(tab)[i].mod_id)) {
            return true;
        }
    }
    return false;
}

uint16_t ext_skip_opt_fn_kw(const ExtLoader *L, uint16_t pos) {
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

bool ext_peek_is_fn_decl(const ExtLoader *L, uint16_t pos) {
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

bool ext_peek_is_var_decl(const ExtLoader *L, uint16_t pos) {
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
        if (pos == L->pos && L->tab && L->mem) {
            int sid = sym_find(L->tab, L->mem, L->src + id_start, id_len);
            if (sid >= 0 && sym_find_own_var(L->tab, (uint16_t)sid, L->cur_mod) >= 0) {
                return false; /* Reassignment of an already-declared module variable */
            }
        }
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

bool ext_peek_is_import_decl(const ExtLoader *L, uint16_t pos) {
    if (pos + 6 > L->len) return false;
    if (memcmp(L->src + pos, "import", 6) != 0) return false;
    if (pos + 6 < L->len && lex_is_ident_cont((unsigned char)L->src[pos + 6])) return false;
    uint16_t p = ext_skip_ws_at(L->src, (uint16_t)(pos + 6), L->len);
    if (p >= L->len) return false;
    return (L->src[p] == '"' || L->src[p] == '(' || lex_is_ident_start((unsigned char)L->src[p]));
}

