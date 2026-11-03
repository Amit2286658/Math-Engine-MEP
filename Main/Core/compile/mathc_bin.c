/*
 * mathc_bin.c — In-Memory `.mathc` Binary Serializer, Validator & O(1) Loader
 */

#include "mathc_bin.h"
#include "parser.h"
#include "../lib/delegate/delegate.h"
#include <string.h>

static uint32_t calc_mathc_payload_bytes(const MathcHeader *h) {
    return (uint32_t)h->perm_src_len
         + (uint32_t)h->perm_ast_count  * (uint32_t)sizeof(AstNode)
         + (uint32_t)h->perm_pool_top   * (uint32_t)sizeof(double)
         + (uint32_t)h->mod_count       * ((uint32_t)sizeof(StrSlice) + (uint32_t)sizeof(uint8_t))
         + (uint32_t)h->type_count      * (uint32_t)sizeof(TypeDef)
         + (uint32_t)h->unit_count      * ((uint32_t)sizeof(StrSlice) + (uint32_t)sizeof(UnitDimDef))
         + (uint32_t)h->pat_elem_count  * (uint32_t)sizeof(PatElem)
         + (uint32_t)h->field_count     * (uint32_t)sizeof(FieldDef)
         + (uint32_t)h->sym_count       * (uint32_t)sizeof(StrSlice)
         + (uint32_t)h->overload_count  * (uint32_t)sizeof(OpOverload)
         + (uint32_t)h->const_count     * (uint32_t)sizeof(ConstDef);
}

static void fill_mathc_header(const MemArena *mem, const SymTable *tab, MathcHeader *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic          = MATHC_MAGIC;
    hdr->version        = MATHC_VERSION;
    hdr->fast_real_type = mem->fast_real_type;
    hdr->str_type_id    = mem->str_type_id;
    hdr->vec_type_id    = mem->vec_type_id;
    hdr->mat_type_id    = mem->mat_type_id;
    hdr->mod_count      = tab->mod_count;
    hdr->sym_count      = tab->sym_count;
    hdr->type_count     = tab->type_count;
    hdr->const_count    = tab->const_count;
    hdr->perm_src_len   = mem->perm_src_len;
    hdr->perm_ast_count = mem->perm_ast_count;
    hdr->perm_pool_top  = mem->perm_pool_top;
    hdr->pat_elem_count = tab->pat_elem_count;
    hdr->field_count    = tab->field_count;
    hdr->overload_count = tab->overload_count;
    hdr->unit_count     = tab->unit_count;
    hdr->payload_bytes  = calc_mathc_payload_bytes(hdr);
    hdr->intr_hash      = delegate_intrinsic_table_hash();
    hdr->checksum       = 0;
}

uint32_t ext_calc_mathc_checksum(const MathcHeader *hdr,
                                 const uint8_t *payload, size_t payload_len) {
    if (!hdr || (!payload && payload_len > 0)) return 0;
    MathcHeader clean_hdr = *hdr;
    clean_hdr.checksum = 0;

    uint32_t h = 2166136261u;
    const uint8_t *hb = (const uint8_t *)&clean_hdr;
    for (size_t i = 0; i < sizeof(MathcHeader); ++i) {
        h ^= hb[i];
        h *= 16777619u;
    }
    for (size_t i = 0; i < payload_len; ++i) {
        h ^= payload[i];
        h *= 16777619u;
    }
    return (h == 0) ? 1u : h;
}

uint32_t ext_calc_mathc_size(const MemArena *mem, const SymTable *tab) {
    MathcHeader hdr;
    fill_mathc_header(mem, tab, &hdr);
    return (uint32_t)sizeof(MathcHeader) + hdr.payload_bytes;
}

size_t ext_save_mathc_mem(MemArena *mem, const SymTable *tab,
                          uint8_t *out_buf, size_t buf_cap) {
    mem_clear_err(mem);
    MathcHeader hdr;
    fill_mathc_header(mem, tab, &hdr);

    size_t total_bytes = sizeof(MathcHeader) + (size_t)hdr.payload_bytes;
    if (!out_buf || buf_cap < total_bytes) {
        mem_err(mem, "Output buffer too small for .mathc image (%zu < %zu bytes)",
                buf_cap, total_bytes);
        return 0;
    }

    uint8_t *cur = out_buf;

    #define WRITE_SLICE(src_ptr, byte_cnt) do { \
        size_t _n = (size_t)(byte_cnt); \
        if (_n > 0) { \
            memcpy(cur, (src_ptr), _n); \
            cur += _n; \
        } \
    } while (0)

    WRITE_SLICE(&hdr,           sizeof(MathcHeader));
    WRITE_SLICE(mem->src_buf,   hdr.perm_src_len);
    WRITE_SLICE(mem->ast,       hdr.perm_ast_count * sizeof(AstNode));
    WRITE_SLICE(mem->f64_pool,  hdr.perm_pool_top  * sizeof(double));
    WRITE_SLICE(sym_mod_names(tab),  (size_t)hdr.mod_count * sizeof(StrSlice));
    WRITE_SLICE(sym_mod_active(tab), (size_t)hdr.mod_count * sizeof(uint8_t));
    WRITE_SLICE(tab->types,     hdr.type_count     * sizeof(TypeDef));
    WRITE_SLICE(tab->units,     hdr.unit_count     * sizeof(StrSlice));
    WRITE_SLICE(tab->unit_dims, hdr.unit_count     * sizeof(UnitDimDef));
    WRITE_SLICE(tab->pat_elems, hdr.pat_elem_count * sizeof(PatElem));
    WRITE_SLICE(tab->fields,    hdr.field_count    * sizeof(FieldDef));
    WRITE_SLICE(tab->syms,      hdr.sym_count      * sizeof(StrSlice));
    WRITE_SLICE(tab->overloads, hdr.overload_count * sizeof(OpOverload));
    WRITE_SLICE(tab->consts,    hdr.const_count    * sizeof(ConstDef));

    #undef WRITE_SLICE

    hdr.checksum = ext_calc_mathc_checksum(&hdr,
                                           out_buf + sizeof(MathcHeader),
                                           hdr.payload_bytes);
    memcpy(out_buf, &hdr, sizeof(MathcHeader));

    return total_bytes;
}

static bool validate_mathc_payload(const uint8_t *payload, const MathcHeader *hdr) {
    /* 1. Validate special bootstrap type IDs in header */
    if (hdr->fast_real_type >= hdr->type_count ||
        hdr->str_type_id    >= hdr->type_count ||
        hdr->vec_type_id    >= hdr->type_count ||
        hdr->mat_type_id    >= hdr->type_count) {
        return false;
    }

    const uint8_t *cur        = payload;
    const uint8_t *src_ptr    = cur; cur += hdr->perm_src_len;
    const uint8_t *ast_ptr    = cur; cur += (size_t)hdr->perm_ast_count * sizeof(AstNode);
    const uint8_t *pool_ptr   = cur; cur += (size_t)hdr->perm_pool_top  * sizeof(double);
    const uint8_t *mods_ptr   = cur; cur += (size_t)hdr->mod_count      * sizeof(StrSlice);
    const uint8_t *mact_ptr   = cur; cur += (size_t)hdr->mod_count      * sizeof(uint8_t);
    const uint8_t *types_ptr  = cur; cur += (size_t)hdr->type_count     * sizeof(TypeDef);
    const uint8_t *units_ptr  = cur; cur += (size_t)hdr->unit_count     * sizeof(StrSlice);
    const uint8_t *udims_ptr  = cur; cur += (size_t)hdr->unit_count     * sizeof(UnitDimDef);
    (void)udims_ptr;
    const uint8_t *pats_ptr   = cur; cur += (size_t)hdr->pat_elem_count * sizeof(PatElem);
    const uint8_t *fields_ptr = cur; cur += (size_t)hdr->field_count    * sizeof(FieldDef);
    const uint8_t *syms_ptr   = cur; cur += (size_t)hdr->sym_count      * sizeof(StrSlice);
    const uint8_t *ovs_ptr    = cur; cur += (size_t)hdr->overload_count * sizeof(OpOverload);
    const uint8_t *consts_ptr = cur; cur += (size_t)hdr->const_count    * sizeof(ConstDef);
    (void)src_ptr;
    (void)pool_ptr;

    /* 2. Validate module names & active flags */
    for (uint16_t i = 0; i < hdr->mod_count; ++i) {
        StrSlice s;
        memcpy(&s, mods_ptr + (size_t)i * sizeof(StrSlice), sizeof(StrSlice));
        if ((uint32_t)s.off + (uint32_t)s.len > hdr->perm_src_len) return false;
        if (mact_ptr[i] > 1u) return false;
    }

    /* 3. Validate types */
    for (uint16_t i = 1; i < hdr->type_count; ++i) {
        TypeDef td;
        memcpy(&td, types_ptr + (size_t)i * sizeof(TypeDef), sizeof(TypeDef));
        if ((uint32_t)td.name.off + (uint32_t)td.name.len > hdr->perm_src_len) return false;
        if (td.mod_id >= hdr->mod_count) return false;
        if (td.parser_intr >= INTR_COUNT) return false;
        if (td.is_consuming) {
            if ((uint32_t)td.start_idx + (uint32_t)td.field_count > hdr->pat_elem_count) return false;
        } else if (td.parser_intr == INTR_NONE || td.parser_intr == INTR_COMPLEX_TYPE) {
            if ((uint32_t)td.start_idx + (uint32_t)td.field_count > hdr->field_count) return false;
            if (td.flat_size == 0 || td.flat_size > 256u) return false;
        }
    }

    /* 4. Validate units */
    for (uint8_t i = 1; i < hdr->unit_count; ++i) {
        StrSlice s;
        memcpy(&s, units_ptr + (size_t)i * sizeof(StrSlice), sizeof(StrSlice));
        if ((uint32_t)s.off + (uint32_t)s.len > hdr->perm_src_len) return false;
    }

    /* 5. Validate pattern elements */
    for (uint16_t i = 0; i < hdr->pat_elem_count; ++i) {
        PatElem pe;
        memcpy(&pe, pats_ptr + (size_t)i * sizeof(PatElem), sizeof(PatElem));
        if (pe.kind < PAT_DIGIT || pe.kind > PAT_ALT) return false;
        if (pe.quant > QUANT_OPT) return false;
        if ((uint32_t)pe.chars.off + (uint32_t)pe.chars.len > hdr->perm_src_len) return false;
        if (pe.kind == PAT_LIT && pe.chars.len == 0) return false;
    }

    /* 6. Validate struct fields */
    for (uint16_t i = 0; i < hdr->field_count; ++i) {
        FieldDef fd;
        memcpy(&fd, fields_ptr + (size_t)i * sizeof(FieldDef), sizeof(FieldDef));
        if ((uint32_t)fd.name.off + (uint32_t)fd.name.len > hdr->perm_src_len) return false;
        if (fd.type_id == 0 || fd.type_id >= hdr->type_count) return false;
        if (fd.flat_offset >= 256u) return false;
    }

    /* 7. Validate struct field ranges against parent struct flat_size */
    for (uint16_t i = 1; i < hdr->type_count; ++i) {
        TypeDef td;
        memcpy(&td, types_ptr + (size_t)i * sizeof(TypeDef), sizeof(TypeDef));
        if (!td.is_consuming && (td.parser_intr == INTR_NONE || td.parser_intr == INTR_COMPLEX_TYPE)) {
            for (uint8_t f = 0; f < td.field_count; ++f) {
                FieldDef fd;
                memcpy(&fd, fields_ptr + (size_t)(td.start_idx + f) * sizeof(FieldDef), sizeof(FieldDef));
                if (fd.type_id == i) return false;
                TypeDef ftd;
                memcpy(&ftd, types_ptr + (size_t)fd.type_id * sizeof(TypeDef), sizeof(TypeDef));
                uint32_t fsz = ftd.is_consuming ? 1u : (uint32_t)ftd.flat_size;
                if (fsz == 0 || (uint32_t)fd.flat_offset + fsz > (uint32_t)td.flat_size) return false;
            }
        }
    }

    /* 8. Validate interned symbols */
    for (uint16_t i = 0; i < hdr->sym_count; ++i) {
        StrSlice s;
        memcpy(&s, syms_ptr + (size_t)i * sizeof(StrSlice), sizeof(StrSlice));
        if ((uint32_t)s.off + (uint32_t)s.len > hdr->perm_src_len) return false;
    }

    /* 9. Validate operator & function overloads */
    uint8_t eff_units = hdr->unit_count ? hdr->unit_count : 1u;
    for (uint16_t i = 0; i < hdr->overload_count; ++i) {
        OpOverload ov;
        memcpy(&ov, ovs_ptr + (size_t)i * sizeof(OpOverload), sizeof(OpOverload));
        if (ov.sym_id >= hdr->sym_count) return false;
        if (ov.mod_id >= hdr->mod_count) return false;
        if (ov.fixity > FIX_SYNTAX) return false;
        if (ov.param_count > SYM_MAX_OV_PARAMS || ov.local_count > PARSER_MAX_LOCALS) return false;
        if (ov.local_count < ov.param_count) return false;
        if ((ov.meta_tag & SYM_META_LAZY_MASK) == 0 && ov.meta_tag >= INTR_COUNT) return false;
        if (ov.ret_type >= hdr->type_count && ov.ret_type != SYM_TYPE_ANY) return false;
        if (ov.ret_unit < SYM_UNIT_LOCK_NONE && ov.ret_unit >= eff_units) return false;
        for (uint8_t p = 0; p < ov.param_count; ++p) {
            if (ov.param_types[p] >= hdr->type_count && ov.param_types[p] != SYM_TYPE_ANY) return false;
            if (ov.param_units[p] < SYM_UNIT_LOCK_NONE && ov.param_units[p] >= eff_units) return false;
        }
        if (ov.fixity != FIX_SYNTAX) {
            if (ov.body_ast >= hdr->perm_ast_count) return false;
        }
    }

    /* 10. Validate constants */
    for (uint8_t i = 0; i < hdr->const_count; ++i) {
        ConstDef cd;
        memcpy(&cd, consts_ptr + (size_t)i * sizeof(ConstDef), sizeof(ConstDef));
        if (cd.sym_id >= hdr->sym_count) return false;
        if (cd.mod_id >= hdr->mod_count) return false;
        if (cd.type_id == 0 || cd.type_id >= hdr->type_count) return false;
        if (cd.unit_id >= eff_units) return false;
        if (cd.val_idx >= hdr->const_count) return false;
        if (cd.pool_idx >= hdr->perm_pool_top) return false;
    }

    /* 11. Validate every AST node and all child indices (`left`, `right`, `ref`, `sub`) */
    #define AST_OPT_OK(idx) ((idx) == MEM_NO_IDX || (idx) < hdr->perm_ast_count)
    #define AST_REQ_OK(idx) ((idx) < hdr->perm_ast_count)

    for (uint16_t i = 0; i < hdr->perm_ast_count; ++i) {
        AstNode n;
        memcpy(&n, ast_ptr + (size_t)i * sizeof(AstNode), sizeof(AstNode));
        switch ((AstKind)n.kind) {
            case AST_LIT:
                if (n.sub >= hdr->type_count) return false;
                if (n.sub == hdr->str_type_id && hdr->str_type_id != 0) {
                    if ((uint32_t)n.left + (uint32_t)n.right > hdr->perm_src_len) return false;
                } else if (n.sub != MEM_TYPE_VOID) {
                    if (n.ref >= hdr->perm_pool_top) return false;
                }
                break;

            case AST_CONST_SYM:
                if (n.ref >= hdr->sym_count) return false;
                break;

            case AST_SLOT_LOAD:
                if ((n.sub & AST_SLOT_IDX_MASK) >= PARSER_MAX_LOCALS) return false;
                if (n.left != MEM_NO_IDX && n.left != SYM_TYPE_ANY &&
                    (n.left & 0x4000u) == 0 && n.left >= hdr->type_count) return false;
                if (n.right != MEM_NO_IDX && n.right < SYM_UNIT_LOCK_NONE && n.right >= eff_units) return false;
                break;

            case AST_FIELD_LOAD:
                if (n.sub >= hdr->type_count) return false;
                if (n.ref >= 256u) return false;
                if (!AST_REQ_OK(n.left)) return false;
                if (n.right != MEM_NO_IDX && (n.right & AST_SLOT_IDX_MASK) >= PARSER_MAX_LOCALS) return false;
                break;

            case AST_UNARY:
                if (n.sub > FIX_SYNTAX || n.ref >= hdr->sym_count || !AST_REQ_OK(n.left)) return false;
                break;

            case AST_BINARY:
                if (n.sub > FIX_SYNTAX || n.ref >= hdr->sym_count ||
                    !AST_REQ_OK(n.left) || !AST_REQ_OK(n.right)) return false;
                break;

            case AST_CALL:
                if (n.ref >= hdr->sym_count || !AST_OPT_OK(n.left)) return false;
                if (n.right != MEM_NO_IDX) {
                    if ((n.right & 0x8000u) != 0) {
                        uint8_t c_tid = (uint8_t)(n.right & 0x3Fu);
                        uint8_t c_uid = (uint8_t)((n.right >> 6u) & 0x3Fu);
                        if (c_tid >= hdr->type_count || c_uid >= eff_units) return false;
                    } else if (!AST_OPT_OK(n.right)) {
                        return false;
                    }
                }
                break;

            case AST_ARG_LINK:
                if (!AST_OPT_OK(n.left) || !AST_OPT_OK(n.right)) return false;
                break;

            case AST_INTRINSIC:
                if (n.sub == INTR_NONE || n.sub >= INTR_COUNT) return false;
                if (!AST_OPT_OK(n.left) || !AST_OPT_OK(n.right)) return false;
                if (n.ref != 0 && !AST_OPT_OK(n.ref)) return false;
                break;

            case AST_CONSTRUCT:
                if (n.ref == 0 || n.ref >= hdr->type_count || !AST_OPT_OK(n.left)) return false;
                break;

            case AST_EXTERN:
                if (n.ref >= hdr->sym_count || !AST_OPT_OK(n.left)) return false;
                break;

            case AST_TERNARY:
                if (!AST_REQ_OK(n.left) || !AST_REQ_OK(n.right)) return false;
                break;

            case AST_SWITCH_EQ:
                if (n.sub > 3 || !AST_REQ_OK(n.left) || !AST_OPT_OK(n.right)) return false;
                if (n.sub == 0 && !AST_REQ_OK(n.right)) return false;
                if (n.sub == 1 && (n.ref == 0 || n.ref >= hdr->type_count)) return false;
                if (n.sub == 2 && n.ref >= PARSER_MAX_LOCALS) return false;
                break;

            case AST_TAGGED_SCOPE:
                if (n.sub >= MEM_MAX_LABELS || !AST_REQ_OK(n.left)) return false;
                break;

            case AST_JUMP:
            case AST_RUN_TAG:
                if (n.sub >= MEM_MAX_LABELS) return false;
                break;

            case AST_BREAK:
                if ((n.sub != 0xFFu && n.sub >= MEM_MAX_LABELS) || !AST_OPT_OK(n.left)) return false;
                break;

            case AST_CONTINUE:
                if (n.sub != 0xFFu && n.sub >= MEM_MAX_LABELS) return false;
                break;

            case AST_RETURN:
            case AST_MAT_LIT:
                if (!AST_OPT_OK(n.left)) return false;
                break;

            case AST_INDEX:
                if (!AST_REQ_OK(n.left) || !AST_REQ_OK(n.right)) return false;
                break;

            case AST_FOR_IN:
                if (n.sub >= PARSER_MAX_LOCALS || !AST_REQ_OK(n.left) || !AST_REQ_OK(n.right)) return false;
                break;

            case AST_UNPACK_ELEM:
                if (n.left >= PARSER_MAX_LOCALS) return false;
                break;

            case AST_LAMBDA:
                if (n.sub > MEM_FN_MAX_PARAMS || !AST_REQ_OK(n.ref) || !AST_OPT_OK(n.right)) return false;
                break;

            case AST_FN_CALL:
                if (!AST_REQ_OK(n.left) || !AST_OPT_OK(n.right)) return false;
                break;

            default:
                return false;
        }
    }

    #undef AST_OPT_OK
    #undef AST_REQ_OK
    return true;
}

bool ext_validate_mathc_buf(const uint8_t *data, size_t len) {
    if (!data || len < sizeof(MathcHeader)) return false;
    MathcHeader hdr;
    memcpy(&hdr, data, sizeof(MathcHeader));
    if (hdr.magic != MATHC_MAGIC || hdr.version != MATHC_VERSION) return false;
    if (hdr.intr_hash != delegate_intrinsic_table_hash()) return false;
    uint32_t expected_payload = calc_mathc_payload_bytes(&hdr);
    if (hdr.payload_bytes != expected_payload ||
        sizeof(MathcHeader) + (size_t)expected_payload > len) {
        return false;
    }
    if (hdr.perm_src_len   > MEM_SRC_BUF_CAP   ||
        hdr.perm_ast_count > MEM_MAX_AST_NODES ||
        hdr.perm_pool_top  > MEM_MAX_F64_POOL  ||
        hdr.mod_count      >= 0xFFFEu          ||
        hdr.type_count     > SYM_MAX_TYPES     ||
        hdr.type_count     == 0                ||
        hdr.unit_count     > SYM_MAX_UNITS     ||
        hdr.pat_elem_count > SYM_MAX_PAT_ELEMS ||
        hdr.field_count    > SYM_MAX_FIELDS    ||
        hdr.sym_count      > SYM_MAX_SYMS      ||
        hdr.overload_count > SYM_MAX_OVERLOADS ||
        hdr.const_count    > SYM_MAX_CONSTS) {
        return false;
    }
    uint32_t expected_csum = ext_calc_mathc_checksum(&hdr,
                                                     data + sizeof(MathcHeader),
                                                     hdr.payload_bytes);
    if (hdr.checksum != expected_csum) return false;
    return validate_mathc_payload(data + sizeof(MathcHeader), &hdr);
}

bool ext_load_mathc_mem(MemArena *mem, SymTable *tab,
                        const uint8_t *data, size_t len) {
    if (!mem || !tab) return false;
    mem->sym_tab = tab;
    mem_clear_err(mem);
    if (!data || len < sizeof(MathcHeader)) {
        mem_err(mem, "Invalid or truncated .mathc image");
        return false;
    }

    MathcHeader hdr;
    memcpy(&hdr, data, sizeof(MathcHeader));
    if (hdr.magic != MATHC_MAGIC || hdr.version != MATHC_VERSION) {
        mem_err(mem, "Bad .mathc magic or version mismatch");
        return false;
    }
    if (hdr.intr_hash != delegate_intrinsic_table_hash()) {
        mem_err(mem, "Stale .mathc image: intrinsic table fingerprint mismatch");
        return false;
    }

    uint32_t expected_payload = calc_mathc_payload_bytes(&hdr);
    if (hdr.payload_bytes != expected_payload ||
        sizeof(MathcHeader) + (size_t)expected_payload > len) {
        mem_err(mem, "Corrupted .mathc payload size");
        return false;
    }

    if (hdr.perm_src_len   > MEM_SRC_BUF_CAP   ||
        hdr.perm_ast_count > MEM_MAX_AST_NODES ||
        hdr.perm_pool_top  > MEM_MAX_F64_POOL  ||
        hdr.mod_count      >= 0xFFFEu          ||
        hdr.type_count     > SYM_MAX_TYPES     ||
        hdr.type_count     == 0                ||
        hdr.unit_count     > SYM_MAX_UNITS     ||
        hdr.pat_elem_count > SYM_MAX_PAT_ELEMS ||
        hdr.field_count    > SYM_MAX_FIELDS    ||
        hdr.sym_count      > SYM_MAX_SYMS      ||
        hdr.overload_count > SYM_MAX_OVERLOADS ||
        hdr.const_count    > SYM_MAX_CONSTS) {
        mem_err(mem, ".mathc image exceeds compiled arena capacities");
        return false;
    }

    /* Validate all payload table & AST child indices BEFORE touching mem or tab */
    if (!validate_mathc_payload(data + sizeof(MathcHeader), &hdr)) {
        mem_err(mem, "Corrupted .mathc index out of bounds");
        return false;
    }

    uint32_t expected_csum = ext_calc_mathc_checksum(&hdr,
                                                     data + sizeof(MathcHeader),
                                                     hdr.payload_bytes);
    if (hdr.checksum != expected_csum) {
        mem_err(mem, "Corrupted .mathc checksum mismatch");
        return false;
    }

    if (!sym_ensure_mod_cap(tab, mem, hdr.mod_count)) {
        return false;
    }

    const uint8_t *cur = data + sizeof(MathcHeader);

    #define COPY_SLICE(dst, byte_cnt) do { \
        size_t _n = (size_t)(byte_cnt); \
        if (_n > 0) { \
            memcpy((dst), cur, _n); \
            cur += _n; \
        } \
    } while (0)

    COPY_SLICE(mem->src_buf,            hdr.perm_src_len);
    COPY_SLICE(mem->ast,                (size_t)hdr.perm_ast_count * sizeof(AstNode));
    COPY_SLICE(mem->f64_pool,           (size_t)hdr.perm_pool_top  * sizeof(double));
    COPY_SLICE(sym_mod_names_mut(tab),  (size_t)hdr.mod_count      * sizeof(StrSlice));
    COPY_SLICE(sym_mod_active_mut(tab), (size_t)hdr.mod_count      * sizeof(uint8_t));
    COPY_SLICE(tab->types,     hdr.type_count     * sizeof(TypeDef));
    COPY_SLICE(tab->units,     hdr.unit_count     * sizeof(StrSlice));
    COPY_SLICE(tab->unit_dims, hdr.unit_count     * sizeof(UnitDimDef));
    COPY_SLICE(tab->pat_elems, hdr.pat_elem_count * sizeof(PatElem));
    COPY_SLICE(tab->fields,    hdr.field_count    * sizeof(FieldDef));
    COPY_SLICE(tab->syms,      hdr.sym_count      * sizeof(StrSlice));
    COPY_SLICE(tab->overloads, hdr.overload_count * sizeof(OpOverload));
    COPY_SLICE(tab->consts,    hdr.const_count    * sizeof(ConstDef));

    #undef COPY_SLICE

    mem->fast_real_type = hdr.fast_real_type;
    mem->str_type_id    = hdr.str_type_id;
    mem->vec_type_id    = hdr.vec_type_id;
    mem->mat_type_id    = hdr.mat_type_id;
    mem->fn_type_id     = 0;
    mem->map_type_id    = 0;
    memset(mem->type_stride, 0, sizeof(mem->type_stride));
    for (uint16_t i = 1; i < hdr.type_count; ++i) {
        if (i < 64u) mem->type_stride[i] = (uint8_t)tab->types[i].flat_size;
        if (tab->types[i].parser_intr == INTR_FN_TYPE && mem->fn_type_id == 0) {
            mem->fn_type_id = (uint8_t)i;
        } else if (tab->types[i].parser_intr == INTR_MAP_TYPE && mem->map_type_id == 0) {
            mem->map_type_id = (uint8_t)i;
        }
    }
    mem->src_len        = hdr.perm_src_len;
    mem->ast_count      = hdr.perm_ast_count;
    mem->pool_top       = hdr.perm_pool_top;

    tab->mod_count      = hdr.mod_count;
    tab->type_count     = hdr.type_count;
    tab->unit_count     = hdr.unit_count ? hdr.unit_count : 1;
    tab->pat_elem_count = hdr.pat_elem_count;
    tab->field_count    = hdr.field_count;
    tab->sym_count      = hdr.sym_count;
    tab->overload_count = hdr.overload_count;
    tab->const_count    = hdr.const_count;

    /* Reconstruct ABI-specific Value representations for global constants IN-PLACE
       using their saved `pool_idx` (zero new pool allocations). */
    for (uint8_t i = 0; i < tab->const_count; ++i) {
        uint16_t rtid = tab->consts[i].type_id;
        uint8_t  ruid = tab->consts[i].unit_id;
        uint16_t pidx = tab->consts[i].pool_idx;
        tab->const_vals[i] = val_make_f64_at_pool(mem, rtid, ruid, pidx);
    }

    tab->var_count = 0;
    sym_sync_consts_to_vars(tab, mem);
    sym_rebuild_index(tab);
    mem_snapshot_perm(mem);
    return true;
}
