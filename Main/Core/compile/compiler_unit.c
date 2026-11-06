/*
 * compiler_unit.c — Multi-Module Compilation Unit Stager & Construct Detector
 */
#include "compiler_internal.h"


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
    ctx->unit_count  = 0;
    ctx->pool_used   = 0;
    ctx->allow_redef = false;
    ctx->has_error   = false;
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

                    if (!is_def && !ctx->allow_redef) {
                        /* Reject if module is already loaded in SymTable (unless in patch mode) */
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

    /* Analyze newly staged unit(s) for `@entry`, `main()`, `has_top_expr`, and `import` dependencies */
    uint16_t end_unit = ctx->unit_count;
    for (uint16_t ui = 0; ui < end_unit; ++ui) {
        ExtCompiledUnit *u = &ext_ctx_units_mut(ctx)[ui];
        if (u->src_off < base_off) continue;

        ExtLoader L;
        memset(&L, 0, sizeof(L));
        L.mem = mem;
        L.tab = (SymTable *)(uintptr_t)tab;
        L.src = ctx->src_pool + u->src_off;
        L.len = u->src_len;
        L.pos = 0;

        char   imp_specs[16][120];
        bool   imp_quoted[16];
        char   imp_stems[16][32];
        int    imp_cnt = 0;

        while (L.pos < L.len) {
            ext_skip_ws(&L);
            while (L.pos < L.len && L.src[L.pos] == ';') {
                L.pos++;
                ext_skip_ws(&L);
            }
            if (L.pos >= L.len) break;

            if (L.src[L.pos] == '[') {
                uint16_t after_r = 0;
                bool mal = false;
                if (ext_peek_is_mod_header(L.src, L.pos, L.len, NULL, NULL, &after_r, &mal)) {
                    L.pos = after_r;
                    continue;
                }
            }
            if (L.src[L.pos] == '<' && L.pos + 1 < L.len && L.src[L.pos + 1] == '[') {
                L.pos = (uint16_t)(L.pos + 2);
                (void)ext_read_ident(&L);
                (void)ext_match_char(&L, ']');
                continue;
            }
            if (ext_peek_is_import_decl(&L, L.pos)) {
                L.pos = (uint16_t)(L.pos + 6);
                ext_skip_ws(&L);
                bool has_p = ext_match_char(&L, '(');
                ext_skip_ws(&L);
                uint16_t sp_off = 0, sp_len = 0;
                bool is_q = false;
                if (L.pos < L.len && L.src[L.pos] == '"') {
                    is_q = true;
                    L.pos++;
                    sp_off = L.pos;
                    while (L.pos < L.len && L.src[L.pos] != '"') {
                        if (L.src[L.pos] == '\\' && L.pos + 1 < L.len) L.pos = (uint16_t)(L.pos + 2);
                        else L.pos++;
                    }
                    sp_len = (uint16_t)(L.pos - sp_off);
                    if (L.pos < L.len && L.src[L.pos] == '"') L.pos++;
                } else {
                    sp_off = L.pos;
                    while (L.pos < L.len && lex_is_ident_cont((unsigned char)L.src[L.pos])) L.pos++;
                    sp_len = (uint16_t)(L.pos - sp_off);
                }
                if (has_p) {
                    ext_skip_ws(&L);
                    (void)ext_match_char(&L, ')');
                }
                if (sp_len > 0 && sp_len < 118u) {
                    char raw_sp[120];
                    memcpy(raw_sp, L.src + sp_off, sp_len);
                    raw_sp[sp_len] = '\0';
                    const char *slash = strrchr(raw_sp, '/');
                    const char *base  = slash ? (slash + 1) : raw_sp;
                    char stem[32];
                    size_t bl = 0;
                    while (base[bl] != '\0' && base[bl] != '.' && bl + 1u < sizeof(stem)) {
                        stem[bl] = base[bl];
                        bl++;
                    }
                    stem[bl] = '\0';
                    if (stem[0] != '\0') {
                        if (!is_q) {
                            u = &ext_ctx_units_mut(ctx)[ui];
                            if (!ext_add_unit_dep(mem, ctx, u, stem, (uint16_t)strlen(stem))) {
                                return false;
                            }
                        }
                        if (imp_cnt < 16) {
                            snprintf(imp_specs[imp_cnt], sizeof(imp_specs[0]), "%s", raw_sp);
                            imp_quoted[imp_cnt] = is_q;
                            snprintf(imp_stems[imp_cnt], sizeof(imp_stems[0]), "%s", stem);
                            imp_cnt++;
                        }
                    }
                }
                continue;
            }
            if (L.src[L.pos] == '@') {
                uint16_t q = (uint16_t)(L.pos + 1);
                while (q < L.len && lex_is_ident_cont((unsigned char)L.src[q])) q++;
                uint16_t dlen = (uint16_t)(q - (L.pos + 1));
                if ((dlen == 3 && memcmp(L.src + L.pos + 1, "dim", 3) == 0) ||
                    (dlen == 8 && memcmp(L.src + L.pos + 1, "unit_dim", 8) == 0)) {
                    L.pos = q;
                    while (L.pos < L.len && L.src[L.pos] != ')' && L.src[L.pos] != '\n') L.pos++;
                    if (L.pos < L.len && L.src[L.pos] == ')') L.pos++;
                    continue;
                }
                if (dlen == 5 && memcmp(L.src + L.pos + 1, "entry", 5) == 0) {
                    L.pos = q;
                    bool has_p = ext_match_char(&L, '(');
                    ext_skip_ws(&L);
                    uint16_t en_off = L.pos;
                    while (L.pos < L.len && lex_is_ident_cont((unsigned char)L.src[L.pos])) L.pos++;
                    uint16_t en_len = (uint16_t)(L.pos - en_off);
                    if (has_p) (void)ext_match_char(&L, ')');
                    u = &ext_ctx_units_mut(ctx)[ui];
                    if (u->explicit_entry[0] != '\0') {
                        return build_fail(mem, ctx, "Multiple '@entry' declarations in module");
                    }
                    if (en_len == 0 || en_len >= sizeof(u->explicit_entry)) {
                        return build_fail(mem, ctx, "Invalid function name in '@entry'");
                    }
                    memcpy(u->explicit_entry, L.src + en_off, en_len);
                    u->explicit_entry[en_len] = '\0';
                    continue;
                }
            }

            uint8_t dflags = 0;
            bool has_m = false;
            uint16_t am = ext_peek_modifiers(&L, L.pos, &dflags, &has_m, false);
            if (am < L.len) {
                uint16_t fn_pos = ext_skip_opt_fn_kw(&L, am);
                if (fn_pos != am && ext_peek_is_fn_decl(&L, fn_pos)) {
                    am = fn_pos;
                }
                char mc = L.src[am];
                if (mc == '$' || ext_peek_is_kw_type_decl(&L, am)) {
                    L.pos = ext_find_stmt_chunk_end(&L);
                    continue;
                }
                if (mc == '"' && ext_peek_is_quoted_decl(&L, am)) {
                    L.pos = ext_find_stmt_chunk_end(&L);
                    continue;
                }
                if (mc == '(' && ext_peek_is_op_decl(&L, am)) {
                    L.pos = ext_find_stmt_chunk_end(&L);
                    continue;
                }
                if (lex_is_ident_start((unsigned char)mc) && ext_peek_is_fn_decl(&L, am)) {
                    L.pos = am;
                    uint16_t fn_off = L.pos;
                    while (L.pos < L.len && lex_is_ident_cont((unsigned char)L.src[L.pos])) L.pos++;
                    uint16_t fn_len = (uint16_t)(L.pos - fn_off);
                    ext_skip_ws(&L);
                    if (fn_len == 4 && memcmp(L.src + fn_off, "main", 4) == 0 &&
                        L.pos < L.len && L.src[L.pos] == '(') {
                        uint16_t p = ext_skip_ws_at(L.src, (uint16_t)(L.pos + 1), L.len);
                        if (p < L.len && L.src[p] == ')') {
                            ext_ctx_units_mut(ctx)[ui].has_main_fn = true;
                        }
                    }
                    L.pos = ext_find_stmt_chunk_end(&L);
                    continue;
                }
            }

            uint16_t chunk_end = ext_find_stmt_chunk_end(&L);
            if (!ext_chunk_is_var_decl_only(&L, L.pos, chunk_end)) {
                ext_ctx_units_mut(ctx)[ui].has_top_expr = true;
            }
            L.pos = chunk_end;
        }

        /* Auto-stage any external files referenced via `import` that are not yet in `tab` or `ctx` */
        for (int im = 0; im < imp_cnt; ++im) {
            const char *stem = imp_stems[im];
            bool already_known = false;
            const StrSlice *mnames = sym_mod_names(tab);
            for (uint16_t m = 0; m < tab->mod_count; ++m) {
                if (mem_slice_eq_cstr(mem, mnames[m], stem)) {
                    already_known = true;
                    break;
                }
            }
            const ExtCompiledUnit *staged = ext_ctx_units(ctx);
            for (uint16_t su = 0; su < ctx->unit_count && !already_known; ++su) {
                if (!staged[su].is_default_mod && strcmp(staged[su].mod_name, stem) == 0) {
                    already_known = true;
                    break;
                }
            }
            if (already_known) continue;

            const char *raw_path = imp_specs[im];
            size_t rlen = strlen(raw_path);
            bool has_ext = (rlen >= 5 && memcmp(raw_path + rlen - 5, ".math", 5) == 0);
            char with_ext[140];
            if (has_ext) snprintf(with_ext, sizeof(with_ext), "%s", raw_path);
            else         snprintf(with_ext, sizeof(with_ext), "%s.math", raw_path);

            char cands[6][384];
            int  cand_cnt = 0;
            snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s", with_ext);
            if (!has_ext) snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s", raw_path);
            if (mem->import_dir[0] != '\0') {
                snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s/%s", mem->import_dir, with_ext);
                if (!has_ext) snprintf(cands[cand_cnt++], sizeof(cands[0]), "%s/%s", mem->import_dir, raw_path);
            }
            snprintf(cands[cand_cnt++], sizeof(cands[0]), "Library/%s", with_ext);
            snprintf(cands[cand_cnt++], sizeof(cands[0]), "patches/%s", with_ext);

            FILE *fp = NULL;
            for (int ci = 0; ci < cand_cnt; ++ci) {
                fp = fopen(cands[ci], "rb");
                if (fp != NULL) break;
            }
            if (!fp) {
                if (!imp_quoted[im]) {
                    /* Unquoted `import no_such_mod`: let linker report unresolved module dependency */
                    continue;
                }
                mem_err(mem, "Cannot find imported module or file '%s'", raw_path);
                ctx->has_error = true;
                snprintf(ctx->err_msg, sizeof(ctx->err_msg), "%s", mem->err_msg);
                return false;
            }
            if (fseek(fp, 0, SEEK_END) != 0) {
                fclose(fp);
                return build_fail(mem, ctx, "Failed to seek imported file");
            }
            long fsz = ftell(fp);
            if (fsz < 0 || fsz > 65000L) {
                fclose(fp);
                return build_fail(mem, ctx, "Imported file size out of range");
            }
            rewind(fp);
            char *fbuf = (char *)malloc((size_t)fsz + 64u);
            if (!fbuf) {
                fclose(fp);
                return build_fail(mem, ctx, "Out of memory reading imported file");
            }
            size_t nread = fread(fbuf, 1, (size_t)fsz, fp);
            fclose(fp);
            fbuf[nread] = '\0';

            /* Check if imported file starts with an explicit `[module]` header; if not, wrap in `[stem]` */
            uint16_t p0 = 0;
            while (p0 < (uint16_t)nread) {
                while (p0 < (uint16_t)nread && (fbuf[p0] == ' ' || fbuf[p0] == '\t' || fbuf[p0] == '\r' || fbuf[p0] == '\n')) p0++;
                if (p0 < (uint16_t)nread && fbuf[p0] == '#') {
                    while (p0 < (uint16_t)nread && fbuf[p0] != '\n') p0++;
                    continue;
                }
                break;
            }
            bool has_hdr = false;
            uint16_t hid_off = 0, hid_len = 0;
            char actual_mod[32];
            snprintf(actual_mod, sizeof(actual_mod), "%s", stem);
            if (p0 < (uint16_t)nread && fbuf[p0] == '[') {
                bool mal = false;
                has_hdr = ext_peek_is_mod_header(fbuf, p0, (uint16_t)nread, &hid_off, &hid_len, NULL, &mal);
                if (has_hdr && hid_len > 0 && hid_len < sizeof(actual_mod)) {
                    memcpy(actual_mod, fbuf + hid_off, hid_len);
                    actual_mod[hid_len] = '\0';
                }
            }
            if (!has_hdr) {
                char hdr_prefix[40];
                int hlen = snprintf(hdr_prefix, sizeof(hdr_prefix), "[%s]\n", stem);
                if (hlen > 0) {
                    memmove(fbuf + hlen, fbuf, nread + 1u);
                    memcpy(fbuf, hdr_prefix, (size_t)hlen);
                }
            }
            u = &ext_ctx_units_mut(ctx)[ui];
            if (!ext_add_unit_dep(mem, ctx, u, actual_mod, (uint16_t)strlen(actual_mod))) {
                free(fbuf);
                return false;
            }
            bool sub_ok = ext_compile_unit(mem, tab, ctx, fbuf);
            free(fbuf);
            if (!sub_ok) return false;
        }
    }
    return true;
}

bool ext_src_has_module_constructs(const MemArena *mem, const SymTable *tab,
                                   const char *src, uint16_t len) {
    if (!mem || !tab || !src || len == 0) return false;
    ExtLoader L;
    memset(&L, 0, sizeof(L));
    L.mem = (MemArena *)(uintptr_t)mem;
    L.tab = (SymTable *)(uintptr_t)tab;
    L.src = src;
    L.len = len;
    L.pos = 0;

    uint16_t p = 0;
    bool at_line_start = true;
    int depth = 0;
    while (p < len) {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
        if (c == '\n') {
            if (depth == 0) at_line_start = true;
            p++;
            continue;
        }
        if (c == '#') {
            while (p < len && src[p] != '\n') p++;
            continue;
        }
        if (c == '"') {
            p++;
            while (p < len && src[p] != '"') {
                if (src[p] == '\\' && p + 1 < len) p = (uint16_t)(p + 2);
                else p++;
            }
            if (p < len) p++;
            at_line_start = false;
            continue;
        }
        if (at_line_start && depth == 0) {
            bool mal = false;
            if (c == '[' && (ext_peek_is_mod_header(src, p, len, NULL, NULL, NULL, &mal) || mal)) return true;
            if (c == '<' && p + 1 < len && src[p + 1] == '[') return true;
            if (c == '@') {
                uint16_t q = (uint16_t)(p + 1);
                while (q < len && lex_is_ident_cont((unsigned char)src[q])) q++;
                uint16_t dlen = (uint16_t)(q - (p + 1));
                if ((dlen == 3 && memcmp(src + p + 1, "dim", 3) == 0) ||
                    (dlen == 8 && memcmp(src + p + 1, "unit_dim", 8) == 0) ||
                    (dlen == 5 && memcmp(src + p + 1, "entry", 5) == 0)) {
                    return true;
                }
            }
            if (c == '$') return true;
            if (ext_peek_is_kw_type_decl(&L, p)) return true;
            if (ext_peek_is_import_decl(&L, p)) return true;
            uint8_t df = 0;
            bool hm = false;
            uint16_t am = ext_peek_modifiers(&L, p, &df, &hm, false);
            if (hm) return true;
            if (am < len && (src[am] == '$' || ext_peek_is_kw_type_decl(&L, am))) return true;
        }
        if (c == '(' || c == '{' || c == '[') { depth++; at_line_start = false; p++; }
        else if (c == ')' || c == '}' || c == ']') { if (depth > 0) depth--; at_line_start = false; p++; }
        else if (c == ';' && depth == 0) { at_line_start = true; p++; }
        else { at_line_start = false; p++; }
    }
    return false;
}
