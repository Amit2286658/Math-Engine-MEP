/*
 * math_engine.c — Top-Level Engine Wrapper, Module Execution & Value Formatter
 *
 * Every engine boots BLANK ("empty engine") and its host loads the precompiled
 * standard library image (`stdlib.mathc`) separately in O(1) `memcpy` time via
 * the user-loader (`Core/compile/user_loader.h`, JNI `nativeLoadMathc()`).
 */

#include "math_engine.h"
#include "../lib/delegate/delegate.h"
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
    return eng_create_blank();
}

void eng_destroy(Engine *eng) {
    if (!eng) return;
    sym_free_externs(&eng->sym);
    mem_free_all_heap(&eng->mem);
    mem_free_ast_heap(&eng->mem);
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

    if (ext_src_has_module_constructs(&eng->mem, &eng->sym, expr, (uint16_t)raw_len)) {
        return eng_build_and_run(eng, &expr, 1, out_val);
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

void eng_snapshot_base(Engine *eng) {
    if (!eng) return;
    eng->mem.sym_tab = &eng->sym;
    sym_snapshot_base(&eng->sym, &eng->mem);
}

bool eng_flush_user_modules(Engine *eng) {
    if (!eng) return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    return sym_flush_user_modules(&eng->sym, &eng->mem);
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

void eng_format_mat_at(const Engine *eng, uint16_t hdr_idx, bool high_prec,
                       char *buf, size_t cap) {
    if (!eng || !buf || cap == 0) return;
    delegate_format_mat_at(&eng->mem, hdr_idx, high_prec, buf, cap);
}

void eng_format_value(const Engine *eng, Value val, char *buf, size_t buf_cap) {
    if (!eng || !buf || buf_cap == 0) return;
    delegate_format_value(&eng->mem, &eng->sym, val, buf, buf_cap);
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
        const OpOverload *ov = &sym_overloads(&eng->sym)[i];
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
        uint16_t stride   = (elem_tid != SYM_TYPE_ANY && elem_tid > 0 && elem_tid < eng->sym.type_count)
                            ? eng->sym.types[elem_tid].flat_size : 1;

        char et_name[32] = "", eu_name[32] = "";
        if (elem_tid == SYM_TYPE_ANY) snprintf(et_name, sizeof(et_name), "any");
        else if (elem_tid != 0) eng_type_name(eng, elem_tid, et_name, sizeof(et_name));
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
            Value elem_v = delegate_vec_read_elem(&eng->mem, &eng->sym, elem_tid, elem_uid, data_idx, off);
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
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 0));
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8u) & 0x7Fu);
        uint16_t len      = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 3));
        char et_name[32] = "", eu_name[32] = "";
        if (elem_tid == SYM_TYPE_ANY) snprintf(et_name, sizeof(et_name), "any");
        else if (elem_tid != 0) eng_type_name(eng, elem_tid, et_name, sizeof(et_name));
        if (elem_uid != 0) eng_unit_name(eng, elem_uid, eu_name, sizeof(eu_name));

        if (len == 0) {
            if (elem_tid != 0 && elem_uid != 0) {
                eng_buf_append(buf, cap, used, "map<%s, %s>()", et_name, eu_name);
            } else if (elem_tid != 0) {
                eng_buf_append(buf, cap, used, "map<%s>()", et_name);
            } else if (elem_uid != 0) {
                eng_buf_append(buf, cap, used, "map<%s>()", eu_name);
            } else {
                eng_buf_append(buf, cap, used, "map()");
            }
            return;
        }
        if (len <= 3) {
            if (elem_tid == SYM_TYPE_ANY) {
                eng_buf_append(buf, cap, used, "dict<any>(");
            } else {
                eng_buf_append(buf, cap, used, "dict(");
            }
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
        if (elem_tid == SYM_TYPE_ANY) {
            eng_buf_append(buf, cap, used, "{ __m := map<any>()");
        } else {
            eng_buf_append(buf, cap, used, "{ __m := map()");
        }
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
        uint16_t ftid  = sym_unpack_outer_type(&eng->mem, fd->type_id);
        uint16_t fpidx = (uint16_t)(pidx + fd->flat_offset);
        Value fval;
        if (!eng->sym.types[ftid].is_consuming) {
            fval = val_with_unit(&eng->mem,
                                 val_make_comp(&eng->mem, ftid, fpidx, eng->sym.types[ftid].flat_size),
                                 uid);
        } else if (ftid == eng->mem.str_type_id && eng->mem.str_type_id != 0) {
            uint32_t packed = (uint32_t)mem_read_pool(&eng->mem, fpidx);
            StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
            fval = val_make_str(&eng->mem, ftid, s);
        } else {
            fval = val_make_f64_unit(&eng->mem, ftid, uid, mem_read_pool(&eng->mem, fpidx));
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

bool eng_has_entry(const Engine *eng) {
    return eng && eng->sym.entry_ov_idx != MEM_NO_IDX &&
           eng->sym.entry_ov_idx < eng->sym.overload_count;
}

bool eng_run_entry(Engine *eng, Value *out_val) {
    if (out_val) *out_val = val_void();
    if (!eng) return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    mem_rewind_scratch(&eng->mem);
    if (eng->sym.entry_ov_idx == MEM_NO_IDX ||
        eng->sym.entry_ov_idx >= eng->sym.overload_count) {
        mem_err(&eng->mem, "No project entry point ('@entry' or 'main()') is defined");
        return false;
    }
    const OpOverload *ov = &sym_overloads(&eng->sym)[eng->sym.entry_ov_idx];
    uint16_t prev_mem_mod = eng->mem.cur_mod;
    uint16_t prev_tab_mod = eng->sym.cur_mod;
    eng->mem.cur_mod = ov->mod_id;
    eng->sym.cur_mod = ov->mod_id;
    Value res = eval_dispatch_overload(&eng->mem, &eng->sym, ov->sym_id, FIX_FUNC, 0, NULL);
    eng->mem.cur_mod = prev_mem_mod;
    eng->sym.cur_mod = prev_tab_mod;
    if (eng->mem.has_error) return false;
    eng->mem.last_mod_val = res;
    if (out_val) *out_val = res;
    return true;
}

bool eng_build_and_run(Engine *eng, const char *const *sources,
                       size_t unit_count, Value *out_val) {
    if (out_val) *out_val = val_void();
    if (!eng) return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    mem_rewind_scratch(&eng->mem);
    if (!sources || unit_count == 0) {
        mem_err(&eng->mem, "Null or empty source unit list");
        return false;
    }
    if (eng->sym.base_mod_count > 0 &&
        (eng->sym.mod_count > eng->sym.base_mod_count ||
         eng->mem.perm_ast_count > eng->mem.base_ast_count ||
         eng->sym.overload_count > eng->sym.base_overload_count ||
         eng->sym.type_count > eng->sym.base_type_count)) {
        (void)sym_flush_user_modules(&eng->sym, &eng->mem);
    }
    if (!ext_compile_and_link(&eng->mem, &eng->sym, sources, unit_count)) {
        return false;
    }
    if (out_val) *out_val = eng->mem.last_mod_val;
    return !eng->mem.has_error;
}

bool eng_apply_patches(Engine *eng, const char *const *patch_sources,
                       size_t patch_count) {
    if (!eng) return false;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    mem_rewind_scratch(&eng->mem);
    if (!patch_sources && patch_count > 0) {
        mem_err(&eng->mem, "Null patch source array");
        return false;
    }
    if (patch_count == 0) return true;
    ExtBuildCtx *ctx = ext_build_create();
    if (!ctx) {
        mem_err(&eng->mem, "Out of memory allocating patch build context");
        return false;
    }
    ctx->allow_redef = true;
    for (size_t i = 0; i < patch_count; ++i) {
        if (!ext_compile_unit(&eng->mem, &eng->sym, ctx, patch_sources[i])) {
            ext_build_destroy(ctx);
            return false;
        }
    }
    bool ok = ext_link_units(&eng->mem, &eng->sym, ctx);
    ext_build_destroy(ctx);
    return ok;
}

/* ============================================================================
 * NATIVE BATCH 2D PLOT SAMPLER & LIVE SYMTABLE AUTOCOMPLETE CATALOG EXPORTER
 * ============================================================================ */

uint16_t eng_sample_plot(Engine *eng, const char *mod_name,
                         const char *expr, double x_min,
                         double x_max, uint16_t steps,
                         double *out_xy, uint16_t max_pairs) {
    if (!eng || !expr || !out_xy || max_pairs == 0) return 0;
    eng->mem.sym_tab = &eng->sym;
    mem_clear_err(&eng->mem);
    mem_rewind_scratch(&eng->mem);

    const char *eff_mod = (mod_name && mod_name[0] != '\0') ? mod_name : "default";
    int mid = sym_find_mod(&eng->sym, &eng->mem, eff_mod);
    uint16_t target_mod = (mid >= 0) ? (uint16_t)mid : 0u;
    uint16_t prev_mem_mod = eng->mem.cur_mod;
    uint16_t prev_tab_mod = eng->sym.cur_mod;
    eng->mem.cur_mod = target_mod;
    eng->sym.cur_mod = target_mod;

    while (*expr == ' ' || *expr == '\t' || *expr == '\r' || *expr == '\n') expr++;
    size_t raw_len = strlen(expr);
    while (raw_len > 0 && (expr[raw_len - 1] == ' ' || expr[raw_len - 1] == '\t' ||
                           expr[raw_len - 1] == '\r' || expr[raw_len - 1] == '\n')) {
        raw_len--;
    }
    if (raw_len == 0 || raw_len > 16000u) {
        eng->mem.cur_mod = prev_mem_mod;
        eng->sym.cur_mod = prev_tab_mod;
        return 0;
    }

    char clean_expr[16384];
    memcpy(clean_expr, expr, raw_len);
    clean_expr[raw_len] = '\0';

    bool is_simple_ident = true;
    for (size_t i = 0; i < raw_len; ++i) {
        unsigned char c = (unsigned char)clean_expr[i];
        if (i == 0 ? !lex_is_ident_start(c) : !lex_is_ident_cont(c)) {
            is_simple_ident = false;
            break;
        }
    }

    char eval_src[16448];
    snprintf(eval_src, sizeof(eval_src), "%s", clean_expr);

    if (is_simple_ident) {
        int sid = sym_find(&eng->sym, &eng->mem, clean_expr, (uint16_t)raw_len);
        if (sid >= 0) {
            int vidx = sym_find_var(&eng->sym, (uint16_t)sid, target_mod, NULL);
            if (vidx >= 0) {
                Value dv = sym_vars(&eng->sym)[vidx].val;
                uint16_t dtid = val_get_type(&eng->mem, dv);
                if (dtid == eng->mem.vec_type_id && dtid != 0) {
                    uint16_t pidx     = val_get_pool_idx(dv);
                    uint16_t raw_hdr0 = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 0));
                    uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
                    uint32_t vlen     = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 1));
                    uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 3));
                    uint16_t stride   = (elem_tid > 0 && elem_tid < eng->sym.type_count)
                                        ? eng->sym.types[elem_tid].flat_size : 1u;
                    uint16_t out_cnt  = 0;
                    for (uint32_t i = 0; i < vlen && out_cnt < max_pairs; ++i) {
                        uint32_t off = i * (uint32_t)stride;
                        double px = (double)i;
                        double py = 0.0;
                        if (stride == 2 && elem_tid > 0 && elem_tid < eng->sym.type_count &&
                            !eng->sym.types[elem_tid].is_consuming) {
                            px = mem_data_read(&eng->mem, data_idx, off);
                            py = mem_data_read(&eng->mem, data_idx, off + 1u);
                        } else {
                            py = mem_data_read(&eng->mem, data_idx, off);
                        }
                        if (isfinite(px) && isfinite(py)) {
                            out_xy[out_cnt * 2u]      = px;
                            out_xy[out_cnt * 2u + 1u] = py;
                            out_cnt++;
                        }
                    }
                    eng->mem.cur_mod = prev_mem_mod;
                    eng->sym.cur_mod = prev_tab_mod;
                    return out_cnt;
                } else if (dtid == eng->mem.fn_type_id && dtid != 0) {
                    snprintf(eval_src, sizeof(eval_src), "%s(x)", clean_expr);
                }
            } else {
                for (uint16_t oi = eng->sym.sym_first_ov[sid]; oi != MEM_NO_IDX; oi = sym_ov_next(&eng->sym)[oi]) {
                    const OpOverload *ov = &sym_overloads(&eng->sym)[oi];
                    if (ov->fixity == FIX_FUNC && ov->param_count == 1 && sym_ov_is_visible(&eng->sym, ov)) {
                        snprintf(eval_src, sizeof(eval_src), "%s(x)", clean_expr);
                        break;
                    }
                }
            }
        }
    }

    uint16_t saved_ast  = eng->mem.ast_count;
    uint16_t saved_pool = eng->mem.pool_top;
    uint16_t saved_src  = eng->mem.src_len;

    Parser P;
    parser_init(&P, &eng->mem, &eng->sym, eval_src, (uint16_t)strlen(eval_src), 0, true, false);
    P.cur_mod      = target_mod;
    P.is_mod_scope = false;
    uint8_t real_tid = eng->mem.fast_real_type ? eng->mem.fast_real_type : 1u;
    (void)parser_add_local(&P, "x", 1, real_tid);
    (void)parser_add_local(&P, "t", 1, real_tid);

    lex_next(&P.lex);
    uint16_t root = parser_parse_expr(&P, 0);
    if (!eng->mem.has_error && P.lex.cur.kind != TOK_EOF) {
        mem_err(&eng->mem, "Unexpected trailing token '%.*s'",
                (int)P.lex.cur.len, P.lex.src + P.lex.cur.off);
    }
    if (!parser_check_labels(&P) || eng->mem.has_error || root == MEM_NO_IDX) {
        eng->mem.ast_count = saved_ast;
        eng->mem.cur_mod   = prev_mem_mod;
        eng->sym.cur_mod   = prev_tab_mod;
        return 0;
    }

    uint16_t saved_labels[MEM_MAX_LABELS];
    memcpy(saved_labels, eng->mem.label_ast, sizeof(saved_labels));
    for (uint8_t i = 0; i < MEM_MAX_LABELS; ++i) {
        eng->mem.label_ast[i] = (i < P.label_count) ? P.labels[i].body_ast : MEM_NO_IDX;
    }

    uint8_t slot_cnt = (P.local_count >= 2u) ? P.local_count : 2u;
    uint16_t frame_base = mem_push_frame(&eng->mem, slot_cnt);
    if (eng->mem.has_error) {
        memcpy(eng->mem.label_ast, saved_labels, sizeof(saved_labels));
        eng->mem.ast_count = saved_ast;
        eng->mem.cur_mod   = prev_mem_mod;
        eng->sym.cur_mod   = prev_tab_mod;
        return 0;
    }

    double lo = (x_min < x_max) ? x_min : (x_max - 1.0);
    double hi = (x_max > x_min) ? x_max : (x_min + 10.0);
    uint16_t n = (steps < 2u) ? 2u : ((steps > max_pairs) ? max_pairs : steps);
    double dx = (hi - lo) / (double)(n - 1u);

    uint16_t post_parse_pool = eng->mem.pool_top;
    uint16_t post_parse_src  = eng->mem.src_len;
    uint16_t out_cnt = 0;

    for (uint16_t i = 0; i < n && out_cnt < max_pairs; ++i) {
        double xv = lo + (double)i * dx;
        Value xval = val_make_f64(&eng->mem, real_tid, xv);
        mem_set_slot(&eng->mem, frame_base, 0, xval);
        mem_set_slot(&eng->mem, frame_base, 1, xval);
        for (uint8_t s = 2; s < slot_cnt; ++s) {
            mem_set_slot(&eng->mem, frame_base, s, val_make_f64(&eng->mem, real_tid, 0.0));
        }
        eng->mem.eval_steps  = 0;
        eng->mem.eval_depth  = 0;
        eng->mem.call_depth  = 0;
        eng->mem.jump_active = JUMP_NONE;
        mem_clear_err(&eng->mem);

        Value res = eval_ast(&eng->mem, &eng->sym, root, frame_base, 0);
        if (eng->mem.jump_active == JUMP_RETURN) {
            res = eng->mem.jump_val;
            eng->mem.jump_active = JUMP_NONE;
        }
        if (!eng->mem.has_error) {
            uint16_t rtid = val_get_type(&eng->mem, res);
            if (rtid == real_tid) {
                double yv = val_get_f64(&eng->mem, res);
                if (isfinite(xv) && isfinite(yv)) {
                    out_xy[out_cnt * 2u]      = xv;
                    out_xy[out_cnt * 2u + 1u] = yv;
                    out_cnt++;
                }
            } else if (i == 0 && rtid == eng->mem.vec_type_id && rtid != 0) {
                uint16_t pidx     = val_get_pool_idx(res);
                uint16_t raw_hdr0 = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 0));
                uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
                uint32_t vlen     = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 1));
                uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(pidx + 3));
                uint16_t stride   = (elem_tid > 0 && elem_tid < eng->sym.type_count)
                                    ? eng->sym.types[elem_tid].flat_size : 1u;
                for (uint32_t k = 0; k < vlen && out_cnt < max_pairs; ++k) {
                    uint32_t off = k * (uint32_t)stride;
                    double px = (double)k;
                    double py = (stride == 2 && elem_tid > 0 && !eng->sym.types[elem_tid].is_consuming)
                                ? mem_data_read(&eng->mem, data_idx, off + 1u)
                                : mem_data_read(&eng->mem, data_idx, off);
                    if (stride == 2 && elem_tid > 0 && !eng->sym.types[elem_tid].is_consuming) {
                        px = mem_data_read(&eng->mem, data_idx, off);
                    }
                    if (isfinite(px) && isfinite(py)) {
                        out_xy[out_cnt * 2u]      = px;
                        out_xy[out_cnt * 2u + 1u] = py;
                        out_cnt++;
                    }
                }
                break;
            }
        }
        eng->mem.pool_top = post_parse_pool;
        eng->mem.src_len  = post_parse_src;
    }

    mem_clear_err(&eng->mem);
    mem_pop_frame(&eng->mem, frame_base);
    memcpy(eng->mem.label_ast, saved_labels, sizeof(saved_labels));
    eng->mem.ast_count = saved_ast;
    eng->mem.pool_top  = saved_pool;
    eng->mem.src_len   = saved_src;
    eng->mem.cur_mod   = prev_mem_mod;
    eng->sym.cur_mod   = prev_tab_mod;
    return out_cnt;
}

size_t eng_export_catalog(const Engine *eng, char *out_buf, size_t buf_cap) {
    if (!out_buf || buf_cap == 0) return 0;
    out_buf[0] = '\0';
    if (!eng) return 0;

    size_t used = 0;
    const SymTable *tab = &eng->sym;
    const MemArena *mem = &eng->mem;
    const OpOverload *ovs = sym_overloads(tab);

    /* 1. Active Functions, Prefix/Infix/Postfix Operators & Unit Suffixes */
    for (uint16_t i = 0; i < tab->overload_count && used + 128u < buf_cap; ++i) {
        const OpOverload *ov = &ovs[i];
        if (ov->fixity == FIX_SYNTAX) continue;
        if (!sym_mod_enabled(tab, ov->mod_id)) continue;
        if ((ov->reserved & SYM_OV_PRIVATE) != 0 && ov->mod_id != tab->cur_mod) continue;
        if (ov->sym_id >= tab->sym_count) continue;

        StrSlice sym = tab->syms[ov->sym_id];
        if (sym.len == 0) continue;
        char sname[64];
        uint16_t slen = (sym.len < 60u) ? sym.len : 60u;
        memcpy(sname, mem_str_ptr(mem, sym), slen);
        sname[slen] = '\0';

        bool has_alnum = false;
        for (uint16_t c = 0; c < slen; ++c) {
            unsigned char ch = (unsigned char)sname[c];
            if (lex_is_ident_start(ch) || (ch >= '0' && ch <= '9')) {
                has_alnum = true;
                break;
            }
        }
        if (!has_alnum) continue;

        char mname[32] = "default";
        if (ov->mod_id < tab->mod_count) {
            StrSlice ms = sym_mod_name(tab, ov->mod_id);
            snprintf(mname, sizeof(mname), "%.*s", (int)ms.len, mem_str_ptr(mem, ms));
        }

        char rtype[32] = "real", runit[32] = "none";
        if (ov->ret_type == SYM_TYPE_ANY) {
            snprintf(rtype, sizeof(rtype), "any");
        } else if (ov->ret_type > 0 && (ov->ret_type < tab->type_count || sym_type_is_parameterized(ov->ret_type))) {
            eng_type_name(eng, ov->ret_type, rtype, sizeof(rtype));
        }
        if (ov->ret_unit > 0 && ov->ret_unit < SYM_UNIT_LOCK_NONE && ov->ret_unit < tab->unit_count) {
            eng_unit_name(eng, ov->ret_unit, runit, sizeof(runit));
        } else if (ov->ret_unit == SYM_UNIT_GENERIC) {
            snprintf(runit, sizeof(runit), "T");
        }

        const char *kind = "fn";
        char sig[256];
        if (ov->fixity == FIX_POSTFIX && ov->ret_unit > 0 && ov->ret_unit < SYM_UNIT_LOCK_NONE) {
            kind = "unit";
            snprintf(sig, sizeof(sig), "<x> %s -> %s [%s]", sname, rtype, runit);
        } else if (ov->fixity == FIX_POSTFIX) {
            kind = "op";
            snprintf(sig, sizeof(sig), "(x) %s -> %s", sname, rtype);
        } else if (ov->fixity == FIX_INFIX_LEFT || ov->fixity == FIX_INFIX_RIGHT) {
            kind = "op";
            snprintf(sig, sizeof(sig), "(a) %s (b) -> %s", sname, rtype);
        } else if (ov->fixity == FIX_PREFIX) {
            kind = "op";
            snprintf(sig, sizeof(sig), "%s (x) -> %s", sname, rtype);
        } else {
            kind = "fn";
            char pbuf[96] = "";
            size_t pused = 0;
            for (uint8_t p = 0; p < ov->param_count; ++p) {
                char pt[24] = "real";
                if (ov->param_types[p] == SYM_TYPE_ANY) snprintf(pt, sizeof(pt), "any");
                else if (ov->param_types[p] > 0 &&
                         (ov->param_types[p] < tab->type_count || sym_type_is_parameterized(ov->param_types[p]))) {
                    eng_type_name(eng, ov->param_types[p], pt, sizeof(pt));
                }
                eng_buf_append(pbuf, sizeof(pbuf), &pused, "%sp%u: %s",
                               (p > 0 ? ", " : ""), (unsigned)(p + 1), pt);
            }
            if (strcmp(runit, "none") != 0) {
                snprintf(sig, sizeof(sig), "%s(%s) -> %s, %s", sname, pbuf, rtype, runit);
            } else {
                snprintf(sig, sizeof(sig), "%s(%s) -> %s", sname, pbuf, rtype);
            }
        }

        eng_buf_append(out_buf, buf_cap, &used, "%s%s|%s|%s|%s|%s|%s",
                       (used > 0 ? "\n" : ""),
                       kind, sname, sig, rtype, runit, mname);
    }

    /* 2. Registered Types & Structs */
    for (uint16_t tid = 1; tid < tab->type_count && used + 128u < buf_cap; ++tid) {
        const TypeDef *td = &tab->types[tid];
        if (!sym_mod_enabled(tab, td->mod_id)) continue;
        char tname[32];
        eng_type_name(eng, tid, tname, sizeof(tname));
        if (tname[0] == '\0') continue;

        char mname[32] = "default";
        if (td->mod_id < tab->mod_count) {
            StrSlice ms = sym_mod_name(tab, td->mod_id);
            snprintf(mname, sizeof(mname), "%.*s", (int)ms.len, mem_str_ptr(mem, ms));
        }

        char sig[128];
        if (!td->is_consuming && td->field_count > 0) {
            char fbuf[80] = "";
            size_t fused = 0;
            for (uint8_t f = 0; f < td->field_count; ++f) {
                const FieldDef *fd = &tab->fields[td->start_idx + f];
                char ft[24] = "real";
                eng_type_name(eng, fd->type_id, ft, sizeof(ft));
                eng_buf_append(fbuf, sizeof(fbuf), &fused, "%s%.*s: %s",
                               (f > 0 ? ", " : ""),
                               (int)fd->name.len, mem_str_ptr(mem, fd->name), ft);
            }
            snprintf(sig, sizeof(sig), "struct %s(%s)", tname, fbuf);
        } else {
            snprintf(sig, sizeof(sig), "$%s", tname);
        }
        eng_buf_append(out_buf, buf_cap, &used, "%stype|%s|%s|%s|none|%s",
                       (used > 0 ? "\n" : ""), tname, sig, tname, mname);
    }

    /* 3. Registered Unit Groups */
    for (uint8_t uid = 1; uid < tab->unit_count && used + 96u < buf_cap; ++uid) {
        char uname[32];
        eng_unit_name(eng, uid, uname, sizeof(uname));
        if (uname[0] == '\0' || strcmp(uname, "none") == 0) continue;
        eng_buf_append(out_buf, buf_cap, &used, "%sunit|%s|unit group [%s]|real|%s|units",
                       (used > 0 ? "\n" : ""), uname, uname, uname);
    }

    /* 4. Active Module Constants & Public Variables */
    const ModVarDef *vars = sym_vars(tab);
    for (uint16_t v = 0; v < tab->var_count && used + 128u < buf_cap; ++v) {
        const ModVarDef *mv = &vars[v];
        if (!sym_mod_enabled(tab, mv->mod_id)) continue;
        if ((mv->flags & SYM_VAR_PRIVATE) != 0 && mv->mod_id != tab->cur_mod) continue;
        if (mv->sym_id >= tab->sym_count) continue;
        StrSlice vs = tab->syms[mv->sym_id];
        if (vs.len == 0) continue;

        char vname[48], vt[32], vu[32], mname[32] = "default";
        uint16_t vlen = (vs.len < 44u) ? vs.len : 44u;
        memcpy(vname, mem_str_ptr(mem, vs), vlen);
        vname[vlen] = '\0';
        uint16_t disp_vt = (mv->lock_type & 0xFFu) ? (uint16_t)(mv->lock_type & 0xFFu) : eng_val_type(eng, mv->val);
        eng_type_name(eng, disp_vt, vt, sizeof(vt));
        eng_unit_name(eng, eng_val_unit(eng, mv->val), vu, sizeof(vu));
        if (mv->mod_id < tab->mod_count) {
            StrSlice ms = sym_mod_name(tab, mv->mod_id);
            snprintf(mname, sizeof(mname), "%.*s", (int)ms.len, mem_str_ptr(mem, ms));
        }
        char sig[256];
        snprintf(sig, sizeof(sig), "%s : %s%s%s",
                 vname, vt, (strcmp(vu, "none") != 0 ? ", " : ""), (strcmp(vu, "none") != 0 ? vu : ""));
        eng_buf_append(out_buf, buf_cap, &used, "%sconst|%s|%s|%s|%s|%s",
                       (used > 0 ? "\n" : ""), vname, sig, vt, vu, mname);
    }

    /* 5. Loaded Modules */
    for (uint16_t m = 1; m < tab->mod_count && used + 96u < buf_cap; ++m) {
        StrSlice ms = sym_mod_name(tab, m);
        if (ms.len == 0) continue;
        char mname[32];
        snprintf(mname, sizeof(mname), "%.*s", (int)ms.len, mem_str_ptr(mem, ms));
        eng_buf_append(out_buf, buf_cap, &used, "%smod|%s|<[%s]>|module|none|%s",
                       (used > 0 ? "\n" : ""), mname, mname, mname);
    }

    return used;
}
