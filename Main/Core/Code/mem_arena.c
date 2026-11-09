/*
 * mem_arena.c — Arena Lifecycle, String/AST/Frame Pools & Heap Spill Blocks
 */
#include "mem_internal.h"

/* ============================================================================
 * 1. ARENA LIFECYCLE, HEAP SPILL CLEANUP & ERROR HANDLING
 * ============================================================================ */

void mem_free_all_heap(MemArena *mem) {
    if (!mem) return;
    for (uint16_t i = 0; i < MEM_MAX_HEAP_BLOCKS; ++i) {
        if (mem->heap_blocks[i].data != NULL) {
            free(mem->heap_blocks[i].data);
            mem->heap_blocks[i].data = NULL;
        }
        mem->heap_blocks[i].cap_slots = 0;
        mem->heap_blocks[i].owner_hdr = 0;
        mem->heap_blocks[i].in_use    = 0;
        mem->heap_blocks[i].marked    = 0;
    }
    mem->heap_active_count = 0;
    mem->heap_total_slots  = 0;
}

void mem_free_ast_heap(MemArena *mem) {
    if (!mem) return;
    if (mem->heap_ast_magic == MEM_HEAP_AST_MAGIC) {
        free(mem->heap_ast);
    }
    mem->heap_ast       = NULL;
    mem->heap_ast_magic = 0;
    mem->ast_cap        = MEM_SBO_AST_NODES;
}

bool mem_ensure_ast_cap(MemArena *mem, uint16_t needed_cap) {
    if (!mem) return false;
    if (mem->ast_cap == 0) mem->ast_cap = MEM_SBO_AST_NODES;
    if (needed_cap <= mem->ast_cap) return true;
    if (needed_cap > MEM_MAX_AST_NODES) {
        mem_err(mem, "AST node pool overflow");
        return false;
    }
    uint32_t new_cap = (uint32_t)mem->ast_cap;
    while (new_cap < (uint32_t)needed_cap) {
        new_cap *= 2u;
        if (new_cap > MEM_MAX_AST_NODES) {
            new_cap = MEM_MAX_AST_NODES;
            break;
        }
    }
    AstNode *new_ast = (AstNode *)calloc(new_cap, sizeof(AstNode));
    if (!new_ast) {
        mem_err(mem, "Out of memory expanding AST node pool");
        return false;
    }
    if (mem->ast_count > 0) {
        memcpy(new_ast, mem_ast_base(mem), (size_t)mem->ast_count * sizeof(AstNode));
    }
    if (mem->heap_ast_magic == MEM_HEAP_AST_MAGIC) {
        free(mem->heap_ast);
    }
    mem->heap_ast       = new_ast;
    mem->heap_ast_magic = MEM_HEAP_AST_MAGIC;
    mem->ast_cap        = (uint16_t)new_cap;
    return true;
}

void mem_init(MemArena *mem) {
    mem_free_ast_heap(mem);
    memset(mem, 0, sizeof(*mem));
    mem->ast_cap               = MEM_SBO_AST_NODES;
    mem->jump_val              = val_void();
    mem->last_mod_val          = val_void();
    mem->invalid_ast_mut.kind  = 0xFFu;
    mem->invalid_ast_mut.ref   = MEM_NO_IDX;
    mem->invalid_ast_mut.left  = MEM_NO_IDX;
    mem->invalid_ast_mut.right = MEM_NO_IDX;
}

void mem_err(MemArena *mem, const char *fmt, ...) {
    if (mem->has_error) return;
    mem->has_error = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mem->err_msg, sizeof(mem->err_msg), fmt, ap);
    va_end(ap);
}

/* Attach a source-file line number to the most recent runtime error message
 * (`... (line N)`). App-side tooling extracts this suffix to point at the
 * exact statement. No-op if no error is pending, the message already cites a
 * line, or there is no room left in the error buffer. */
void mem_err_attach_line(MemArena *mem, int line) {
    if (!mem || !mem->has_error || line <= 0) return;
    const size_t len = strlen(mem->err_msg);
    if (len >= sizeof(mem->err_msg)) return;
    if (strstr(mem->err_msg, "line ") != NULL) return;
    if (len + 20u >= sizeof(mem->err_msg)) return;
    snprintf(mem->err_msg + len, sizeof(mem->err_msg) - len, " (line %d)", line);
}

void mem_clear_err(MemArena *mem) {
    mem->has_error  = false;
    mem->err_msg[0] = '\0';
}

void mem_out_append(MemArena *mem, const char *s, size_t len) {
    if (!mem || !s || len == 0) return;
    if (mem->out_len + 1u >= MEM_OUT_BUF_CAP) return;
    size_t avail = (size_t)(MEM_OUT_BUF_CAP - 1u - mem->out_len);
    size_t n = (len < avail) ? len : avail;
    if (n > 0) {
        memcpy(mem->out_buf + mem->out_len, s, n);
        mem->out_len = (uint16_t)(mem->out_len + n);
        mem->out_buf[mem->out_len] = '\0';
    }
}

void mem_out_clear(MemArena *mem) {
    if (!mem) return;
    mem->out_len    = 0;
    mem->out_buf[0] = '\0';
}

void mem_snapshot_perm(MemArena *mem) {
    mem->perm_src_len      = mem->src_len;
    mem->perm_ast_count    = mem->ast_count;
    mem->perm_pool_top     = mem->pool_top;
    mem->perm_fn_src_count = mem->fn_src_count;
}

void mem_snapshot_base(MemArena *mem) {
    if (!mem) return;
    mem_snapshot_perm(mem);
    mem->base_src_len        = mem->perm_src_len;
    mem->base_ast_count      = mem->perm_ast_count;
    mem->base_pool_top       = mem->perm_pool_top;
    mem->base_fn_src_count   = mem->perm_fn_src_count;
    mem->base_fast_real_type = mem->fast_real_type;
    mem->base_str_type_id    = mem->str_type_id;
    mem->base_vec_type_id    = mem->vec_type_id;
    mem->base_mat_type_id    = mem->mat_type_id;
    mem->base_fn_type_id     = mem->fn_type_id;
    mem->base_map_type_id    = mem->map_type_id;
}

void mem_rewind_to_base(MemArena *mem) {
    if (!mem) return;
    mem_free_all_heap(mem);
    if (mem->heap_ast_magic == MEM_HEAP_AST_MAGIC && mem->base_ast_count <= MEM_SBO_AST_NODES) {
        if (mem->base_ast_count > 0) {
            memcpy(mem->sbo_ast, mem->heap_ast, (size_t)mem->base_ast_count * sizeof(AstNode));
        }
        mem_free_ast_heap(mem);
    }
    mem->src_len             = mem->base_src_len;
    mem->perm_src_len        = mem->base_src_len;
    mem->ast_count           = mem->base_ast_count;
    mem->perm_ast_count      = mem->base_ast_count;
    mem->pool_top            = mem->base_pool_top;
    mem->perm_pool_top       = mem->base_pool_top;
    mem->fn_src_count        = mem->base_fn_src_count;
    mem->perm_fn_src_count   = mem->base_fn_src_count;
    mem->fast_real_type      = mem->base_fast_real_type;
    mem->str_type_id         = mem->base_str_type_id;
    mem->vec_type_id         = mem->base_vec_type_id;
    mem->mat_type_id         = mem->base_mat_type_id;
    mem->fn_type_id          = mem->base_fn_type_id;
    mem->map_type_id         = mem->base_map_type_id;
    mem->sp                  = 0;
    mem->jump_active         = 0;
    mem->jump_target_id      = 0;
    mem->jump_val            = val_void();
    mem->last_mod_val        = val_void();
    mem->cur_mod             = 0;
    mem->temp_root_count     = 0;
    memset(mem->local_slot_names, 0, sizeof(mem->local_slot_names));
    mem->vrange_cnt          = 0;
    memset(mem->vrange_hdr, 0, sizeof(mem->vrange_hdr));
    mem->imported_file_count = 0;
    mem->eval_depth          = 0;
    mem->call_depth          = 0;
    mem->eval_steps          = 0;
    for (uint8_t i = 0; i < MEM_MAX_LABELS; ++i) {
        mem->label_ast[i] = MEM_NO_IDX;
    }
    mem_clear_err(mem);
}

void mem_rewind_scratch(MemArena *mem) {
    mem->ast_count       = mem->perm_ast_count;
    mem->sp              = 0;
    mem->jump_active     = 0;
    mem->jump_target_id  = 0;
    mem->jump_val        = val_void();
    mem->fn_src_count    = mem->perm_fn_src_count;
    mem->temp_root_count = 0;
    memset(mem->local_slot_names, 0, sizeof(mem->local_slot_names));
    mem->vrange_cnt      = 0;
    memset(mem->vrange_hdr, 0, sizeof(mem->vrange_hdr));
    mem->eval_depth      = 0;
    mem->call_depth      = 0;
    mem->eval_steps      = 0;
    for (uint8_t i = 0; i < MEM_MAX_LABELS; ++i) {
        mem->label_ast[i] = MEM_NO_IDX;
    }
    for (uint16_t i = 0; i < MEM_MAX_HEAP_BLOCKS; ++i) {
        if (mem->heap_blocks[i].in_use == 2u) {
            mem->heap_blocks[i].in_use = 3u;
        }
    }

    if (mem->sym_tab != NULL && mem->sym_tab->var_count > 0) {
        (void)mem_compact_scratch(mem, NULL, 0);
        mem_compact_strings(mem);
    } else {
        mem_free_all_heap(mem);
        mem->pool_top = mem->perm_pool_top;
        mem->src_len  = mem->perm_src_len;
    }
}

/* ============================================================================
 * 2. UTF-8 STRING INTERNING & SLICES
 * ============================================================================ */

StrSlice mem_intern_str(MemArena *mem, const char *str, uint16_t len) {
    for (uint16_t i = 0; i + len <= mem->src_len; ++i) {
        if (memcmp(mem->src_buf + i, str, len) == 0) {
            StrSlice s = { i, len };
            return s;
        }
    }
    if ((uint32_t)mem->src_len + (uint32_t)len >= (uint32_t)MEM_SRC_BUF_CAP) {
        char *tmp_copy = NULL;
        const char *eff_str = str;
        if (str >= mem->src_buf && str < mem->src_buf + MEM_SRC_BUF_CAP && len > 0) {
            tmp_copy = (char *)malloc(len);
            if (tmp_copy) {
                memcpy(tmp_copy, str, len);
                eff_str = tmp_copy;
            }
        }
        mem_compact_strings(mem);
        for (uint16_t i = 0; i + len <= mem->src_len; ++i) {
            if (memcmp(mem->src_buf + i, eff_str, len) == 0) {
                if (tmp_copy) free(tmp_copy);
                StrSlice s = { i, len };
                return s;
            }
        }
        if ((uint32_t)mem->src_len + (uint32_t)len >= (uint32_t)MEM_SRC_BUF_CAP) {
            if (tmp_copy) free(tmp_copy);
            mem_err(mem, "Source arena buffer overflow (%u bytes)", (unsigned)MEM_SRC_BUF_CAP);
            StrSlice empty = { 0, 0 };
            return empty;
        }
        StrSlice s = { mem->src_len, len };
        memcpy(mem->src_buf + mem->src_len, eff_str, len);
        mem->src_len = (uint16_t)(mem->src_len + len);
        if (tmp_copy) free(tmp_copy);
        return s;
    }
    StrSlice s = { mem->src_len, len };
    memcpy(mem->src_buf + mem->src_len, str, len);
    mem->src_len = (uint16_t)(mem->src_len + len);
    return s;
}

const char *mem_str_ptr(const MemArena *mem, StrSlice s) {
    return mem->src_buf + s.off;
}

bool mem_slice_eq(const MemArena *mem, StrSlice a, StrSlice b) {
    if (a.len != b.len) return false;
    return memcmp(mem->src_buf + a.off, mem->src_buf + b.off, a.len) == 0;
}

bool mem_slice_eq_cstr(const MemArena *mem, StrSlice s, const char *cstr) {
    size_t len = strlen(cstr);
    if (s.len != len) return false;
    return memcmp(mem->src_buf + s.off, cstr, len) == 0;
}

/* ============================================================================
 * 3. FLOAT POOL, SBO-TO-HEAP SPILL ALLOCATOR & UNIFIED PAYLOAD ACCESSORS
 * ============================================================================ */

uint16_t mem_alloc_pool(MemArena *mem, uint16_t count) {
    if (!mem || (uint32_t)mem->pool_top + (uint32_t)count > (uint32_t)MEM_MAX_F64_POOL) {
        if (mem) mem_err(mem, "Float value pool exhausted");
        return MEM_NO_IDX;
    }
    uint16_t idx = mem->pool_top;
    mem->pool_top = (uint16_t)(mem->pool_top + count);
    return idx;
}

double mem_read_pool(const MemArena *mem, uint16_t idx) {
    return (mem && idx < MEM_MAX_F64_POOL) ? mem->f64_pool[idx] : 0.0;
}

void mem_write_pool(MemArena *mem, uint16_t idx, double val) {
    if (!mem || mem->has_error) return;
    if (idx < MEM_MAX_F64_POOL) {
        mem->f64_pool[idx] = canonicalize_f64(val);
    }
}

uint16_t mem_heap_alloc_block(MemArena *mem, uint16_t owner_hdr, uint32_t slots) {
    if (!mem) return MEM_NO_IDX;
    if (slots == 0) slots = 1;
    if (slots > MEM_MAX_HEAP_SLOTS) {
        mem_err(mem, "Container allocation (%u slots) exceeds heap block limit", (unsigned)slots);
        return MEM_NO_IDX;
    }
    /* Find a free HeapBlock slot; if full, trigger mark-sweep GC first */
    int free_id = -1;
    for (uint16_t i = 0; i < MEM_MAX_HEAP_BLOCKS; ++i) {
        if (!mem->heap_blocks[i].in_use) {
            free_id = (int)i;
            break;
        }
    }
    if (free_id < 0) {
        (void)mem_compact_scratch(mem, NULL, 0);
        for (uint16_t i = 0; i < MEM_MAX_HEAP_BLOCKS; ++i) {
            if (!mem->heap_blocks[i].in_use) {
                free_id = (int)i;
                break;
            }
        }
    }
    if (free_id < 0) {
        mem_err(mem, "Heap block table exhausted (max %u blocks)", (unsigned)MEM_MAX_HEAP_BLOCKS);
        return MEM_NO_IDX;
    }
    double *ptr = (double *)calloc((size_t)slots, sizeof(double));
    if (!ptr) {
        mem_err(mem, "Out of heap memory allocating %u slots", (unsigned)slots);
        return MEM_NO_IDX;
    }
    HeapBlock *hb = &mem->heap_blocks[free_id];
    hb->data      = ptr;
    hb->cap_slots = slots;
    hb->owner_hdr = owner_hdr;
    hb->in_use    = 1u;
    hb->marked    = 0u;
    mem->heap_active_count++;
    mem->heap_total_slots += slots;
    return (uint16_t)(MEM_HEAP_IDX_BIT | (uint16_t)free_id);
}

bool mem_heap_realloc_block(MemArena *mem, uint16_t data_idx, uint32_t new_slots) {
    if (!mem || !mem_is_heap_idx(data_idx)) return false;
    uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
    if (hid >= MEM_MAX_HEAP_BLOCKS || !mem->heap_blocks[hid].in_use) {
        mem_err(mem, "Invalid heap block handle");
        return false;
    }
    if (new_slots > MEM_MAX_HEAP_SLOTS) {
        mem_err(mem, "Container allocation (%u slots) exceeds heap block limit", (unsigned)new_slots);
        return false;
    }
    HeapBlock *hb = &mem->heap_blocks[hid];
    if (new_slots <= hb->cap_slots) return true;
    double *new_ptr = (double *)realloc(hb->data, (size_t)new_slots * sizeof(double));
    if (!new_ptr) {
        mem_err(mem, "Out of heap memory resizing to %u slots", (unsigned)new_slots);
        return false;
    }
    memset(new_ptr + hb->cap_slots, 0, (size_t)(new_slots - hb->cap_slots) * sizeof(double));
    mem->heap_total_slots += (new_slots - hb->cap_slots);
    hb->data      = new_ptr;
    hb->cap_slots = new_slots;
    return true;
}

void mem_heap_free_block(MemArena *mem, uint16_t data_idx) {
    if (!mem || !mem_is_heap_idx(data_idx)) return;
    uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
    if (hid >= MEM_MAX_HEAP_BLOCKS || !mem->heap_blocks[hid].in_use) return;
    HeapBlock *hb = &mem->heap_blocks[hid];
    if (hb->data) {
        free(hb->data);
        hb->data = NULL;
    }
    if (mem->heap_total_slots >= hb->cap_slots) {
        mem->heap_total_slots -= hb->cap_slots;
    } else {
        mem->heap_total_slots = 0;
    }
    if (mem->heap_active_count > 0) {
        mem->heap_active_count--;
    }
    hb->cap_slots = 0;
    hb->owner_hdr = 0;
    hb->in_use    = 0;
    hb->marked    = 0;
}

double *mem_data_ptr(MemArena *mem, uint16_t data_idx, uint32_t offset) {
    if (!mem) return NULL;
    if (mem_is_heap_idx(data_idx)) {
        uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
        if (hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[hid].in_use &&
            mem->heap_blocks[hid].data != NULL && offset < mem->heap_blocks[hid].cap_slots) {
            return &mem->heap_blocks[hid].data[offset];
        }
        return NULL;
    }
    uint32_t pool_slot = (uint32_t)data_idx + offset;
    if (pool_slot < MEM_MAX_F64_POOL) {
        return &mem->f64_pool[pool_slot];
    }
    return NULL;
}

const double *mem_data_ptr_ro(const MemArena *mem, uint16_t data_idx, uint32_t offset) {
    if (!mem) return NULL;
    if (mem_is_heap_idx(data_idx)) {
        uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
        if (hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[hid].in_use &&
            mem->heap_blocks[hid].data != NULL && offset < mem->heap_blocks[hid].cap_slots) {
            return &mem->heap_blocks[hid].data[offset];
        }
        return NULL;
    }
    uint32_t pool_slot = (uint32_t)data_idx + offset;
    if (pool_slot < MEM_MAX_F64_POOL) {
        return &mem->f64_pool[pool_slot];
    }
    return NULL;
}

double mem_data_read(const MemArena *mem, uint16_t data_idx, uint32_t offset) {
    const double *p = mem_data_ptr_ro(mem, data_idx, offset);
    return p ? *p : 0.0;
}

void mem_data_write(MemArena *mem, uint16_t data_idx, uint32_t offset, double val) {
    if (!mem || mem->has_error) return;
    double *p = mem_data_ptr(mem, data_idx, offset);
    if (p) {
        *p = canonicalize_f64(val);
    }
}

/* Should a container payload of `total_slots` be placed in the SBO `f64_pool`? */
