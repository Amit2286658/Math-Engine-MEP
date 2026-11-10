/*
 * eval_index.c — Container/Struct Indexing, Slicing & Matrix Literal Evaluation
 */
#include "eval_internal.h"

Value eval_index_op(MemArena *mem, SymTable *tab,
                           const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type,
                           bool is_write, Value rhs) {
    Value base = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    if (is_write && mem_val_is_immutable(mem, base)) {
        mem_err(mem, "Cannot mutate immutable constant container");
        return val_void();
    }

    uint8_t argc = n->sub;
    Value   iv_args[4] = { 0 };
    bool    has_slice_arg = false;
    uint16_t cur = n->right;
    for (uint8_t i = 0; i < argc && cur != MEM_NO_IDX; ++i) {
        const AstNode *link = mem_get_ast(mem, cur);
        iv_args[i] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
        uint16_t itid = val_get_type(mem, iv_args[i]);
        if ((itid == mem->vec_type_id && mem->vec_type_id != 0) ||
            (itid == mem->mat_type_id && mem->mat_type_id != 0)) {
            has_slice_arg = true;
        }
        cur = link->right;
    }

    uint16_t base_tid  = val_get_type(mem, base);
    uint8_t  base_unit = val_get_unit(mem, base);

    /* A0. String (`$str`) Indexing & Substring Slicing -> Delegate to `@vget` (`INTR_VGET`) */
    if (base_tid == mem->str_type_id && mem->str_type_id != 0) {
        if (is_write) {
            mem_err(mem, "Strings are immutable and do not support index assignment");
            return val_void();
        }
        if (argc != 1) {
            mem_err(mem, "String subscript expects a single index or range slice");
            return val_void();
        }
        if (has_slice_arg) {
            int at_sid = sym_find(tab, mem, "at", 2);
            if (at_sid >= 0) {
                Value at_args[2] = { base, iv_args[0] };
                return eval_dispatch_overload(mem, tab, (uint16_t)at_sid, FIX_FUNC, 2, at_args);
            }
        }
        return delegate_eval_vec_intrinsic(mem, tab, INTR_VGET, base, iv_args[0], val_void());
    }

    /* 0b. Dynamic Map (`$map`) Subscript (`m["key"]`, `m["key"] = val`, `m["key"] += val`) */
    if (base_tid == mem->map_type_id && mem->map_type_id != 0) {
        if (argc != 1) {
            mem_err(mem, "Map subscript expects a single key");
            return val_void();
        }
        if (!is_write) {
            return delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_GET, base, iv_args[0], val_void());
        } else {
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_SET, base, iv_args[0], rhs);
            return rhs;
        }
    }

    /* A. Dynamic Vector (`$vec`) Slicing (`v[1..3]`, `v[1...3]`, `v[vec(0, 2)]`) */
    if (has_slice_arg && base_tid == mem->vec_type_id && mem->vec_type_id != 0 && argc == 1) {
        uint16_t hdr_idx  = val_get_pool_idx(base);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1;

        uint16_t slice_idx[128];
        uint16_t slice_cnt = 0;
        if (!delegate_extract_slice_indices(mem, tab, iv_args[0], (int)len, slice_idx, &slice_cnt, 128)) {
            return val_void();
        }

        if (!is_write) {
            uint16_t out_hdr = mem_vec_alloc_hdr(mem, elem_tid, slice_cnt);
            if (mem->has_error) return val_void();
            mem_write_pool(mem, out_hdr + 0, (double)(raw_hdr0 & (uint16_t)~MEM_HDR_IMMUTABLE_BIT));
            if (slice_cnt > 0) {
                if (!mem_vec_reserve(mem, out_hdr, stride, slice_cnt)) return val_void();
                uint16_t out_data = (uint16_t)mem_read_pool(mem, out_hdr + 3);
                for (uint16_t i = 0; i < slice_cnt; ++i) {
                    Value item = delegate_vec_read_elem(mem, tab, elem_tid, elem_uid,
                                                        data_idx, (uint32_t)slice_idx[i] * (uint32_t)stride);
                    delegate_vec_write_elem(mem, tab, elem_tid, out_data, (uint32_t)i * (uint32_t)stride, item);
                }
                mem_write_pool(mem, out_hdr + 1, (double)slice_cnt);
            }
            return val_make_comp(mem, mem->vec_type_id, out_hdr, MEM_VEC_HDR_SLOTS);
        } else {
            uint16_t rhs_tid = val_get_type(mem, rhs);
            if (rhs_tid == mem->vec_type_id && mem->vec_type_id != 0) {
                uint16_t rhdr = val_get_pool_idx(rhs);
                uint16_t rraw = (uint16_t)mem_read_pool(mem, rhdr + 0);
                uint16_t rtid = (uint16_t)(rraw & 0xFFu);
                uint8_t  ruid = (uint8_t)((rraw >> 8) & 0x7Fu);
                uint32_t rlen = (uint32_t)mem_read_pool(mem, rhdr + 1);
                uint16_t rdat = (uint16_t)mem_read_pool(mem, rhdr + 3);
                if (rlen != slice_cnt) {
                    mem_err(mem, "Vector slice assignment length mismatch (%u vs %u)",
                            (unsigned)slice_cnt, (unsigned)rlen);
                    return val_void();
                }
                if (elem_tid != 0 && elem_tid != SYM_TYPE_ANY && (elem_tid != rtid || elem_uid != ruid)) {
                    char exp_t[32], got_t[32];
                    sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                    sym_type_name(tab, mem, rtid, got_t, sizeof(got_t));
                    mem_err(mem, "Vector type mismatch: cannot set %s in vec<%s>", got_t, exp_t);
                    return val_void();
                }
                for (uint16_t i = 0; i < slice_cnt; ++i) {
                    Value item = delegate_vec_read_elem(mem, tab, rtid, ruid, rdat, (uint32_t)i * (uint32_t)stride);
                    delegate_vec_write_elem(mem, tab, elem_tid,
                                            data_idx, (uint32_t)slice_idx[i] * (uint32_t)stride, item);
                }
            } else {
                uint8_t rhs_uid = val_get_unit(mem, rhs);
                if (elem_tid != 0 && elem_tid != SYM_TYPE_ANY && (elem_tid != rhs_tid || elem_uid != rhs_uid)) {
                    char exp_t[32], got_t[32];
                    sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                    sym_type_name(tab, mem, rhs_tid, got_t, sizeof(got_t));
                    mem_err(mem, "Vector type mismatch: cannot set %s in vec<%s>", got_t, exp_t);
                    return val_void();
                }
                for (uint16_t i = 0; i < slice_cnt; ++i) {
                    delegate_vec_write_elem(mem, tab, elem_tid,
                                            data_idx, (uint32_t)slice_idx[i] * (uint32_t)stride, rhs);
                }
            }
            return rhs;
        }
    }

    /* B. Matrix (`$mat`) Slicing (`M[0..1, 1..2]`, `M[0..1]`, `M[1, 0..2]`) */
    if (has_slice_arg && base_tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, base, &rank, &tot_len, &data_idx, dims);
        if (argc == 0 || argc > rank) {
            mem_err(mem, "Matrix subscript has %u indices, but matrix rank is %u",
                    (unsigned)argc, (unsigned)rank);
            return val_void();
        }

        uint16_t ax_idx[4][64];
        uint16_t ax_cnt[4]     = { 1, 1, 1, 1 };
        bool     ax_is_slc[4]  = { false, false, false, false };
        uint16_t ax_stride[4]  = { 1, 1, 1, 1 };

        for (int a = (int)rank - 2; a >= 0; --a) {
            ax_stride[a] = (uint16_t)(ax_stride[a + 1] * dims[a + 1]);
        }

        for (uint8_t a = 0; a < rank; ++a) {
            if (a < argc) {
                uint16_t itid = val_get_type(mem, iv_args[a]);
                if ((itid == mem->vec_type_id && mem->vec_type_id != 0) ||
                    (itid == mem->mat_type_id && mem->mat_type_id != 0)) {
                    if (!delegate_extract_slice_indices(mem, tab, iv_args[a], (int)dims[a],
                                               ax_idx[a], &ax_cnt[a], 64)) {
                        return val_void();
                    }
                    ax_is_slc[a] = true;
                } else {
                    int k = 0;
                    if (!delegate_val_to_exact_int(mem, tab, iv_args[a], "subscript index", &k)) {
                        return val_void();
                    }
                    if (k < 0) k += (int)dims[a];
                    if (k < 0 || k >= (int)dims[a]) {
                        mem_err(mem, "Matrix index %d out of bounds on axis %u (size=%u)",
                                k, (unsigned)a, (unsigned)dims[a]);
                        return val_void();
                    }
                    ax_idx[a][0] = (uint16_t)k;
                    ax_cnt[a]    = 1;
                    ax_is_slc[a] = false;
                }
            } else {
                ax_cnt[a]    = dims[a];
                ax_is_slc[a] = true;
                for (uint16_t k = 0; k < dims[a] && k < 64; ++k) {
                    ax_idx[a][k] = k;
                }
            }
        }

        uint16_t out_dims[4] = { 0, 0, 0, 0 };
        uint8_t  out_rank    = 0;
        for (uint8_t a = 0; a < rank; ++a) {
            if (ax_is_slc[a]) {
                out_dims[out_rank++] = ax_cnt[a];
            }
        }
        if (out_rank == 0) {
            out_rank    = 1;
            out_dims[0] = 1;
        }

        if (!is_write) {
            uint16_t out_tot = 0, out_data = 0;
            uint16_t out_hdr = mem_mat_alloc_hdr(mem, out_rank, out_dims, &out_tot, &out_data);
            if (mem->has_error) return val_void();

            uint16_t dst = 0;
            for (uint16_t i0 = 0; i0 < ax_cnt[0]; ++i0) {
                uint16_t off0 = (uint16_t)(ax_idx[0][i0] * ax_stride[0]);
                for (uint16_t i1 = 0; i1 < (rank > 1 ? ax_cnt[1] : 1); ++i1) {
                    uint16_t off1 = (uint16_t)(off0 + (rank > 1 ? ax_idx[1][i1] * ax_stride[1] : 0));
                    for (uint16_t i2 = 0; i2 < (rank > 2 ? ax_cnt[2] : 1); ++i2) {
                        uint16_t off2 = (uint16_t)(off1 + (rank > 2 ? ax_idx[2][i2] * ax_stride[2] : 0));
                        for (uint16_t i3 = 0; i3 < (rank > 3 ? ax_cnt[3] : 1); ++i3) {
                            uint16_t src_off = (uint16_t)(off2 + (rank > 3 ? ax_idx[3][i3] : 0));
                            mem_data_write(mem, out_data, dst++,
                                           mem_data_read(mem, data_idx, src_off));
                        }
                    }
                }
            }
            return val_with_unit(mem, val_make_comp(mem, mem->mat_type_id, out_hdr, MEM_MAT_HDR_SLOTS), base_unit);
        } else {
            double fill_val = val_get_f64(mem, rhs);
            for (uint16_t i0 = 0; i0 < ax_cnt[0]; ++i0) {
                uint16_t off0 = (uint16_t)(ax_idx[0][i0] * ax_stride[0]);
                for (uint16_t i1 = 0; i1 < (rank > 1 ? ax_cnt[1] : 1); ++i1) {
                    uint16_t off1 = (uint16_t)(off0 + (rank > 1 ? ax_idx[1][i1] * ax_stride[1] : 0));
                    for (uint16_t i2 = 0; i2 < (rank > 2 ? ax_cnt[2] : 1); ++i2) {
                        uint16_t off2 = (uint16_t)(off1 + (rank > 2 ? ax_idx[2][i2] * ax_stride[2] : 0));
                        for (uint16_t i3 = 0; i3 < (rank > 3 ? ax_cnt[3] : 1); ++i3) {
                            uint16_t dst_off = (uint16_t)(off2 + (rank > 3 ? ax_idx[3][i3] : 0));
                            mem_data_write(mem, data_idx, dst_off, fill_val);
                        }
                    }
                }
            }
            return rhs;
        }
    }

    /* C. Standard Scalar Subscript Indexing */
    int idx_k[4] = { 0, 0, 0, 0 };
    for (uint8_t i = 0; i < argc; ++i) {
        if (!delegate_val_to_exact_int(mem, tab, iv_args[i], "subscript index", &idx_k[i])) return val_void();
    }

    if (base_tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, base, &rank, &tot_len, &data_idx, dims);

        if (argc == 0 || argc > rank) {
            mem_err(mem, "Matrix subscript has %u indices, but matrix rank is %u",
                    (unsigned)argc, (unsigned)rank);
            return val_void();
        }

        uint32_t prefix_off = 0;
        for (uint8_t a = 0; a < argc; ++a) {
            int dim_a = (int)dims[a];
            int ka    = idx_k[a];
            if (ka < 0) ka += dim_a;
            if (ka < 0 || ka >= dim_a) {
                mem_err(mem, "Matrix index %d out of bounds on axis %u (size=%d)",
                        idx_k[a], (unsigned)a, dim_a);
                return val_void();
            }
            prefix_off = prefix_off * (uint32_t)dim_a + (uint32_t)ka;
        }

        uint32_t sub_len = 1;
        for (uint8_t a = argc; a < rank; ++a) {
            sub_len *= (uint32_t)dims[a];
        }
        uint32_t elem_off = prefix_off * sub_len;

        if (argc == rank) {
            if (!is_write) {
                return val_make_f64_unit(mem, mem->fast_real_type, base_unit,
                                         mem_data_read(mem, data_idx, elem_off));
            } else {
                mem_data_write(mem, data_idx, elem_off, val_get_f64(mem, rhs));
                return rhs;
            }
        } else {
            uint8_t sub_rank = (uint8_t)(rank - argc);
            if (!is_write) {
                uint16_t sub_dims[4] = { 0, 0, 0, 0 };
                for (uint8_t i = 0; i < sub_rank && i < 4; ++i) {
                    sub_dims[i] = dims[argc + i];
                }
                if (mem_is_heap_idx(data_idx)) {
                    uint16_t sub_tot = 0, sub_dat = 0;
                    uint16_t view_hdr = mem_mat_alloc_hdr(mem, sub_rank, sub_dims, &sub_tot, &sub_dat);
                    if (mem->has_error) return val_void();
                    for (uint32_t i = 0; i < sub_len; ++i) {
                        mem_data_write(mem, sub_dat, i, mem_data_read(mem, data_idx, elem_off + i));
                    }
                    return val_with_unit(mem,
                                         val_make_comp(mem, mem->mat_type_id, view_hdr, MEM_MAT_HDR_SLOTS),
                                         base_unit);
                }
                uint16_t target_data_idx = (uint16_t)(data_idx + elem_off);
                uint16_t view_hdr = mem_alloc_pool(mem, MEM_MAT_HDR_SLOTS);
                if (mem->has_error) return val_void();
                mem_write_pool(mem, view_hdr + 0, (double)sub_rank);
                mem_write_pool(mem, view_hdr + 1, (double)sub_len);
                mem_write_pool(mem, view_hdr + 2, (double)target_data_idx);
                mem_write_pool(mem, view_hdr + 3, (double)mem->fast_real_type);
                for (uint8_t i = 0; i < 4; ++i) {
                    mem_write_pool(mem, (uint16_t)(view_hdr + 4u + i),
                                   (i < sub_rank) ? (double)dims[argc + i] : 0.0);
                }
                return val_with_unit(mem,
                                     val_make_comp(mem, mem->mat_type_id, view_hdr, MEM_MAT_HDR_SLOTS),
                                     base_unit);
            } else {
                uint8_t  rhs_rank = 0;
                uint16_t rhs_tot = 0, rhs_dat = 0, rhs_dims[4] = { 0 };
                if (!delegate_mat_get_info(mem, rhs, &rhs_rank, &rhs_tot, &rhs_dat, rhs_dims) ||
                    rhs_rank != sub_rank || rhs_tot != sub_len) {
                    mem_err(mem, "Sub-matrix assignment shape mismatch");
                    return val_void();
                }
                for (uint32_t i = 0; i < sub_len; ++i) {
                    mem_data_write(mem, data_idx, elem_off + i, mem_data_read(mem, rhs_dat, i));
                }
                return rhs;
            }
        }
    }

    if (base_tid == mem->vec_type_id && mem->vec_type_id != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(base);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1;

        int k = idx_k[0];
        if (k < 0) k += (int)len;
        if (k < 0 || k >= (int)len) {
            mem_err(mem, "Vector index %d out of bounds (len=%u)", idx_k[0], (unsigned)len);
            return val_void();
        }
        uint32_t elem_off  = (uint32_t)k * (uint32_t)stride;
        uint16_t elem_pidx = (uint16_t)(data_idx + elem_off);

        if (argc == 1) {
            if (!is_write) {
                return delegate_vec_read_elem(mem, tab, elem_tid, elem_uid, data_idx, elem_off);
            } else {
                uint16_t item_tid = val_get_type(mem, rhs);
                uint8_t  item_uid = val_get_unit(mem, rhs);
                if (elem_tid != 0 && elem_tid != SYM_TYPE_ANY && (elem_tid != item_tid || elem_uid != item_uid)) {
                    char exp_t[32], got_t[32];
                    sym_type_name(tab, mem, elem_tid, exp_t, sizeof(exp_t));
                    sym_type_name(tab, mem, item_tid, got_t, sizeof(got_t));
                    mem_err(mem, "Vector type mismatch: cannot set '%s' in vec<%s>", got_t, exp_t);
                    return val_void();
                }
                delegate_vec_write_elem(mem, tab, elem_tid, data_idx, elem_off, rhs);
                return rhs;
            }
        }
        base_tid  = elem_tid;
        base_unit = elem_uid;
        uint16_t cur_pidx = elem_pidx;
        uint16_t cur_tid  = base_tid;
        for (uint8_t a = 1; a < argc; ++a) {
            if (cur_tid == 0 || cur_tid >= tab->type_count ||
                tab->types[cur_tid].is_consuming || tab->types[cur_tid].field_count == 0) {
                mem_err(mem, "Cannot subscript scalar vector element");
                return val_void();
            }
            const TypeDef *td = &tab->types[cur_tid];
            int fk = idx_k[a];
            if (fk < 0) fk += (int)td->field_count;
            if (fk < 0 || fk >= (int)td->field_count) {
                mem_err(mem, "Struct/vector field index %d out of bounds (fields=%u)",
                        idx_k[a], (unsigned)td->field_count);
                return val_void();
            }
            const FieldDef *fd = &tab->fields[td->start_idx + (uint16_t)fk];
            cur_pidx = (uint16_t)(cur_pidx + fd->flat_offset);
            cur_tid  = sym_unpack_outer_type(mem, fd->type_id);
        }
        if (!is_write) {
            if (!tab->types[cur_tid].is_consuming) {
                return val_with_unit(mem, val_make_comp(mem, cur_tid, cur_pidx, tab->types[cur_tid].flat_size),
                                     base_unit);
            }
            return val_make_f64_unit(mem, cur_tid ? cur_tid : mem->fast_real_type, base_unit,
                                     mem_read_pool(mem, cur_pidx));
        } else {
            mem_write_pool(mem, cur_pidx, val_get_f64(mem, rhs));
            return rhs;
        }
    }

    if (base_tid > 0 && base_tid < tab->type_count && !tab->types[base_tid].is_consuming) {
        uint16_t cur_pidx = val_get_pool_idx(base);
        uint16_t cur_tid  = base_tid;
        for (uint8_t a = 0; a < argc; ++a) {
            if (cur_tid == 0 || cur_tid >= tab->type_count ||
                tab->types[cur_tid].is_consuming || tab->types[cur_tid].field_count == 0) {
                mem_err(mem, "Cannot subscript scalar field with '[...]'");
                return val_void();
            }
            const TypeDef *td = &tab->types[cur_tid];
            int fk = idx_k[a];
            if (fk < 0) fk += (int)td->field_count;
            if (fk < 0 || fk >= (int)td->field_count) {
                mem_err(mem, "Vector/struct index %d out of bounds (fields=%u)",
                        idx_k[a], (unsigned)td->field_count);
                return val_void();
            }
            const FieldDef *fd = &tab->fields[td->start_idx + (uint16_t)fk];
            cur_pidx = (uint16_t)(cur_pidx + fd->flat_offset);
            cur_tid  = sym_unpack_outer_type(mem, fd->type_id);
        }

        if (!is_write) {
            if (cur_tid != 0 && cur_tid < tab->type_count && !tab->types[cur_tid].is_consuming) {
                return val_with_unit(mem,
                                     val_make_comp(mem, cur_tid, cur_pidx, tab->types[cur_tid].flat_size),
                                     base_unit);
            }
            if (cur_tid == mem->str_type_id && mem->str_type_id != 0) {
                uint32_t packed = (uint32_t)mem_read_pool(mem, cur_pidx);
                StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
                return val_make_str(mem, cur_tid, s);
            }
            return val_make_f64_unit(mem, cur_tid ? cur_tid : mem->fast_real_type, base_unit,
                                     mem_read_pool(mem, cur_pidx));
        } else {
            if (cur_tid != 0 && cur_tid < tab->type_count && !tab->types[cur_tid].is_consuming) {
                uint16_t fsize   = tab->types[cur_tid].flat_size;
                uint16_t src_idx = val_get_pool_idx(rhs);
                for (uint16_t i = 0; i < fsize; ++i) {
                    mem_write_pool(mem, cur_pidx + i, mem_read_pool(mem, src_idx + i));
                }
            } else if (cur_tid == mem->str_type_id && mem->str_type_id != 0) {
                StrSlice s = val_get_str(mem, rhs);
                mem_write_pool(mem, cur_pidx, (double)(((uint32_t)s.off << 16u) | (uint32_t)s.len));
            } else {
                mem_write_pool(mem, cur_pidx, val_get_f64(mem, rhs));
            }
            return rhs;
        }
    }

    mem_err(mem, "Cannot subscript non-indexable type with '[...]'");
    return val_void();
}

Value eval_mat_lit_ast(MemArena *mem, SymTable *tab,
                              const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type) {
    if (mem->mat_type_id == 0) {
        mem_err(mem, "Multidimensional matrix type ($mat) is not loaded");
        return val_void();
    }
    uint8_t count = n->sub;
    if (count == 0) {
        uint16_t hdr_idx = mem_alloc_pool(mem, MEM_MAT_HDR_SLOTS);
        if (mem->has_error) return val_void();
        for (uint8_t i = 0; i < MEM_MAT_HDR_SLOTS; ++i) mem_write_pool(mem, hdr_idx + i, 0.0);
        mem_write_pool(mem, hdr_idx + 0, 1.0);
        return val_make_comp(mem, mem->mat_type_id, hdr_idx, MEM_MAT_HDR_SLOTS);
    }

    Value items[64];
    memset(items, 0, sizeof(items));
    uint8_t actual = 0;
    uint16_t cur = n->left;
    while (cur != MEM_NO_IDX && actual < 64) {
        const AstNode *link = mem_get_ast(mem, cur);
        items[actual] = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
        actual++;
        cur = link->right;
    }
    if (actual == 0) return val_void();

    uint16_t first_tid = val_get_type(mem, items[0]);
    uint8_t  common_u  = val_get_unit(mem, items[0]);

    if (first_tid == mem->mat_type_id) {
        uint8_t  sub_rank = 0;
        uint16_t sub_tot = 0, sub_dat0 = 0, sub_dims[4] = { 0 };
        delegate_mat_get_info(mem, items[0], &sub_rank, &sub_tot, &sub_dat0, sub_dims);
        if (sub_rank >= 4) {
            mem_err(mem, "Maximum matrix rank (4D) exceeded");
            return val_void();
        }
        for (uint8_t i = 1; i < actual; ++i) {
            uint8_t  r_i = 0;
            uint16_t t_i = 0, d_i = 0, dims_i[4] = { 0 };
            if (!delegate_mat_get_info(mem, items[i], &r_i, &t_i, &d_i, dims_i) || r_i != sub_rank) {
                mem_err(mem, "Matrix rows/sub-tensors must have matching dimensions");
                return val_void();
            }
            if (val_get_unit(mem, items[i]) != common_u) {
                char exp_u[32], got_u[32];
                sym_unit_name(tab, mem, common_u, exp_u, sizeof(exp_u));
                sym_unit_name(tab, mem, val_get_unit(mem, items[i]), got_u, sizeof(got_u));
                mem_err(mem, "Matrix literal unit mismatch: expected '%s', got '%s'", exp_u, got_u);
                return val_void();
            }
            for (uint8_t k = 0; k < sub_rank; ++k) {
                if (dims_i[k] != sub_dims[k]) {
                    mem_err(mem, "Matrix rows/sub-tensors must have matching dimensions");
                    return val_void();
                }
            }
        }
        uint16_t dims[4] = { actual, 0, 0, 0 };
        for (uint8_t k = 0; k < sub_rank && k < 3; ++k) {
            dims[k + 1] = sub_dims[k];
        }
        uint16_t tot_len = 0, data_idx = 0;
        uint16_t hdr_idx = mem_mat_alloc_hdr(mem, (uint8_t)(sub_rank + 1), dims, &tot_len, &data_idx);
        if (mem->has_error) return val_void();

        for (uint8_t i = 0; i < actual; ++i) {
            uint16_t d_i = 0;
            uint16_t dummy_dims[4];
            delegate_mat_get_info(mem, items[i], NULL, NULL, &d_i, dummy_dims);
            for (uint16_t k = 0; k < sub_tot; ++k) {
                mem_data_write(mem, data_idx, (uint32_t)i * (uint32_t)sub_tot + k,
                               mem_data_read(mem, d_i, k));
            }
        }
        return val_with_unit(mem, val_make_comp(mem, mem->mat_type_id, hdr_idx, MEM_MAT_HDR_SLOTS), common_u);
    }

    bool has_non_num = false;
    for (uint8_t i = 0; i < actual; ++i) {
        uint16_t tid = val_get_type(mem, items[i]);
        uint8_t  uid = val_get_unit(mem, items[i]);
        if (tid != first_tid) {
            char exp_t[32], got_t[32];
            sym_type_name(tab, mem, first_tid, exp_t, sizeof(exp_t));
            sym_type_name(tab, mem, tid, got_t, sizeof(got_t));
            mem_err(mem, "Vector/matrix literal type mismatch: expected '%s', got '%s' (use vec<any>(...) for heterogeneous vectors)",
                    exp_t, got_t);
            return val_void();
        }
        if (uid != common_u) {
            char exp_u[32], got_u[32];
            sym_unit_name(tab, mem, common_u, exp_u, sizeof(exp_u));
            sym_unit_name(tab, mem, uid, got_u, sizeof(got_u));
            mem_err(mem, "Vector/matrix literal unit mismatch: expected '%s', got '%s'", exp_u, got_u);
            return val_void();
        }
        if (tid == 0 || tid >= tab->type_count || !tab->types[tid].is_consuming || tid == mem->str_type_id) {
            has_non_num = true;
        }
    }
    if (has_non_num) {
        if (mem->vec_type_id == 0) {
            mem_err(mem, "Matrix elements must be numeric scalars or sub-matrices");
            return val_void();
        }
        uint16_t elem_tid = first_tid;
        uint16_t stride   = (elem_tid != SYM_TYPE_ANY && elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1u;
        uint16_t saved_tr = mem->temp_root_count;
        for (uint8_t i = 0; i < actual; ++i) {
            (void)mem_push_temp_root(mem, &items[i]);
        }
        uint16_t hdr_idx = mem_vec_alloc_hdr(mem, elem_tid, actual);
        if (mem->has_error || !mem_vec_reserve(mem, hdr_idx, stride, actual)) {
            mem_pop_temp_roots(mem, saved_tr);
            return val_void();
        }
        uint16_t packed_hdr0 = (uint16_t)((elem_tid & 0xFFu) | (((uint16_t)common_u & 0x7Fu) << 8u));
        mem_write_pool(mem, (uint16_t)(hdr_idx + 0), (double)packed_hdr0);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, (uint16_t)(hdr_idx + 3));
        for (uint8_t i = 0; i < actual; ++i) {
            delegate_vec_write_elem(mem, tab, elem_tid, data_idx, (uint32_t)i * (uint32_t)stride, items[i]);
        }
        mem_write_pool(mem, (uint16_t)(hdr_idx + 1), (double)actual);
        mem_pop_temp_roots(mem, saved_tr);
        return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    }
    uint16_t dims[4] = { actual, 0, 0, 0 };
    uint16_t tot_len = 0, data_idx = 0;
    uint16_t hdr_idx = mem_mat_alloc_hdr(mem, 1, dims, &tot_len, &data_idx);
    if (mem->has_error) return val_void();
    for (uint8_t i = 0; i < actual; ++i) {
        mem_data_write(mem, data_idx, i, val_get_f64(mem, items[i]));
    }
    return val_with_unit(mem, val_make_comp(mem, mem->mat_type_id, hdr_idx, MEM_MAT_HDR_SLOTS), common_u);
}
