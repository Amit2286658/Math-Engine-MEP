/*
 * math_engine.c — Top-Level Engine Wrapper, Module Execution & Value Formatter
 *
 * Every engine boots BLANK ("empty engine") and its host loads the precompiled
 * standard library image (`stdlib.mathc`) separately in O(1) `memcpy` time via
 * the user-loader (`Core/compile/user_loader.h`, JNI `nativeLoadMathc()`).
 *
 * Note:
 *   - Compilation, dependency-resolved linking, `.mathc` serialization,
 *     top-to-bottom module execution, and user extension loading live in
 *     `Core/compile/`.
 *   - Module-scoped variables (`:=` / `mut` mutable vs. `=` / `const` immutable,
 *     `pub` vs. `priv` visibility) live directly in `SymTable.vars`.
 */

#include "math_engine.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void eng_init_blank(Engine *eng) {
    if (!eng) return;
    mem_init(&eng->mem);
    sym_init(&eng->sym);
    eng->mem.sym_tab = &eng->sym;
}

void eng_init(Engine *eng) {
    /* Blank init; the returned engine has NO standard library. Hosts load
     * `stdlib.mathc` explicitly (user_load_mathc / nativeLoadMathc). */
    eng_init_blank(eng);
}

Engine *eng_create_blank(void) {
    Engine *eng = (Engine *)malloc(sizeof(Engine));
    if (!eng) return NULL;
    memset(eng, 0, sizeof(Engine));
    eng_init_blank(eng);
    return eng;
}

Engine *eng_create(void) {
    /* Same as eng_create_blank: an empty engine whose host is responsible
     * for loading `stdlib.mathc` before evaluating. */
    return eng_create_blank();
}

void eng_destroy(Engine *eng) {
    if (!eng) return;
    /* Extern destructors first: JNI bridges free their global refs / payload
     * allocations here and do not depend on engine heap or symbol memory. */
    sym_free_externs(&eng->sym);
    mem_free_all_heap(&eng->mem);
    sym_free_heap(&eng->sym);
    free(eng);
}

bool eng_register_extern(Engine *eng, const char *name,
                         EngExternFn fn, void *user_ctx) {
    if (!eng) return false;
    return sym_register_extern(&eng->sym, &eng->mem, name, fn, user_ctx);
}

bool eng_register_extern_full(Engine *eng, const char *name,
                              EngExternFn fn, void *user_ctx,
                              void (*fn_free)(void *user_ctx)) {
    if (!eng) return false;
    return sym_register_extern_full(&eng->sym, &eng->mem, name, fn, user_ctx, fn_free);
}

void eng_free_externs(Engine *eng) {
    if (!eng) return;
    sym_free_externs(&eng->sym);
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
    eng->mem.sym_tab = &eng->sym;
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

    /* Delegate scripts containing top-level $type / $enum / $struct / import declarations to ext_eval_slice */
    {
        size_t p = 0;
        while (p < raw_len && (expr[p] == ' ' || expr[p] == '\t' || expr[p] == '\r' || expr[p] == '\n')) p++;
        if (p < raw_len && (expr[p] == '$' ||
            (p + 5 <= raw_len && memcmp(expr + p, "enum ", 5) == 0) ||
            (p + 7 <= raw_len && memcmp(expr + p, "struct ", 7) == 0) ||
            (p + 7 <= raw_len && memcmp(expr + p, "import ", 7) == 0))) {
            return ext_eval_slice(&eng->mem, &eng->sym, "default", true, expr, (uint16_t)raw_len, out_val);
        }
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

    Value res;
    uint16_t bc_pc = (P.label_count == 0) ? bc_compile_ast(&eng->mem, &eng->sym, root, false) : MEM_NO_IDX;
    if (bc_pc != MEM_NO_IDX) {
        res = bc_exec(&eng->mem, &eng->sym, bc_pc, frame_base, 0);
    } else {
        res = eval_ast(&eng->mem, &eng->sym, root, frame_base, 0);
    }
    if (eng->mem.jump_active == JUMP_RETURN) {
        res = eng->mem.jump_val;
        eng->mem.jump_active = JUMP_NONE;
    } else if (eng->mem.jump_active == JUMP_THROW) {
        eng->mem.jump_active = JUMP_NONE;
        if (!eng->mem.has_error) {
            mem_err(&eng->mem, "Unhandled exception");
        }
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

/* ============================================================================
 * TOP-TO-BOTTOM MODULE EXECUTION, RESET & VARIABLE INTROSPECTION
 * ============================================================================ */

bool eng_mod_eval(Engine *eng, const char *mod_name,
                  const char *src, Value *out_val) {
    if (out_val) *out_val = val_void();
    if (!eng) return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    mem_rewind_scratch(&eng->mem);

    if (!src) {
        mem_err(&eng->mem, "Null module source or expression");
        return false;
    }
    size_t raw_len = strlen(src);
    if (raw_len > 65534u) {
        mem_err(&eng->mem, "Module source length exceeds maximum (65534 bytes)");
        return false;
    }
    if (raw_len == 0) {
        mem_err(&eng->mem, "Empty expression");
        return false;
    }

    Value res = val_void();
    if (!ext_eval_slice(&eng->mem, &eng->sym, mod_name, true, src, (uint16_t)raw_len, &res)) {
        return false;
    }
    if (out_val) *out_val = res;
    return true;
}

bool eng_mod_reset(Engine *eng, const char *mod_name) {
    if (!eng) return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    return sym_reset_module(&eng->sym, &eng->mem, mod_name);
}

uint16_t eng_mod_var_count(const Engine *eng, const char *mod_name) {
    if (!eng) return 0;
    const char *eff = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    int mid = sym_find_mod(&eng->sym, &eng->mem, eff);
    if (mid < 0) return 0;
    uint16_t mod_id = (uint16_t)mid;
    uint16_t count  = 0;
    const ModVarDef *vars = sym_vars(&eng->sym);
    for (uint16_t i = 0; i < eng->sym.var_count; ++i) {
        if (vars[i].mod_id == mod_id) {
            count++;
        }
    }
    return count;
}

bool eng_mod_var_info(const Engine *eng, const char *mod_name,
                      uint16_t idx, char *name_buf, size_t name_cap,
                      Value *out_val, uint16_t *out_lock_type,
                      uint8_t *out_lock_unit, uint8_t *out_flags) {
    if (name_buf && name_cap > 0) name_buf[0] = '\0';
    if (out_val)       *out_val       = val_void();
    if (out_lock_type) *out_lock_type = 0;
    if (out_lock_unit) *out_lock_unit = 0;
    if (out_flags)     *out_flags     = 0;
    if (!eng) return false;

    const char *eff = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    int mid = sym_find_mod(&eng->sym, &eng->mem, eff);
    if (mid < 0) return false;
    uint16_t mod_id = (uint16_t)mid;

    const ModVarDef *vars = sym_vars(&eng->sym);
    uint16_t cur = 0;
    for (uint16_t i = 0; i < eng->sym.var_count; ++i) {
        if (vars[i].mod_id != mod_id) continue;
        if (cur == idx) {
            if (name_buf && name_cap > 0 && vars[i].sym_id < eng->sym.sym_count) {
                StrSlice s = eng->sym.syms[vars[i].sym_id];
                snprintf(name_buf, name_cap, "%.*s", (int)s.len, mem_str_ptr(&eng->mem, s));
            }
            if (out_val)       *out_val       = vars[i].val;
            if (out_lock_type) *out_lock_type = (uint16_t)(vars[i].lock_type & 0xFFu);
            if (out_lock_unit) *out_lock_unit = vars[i].lock_unit;
            if (out_flags)     *out_flags     = vars[i].flags;
            return true;
        }
        cur++;
    }
    return false;
}

bool eng_mod_get_var(const Engine *eng, const char *mod_name,
                     const char *var_name, Value *out_val,
                     uint8_t *out_flags) {
    if (out_val)   *out_val   = val_void();
    if (out_flags) *out_flags = 0;
    if (!eng || !var_name || var_name[0] == '\0') return false;

    const char *eff = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    int mid = sym_find_mod(&eng->sym, &eng->mem, eff);
    if (mid < 0) return false;
    int sid = sym_find(&eng->sym, &eng->mem, var_name, (uint16_t)strlen(var_name));
    if (sid < 0) return false;

    int vidx = sym_find_own_var(&eng->sym, (uint16_t)sid, (uint16_t)mid);
    if (vidx < 0) return false;
    const ModVarDef *mv = &sym_vars(&eng->sym)[vidx];
    if (out_val)   *out_val   = mv->val;
    if (out_flags) *out_flags = mv->flags;
    return true;
}

bool eng_mod_set_var(Engine *eng, const char *mod_name,
                     const char *var_name, Value val,
                     uint8_t flags) {
    if (!eng || !var_name || var_name[0] == '\0') return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);

    const char *eff = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    StrSlice ms = mem_intern_str(&eng->mem, eff, (uint16_t)strlen(eff));
    if (eng->mem.has_error) return false;
    uint16_t mod_id = sym_find_or_add_mod(&eng->sym, &eng->mem, ms);
    if (eng->mem.has_error) return false;

    StrSlice vs = mem_intern_str(&eng->mem, var_name, (uint16_t)strlen(var_name));
    if (eng->mem.has_error) return false;
    uint16_t sym_id = sym_intern(&eng->sym, &eng->mem, vs);
    if (eng->mem.has_error) return false;

    int res = sym_set_or_add_var(&eng->sym, &eng->mem, sym_id, mod_id, val, 0, 0, flags, true);
    return (res >= 0 && !eng->mem.has_error);
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
 * HUMAN-READABLE VALUE FORMATTER & MODULE SOURCE EXPORTER
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

    if (!td->is_consuming && td->parser_intr == INTR_PARSE_I64 && td->field_count > 0) {
        int v_tag = (int)llround(mem_read_pool(&eng->mem, pool_idx));
        uint8_t i = 0;
        while (i < td->field_count) {
            uint16_t vfd_i = (uint16_t)(td->start_idx + i);
            const FieldDef *vfd = &eng->sym.fields[vfd_i];
            if (vfd->type_id == type_id) {
                int cur_tag  = (int)(vfd->flat_offset & 0xFFu);
                uint8_t pcnt = (uint8_t)(vfd->flat_offset >> 8u);
                if (cur_tag == v_tag) {
                    size_t used = 0;
                    if (cap > 0) buf[0] = '\0';
                    eng_buf_append(buf, cap, &used, "%s.%.*s",
                                   tname, (int)vfd->name.len, mem_str_ptr(&eng->mem, vfd->name));
                    if (pcnt > 0) {
                        eng_buf_append(buf, cap, &used, "(");
                        for (uint8_t pi = 0; pi < pcnt && used + 1 < cap; ++pi) {
                            const FieldDef *pfd = &eng->sym.fields[vfd_i + 1u + pi];
                            char p_str[64];
                            if (!eng->sym.types[pfd->type_id].is_consuming) {
                                format_struct_at(eng, pfd->type_id,
                                                 (uint16_t)(pool_idx + pfd->flat_offset),
                                                 (uint8_t)(depth + 1), p_str, sizeof(p_str));
                            } else if (pfd->type_id == eng->mem.str_type_id && eng->mem.str_type_id != 0) {
                                uint32_t pk = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(pool_idx + pfd->flat_offset));
                                StrSlice s = { (uint16_t)(pk >> 16u), (uint16_t)(pk & 0xFFFFu) };
                                snprintf(p_str, sizeof(p_str), "\"%.*s\"", (int)s.len, mem_str_ptr(&eng->mem, s));
                            } else {
                                snprintf(p_str, sizeof(p_str), "%.6g",
                                         mem_read_pool(&eng->mem, (uint16_t)(pool_idx + pfd->flat_offset)));
                            }
                            eng_buf_append(buf, cap, &used, "%s%s", (pi > 0 ? ", " : ""), p_str);
                        }
                        eng_buf_append(buf, cap, &used, ")");
                    }
                    return;
                }
                i = (uint8_t)(i + 1u + pcnt);
            } else {
                i++;
            }
        }
    }

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
        } else if (fd->type_id == eng->mem.str_type_id && eng->mem.str_type_id != 0) {
            uint32_t packed = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(pool_idx + fd->flat_offset));
            StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
            snprintf(f_str, sizeof(f_str), "\"%.*s\"", (int)s.len, mem_str_ptr(&eng->mem, s));
        } else if (eng->sym.types[fd->type_id].parser_intr == INTR_PARSE_I64 &&
                   eng->sym.types[fd->type_id].field_count > 0) {
            const TypeDef *etd = &eng->sym.types[fd->type_id];
            int ev = (int)llround(mem_read_pool(&eng->mem, (uint16_t)(pool_idx + fd->flat_offset)));
            bool found_ev = false;
            for (uint8_t vi = 0; vi < etd->field_count; ++vi) {
                const FieldDef *vfd = &eng->sym.fields[etd->start_idx + vi];
                if ((int)(int16_t)vfd->flat_offset == ev) {
                    snprintf(f_str, sizeof(f_str), "%.*s.%.*s",
                             (int)etd->name.len, mem_str_ptr(&eng->mem, etd->name),
                             (int)vfd->name.len, mem_str_ptr(&eng->mem, vfd->name));
                    found_ev = true;
                    break;
                }
            }
            if (!found_ev) snprintf(f_str, sizeof(f_str), "%d", ev);
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
        } else if (td->parser_intr == INTR_PARSE_I64 && td->field_count > 0) {
            int ev = (int)llround(val_get_f64(&eng->mem, val));
            bool found_ev = false;
            for (uint8_t vi = 0; vi < td->field_count; ++vi) {
                const FieldDef *vfd = &eng->sym.fields[td->start_idx + vi];
                if ((int)(int16_t)vfd->flat_offset == ev) {
                    snprintf(buf, buf_cap, "%.*s.%.*s",
                             (int)td->name.len, mem_str_ptr(&eng->mem, td->name),
                             (int)vfd->name.len, mem_str_ptr(&eng->mem, vfd->name));
                    found_ev = true;
                    break;
                }
            }
            if (!found_ev) snprintf(buf, buf_cap, "%d", ev);
        } else {
            snprintf(buf, buf_cap, "%.10g", val_get_f64(&eng->mem, val));
        }
    } else {
        format_struct_at(eng, tid, val_get_pool_idx(val), 0, buf, buf_cap);
    }
}

size_t eng_mod_format_vars(const Engine *eng, const char *mod_name,
                           char *out_buf, size_t buf_cap) {
    if (!out_buf || buf_cap == 0) return 0;
    out_buf[0] = '\0';
    if (!eng) return 0;

    const char *eff = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    int mid = sym_find_mod(&eng->sym, &eng->mem, eff);
    if (mid < 0) return 0;
    uint16_t mod_id = (uint16_t)mid;

    size_t used = 0;
    const ModVarDef *vars = sym_vars(&eng->sym);
    for (uint16_t i = 0; i < eng->sym.var_count && used + 1 < buf_cap; ++i) {
        if (vars[i].mod_id != mod_id) continue;
        if (val_get_type(&eng->mem, vars[i].val) == MEM_TYPE_VOID) continue;

        StrSlice vn = (vars[i].sym_id < eng->sym.sym_count)
                      ? eng->sym.syms[vars[i].sym_id] : (StrSlice){0, 0};
        char tbuf[32], ubuf[32], vbuf[512];
        eng_type_name(eng, eng_val_type(eng, vars[i].val), tbuf, sizeof(tbuf));
        eng_unit_name(eng, eng_val_unit(eng, vars[i].val), ubuf, sizeof(ubuf));
        eng_format_value(eng, vars[i].val, vbuf, sizeof(vbuf));

        const char *mut_s = (vars[i].flags & SYM_VAR_MUTABLE) ? "mut" : "const";
        const char *vis_s = (vars[i].flags & SYM_VAR_PRIVATE) ? "priv" : "pub";
        eng_buf_append(out_buf, buf_cap, &used, "%s%.*s|%s|%s|%s|%s|%s",
                       (used > 0 ? "\n" : ""),
                       (int)vn.len, mem_str_ptr(&eng->mem, vn),
                       tbuf, ubuf, mut_s, vis_s, vbuf);
    }
    return used;
}

static const char *find_base_unit_op_sym(const Engine *eng, uint8_t uid, uint16_t *out_len) {
    if (out_len) *out_len = 0;
    if (!eng || uid == MEM_UNIT_NONE) return NULL;
    for (uint16_t i = 0; i < eng->sym.overload_count; ++i) {
        const OpOverload *ov = &eng->sym.overloads[i];
        if (ov->fixity != FIX_POSTFIX || ov->ret_unit != uid ||
            !sym_mod_enabled(&eng->sym, ov->mod_id) || ov->body_ast == MEM_NO_IDX) {
            continue;
        }
        const AstNode *b = mem_get_ast(&eng->mem, ov->body_ast);
        if (b && b->kind == AST_SLOT_LOAD) {
            StrSlice s = eng->sym.syms[ov->sym_id];
            if (out_len) *out_len = s.len;
            return mem_str_ptr(&eng->mem, s);
        }
    }
    return NULL;
}

static void export_escaped_str(const Engine *eng, StrSlice s, char *buf, size_t cap, size_t *used) {
    const char *p = mem_str_ptr(&eng->mem, s);
    eng_buf_append(buf, cap, used, "\"");
    for (uint16_t i = 0; i < s.len && *used + 2 < cap; ++i) {
        char c = p[i];
        if (c == '\\')      eng_buf_append(buf, cap, used, "\\\\");
        else if (c == '"')  eng_buf_append(buf, cap, used, "\\\"");
        else if (c == '\n') eng_buf_append(buf, cap, used, "\\n");
        else if (c == '\t') eng_buf_append(buf, cap, used, "\\t");
        else if (c == '\r') eng_buf_append(buf, cap, used, "\\r");
        else                eng_buf_append(buf, cap, used, "%c", c);
    }
    eng_buf_append(buf, cap, used, "\"");
}

static void export_val_expr(Engine *eng, Value val, uint8_t depth,
                            char *buf, size_t cap, size_t *used) {
    if (depth > 16) {
        eng_buf_append(buf, cap, used, "0");
        return;
    }
    uint16_t tid = val_get_type(&eng->mem, val);
    uint8_t  uid = val_get_unit(&eng->mem, val);
    if (tid == 0 || tid >= eng->sym.type_count) {
        eng_buf_append(buf, cap, used, "0");
        return;
    }

    uint16_t uop_len = 0;
    const char *uop_ptr = find_base_unit_op_sym(eng, uid, &uop_len);

    const TypeDef *td = &eng->sym.types[tid];
    if (td->is_consuming) {
        if (td->parser_intr == INTR_PARSE_STR_SLICE) {
            export_escaped_str(eng, val_get_str(&eng->mem, val), buf, cap, used);
            return;
        }
        double d = val_get_f64(&eng->mem, val);
        if (d == 0.0) d = 0.0;
        if (td->parser_intr == INTR_PARSE_F64_TRIM1) {
            if (d < 0.0) eng_buf_append(buf, cap, used, "(%.17gi)", d);
            else         eng_buf_append(buf, cap, used, "%.17gi", d);
        } else {
            if (d < 0.0) eng_buf_append(buf, cap, used, "(%.17g)", d);
            else         eng_buf_append(buf, cap, used, "%.17g", d);
        }
        if (uop_ptr && uop_len > 0) {
            eng_buf_append(buf, cap, used, " %.*s", (int)uop_len, uop_ptr);
        }
        return;
    }

    uint16_t pidx = val_get_pool_idx(val);

    if (tid == eng->mem.vec_type_id && tid != 0) {
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 0));
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8u) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 3));
        uint16_t stride   = (elem_tid > 0 && elem_tid < eng->sym.type_count)
                            ? eng->sym.types[elem_tid].flat_size : 1;

        char et_name[32] = "", eu_name[32] = "";
        if (elem_tid != 0) eng_type_name(eng, elem_tid, et_name, sizeof(et_name));
        if (elem_uid != 0) eng_unit_name(eng, elem_uid, eu_name, sizeof(eu_name));

        if (elem_tid != 0 && elem_uid != 0) {
            eng_buf_append(buf, cap, used, "vec<%s, %s>(", et_name, eu_name);
        } else if (elem_tid != 0) {
            eng_buf_append(buf, cap, used, "vec<%s>(", et_name);
        } else if (elem_uid != 0) {
            eng_buf_append(buf, cap, used, "vec<%s>(", eu_name);
        } else {
            eng_buf_append(buf, cap, used, "vec(");
        }

        for (uint32_t i = 0; i < len && *used + 1 < cap; ++i) {
            if (i > 0) eng_buf_append(buf, cap, used, ", ");
            uint32_t off = i * (uint32_t)stride;
            Value elem_v;
            if (elem_tid == eng->mem.str_type_id && elem_tid != 0) {
                uint32_t packed = (uint32_t)mem_data_read(&eng->mem, data_idx, off);
                StrSlice s = { (uint16_t)(packed >> 16), (uint16_t)(packed & 0xFFFFu) };
                elem_v = val_make_str(&eng->mem, elem_tid, s);
            } else if (elem_tid > 0 && elem_tid < eng->sym.type_count &&
                       !eng->sym.types[elem_tid].is_consuming) {
                elem_v = val_with_unit(&eng->mem,
                                       val_make_comp(&eng->mem, elem_tid, (uint16_t)(data_idx + off), stride),
                                       elem_uid);
            } else {
                elem_v = val_make_f64_unit(&eng->mem,
                                           elem_tid ? elem_tid : eng->mem.fast_real_type,
                                           elem_uid,
                                           mem_data_read(&eng->mem, data_idx, off));
            }
            export_val_expr(eng, elem_v, (uint8_t)(depth + 1), buf, cap, used);
        }
        eng_buf_append(buf, cap, used, ")");
        return;
    }

    if (tid == eng->mem.mat_type_id && tid != 0) {
        char mbuf[2048];
        eng_format_mat_at(eng, pidx, true, mbuf, sizeof(mbuf));
        if (uop_ptr && uop_len > 0) {
            eng_buf_append(buf, cap, used, "(%s * 1 %.*s)", mbuf, (int)uop_len, uop_ptr);
        } else {
            eng_buf_append(buf, cap, used, "%s", mbuf);
        }
        return;
    }

    if (tid == eng->mem.map_type_id && tid != 0) {
        uint16_t len      = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 3));
        if (len == 0) {
            eng_buf_append(buf, cap, used, "map()");
            return;
        }
        if (len <= 3) {
            eng_buf_append(buf, cap, used, "dict(");
            for (uint16_t i = 0; i < len && *used + 1 < cap; ++i) {
                if (i > 0) eng_buf_append(buf, cap, used, ", ");
                export_escaped_str(eng, mem_map_entry_key(&eng->mem, data_idx, i), buf, cap, used);
                eng_buf_append(buf, cap, used, ", ");
                export_val_expr(eng, mem_map_entry_val(&eng->mem, data_idx, i),
                                (uint8_t)(depth + 1), buf, cap, used);
            }
            eng_buf_append(buf, cap, used, ")");
            return;
        }
        eng_buf_append(buf, cap, used, "{ __m := map()");
        for (uint16_t i = 0; i < len && *used + 1 < cap; ++i) {
            eng_buf_append(buf, cap, used, " ; __m[");
            export_escaped_str(eng, mem_map_entry_key(&eng->mem, data_idx, i), buf, cap, used);
            eng_buf_append(buf, cap, used, "] = ");
            export_val_expr(eng, mem_map_entry_val(&eng->mem, data_idx, i),
                            (uint8_t)(depth + 1), buf, cap, used);
        }
        eng_buf_append(buf, cap, used, " ; __m }");
        return;
    }

    if (tid == eng->mem.fn_type_id && tid != 0) {
        uint16_t body_ast = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 0));
        for (int k = (int)eng->mem.fn_src_count - 1; k >= 0; --k) {
            if (eng->mem.fn_body_ast[k] == body_ast) {
                StrSlice s = eng->mem.fn_src_slice[k];
                eng_buf_append(buf, cap, used, "%.*s", (int)s.len, mem_str_ptr(&eng->mem, s));
                return;
            }
        }
        eng_buf_append(buf, cap, used, "(x) => x");
        return;
    }

    if (td->parser_intr == INTR_COMPLEX_TYPE && td->flat_size == 2) {
        double re = mem_read_pool(&eng->mem, pidx);
        double im = mem_read_pool(&eng->mem, (uint16_t)(pidx + 1));
        if (re == 0.0) re = 0.0;
        if (im == 0.0) im = 0.0;
        if (im >= 0.0) {
            eng_buf_append(buf, cap, used, "(%.17g + %.17gi)", re, im);
        } else {
            eng_buf_append(buf, cap, used, "(%.17g - %.17gi)", re, -im);
        }
        if (uop_ptr && uop_len > 0) {
            eng_buf_append(buf, cap, used, " * 1 %.*s", (int)uop_len, uop_ptr);
        }
        return;
    }

    char tname[32];
    eng_type_name(eng, tid, tname, sizeof(tname));
    eng_buf_append(buf, cap, used, "%s(", tname);
    for (uint8_t i = 0; i < td->field_count && *used + 1 < cap; ++i) {
        if (i > 0) eng_buf_append(buf, cap, used, ", ");
        const FieldDef *fd = &eng->sym.fields[td->start_idx + i];
        uint16_t fpidx = (uint16_t)(pidx + fd->flat_offset);
        Value fval;
        if (!eng->sym.types[fd->type_id].is_consuming) {
            fval = val_with_unit(&eng->mem,
                                 val_make_comp(&eng->mem, fd->type_id, fpidx, eng->sym.types[fd->type_id].flat_size),
                                 uid);
        } else if (fd->type_id == eng->mem.str_type_id && eng->mem.str_type_id != 0) {
            uint32_t packed = (uint32_t)mem_read_pool(&eng->mem, fpidx);
            StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
            fval = val_make_str(&eng->mem, fd->type_id, s);
        } else {
            fval = val_make_f64_unit(&eng->mem, fd->type_id, uid, mem_read_pool(&eng->mem, fpidx));
        }
        export_val_expr(eng, fval, (uint8_t)(depth + 1), buf, cap, used);
    }
    eng_buf_append(buf, cap, used, ")");
}

size_t eng_mod_export_source(Engine *eng, const char *mod_name,
                             char *out_buf, size_t buf_cap) {
    if (!out_buf || buf_cap == 0) return 0;
    out_buf[0] = '\0';
    if (!eng) return 0;

    const char *eff = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    int mid = sym_find_mod(&eng->sym, &eng->mem, eff);
    if (mid < 0) return 0;
    uint16_t mod_id = (uint16_t)mid;

    size_t used = 0;
    if (strcmp(eff, "default") != 0) {
        eng_buf_append(out_buf, buf_cap, &used, "[%s]\n", eff);
    }

    const ModVarDef *vars = sym_vars(&eng->sym);
    for (uint16_t i = 0; i < eng->sym.var_count && used + 1 < buf_cap; ++i) {
        const ModVarDef *mv = &vars[i];
        if (mv->mod_id != mod_id) continue;
        if (val_get_type(&eng->mem, mv->val) == MEM_TYPE_VOID) continue;
        if (mv->sym_id >= eng->sym.sym_count) continue;

        StrSlice vn = eng->sym.syms[mv->sym_id];
        if ((mv->flags & SYM_VAR_PRIVATE) != 0) {
            eng_buf_append(out_buf, buf_cap, &used, "priv ");
        }
        eng_buf_append(out_buf, buf_cap, &used, "%.*s", (int)vn.len, mem_str_ptr(&eng->mem, vn));

        uint16_t ltype = (uint16_t)(mv->lock_type & 0xFFu);
        uint8_t  lunit = mv->lock_unit;
        if (ltype == eng->mem.fn_type_id) ltype = 0;

        if (ltype != 0 && lunit != 0 && lunit < SYM_UNIT_LOCK_NONE) {
            char lt_s[32], lu_s[32];
            eng_type_name(eng, ltype, lt_s, sizeof(lt_s));
            eng_unit_name(eng, lunit, lu_s, sizeof(lu_s));
            eng_buf_append(out_buf, buf_cap, &used, " : %s, %s", lt_s, lu_s);
        } else if (ltype != 0) {
            char lt_s[32];
            eng_type_name(eng, ltype, lt_s, sizeof(lt_s));
            eng_buf_append(out_buf, buf_cap, &used, " : %s", lt_s);
        } else if (lunit != 0 && lunit < SYM_UNIT_LOCK_NONE) {
            char lu_s[32];
            eng_unit_name(eng, lunit, lu_s, sizeof(lu_s));
            eng_buf_append(out_buf, buf_cap, &used, " : %s", lu_s);
        }

        eng_buf_append(out_buf, buf_cap, &used, "%s",
                       (mv->flags & SYM_VAR_MUTABLE) ? " := " : " = ");
        export_val_expr(eng, mv->val, 0, out_buf, buf_cap, &used);
        eng_buf_append(out_buf, buf_cap, &used, "\n");
    }
    return used;
}

const char *eng_get_output(const Engine *eng) {
    if (!eng) return "";
    return eng->mem.out_buf;
}

void eng_clear_output(Engine *eng) {
    if (!eng) return;
    mem_out_clear(&eng->mem);
}

void eng_set_import_dir(Engine *eng, const char *dir_path) {
    if (!eng) return;
    if (!dir_path) {
        eng->mem.import_dir[0] = '\0';
        return;
    }
    snprintf(eng->mem.import_dir, sizeof(eng->mem.import_dir), "%s", dir_path);
}

bool eng_import_file(Engine *eng, const char *file_path) {
    if (!eng || !file_path || file_path[0] == '\0') return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    return ext_import_module_or_file(&eng->mem, &eng->sym,
                                     file_path, (uint16_t)strlen(file_path),
                                     true, NULL);
}

uint32_t eng_bc_steps(const Engine *eng) {
    return eng ? eng->mem.bc_exec_steps : 0u;
}

uint16_t eng_bc_inst_count(const Engine *eng) {
    return eng ? eng->mem.bc_count : 0u;
}

size_t eng_bc_disasm(Engine *eng, const char *fn_name, char *out_buf, size_t buf_cap) {
    if (!out_buf || buf_cap == 0) return 0;
    out_buf[0] = '\0';
    if (!eng || !fn_name || fn_name[0] == '\0') return 0;
    return bc_disasm_func(&eng->mem, &eng->sym, fn_name, (uint16_t)strlen(fn_name), out_buf, buf_cap);
}
