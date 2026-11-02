/*
 * sym_table.c — Dynamic Module, Type, Unit, Struct, Symbol, Overload & Extern Implementation
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
}

void sym_init(SymTable *tab) {
    if (!tab) return;
    sym_free_heap(tab);
    memset(tab, 0, sizeof(*tab));
    tab->mod_cap    = SYM_SBO_MODULES;
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
    return id;
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

bool sym_mod_enabled(const SymTable *tab, uint16_t mod_id) {
    if (!tab || mod_id >= tab->mod_count) {
        return false;
    }
    return sym_mod_active(tab)[mod_id] != 0;
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
            sym_mod_enabled(tab, ov->mod_id)) {
            return true;
        }
    }
    return false;
}

uint8_t sym_get_active_syntax(const SymTable *tab, uint16_t sym_id) {
    if (sym_id >= tab->sym_count) return 0;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (ov->fixity == FIX_SYNTAX && sym_mod_enabled(tab, ov->mod_id)) {
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
        if (ov->fixity == (uint8_t)fixity && sym_mod_enabled(tab, ov->mod_id)) {
            if ((int)ov->prec > best) best = (int)ov->prec;
        }
    }
    return best;
}

uint8_t sym_active_meta_tag(const SymTable *tab, uint16_t sym_id, OpFixity fix_filter) {
    if (sym_id >= tab->sym_count) return 0;
    for (uint16_t i = tab->sym_first_ov[sym_id]; i != MEM_NO_IDX; i = tab->ov_next[i]) {
        const OpOverload *ov = &tab->overloads[i];
        if (sym_mod_enabled(tab, ov->mod_id)) {
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
            /* Replace: hand the previous registration's destructor its ctx
             * BEFORE the pointers are overwritten (JNI bridges keep global
             * refs / heap allocations inside user_ctx). */
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
