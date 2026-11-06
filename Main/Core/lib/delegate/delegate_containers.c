/*
 * delegate_containers.c — Vector, Map & Matrix Intrinsic Delegates and Value Formatting
 */
#include "delegate.h"
#include "../../Headers/parser.h"
#include "../hardcode/hardcode.h"
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void delegate_vec_write_elem(MemArena *mem, const SymTable *tab,
                             uint16_t elem_tid, uint16_t data_idx, uint32_t offset, Value item) {
    if (elem_tid == SYM_TYPE_ANY) {
        double *p = mem_data_ptr(mem, data_idx, offset);
        if (p) memcpy(p, &item, sizeof(Value));
    } else if (elem_tid == mem->str_type_id && elem_tid != 0) {
        StrSlice s = val_get_str(mem, item);
        uint32_t packed = ((uint32_t)s.off << 16) | (uint32_t)s.len;
        mem_data_write(mem, data_idx, offset, (double)packed);
    } else if (elem_tid < tab->type_count && !tab->types[elem_tid].is_consuming) {
        uint16_t stride   = tab->types[elem_tid].flat_size;
        uint16_t src_pidx = val_get_pool_idx(item);
        for (uint16_t k = 0; k < stride; ++k) {
            mem_data_write(mem, data_idx, offset + k, mem_read_pool(mem, (uint16_t)(src_pidx + k)));
        }
    } else {
        mem_data_write(mem, data_idx, offset, val_get_f64(mem, item));
    }
}

Value delegate_vec_read_elem(MemArena *mem, const SymTable *tab,
                             uint16_t elem_tid, uint8_t elem_uid, uint16_t data_idx, uint32_t offset) {
    if (elem_tid == SYM_TYPE_ANY) {
        const double *p = mem_data_ptr_ro(mem, data_idx, offset);
        if (!p) return val_void();
        Value v = 0;
        memcpy(&v, p, sizeof(Value));
        return v;
    } else if (elem_tid == mem->str_type_id && elem_tid != 0) {
        uint32_t packed = (uint32_t)mem_data_read(mem, data_idx, offset);
        StrSlice s = { (uint16_t)(packed >> 16), (uint16_t)(packed & 0xFFFFu) };
        return val_make_str(mem, elem_tid, s);
    } else if (elem_tid < tab->type_count && !tab->types[elem_tid].is_consuming) {
        uint16_t stride = tab->types[elem_tid].flat_size;
        uint16_t src_pidx = (uint16_t)(data_idx + offset);
        return val_with_unit(mem, val_make_comp(mem, elem_tid, src_pidx, stride), elem_uid);
    } else {
        return val_make_f64_unit(mem, elem_tid ? elem_tid : mem->fast_real_type,
                                 elem_uid, mem_data_read(mem, data_idx, offset));
    }
}

/*Below section is the C's native for operator/function delegates @fcall, @vmap, @vfilter, @vreduce, @vany, @vall, @vfind, @vcount, @vnew, @vfill, @vrange, @vin, @vpush, @vpop, @vshift, @vpeek, @vget, @vset, @vlen, @vcap, @vresize, @vshrink, @vclear*/
/*Construct call(f, ...), map, filter, reduce, fold, any, all, find, count_if, vec/list/stack/queue, .., ..., ..<, <.., in, into, push, pop, enqueue, dequeue, shift, peek, top, front, back*/
Value delegate_eval_vec_intrinsic(MemArena *mem, SymTable *tab,
                                  IntrinsicId intr, Value a, Value b, Value c) {
    /*Below section is the C's native for operator delegate @fcall*/
    /*Construct call(f, a) and call(f, a, b)*/
    if (intr == INTR_FCALL) {
        if (mem->fn_type_id == 0 || val_get_type(mem, a) != mem->fn_type_id) {
            mem_err(mem, "First argument to call() must be a function ($fn)");
            return val_void();
        }
        uint16_t hdr_idx   = val_get_pool_idx(a);
        uint8_t  param_cnt = (uint8_t)((uint8_t)mem_read_pool(mem, hdr_idx + 1) & 0x3Fu);
        Value args[2] = { b, c };
        return delegate_call_fn(mem, tab, a, param_cnt, args);
    }

    /*Below section is the C's native for operator/function delegates @map_new..@map_entries and @vlen/@vclear/@vin on $map*/
    /*Construct map(), dict(), m[k], m[k] = v, k in m, map_set, map_get, map_has, map_del, keys(m), vals(m), entries(m)*/
    if (delegate_is_map_intrinsic(intr)) {
        if (mem->map_type_id == 0) {
            mem_err(mem, "Map type ($map) is not loaded");
            return val_void();
        }
        if (intr == INTR_MAP_NEW) {
            int cap_in = 0;
            if (val_get_type(mem, a) != MEM_TYPE_VOID) {
                if (!delegate_val_to_exact_int(mem, tab, a, "map capacity", &cap_in)) return val_void();
            }
            if (cap_in < 0 || cap_in > MEM_MAX_F64_POOL / 2) {
                mem_err(mem, "Map capacity out of range (%d)", cap_in);
                return val_void();
            }
            uint16_t hdr_idx = mem_map_alloc_hdr(mem, 0, (uint16_t)cap_in);
            if (mem->has_error) return val_void();
            return val_make_comp(mem, mem->map_type_id, hdr_idx, MEM_MAP_HDR_SLOTS);
        }
        if (val_get_type(mem, a) != mem->map_type_id) {
            mem_err(mem, "Expected map ($map) as first argument");
            return val_void();
        }
        if ((intr == INTR_MAP_SET || intr == INTR_MAP_DEL) && mem_val_is_immutable(mem, a)) {
            mem_err(mem, "Cannot mutate immutable constant map");
            return val_void();
        }
        uint16_t hdr_idx  = val_get_pool_idx(a);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint16_t lock_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  lock_uid = (uint8_t)((raw_hdr0 >> 8u) & 0x7Fu);
        uint16_t len      = (uint16_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);

        /* Convert key `b` to StrSlice (either direct $str or stringified scalar) */
        StrSlice key_s = { 0, 0 };
        if (val_get_type(mem, b) == mem->str_type_id && mem->str_type_id != 0) {
            key_s = val_get_str(mem, b);
        } else {
            Value sb = delegate_eval_intrinsic(mem, tab, INTR_TOSTR, b, val_void(), val_void(), 0);
            if (mem->has_error) return val_void();
            key_s = val_get_str(mem, sb);
        }

        int found_idx = -1;
        for (uint16_t i = 0; i < len; ++i) {
            StrSlice ks = mem_map_entry_key(mem, data_idx, i);
            if (mem_slice_eq(mem, ks, key_s)) {
                found_idx = (int)i;
                break;
            }
        }

        if (intr == INTR_MAP_HAS) {
            return val_make_f64(mem, mem->fast_real_type, (found_idx >= 0) ? 1.0 : 0.0);
        }
        if (intr == INTR_MAP_GET) {
            if (found_idx >= 0) {
                return mem_map_entry_val(mem, data_idx, (uint16_t)found_idx);
            }
            if (val_get_type(mem, c) != MEM_TYPE_VOID) {
                return c; /* get_or(m, key, default_val) */
            }
            mem_err(mem, "Key '%.*s' not found in map", (int)key_s.len, mem_str_ptr(mem, key_s));
            return val_void();
        }
        if (intr == INTR_MAP_DEL) {
            if (found_idx >= 0) {
                uint16_t new_len = (uint16_t)(len - 1);
                for (uint16_t i = (uint16_t)found_idx; i < new_len; ++i) {
                    StrSlice nk = mem_map_entry_key(mem, data_idx, (uint16_t)(i + 1));
                    Value    nv = mem_map_entry_val(mem, data_idx, (uint16_t)(i + 1));
                    mem_map_set_entry(mem, data_idx, i, nk, nv);
                }
                mem_write_pool(mem, hdr_idx + 1, (double)new_len);
            }
            return a;
        }
        if (intr == INTR_MAP_SET) {
            uint16_t v_tid = val_get_type(mem, c);
            uint8_t  v_uid = val_get_unit(mem, c);
            if (lock_tid != 0 && lock_tid != v_tid) {
                char exp_t[32], got_t[32];
                sym_type_name(tab, mem, lock_tid, exp_t, sizeof(exp_t));
                sym_type_name(tab, mem, v_tid, got_t, sizeof(got_t));
                mem_err(mem, "Map value type mismatch: expected '%s', got '%s'", exp_t, got_t);
                return val_void();
            }
            if (lock_uid != 0 && lock_uid != v_uid) {
                mem_err(mem, "Map value unit mismatch");
                return val_void();
            }
            Value stored_v = c;
            if (v_tid != 0 && v_tid < tab->type_count && !tab->types[v_tid].is_consuming &&
                v_tid != mem->vec_type_id && v_tid != mem->mat_type_id &&
                v_tid != mem->fn_type_id && v_tid != mem->map_type_id) {
                stored_v = val_clone_comp(mem, c, tab->types[v_tid].flat_size);
                if (mem->has_error) return val_void();
            }
            if (found_idx >= 0) {
                mem_map_set_entry(mem, data_idx, (uint16_t)found_idx, key_s, stored_v);
                return a;
            }
            if (!mem_map_reserve(mem, hdr_idx, (uint16_t)(len + 1))) {
                return val_void();
            }
            data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            mem_map_set_entry(mem, data_idx, len, key_s, stored_v);
            mem_write_pool(mem, hdr_idx + 1, (double)(len + 1));
            return a;
        }
    }

    if (mem->map_type_id != 0 && val_get_type(mem, a) == mem->map_type_id) {
        uint16_t hdr_idx = val_get_pool_idx(a);
        if (intr == INTR_VLEN) {
            return val_make_f64(mem, mem->fast_real_type, mem_read_pool(mem, hdr_idx + 1));
        }
        if (intr == INTR_VCLEAR) {
            if (mem_val_is_immutable(mem, a)) {
                mem_err(mem, "Cannot mutate immutable constant map");
                return val_void();
            }
            mem_write_pool(mem, hdr_idx + 1, 0.0);
            return a;
        }
    }

    if (mem->vec_type_id == 0) {
        mem_err(mem, "Dynamic vector type ($vec) is not loaded");
        return val_void();
    }

    /*Below section is the C's native for operator delegate @vnew*/
    /*Construct vec(), vec(cap), list(), stack(), queue()*/
    if (intr == INTR_VNEW) {
        int cap_in = 0;
        if (val_get_type(mem, a) != MEM_TYPE_VOID) {
            if (!delegate_val_to_exact_int(mem, tab, a, "vector capacity", &cap_in)) return val_void();
        }
        if (cap_in < 0 || (uint32_t)cap_in > MEM_MAX_HEAP_SLOTS) {
            mem_err(mem, "Vector capacity out of range (%d)", cap_in);
            return val_void();
        }
        uint16_t hdr_idx = mem_vec_alloc_hdr(mem, 0, (uint32_t)cap_in);
        if (mem->has_error) return val_void();
        return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    }

    /*Below section is the C's native for operator delegate @vfill*/
    /*Construct vec(count, init), list(item), stack(item), queue(item)*/
    if (intr == INTR_VFILL) {
        int count = 0;
        if (!delegate_val_to_exact_int(mem, tab, a, "vector fill count", &count)) return val_void();
        if (count < 0 || (uint32_t)count > MEM_MAX_HEAP_SLOTS) {
            mem_err(mem, "Vector fill count out of range (%d)", count);
            return val_void();
        }
        uint16_t elem_tid = val_get_type(mem, b);
        uint8_t  elem_uid = val_get_unit(mem, b);
        uint16_t packed_t = (uint16_t)(((uint32_t)elem_uid << 8u) | ((uint32_t)elem_tid & 0xFFu));
        uint16_t stride   = (elem_tid < tab->type_count) ? tab->types[elem_tid].flat_size : 1;
        uint16_t hdr_idx  = mem_vec_alloc_hdr(mem, packed_t, (uint32_t)count);
        if (mem->has_error) return val_void();
        if (count > 0) {
            if (!mem_vec_reserve(mem, hdr_idx, stride, (uint32_t)count)) return val_void();
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            for (uint32_t i = 0; i < (uint32_t)count; ++i) {
                delegate_vec_write_elem(mem, tab, elem_tid, data_idx, i * (uint32_t)stride, b);
            }
            mem_write_pool(mem, hdr_idx + 1, (double)count);
        }
        return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    }

    /*Below section is the C's native for operator delegates @vlen and @vget on $str*/
    /*Construct len(s:str), at(s:str, i), s[i], s[a...b], and s[a...b by step]*/
    if (val_get_type(mem, a) == mem->str_type_id && mem->str_type_id != 0) {
        if (intr == INTR_VLEN) {
            return val_make_f64(mem, mem->fast_real_type, (double)val_get_str(mem, a).len);
        }
        if (intr == INTR_VGET) {
            StrSlice str_s = val_get_str(mem, a);
            int k = 0;
            if (!delegate_val_to_exact_int(mem, tab, b, "string index", &k)) {
                return val_void();
            }
            if (k < 0) k += (int)str_s.len;
            if (k < 0 || k >= (int)str_s.len) {
                mem_err(mem, "String index %d out of bounds (len=%u)", k, (unsigned)str_s.len);
                return val_void();
            }
            const char *sp = mem_str_ptr(mem, str_s);
            StrSlice ch_s = mem_intern_str(mem, sp + k, 1);
            if (mem->has_error) return val_void();
            return val_make_str(mem, mem->str_type_id, ch_s);
        }
    }

    /*Below section is the C's native for operator delegates @vrange, @vrange_cat, and @vrange_step*/
    /*Construct a...b, a..<b, a>..b, a>..<b, nested multi-segment exclusion ranges, and stepped ranges (a...b by step) / strided vectors (v by step)*/
    if (intr == INTR_VRANGE) {
        int mode = (val_get_type(mem, c) != MEM_TYPE_VOID) ? (int)val_get_f64(mem, c) : 0;
        return delegate_eval_range_step(mem, tab, a, b, val_void(), mode);
    }
    if (intr == INTR_VRANGE_CAT) {
        if (val_get_type(mem, a) != mem->vec_type_id || val_get_type(mem, b) != mem->vec_type_id) {
            mem_err(mem, "Range concatenation expects two range vectors");
            return val_void();
        }
        uint16_t ha = val_get_pool_idx(a);
        uint16_t hb = val_get_pool_idx(b);
        uint16_t raw_a = (uint16_t)mem_read_pool(mem, ha + 0);
        uint16_t raw_b = (uint16_t)mem_read_pool(mem, hb + 0);
        uint16_t tid_a = (uint16_t)(raw_a & 0xFFu);
        uint16_t tid_b = (uint16_t)(raw_b & 0xFFu);
        uint8_t  uid_a = (uint8_t)((raw_a >> 8) & 0x7Fu);
        uint8_t  uid_b = (uint8_t)((raw_b >> 8) & 0x7Fu);
        uint32_t len_a = (uint32_t)mem_read_pool(mem, ha + 1);
        uint32_t len_b = (uint32_t)mem_read_pool(mem, hb + 1);
        if (len_a > 0 && len_b > 0 && (tid_a != tid_b || uid_a != uid_b)) {
            mem_err(mem, "Unit or type mismatch across concatenated range segments");
            return val_void();
        }
        uint16_t out_raw = (len_a > 0) ? raw_a : raw_b;
        uint16_t out_tid = (uint16_t)(out_raw & 0xFFu);
        if (out_tid == 0) out_tid = mem->fast_real_type;
        uint16_t stride  = (out_tid < tab->type_count) ? tab->types[out_tid].flat_size : 1;
        uint32_t tot_len = len_a + len_b;
        if (tot_len > MEM_MAX_HEAP_SLOTS) {
            mem_err(mem, "Concatenated range length (%u) exceeds maximum capacity", (unsigned)tot_len);
            return val_void();
        }
        uint16_t out_hdr = mem_vec_alloc_hdr(mem, out_raw, tot_len);
        if (mem->has_error) return val_void();
        if (tot_len > 0) {
            if (!mem_vec_reserve(mem, out_hdr, stride, tot_len)) return val_void();
            uint16_t dat_a   = (uint16_t)mem_read_pool(mem, ha + 3);
            uint16_t dat_b   = (uint16_t)mem_read_pool(mem, hb + 3);
            uint16_t out_dat = (uint16_t)mem_read_pool(mem, out_hdr + 3);
            double *dst = mem_data_ptr(mem, out_dat, 0);
            if (!dst) return val_void();
            if (len_a > 0) {
                const double *sa = mem_data_ptr_ro(mem, dat_a, 0);
                if (sa) memcpy(dst, sa, (size_t)(len_a * stride) * sizeof(double));
            }
            if (len_b > 0) {
                const double *sb = mem_data_ptr_ro(mem, dat_b, 0);
                if (sb) memcpy(dst + (size_t)(len_a * stride), sb, (size_t)(len_b * stride) * sizeof(double));
            }
            mem_write_pool(mem, out_hdr + 1, (double)tot_len);
        }
        /* Merge vrange metadata cache if both operands are cached range segments */
        int ka = -1, kb = -1;
        for (uint8_t k = 0; k < 4; ++k) {
            if (ha != 0 && mem->vrange_hdr[k] == ha) ka = (int)k;
            if (hb != 0 && mem->vrange_hdr[k] == hb) kb = (int)k;
        }
        if (ka >= 0 && kb >= 0 &&
            (uint32_t)mem->vrange_seg_cnt[ka] + (uint32_t)mem->vrange_seg_cnt[kb] <= 8u) {
            uint8_t sc_a = mem->vrange_seg_cnt[ka];
            uint8_t sc_b = mem->vrange_seg_cnt[kb];
            uint8_t m_modes[8];
            double  m_a[8], m_b[8];
            for (uint8_t i = 0; i < sc_a; ++i) {
                m_modes[i] = mem->vrange_mode[ka][i];
                m_a[i]     = mem->vrange_a[ka][i];
                m_b[i]     = mem->vrange_b[ka][i];
            }
            for (uint8_t i = 0; i < sc_b; ++i) {
                m_modes[sc_a + i] = mem->vrange_mode[kb][i];
                m_a[sc_a + i]     = mem->vrange_a[kb][i];
                m_b[sc_a + i]     = mem->vrange_b[kb][i];
            }
            uint8_t slot = (uint8_t)(mem->vrange_cnt++ & 3u);
            mem->vrange_hdr[slot]     = out_hdr;
            mem->vrange_unit[slot]    = mem->vrange_unit[ka];
            mem->vrange_seg_cnt[slot] = (uint8_t)(sc_a + sc_b);
            for (uint8_t i = 0; i < sc_a + sc_b; ++i) {
                mem->vrange_mode[slot][i] = m_modes[i];
                mem->vrange_a[slot][i]    = m_a[i];
                mem->vrange_b[slot][i]    = m_b[i];
            }
        }
        return val_make_comp(mem, mem->vec_type_id, out_hdr, MEM_VEC_HDR_SLOTS);
    }
    if (intr == INTR_VRANGE_STEP) {
        if (val_get_type(mem, a) != mem->vec_type_id) {
            return delegate_eval_range_step(mem, tab, a, b, c, 0);
        }
        uint16_t rng_hdr = val_get_pool_idx(a);
        for (uint8_t k = 0; k < 4; ++k) {
            if (rng_hdr != 0 && mem->vrange_hdr[k] == rng_hdr) {
                uint8_t sc = mem->vrange_seg_cnt[k];
                if (sc <= 1) {
                    Value va = val_make_f64_unit(mem, mem->fast_real_type, mem->vrange_unit[k], mem->vrange_a[k][0]);
                    Value vb = val_make_f64_unit(mem, mem->fast_real_type, mem->vrange_unit[k], mem->vrange_b[k][0]);
                    return delegate_eval_range_step(mem, tab, va, vb, b, (int)mem->vrange_mode[k][0]);
                }
                Value acc_vec = val_void();
                for (uint8_t s_i = 0; s_i < sc && !mem->has_error; ++s_i) {
                    Value va = val_make_f64_unit(mem, mem->fast_real_type, mem->vrange_unit[k], mem->vrange_a[k][s_i]);
                    Value vb = val_make_f64_unit(mem, mem->fast_real_type, mem->vrange_unit[k], mem->vrange_b[k][s_i]);
                    Value seg_v = delegate_eval_range_step(mem, tab, va, vb, b, (int)mem->vrange_mode[k][s_i]);
                    if (mem->has_error) return val_void();
                    if (s_i == 0) {
                        acc_vec = seg_v;
                    } else {
                        acc_vec = delegate_eval_vec_intrinsic(mem, tab, INTR_VRANGE_CAT, acc_vec, seg_v, val_void());
                    }
                }
                return acc_vec;
            }
        }
        mem_err(mem, "Operator 'by' requires a range expression");
        return val_void();
    }

    if (val_get_type(mem, a) != mem->vec_type_id) {
        mem_err(mem, "Expected dynamic vector ($vec) argument");
        return val_void();
    }
    if ((intr == INTR_VCLEAR || intr == INTR_VPUSH || intr == INTR_VPOP ||
         intr == INTR_VSHIFT || intr == INTR_VSET  || intr == INTR_VRESIZE) &&
        mem_val_is_immutable(mem, a)) {
        mem_err(mem, "Cannot mutate immutable constant vector");
        return val_void();
    }
    uint16_t hdr_idx  = val_get_pool_idx(a);
    uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
    uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
    uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
    uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
    uint32_t cap      = (uint32_t)mem_read_pool(mem, hdr_idx + 2);
    uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                        ? tab->types[elem_tid].flat_size : 1;

    switch (intr) {
        /*Below section is the C's native for operator delegates @vlen, @vcap, @vclear, @vshrink*/
        /*Construct len(v), cap(v), clear(v), shrink(v)*/
        case INTR_VLEN:
            return val_make_f64(mem, mem->fast_real_type, (double)len);
        case INTR_VCAP:
            return val_make_f64(mem, mem->fast_real_type, (double)cap);
        case INTR_VCLEAR:
            mem_write_pool(mem, hdr_idx + 1, 0.0);
            return a;
        case INTR_VSHRINK:
            mem_vec_shrink_to_fit(mem, hdr_idx, stride);
            return a;

        /*Below section is the C's native for operator delegate @vpush*/
        /*Construct push(v, x), enqueue(q, x), and x into v*/
        case INTR_VPUSH: {
            uint16_t item_tid = val_get_type(mem, b);
            uint8_t  item_uid = val_get_unit(mem, b);
            if (elem_tid == 0) {
                if (elem_uid != 0 && elem_uid != item_uid) {
                    char exp_u[32], got_u[32];
                    sym_unit_name(tab, mem, elem_uid, exp_u, sizeof(exp_u));
                    sym_unit_name(tab, mem, item_uid, got_u, sizeof(got_u));
                    mem_err(mem, "Vector unit mismatch: cannot push '[%s]' into vec<[%s]>", got_u, exp_u);
                    return val_void();
                }
                elem_tid = item_tid;
                elem_uid = item_uid;
                stride   = (elem_tid < tab->type_count) ? tab->types[elem_tid].flat_size : 1;
                uint16_t packed_t = (uint16_t)(((uint32_t)elem_uid << 8u) | ((uint32_t)elem_tid & 0xFFu));
                mem_write_pool(mem, hdr_idx + 0, (double)packed_t);
            } else if (elem_tid != SYM_TYPE_ANY && (elem_tid != item_tid || elem_uid != item_uid)) {
                char exp_t[32], got_t[32];
                sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                sym_type_name(tab, mem, item_tid, got_t, sizeof(got_t));
                mem_err(mem, "Vector type mismatch: cannot push '%s' into vec<%s>", got_t, exp_t);
                return val_void();
            }
            if (!mem_vec_reserve(mem, hdr_idx, stride, len + 1u)) {
                return val_void();
            }
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            delegate_vec_write_elem(mem, tab, elem_tid, data_idx, len * (uint32_t)stride, b);
            mem_write_pool(mem, hdr_idx + 1, (double)(len + 1u));
            return a;
        }

        /*Below section is the C's native for operator delegate @vpop*/
        /*Construct pop(v) (LIFO stack/vector pop from back)*/
        case INTR_VPOP: {
            if (len == 0) {
                mem_err(mem, "Cannot pop from an empty vector");
                return val_void();
            }
            uint32_t new_len  = len - 1u;
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            mem_write_pool(mem, hdr_idx + 1, (double)new_len);
            return delegate_vec_read_elem(mem, tab, elem_tid, elem_uid, data_idx, new_len * (uint32_t)stride);
        }

        /*Below section is the C's native for operator delegate @vshift*/
        /*Construct dequeue(q), shift(q), pop_front(q) (FIFO queue pop from front)*/
        case INTR_VSHIFT: {
            if (len == 0) {
                mem_err(mem, "Cannot dequeue/shift from an empty vector");
                return val_void();
            }
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            Value first = delegate_vec_read_elem(mem, tab, elem_tid, elem_uid, data_idx, 0);
            if (elem_tid != 0 && elem_tid < tab->type_count &&
                !tab->types[elem_tid].is_consuming &&
                elem_tid != mem->vec_type_id && elem_tid != mem->mat_type_id &&
                elem_tid != mem->fn_type_id && elem_tid != mem->map_type_id) {
                first = val_clone_comp(mem, first, stride);
                if (mem->has_error) return val_void();
            }
            uint32_t new_len = len - 1u;
            if (new_len > 0) {
                double *dptr = mem_data_ptr(mem, data_idx, 0);
                if (dptr) {
                    memmove(dptr, dptr + stride, (size_t)(new_len * (uint32_t)stride) * sizeof(double));
                }
            }
            mem_write_pool(mem, hdr_idx + 1, (double)new_len);
            return first;
        }

        /*Below section is the C's native for operator delegates @vget and @vset*/
        /*Construct at(v, i), set(v, i, x)*/
        case INTR_VGET: {
            int raw_idx = 0;
            if (!delegate_val_to_exact_int(mem, tab, b, "vector index", &raw_idx)) return val_void();
            int idx = (raw_idx < 0) ? (raw_idx + (int)len) : raw_idx;
            if (idx < 0 || idx >= (int)len) {
                mem_err(mem, "Vector index %d out of bounds (len=%u)", raw_idx, (unsigned)len);
                return val_void();
            }
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            return delegate_vec_read_elem(mem, tab, elem_tid, elem_uid, data_idx, (uint32_t)idx * (uint32_t)stride);
        }
        case INTR_VSET: {
            int raw_idx = 0;
            if (!delegate_val_to_exact_int(mem, tab, b, "vector index", &raw_idx)) return val_void();
            int idx = (raw_idx < 0) ? (raw_idx + (int)len) : raw_idx;
            if (idx < 0 || idx >= (int)len) {
                mem_err(mem, "Vector index %d out of bounds (len=%u)", raw_idx, (unsigned)len);
                return val_void();
            }
            uint16_t item_tid = val_get_type(mem, c);
            uint8_t  item_uid = val_get_unit(mem, c);
            if (elem_tid != 0 && (elem_tid != item_tid || elem_uid != item_uid)) {
                char exp_t[32], got_t[32];
                sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                sym_type_name(tab, mem, item_tid, got_t, sizeof(got_t));
                mem_err(mem, "Vector type mismatch: cannot set '%s' in vec<%s>", got_t, exp_t);
                return val_void();
            }
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            delegate_vec_write_elem(mem, tab, elem_tid, data_idx, (uint32_t)idx * (uint32_t)stride, c);
            return a;
        }

        /*Below section is the C's native for operator delegate @vresize*/
        /*Construct resize(v, new_len)*/
        case INTR_VRESIZE: {
            int new_len_in = 0;
            if (!delegate_val_to_exact_int(mem, tab, b, "vector resize length", &new_len_in)) return val_void();
            if (new_len_in < 0 || (uint32_t)new_len_in > MEM_MAX_HEAP_SLOTS) {
                mem_err(mem, "Vector resize length out of range (%d)", new_len_in);
                return val_void();
            }
            uint32_t new_len = (uint32_t)new_len_in;
            if (elem_tid == 0) {
                elem_tid = mem->fast_real_type;
                stride   = 1;
                mem_write_pool(mem, hdr_idx + 0, (double)elem_tid);
            }
            if (new_len > len) {
                if (!mem_vec_reserve(mem, hdr_idx, stride, new_len)) return val_void();
                uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
                double *dptr = mem_data_ptr(mem, data_idx, len * (uint32_t)stride);
                if (dptr) {
                    memset(dptr, 0, (size_t)((new_len - len) * (uint32_t)stride) * sizeof(double));
                }
            }
            mem_write_pool(mem, hdr_idx + 1, (double)new_len);
            return a;
        }
        default:
            return val_void();
    }
}

/* ============================================================================
 * 4. MULTIDIMENSIONAL MATRIX (`$mat`) DELEGATE INTRINSICS (`@mzero` .. `@minv`)
 * ============================================================================ */

/*Below section is the C's native for operator/function delegates @mzero, @mones, @meye, @mfill, @mrows, @mcols, @mrank, @mlen, @mshape, @madd, @msub, @mscal, @mmul, @mtrans, @mtrace, @mdet, @minv*/
/*Construct mat_zeros, mat_ones, mat_eye, mat_fill, mat_rows, mat_cols, mat_rank, mat_len, mat_shape, A + B, A - B, s * A, A * B, transpose(A), trace(A), det(A), inv(A)*/
bool delegate_mat_get_info(const MemArena *mem, Value v,
                           uint8_t *out_rank, uint16_t *out_total_len,
                           uint16_t *out_data_idx, uint16_t dims[4]) {
    if (mem->mat_type_id == 0 || val_get_type(mem, v) != mem->mat_type_id) {
        return false;
    }
    uint16_t hdr_idx = val_get_pool_idx(v);
    uint8_t  rank    = (uint8_t)mem_read_pool(mem, hdr_idx + 0);
    uint16_t tot_len = (uint16_t)mem_read_pool(mem, hdr_idx + 1);
    uint16_t dat_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 2);
    for (uint8_t i = 0; i < 4; ++i) {
        dims[i] = (i < rank) ? (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 4u + i)) : 0;
    }
    if (out_rank)      *out_rank      = rank;
    if (out_total_len) *out_total_len = tot_len;
    if (out_data_idx)  *out_data_idx  = dat_idx;
    return true;
}

Value delegate_eval_mat_intrinsic(MemArena *mem, const SymTable *tab,
                                  IntrinsicId intr, Value a, Value b, Value c) {
    if (mem->mat_type_id == 0) {
        mem_err(mem, "Multidimensional matrix type ($mat) is not loaded");
        return val_void();
    }

    if (intr == INTR_MRESHAPE) {
        uint16_t src_len = 0, src_dat = 0;
        uint8_t  src_uid = MEM_UNIT_NONE;
        uint8_t  ra_tmp  = 0;
        uint16_t da_tmp[4] = { 0 };
        if (delegate_mat_get_info(mem, a, &ra_tmp, &src_len, &src_dat, da_tmp)) {
            src_uid = val_get_unit(mem, a);
        } else if (mem->vec_type_id != 0 && val_get_type(mem, a) == mem->vec_type_id) {
            uint16_t hdr_a    = val_get_pool_idx(a);
            uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_a + 0);
            uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
            src_uid = (uint8_t)((raw_hdr0 >> 8u) & 0x7Fu);
            src_len = (uint16_t)mem_read_pool(mem, hdr_a + 1);
            src_dat = (uint16_t)mem_read_pool(mem, hdr_a + 3);
            if (src_len > 0 && elem_tid != mem->fast_real_type) {
                mem_err(mem, "reshape() requires a real scalar vector or matrix");
                return val_void();
            }
        } else {
            mem_err(mem, "reshape() expects a matrix ($mat) or vector ($vec)");
            return val_void();
        }

        uint8_t  out_rank = 0;
        uint16_t dims[4]  = { 0, 0, 0, 0 };
        uint16_t donor_tot = 0, donor_dat = 0;
        if (delegate_mat_get_info(mem, b, &out_rank, &donor_tot, &donor_dat, dims)) {
            if (donor_tot != src_len) {
                mem_err(mem, "Cannot reshape %u element(s) to target matrix of %u element(s)",
                        (unsigned)src_len, (unsigned)donor_tot);
                return val_void();
            }
        } else {
            int r_in = 0, c_in = 0;
            if (!delegate_val_to_exact_int(mem, tab, b, "reshape rows", &r_in) ||
                !delegate_val_to_exact_int(mem, tab, c, "reshape cols", &c_in)) {
                return val_void();
            }
            if (r_in <= 0 || c_in < 0 || r_in > 4096 || c_in > 4096) {
                mem_err(mem, "Reshape dimensions must be positive (%d x %d)", r_in, c_in);
                return val_void();
            }
            uint32_t eff_cols = (c_in == 0) ? 1u : (uint32_t)c_in;
            if ((uint32_t)r_in * eff_cols != (uint32_t)src_len) {
                mem_err(mem, "Cannot reshape %u element(s) into %d x %d matrix",
                        (unsigned)src_len, r_in, c_in);
                return val_void();
            }
            out_rank = (c_in == 0) ? 1u : 2u;
            dims[0]  = (uint16_t)r_in;
            dims[1]  = (uint16_t)c_in;
        }
        uint16_t tot_out = 0, dat_out = 0;
        uint16_t hdr_out = mem_mat_alloc_hdr(mem, out_rank, dims, &tot_out, &dat_out);
        if (hdr_out == MEM_NO_IDX || mem->has_error) return val_void();
        if (src_len > 0) {
            const double *sptr = mem_data_ptr_ro(mem, src_dat, 0);
            double       *dptr = mem_data_ptr(mem, dat_out, 0);
            if (sptr && dptr) memcpy(dptr, sptr, (size_t)src_len * sizeof(double));
        }
        return val_with_unit(mem, val_make_comp(mem, mem->mat_type_id, hdr_out, MEM_MAT_HDR_SLOTS), src_uid);
    }

    uint8_t  ra = 0;
    uint16_t tot_a = 0, dat_a = 0, da[4] = { 0 };
    if (!delegate_mat_get_info(mem, a, &ra, &tot_a, &dat_a, da)) {
        mem_err(mem, "Expected matrix ($mat) argument");
        return val_void();
    }
    switch (intr) {
        case INTR_MRANK:
            return val_make_f64(mem, mem->fast_real_type, (double)ra);
        case INTR_MLEN:
            return val_make_f64(mem, mem->fast_real_type, (double)tot_a);
        case INTR_MDIM: {
            int ax = 0;
            if (!delegate_val_to_exact_int(mem, tab, b, "matrix dimension axis", &ax)) {
                return val_void();
            }
            if (ax < 0) ax += (int)ra;
            if (ax < 0 || ax >= (int)ra) {
                mem_err(mem, "Matrix axis %d out of bounds for rank %u", ax, (unsigned)ra);
                return val_void();
            }
            return val_make_f64(mem, mem->fast_real_type, (double)da[ax]);
        }
        default:
            return val_void();
    }
}

/* ============================================================================
 * 4b. STRING CONVERSION DELEGATES `@tostr` (POSTFIX `_`) & `@fromstr` (PREFIX `_`)
 * ============================================================================ */

/*Below section is the C's native for operator delegates @tostr (postfix "_") and @fromstr (prefix "_")*/
/*Construct x_ (plain string representation of any value/type) and _s (convert plain string into most appropriate type)*/
static void delegate_buf_append(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
    if (!buf || cap == 0 || *used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= cap - *used) {
        *used = cap - 1;
    } else {
        *used += (size_t)n;
    }
}

static void delegate_format_struct_at(const MemArena *mem, const SymTable *tab,
                                      uint16_t type_id, uint16_t pool_idx,
                                      uint8_t depth, char *buf, size_t cap);

static void delegate_format_mat_slice(const MemArena *mem, uint16_t data_idx, uint32_t base_off,
                                      const uint16_t *dims, uint8_t rank, uint8_t axis,
                                      bool high_prec, char *buf, size_t cap, size_t *used) {
    delegate_buf_append(buf, cap, used, "[");
    if (axis + 1 >= rank) {
        uint16_t n = dims[axis];
        for (uint16_t i = 0; i < n && *used + 1 < cap; ++i) {
            double v = mem_data_read(mem, data_idx, base_off + i);
            if (v == 0.0) v = 0.0;
            delegate_buf_append(buf, cap, used,
                                high_prec ? "%s%.17g" : "%s%.6g",
                                (i > 0 ? ", " : ""), v);
        }
    } else {
        uint32_t stride = 1;
        for (uint8_t a = (uint8_t)(axis + 1); a < rank; ++a) {
            stride *= (uint32_t)dims[a];
        }
        uint16_t n = dims[axis];
        for (uint16_t i = 0; i < n && *used + 1 < cap; ++i) {
            if (i > 0) {
                delegate_buf_append(buf, cap, used, ", ");
            }
            delegate_format_mat_slice(mem, data_idx, base_off + (uint32_t)i * stride,
                                      dims, rank, (uint8_t)(axis + 1), high_prec, buf, cap, used);
        }
    }
    delegate_buf_append(buf, cap, used, "]");
}

void delegate_format_mat_at(const MemArena *mem, uint16_t hdr_idx, bool high_prec,
                            char *buf, size_t cap) {
    if (!mem || !buf || cap == 0) return;
    uint8_t  rank     = (uint8_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 0));
    uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 2));
    uint16_t dims[4]  = { 0, 0, 0, 0 };
    for (uint8_t i = 0; i < 4; ++i) {
        dims[i] = (i < rank) ? (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 4u + i)) : 0;
    }
    if (rank == 0) {
        snprintf(buf, cap, "[]");
        return;
    }
    size_t used = 0;
    buf[0] = '\0';
    delegate_format_mat_slice(mem, data_idx, 0, dims, rank, 0, high_prec, buf, cap, &used);
}

static void delegate_format_vec_at(const MemArena *mem, const SymTable *tab,
                                   uint16_t hdr_idx, uint8_t depth,
                                   char *buf, size_t cap) {
    if (depth > 16 || cap == 0) {
        if (cap > 0) snprintf(buf, cap, "[...]");
        return;
    }
    uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
    uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
    uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
    uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
    uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                        ? tab->types[elem_tid].flat_size : 1;

    size_t used = 0;
    buf[0] = '\0';
    delegate_buf_append(buf, cap, &used, "[");
    for (uint32_t i = 0; i < len && used + 1 < cap; ++i) {
        uint32_t off = i * (uint32_t)stride;
        char elem_str[96];
        if (elem_tid == SYM_TYPE_ANY) {
            double raw_d = mem_data_read(mem, data_idx, off);
            Value ev = 0;
            memcpy(&ev, &raw_d, sizeof(Value));
            uint16_t etid = val_get_type(mem, ev);
            if (etid == mem->str_type_id && etid != 0) {
                StrSlice s = val_get_str(mem, ev);
                snprintf(elem_str, sizeof(elem_str), "\"%.*s\"", (int)s.len, mem_str_ptr(mem, s));
            } else if (etid > 0 && etid < tab->type_count && !tab->types[etid].is_consuming) {
                delegate_format_struct_at(mem, tab, etid, val_get_pool_idx(ev), (uint8_t)(depth + 1), elem_str, sizeof(elem_str));
            } else {
                snprintf(elem_str, sizeof(elem_str), "%.6g", val_get_f64(mem, ev));
            }
        } else if (elem_tid == mem->str_type_id && elem_tid != 0) {
            uint32_t packed = (uint32_t)mem_data_read(mem, data_idx, off);
            StrSlice s = { (uint16_t)(packed >> 16), (uint16_t)(packed & 0xFFFFu) };
            snprintf(elem_str, sizeof(elem_str), "\"%.*s\"",
                     (int)s.len, mem_str_ptr(mem, s));
        } else if (elem_tid == mem->vec_type_id && elem_tid != 0) {
            delegate_format_vec_at(mem, tab, (uint16_t)(data_idx + off), (uint8_t)(depth + 1), elem_str, sizeof(elem_str));
        } else if (elem_tid > 0 && elem_tid < tab->type_count &&
                   !tab->types[elem_tid].is_consuming) {
            delegate_format_struct_at(mem, tab, elem_tid, (uint16_t)(data_idx + off), (uint8_t)(depth + 1), elem_str, sizeof(elem_str));
        } else {
            snprintf(elem_str, sizeof(elem_str), "%.6g", mem_data_read(mem, data_idx, off));
        }
        delegate_buf_append(buf, cap, &used, "%s%s", (i > 0 ? ", " : ""), elem_str);
    }
    delegate_buf_append(buf, cap, &used, "]");
}

static void delegate_format_struct_at(const MemArena *mem, const SymTable *tab,
                                      uint16_t type_id, uint16_t pool_idx,
                                      uint8_t depth, char *buf, size_t cap) {
    if (depth > 16 || cap == 0) {
        if (cap > 0) snprintf(buf, cap, "<...>");
        return;
    }
    if (type_id == mem->vec_type_id && type_id != 0) {
        delegate_format_vec_at(mem, tab, pool_idx, depth, buf, cap);
        return;
    }
    if (type_id == mem->mat_type_id && type_id != 0) {
        delegate_format_mat_at(mem, pool_idx, false, buf, cap);
        return;
    }
    if (type_id == mem->fn_type_id && type_id != 0) {
        unsigned pcnt = (unsigned)mem_read_pool(mem, pool_idx + 1) & 0x3Fu;
        snprintf(buf, cap, "<fn/%u>", pcnt);
        return;
    }
    if (type_id == mem->map_type_id && type_id != 0) {
        uint16_t len      = (uint16_t)mem_read_pool(mem, pool_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, pool_idx + 3);
        size_t used = 0;
        buf[0] = '\0';
        delegate_buf_append(buf, cap, &used, "{");
        for (uint16_t i = 0; i < len && used + 1 < cap; ++i) {
            StrSlice kslice = mem_map_entry_key(mem, data_idx, i);
            Value    vval   = mem_map_entry_val(mem, data_idx, i);
            char vstr[96];
            uint16_t vtid = val_get_type(mem, vval);
            if (vtid == mem->str_type_id && vtid != 0) {
                StrSlice vs = val_get_str(mem, vval);
                snprintf(vstr, sizeof(vstr), "\"%.*s\"", (int)vs.len, mem_str_ptr(mem, vs));
            } else if (vtid > 0 && vtid < tab->type_count && !tab->types[vtid].is_consuming) {
                delegate_format_struct_at(mem, tab, vtid, val_get_pool_idx(vval), (uint8_t)(depth + 1), vstr, sizeof(vstr));
            } else {
                snprintf(vstr, sizeof(vstr), "%.6g", val_get_f64(mem, vval));
            }
            delegate_buf_append(buf, cap, &used, "%s\"%.*s\": %s",
                                (i > 0 ? ", " : ""),
                                (int)kslice.len, mem_str_ptr(mem, kslice), vstr);
        }
        delegate_buf_append(buf, cap, &used, "}");
        return;
    }

    const TypeDef *td = &tab->types[type_id];
    char tname[32];
    sym_type_name(tab, mem, type_id, tname, sizeof(tname));

    if (td->parser_intr == INTR_COMPLEX_TYPE && td->flat_size == 2) {
        double re = mem_read_pool(mem, pool_idx);
        double im = mem_read_pool(mem, pool_idx + 1);
        if (im >= 0.0) {
            snprintf(buf, cap, "%.6g + %.6gi", re, im);
        } else {
            snprintf(buf, cap, "%.6g - %.6gi", re, -im);
        }
        return;
    }

    size_t used = 0;
    buf[0] = '\0';
    delegate_buf_append(buf, cap, &used, "%s(", tname);
    for (uint8_t i = 0; i < td->field_count && used + 1 < cap; ++i) {
        const FieldDef *fd = &tab->fields[td->start_idx + i];
        char f_str[64];
        if (!tab->types[fd->type_id].is_consuming) {
            delegate_format_struct_at(mem, tab, fd->type_id, (uint16_t)(pool_idx + fd->flat_offset),
                                      (uint8_t)(depth + 1), f_str, sizeof(f_str));
        } else if (fd->type_id == mem->str_type_id && mem->str_type_id != 0) {
            uint32_t packed = (uint32_t)mem_read_pool(mem, (uint16_t)(pool_idx + fd->flat_offset));
            StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
            snprintf(f_str, sizeof(f_str), "\"%.*s\"", (int)s.len, mem_str_ptr(mem, s));
        } else {
            snprintf(f_str, sizeof(f_str), "%.6g", mem_read_pool(mem, (uint16_t)(pool_idx + fd->flat_offset)));
        }
        delegate_buf_append(buf, cap, &used, "%s%.*s=%s",
                            (i > 0 ? ", " : ""),
                            (int)fd->name.len, mem_str_ptr(mem, fd->name),
                            f_str);
    }
    delegate_buf_append(buf, cap, &used, ")");
}

void delegate_format_value(const MemArena *mem, const SymTable *tab,
                           Value val, char *buf, size_t buf_cap) {
    if (!mem || !tab || !buf || buf_cap == 0) return;
    uint16_t tid = val_get_type(mem, val);
    if (tid == 0 || tid >= tab->type_count) {
        snprintf(buf, buf_cap, "<void>");
        return;
    }
    const TypeDef *td = &tab->types[tid];
    if (td->is_consuming) {
        if (td->parser_intr == INTR_PARSE_STR_SLICE) {
            StrSlice s = val_get_str(mem, val);
            snprintf(buf, buf_cap, "\"%.*s\"", (int)s.len, mem_str_ptr(mem, s));
        } else if (td->parser_intr == INTR_PARSE_F64_TRIM1) {
            snprintf(buf, buf_cap, "%.6gi", val_get_f64(mem, val));
        } else {
            snprintf(buf, buf_cap, "%.10g", val_get_f64(mem, val));
        }
    } else {
        delegate_format_struct_at(mem, tab, tid, val_get_pool_idx(val), 0, buf, buf_cap);
    }
}

