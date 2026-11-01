/*
 * math_engine.c — Top-Level Engine Wrapper, Stateless Eval & Value Formatter
 *
 * Internally loads the precompiled bootstrap and standard library modules
 * (`stdlib_precompiled.h`) in O(1) `memcpy` time inside `eng_init()` with
 * zero filesystem, directory, or file extension dependencies.
 *
 * Note:
 *   - Compilation, dependency-resolved linking, `.mathc` serialization, and
 *     user extension loading live in `Core/compile/`.
 *   - Parallel in-memory sessions and order-tagged raw-text snapshots live in
 *     `Core/session/`.
 */

#include "math_engine.h"
#if !defined(MATH_ENGINE_SEPARATE_STDLIB)
#include "stdlib_precompiled.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void eng_init_blank(Engine *eng) {
    if (!eng) return;
    mem_init(&eng->mem);
    sym_init(&eng->sym);
    memset(eng->sessions, 0, sizeof(eng->sessions));
}

bool eng_init(Engine *eng) {
    if (!eng) return false;
    eng_init_blank(eng);
#if !defined(MATH_ENGINE_SEPARATE_STDLIB)
    return ext_load_mathc_mem(&eng->mem, &eng->sym, k_stdlib_mathc, sizeof(k_stdlib_mathc));
#else
    mem_err(&eng->mem, "Engine uses separate stdlib.mathc; initialize via nativeCreateBlank() + nativeLoadMathc()");
    return false;
#endif
}

Engine *eng_create_blank(void) {
    Engine *eng = (Engine *)malloc(sizeof(Engine));
    if (!eng) return NULL;
    memset(eng, 0, sizeof(Engine));
    eng_init_blank(eng);
    return eng;
}

Engine *eng_create(void) {
    Engine *eng = (Engine *)malloc(sizeof(Engine));
    if (!eng) return NULL;
    memset(eng, 0, sizeof(Engine));
    if (!eng_init(eng)) {
        sym_free_heap(&eng->sym);
        free(eng);
        return NULL;
    }
    return eng;
}

void eng_destroy(Engine *eng) {
    if (!eng) return;
    mem_free_all_heap(&eng->mem);
    sym_free_heap(&eng->sym);
    free(eng);
}

bool eng_register_extern(Engine *eng, const char *name,
                         EngExternFn fn, void *user_ctx) {
    if (!eng) return false;
    return sym_register_extern(&eng->sym, &eng->mem, name, fn, user_ctx);
}

bool eng_set_module(Engine *eng, const char *mod_name, bool enabled) {
    if (!eng || !mod_name) return false;
    return sym_set_module(&eng->sym, &eng->mem, mod_name, enabled);
}

bool eng_is_module_active(const Engine *eng, const char *mod_name) {
    if (!eng || !mod_name) return false;
    return sym_is_module_active(&eng->sym, &eng->mem, mod_name);
}

bool eng_eval(Engine *eng, const char *expr, Value *out_val) {
    if (!eng) return false;
    mem_clear_err(&eng->mem);
    mem_rewind_scratch(&eng->mem);

    if (!expr) {
        mem_err(&eng->mem, "Null expression");
        return false;
    }
    size_t raw_len = strlen(expr);
    if (raw_len > 65534u) {
        mem_err(&eng->mem, "Expression length exceeds maximum (65534 bytes)");
        return false;
    }

    Parser P;
    parser_init(&P, &eng->mem, &eng->sym, expr, (uint16_t)raw_len, 0, false, false);

    lex_next(&P.lex);
    if (P.lex.cur.kind == TOK_EOF && !eng->mem.has_error) {
        mem_err(&eng->mem, "Empty expression");
        return false;
    }

    uint16_t root = parser_parse_expr(&P, 0);
    if (!eng->mem.has_error && P.lex.cur.kind != TOK_EOF) {
        mem_err(&eng->mem, "Unexpected trailing token '%.*s'",
                (int)P.lex.cur.len, P.lex.src + P.lex.cur.off);
    }
    if (!parser_check_labels(&P) || eng->mem.has_error || root == MEM_NO_IDX) {
        return false;
    }

    for (uint8_t i = 0; i < P.label_count && i < MEM_MAX_LABELS; ++i) {
        eng->mem.label_ast[i] = P.labels[i].body_ast;
    }

    uint16_t frame_base = 0;
    if (P.local_count > 0) {
        frame_base = mem_push_frame(&eng->mem, P.local_count);
        if (eng->mem.has_error) return false;
        for (uint8_t i = 0; i < P.local_count; ++i) {
            mem_set_slot(&eng->mem, frame_base, i, val_make_f64(&eng->mem, eng->mem.fast_real_type, 0.0));
        }
    }

    Value res = eval_ast(&eng->mem, &eng->sym, root, frame_base, 0);
    if (eng->mem.jump_active == JUMP_RETURN) {
        res = eng->mem.jump_val;
        eng->mem.jump_active = JUMP_NONE;
    } else if (eng->mem.jump_active && !eng->mem.has_error) {
        eng->mem.jump_active = 0;
        mem_err(&eng->mem, "Unhandled jump, break, or continue outside active loop/scope");
    }
    if (P.local_count > 0) {
        mem_pop_frame(&eng->mem, frame_base);
    }
    if (eng->mem.has_error) return false;
    if (out_val) *out_val = res;
    return true;
}

uint16_t eng_val_type(const Engine *eng, Value v) {
    if (!eng) return MEM_TYPE_VOID;
    return val_get_type(&eng->mem, v);
}

uint8_t eng_val_unit(const Engine *eng, Value v) {
    if (!eng) return MEM_UNIT_NONE;
    return val_get_unit(&eng->mem, v);
}

const char *eng_type_name(const Engine *eng, uint16_t type_id, char *buf, size_t cap) {
    if (!eng) {
        if (buf && cap > 0) buf[0] = '\0';
        return "";
    }
    return sym_type_name(&eng->sym, &eng->mem, type_id, buf, cap);
}

const char *eng_unit_name(const Engine *eng, uint8_t unit_id, char *buf, size_t cap) {
    if (!eng) {
        if (buf && cap > 0) buf[0] = '\0';
        return "";
    }
    return sym_unit_name(&eng->sym, &eng->mem, unit_id, buf, cap);
}

const char *eng_last_error(const Engine *eng) {
    if (!eng) return "Null engine pointer";
    return eng->mem.err_msg;
}

uint16_t eng_module_count(const Engine *eng) {
    return eng ? eng->sym.mod_count : 0;
}

const char *eng_module_name(const Engine *eng, uint16_t mod_id, char *buf, size_t cap) {
    if (!buf || cap == 0) return "";
    if (!eng || mod_id >= eng->sym.mod_count) {
        buf[0] = '\0';
        return buf;
    }
    StrSlice s = sym_mod_name(&eng->sym, mod_id);
    snprintf(buf, cap, "%.*s", (int)s.len, mem_str_ptr(&eng->mem, s));
    return buf;
}

uint16_t eng_overload_count(const Engine *eng) {
    return eng ? eng->sym.overload_count : 0;
}

uint8_t eng_const_count(const Engine *eng) {
    return eng ? eng->sym.const_count : 0;
}

uint16_t eng_compact_scratch(Engine *eng) {
    if (!eng) return 0;
    return mem_compact_scratch(&eng->mem, NULL, 0);
}

uint16_t eng_scratch_pool_used(const Engine *eng) {
    if (!eng) return 0;
    return mem_scratch_pool_used(&eng->mem);
}

bool eng_sym_simplify(Engine *eng, const char *expr, char *out_buf, size_t buf_cap) {
    if (!eng || !expr || !out_buf || buf_cap == 0) return false;
    out_buf[0] = '\0';
    char qbuf[8192];
    int n = snprintf(qbuf, sizeof(qbuf), "sym_simplify(%s)", expr);
    if (n < 0 || (size_t)n >= sizeof(qbuf)) {
        mem_err(&eng->mem, "Symbolic expression exceeds maximum length");
        return false;
    }
    Value v = val_void();
    if (!eng_eval(eng, qbuf, &v)) return false;
    StrSlice s = val_get_str(&eng->mem, v);
    if ((size_t)s.len + 1u > buf_cap) {
        mem_err(&eng->mem, "Symbolic output exceeds caller buffer capacity (%zu bytes)", buf_cap);
        return false;
    }
    return val_as_cstr(&eng->mem, v, out_buf, buf_cap);
}

bool eng_sym_diff(Engine *eng, const char *var_name, const char *expr, char *out_buf, size_t buf_cap) {
    if (!eng || !var_name || !expr || !out_buf || buf_cap == 0) return false;
    out_buf[0] = '\0';
    char qbuf[8192];
    int n = snprintf(qbuf, sizeof(qbuf), "sym_diff(%s, %s)", var_name, expr);
    if (n < 0 || (size_t)n >= sizeof(qbuf)) {
        mem_err(&eng->mem, "Symbolic expression exceeds maximum length");
        return false;
    }
    Value v = val_void();
    if (!eng_eval(eng, qbuf, &v)) return false;
    StrSlice s = val_get_str(&eng->mem, v);
    if ((size_t)s.len + 1u > buf_cap) {
        mem_err(&eng->mem, "Symbolic output exceeds caller buffer capacity (%zu bytes)", buf_cap);
        return false;
    }
    return val_as_cstr(&eng->mem, v, out_buf, buf_cap);
}

bool eng_sym_integ(Engine *eng, const char *var_name, const char *expr, char *out_buf, size_t buf_cap) {
    if (!eng || !var_name || !expr || !out_buf || buf_cap == 0) return false;
    out_buf[0] = '\0';
    char qbuf[8192];
    int n = snprintf(qbuf, sizeof(qbuf), "sym_integ(%s, %s)", var_name, expr);
    if (n < 0 || (size_t)n >= sizeof(qbuf)) {
        mem_err(&eng->mem, "Symbolic expression exceeds maximum length");
        return false;
    }
    Value v = val_void();
    if (!eng_eval(eng, qbuf, &v)) return false;
    StrSlice s = val_get_str(&eng->mem, v);
    if ((size_t)s.len + 1u > buf_cap) {
        mem_err(&eng->mem, "Symbolic output exceeds caller buffer capacity (%zu bytes)", buf_cap);
        return false;
    }
    return val_as_cstr(&eng->mem, v, out_buf, buf_cap);
}

bool eng_sym_solve(Engine *eng, const char *var_name, const char *eq, char *out_buf, size_t buf_cap) {
    if (!eng || !var_name || !eq || !out_buf || buf_cap == 0) return false;
    out_buf[0] = '\0';
    char qbuf[8192];
    int n = snprintf(qbuf, sizeof(qbuf), "sym_solve(%s, %s)", var_name, eq);
    if (n < 0 || (size_t)n >= sizeof(qbuf)) {
        mem_err(&eng->mem, "Symbolic expression exceeds maximum length");
        return false;
    }
    Value v = val_void();
    if (!eng_eval(eng, qbuf, &v)) return false;
    StrSlice s = val_get_str(&eng->mem, v);
    if ((size_t)s.len + 1u > buf_cap) {
        mem_err(&eng->mem, "Symbolic output exceeds caller buffer capacity (%zu bytes)", buf_cap);
        return false;
    }
    return val_as_cstr(&eng->mem, v, out_buf, buf_cap);
}

/* ============================================================================
 * HUMAN-READABLE VALUE FORMATTER (`eng_format_value`, `eng_format_mat_at`)
 * ============================================================================ */

static void eng_buf_append(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
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

static void format_struct_at(const Engine *eng, uint16_t type_id, uint16_t pool_idx,
                             uint8_t depth, char *buf, size_t cap);

static void format_mat_slice(const Engine *eng, uint16_t data_idx, uint32_t base_off,
                             const uint16_t *dims, uint8_t rank, uint8_t axis,
                             bool high_prec, char *buf, size_t cap, size_t *used) {
    eng_buf_append(buf, cap, used, "[");
    if (axis + 1 >= rank) {
        uint16_t n = dims[axis];
        for (uint16_t i = 0; i < n && *used + 1 < cap; ++i) {
            double v = mem_data_read(&eng->mem, data_idx, base_off + i);
            if (v == 0.0) v = 0.0; /* normalize -0.0 */
            eng_buf_append(buf, cap, used,
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
                eng_buf_append(buf, cap, used, ", ");
            }
            format_mat_slice(eng, data_idx, base_off + (uint32_t)i * stride,
                             dims, rank, (uint8_t)(axis + 1), high_prec, buf, cap, used);
        }
    }
    eng_buf_append(buf, cap, used, "]");
}

void eng_format_mat_at(const Engine *eng, uint16_t hdr_idx, bool high_prec,
                       char *buf, size_t cap) {
    uint8_t  rank     = (uint8_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 0));
    uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 2));
    uint16_t dims[4]  = { 0, 0, 0, 0 };
    for (uint8_t i = 0; i < 4; ++i) {
        dims[i] = (i < rank) ? (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 4u + i)) : 0;
    }
    if (rank == 0) {
        snprintf(buf, cap, "[]");
        return;
    }
    size_t used = 0;
    format_mat_slice(eng, data_idx, 0, dims, rank, 0, high_prec, buf, cap, &used);
}

static void format_vec_at(const Engine *eng, uint16_t hdr_idx, uint8_t depth,
                          char *buf, size_t cap) {
    if (depth > 16) {
        snprintf(buf, cap, "[...]");
        return;
    }
    uint16_t raw_hdr0 = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 0));
    uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
    uint32_t len      = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 1));
    uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 3));
    uint16_t stride   = (elem_tid > 0 && elem_tid < eng->sym.type_count)
                        ? eng->sym.types[elem_tid].flat_size : 1;

    size_t used = 0;
    if (cap > 0) buf[0] = '\0';
    eng_buf_append(buf, cap, &used, "[");
    for (uint32_t i = 0; i < len && used + 1 < cap; ++i) {
        uint32_t off = i * (uint32_t)stride;
        char elem_str[96];
        if (elem_tid == eng->mem.str_type_id && elem_tid != 0) {
            uint32_t packed = (uint32_t)mem_data_read(&eng->mem, data_idx, off);
            StrSlice s = { (uint16_t)(packed >> 16), (uint16_t)(packed & 0xFFFFu) };
            snprintf(elem_str, sizeof(elem_str), "\"%.*s\"",
                     (int)s.len, mem_str_ptr(&eng->mem, s));
        } else if (elem_tid == eng->mem.vec_type_id && elem_tid != 0) {
            format_vec_at(eng, (uint16_t)(data_idx + off), (uint8_t)(depth + 1), elem_str, sizeof(elem_str));
        } else if (elem_tid > 0 && elem_tid < eng->sym.type_count &&
                   !eng->sym.types[elem_tid].is_consuming) {
            format_struct_at(eng, elem_tid, (uint16_t)(data_idx + off), (uint8_t)(depth + 1), elem_str, sizeof(elem_str));
        } else {
            snprintf(elem_str, sizeof(elem_str), "%.6g", mem_data_read(&eng->mem, data_idx, off));
        }
        eng_buf_append(buf, cap, &used, "%s%s", (i > 0 ? ", " : ""), elem_str);
    }
    eng_buf_append(buf, cap, &used, "]");
}

static void format_struct_at(const Engine *eng, uint16_t type_id, uint16_t pool_idx,
                             uint8_t depth, char *buf, size_t cap) {
    if (depth > 16) {
        snprintf(buf, cap, "<...> ");
        return;
    }
    if (type_id == eng->mem.vec_type_id && type_id != 0) {
        format_vec_at(eng, pool_idx, depth, buf, cap);
        return;
    }
    if (type_id == eng->mem.mat_type_id && type_id != 0) {
        eng_format_mat_at(eng, pool_idx, false, buf, cap);
        return;
    }
    if (type_id == eng->mem.fn_type_id && type_id != 0) {
        unsigned pcnt = (unsigned)mem_read_pool(&eng->mem, (uint16_t)(pool_idx + 1)) & 0x3Fu;
        snprintf(buf, cap, "<fn/%u>", pcnt);
        return;
    }
    if (type_id == eng->mem.map_type_id && type_id != 0) {
        uint16_t len      = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pool_idx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pool_idx + 3));
        size_t used = 0;
        if (cap > 0) buf[0] = '\0';
        eng_buf_append(buf, cap, &used, "{");
        for (uint16_t i = 0; i < len && used + 1 < cap; ++i) {
            StrSlice kslice = mem_map_entry_key(&eng->mem, data_idx, i);
            Value    vval   = mem_map_entry_val(&eng->mem, data_idx, i);
            char vstr[96];
            eng_format_value(eng, vval, vstr, sizeof(vstr));
            eng_buf_append(buf, cap, &used, "%s\"%.*s\": %s",
                           (i > 0 ? ", " : ""),
                           (int)kslice.len, mem_str_ptr(&eng->mem, kslice),
                           vstr);
        }
        eng_buf_append(buf, cap, &used, "}");
        return;
    }

    const TypeDef *td = &eng->sym.types[type_id];
    char tname[32];
    eng_type_name(eng, type_id, tname, sizeof(tname));

    if (td->parser_intr == INTR_COMPLEX_TYPE && td->flat_size == 2) {
        double re = mem_read_pool(&eng->mem, pool_idx);
        double im = mem_read_pool(&eng->mem, (uint16_t)(pool_idx + 1));
        if (im >= 0.0) {
            snprintf(buf, cap, "%.6g + %.6gi", re, im);
        } else {
            snprintf(buf, cap, "%.6g - %.6gi", re, -im);
        }
        return;
    }

    size_t used = 0;
    if (cap > 0) buf[0] = '\0';
    eng_buf_append(buf, cap, &used, "%s(", tname);
    for (uint8_t i = 0; i < td->field_count && used + 1 < cap; ++i) {
        const FieldDef *fd = &eng->sym.fields[td->start_idx + i];
        char f_str[64];
        if (!eng->sym.types[fd->type_id].is_consuming) {
            format_struct_at(eng, fd->type_id, (uint16_t)(pool_idx + fd->flat_offset),
                             (uint8_t)(depth + 1), f_str, sizeof(f_str));
        } else {
            snprintf(f_str, sizeof(f_str), "%.6g", mem_read_pool(&eng->mem, (uint16_t)(pool_idx + fd->flat_offset)));
        }
        eng_buf_append(buf, cap, &used, "%s%.*s=%s",
                       (i > 0 ? ", " : ""),
                       (int)fd->name.len, mem_str_ptr(&eng->mem, fd->name),
                       f_str);
    }
    eng_buf_append(buf, cap, &used, ")");
}

void eng_format_value(const Engine *eng, Value val, char *buf, size_t buf_cap) {
    if (!eng || !buf || buf_cap == 0) return;
    uint16_t tid = eng_val_type(eng, val);
    if (tid == 0 || tid >= eng->sym.type_count) {
        snprintf(buf, buf_cap, "<void>");
        return;
    }
    const TypeDef *td = &eng->sym.types[tid];
    if (td->is_consuming) {
        if (td->parser_intr == INTR_PARSE_STR_SLICE) {
            StrSlice s = val_get_str(&eng->mem, val);
            snprintf(buf, buf_cap, "\"%.*s\"", (int)s.len, mem_str_ptr(&eng->mem, s));
        } else if (td->parser_intr == INTR_PARSE_F64_TRIM1) {
            snprintf(buf, buf_cap, "%.6gi", val_get_f64(&eng->mem, val));
        } else {
            snprintf(buf, buf_cap, "%.10g", val_get_f64(&eng->mem, val));
        }
    } else {
        format_struct_at(eng, tid, val_get_pool_idx(val), 0, buf, buf_cap);
    }
}
