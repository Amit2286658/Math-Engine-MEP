/*
 * mem_containers.c — Dynamic Vector, Matrix & Map SBO/Heap Containers and NaN-Boxed Value API
 */
#include "mem_internal.h"

static bool mem_should_use_sbo(const MemArena *mem, uint32_t total_slots, bool is_non_consuming_struct) {
    if (is_non_consuming_struct) {
        /* Struct elements (`vec<vec2>`) require pool indices for direct field view references */
        return ((uint32_t)mem->pool_top + total_slots <= (uint32_t)MEM_MAX_F64_POOL);
    }
    if (total_slots > MEM_SBO_MAX_SLOTS) {
        return false;
    }
    /* Keep at least 512 slots of headroom in `f64_pool` for scalar/header allocations */
    if ((uint32_t)mem->pool_top + total_slots + 512u > (uint32_t)MEM_MAX_F64_POOL) {
        return false;
    }
    return true;
}

/*
 * Dynamic Vector 4-Slot Header Layout in `f64_pool[hdr_idx .. hdr_idx+3]`:
 *   [0] = elem_type_id (0 = untyped/auto-inferred on first push, >0 = type ID T; bit 15 = MEM_HDR_IMMUTABLE_BIT)
 *   [1] = len          (current number of active elements T)
 *   [2] = cap          (allocated capacity in elements T)
 *   [3] = data_idx     (start index in `f64_pool` OR `MEM_HEAP_IDX_BIT | heap_id`, or 0)
 */
uint16_t mem_vec_alloc_hdr(MemArena *mem, uint16_t elem_tid, uint32_t init_cap) {
    if (!mem) return MEM_NO_IDX;
    if (init_cap > MEM_MAX_HEAP_SLOTS) {
        mem_err(mem, "Vector capacity (%u) exceeds maximum limit", (unsigned)init_cap);
        return MEM_NO_IDX;
    }
    uint16_t hdr_idx = mem_alloc_pool(mem, MEM_VEC_HDR_SLOTS);
    if (hdr_idx == MEM_NO_IDX || mem->has_error) return MEM_NO_IDX;
    mem_write_pool(mem, (uint16_t)(hdr_idx + 0), (double)elem_tid);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 1), 0.0);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)init_cap);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 3), 0.0);
    return hdr_idx;
}

bool mem_vec_reserve(MemArena *mem, uint16_t hdr_idx, uint16_t stride, uint32_t min_cap) {
    if (!mem || hdr_idx == MEM_NO_IDX || (uint32_t)hdr_idx + MEM_VEC_HDR_SLOTS > MEM_MAX_F64_POOL) {
        if (mem) mem_err(mem, "Invalid vector header index");
        return false;
    }
    if (stride == 0) stride = 1;
    uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 0));
    uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
    bool is_struct_elem = (stride > 1u) || (elem_tid == mem->vec_type_id && elem_tid != 0);

    uint32_t len      = (uint32_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 1));
    uint32_t cap      = (uint32_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 2));
    uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));

    if (data_idx == 0) {
        uint32_t target_cap = (cap >= min_cap) ? cap : min_cap;
        if (target_cap < 4u) target_cap = 4u;
        uint64_t total_slots64 = (uint64_t)target_cap * (uint64_t)stride;
        if (total_slots64 > (uint64_t)MEM_MAX_HEAP_SLOTS) {
            mem_err(mem, "Vector capacity exceeds maximum limit");
            return false;
        }
        uint32_t total_slots = (uint32_t)total_slots64;
        if (mem_should_use_sbo(mem, total_slots, is_struct_elem)) {
            uint16_t new_data = mem_alloc_pool(mem, (uint16_t)total_slots);
            if (new_data == MEM_NO_IDX || mem->has_error) return false;
            memset(&mem->f64_pool[new_data], 0, (size_t)total_slots * sizeof(double));
            mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)target_cap);
            mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)new_data);
            return true;
        } else if (!is_struct_elem) {
            uint16_t heap_idx = mem_heap_alloc_block(mem, hdr_idx, total_slots);
            if (heap_idx == MEM_NO_IDX || mem->has_error) return false;
            mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)target_cap);
            mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)heap_idx);
            return true;
        } else {
            mem_err(mem, "Vector capacity exceeds pool limit");
            return false;
        }
    }

    if (cap >= min_cap) return true;

    uint32_t new_cap = (cap < 4u) ? 4u : (cap * 2u);
    if (new_cap < min_cap) new_cap = min_cap;
    uint64_t new_slots64 = (uint64_t)new_cap * (uint64_t)stride;
    if (new_slots64 > (uint64_t)MEM_MAX_HEAP_SLOTS) {
        mem_err(mem, "Vector capacity exceeds maximum limit");
        return false;
    }
    uint32_t new_slots  = (uint32_t)new_slots64;
    uint32_t copy_slots = len * (uint32_t)stride;

    /* Case A: Already heap-backed -> grow in-place via `realloc` */
    if (mem_is_heap_idx(data_idx)) {
        if (!mem_heap_realloc_block(mem, data_idx, new_slots)) return false;
        mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
        return true;
    }

    /* Case B: Currently in SBO `f64_pool`, and still fits within SBO threshold */
    if (mem_should_use_sbo(mem, new_slots, is_struct_elem)) {
        uint16_t extra_slots = (uint16_t)((new_cap - cap) * stride);
        if ((uint32_t)data_idx + cap * (uint32_t)stride == (uint32_t)mem->pool_top &&
            (uint32_t)mem->pool_top + extra_slots <= (uint32_t)MEM_MAX_F64_POOL) {
            uint16_t ext_idx = mem_alloc_pool(mem, extra_slots);
            if (ext_idx == MEM_NO_IDX || mem->has_error) return false;
            memset(&mem->f64_pool[ext_idx], 0, (size_t)extra_slots * sizeof(double));
            mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
            return true;
        }
        uint16_t new_data = mem_alloc_pool(mem, (uint16_t)new_slots);
        if (new_data == MEM_NO_IDX || mem->has_error) return false;
        if (copy_slots > 0) {
            memcpy(&mem->f64_pool[new_data], &mem->f64_pool[data_idx],
                   (size_t)copy_slots * sizeof(double));
        }
        if (new_slots > copy_slots) {
            memset(&mem->f64_pool[new_data + copy_slots], 0,
                   (size_t)(new_slots - copy_slots) * sizeof(double));
        }
        mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
        mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)new_data);
        return true;
    }

    /* Case C: SBO -> Heap Spill! Migrate payload from `f64_pool` to a new `HeapBlock` */
    if (is_struct_elem) {
        mem_err(mem, "Struct vector capacity exceeds pool limit");
        return false;
    }
    uint16_t heap_idx = mem_heap_alloc_block(mem, hdr_idx, new_slots);
    if (heap_idx == MEM_NO_IDX || mem->has_error) return false;
    double *dst = mem_data_ptr(mem, heap_idx, 0);
    if (copy_slots > 0 && dst != NULL) {
        memcpy(dst, &mem->f64_pool[data_idx], (size_t)copy_slots * sizeof(double));
    }
    mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)heap_idx);
    return true;
}

void mem_vec_shrink_to_fit(MemArena *mem, uint16_t hdr_idx, uint16_t stride) {
    if (!mem || hdr_idx == MEM_NO_IDX || (uint32_t)hdr_idx + MEM_VEC_HDR_SLOTS > MEM_MAX_F64_POOL) {
        return;
    }
    if (stride == 0) stride = 1;
    uint32_t len      = (uint32_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 1));
    uint32_t cap      = (uint32_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 2));
    uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));

    if (cap <= len) return;
    if (mem_is_heap_idx(data_idx)) {
        if (len == 0) {
            mem_heap_free_block(mem, data_idx);
            mem_write_pool(mem, (uint16_t)(hdr_idx + 3), 0.0);
        } else {
            uint16_t hid = (uint16_t)(data_idx & MEM_HEAP_ID_MASK);
            uint32_t new_slots = len * (uint32_t)stride;
            if (hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[hid].in_use &&
                new_slots < mem->heap_blocks[hid].cap_slots) {
                double *shrunk = (double *)realloc(mem->heap_blocks[hid].data,
                                                   (size_t)new_slots * sizeof(double));
                if (shrunk) {
                    mem->heap_total_slots -= (mem->heap_blocks[hid].cap_slots - new_slots);
                    mem->heap_blocks[hid].data      = shrunk;
                    mem->heap_blocks[hid].cap_slots = new_slots;
                }
            }
        }
        mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)len);
        return;
    }
    if (data_idx != 0 && (uint32_t)data_idx + cap * (uint32_t)stride == (uint32_t)mem->pool_top) {
        mem->pool_top = (uint16_t)(data_idx + len * stride);
    }
    if (len == 0) {
        mem_write_pool(mem, (uint16_t)(hdr_idx + 3), 0.0);
    }
    mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)len);
}

/*
 * Multidimensional Matrix (`$mat`) 8-Slot Header Layout in `f64_pool[hdr_idx .. hdr_idx+7]`:
 *   [0] = rank       (number of dimensions: 1, 2, 3, or 4)
 *   [1] = total_len  (total scalar elements = d0 * d1 * ... * d_{rank-1})
 *   [2] = data_idx   (start index in `f64_pool` OR `MEM_HEAP_IDX_BIT | heap_id` of `total_len` doubles)
 *   [3] = elem_tid   (element type ID, default fast_real_type; bit 15 = MEM_HDR_IMMUTABLE_BIT)
 *   [4] = d0         (dimension 0 size, e.g. rows)
 *   [5] = d1         (dimension 1 size, e.g. cols)
 *   [6] = d2         (dimension 2 size)
 *   [7] = d3         (dimension 3 size)
 */
uint16_t mem_mat_alloc_hdr(MemArena *mem, uint8_t rank, const uint16_t *dims,
                           uint16_t *out_total_len, uint16_t *out_data_idx) {
    if (rank == 0 || rank > 4) {
        mem_err(mem, "Matrix rank must be between 1 and 4 (got %u)", (unsigned)rank);
        return MEM_NO_IDX;
    }
    uint64_t total64 = 1u;
    for (uint8_t i = 0; i < rank; ++i) {
        total64 *= (uint64_t)dims[i];
        if (total64 > 65534ULL || total64 > (uint64_t)MEM_MAX_HEAP_SLOTS) {
            mem_err(mem, "Matrix dimensions exceed capacity");
            return MEM_NO_IDX;
        }
    }
    uint32_t total = (uint32_t)total64;
    uint16_t hdr_idx = mem_alloc_pool(mem, MEM_MAT_HDR_SLOTS);
    if (hdr_idx == MEM_NO_IDX || mem->has_error) return MEM_NO_IDX;

    uint16_t data_idx = 0;
    if (total > 0) {
        if (mem_should_use_sbo(mem, total, false)) {
            data_idx = mem_alloc_pool(mem, (uint16_t)total);
            if (data_idx == MEM_NO_IDX || mem->has_error) return MEM_NO_IDX;
            memset(&mem->f64_pool[data_idx], 0, (size_t)total * sizeof(double));
        } else {
            data_idx = mem_heap_alloc_block(mem, hdr_idx, total);
            if (data_idx == MEM_NO_IDX || mem->has_error) return MEM_NO_IDX;
        }
    }
    mem_write_pool(mem, (uint16_t)(hdr_idx + 0), (double)rank);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 1), (double)total);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)data_idx);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)mem->fast_real_type);
    for (uint8_t i = 0; i < 4; ++i) {
        mem_write_pool(mem, (uint16_t)(hdr_idx + 4u + i), (i < rank) ? (double)dims[i] : 0.0);
    }
    if (out_total_len) *out_total_len = (uint16_t)total;
    if (out_data_idx)  *out_data_idx  = data_idx;
    return hdr_idx;
}

/*
 * Dynamic Map (`$map`) 4-Slot Header Layout in `f64_pool[hdr_idx .. hdr_idx+3]`:
 *   [0] = packed_val_type (low 8 bits = locked val type ID or 0, bits 8..14 = locked unit ID, bit 15 = MEM_HDR_IMMUTABLE_BIT)
 *   [1] = len             (number of active key-value pairs)
 *   [2] = cap             (allocated entry capacity)
 *   [3] = data_idx        (start index in `f64_pool` OR `MEM_HEAP_IDX_BIT | heap_id` of `cap * 2` doubles:
 *                          [2*i + 0] = packed key StrSlice, [2*i + 1] = raw 64-bit Value bits)
 */
uint16_t mem_map_alloc_hdr(MemArena *mem, uint16_t packed_val_type, uint16_t init_cap) {
    uint16_t hdr_idx = mem_alloc_pool(mem, MEM_MAP_HDR_SLOTS);
    if (hdr_idx == MEM_NO_IDX || mem->has_error) return MEM_NO_IDX;
    mem_write_pool(mem, (uint16_t)(hdr_idx + 0), (double)packed_val_type);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 1), 0.0);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)init_cap);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 3), 0.0);
    return hdr_idx;
}

bool mem_map_reserve(MemArena *mem, uint16_t hdr_idx, uint16_t min_cap) {
    if (!mem || hdr_idx == MEM_NO_IDX || (uint32_t)hdr_idx + MEM_MAP_HDR_SLOTS > MEM_MAX_F64_POOL) {
        if (mem) mem_err(mem, "Invalid map header index");
        return false;
    }
    uint16_t len      = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 1));
    uint16_t cap      = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 2));
    uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));

    if (data_idx == 0) {
        uint16_t target_cap = (cap >= min_cap) ? cap : min_cap;
        if (target_cap < 4) target_cap = 4;
        uint32_t total_slots = (uint32_t)target_cap * 2u;
        if (total_slots > MEM_MAX_HEAP_SLOTS) {
            mem_err(mem, "Map capacity exceeds limit");
            return false;
        }
        if (mem_should_use_sbo(mem, total_slots, false)) {
            uint16_t new_data = mem_alloc_pool(mem, (uint16_t)total_slots);
            if (new_data == MEM_NO_IDX || mem->has_error) return false;
            memset(&mem->f64_pool[new_data], 0, (size_t)total_slots * sizeof(double));
            mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)target_cap);
            mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)new_data);
            return true;
        } else {
            uint16_t heap_idx = mem_heap_alloc_block(mem, hdr_idx, total_slots);
            if (heap_idx == MEM_NO_IDX || mem->has_error) return false;
            mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)target_cap);
            mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)heap_idx);
            return true;
        }
    }

    if (cap >= min_cap) return true;

    uint32_t new_cap32 = (cap < 4) ? 4u : ((uint32_t)cap * 2u);
    if (new_cap32 < (uint32_t)min_cap) new_cap32 = (uint32_t)min_cap;
    if (new_cap32 > 32767u || new_cap32 * 2u > MEM_MAX_HEAP_SLOTS) {
        mem_err(mem, "Map capacity exceeds limit");
        return false;
    }
    uint16_t new_cap    = (uint16_t)new_cap32;
    uint32_t new_slots  = (uint32_t)new_cap * 2u;
    uint32_t copy_slots = (uint32_t)len * 2u;

    if (mem_is_heap_idx(data_idx)) {
        if (!mem_heap_realloc_block(mem, data_idx, new_slots)) return false;
        mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
        return true;
    }

    if (mem_should_use_sbo(mem, new_slots, false)) {
        uint16_t extra_slots = (uint16_t)((new_cap - cap) * 2u);
        if ((uint16_t)(data_idx + cap * 2u) == mem->pool_top &&
            (uint32_t)mem->pool_top + extra_slots <= (uint32_t)MEM_MAX_F64_POOL) {
            uint16_t ext_idx = mem_alloc_pool(mem, extra_slots);
            if (ext_idx == MEM_NO_IDX || mem->has_error) return false;
            memset(&mem->f64_pool[ext_idx], 0, (size_t)extra_slots * sizeof(double));
            mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
            return true;
        }
        uint16_t new_data = mem_alloc_pool(mem, (uint16_t)new_slots);
        if (new_data == MEM_NO_IDX || mem->has_error) return false;
        if (copy_slots > 0) {
            memcpy(&mem->f64_pool[new_data], &mem->f64_pool[data_idx],
                   (size_t)copy_slots * sizeof(double));
        }
        if (new_slots > copy_slots) {
            memset(&mem->f64_pool[new_data + copy_slots], 0,
                   (size_t)(new_slots - copy_slots) * sizeof(double));
        }
        mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
        mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)new_data);
        return true;
    }

    /* Spill Map from SBO `f64_pool` to HeapBlock */
    uint16_t heap_idx = mem_heap_alloc_block(mem, hdr_idx, new_slots);
    if (heap_idx == MEM_NO_IDX || mem->has_error) return false;
    double *dst = mem_data_ptr(mem, heap_idx, 0);
    if (copy_slots > 0 && dst != NULL) {
        memcpy(dst, &mem->f64_pool[data_idx], (size_t)copy_slots * sizeof(double));
    }
    if ((uint16_t)(data_idx + cap * 2u) == mem->pool_top) {
        mem->pool_top = data_idx;
    }
    mem_write_pool(mem, (uint16_t)(hdr_idx + 2), (double)new_cap);
    mem_write_pool(mem, (uint16_t)(hdr_idx + 3), (double)heap_idx);
    return true;
}

StrSlice mem_map_entry_key(const MemArena *mem, uint16_t data_idx, uint16_t entry_i) {
    StrSlice empty = { 0, 0 };
    const double *p = mem_data_ptr_ro(mem, data_idx, (uint32_t)entry_i * 2u);
    if (!p) return empty;
    uint32_t packed = (uint32_t)(*p);
    StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
    return s;
}

Value mem_map_entry_val(const MemArena *mem, uint16_t data_idx, uint16_t entry_i) {
    const double *p = mem_data_ptr_ro(mem, data_idx, (uint32_t)entry_i * 2u + 1u);
    if (!p) return val_void();
    Value v = 0;
    memcpy(&v, p, sizeof(Value));
    return v;
}

void mem_map_set_entry(MemArena *mem, uint16_t data_idx, uint16_t entry_i,
                       StrSlice key, Value val) {
    if (!mem || mem->has_error) return;
    double *pk = mem_data_ptr(mem, data_idx, (uint32_t)entry_i * 2u);
    double *pv = mem_data_ptr(mem, data_idx, (uint32_t)entry_i * 2u + 1u);
    if (!pk || !pv) return;
    uint32_t packed = ((uint32_t)key.off << 16u) | (uint32_t)key.len;
    *pk = (double)packed;
    memcpy(pv, &val, sizeof(Value));
}

void mem_val_mark_immutable(MemArena *mem, Value v) {
    if (!mem) return;
    uint16_t tid = val_get_type(mem, v);
    if (tid == 0) return;
    if (tid == mem->vec_type_id || tid == mem->map_type_id) {
        uint16_t hdr = val_get_pool_idx(v);
        if (hdr < MEM_MAX_F64_POOL) {
            uint16_t raw0 = (uint16_t)mem_read_pool(mem, hdr);
            mem_write_pool(mem, hdr, (double)(uint16_t)(raw0 | MEM_HDR_IMMUTABLE_BIT));
        }
    } else if (tid == mem->mat_type_id) {
        uint16_t hdr = val_get_pool_idx(v);
        if ((uint32_t)hdr + 3u < MEM_MAX_F64_POOL) {
            uint16_t raw3 = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr + 3u));
            mem_write_pool(mem, (uint16_t)(hdr + 3u), (double)(uint16_t)(raw3 | MEM_HDR_IMMUTABLE_BIT));
        }
    }
}

bool mem_val_is_immutable(const MemArena *mem, Value v) {
    if (!mem) return false;
    uint16_t tid = val_get_type(mem, v);
    if (tid == 0) return false;
    if (tid == mem->vec_type_id || tid == mem->map_type_id) {
        uint16_t hdr = val_get_pool_idx(v);
        if (hdr < MEM_MAX_F64_POOL) {
            uint16_t raw0 = (uint16_t)mem_read_pool(mem, hdr);
            return (raw0 & MEM_HDR_IMMUTABLE_BIT) != 0;
        }
    } else if (tid == mem->mat_type_id) {
        uint16_t hdr = val_get_pool_idx(v);
        if ((uint32_t)hdr + 3u < MEM_MAX_F64_POOL) {
            uint16_t raw3 = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr + 3u));
            return (raw3 & MEM_HDR_IMMUTABLE_BIT) != 0;
        }
    }
    return false;
}

uint16_t mem_push_temp_root(MemArena *mem, Value *vptr) {
    if (!mem || !vptr) return 0;
    uint16_t saved = mem->temp_root_count;
    if (mem->temp_root_count < MEM_MAX_TEMP_ROOTS) {
        mem->temp_roots[mem->temp_root_count++] = vptr;
    }
    return saved;
}

void mem_pop_temp_roots(MemArena *mem, uint16_t saved_count) {
    if (!mem) return;
    if (saved_count <= MEM_MAX_TEMP_ROOTS) {
        mem->temp_root_count = saved_count;
    }
}

uint16_t mem_scratch_pool_used(const MemArena *mem) {
    if (!mem || mem->pool_top <= mem->perm_pool_top) return 0;
    return (uint16_t)(mem->pool_top - mem->perm_pool_top);
}


uint16_t mem_alloc_ast(MemArena *mem, uint8_t kind, uint8_t sub,
                       uint16_t ref, uint16_t left, uint16_t right) {
    if (mem->ast_count >= MEM_MAX_AST_NODES) {
        mem_err(mem, "AST node pool exhausted");
        return MEM_NO_IDX;
    }
    if (!mem_ensure_ast_cap(mem, (uint16_t)(mem->ast_count + 1u))) {
        return MEM_NO_IDX;
    }
    uint16_t idx = mem->ast_count++;
    AstNode *n = &mem_ast_base_mut(mem)[idx];
    n->kind  = kind;
    n->sub   = sub;
    n->ref   = ref;
    n->left  = left;
    n->right = right;
    return idx;
}

static const AstNode k_invalid_ast = { 0xFFu, 0, MEM_NO_IDX, MEM_NO_IDX, MEM_NO_IDX };

const AstNode *mem_get_ast(const MemArena *mem, uint16_t idx) {
    if (!mem || idx >= mem->ast_count || idx >= MEM_MAX_AST_NODES) {
        return &k_invalid_ast;
    }
    return &mem_ast_base(mem)[idx];
}

AstNode *mem_get_ast_mut(MemArena *mem, uint16_t idx) {
    if (!mem) return NULL;
    if (idx >= mem->ast_count || idx >= MEM_MAX_AST_NODES) {
        mem_err(mem, "AST node index %u out of bounds (count=%u)",
                (unsigned)idx, (unsigned)mem->ast_count);
        mem->invalid_ast_mut = k_invalid_ast;
        return &mem->invalid_ast_mut;
    }
    return &mem_ast_base_mut(mem)[idx];
}

uint16_t mem_push_frame(MemArena *mem, uint8_t slot_count) {
    uint16_t base = mem->sp;
    if (base + slot_count > MEM_MAX_CALL_STACK) {
        mem_err(mem, "Call stack overflow (max %d slots)", MEM_MAX_CALL_STACK);
        return base;
    }
    if (slot_count > 0) {
        memset(&mem->slot_locked_type[base], 0, slot_count);
        memset(&mem->slot_locked_unit[base], 0, slot_count);
        memset(&mem->slot_is_const[base],    0, slot_count);
    }
    mem->sp = (uint16_t)(mem->sp + slot_count);
    return base;
}

void mem_pop_frame(MemArena *mem, uint16_t frame_base) {
    if (!mem) return;
    if (mem->heap_active_count > 0) {
        for (uint16_t i = 0; i < MEM_MAX_HEAP_BLOCKS; ++i) {
            HeapBlock *hb = &mem->heap_blocks[i];
            if (hb->in_use == 2u && hb->data != NULL && hb->cap_slots >= 3u) {
                uint32_t meta   = (uint32_t)hb->data[hb->cap_slots - 1u];
                uint16_t orig_fb = (uint16_t)(meta >> 8u);
                uint8_t  s_start = (uint8_t)(meta & 0xFFu);
                if (orig_fb >= frame_base) {
                    for (uint8_t k = 0; k < s_start && (uint32_t)k * 2u + 1u < hb->cap_slots; ++k) {
                        uint16_t abs_s = (uint16_t)(orig_fb + k);
                        if (abs_s < MEM_MAX_CALL_STACK) {
                            memcpy(&hb->data[k], &mem->stack[abs_s], sizeof(Value));
                            uint32_t lpack = (uint32_t)mem->slot_locked_type[abs_s]
                                           | ((uint32_t)mem->slot_locked_unit[abs_s] << 8u)
                                           | ((uint32_t)mem->slot_is_const[abs_s] << 16u);
                            hb->data[s_start + k] = (double)lpack;
                        }
                    }
                    hb->in_use = 3u;
                }
            }
        }
    }
    mem->sp = frame_base;
}

Value mem_get_slot(const MemArena *mem, uint16_t frame_base, uint8_t slot) {
    uint32_t idx = (uint32_t)frame_base + (uint32_t)slot;
    if (!mem || idx >= MEM_MAX_CALL_STACK) return val_void();
    return mem->stack[idx];
}

void mem_set_slot(MemArena *mem, uint16_t frame_base, uint8_t slot, Value v) {
    uint32_t idx = (uint32_t)frame_base + (uint32_t)slot;
    if (!mem || idx >= MEM_MAX_CALL_STACK) {
        if (mem) mem_err(mem, "Stack slot index %u out of bounds", (unsigned)idx);
        return;
    }
    mem->stack[idx] = v;
}

/* ============================================================================
 * 4. VALUE ENCODING (8-BYTE IEEE-754 NAN-BOXING)
 * ============================================================================ */

#define NANBOX_PREFIX     0xFFF8000000000000ULL
#define NANBOX_MASK       0xFFF8000000000000ULL
#define NANBOX_TAG_SHIFT  48
#define NANBOX_TYPE_SHIFT 32
#define NANBOX_UNIT_SHIFT 40

#define NB_TAG_VOID       0xFFF8ULL
#define NB_TAG_POOL       0xFFF9ULL
#define NB_TAG_STR        0xFFFAULL

static inline uint64_t f64_to_bits(double d) {
    d = canonicalize_f64(d);
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    if ((u & NANBOX_MASK) == NANBOX_PREFIX) {
        u = 0x7FF8000000000000ULL;
    }
    return u;
}

static inline double bits_to_f64(uint64_t u) {
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

Value val_void(void) {
    return (NB_TAG_VOID << NANBOX_TAG_SHIFT);
}

Value val_make_f64_at_pool(const MemArena *mem, uint16_t type_id, uint8_t unit_id, uint16_t pool_idx) {
    if (type_id == mem->fast_real_type && type_id != 0 && unit_id == MEM_UNIT_NONE) {
        return f64_to_bits(mem_read_pool(mem, pool_idx));
    }
    return (NB_TAG_POOL << NANBOX_TAG_SHIFT)
         | ((uint64_t)unit_id << NANBOX_UNIT_SHIFT)
         | ((uint64_t)(type_id & 0xFFu) << NANBOX_TYPE_SHIFT)
         | (uint64_t)pool_idx;
}

Value val_make_f64_unit(MemArena *mem, uint16_t type_id, uint8_t unit_id, double d) {
    if (type_id == mem->fast_real_type && type_id != 0 && unit_id == MEM_UNIT_NONE) {
        return f64_to_bits(d);
    }
    uint16_t idx = mem_alloc_pool(mem, 1);
    if (idx == MEM_NO_IDX || mem->has_error) return val_void();
    mem_write_pool(mem, idx, d);
    return (NB_TAG_POOL << NANBOX_TAG_SHIFT)
         | ((uint64_t)unit_id << NANBOX_UNIT_SHIFT)
         | ((uint64_t)(type_id & 0xFFu) << NANBOX_TYPE_SHIFT)
         | (uint64_t)idx;
}

Value val_make_f64(MemArena *mem, uint16_t type_id, double d) {
    return val_make_f64_unit(mem, type_id, MEM_UNIT_NONE, d);
}

double val_get_f64(const MemArena *mem, Value v) {
    uint16_t top = (uint16_t)(v >> NANBOX_TAG_SHIFT);
    if (top < NB_TAG_VOID) {
        return bits_to_f64(v);
    }
    uint16_t idx = (uint16_t)(v & 0xFFFFu);
    return mem_read_pool(mem, idx);
}

Value val_make_str(MemArena *mem, uint16_t type_id, StrSlice s) {
    (void)mem;
    uint32_t payload = ((uint32_t)s.off << 16) | (uint32_t)s.len;
    return (NB_TAG_STR << NANBOX_TAG_SHIFT)
         | ((uint64_t)(type_id & 0xFFu) << NANBOX_TYPE_SHIFT)
         | payload;
}

StrSlice val_get_str(const MemArena *mem, Value v) {
    (void)mem;
    uint32_t payload = (uint32_t)(v & 0xFFFFFFFFu);
    StrSlice s = { (uint16_t)(payload >> 16), (uint16_t)(payload & 0xFFFFu) };
    return s;
}

Value val_make_comp(MemArena *mem, uint16_t type_id, uint16_t pool_idx, uint16_t flat_size) {
    (void)mem;
    if (pool_idx == MEM_NO_IDX) return val_void();
    uint32_t payload = ((uint32_t)flat_size << 16) | (uint32_t)pool_idx;
    return (NB_TAG_POOL << NANBOX_TAG_SHIFT)
         | ((uint64_t)(type_id & 0xFFu) << NANBOX_TYPE_SHIFT)
         | payload;
}

uint16_t val_get_pool_idx(Value v) {
    return (uint16_t)(v & 0xFFFFu);
}

uint16_t val_get_type(const MemArena *mem, Value v) {
    uint16_t top = (uint16_t)(v >> NANBOX_TAG_SHIFT);
    if (top < NB_TAG_VOID) {
        return mem->fast_real_type;
    }
    if (top == NB_TAG_VOID) return MEM_TYPE_VOID;
    return (uint16_t)((v >> NANBOX_TYPE_SHIFT) & 0xFFu);
}

uint8_t val_get_unit(const MemArena *mem, Value v) {
    (void)mem;
    uint16_t top = (uint16_t)(v >> NANBOX_TAG_SHIFT);
    if (top <= NB_TAG_VOID) {
        return MEM_UNIT_NONE;
    }
    return (uint8_t)((v >> NANBOX_UNIT_SHIFT) & 0xFFu);
}

Value val_with_unit(MemArena *mem, Value v, uint8_t unit_id) {
    uint16_t top = (uint16_t)(v >> NANBOX_TAG_SHIFT);
    if (top == NB_TAG_VOID) return v;
    if (top < NB_TAG_VOID) {
        if (unit_id == MEM_UNIT_NONE) return v;
        return val_make_f64_unit(mem, mem->fast_real_type, unit_id, bits_to_f64(v));
    }
    uint16_t tid = (uint16_t)((v >> NANBOX_TYPE_SHIFT) & 0xFFu);
    if (unit_id == MEM_UNIT_NONE && tid == mem->fast_real_type && tid != 0 &&
        ((v >> 16) & 0xFFFFu) == 0) {
        return f64_to_bits(val_get_f64(mem, v));
    }
    v &= ~(0xFFULL << NANBOX_UNIT_SHIFT);
    v |= ((uint64_t)unit_id << NANBOX_UNIT_SHIFT);
    return v;
}

Value val_clone_comp(MemArena *mem, Value v, uint16_t flat_size) {
    uint16_t tid     = val_get_type(mem, v);
    uint8_t  unit    = val_get_unit(mem, v);
    uint16_t old_idx = val_get_pool_idx(v);
    uint16_t new_idx = mem_alloc_pool(mem, flat_size);
    if (new_idx == MEM_NO_IDX || mem->has_error) return val_void();
    for (uint16_t i = 0; i < flat_size; ++i) {
        mem_write_pool(mem, (uint16_t)(new_idx + i), mem_read_pool(mem, (uint16_t)(old_idx + i)));
    }
    return val_with_unit(mem, val_make_comp(mem, tid, new_idx, flat_size), unit);
}

Value val_from_f64(MemArena *mem, double d) {
    return val_make_f64(mem, mem->fast_real_type, d);
}

Value val_from_cstr(MemArena *mem, const char *cstr) {
    if (!cstr || mem->str_type_id == 0) return val_void();
    StrSlice s = mem_intern_str(mem, cstr, (uint16_t)strlen(cstr));
    if (mem->has_error) return val_void();
    return val_make_str(mem, mem->str_type_id, s);
}

Value val_from_f64_vec(MemArena *mem, const double *vals, uint16_t count) {
    if (mem->vec_type_id == 0) return val_void();
    uint16_t hdr_idx = mem_vec_alloc_hdr(mem, mem->fast_real_type, count);
    if (hdr_idx == MEM_NO_IDX || mem->has_error) return val_void();
    if (count > 0 && vals != NULL) {
        if (!mem_vec_reserve(mem, hdr_idx, 1, count)) return val_void();
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
        for (uint16_t i = 0; i < count; ++i) {
            mem_data_write(mem, data_idx, i, vals[i]);
        }
        mem_write_pool(mem, (uint16_t)(hdr_idx + 1), (double)count);
    }
    return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
}

bool val_as_cstr(const MemArena *mem, Value v, char *buf, size_t cap) {
    if (!buf || cap == 0) return false;
    buf[0] = '\0';
    if (mem->str_type_id == 0 || val_get_type(mem, v) != mem->str_type_id) {
        return false;
    }
    StrSlice s = val_get_str(mem, v);
    size_t copy_len = (s.len < cap - 1) ? s.len : (cap - 1);
    if (copy_len > 0) {
        memcpy(buf, mem_str_ptr(mem, s), copy_len);
    }
    buf[copy_len] = '\0';
    return true;
}

