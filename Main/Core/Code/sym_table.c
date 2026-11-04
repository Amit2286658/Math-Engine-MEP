/*
 * sym_table.c — Dynamic Module, Type, Unit, Struct, Symbol, Variable, Overload & Extern Implementation
 *
 * Routes:
 *   - `@<intrinsic>` lookup -> `Core/lib/delegate/delegate.h`
 *   - Reserved keywords & wildcard type/unit rules -> `Core/lib/hardcode/hardcode.h`
 */

#include "sym_table.h"
#include "../lib/delegate/delegate.h"
#include "../lib/hardcode/hardcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sym_free_heap(SymTable *tab) {
    if (!tab) return;
    if (tab->heap_mod_magic == SYM_HEAP_MOD_MAGIC) {
        free(tab->heap_mod_names);
        free(tab->heap_mod_active);
    }
    tab->heap_mod_names  = NULL;
    tab->heap_mod_active = NULL;
    tab->heap_mod_magic  = 0;
    tab->mod_cap         = SYM_SBO_MODULES;

    if (tab->heap_var_magic == SYM_HEAP_VAR_MAGIC) {
        free(tab->heap_vars);
    }
    tab->heap_vars      = NULL;
    tab->heap_var_magic = 0;
    tab->var_cap        = SYM_SBO_VARS;
}

void sym_init(SymTable *tab) {
    if (!tab) return;
    sym_free_heap(tab);
    memset(tab, 0, sizeof(*tab));
    tab->mod_cap    = SYM_SBO_MODULES;
    tab->var_cap    = SYM_SBO_VARS;
    tab->type_count = 1; /* Index 0 reserved for MEM_TYPE_VOID */
    tab->unit_count = 1; /* Index 0 reserved for MEM_UNIT_NONE */
    memset(tab->sym_first_ov, 0xFF, sizeof(tab->sym_first_ov));
    memset(tab->sym_last_ov,  0xFF, sizeof(tab->sym_last_ov));
    memset(tab->ov_next,      0xFF, sizeof(tab->ov_next));
}

bool sym_ensure_mod_cap(SymTable *tab, MemArena *mem, uint16_t needed_cap) {
    if (!tab) return false;
    if (tab->mod_cap == 0) tab->mod_cap = SYM_SBO_MODULES;
    if (needed_cap <= tab->mod_cap) return true;

    uint32_t new_cap = (uint32_t)tab->mod_cap;
    while (new_cap < (uint32_t)needed_cap) {
        new_cap *= 2u;
        if (new_cap > 0xFFFEu) {
            new_cap = 0xFFFEu;
            break;
        }
    }
    if (new_cap < (uint32_t)needed_cap) {
        if (mem) mem_err(mem, "Module table index overflow");
        return false;
    }

    StrSlice *new_names  = (StrSlice *)calloc(new_cap, sizeof(StrSlice));
    uint8_t  *new_active = (uint8_t *)calloc(new_cap, sizeof(uint8_t));
    if (!new_names || !new_active) {
        free(new_names);
        free(new_active);
        if (mem) mem_err(mem, "Out of memory expanding module table");
        return false;
    }

    if (tab->mod_count > 0) {
        memcpy(new_names,  sym_mod_names(tab),  (size_t)tab->mod_count * sizeof(StrSlice));
        memcpy(new_active, sym_mod_active(tab), (size_t)tab->mod_count * sizeof(uint8_t));
    }
    if (tab->heap_mod_magic == SYM_HEAP_MOD_MAGIC) {
        free(tab->heap_mod_names);
        free(tab->heap_mod_active);
    }
    tab->heap_mod_names  = new_names;
    tab->heap_mod_active = new_active;
    tab->heap_mod_magic  = SYM_HEAP_MOD_MAGIC;
    tab->mod_cap         = (uint16_t)new_cap;
    return true;
}

bool sym_ensure_var_cap(SymTable *tab, MemArena *mem, uint16_t needed_cap) {
    if (!tab) return false;
    if (tab->var_cap == 0) tab->var_cap = SYM_SBO_VARS;
    if (needed_cap <= tab->var_cap) return true;

    uint32_t new_cap = (uint32_t)tab->var_cap;
    while (new_cap < (uint32_t)needed_cap) {
        new_cap *= 2u;
        if (new_cap > 0xFFFEu) {
            new_cap = 0xFFFEu;
            break;
        }
    }
    if (new_cap < (uint32_t)needed_cap) {
        if (mem) mem_err(mem, "Module variable table index overflow");
        return false;
    }

    ModVarDef *new_vars = (ModVarDef *)calloc(new_cap, sizeof(ModVarDef));
    if (!new_vars) {
        if (mem) mem_err(mem, "Out of memory expanding module variable table");
        return false;
    }

    if (tab->var_count > 0) {
        memcpy(new_vars, sym_vars(tab), (size_t)tab->var_count * sizeof(ModVarDef));
    }
    if (tab->heap_var_magic == SYM_HEAP_VAR_MAGIC) {
        free(tab->heap_vars);
    }
    tab->heap_vars      = new_vars;
    tab->heap_var_magic = SYM_HEAP_VAR_MAGIC;
    tab->var_cap        = (uint16_t)new_cap;
    return true;
}

void sym_link_overload(SymTable *tab, uint16_t ov_idx) {
    if (ov_idx >= tab->overload_count) return;
    uint16_t sid = tab->overloads[ov_idx].sym_id;
    tab->ov_next[ov_idx] = MEM_NO_IDX;
    if (sid >= SYM_MAX_SYMS) return;
    if (tab->sym_first_ov[sid] == MEM_NO_IDX) {
        tab->sym_first_ov[sid] = ov_idx;
        tab->sym_last_ov[sid]  = ov_idx;
    } else {
        uint16_t prev = tab->sym_last_ov[sid];
        if (prev < SYM_MAX_OVERLOADS) {
            tab->ov_next[prev] = ov_idx;
        }
        tab->sym_last_ov[sid] = ov_idx;
    }
}

void sym_rebuild_index(SymTable *tab) {
    memset(tab->sym_first_ov, 0xFF, sizeof(tab->sym_first_ov));
    memset(tab->sym_last_ov,  0xFF, sizeof(tab->sym_last_ov));
    memset(tab->ov_next,      0xFF, sizeof(tab->ov_next));
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        sym_link_overload(tab, i);
    }
}

uint16_t sym_find_or_add_mod(SymTable *tab, MemArena *mem, StrSlice name) {
    const StrSlice *names = sym_mod_names(tab);
    for (uint16_t i = 0; i < tab->mod_count; ++i) {
        if (mem_slice_eq(mem, names[i], name)) return i;
    }
    if (tab->mod_count >= 0xFFFEu) {
        mem_err(mem, "Module table index overflow");
        return 0;
    }
    if (!sym_ensure_mod_cap(tab, mem, (uint16_t)(tab->mod_count + 1u))) {
        return 0;
    }
    uint16_t id = tab->mod_count++;
    sym_mod_names_mut(tab)[id]  = name;
    sym_mod_active_mut(tab)[id] = 1u;
    if ((uint32_t)name.off + (uint32_t)name.len > mem->perm_src_len &&
        (uint32_t)name.off + (uint32_t)name.len <= mem->src_len) {
        mem->perm_src_len = (uint16_t)(name.off + name.len);
    }
    return id;
}

int sym_find_mod(const SymTable *tab, const MemArena *mem, const char *mod_name) {
    if (!tab || !mem || !mod_name) return -1;
    const StrSlice *names = sym_mod_names(tab);
    for (uint16_t i = 0; i < tab->mod_count; ++i) {
        if (mem_slice_eq_cstr(mem, names[i], mod_name)) {
            return (int)i;
        }
    }
    return -1;
}

int sym_find_mod_slice(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len) {
    if (!tab || !mem || !name || len == 0) return -1;
    const StrSlice *names = sym_mod_names(tab);
    for (uint16_t i = 0; i < tab->mod_count; ++i) {
        if (names[i].len == len && memcmp(mem_str_ptr(mem, names[i]), name, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool sym_set_module(SymTable *tab, const MemArena *mem, const char *mod_name, bool enabled) {
    const StrSlice *names = sym_mod_names(tab);
    uint8_t *active = sym_mod_active_mut(tab);
    for (uint16_t i = 0; i < tab->mod_count; ++i) {
        if (mem_slice_eq_cstr(mem, names[i], mod_name)) {
            active[i] = enabled ? 1u : 0u;
            return true;
        }
    }
    return false;
}

bool sym_is_module_active(const SymTable *tab, const MemArena *mem, const char *mod_name) {
    const StrSlice *names  = sym_mod_names(tab);
    const uint8_t  *active = sym_mod_active(tab);
    for (uint16_t i = 0; i < tab->mod_count; ++i) {
        if (mem_slice_eq_cstr(mem, names[i], mod_name)) {
            return active[i] != 0;
        }
    }
    return false;
}

void sym_sync_consts_to_vars(SymTable *tab, MemArena *mem) {
    if (!tab || !mem) return;
    for (uint8_t i = 0; i < tab->const_count; ++i) {
        const ConstDef *cd = &tab->consts[i];
        int existing = sym_find_own_var(tab, cd->sym_id, cd->mod_id);
        if (existing >= 0) {
            ModVarDef *mv = &sym_vars_mut(tab)[existing];
            mv->val   = tab->const_vals[cd->val_idx];
            mv->flags = cd->reserved;
            continue;
        }
        if (!sym_ensure_var_cap(tab, mem, (uint16_t)(tab->var_count + 1u))) return;
        ModVarDef *mv = &sym_vars_mut(tab)[tab->var_count++];
        mv->sym_id    = cd->sym_id;
        mv->mod_id    = cd->mod_id;
        mv->lock_type = cd->type_id;
        mv->lock_unit = cd->unit_id;
        mv->flags     = cd->reserved;
        mv->val       = tab->const_vals[cd->val_idx];
    }
}

int sym_find_own_var(const SymTable *tab, uint16_t sym_id, uint16_t mod_id) {
    if (!tab) return -1;
    const ModVarDef *vars = sym_vars(tab);
    for (uint16_t i = 0; i < tab->var_count; ++i) {
        if (vars[i].sym_id == sym_id && vars[i].mod_id == mod_id) {
            return (int)i;
        }
    }
    return -1;
}

int sym_find_var(const SymTable *tab, uint16_t sym_id, uint16_t caller_mod,
                 uint16_t *out_priv_mod) {
    if (out_priv_mod) *out_priv_mod = MEM_NO_IDX;
    if (!tab) return -1;
    const ModVarDef *vars = sym_vars(tab);

    /* 1. Prefer a binding in `caller_mod` (both public and private bindings of caller_mod are visible) */
    if (caller_mod < tab->mod_count && sym_mod_enabled(tab, caller_mod)) {
        for (int i = (int)tab->var_count - 1; i >= 0; --i) {
            if (vars[i].sym_id == sym_id && vars[i].mod_id == caller_mod) {
                return i;
            }
        }
    }

    /* 2. Search active modules for a public binding; track if a private binding was skipped */
    uint16_t seen_priv_mod = MEM_NO_IDX;
    for (int i = (int)tab->var_count - 1; i >= 0; --i) {
        if (vars[i].sym_id != sym_id) continue;
        if (!sym_mod_enabled(tab, vars[i].mod_id)) continue;
        if ((vars[i].flags & SYM_VAR_PRIVATE) != 0 && vars[i].mod_id != caller_mod) {
            if (seen_priv_mod == MEM_NO_IDX) {
                seen_priv_mod = vars[i].mod_id;
            }
            continue;
        }
        return i;
    }

    if (out_priv_mod) *out_priv_mod = seen_priv_mod;
    return -1;
}

static bool sym_check_var_locks(const SymTable *tab, MemArena *mem,
                                uint16_t sym_id, uint16_t req_type, uint8_t req_unit,
                                Value val) {
    if (val_get_type(mem, val) == MEM_TYPE_VOID) return true;
    req_type &= 0xFFu;
    if (req_type != 0 && req_type != SYM_TYPE_ANY) {
        uint16_t rhs_type = val_get_type(mem, val);
        if (rhs_type != req_type) {
            char exp_t[32], got_t[32];
            sym_type_name(tab, mem, req_type, exp_t, sizeof(exp_t));
            sym_type_name(tab, mem, rhs_type, got_t, sizeof(got_t));
            StrSlice s = (sym_id < tab->sym_count) ? tab->syms[sym_id] : (StrSlice){0, 0};
            if (s.len > 0) {
                mem_err(mem, "Type mismatch: variable '%.*s' holds concrete type '%s', cannot assign '%s'",
                        (int)s.len, mem_str_ptr(mem, s), exp_t, got_t);
            } else {
                mem_err(mem, "Type mismatch: variable holds concrete type '%s', cannot assign '%s'",
                        exp_t, got_t);
            }
            return false;
        }
    }
    if (req_unit != 0 && req_unit != SYM_UNIT_GENERIC && req_unit != SYM_UNIT_OMITTED) {
        uint8_t expected_u = (req_unit == SYM_UNIT_LOCK_NONE) ? MEM_UNIT_NONE : req_unit;
        uint8_t rhs_unit   = val_get_unit(mem, val);
        if (rhs_unit != expected_u) {
            char exp_u[32], got_u[32];
            sym_unit_name(tab, mem, expected_u, exp_u, sizeof(exp_u));
            sym_unit_name(tab, mem, rhs_unit,   got_u, sizeof(got_u));
            StrSlice s = (sym_id < tab->sym_count) ? tab->syms[sym_id] : (StrSlice){0, 0};
            if (s.len > 0) {
                mem_err(mem, "Unit mismatch: variable '%.*s' holds concrete unit '%s', cannot assign '%s'",
                        (int)s.len, mem_str_ptr(mem, s), exp_u, got_u);
            } else {
                mem_err(mem, "Unit mismatch: variable holds concrete unit '%s', cannot assign '%s'",
                        exp_u, got_u);
            }
            return false;
        }
    }
    return true;
}

static void sym_sync_var_to_const(SymTable *tab, MemArena *mem, const ModVarDef *mv) {
    for (uint8_t i = 0; i < tab->const_count; ++i) {
        if (tab->consts[i].sym_id == mv->sym_id && tab->consts[i].mod_id == mv->mod_id) {
            uint16_t tid = val_get_type(mem, mv->val);
            if (tid != 0 && tid < tab->type_count && tab->types[tid].is_consuming &&
                tid != mem->str_type_id) {
                uint8_t uid = val_get_unit(mem, mv->val);
                double  d   = val_get_f64(mem, mv->val);
                mem_write_pool(mem, tab->consts[i].pool_idx, d);
                tab->consts[i].type_id  = (uint8_t)tid;
                tab->consts[i].unit_id  = uid;
                tab->consts[i].reserved = mv->flags;
                tab->const_vals[tab->consts[i].val_idx] =
                    val_make_f64_at_pool(mem, tid, uid, tab->consts[i].pool_idx);
            }
            return;
        }
    }
}

int sym_set_or_add_var(SymTable *tab, MemArena *mem, uint16_t sym_id, uint16_t mod_id,
                       Value val, uint16_t lock_type, uint8_t lock_unit,
                       uint8_t flags, bool is_decl) {
    if (!tab || !mem || sym_id >= tab->sym_count) return -1;
    StrSlice vname = tab->syms[sym_id];

    /* Clone non-container flat structs so module state owns its own pool slots */
    uint16_t vtid = val_get_type(mem, val);
    if (vtid != 0 && vtid < tab->type_count && !tab->types[vtid].is_consuming &&
        vtid != mem->vec_type_id && vtid != mem->mat_type_id &&
        vtid != mem->fn_type_id  && vtid != mem->map_type_id) {
        val = val_clone_comp(mem, val, tab->types[vtid].flat_size);
        if (mem->has_error) return -1;
    }
    if ((uint32_t)vname.off + (uint32_t)vname.len > (uint32_t)mem->perm_src_len) {
        mem->perm_src_len = (uint16_t)(vname.off + vname.len);
    }
    if (vtid == mem->fn_type_id && mem->fn_type_id != 0) {
        if (mem->src_len > mem->perm_src_len)           mem->perm_src_len = mem->src_len;
        if (mem->ast_count > mem->perm_ast_count)       mem->perm_ast_count = mem->ast_count;
        if (mem->pool_top > mem->perm_pool_top)         mem->perm_pool_top = mem->pool_top;
        if (mem->fn_src_count > mem->perm_fn_src_count) mem->perm_fn_src_count = mem->fn_src_count;
    }

    if (is_decl) {
        int own_idx = sym_find_own_var(tab, sym_id, mod_id);
        if (own_idx >= 0) {
            ModVarDef *mv = &sym_vars_mut(tab)[own_idx];
            bool is_uninit = (val_get_type(mem, mv->val) == MEM_TYPE_VOID);
            if (!is_uninit && (mv->flags & SYM_VAR_MUTABLE) == 0) {
                mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                        (int)vname.len, mem_str_ptr(mem, vname));
                return -1;
            }
            uint16_t eff_ltype = ((lock_type & 0xFFu) != 0)
                                 ? (uint16_t)(lock_type & 0xFFu)
                                 : (uint16_t)(mv->lock_type & 0xFFu);
            uint8_t  eff_lunit = (lock_unit != 0) ? lock_unit : mv->lock_unit;
            if (is_uninit && eff_lunit != 0 && eff_lunit < SYM_UNIT_LOCK_NONE &&
                vtid != MEM_TYPE_VOID && val_get_unit(mem, val) == MEM_UNIT_NONE) {
                val = val_with_unit(mem, val, eff_lunit);
            }
            if (!sym_check_var_locks(tab, mem, sym_id, eff_ltype, eff_lunit, val)) {
                return -1;
            }
            mv->lock_type = eff_ltype;
            mv->lock_unit = eff_lunit;
            mv->flags     = (uint8_t)(flags & (SYM_VAR_MUTABLE | SYM_VAR_PRIVATE | SYM_VAR_EXPLICIT_CONST));
            mv->val       = val;
            if ((mv->flags & SYM_VAR_MUTABLE) == 0 && vtid != MEM_TYPE_VOID) {
                mem_val_mark_immutable(mem, val);
                if (is_uninit && vtid < tab->type_count && tab->types[vtid].is_consuming &&
                    vtid != mem->str_type_id && tab->const_count < SYM_MAX_CONSTS) {
                    bool already_c = false;
                    for (uint8_t ci = 0; ci < tab->const_count; ++ci) {
                        if (tab->consts[ci].sym_id == sym_id && tab->consts[ci].mod_id == mod_id) {
                            already_c = true;
                            break;
                        }
                    }
                    if (!already_c) {
                        uint16_t c_pool_idx = mem_alloc_pool(mem, 1);
                        if (c_pool_idx != MEM_NO_IDX && !mem->has_error) {
                            uint8_t vuid = val_get_unit(mem, val);
                            mem_write_pool(mem, c_pool_idx, val_get_f64(mem, val));
                            if ((uint16_t)(c_pool_idx + 1u) > mem->perm_pool_top) {
                                mem->perm_pool_top = (uint16_t)(c_pool_idx + 1u);
                            }
                            Value cval = val_make_f64_at_pool(mem, vtid, vuid, c_pool_idx);
                            mv->val = cval;
                            uint8_t cid = tab->const_count++;
                            tab->consts[cid].sym_id   = sym_id;
                            tab->consts[cid].mod_id   = mod_id;
                            tab->consts[cid].type_id  = (uint8_t)vtid;
                            tab->consts[cid].unit_id  = vuid;
                            tab->consts[cid].val_idx  = cid;
                            tab->consts[cid].reserved = mv->flags;
                            tab->consts[cid].pool_idx = c_pool_idx;
                            tab->const_vals[cid]      = cval;
                        }
                    }
                }
            }
            sym_sync_var_to_const(tab, mem, mv);
            return own_idx;
        }
    } else if ((flags & SYM_VAR_QUAL_MOD) != 0) {
        int own_idx = sym_find_own_var(tab, sym_id, mod_id);
        if (own_idx >= 0) {
            ModVarDef *mv = &sym_vars_mut(tab)[own_idx];
            if ((mv->flags & SYM_VAR_PRIVATE) != 0 && mod_id != tab->cur_mod) {
                StrSlice pmod_s = sym_mod_name(tab, mod_id);
                mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                        (int)vname.len, mem_str_ptr(mem, vname),
                        (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
                return -1;
            }
            if ((mv->flags & SYM_VAR_MUTABLE) == 0 || (flags & SYM_VAR_EXPLICIT_CONST) != 0) {
                mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                        (int)vname.len, mem_str_ptr(mem, vname));
                return -1;
            }
            uint16_t eff_ltype = ((lock_type & 0xFFu) != 0)
                                 ? (uint16_t)(lock_type & 0xFFu)
                                 : (uint16_t)(mv->lock_type & 0xFFu);
            uint8_t  eff_lunit = (lock_unit != 0) ? lock_unit : mv->lock_unit;
            if (!sym_check_var_locks(tab, mem, sym_id, eff_ltype, eff_lunit, val)) {
                return -1;
            }
            mv->lock_type = eff_ltype;
            mv->lock_unit = eff_lunit;
            mv->val       = val;
            sym_sync_var_to_const(tab, mem, mv);
            return own_idx;
        }
        StrSlice qmod_s = sym_mod_name(tab, mod_id);
        if ((flags & SYM_VAR_COMPOUND_OP) != 0) {
            mem_err(mem, "Cannot apply compound assignment to undeclared variable '%.*s::%.*s'",
                    (int)qmod_s.len, mem_str_ptr(mem, qmod_s),
                    (int)vname.len, mem_str_ptr(mem, vname));
        } else {
            mem_err(mem, "Undefined variable '%.*s' in module '[%.*s]'",
                    (int)vname.len, mem_str_ptr(mem, vname),
                    (int)qmod_s.len, mem_str_ptr(mem, qmod_s));
        }
        return -1;
    } else {
        uint16_t priv_mod = MEM_NO_IDX;
        int vis_idx = sym_find_var(tab, sym_id, mod_id, &priv_mod);
        if (vis_idx >= 0) {
            ModVarDef *mv = &sym_vars_mut(tab)[vis_idx];
            if ((mv->flags & SYM_VAR_MUTABLE) == 0 || (flags & SYM_VAR_EXPLICIT_CONST) != 0) {
                mem_err(mem, "Cannot reassign or mutate immutable constant '%.*s'",
                        (int)vname.len, mem_str_ptr(mem, vname));
                return -1;
            }
            uint16_t eff_ltype = ((lock_type & 0xFFu) != 0)
                                 ? (uint16_t)(lock_type & 0xFFu)
                                 : (uint16_t)(mv->lock_type & 0xFFu);
            uint8_t  eff_lunit = (lock_unit != 0) ? lock_unit : mv->lock_unit;
            if (!sym_check_var_locks(tab, mem, sym_id, eff_ltype, eff_lunit, val)) {
                return -1;
            }
            mv->lock_type = eff_ltype;
            mv->lock_unit = eff_lunit;
            if ((flags & SYM_VAR_PRIVATE) != 0 && mv->mod_id == mod_id) {
                mv->flags |= SYM_VAR_PRIVATE;
            }
            mv->val = val;
            sym_sync_var_to_const(tab, mem, mv);
            return vis_idx;
        }
        if (priv_mod != MEM_NO_IDX) {
            StrSlice pmod_s = sym_mod_name(tab, priv_mod);
            mem_err(mem, "Variable or constant '%.*s' is private to module '[%.*s]'",
                    (int)vname.len, mem_str_ptr(mem, vname),
                    (int)pmod_s.len, mem_str_ptr(mem, pmod_s));
            return -1;
        }
        if ((flags & SYM_VAR_COMPOUND_OP) != 0) {
            mem_err(mem, "Cannot apply compound assignment to undeclared variable '%.*s'",
                    (int)vname.len, mem_str_ptr(mem, vname));
            return -1;
        }
    }

    /* Creating a new module variable or constant in `mod_id` */
    if (is_decl && lock_unit != 0 && lock_unit < SYM_UNIT_LOCK_NONE &&
        vtid != MEM_TYPE_VOID && val_get_unit(mem, val) == MEM_UNIT_NONE) {
        val = val_with_unit(mem, val, lock_unit);
    }
    if (!sym_check_var_locks(tab, mem, sym_id, lock_type, lock_unit, val)) {
        return -1;
    }
    if (!sym_ensure_var_cap(tab, mem, (uint16_t)(tab->var_count + 1u))) {
        return -1;
    }
    int new_idx = (int)tab->var_count++;
    ModVarDef *mv = &sym_vars_mut(tab)[new_idx];
    mv->sym_id    = sym_id;
    mv->mod_id    = mod_id;
    mv->lock_type = (uint16_t)(lock_type & 0xFFu);
    mv->lock_unit = lock_unit;
    mv->flags     = (uint8_t)(flags & (SYM_VAR_MUTABLE | SYM_VAR_PRIVATE | SYM_VAR_EXPLICIT_CONST));
    mv->val       = val;
    if ((mv->flags & SYM_VAR_MUTABLE) == 0 && vtid != MEM_TYPE_VOID) {
        mem_val_mark_immutable(mem, val);
        if (vtid < tab->type_count && tab->types[vtid].is_consuming &&
            vtid != mem->str_type_id && tab->const_count < SYM_MAX_CONSTS) {
            bool already_c = false;
            for (uint8_t ci = 0; ci < tab->const_count; ++ci) {
                if (tab->consts[ci].sym_id == sym_id && tab->consts[ci].mod_id == mod_id) {
                    already_c = true;
                    break;
                }
            }
            if (!already_c) {
                uint16_t c_pool_idx = mem_alloc_pool(mem, 1);
                if (c_pool_idx != MEM_NO_IDX && !mem->has_error) {
                    uint8_t vuid = val_get_unit(mem, val);
                    mem_write_pool(mem, c_pool_idx, val_get_f64(mem, val));
                    if ((uint16_t)(c_pool_idx + 1u) > mem->perm_pool_top) {
                        mem->perm_pool_top = (uint16_t)(c_pool_idx + 1u);
                    }
                    Value cval = val_make_f64_at_pool(mem, vtid, vuid, c_pool_idx);
                    mv->val = cval;
                    uint8_t cid = tab->const_count++;
                    tab->consts[cid].sym_id   = sym_id;
                    tab->consts[cid].mod_id   = mod_id;
                    tab->consts[cid].type_id  = (uint8_t)vtid;
                    tab->consts[cid].unit_id  = vuid;
                    tab->consts[cid].val_idx  = cid;
                    tab->consts[cid].reserved = mv->flags;
                    tab->consts[cid].pool_idx = c_pool_idx;
                    tab->const_vals[cid]      = cval;
                }
            }
        }
    }
    return new_idx;
}

bool sym_reset_module(SymTable *tab, MemArena *mem, const char *mod_name) {
    if (mem) memset(mem->ov_bc_entry, 0, sizeof(mem->ov_bc_entry));
    if (!tab || !mem) return false;
    const char *eff_name = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    int mid = sym_find_mod(tab, mem, eff_name);
    if (mid < 0) return true; /* Module has no state yet; reset is a clean no-op */
    uint16_t mod_id = (uint16_t)mid;

    /* 1. Remove all ModVarDef entries owned by `mod_id` */
    ModVarDef *vars = sym_vars_mut(tab);
    uint16_t w_var = 0;
    for (uint16_t i = 0; i < tab->var_count; ++i) {
        if (vars[i].mod_id != mod_id) {
            if (w_var != i) vars[w_var] = vars[i];
            w_var++;
        }
    }
    tab->var_count = w_var;

    /* 2. Remove all ConstDef entries owned by `mod_id` */
    uint8_t w_const = 0;
    for (uint8_t i = 0; i < tab->const_count; ++i) {
        if (tab->consts[i].mod_id != mod_id) {
            if (w_const != i) {
                tab->consts[w_const]     = tab->consts[i];
                tab->consts[w_const].val_idx = w_const;
                tab->const_vals[w_const] = tab->const_vals[i];
            }
            w_const++;
        }
    }
    tab->const_count = w_const;

    /* 3. Remove all OpOverload entries owned by `mod_id` and rebuild dispatch chains */
    uint16_t w_ov = 0;
    for (uint16_t i = 0; i < tab->overload_count; ++i) {
        if (tab->overloads[i].mod_id != mod_id) {
            if (w_ov != i) tab->overloads[w_ov] = tab->overloads[i];
            w_ov++;
        }
    }
    tab->overload_count = w_ov;
    sym_rebuild_index(tab);

    /* 4. Reclaim any pool/heap memory that was only referenced by `mod_id` */
    mem_rewind_scratch(mem);
    return true;
}

bool sym_has_priv_ov(const SymTable *tab, uint16_t sym_id, OpFixity fix_filter,
                     uint16_t *out_priv_mod) {
    if (out_priv_mod) *out_priv_mod = MEM_NO_IDX;
    if (!tab || sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (!sym_mod_enabled(tab, ov->mod_id)) continue;
        if (fix_filter != FIX_SYNTAX && ov->fixity != (uint8_t)fix_filter) continue;
        if ((ov->reserved & SYM_OV_PRIVATE) != 0 && ov->mod_id != tab->cur_mod) {
            if (out_priv_mod) *out_priv_mod = ov->mod_id;
            return true;
        }
    }
    return false;
}

uint16_t sym_intern(SymTable *tab, MemArena *mem, StrSlice s) {
    for (uint16_t i = 0; i < tab->sym_count; ++i) {
        if (mem_slice_eq(mem, tab->syms[i], s)) return i;
    }
    if (tab->sym_count >= SYM_MAX_SYMS) {
        mem_err(mem, "Symbol table overflow (max %d)", SYM_MAX_SYMS);
        return 0;
    }
    uint16_t id = tab->sym_count++;
    tab->syms[id]         = s;
    tab->sym_first_ov[id] = MEM_NO_IDX;
    tab->sym_last_ov[id]  = MEM_NO_IDX;
    if ((uint32_t)s.off + (uint32_t)s.len > mem->perm_src_len &&
        (uint32_t)s.off + (uint32_t)s.len <= mem->src_len) {
        mem->perm_src_len = (uint16_t)(s.off + s.len);
    }
    return id;
}

int sym_find(const SymTable *tab, const MemArena *mem, const char *str, uint16_t len) {
    for (uint16_t i = 0; i < tab->sym_count; ++i) {
        StrSlice s = tab->syms[i];
        if (s.len == len && memcmp(mem_str_ptr(mem, s), str, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool sym_has_active_op(const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity != FIX_FUNC && ov->fixity != FIX_SYNTAX &&
            sym_ov_is_visible(tab, ov)) {
            return true;
        }
    }
    return false;
}

uint8_t sym_get_active_syntax(const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return 0;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == FIX_SYNTAX && sym_ov_is_visible(tab, ov)) {
            return ov->meta_tag;
        }
    }
    return 0;
}

int sym_get_active_prec(const SymTable *tab, uint16_t sym_id, OpFixity fixity) {
    if (sym_id >= tab->sym_count) return -1;
    int best = -1;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == (uint8_t)fixity && sym_ov_is_visible(tab, ov)) {
            if ((int)ov->prec > best) best = (int)ov->prec;
        }
    }
    return best;
}

uint8_t sym_active_meta_tag(const SymTable *tab, uint16_t sym_id, OpFixity fix_filter) {
    if (sym_id >= tab->sym_count) return 0;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (sym_ov_is_visible(tab, ov)) {
            if (fix_filter == FIX_FUNC && ov->fixity != FIX_FUNC) continue;
            if ((fix_filter == FIX_PREFIX || fix_filter == FIX_POSTFIX) && ov->fixity != (uint8_t)fix_filter) continue;
            if (fix_filter != FIX_FUNC && (ov->fixity == FIX_FUNC || ov->fixity == FIX_SYNTAX)) continue;
            if ((ov->meta_tag & SYM_META_LAZY_MASK) != 0 ||
                delegate_is_meta_intrinsic((IntrinsicId)ov->meta_tag)) {
                return ov->meta_tag;
            }
        }
    }
    return 0;
}

bool sym_is_reserved_kw(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len) {
    return hardcode_is_reserved_kw(tab, mem, name, len);
}

uint16_t sym_find_type(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len) {
    uint16_t wc = hardcode_resolve_wildcard_type(tab, mem, name, len);
    if (wc != MEM_TYPE_VOID) return wc;
    for (uint16_t i = 1; i < tab->type_count; ++i) {
        if (!sym_mod_enabled(tab, tab->types[i].mod_id)) continue;
        if ((tab->types[i].reserved & SYM_TYPE_PRIVATE) != 0 &&
            tab->types[i].mod_id != tab->cur_mod) continue;
        if (tab->types[i].parser_intr == INTR_ANY_TYPE ||
            tab->types[i].parser_intr == INTR_NONE_UNIT) continue;
        StrSlice s = tab->types[i].name;
        if (s.len == len && memcmp(mem_str_ptr(mem, s), name, len) == 0) {
            return i;
        }
    }
    return MEM_TYPE_VOID;
}

uint8_t sym_find_unit(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len) {
    uint8_t wc = hardcode_resolve_wildcard_unit(tab, mem, name, len);
    if (wc != MEM_UNIT_NONE) return wc;
    for (uint8_t i = 1; i < tab->unit_count; ++i) {
        StrSlice s = tab->units[i];
        if (s.len == len && memcmp(mem_str_ptr(mem, s), name, len) == 0) {
            return i;
        }
    }
    return MEM_UNIT_NONE;
}

uint8_t sym_find_or_add_unit(SymTable *tab, MemArena *mem, const char *name, uint16_t len) {
    uint8_t existing = sym_find_unit(tab, mem, name, len);
    if (existing != MEM_UNIT_NONE) return existing;
    if (tab->unit_count >= SYM_MAX_UNITS) {
        mem_err(mem, "Unit table overflow (max %d)", SYM_MAX_UNITS);
        return MEM_UNIT_NONE;
    }
    uint8_t id = tab->unit_count++;
    tab->units[id] = mem_intern_str(mem, name, len);
    return id;
}

bool sym_resolve_field(const SymTable *tab, const MemArena *mem, uint16_t type_id,
                       const char *fname, uint16_t flen,
                       uint16_t *out_field_type, uint16_t *out_flat_off) {
    if (type_id == 0 || type_id >= tab->type_count) return false;
    const TypeDef *td = &tab->types[type_id];
    if (td->is_consuming || td->field_count == 0) return false;
    for (uint8_t i = 0; i < td->field_count; ++i) {
        const FieldDef *fd = &tab->fields[td->start_idx + i];
        if (fd->name.len == flen && memcmp(mem_str_ptr(mem, fd->name), fname, flen) == 0) {
            *out_field_type = fd->type_id;
            *out_flat_off   = fd->flat_offset;
            return true;
        }
    }
    return false;
}

const char *sym_type_name(const SymTable *tab, const MemArena *mem, uint16_t type_id,
                          char *buf, size_t cap) {
    if (type_id == SYM_TYPE_ANY) {
        for (uint16_t i = 1; i < tab->type_count; ++i) {
            if (tab->types[i].parser_intr == INTR_ANY_TYPE) {
                StrSlice s = tab->types[i].name;
                snprintf(buf, cap, "%.*s", (int)s.len, mem_str_ptr(mem, s));
                return buf;
            }
        }
        snprintf(buf, cap, "void");
        return buf;
    }
    if (type_id == 0 || type_id >= tab->type_count) {
        snprintf(buf, cap, "void");
        return buf;
    }
    StrSlice s = tab->types[type_id].name;
    snprintf(buf, cap, "%.*s", (int)s.len, mem_str_ptr(mem, s));
    return buf;
}

const char *sym_unit_name(const SymTable *tab, const MemArena *mem, uint8_t unit_id,
                          char *buf, size_t cap) {
    if (unit_id == SYM_UNIT_GENERIC) {
        for (uint16_t i = 1; i < tab->type_count; ++i) {
            if (tab->types[i].parser_intr == INTR_ANY_TYPE) {
                StrSlice s = tab->types[i].name;
                snprintf(buf, cap, "%.*s", (int)s.len, mem_str_ptr(mem, s));
                return buf;
            }
        }
        snprintf(buf, cap, "none");
        return buf;
    }
    if (unit_id == 0 || unit_id == SYM_UNIT_OMITTED || unit_id == SYM_UNIT_LOCK_NONE ||
        unit_id >= tab->unit_count) {
        for (uint16_t i = 1; i < tab->type_count; ++i) {
            if (tab->types[i].parser_intr == INTR_NONE_UNIT) {
                StrSlice s = tab->types[i].name;
                snprintf(buf, cap, "%.*s", (int)s.len, mem_str_ptr(mem, s));
                return buf;
            }
        }
        snprintf(buf, cap, "none");
        return buf;
    }
    StrSlice s = tab->units[unit_id];
    snprintf(buf, cap, "%.*s", (int)s.len, mem_str_ptr(mem, s));
    return buf;
}

IntrinsicId sym_lookup_intrinsic(const char *s, uint16_t len) {
    return delegate_lookup_intrinsic(s, len);
}

static bool sym_register_extern_impl(SymTable *tab, MemArena *mem, const char *name,
                                     EngExternFn fn, void *user_ctx,
                                     void (*fn_free)(void *user_ctx)) {
    if (!name || !fn) return false;
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen >= sizeof(tab->externs[0].name)) {
        mem_err(mem, "Extern name '%s' is empty or exceeds 31 chars", name ? name : "");
        return false;
    }
    for (uint8_t i = 0; i < tab->extern_count; ++i) {
        if (strcmp(tab->externs[i].name, name) == 0) {
            void (*old_free)(void *) = tab->externs[i].fn_free;
            void       *old_ctx      = tab->externs[i].user_ctx;
            tab->externs[i].fn       = fn;
            tab->externs[i].user_ctx = user_ctx;
            tab->externs[i].fn_free  = fn_free;
            if (old_free && old_ctx && !(old_free == fn_free && old_ctx == user_ctx)) {
                old_free(old_ctx);
            }
            return true;
        }
    }
    if (tab->extern_count >= SYM_MAX_EXTERNS) {
        mem_err(mem, "Extern table overflow (max %d)", SYM_MAX_EXTERNS);
        return false;
    }
    uint8_t id = tab->extern_count++;
    memcpy(tab->externs[id].name, name, nlen + 1);
    tab->externs[id].fn       = fn;
    tab->externs[id].user_ctx = user_ctx;
    tab->externs[id].fn_free  = fn_free;
    return true;
}

bool sym_register_extern(SymTable *tab, MemArena *mem, const char *name,
                         EngExternFn fn, void *user_ctx) {
    return sym_register_extern_impl(tab, mem, name, fn, user_ctx, NULL);
}

bool sym_register_extern_full(SymTable *tab, MemArena *mem, const char *name,
                              EngExternFn fn, void *user_ctx,
                              void (*fn_free)(void *user_ctx)) {
    return sym_register_extern_impl(tab, mem, name, fn, user_ctx, fn_free);
}

void sym_free_externs(SymTable *tab) {
    if (!tab) return;
    for (uint8_t i = 0; i < tab->extern_count; ++i) {
        if (tab->externs[i].fn_free) {
            tab->externs[i].fn_free(tab->externs[i].user_ctx);
        }
        tab->externs[i].fn       = NULL;
        tab->externs[i].user_ctx = NULL;
        tab->externs[i].fn_free  = NULL;
    }
    tab->extern_count = 0;
}

const ExternDef *sym_find_extern(const SymTable *tab, const MemArena *mem, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return NULL;
    StrSlice s = tab->syms[sym_id];
    const char *p = mem_str_ptr(mem, s);
    for (uint8_t i = 0; i < tab->extern_count; ++i) {
        if ( strlen(tab->externs[i].name) == s.len &&
             memcmp(tab->externs[i].name, p, s.len) == 0 ) {
            return &tab->externs[i];
        }
    }
    return NULL;
}

int sym_find_extern_by_name(const SymTable *tab, const char *name, uint16_t len) {
    for (uint8_t i = 0; i < tab->extern_count; ++i) {
        if (strlen(tab->externs[i].name) == len &&
            memcmp(tab->externs[i].name, name, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}
