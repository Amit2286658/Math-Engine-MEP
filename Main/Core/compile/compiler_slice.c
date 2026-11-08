/*
 * compiler_slice.c — Module Slice Evaluator, Statement Chunking & Dynamic Module Import
 */
#include "compiler_internal.h"

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

    char cands[6][384];
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
    bool ok = ext_eval_slice_ex(mem, tab, stem, true, true, fbuf, (uint16_t)nread, &imp_val);
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
            (dlen == 8 && memcmp(L->src + p + 1, "unit_dim", 8) == 0) ||
            (dlen == 5 && memcmp(L->src + p + 1, "entry", 5) == 0)) {
            return true;
        }
    }

    uint8_t dummy_flags = 0;
    bool    dummy_mods  = false;
    uint16_t am = ext_peek_modifiers(L, p, &dummy_flags, &dummy_mods, false);
    if (am >= L->len) return false;

    if (dummy_mods && ext_peek_is_var_decl(L, am)) return true;
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

uint16_t ext_find_stmt_chunk_end(const ExtLoader *L) {
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

/* Check whether a top-level statement chunk [start..end) consists purely of
 * module variable/constant declarations (`pub`/`priv`/`const`/`mut` or `id := ...` / `id = ...`). */
bool ext_chunk_is_var_decl_only(const ExtLoader *L, uint16_t start, uint16_t end) {
    ExtLoader sub = *L;
    sub.pos = start;
    sub.len = end;
    ext_skip_ws(&sub);
    if (sub.pos >= sub.len) return false;
    uint8_t dflags = 0;
    bool    dmods  = false;
    uint16_t am = ext_peek_modifiers(&sub, sub.pos, &dflags, &dmods, false);
    if (am >= sub.len) return false;
    if (!ext_peek_is_var_decl(&sub, am)) return false;
    /* Ensure there is no semicolon separating a second non-declaration statement on the same chunk */
    int depth = 0;
    for (uint16_t i = am; i < end; ++i) {
        char c = L->src[i];
        if (c == '"') {
            i++;
            while (i < end && L->src[i] != '"') {
                if (L->src[i] == '\\' && i + 1 < end) i++;
                i++;
            }
            continue;
        }
        if (c == '#') {
            while (i < end && L->src[i] != '\n') i++;
            continue;
        }
        if (c == '(' || c == '{' || c == '[') depth++;
        else if (c == ')' || c == '}' || c == ']') { if (depth > 0) depth--; }
        else if (depth == 0 && (c == ';' || c == '\n')) {
            uint16_t next_p = ext_skip_ws_at(L->src, (uint16_t)(i + 1), end);
            if (next_p < end) {
                uint8_t nf = 0;
                bool    nm = false;
                uint16_t nam = ext_peek_modifiers(&sub, next_p, &nf, &nm, false);
                if (nam >= end || !ext_peek_is_var_decl(&sub, nam)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/* Find a zero-required-argument function overload named `fn_name` defined inside `mod_id` */
static int ext_find_mod_entry_overload(const SymTable *tab, const MemArena *mem,
                                       uint16_t mod_id, const char *fn_name, uint16_t fn_len) {
    int sid = sym_find(tab, mem, fn_name, fn_len);
    if (sid < 0 || (uint16_t)sid >= tab->sym_count) return -1;
    for (uint16_t i = tab->sym_first_ov[sid]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        const OpOverload *ov = &sym_overloads(tab)[i];
        if (ov->mod_id == mod_id && ov->fixity == FIX_FUNC && ov->param_count == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool ext_eval_slice_ex(MemArena *mem, SymTable *tab,
                       const char *default_mod_name, bool allow_redef,
                       bool is_dep_load,
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

    Value    last_val            = val_void();
    bool     had_any_token       = false;
    bool     has_explicit_entry  = false;
    StrSlice explicit_entry_sym  = { 0, 0 };

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
            if (dlen == 5 && memcmp(L.src + L.pos + 1, "entry", 5) == 0) {
                L.pos = q;
                bool has_p = ext_match_char(&L, '(');
                StrSlice ename = ext_read_ident(&L);
                if (ename.len == 0) {
                    mem_err(mem, "Expected function name in '@entry'");
                    break;
                }
                if (has_p && !ext_match_char(&L, ')')) {
                    mem_err(mem, "Expected ')' at end of '@entry(...)'");
                    break;
                }
                explicit_entry_sym = ename;
                has_explicit_entry = true;
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
                    if (tab->overload_count >= SYM_MAX_OVERLOADS ||
                        !sym_ensure_ov_cap(tab, mem, (uint16_t)(tab->overload_count + 1u))) {
                        break;
                    }
                    ov_idx = tab->overload_count++;
                }
                OpOverload *ov     = &sym_overloads_mut(tab)[ov_idx];
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
        bool is_var_decl_chunk = ext_chunk_is_var_decl_only(&L, L.pos, chunk_end);
        if (is_dep_load && !is_var_decl_chunk) {
            /* In declaration-only dependency mode (`<[dep]` / `import`), skip standalone
             * top-level scratch expressions & demo prints so importing a module never
             * triggers its standalone side effects. */
            L.pos = chunk_end;
            continue;
        }
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

    /* 6. Hybrid Start Point Execution (`@entry(fn)` or `pub main()` / `main()`):
     *    When evaluating a target module (`!is_dep_load`), if the module declared
     *    `@entry(fn)` OR defines a 0-arg `main()` function without trailing standalone
     *    expressions, automatically invoke the entry point via the bytecode VM / evaluator. */
    if (!mem->has_error && !is_dep_load) {
        const char *entry_name = NULL;
        uint16_t    entry_len  = 0;
        char        entry_buf[64];
        if (has_explicit_entry && explicit_entry_sym.len > 0) {
            uint16_t clen = explicit_entry_sym.len < 60u ? explicit_entry_sym.len : 60u;
            memcpy(entry_buf, mem_str_ptr(mem, explicit_entry_sym), clen);
            entry_buf[clen] = '\0';
            entry_name = entry_buf;
            entry_len  = clen;
        } else if (!allow_redef &&
                   ext_find_mod_entry_overload(tab, mem, L.cur_mod, "main", 4) >= 0) {
            memcpy(entry_buf, "main", 5);
            entry_name = entry_buf;
            entry_len  = 4;
        }

        if (entry_name != NULL) {
            int entry_ov = ext_find_mod_entry_overload(tab, mem, L.cur_mod, entry_name, entry_len);
            if (entry_ov < 0) {
                mem_err(mem, "Entry point '%s()' not found (or requires arguments) in module '[%.*s]'",
                        entry_name,
                        (int)sym_mod_name(tab, L.cur_mod).len,
                        mem_str_ptr(mem, sym_mod_name(tab, L.cur_mod)));
            } else {
                tab->entry_mod_id = L.cur_mod;
                tab->entry_ov_idx = (uint16_t)entry_ov;
                mem->eval_steps   = 0;
                mem->eval_depth   = 0;
                mem->call_depth   = 0;
                mem->jump_active  = JUMP_NONE;
                Value eval_res = eval_dispatch_overload(mem, tab,
                                                        sym_overloads(tab)[entry_ov].sym_id,
                                                        FIX_FUNC, 0, NULL);
                if (mem->jump_active == JUMP_RETURN) {
                    eval_res         = mem->jump_val;
                    mem->jump_val    = val_void();
                    mem->jump_active = JUMP_NONE;
                }
                if (!mem->has_error) {
                    last_val = eval_res;
                }
            }
        }
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

bool ext_eval_slice(MemArena *mem, SymTable *tab,
                    const char *default_mod_name, bool allow_redef,
                    const char *slice_src, uint16_t slice_len,
                    Value *out_val) {
    return ext_eval_slice_ex(mem, tab, default_mod_name, allow_redef, false,
                             slice_src, slice_len, out_val);
}

bool ext_compile_slice_ex(MemArena *mem, SymTable *tab,
                          bool is_dep_load,
                          const char *slice_src, uint16_t slice_len) {
    return ext_eval_slice_ex(mem, tab, "default", false, is_dep_load,
                             slice_src, slice_len, NULL);
}

bool ext_compile_slice(MemArena *mem, SymTable *tab,
                       const char *slice_src, uint16_t slice_len) {
    return ext_eval_slice_ex(mem, tab, "default", false, false,
                             slice_src, slice_len, NULL);
}

/* ============================================================================
 * MODULE UNIT STAGING (`ext_build_init`, `ext_build_create`, `ext_compile_unit`)
 * ============================================================================ */
