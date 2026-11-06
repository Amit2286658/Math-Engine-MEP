/*
 * mem_gc.c — Scratch Pool, Heap Block & String Arena Sliding Compaction GC
 */
#include "mem_internal.h"

/* ============================================================================
 * 5. SCRATCH POOL & HEAP BLOCK MARK-SWEEP / SLIDING COMPACTION GC
 * ============================================================================ */

static void gc_mark_val_rec(MemArena *mem, Value v, uint16_t perm_top, uint16_t old_top, uint8_t depth);

static void gc_mark_vec_hdr(MemArena *mem, uint16_t hdr_idx, uint16_t perm_top, uint16_t old_top, uint8_t depth) {
    if (depth > 16 || (uint32_t)hdr_idx + MEM_VEC_HDR_SLOTS > old_top) return;
    if (hdr_idx >= perm_top) {
        if (mem->gc_mark[hdr_idx] == 2u) return;
        mem->gc_mark[hdr_idx] = 2u;
        for (uint16_t k = 1; k < MEM_VEC_HDR_SLOTS; ++k) {
            mem->gc_mark[hdr_idx + k] = 1u;
        }
    }
    uint16_t raw_hdr0 = (uint16_t)mem->f64_pool[hdr_idx + 0];
    uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
    uint32_t len      = (uint32_t)mem->f64_pool[hdr_idx + 1];
    uint32_t cap      = (uint32_t)mem->f64_pool[hdr_idx + 2];
    uint16_t data_idx = (uint16_t)mem->f64_pool[hdr_idx + 3];

    if (len == 0) {
        if (mem_is_heap_idx(data_idx)) {
            mem_heap_free_block(mem, data_idx);
        }
        mem->f64_pool[hdr_idx + 2] = 0.0;
        mem->f64_pool[hdr_idx + 3] = 0.0;
        return;
    }

    if (mem_is_heap_idx(data_idx)) {
        uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
        if (hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[hid].in_use) {
            mem->heap_blocks[hid].marked = 1u;
        }
        if (elem_tid == 0xFEu) {
            for (uint32_t i = 0; i < len; ++i) {
                const double *p = mem_data_ptr_ro(mem, data_idx, i);
                if (p) {
                    Value ev = 0;
                    memcpy(&ev, p, sizeof(Value));
                    gc_mark_val_rec(mem, ev, perm_top, old_top, (uint8_t)(depth + 1));
                }
            }
        }
        return;
    }

    /* Reclaim slack SBO vector capacity during compaction */
    if (cap > len && data_idx >= perm_top) {
        mem->f64_pool[hdr_idx + 2] = (double)len;
        cap = len;
    }
    if (data_idx < perm_top || data_idx >= old_top) return;

    uint16_t stride = (elem_tid < 64u && mem->type_stride[elem_tid] > 0)
                      ? mem->type_stride[elem_tid] : 1u;
    uint32_t total = len * (uint32_t)stride;
    for (uint32_t s = 0; s < total && (data_idx + s) < old_top; ++s) {
        if (mem->gc_mark[data_idx + s] == 0u) {
            mem->gc_mark[data_idx + s] = 1u;
        }
    }
    if (elem_tid == 0xFEu) {
        for (uint32_t i = 0; i < len; ++i) {
            const double *p = mem_data_ptr_ro(mem, data_idx, i);
            if (p) {
                Value ev = 0;
                memcpy(&ev, p, sizeof(Value));
                gc_mark_val_rec(mem, ev, perm_top, old_top, (uint8_t)(depth + 1));
            }
        }
    } else if (elem_tid == mem->vec_type_id && elem_tid != 0) {
        for (uint32_t i = 0; i < len; ++i) {
            gc_mark_vec_hdr(mem, (uint16_t)(data_idx + i * stride), perm_top, old_top, (uint8_t)(depth + 1));
        }
    } else if (elem_tid == mem->fn_type_id && elem_tid != 0) {
        for (uint32_t i = 0; i < len; ++i) {
            Value fv = val_make_comp(mem, elem_tid, (uint16_t)(data_idx + i * stride), stride);
            gc_mark_val_rec(mem, fv, perm_top, old_top, (uint8_t)(depth + 1));
        }
    } else if (elem_tid > 0 && mem->sym_tab != NULL && elem_tid < mem->sym_tab->type_count &&
               !mem->sym_tab->types[elem_tid].is_consuming) {
        for (uint32_t i = 0; i < len; ++i) {
            Value sv = val_make_comp(mem, elem_tid, (uint16_t)(data_idx + i * stride), stride);
            gc_mark_val_rec(mem, sv, perm_top, old_top, (uint8_t)(depth + 1));
        }
    }
}

static void gc_mark_map_hdr(MemArena *mem, uint16_t hdr_idx, uint16_t perm_top, uint16_t old_top, uint8_t depth) {
    if (depth > 16 || (uint32_t)hdr_idx + MEM_MAP_HDR_SLOTS > old_top) return;
    if (hdr_idx >= perm_top) {
        if (mem->gc_mark[hdr_idx] == 3u) return;
        mem->gc_mark[hdr_idx] = 3u;
        for (uint16_t k = 1; k < MEM_MAP_HDR_SLOTS; ++k) {
            mem->gc_mark[hdr_idx + k] = 1u;
        }
    }
    uint16_t len      = (uint16_t)mem->f64_pool[hdr_idx + 1];
    uint16_t cap      = (uint16_t)mem->f64_pool[hdr_idx + 2];
    uint16_t data_idx = (uint16_t)mem->f64_pool[hdr_idx + 3];

    if (len == 0) {
        if (mem_is_heap_idx(data_idx)) {
            mem_heap_free_block(mem, data_idx);
        }
        mem->f64_pool[hdr_idx + 2] = 0.0;
        mem->f64_pool[hdr_idx + 3] = 0.0;
        return;
    }

    if (mem_is_heap_idx(data_idx)) {
        uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
        if (hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[hid].in_use) {
            mem->heap_blocks[hid].marked = 1u;
        }
        for (uint16_t i = 0; i < len; ++i) {
            Value ev = mem_map_entry_val(mem, data_idx, i);
            gc_mark_val_rec(mem, ev, perm_top, old_top, (uint8_t)(depth + 1));
        }
        return;
    }

    if (cap > len && data_idx >= perm_top) {
        mem->f64_pool[hdr_idx + 2] = (double)len;
        cap = len;
    }
    if (data_idx < perm_top || data_idx >= old_top) return;

    uint32_t total = (uint32_t)len * 2u;
    for (uint32_t s = 0; s < total && (data_idx + s) < old_top; ++s) {
        if (mem->gc_mark[data_idx + s] == 0u) {
            mem->gc_mark[data_idx + s] = 1u;
        }
    }
    for (uint16_t i = 0; i < len; ++i) {
        Value ev = mem_map_entry_val(mem, data_idx, i);
        gc_mark_val_rec(mem, ev, perm_top, old_top, (uint8_t)(depth + 1));
    }
}

static void gc_mark_mat_hdr(MemArena *mem, uint16_t hdr_idx, uint16_t perm_top, uint16_t old_top) {
    if ((uint32_t)hdr_idx + MEM_MAT_HDR_SLOTS > old_top) return;
    if (hdr_idx >= perm_top) {
        if (mem->gc_mark[hdr_idx] == 4u) return;
        mem->gc_mark[hdr_idx] = 4u;
        for (uint16_t k = 1; k < MEM_MAT_HDR_SLOTS; ++k) {
            mem->gc_mark[hdr_idx + k] = 1u;
        }
    }
    uint32_t tot_len  = (uint32_t)mem->f64_pool[hdr_idx + 1];
    uint16_t data_idx = (uint16_t)mem->f64_pool[hdr_idx + 2];
    if (mem_is_heap_idx(data_idx)) {
        uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
        if (hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[hid].in_use) {
            mem->heap_blocks[hid].marked = 1u;
        }
        return;
    }
    if (tot_len > 0 && data_idx >= perm_top && data_idx < old_top) {
        for (uint32_t s = 0; s < tot_len && (data_idx + s) < old_top; ++s) {
            if (mem->gc_mark[data_idx + s] == 0u) {
                mem->gc_mark[data_idx + s] = 1u;
            }
        }
    }
}

static void gc_mark_fn_hdr(MemArena *mem, uint16_t hdr_idx, uint16_t perm_top, uint16_t old_top, uint8_t depth) {
    if (depth > 16 || (uint32_t)hdr_idx + MEM_FN_HDR_SLOTS > old_top) return;
    if (hdr_idx >= perm_top) {
        if (mem->gc_mark[hdr_idx] == 5u) return;
        mem->gc_mark[hdr_idx] = 5u;
        for (uint16_t k = 1; k < MEM_FN_HDR_SLOTS; ++k) {
            mem->gc_mark[hdr_idx + k] = 1u;
        }
    }
    uint32_t packed_slt = (uint32_t)mem->f64_pool[hdr_idx + 2];
    uint8_t  slot_start = (uint8_t)(packed_slt & 0xFFu);
    uint32_t raw_f3     = (uint32_t)mem->f64_pool[hdr_idx + 3];
    uint16_t env_idx    = (uint16_t)(raw_f3 & 0xFFFFu);
    if (slot_start > 0 && mem_is_heap_idx(env_idx)) {
        uint16_t hid = (uint16_t)(env_idx & MEM_HEAP_ID_MASK);
        if (hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[hid].in_use) {
            if (mem->heap_blocks[hid].marked != 0u) return;
            mem->heap_blocks[hid].marked = 1u;
            HeapBlock *hb = &mem->heap_blocks[hid];
            for (uint8_t k = 0; k < slot_start && k < hb->cap_slots; ++k) {
                Value cv = 0;
                memcpy(&cv, &hb->data[k], sizeof(Value));
                gc_mark_val_rec(mem, cv, perm_top, old_top, (uint8_t)(depth + 1));
            }
        }
    }
}

static void gc_mark_struct_fields(MemArena *mem, uint16_t tid, uint16_t idx,
                                  uint16_t perm_top, uint16_t old_top, uint8_t depth) {
    if (depth > 16 || !mem->sym_tab || tid == 0 || tid >= mem->sym_tab->type_count) return;
    const TypeDef *td = &mem->sym_tab->types[tid];
    if (td->is_consuming || td->field_count == 0) return;
    for (uint8_t i = 0; i < td->field_count; ++i) {
        const FieldDef *fd = &mem->sym_tab->fields[td->start_idx + i];
        uint16_t ftid  = fd->type_id;
        uint16_t fpidx = (uint16_t)(idx + fd->flat_offset);
        if (ftid == mem->vec_type_id && ftid != 0) {
            gc_mark_vec_hdr(mem, fpidx, perm_top, old_top, (uint8_t)(depth + 1));
        } else if (ftid == mem->map_type_id && ftid != 0) {
            gc_mark_map_hdr(mem, fpidx, perm_top, old_top, (uint8_t)(depth + 1));
        } else if (ftid == mem->mat_type_id && ftid != 0) {
            gc_mark_mat_hdr(mem, fpidx, perm_top, old_top);
        } else if (ftid == mem->fn_type_id && ftid != 0) {
            gc_mark_fn_hdr(mem, fpidx, perm_top, old_top, (uint8_t)(depth + 1));
        } else if (ftid > 0 && ftid < mem->sym_tab->type_count &&
                   !mem->sym_tab->types[ftid].is_consuming) {
            gc_mark_struct_fields(mem, ftid, fpidx, perm_top, old_top, (uint8_t)(depth + 1));
        }
    }
}

static void gc_mark_val_rec(MemArena *mem, Value v, uint16_t perm_top, uint16_t old_top, uint8_t depth) {
    uint16_t top = (uint16_t)(v >> NANBOX_TAG_SHIFT);
    if (top != NB_TAG_POOL) return;
    uint16_t idx = val_get_pool_idx(v);
    if (idx >= old_top) return;

    uint16_t tid = val_get_type(mem, v);
    if (tid == mem->vec_type_id && tid != 0) {
        gc_mark_vec_hdr(mem, idx, perm_top, old_top, depth);
    } else if (tid == mem->map_type_id && tid != 0) {
        gc_mark_map_hdr(mem, idx, perm_top, old_top, depth);
    } else if (tid == mem->mat_type_id && tid != 0) {
        gc_mark_mat_hdr(mem, idx, perm_top, old_top);
    } else if (tid == mem->fn_type_id && tid != 0) {
        gc_mark_fn_hdr(mem, idx, perm_top, old_top, depth);
    } else {
        if (idx >= perm_top) {
            uint16_t fsz = (uint16_t)((v >> 16u) & 0xFFFFu);
            if (fsz == 0) fsz = 1u;
            for (uint16_t k = 0; k < fsz && (idx + k) < old_top; ++k) {
                if (mem->gc_mark[idx + k] == 0u) {
                    mem->gc_mark[idx + k] = 1u;
                }
            }
        }
        gc_mark_struct_fields(mem, tid, idx, perm_top, old_top, depth);
    }
}

static Value gc_remap_val(const MemArena *mem, Value v, uint16_t perm_top, uint16_t old_top) {
    uint16_t top = (uint16_t)(v >> NANBOX_TAG_SHIFT);
    if (top != NB_TAG_POOL) return v;
    uint16_t idx = val_get_pool_idx(v);
    if (idx < perm_top || idx >= old_top) return v;
    uint16_t new_idx = mem->gc_fwd[idx];
    if (new_idx == MEM_NO_IDX) return v;
    return (v & ~0xFFFFULL) | (uint64_t)new_idx;
}

uint16_t mem_compact_scratch(MemArena *mem, Value *extra_roots, uint16_t extra_root_count) {
    if (!mem) return 0;
    if (mem->pool_top <= mem->perm_pool_top && mem->heap_active_count == 0) return 0;
    uint16_t perm_top = mem->perm_pool_top;
    uint16_t old_top  = mem->pool_top;

    if (old_top > perm_top) {
        memset(&mem->gc_mark[perm_top], 0, (size_t)(old_top - perm_top));
    }
    for (uint16_t i = 0; i < MEM_MAX_HEAP_BLOCKS; ++i) {
        mem->heap_blocks[i].marked = 0u;
    }

    /* 1. Pin all scratch AST_LIT float slots in [perm_ast_count .. ast_count) */
    for (uint16_t i = mem->perm_ast_count; i < mem->ast_count; ++i) {
        const AstNode *n = &mem->ast[i];
        if (n->kind == 0 /* AST_LIT */ && n->sub != mem->str_type_id && n->sub != MEM_TYPE_VOID) {
            if (n->ref >= perm_top && n->ref < old_top) {
                mem->gc_mark[n->ref] = 1u;
            }
        }
    }

    /* 2. Mark all live Value roots (call stack, jump_val, temp_roots, module vars, extra_roots) */
    for (uint16_t i = 0; i < mem->sp; ++i) {
        gc_mark_val_rec(mem, mem->stack[i], perm_top, old_top, 0);
    }
    gc_mark_val_rec(mem, mem->jump_val, perm_top, old_top, 0);
    for (uint16_t i = 0; i < mem->temp_root_count; ++i) {
        if (mem->temp_roots[i]) {
            gc_mark_val_rec(mem, *(mem->temp_roots[i]), perm_top, old_top, 0);
        }
    }
    if (mem->sym_tab != NULL) {
        const ModVarDef *mvars = sym_vars(mem->sym_tab);
        for (uint16_t i = 0; i < mem->sym_tab->var_count; ++i) {
            gc_mark_val_rec(mem, mvars[i].val, perm_top, old_top, 0);
        }
    }
    for (uint16_t i = 0; i < extra_root_count; ++i) {
        if (extra_roots) {
            gc_mark_val_rec(mem, extra_roots[i], perm_top, old_top, 0);
        }
    }

    /* 2b. Sweep unreachable HeapBlocks immediately */
    for (uint16_t i = 0; i < MEM_MAX_HEAP_BLOCKS; ++i) {
        if (mem->heap_blocks[i].in_use && !mem->heap_blocks[i].marked) {
            mem_heap_free_block(mem, (uint16_t)(MEM_HEAP_IDX_BIT | i));
        }
    }

    if (old_top <= perm_top) return 0;

    /* 3. Compute sliding destination addresses in gc_fwd */
    uint16_t write_pos = perm_top;
    for (uint16_t i = perm_top; i < old_top; ++i) {
        if (mem->gc_mark[i] != 0u) {
            mem->gc_fwd[i] = write_pos++;
        } else {
            mem->gc_fwd[i] = MEM_NO_IDX;
        }
    }
    if (write_pos == old_top) {
        return 0; /* Nothing to slide in f64_pool */
    }

    /* 4. Fix up internal container data_idx pointers & map entry Values BEFORE sliding */
    for (uint16_t i = perm_top; i < old_top; ++i) {
        uint8_t m = mem->gc_mark[i];
        if (m == 2u) { /* $vec header */
            uint16_t raw0     = (uint16_t)mem->f64_pool[i + 0];
            uint16_t elem_tid = (uint16_t)(raw0 & 0xFFu);
            uint32_t vlen     = (uint32_t)mem->f64_pool[i + 1];
            uint16_t d_idx    = (uint16_t)mem->f64_pool[i + 3];
            if (elem_tid == 0xFEu && vlen > 0) {
                for (uint32_t e = 0; e < vlen; ++e) {
                    double *p = mem_data_ptr(mem, d_idx, e);
                    if (p) {
                        Value ev = 0;
                        memcpy(&ev, p, sizeof(Value));
                        ev = gc_remap_val(mem, ev, perm_top, old_top);
                        memcpy(p, &ev, sizeof(Value));
                    }
                }
            }
            if (mem_is_heap_idx(d_idx)) {
                uint16_t hid = (uint16_t)(d_idx & MEM_HEAP_ID_MASK);
                if (hid < MEM_MAX_HEAP_BLOCKS && mem->gc_fwd[i] != MEM_NO_IDX) {
                    mem->heap_blocks[hid].owner_hdr = mem->gc_fwd[i];
                }
            } else if (d_idx >= perm_top && d_idx < old_top && mem->gc_fwd[d_idx] != MEM_NO_IDX) {
                mem->f64_pool[i + 3] = (double)mem->gc_fwd[d_idx];
            }
        } else if (m == 3u) { /* $map header */
            uint16_t len   = (uint16_t)mem->f64_pool[i + 1];
            uint16_t d_idx = (uint16_t)mem->f64_pool[i + 3];
            if (mem_is_heap_idx(d_idx)) {
                for (uint16_t e = 0; e < len; ++e) {
                    Value    ev = mem_map_entry_val(mem, d_idx, e);
                    StrSlice ek = mem_map_entry_key(mem, d_idx, e);
                    ev = gc_remap_val(mem, ev, perm_top, old_top);
                    mem_map_set_entry(mem, d_idx, e, ek, ev);
                }
                uint16_t hid = (uint16_t)(d_idx & MEM_HEAP_ID_MASK);
                if (hid < MEM_MAX_HEAP_BLOCKS && mem->gc_fwd[i] != MEM_NO_IDX) {
                    mem->heap_blocks[hid].owner_hdr = mem->gc_fwd[i];
                }
            } else if (d_idx >= perm_top && d_idx < old_top) {
                for (uint16_t e = 0; e < len; ++e) {
                    Value    ev = mem_map_entry_val(mem, d_idx, e);
                    StrSlice ek = mem_map_entry_key(mem, d_idx, e);
                    ev = gc_remap_val(mem, ev, perm_top, old_top);
                    mem_map_set_entry(mem, d_idx, e, ek, ev);
                }
                if (mem->gc_fwd[d_idx] != MEM_NO_IDX) {
                    mem->f64_pool[i + 3] = (double)mem->gc_fwd[d_idx];
                }
            }
        } else if (m == 4u) { /* $mat header */
            uint16_t d_idx = (uint16_t)mem->f64_pool[i + 2];
            if (mem_is_heap_idx(d_idx)) {
                uint16_t hid = (uint16_t)(d_idx & MEM_HEAP_ID_MASK);
                if (hid < MEM_MAX_HEAP_BLOCKS && mem->gc_fwd[i] != MEM_NO_IDX) {
                    mem->heap_blocks[hid].owner_hdr = mem->gc_fwd[i];
                }
            } else if (d_idx >= perm_top && d_idx < old_top && mem->gc_fwd[d_idx] != MEM_NO_IDX) {
                mem->f64_pool[i + 2] = (double)mem->gc_fwd[d_idx];
            }
        } else if (m == 5u) { /* $fn header */
            uint32_t raw_f3  = (uint32_t)mem->f64_pool[i + 3];
            uint16_t env_idx = (uint16_t)(raw_f3 & 0xFFFFu);
            if (mem_is_heap_idx(env_idx)) {
                uint16_t hid = (uint16_t)(env_idx & MEM_HEAP_ID_MASK);
                if (hid < MEM_MAX_HEAP_BLOCKS && mem->gc_fwd[i] != MEM_NO_IDX) {
                    mem->heap_blocks[hid].owner_hdr = mem->gc_fwd[i];
                }
            }
        }
    }
    /* Remap Values inside all active closure environment HeapBlocks */
    for (uint16_t hid = 0; hid < MEM_MAX_HEAP_BLOCKS; ++hid) {
        HeapBlock *hb = &mem->heap_blocks[hid];
        if (hb->in_use >= 2u && hb->marked && hb->data != NULL && hb->cap_slots >= 3u) {
            uint32_t meta    = (uint32_t)hb->data[hb->cap_slots - 1u];
            uint8_t  s_start = (uint8_t)(meta & 0xFFu);
            for (uint8_t k = 0; k < s_start && k < hb->cap_slots; ++k) {
                Value cv = 0;
                memcpy(&cv, &hb->data[k], sizeof(Value));
                cv = gc_remap_val(mem, cv, perm_top, old_top);
                memcpy(&hb->data[k], &cv, sizeof(Value));
            }
        }
    }

    /* 5. Slide all live f64_pool slots down contiguously */
    for (uint16_t i = perm_top; i < old_top; ++i) {
        if (mem->gc_mark[i] != 0u) {
            uint16_t dst = mem->gc_fwd[i];
            if (dst != i) {
                memcpy(&mem->f64_pool[dst], &mem->f64_pool[i], sizeof(double));
            }
        }
    }
    mem->pool_top = write_pos;

    /* 6. Remap AST_LIT refs, vrange_hdr, stack, jump_val, temp_roots, module vars, and extra_roots */
    for (uint16_t i = mem->perm_ast_count; i < mem->ast_count; ++i) {
        AstNode *n = &mem->ast[i];
        if (n->kind == 0 /* AST_LIT */ && n->sub != mem->str_type_id && n->sub != MEM_TYPE_VOID) {
            if (n->ref >= perm_top && n->ref < old_top && mem->gc_fwd[n->ref] != MEM_NO_IDX) {
                n->ref = mem->gc_fwd[n->ref];
            }
        }
    }
    for (uint8_t k = 0; k < 4; ++k) {
        uint16_t vh = mem->vrange_hdr[k];
        if (vh >= perm_top && vh < old_top) {
            mem->vrange_hdr[k] = (mem->gc_fwd[vh] != MEM_NO_IDX) ? mem->gc_fwd[vh] : 0;
        }
    }
    for (uint16_t i = 0; i < mem->sp; ++i) {
        mem->stack[i] = gc_remap_val(mem, mem->stack[i], perm_top, old_top);
    }
    mem->jump_val = gc_remap_val(mem, mem->jump_val, perm_top, old_top);
    for (uint16_t i = 0; i < mem->temp_root_count; ++i) {
        if (mem->temp_roots[i]) {
            *(mem->temp_roots[i]) = gc_remap_val(mem, *(mem->temp_roots[i]), perm_top, old_top);
        }
    }
    if (mem->sym_tab != NULL) {
        ModVarDef *mvars = sym_vars_mut(mem->sym_tab);
        for (uint16_t i = 0; i < mem->sym_tab->var_count; ++i) {
            mvars[i].val = gc_remap_val(mem, mvars[i].val, perm_top, old_top);
        }
    }
    for (uint16_t i = 0; i < extra_root_count; ++i) {
        if (extra_roots) {
            extra_roots[i] = gc_remap_val(mem, extra_roots[i], perm_top, old_top);
        }
    }

    return (uint16_t)(old_top - write_pos);
}


/* ============================================================================
 * 6. SCRATCH STRING ARENA SLIDING COMPACTION (`mem_compact_strings`)
 * ============================================================================ */

typedef struct {
    uint16_t start;
    uint16_t end;
    uint16_t new_start;
} StrSpan;

typedef struct {
    StrSpan  spans[512];
    uint16_t count;
    uint16_t perm_len;
    uint16_t old_len;
    bool     overflow;
} StrGcCtx;

static void str_gc_add_slice(StrGcCtx *ctx, StrSlice s) {
    if (ctx->overflow || s.len == 0) return;
    if (s.off < ctx->perm_len || (uint32_t)s.off + (uint32_t)s.len > (uint32_t)ctx->old_len) return;
    uint16_t st = s.off;
    uint16_t en = (uint16_t)(s.off + s.len);
    for (uint16_t i = 0; i < ctx->count; ++i) {
        if (ctx->spans[i].start == st && ctx->spans[i].end == en) return;
    }
    if (ctx->count >= 512u) {
        ctx->overflow = true;
        return;
    }
    ctx->spans[ctx->count].start     = st;
    ctx->spans[ctx->count].end       = en;
    ctx->spans[ctx->count].new_start = st;
    ctx->count++;
}

static StrSlice str_gc_remap_slice(const StrGcCtx *ctx, StrSlice s) {
    if (s.len == 0 || s.off < ctx->perm_len || (uint32_t)s.off + (uint32_t)s.len > (uint32_t)ctx->old_len) {
        return s;
    }
    for (uint16_t i = 0; i < ctx->count; ++i) {
        if (s.off >= ctx->spans[i].start && (uint16_t)(s.off + s.len) <= ctx->spans[i].end) {
            StrSlice out = { (uint16_t)(ctx->spans[i].new_start + (s.off - ctx->spans[i].start)), s.len };
            return out;
        }
    }
    return s;
}

static void str_gc_visit_struct(MemArena *mem, StrGcCtx *ctx, uint16_t tid, uint16_t pidx,
                                bool remap, uint8_t depth);

static void str_gc_visit_val(MemArena *mem, StrGcCtx *ctx, Value *vptr, bool remap, uint8_t depth) {
    if (!vptr || depth > 16) return;
    Value v = *vptr;
    uint16_t tid = val_get_type(mem, v);
    if (tid == 0) return;
    if (tid == mem->str_type_id && mem->str_type_id != 0) {
        StrSlice s = val_get_str(mem, v);
        if (!remap) {
            str_gc_add_slice(ctx, s);
        } else {
            StrSlice ns = str_gc_remap_slice(ctx, s);
            *vptr = val_with_unit(mem, val_make_str(mem, tid, ns), val_get_unit(mem, v));
        }
        return;
    }
    uint16_t top = (uint16_t)(v >> NANBOX_TAG_SHIFT);
    if (top != NB_TAG_POOL) return;
    uint16_t idx = val_get_pool_idx(v);
    if (idx >= mem->pool_top) return;

    if (tid == mem->vec_type_id && mem->vec_type_id != 0) {
        uint16_t raw0     = (uint16_t)mem->f64_pool[idx + 0];
        uint16_t elem_tid = (uint16_t)(raw0 & 0xFFu);
        uint32_t len      = (uint32_t)mem->f64_pool[idx + 1];
        uint16_t data_idx = (uint16_t)mem->f64_pool[idx + 3];
        if (len == 0) return;
        if (elem_tid == 0xFEu) {
            for (uint32_t i = 0; i < len; ++i) {
                double *p = mem_data_ptr(mem, data_idx, i);
                if (p) {
                    Value ev = 0;
                    memcpy(&ev, p, sizeof(Value));
                    str_gc_visit_val(mem, ctx, &ev, remap, (uint8_t)(depth + 1));
                    if (remap) {
                        memcpy(p, &ev, sizeof(Value));
                    }
                }
            }
        } else if (elem_tid == mem->str_type_id && mem->str_type_id != 0) {
            for (uint32_t i = 0; i < len; ++i) {
                uint32_t packed = (uint32_t)mem_data_read(mem, data_idx, i);
                StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
                if (!remap) {
                    str_gc_add_slice(ctx, s);
                } else {
                    StrSlice ns = str_gc_remap_slice(ctx, s);
                    uint32_t np = ((uint32_t)ns.off << 16u) | (uint32_t)ns.len;
                    mem_data_write(mem, data_idx, i, (double)np);
                }
            }
        } else if (elem_tid > 0 && mem->sym_tab != NULL && elem_tid < mem->sym_tab->type_count &&
                   !mem->sym_tab->types[elem_tid].is_consuming && !mem_is_heap_idx(data_idx)) {
            uint16_t stride = mem->sym_tab->types[elem_tid].flat_size;
            for (uint32_t i = 0; i < len; ++i) {
                str_gc_visit_struct(mem, ctx, elem_tid, (uint16_t)(data_idx + i * stride), remap, (uint8_t)(depth + 1));
            }
        }
    } else if (tid == mem->map_type_id && mem->map_type_id != 0) {
        uint16_t len      = (uint16_t)mem->f64_pool[idx + 1];
        uint16_t data_idx = (uint16_t)mem->f64_pool[idx + 3];
        for (uint16_t i = 0; i < len; ++i) {
            StrSlice ks = mem_map_entry_key(mem, data_idx, i);
            Value    ev = mem_map_entry_val(mem, data_idx, i);
            if (!remap) {
                str_gc_add_slice(ctx, ks);
                str_gc_visit_val(mem, ctx, &ev, false, (uint8_t)(depth + 1));
            } else {
                StrSlice nks = str_gc_remap_slice(ctx, ks);
                str_gc_visit_val(mem, ctx, &ev, true, (uint8_t)(depth + 1));
                mem_map_set_entry(mem, data_idx, i, nks, ev);
            }
        }
    } else if (mem->sym_tab != NULL && tid < mem->sym_tab->type_count &&
               !mem->sym_tab->types[tid].is_consuming) {
        str_gc_visit_struct(mem, ctx, tid, idx, remap, (uint8_t)(depth + 1));
    }
}

static void str_gc_visit_struct(MemArena *mem, StrGcCtx *ctx, uint16_t tid, uint16_t pidx,
                                bool remap, uint8_t depth) {
    if (depth > 16 || !mem->sym_tab || tid == 0 || tid >= mem->sym_tab->type_count) return;
    if (tid == mem->vec_type_id || tid == mem->map_type_id) {
        Value cv = val_make_comp(mem, tid, pidx, 4);
        str_gc_visit_val(mem, ctx, &cv, remap, depth);
        return;
    }
    const TypeDef *td = &mem->sym_tab->types[tid];
    if (td->is_consuming || td->field_count == 0) return;
    for (uint8_t i = 0; i < td->field_count; ++i) {
        const FieldDef *fd = &mem->sym_tab->fields[td->start_idx + i];
        uint16_t ftid  = fd->type_id;
        uint16_t fpidx = (uint16_t)(pidx + fd->flat_offset);
        if (fpidx >= mem->pool_top) continue;
        if (ftid == mem->str_type_id && mem->str_type_id != 0) {
            uint32_t packed = (uint32_t)mem->f64_pool[fpidx];
            StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
            if (!remap) {
                str_gc_add_slice(ctx, s);
            } else {
                StrSlice ns = str_gc_remap_slice(ctx, s);
                mem->f64_pool[fpidx] = (double)(((uint32_t)ns.off << 16u) | (uint32_t)ns.len);
            }
        } else if (ftid > 0 && ftid < mem->sym_tab->type_count &&
                   !mem->sym_tab->types[ftid].is_consuming) {
            str_gc_visit_struct(mem, ctx, ftid, fpidx, remap, (uint8_t)(depth + 1));
        }
    }
}

static void str_gc_walk_all(MemArena *mem, StrGcCtx *ctx, bool remap) {
    for (uint8_t i = 0; i < mem->fn_src_count; ++i) {
        if (!remap) str_gc_add_slice(ctx, mem->fn_src_slice[i]);
        else        mem->fn_src_slice[i] = str_gc_remap_slice(ctx, mem->fn_src_slice[i]);
    }
    for (uint8_t i = 0; i < 64u; ++i) {
        if (!remap) str_gc_add_slice(ctx, mem->local_slot_names[i]);
        else        mem->local_slot_names[i] = str_gc_remap_slice(ctx, mem->local_slot_names[i]);
    }
    for (uint16_t i = mem->perm_ast_count; i < mem->ast_count; ++i) {
        AstNode *n = &mem->ast[i];
        if (n->kind == 0 /* AST_LIT */ && n->sub == mem->str_type_id && mem->str_type_id != 0) {
            StrSlice s = { n->left, n->right };
            if (!remap) {
                str_gc_add_slice(ctx, s);
            } else {
                StrSlice ns = str_gc_remap_slice(ctx, s);
                n->left  = ns.off;
                n->right = ns.len;
            }
        }
    }
    for (uint16_t i = 0; i < mem->sp; ++i) {
        str_gc_visit_val(mem, ctx, &mem->stack[i], remap, 0);
    }
    str_gc_visit_val(mem, ctx, &mem->jump_val, remap, 0);
    for (uint16_t i = 0; i < mem->temp_root_count; ++i) {
        if (mem->temp_roots[i]) {
            str_gc_visit_val(mem, ctx, mem->temp_roots[i], remap, 0);
        }
    }
    if (mem->sym_tab != NULL) {
        SymTable *tab = mem->sym_tab;
        for (uint16_t i = 0; i < tab->sym_count; ++i) {
            if (!remap) str_gc_add_slice(ctx, tab->syms[i]);
            else        tab->syms[i] = str_gc_remap_slice(ctx, tab->syms[i]);
        }
        ModVarDef *mvars = sym_vars_mut(tab);
        for (uint16_t i = 0; i < tab->var_count; ++i) {
            str_gc_visit_val(mem, ctx, &mvars[i].val, remap, 0);
        }
    }
    for (uint16_t hid = 0; hid < MEM_MAX_HEAP_BLOCKS; ++hid) {
        HeapBlock *hb = &mem->heap_blocks[hid];
        if (hb->in_use >= 2u && hb->data != NULL && hb->cap_slots >= 3u) {
            uint32_t meta    = (uint32_t)hb->data[hb->cap_slots - 1u];
            uint8_t  s_start = (uint8_t)(meta & 0xFFu);
            for (uint8_t k = 0; k < s_start && k < hb->cap_slots; ++k) {
                Value cv = 0;
                memcpy(&cv, &hb->data[k], sizeof(Value));
                str_gc_visit_val(mem, ctx, &cv, remap, 0);
                if (remap) {
                    memcpy(&hb->data[k], &cv, sizeof(Value));
                }
            }
        }
    }
}

void mem_compact_strings(MemArena *mem) {
    if (!mem || mem->src_len <= mem->perm_src_len) return;
    StrGcCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.perm_len = mem->perm_src_len;
    ctx.old_len  = mem->src_len;

    str_gc_walk_all(mem, &ctx, false);
    if (ctx.overflow) return;
    if (ctx.count == 0) {
        mem->src_len = mem->perm_src_len;
        return;
    }

    /* Sort spans by start ascending */
    for (uint16_t i = 1; i < ctx.count; ++i) {
        StrSpan key = ctx.spans[i];
        int j = (int)i - 1;
        while (j >= 0 && (ctx.spans[j].start > key.start ||
               (ctx.spans[j].start == key.start && ctx.spans[j].end < key.end))) {
            ctx.spans[j + 1] = ctx.spans[j];
            j--;
        }
        ctx.spans[j + 1] = key;
    }

    /* Merge overlapping or contiguous spans */
    uint16_t mcnt = 0;
    for (uint16_t i = 0; i < ctx.count; ++i) {
        if (mcnt == 0 || ctx.spans[i].start > ctx.spans[mcnt - 1].end) {
            ctx.spans[mcnt++] = ctx.spans[i];
        } else if (ctx.spans[i].end > ctx.spans[mcnt - 1].end) {
            ctx.spans[mcnt - 1].end = ctx.spans[i].end;
        }
    }
    ctx.count = mcnt;

    /* Slide merged spans down to perm_src_len */
    uint16_t write_pos = mem->perm_src_len;
    for (uint16_t i = 0; i < ctx.count; ++i) {
        uint16_t len = (uint16_t)(ctx.spans[i].end - ctx.spans[i].start);
        ctx.spans[i].new_start = write_pos;
        if (write_pos != ctx.spans[i].start) {
            memmove(mem->src_buf + write_pos, mem->src_buf + ctx.spans[i].start, len);
        }
        write_pos = (uint16_t)(write_pos + len);
    }

    str_gc_walk_all(mem, &ctx, true);
    mem->src_len = write_pos;
}
