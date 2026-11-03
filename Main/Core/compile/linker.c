/*
 * linker.c — Topological `<[dep]` Dependency Linker Implementation
 *
 * Implements Phase 2 (Linking) of extension building:
 *   - Verifies that every `<[dep]` dependency is either already loaded in
 *     `SymTable` or provided by a staged unit in `ExtBuildCtx`.
 *   - Runs Kahn's topological sort algorithm to order modules and detect
 *     circular `<[dep]` dependencies (supporting unbounded module & dependency counts).
 *   - Invokes `ext_compile_slice` in resolved topological order with full
 *     transactional rollback on error.
 *   - Zero global or static mutable state across engines or threads.
 */

#include "linker.h"
#include <stdlib.h>
#include <string.h>

bool ext_link_units(MemArena *mem, SymTable *tab, const ExtBuildCtx *ctx) {
    if (!mem || !tab || !ctx) return false;
    mem_clear_err(mem);
    if (ctx->has_error) {
        mem_err(mem, "%s", ctx->err_msg[0] ? ctx->err_msg : "Compilation failed prior to linking");
        return false;
    }
    if (ctx->unit_count == 0) {
        mem_snapshot_perm(mem);
        return true;
    }

    uint16_t n = ctx->unit_count;
    const ExtCompiledUnit *units = ext_ctx_units(ctx);

    uint8_t  *adj       = (uint8_t *)calloc((size_t)n * (size_t)n, sizeof(uint8_t));
    uint16_t *in_deg    = (uint16_t *)calloc(n, sizeof(uint16_t));
    bool     *scheduled = (bool *)calloc(n, sizeof(bool));
    uint16_t *order     = (uint16_t *)calloc(n, sizeof(uint16_t));
    if (!adj || !in_deg || !scheduled || !order) {
        free(adj);
        free(in_deg);
        free(scheduled);
        free(order);
        mem_err(mem, "Out of memory allocating module linker graph");
        return false;
    }

    #define FREE_GRAPH() do { \
        free(adj); \
        free(in_deg); \
        free(scheduled); \
        free(order); \
    } while (0)

    #define ADJ(r, c) adj[(size_t)(r) * (size_t)n + (size_t)(c)]

    const StrSlice *mod_names = sym_mod_names(tab);

    /* 1. Verify all `<[dep]` dependencies and construct the dependency DAG */
    for (uint16_t u = 0; u < n; ++u) {
        const ExtCompiledUnit *unit = &units[u];

        /* Ensure named module has not been loaded into `tab` since compilation */
        if (!unit->is_default_mod) {
            for (uint16_t m = 0; m < tab->mod_count; ++m) {
                if (mem_slice_eq_cstr(mem, mod_names[m], unit->mod_name)) {
                    mem_err(mem, "Module '[%s]' is already loaded", unit->mod_name);
                    FREE_GRAPH();
                    return false;
                }
            }
        }

        const char (*deps)[32] = ext_unit_deps(unit);
        for (uint16_t d = 0; d < unit->dep_count; ++d) {
            const char *dep_name = deps[d];

            if (!unit->is_default_mod && strcmp(dep_name, unit->mod_name) == 0) {
                mem_err(mem, "Circular module dependency involving '[%s]'", unit->mod_name);
                FREE_GRAPH();
                return false;
            }

            /* Check if dependency is already present in `tab` */
            bool in_tab = false;
            for (uint16_t m = 0; m < tab->mod_count; ++m) {
                if (mem_slice_eq_cstr(mem, mod_names[m], dep_name)) {
                    in_tab = true;
                    break;
                }
            }

            /* Check if dependency is provided by another unit in `ctx` */
            int provider = -1;
            for (uint16_t v = 0; v < n; ++v) {
                if (!units[v].is_default_mod &&
                    strcmp(units[v].mod_name, dep_name) == 0) {
                    provider = (int)v;
                    break;
                }
            }

            if (!in_tab && provider < 0) {
                mem_err(mem, "Unresolved module dependency '<[%s]' required by '[%s]'",
                        dep_name, unit->mod_name);
                FREE_GRAPH();
                return false;
            }

            if (provider >= 0 && !ADJ(provider, u)) {
                ADJ(provider, u) = 1u;
                in_deg[u]++;
            }
        }
    }

    /* Schedule implicit `[default]` units after named units in the same link batch */
    for (uint16_t u = 0; u < n; ++u) {
        if (!units[u].is_default_mod) continue;
        for (uint16_t v = 0; v < n; ++v) {
            if (units[v].is_default_mod) continue;
            if (!ADJ(v, u) && !ADJ(u, v)) {
                ADJ(v, u) = 1u;
                in_deg[u]++;
            }
        }
    }

    /* 2. Kahn's Topological Sort with Cycle Detection */
    uint16_t order_cnt = 0;

    while (order_cnt < n) {
        int pick = -1;
        for (uint16_t u = 0; u < n; ++u) {
            if (!scheduled[u] && in_deg[u] == 0) {
                pick = (int)u;
                break;
            }
        }
        if (pick < 0) {
            for (uint16_t u = 0; u < n; ++u) {
                if (!scheduled[u]) {
                    mem_err(mem, "Circular module dependency involving '[%s]'",
                            units[u].mod_name);
                    break;
                }
            }
            FREE_GRAPH();
            return false;
        }
        scheduled[pick]    = true;
        order[order_cnt++] = (uint16_t)pick;
        for (uint16_t w = 0; w < n; ++w) {
            if (ADJ(pick, w) && in_deg[w] > 0) {
                in_deg[w]--;
            }
        }
    }

    #undef ADJ

    /* 3. Save watermarks for transactional rollback across the entire link pass */
    uint16_t saved_src_len        = mem->src_len;
    uint16_t saved_ast_count      = mem->ast_count;
    uint16_t saved_pool_top       = mem->pool_top;
    uint16_t saved_perm_src_len   = mem->perm_src_len;
    uint16_t saved_perm_ast_count = mem->perm_ast_count;
    uint16_t saved_perm_pool_top  = mem->perm_pool_top;
    uint8_t  saved_fn_src_count   = mem->fn_src_count;
    uint8_t  saved_perm_fn_src    = mem->perm_fn_src_count;
    uint8_t  saved_fast_real      = mem->fast_real_type;
    uint8_t  saved_str_type       = mem->str_type_id;
    uint8_t  saved_vec_type       = mem->vec_type_id;
    uint8_t  saved_mat_type       = mem->mat_type_id;
    uint8_t  saved_fn_type        = mem->fn_type_id;
    uint8_t  saved_map_type       = mem->map_type_id;

    uint16_t saved_mod_count      = tab->mod_count;
    uint8_t *saved_mod_active     = NULL;
    if (saved_mod_count > 0) {
        saved_mod_active = (uint8_t *)malloc((size_t)saved_mod_count * sizeof(uint8_t));
        if (!saved_mod_active) {
            FREE_GRAPH();
            mem_err(mem, "Out of memory saving module state");
            return false;
        }
        memcpy(saved_mod_active, sym_mod_active(tab), (size_t)saved_mod_count * sizeof(uint8_t));
    }
    uint8_t  saved_type_count     = tab->type_count;
    uint8_t  saved_unit_count     = tab->unit_count;
    UnitDimDef saved_unit_dims[SYM_MAX_UNITS];
    memcpy(saved_unit_dims, tab->unit_dims, sizeof(saved_unit_dims));
    uint16_t saved_pat_elem_count = tab->pat_elem_count;
    uint16_t saved_field_count    = tab->field_count;
    uint16_t saved_sym_count      = tab->sym_count;
    uint16_t saved_overload_count = tab->overload_count;
    uint8_t  saved_const_count    = tab->const_count;
    uint16_t saved_var_count      = tab->var_count;

    /* 4. Compile each module slice in resolved topological order */
    for (uint16_t k = 0; k < order_cnt; ++k) {
        const ExtCompiledUnit *u = &units[order[k]];
        if (!ext_compile_slice(mem, tab, ctx->src_pool + u->src_off, u->src_len)) {
            break;
        }
    }

    FREE_GRAPH();
    #undef FREE_GRAPH

    if (mem->has_error) {
        mem->src_len           = saved_src_len;
        mem->ast_count         = saved_ast_count;
        mem->pool_top          = saved_pool_top;
        mem->perm_src_len      = saved_perm_src_len;
        mem->perm_ast_count    = saved_perm_ast_count;
        mem->perm_pool_top     = saved_perm_pool_top;
        mem->fn_src_count      = saved_fn_src_count;
        mem->perm_fn_src_count = saved_perm_fn_src;
        mem->fast_real_type = saved_fast_real;
        mem->str_type_id    = saved_str_type;
        mem->vec_type_id    = saved_vec_type;
        mem->mat_type_id    = saved_mat_type;
        mem->fn_type_id     = saved_fn_type;
        mem->map_type_id    = saved_map_type;

        tab->mod_count      = saved_mod_count;
        if (saved_mod_active && saved_mod_count > 0) {
            memcpy(sym_mod_active_mut(tab), saved_mod_active, (size_t)saved_mod_count * sizeof(uint8_t));
        }
        free(saved_mod_active);
        tab->type_count     = saved_type_count;
        tab->unit_count     = saved_unit_count;
        memcpy(tab->unit_dims, saved_unit_dims, sizeof(saved_unit_dims));
        tab->pat_elem_count = saved_pat_elem_count;
        tab->field_count    = saved_field_count;
        tab->sym_count      = saved_sym_count;
        tab->overload_count = saved_overload_count;
        tab->const_count    = saved_const_count;
        tab->var_count      = saved_var_count;
        sym_rebuild_index(tab);
        return false;
    }

    free(saved_mod_active);
    mem_snapshot_perm(mem);
    return true;
}

bool ext_compile_and_link_ctx(MemArena *mem, SymTable *tab, ExtBuildCtx *ctx,
                              const char *const *raw_texts, size_t unit_count) {
    if (!mem || !tab || !ctx) return false;
    mem_clear_err(mem);
    if (!raw_texts && unit_count > 0) {
        mem_err(mem, "Null extension source array");
        return false;
    }
    ext_build_init(ctx);
    for (size_t i = 0; i < unit_count; ++i) {
        if (!ext_compile_unit(mem, tab, ctx, raw_texts[i])) {
            return false;
        }
    }
    return ext_link_units(mem, tab, ctx);
}

bool ext_compile_and_link(MemArena *mem, SymTable *tab,
                          const char *const *raw_texts, size_t unit_count) {
    if (!mem || !tab) return false;
    mem_clear_err(mem);
    if (!raw_texts && unit_count > 0) {
        mem_err(mem, "Null extension source array");
        return false;
    }
    ExtBuildCtx *ctx = ext_build_create();
    if (!ctx) {
        mem_err(mem, "Out of memory allocating temporary ExtBuildCtx");
        return false;
    }
    bool ok = ext_compile_and_link_ctx(mem, tab, ctx, raw_texts, unit_count);
    ext_build_destroy(ctx);
    return ok;
}

bool ext_load_text(MemArena *mem, SymTable *tab, const char *ext_src) {
    if (!mem || !tab) return false;
    if (!ext_src) {
        mem_clear_err(mem);
        mem_err(mem, "Null extension source string");
        return false;
    }
    return ext_compile_and_link(mem, tab, &ext_src, 1);
}
