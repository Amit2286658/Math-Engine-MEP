/*
 * delegate_core.c — Intrinsic Opcode Registry, Dimensional Algebra & Higher-Order Function Dispatch
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

/* ============================================================================
 * 1. `@` INTRINSIC NAME -> OPCODE REGISTRY
 * ============================================================================ */

/*Below section is the C's native lookup table and resolver for operator/function delegates @<intrinsic>*/
/*Construct @f64, @lparen, @seq, @store, @add, @sin, @vnew, @vshift, @vpeek, @mmul, @calc_diff, @tostr, @fromstr*/
typedef struct {
    const char *name;
    IntrinsicId id;
} IntrEntry;

static const IntrEntry k_delegate_intrinsics[] = {
    { "f64",           INTR_PARSE_F64 },
    { "f64_trim1",     INTR_PARSE_F64_TRIM1 },
    { "i64",           INTR_PARSE_I64 },
    { "str_slice",     INTR_PARSE_STR_SLICE },
    { "vec_type",      INTR_VEC_TYPE },
    { "mat_type",      INTR_MAT_TYPE },
    { "fn_type",       INTR_FN_TYPE },
    { "map_type",      INTR_MAP_TYPE },
    { "complex_type",  INTR_COMPLEX_TYPE },
    { "any_type",      INTR_ANY_TYPE },
    { "none_unit",     INTR_NONE_UNIT },
    { "lparen",        INTR_LPAREN },
    { "rparen",        INTR_RPAREN },
    { "lbrace",        INTR_LBRACE },
    { "rbrace",        INTR_RBRACE },
    { "lbrack",        INTR_LBRACK },
    { "rbrack",        INTR_RBRACK },
    { "comma",         INTR_COMMA },
    { "seq",           INTR_SEQ },
    { "store",         INTR_STORE },
    { "while",         INTR_WHILE },
    { "do_while",      INTR_DO_WHILE },
    { "if",            INTR_IF },
    { "or_branch",     INTR_OR_BRANCH },
    { "then_branch",   INTR_OR_BRANCH },
    { "else_branch",   INTR_ELSE_BRANCH },
    { "switch_block",  INTR_SWITCH_BLOCK },
    { "case_branch",   INTR_CASE_BRANCH },
    { "for_kw",        INTR_FOR_KW },
    { "ternary",       INTR_TERNARY },
    { "colon",         INTR_COLON },
    { "jump_tag",      INTR_JUMP_TAG },
    { "run_tag",       INTR_RUN_TAG },
    { "break_loop",    INTR_BREAK_LOOP },
    { "continue_loop", INTR_CONTINUE_LOOP },
    { "return_fn",     INTR_RETURN_FN },
    { "lambda",        INTR_LAMBDA },
    { "pipe",          INTR_PIPE },
    { "cand",          INTR_CAND },
    { "cor",           INTR_COR },
    { "cnot",          INTR_CNOT },
    { "sym_diff",         INTR_SYM_DIFF },
    { "sym_integ",        INTR_SYM_INTEG },
    { "sym_solve",        INTR_SYM_SOLVE },
    { "sym_simplify",     INTR_SYM_SIMPLIFY },
    { "sym_eval",         INTR_SYM_EVAL },
    { "field",         INTR_FIELD },
    { "vnew",          INTR_VNEW },
    { "vfill",         INTR_VFILL },
    { "vpush",         INTR_VPUSH },
    { "vpop",          INTR_VPOP },
    { "vshift",        INTR_VSHIFT },
    { "vget",          INTR_VGET },
    { "vset",          INTR_VSET },
    { "vlen",          INTR_VLEN },
    { "vcap",          INTR_VCAP },
    { "vresize",       INTR_VRESIZE },
    { "vshrink",       INTR_VSHRINK },
    { "vclear",        INTR_VCLEAR },
    { "vrange",        INTR_VRANGE },
    { "vrange_cat",    INTR_VRANGE_CAT },
    { "vrange_step",   INTR_VRANGE_STEP },
    { "fcall",         INTR_FCALL },
    { "map_new",       INTR_MAP_NEW },
    { "map_set",       INTR_MAP_SET },
    { "map_get",       INTR_MAP_GET },
    { "map_has",       INTR_MAP_HAS },
    { "map_del",       INTR_MAP_DEL },
    { "mrank",         INTR_MRANK },
    { "mlen",          INTR_MLEN },
    { "mdim",          INTR_MDIM },
    { "mreshape",      INTR_MRESHAPE },
    { "add",           INTR_ADD },
    { "sub",           INTR_SUB },
    { "mul",           INTR_MUL },
    { "div",           INTR_DIV },
    { "mod",           INTR_MOD },
    { "neg",           INTR_NEG },
    { "abs",           INTR_ABS },
    { "floor",         INTR_FLOOR },
    { "ceil",          INTR_CEIL },
    { "pow",           INTR_POW },
    { "sqrt",          INTR_SQRT },
    { "cbrt",          INTR_CBRT },
    { "exp",           INTR_EXP },
    { "ln",            INTR_LN },
    { "sin",           INTR_SIN },
    { "cos",           INTR_COS },
    { "tan",           INTR_TAN },
    { "asin",          INTR_ASIN },
    { "acos",          INTR_ACOS },
    { "atan",          INTR_ATAN },
    { "atan2",         INTR_ATAN2 },
    { "erf",           INTR_ERF },
    { "gamma",         INTR_GAMMA },
    { "lgamma",        INTR_LGAMMA },
    { "eq",            INTR_EQ },
    { "ne",            INTR_NE },
    { "lt",            INTR_LT },
    { "le",            INTR_LE },
    { "gt",            INTR_GT },
    { "ge",            INTR_GE },
    { "band",          INTR_BAND },
    { "bor",           INTR_BOR },
    { "bxor",          INTR_BXOR },
    { "bnot",          INTR_BNOT },
    { "bshl",          INTR_BSHL },
    { "bshr",          INTR_BSHR },
    { "raw",           INTR_RAW },
    { "type_name",     INTR_TYPE_NAME },
    { "unit_name",     INTR_UNIT_NAME },
    { "tostr",         INTR_TOSTR },
    { "fromstr",       INTR_FROMSTR },
    { "str_fmt",       INTR_STR_FMT },
    { "regex_match",   INTR_REGEX_MATCH },
    { "regex_find",    INTR_REGEX_FIND },
    { "regex_find_all",INTR_REGEX_FINDALL },
    { "regex_replace", INTR_REGEX_REPLACE },
    { "regex_split",   INTR_REGEX_SPLIT },
    { "gc_compact",     INTR_GC_COMPACT },
    { "pool_used",      INTR_POOL_USED },
    { "err",            INTR_ERR },
    { "print",          INTR_PRINT },
    { "println",        INTR_PRINTLN },
    { "assert",         INTR_ASSERT },
    { "clock_ms",       INTR_CLOCK_MS },
    { "file_read",      INTR_FILE_READ },
    { "file_write",     INTR_FILE_WRITE },
    { "file_append",    INTR_FILE_APPEND },
    { "file_exists",    INTR_FILE_EXISTS },
    { "json_parse",     INTR_JSON_PARSE },
    { "json_stringify", INTR_JSON_STRINGIFY },
    { "import",         INTR_IMPORT },
    { NULL,             INTR_NONE }
};

IntrinsicId delegate_lookup_intrinsic(const char *s, uint16_t len) {
    for (int i = 0; k_delegate_intrinsics[i].name != NULL; ++i) {
        if (strlen(k_delegate_intrinsics[i].name) == len &&
            memcmp(k_delegate_intrinsics[i].name, s, len) == 0) {
            return k_delegate_intrinsics[i].id;
        }
    }
    return INTR_NONE;
}

uint32_t delegate_intrinsic_table_hash(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; k_delegate_intrinsics[i].name != NULL; ++i) {
        const char *p = k_delegate_intrinsics[i].name;
        while (*p) {
            h ^= (uint8_t)(*p++);
            h *= 16777619u;
        }
        uint32_t id = (uint32_t)k_delegate_intrinsics[i].id;
        h ^= (id & 0xFFu);         h *= 16777619u;
        h ^= ((id >> 8u) & 0xFFu); h *= 16777619u;
    }
    h ^= (uint32_t)INTR_COUNT;
    h *= 16777619u;
    return (h == 0) ? 1u : h;
}

bool delegate_is_meta_intrinsic(IntrinsicId intr) {
    switch (intr) {
        case INTR_SEQ:
        case INTR_STORE:
        case INTR_WHILE:
        case INTR_DO_WHILE:
        case INTR_IF:
        case INTR_OR_BRANCH:
        case INTR_ELSE_BRANCH:
        case INTR_SWITCH_BLOCK:
        case INTR_CASE_BRANCH:
        case INTR_FOR_KW:
        case INTR_TERNARY:
        case INTR_COLON:
        case INTR_JUMP_TAG:
        case INTR_RUN_TAG:
        case INTR_BREAK_LOOP:
        case INTR_CONTINUE_LOOP:
        case INTR_RETURN_FN:
        case INTR_LAMBDA:
        case INTR_PIPE:
        case INTR_CAND:
        case INTR_COR:
        case INTR_CNOT:
        case INTR_SYM_DIFF:
        case INTR_SYM_INTEG:
        case INTR_SYM_SOLVE:
        case INTR_SYM_SIMPLIFY:
        case INTR_SYM_EVAL:
        case INTR_FIELD:
        case INTR_FCALL:
            return true;
        default:
            return false;
    }
}

bool delegate_is_sym_meta(uint8_t mtag) {
    switch ((IntrinsicId)mtag) {
        case INTR_SYM_DIFF:
        case INTR_SYM_INTEG:
        case INTR_SYM_SOLVE:
        case INTR_SYM_SIMPLIFY:
        case INTR_SYM_EVAL:
            return true;
        default:
            return false;
    }
}

bool delegate_is_rel_intrinsic(IntrinsicId intr) {
    switch (intr) {
        case INTR_EQ:
        case INTR_NE:
        case INTR_LT:
        case INTR_LE:
        case INTR_GT:
        case INTR_GE:
            return true;
        default:
            return false;
    }
}

bool delegate_is_map_intrinsic(IntrinsicId intr) {
    switch (intr) {
        case INTR_MAP_NEW:
        case INTR_MAP_SET:
        case INTR_MAP_GET:
        case INTR_MAP_HAS:
        case INTR_MAP_DEL:
            return true;
        default:
            return false;
    }
}

bool delegate_is_vec_or_map_intrinsic(IntrinsicId intr) {
    switch (intr) {
        case INTR_VNEW:
        case INTR_VFILL:
        case INTR_VPUSH:
        case INTR_VPOP:
        case INTR_VSHIFT:
        case INTR_VGET:
        case INTR_VSET:
        case INTR_VLEN:
        case INTR_VCAP:
        case INTR_VRESIZE:
        case INTR_VSHRINK:
        case INTR_VCLEAR:
        case INTR_VRANGE:
        case INTR_VRANGE_CAT:
        case INTR_VRANGE_STEP:
        case INTR_FCALL:
            return true;
        default:
            return delegate_is_map_intrinsic(intr);
    }
}

bool delegate_is_mat_intrinsic(IntrinsicId intr) {
    switch (intr) {
        case INTR_MRANK:
        case INTR_MLEN:
        case INTR_MDIM:
        case INTR_MRESHAPE:
            return true;
        default:
            return false;
    }
}

/*Below section is the C's native token constructor for consuming type delegates @f64, @f64_trim1, @i64, @str_slice*/
/*Construct $real (~ \d+ ("_" \d+)* ... => @f64), $iota (=> @f64_trim1), and $str (=> @str_slice)*/
Value delegate_construct_token(MemArena *mem, const SymTable *tab,
                               uint16_t type_id, const char *src,
                               uint16_t off, uint16_t len) {
    const TypeDef *td = &tab->types[type_id];
    char tmp[512];
    uint16_t copy_len = 0;
    if (td->parser_intr != INTR_PARSE_STR_SLICE) {
        for (uint16_t i = 0; i < len && copy_len < sizeof(tmp) - 1; ++i) {
            char ch = src[off + i];
            if (ch == '_') continue;
            tmp[copy_len++] = ch;
        }
        tmp[copy_len] = '\0';
    }

    switch (td->parser_intr) {
        case INTR_PARSE_F64: {
            if (copy_len >= 3 && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) {
                unsigned long long u = strtoull(tmp + 2, NULL, 16);
                return val_make_f64(mem, type_id, (double)u);
            }
            if (copy_len >= 3 && tmp[0] == '0' && (tmp[1] == 'b' || tmp[1] == 'B')) {
                unsigned long long u = strtoull(tmp + 2, NULL, 2);
                return val_make_f64(mem, type_id, (double)u);
            }
            double d = strtod(tmp, NULL);
            return val_make_f64(mem, type_id, d);
        }
        case INTR_PARSE_F64_TRIM1: {
            if (copy_len > 0) tmp[copy_len - 1] = '\0';
            double d = strtod(tmp, NULL);
            return val_make_f64(mem, type_id, d);
        }
        case INTR_PARSE_I64: {
            double d = (double)strtoll(tmp, NULL, 10);
            return val_make_f64(mem, type_id, d);
        }
        case INTR_PARSE_STR_SLICE: {
            uint16_t s_off = off;
            uint16_t s_len = len;
            if (s_len >= 2 && src[s_off] == '"' && src[s_off + s_len - 1] == '"') {
                s_off += 1;
                s_len -= 2;
            }
            char stack_ubuf[1024];
            stack_ubuf[0] = '\0';
            char *ubuf = stack_ubuf;
            size_t ucap = sizeof(stack_ubuf);
            if ((size_t)s_len + 2u > ucap) {
                ucap = (size_t)s_len + 2u;
                ubuf = (char *)malloc(ucap);
                if (!ubuf) {
                    mem_err(mem, "Out of memory constructing string literal");
                    return val_void();
                }
            }
            uint16_t ulen = 0;
            for (uint16_t k = 0; k < s_len && (size_t)ulen + 1u < ucap; ++k) {
                char c = src[s_off + k];
                if (c == '\\' && k + 1 < s_len) {
                    char nx = src[s_off + k + 1];
                    if (nx == '\\') { ubuf[ulen++] = '\\'; k++; continue; }
                    if (nx == 'n')  { ubuf[ulen++] = '\n'; k++; continue; }
                    if (nx == 't')  { ubuf[ulen++] = '\t'; k++; continue; }
                    if (nx == 'r')  { ubuf[ulen++] = '\r'; k++; continue; }
                    if (nx == '"')  { ubuf[ulen++] = '"';  k++; continue; }
                }
                ubuf[ulen++] = c;
            }
            StrSlice slice = mem_intern_str(mem, ubuf, ulen);
            if (ubuf != stack_ubuf) free(ubuf);
            return val_make_str(mem, type_id, slice);
        }
        default:
            return val_void();
    }
}

/* ============================================================================
 * 2. SCALAR & UNIT VALIDATION HELPERS
 * ============================================================================ */

/*Below section is the C's native unit compatibility and integer validation helpers for operator delegates @add, @sub, @eq..@approx*/
/*Construct unit-aware + / - / comparisons (universal 0 rule) and exact-integer index/dimension validation*/
bool delegate_is_zero_unitless_scalar(const MemArena *mem, const SymTable *tab, Value v) {
    uint16_t tid = val_get_type(mem, v);
    if (tid == 0 || tid >= tab->type_count || !tab->types[tid].is_consuming) return false;
    if (tid == mem->str_type_id) return false;
    if (val_get_unit(mem, v) != MEM_UNIT_NONE) return false;
    return val_get_f64(mem, v) == 0.0;
}

bool delegate_check_additive_unit_compat(MemArena *mem, const SymTable *tab,
                                         const char *op_name, int op_len,
                                         Value a, Value b, uint8_t *out_unit) {
    uint8_t ua = val_get_unit(mem, a);
    uint8_t ub = val_get_unit(mem, b);
    if (ua == ub) {
        if (out_unit) *out_unit = ua;
        return true;
    }
    if (ua == MEM_UNIT_NONE && delegate_is_zero_unitless_scalar(mem, tab, a)) {
        if (out_unit) *out_unit = ub;
        return true;
    }
    if (ub == MEM_UNIT_NONE && delegate_is_zero_unitless_scalar(mem, tab, b)) {
        if (out_unit) *out_unit = ua;
        return true;
    }

    char t1[32], t2[32], u1[32], u2[32];
    sym_type_name(tab, mem, val_get_type(mem, a), t1, sizeof(t1));
    sym_type_name(tab, mem, val_get_type(mem, b), t2, sizeof(t2));
    sym_unit_name(tab, mem, ua, u1, sizeof(u1));
    sym_unit_name(tab, mem, ub, u2, sizeof(u2));
    mem_err(mem, "Unit mismatch for '%.*s' with (%s [%s], %s [%s])",
            op_len, op_name, t1, u1, t2, u2);
    return false;
}

/*Below section is the C's native 7D compound SI dimensional exponent algebra for @mul, @div, @pow, @sqrt, @cbrt (driven by @dim in Library/units.math)*/
/*Construct Compound unit formation & cancellation on *, /, ^, √/sqrt, cbrt across all 7 SI base units (m^L * kg^M * s^T * A^I * K^Th * mol^N * cd^J)*/
static bool find_si_unit_dim(const SymTable *tab, uint8_t uid, UnitDimDef *out_dim) {
    if (uid == MEM_UNIT_NONE) {
        memset(out_dim, 0, sizeof(*out_dim));
        out_dim->has_dim = 1;
        return true;
    }
    if (!tab || uid >= SYM_MAX_UNITS) return false;
    if ((uid < tab->unit_count || uid >= SYM_EPHEM_UNIT_BASE) &&
        tab->unit_dims[uid].has_dim) {
        *out_dim = tab->unit_dims[uid];
        return true;
    }
    return false;
}

static uint8_t lookup_si_dim_unit_id(const SymTable *tab, const int exps[7]) {
    bool all_zero = true;
    for (int d = 0; d < 7; ++d) {
        if (exps[d] < -64 || exps[d] > 63) return MEM_UNIT_NONE;
        if (exps[d] != 0) all_zero = false;
    }
    if (all_zero) return MEM_UNIT_NONE;
    if (!tab) return MEM_UNIT_NONE;
    /* 1. Search registered named unit groups */
    for (uint8_t u = 1; u < tab->unit_count; ++u) {
        if (!tab->unit_dims[u].has_dim) continue;
        bool match = true;
        for (int d = 0; d < 7; ++d) {
            if ((int)tab->unit_dims[u].exp[d] != exps[d]) {
                match = false;
                break;
            }
        }
        if (match) return u;
    }
    /* 2. Search active ephemeral 7D SI unit slots (48..63) */
    for (uint8_t u = SYM_EPHEM_UNIT_BASE; u < SYM_MAX_UNITS; ++u) {
        if (!tab->unit_dims[u].has_dim) continue;
        bool match = true;
        for (int d = 0; d < 7; ++d) {
            if ((int)tab->unit_dims[u].exp[d] != exps[d]) {
                match = false;
                break;
            }
        }
        if (match) return u;
    }
    /* 3. Allocate an ephemeral 7D SI slot so intermediate compound dimensions survive */
    if (tab->unit_count < SYM_EPHEM_UNIT_BASE) {
        SymTable *mut_tab = (SymTable *)(uintptr_t)tab;
        uint8_t span = (uint8_t)(SYM_MAX_UNITS - SYM_EPHEM_UNIT_BASE);
        uint8_t u = (uint8_t)(SYM_EPHEM_UNIT_BASE + (mut_tab->ephem_unit_cursor % span));
        mut_tab->ephem_unit_cursor++;
        for (int d = 0; d < 7; ++d) {
            mut_tab->unit_dims[u].exp[d] = (int8_t)exps[d];
        }
        mut_tab->unit_dims[u].has_dim = 1u;
        return u;
    }
    return MEM_UNIT_NONE;
}

uint8_t delegate_resolve_mul_unit(const MemArena *mem, const SymTable *tab,
                                  uint8_t ua, uint8_t ub) {
    (void)mem;
    if (ua == MEM_UNIT_NONE) return ub;
    if (ub == MEM_UNIT_NONE) return ua;
    UnitDimDef da, db;
    if (find_si_unit_dim(tab, ua, &da) && find_si_unit_dim(tab, ub, &db)) {
        int sum_e[7];
        for (int d = 0; d < 7; ++d) sum_e[d] = (int)da.exp[d] + (int)db.exp[d];
        return lookup_si_dim_unit_id(tab, sum_e);
    }
    return (ua == ub) ? ua : MEM_UNIT_NONE;
}

uint8_t delegate_resolve_div_unit(const MemArena *mem, const SymTable *tab,
                                  uint8_t ua, uint8_t ub) {
    (void)mem;
    if (ub == MEM_UNIT_NONE) return ua;
    if (ua == ub) return MEM_UNIT_NONE;
    UnitDimDef da, db;
    if (find_si_unit_dim(tab, ua, &da) && find_si_unit_dim(tab, ub, &db)) {
        int diff_e[7];
        for (int d = 0; d < 7; ++d) diff_e[d] = (int)da.exp[d] - (int)db.exp[d];
        return lookup_si_dim_unit_id(tab, diff_e);
    }
    return MEM_UNIT_NONE;
}

uint8_t delegate_resolve_pow_unit(const MemArena *mem, const SymTable *tab,
                                  uint8_t ua, uint8_t ub, double exp_val) {
    (void)mem;
    if (ub != MEM_UNIT_NONE || ua == MEM_UNIT_NONE) return MEM_UNIT_NONE;
    if (fabs(exp_val) <= 1e-12) return MEM_UNIT_NONE;
    if (fabs(exp_val - 1.0) <= 1e-12) return ua;
    UnitDimDef da;
    if (find_si_unit_dim(tab, ua, &da)) {
        double r = floor(exp_val + 0.5);
        if (fabs(exp_val - r) <= 1e-9 && r >= -4.0 && r <= 4.0) {
            int k = (int)r;
            int scaled[7];
            for (int d = 0; d < 7; ++d) scaled[d] = (int)da.exp[d] * k;
            return lookup_si_dim_unit_id(tab, scaled);
        }
        if (fabs(exp_val - 0.5) <= 1e-9 || fabs(exp_val - (1.0 / 3.0)) <= 1e-9) {
            int root_n = (fabs(exp_val - 0.5) <= 1e-9) ? 2 : 3;
            int scaled[7];
            for (int d = 0; d < 7; ++d) {
                if ((int)da.exp[d] % root_n != 0) return MEM_UNIT_NONE;
                scaled[d] = (int)da.exp[d] / root_n;
            }
            return lookup_si_dim_unit_id(tab, scaled);
        }
        return MEM_UNIT_NONE;
    }
    return ua;
}

uint8_t delegate_resolve_root_unit(const MemArena *mem, const SymTable *tab,
                                   uint8_t ua, int root_n) {
    (void)mem;
    if (ua == MEM_UNIT_NONE || root_n <= 0) return MEM_UNIT_NONE;
    UnitDimDef da;
    if (find_si_unit_dim(tab, ua, &da)) {
        int scaled[7];
        for (int d = 0; d < 7; ++d) {
            if ((int)da.exp[d] % root_n != 0) return MEM_UNIT_NONE;
            scaled[d] = (int)da.exp[d] / root_n;
        }
        return lookup_si_dim_unit_id(tab, scaled);
    }
    return ua;
}

bool delegate_val_to_exact_int(MemArena *mem, const SymTable *tab, Value v,
                               const char *ctx, int *out_int) {
    uint16_t tid = val_get_type(mem, v);
    if (tid == 0 || tid >= tab->type_count ||
        !tab->types[tid].is_consuming || tid == mem->str_type_id) {
        mem_err(mem, "Invalid non-numeric %s", ctx);
        return false;
    }
    if (val_get_unit(mem, v) != MEM_UNIT_NONE &&
        strcmp(ctx, "range start") != 0 &&
        strcmp(ctx, "range end") != 0) {
        mem_err(mem, "Expected unitless integer for %s", ctx);
        return false;
    }
    double d = val_get_f64(mem, v);
    if (!isfinite(d) || d < -2147483647.0 || d > 2147483647.0) {
        mem_err(mem, "Out-of-range %s (%.6g)", ctx, d);
        return false;
    }
    double r = floor(d + 0.5);
    if (fabs(d - r) > 1e-9) {
        mem_err(mem, "Non-integer %s (%.6g)", ctx, d);
        return false;
    }
    *out_int = (int)r;
    return true;
}

Value delegate_eval_range_step(MemArena *mem, const SymTable *tab,
                               Value va, Value vb, Value vstep, int mode) {
    uint16_t ta = val_get_type(mem, va);
    uint16_t tb = val_get_type(mem, vb);
    if (ta == 0 || ta >= tab->type_count || !tab->types[ta].is_consuming || ta == mem->str_type_id ||
        tb == 0 || tb >= tab->type_count || !tab->types[tb].is_consuming || tb == mem->str_type_id) {
        mem_err(mem, "Range bounds must be numeric scalars");
        return val_void();
    }
    uint8_t elem_uid = MEM_UNIT_NONE;
    if (!delegate_check_additive_unit_compat(mem, tab, "..", 2, va, vb, &elem_uid)) {
        return val_void();
    }
    double da = val_get_f64(mem, va);
    double db = val_get_f64(mem, vb);
    double step = (da <= db) ? 1.0 : -1.0;

    if (val_get_type(mem, vstep) != MEM_TYPE_VOID) {
        uint16_t ts = val_get_type(mem, vstep);
        if (ts == 0 || ts >= tab->type_count || !tab->types[ts].is_consuming || ts == mem->str_type_id) {
            mem_err(mem, "Range step must be a numeric scalar");
            return val_void();
        }
        uint8_t us = val_get_unit(mem, vstep);
        if (us != MEM_UNIT_NONE && us != elem_uid) {
            mem_err(mem, "Unit mismatch in stepped range 'by'");
            return val_void();
        }
        step = val_get_f64(mem, vstep);
        if (!isfinite(step) || fabs(step) <= 1e-15) {
            mem_err(mem, "Range step cannot be zero");
            return val_void();
        }
    } else {
        int lo_i = 0, hi_i = 0;
        if (!delegate_val_to_exact_int(mem, tab, va, "range start", &lo_i) ||
            !delegate_val_to_exact_int(mem, tab, vb, "range end",   &hi_i)) {
            return val_void();
        }
        da = (double)lo_i;
        db = (double)hi_i;
    }

    double eps = 1e-11 * (1.0 + fabs(da) + fabs(db) + fabs(step));
    long long start_k = (mode & 2) ? 1LL : 0LL;
    double first_val  = da + (double)start_k * step;
    long long cnt_ll  = 0;

    if (step > 0.0) {
        bool ok = (mode & 1) ? (first_val < db - eps) : (first_val <= db + eps);
        if (ok) {
            cnt_ll = (long long)floor(((db - first_val) + ((mode & 1) ? -eps : eps)) / step) + 1LL;
        }
    } else {
        bool ok = (mode & 1) ? (first_val > db + eps) : (first_val >= db - eps);
        if (ok) {
            cnt_ll = (long long)floor(((first_val - db) + ((mode & 1) ? -eps : eps)) / (-step)) + 1LL;
        }
    }
    if (cnt_ll < 0) cnt_ll = 0;
    if (cnt_ll > (long long)MEM_MAX_HEAP_SLOTS) {
        mem_err(mem, "Range length (%lld) exceeds maximum capacity", cnt_ll);
        return val_void();
    }

    uint32_t count    = (uint32_t)cnt_ll;
    uint16_t elem_tid = mem->fast_real_type;
    uint16_t packed_t = (uint16_t)(((uint32_t)elem_uid << 8u) | ((uint32_t)elem_tid & 0xFFu));
    uint16_t hdr_idx  = mem_vec_alloc_hdr(mem, packed_t, count);
    if (mem->has_error) return val_void();
    if (count > 0) {
        if (!mem_vec_reserve(mem, hdr_idx, 1, count)) return val_void();
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        double  *dptr     = mem_data_ptr(mem, data_idx, 0);
        if (!dptr) return val_void();
        for (uint32_t i = 0; i < count; ++i) {
            double v = da + (double)(start_k + (long long)i) * step;
            if (fabs(v) < 1e-15) v = 0.0;
            dptr[i] = v;
        }
        mem_write_pool(mem, hdr_idx + 1, (double)count);
    }
    if (val_get_type(mem, vstep) == MEM_TYPE_VOID && hdr_idx != 0) {
        uint8_t slot = (uint8_t)(mem->vrange_cnt++ & 3u);
        mem->vrange_hdr[slot]     = hdr_idx;
        mem->vrange_unit[slot]    = elem_uid;
        mem->vrange_seg_cnt[slot] = 1;
        mem->vrange_mode[slot][0] = (uint8_t)mode;
        mem->vrange_a[slot][0]    = da;
        mem->vrange_b[slot][0]    = db;
    }
    return val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
}

bool delegate_extract_slice_indices(MemArena *mem, const SymTable *tab,
                                    Value idx_val, int dim_len,
                                    uint16_t *out_indices, uint16_t *out_count,
                                    uint16_t max_cap) {
    uint16_t tid = val_get_type(mem, idx_val);
    if (tid == mem->vec_type_id && mem->vec_type_id != 0) {
        uint16_t hdr_idx = val_get_pool_idx(idx_val);
        uint32_t len32   = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t dat_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        if (len32 > (uint32_t)max_cap) {
            mem_err(mem, "Slice length (%u) exceeds maximum capacity (%u)", (unsigned)len32, (unsigned)max_cap);
            return false;
        }
        uint16_t len = (uint16_t)len32;
        for (uint16_t i = 0; i < len; ++i) {
            Value ev = val_make_f64(mem, mem->fast_real_type, mem_data_read(mem, dat_idx, i));
            int k = 0;
            if (!delegate_val_to_exact_int(mem, tab, ev, "slice index", &k)) return false;
            if (k < 0) k += dim_len;
            if (k < 0 || k >= dim_len) {
                mem_err(mem, "Slice index %d out of bounds (size=%d)", k, dim_len);
                return false;
            }
            out_indices[i] = (uint16_t)k;
        }
        *out_count = len;
        return true;
    }
    if (tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, dat_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, idx_val, &rank, &tot_len, &dat_idx, dims);
        if (rank != 1 || tot_len > max_cap) {
            mem_err(mem, "Matrix slice index must be a 1D index vector");
            return false;
        }
        for (uint16_t i = 0; i < tot_len; ++i) {
            Value ev = val_make_f64(mem, mem->fast_real_type, mem_data_read(mem, dat_idx, i));
            int k = 0;
            if (!delegate_val_to_exact_int(mem, tab, ev, "slice index", &k)) return false;
            if (k < 0) k += dim_len;
            if (k < 0 || k >= dim_len) {
                mem_err(mem, "Slice index %d out of bounds (size=%d)", k, dim_len);
                return false;
            }
            out_indices[i] = (uint16_t)k;
        }
        *out_count = tot_len;
        return true;
    }
    return false;
}

/* ============================================================================
 * 2b. FIRST-CLASS LAMBDA (`$fn`) INVOCATION DELEGATE
 * ============================================================================ */

/*Below section is the C's native for operator delegate @lambda and function delegate @fcall*/
/*Construct (x) => expr, (a, b) => expr, () => expr, f(x), call(f, x)*/
Value delegate_call_fn(MemArena *mem, SymTable *tab,
                       Value fn_val, uint8_t argc, const Value *args) {
    if (mem->fn_type_id == 0 || val_get_type(mem, fn_val) != mem->fn_type_id) {
        mem_err(mem, "Expected function ($fn) handle");
        return val_void();
    }
    if (mem->call_depth > 128u) {
        mem_err(mem, "Maximum function recursion depth (128) exceeded");
        return val_void();
    }
    uint16_t hdr_idx     = val_get_pool_idx(fn_val);
    uint16_t body_ast    = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
    uint32_t raw_p1      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
    uint8_t  param_cnt   = (uint8_t)(raw_p1 & 0x3Fu);
    uint16_t raw_mod_tag = (uint16_t)((raw_p1 >> 8u) & 0xFFFFu);
    uint32_t packed_slt  = (uint32_t)mem_read_pool(mem, hdr_idx + 2);
    uint16_t slot_range  = (uint16_t)(packed_slt & 0xFFFFu);
    uint16_t def_head    = (uint16_t)((packed_slt >> 16u) & 0xFFFFu);
    uint8_t  slot_start  = (uint8_t)(slot_range & 0xFFu);
    uint8_t  total_slots = (uint8_t)((slot_range >> 8u) & 0xFFu);
    uint32_t raw_f3      = (uint32_t)mem_read_pool(mem, hdr_idx + 3);
    uint16_t low16       = (uint16_t)(raw_f3 & 0xFFFFu);
    bool     has_env     = (slot_start > 0 && mem_is_heap_idx(low16));
    uint16_t env_hid     = has_env ? (uint16_t)(low16 & MEM_HEAP_ID_MASK) : MEM_NO_IDX;
    uint16_t orig_fbase  = has_env ? (uint16_t)(raw_f3 >> 16u) : low16;
    uint16_t fbase       = orig_fbase;

    if (total_slots < param_cnt) total_slots = param_cnt;
    if (total_slots > 64u) total_slots = 64u;

    /* Count minimum required parameters (those without default ASTs in def_head) */
    uint8_t min_req = param_cnt;
    if (def_head != MEM_NO_IDX && def_head != 0) {
        const AstNode *dl = mem_get_ast(mem, def_head);
        if (dl->kind == AST_ARG_LINK && dl->sub < min_req) {
            min_req = dl->sub;
        }
    }
    if (argc < min_req || argc > param_cnt) {
        if (min_req == param_cnt) {
            mem_err(mem, "Lambda expects %u argument(s), got %u",
                    (unsigned)param_cnt, (unsigned)argc);
        } else {
            mem_err(mem, "Function expects %u..%u argument(s), got %u",
                    (unsigned)min_req, (unsigned)param_cnt, (unsigned)argc);
        }
        return val_void();
    }

    uint16_t saved_sp = mem->sp;
    if (has_env && env_hid < MEM_MAX_HEAP_BLOCKS && mem->heap_blocks[env_hid].in_use >= 2u) {
        fbase = saved_sp;
        uint16_t max_needed = (uint16_t)(fbase + slot_start + total_slots);
        if (max_needed > MEM_MAX_CALL_STACK) {
            mem_err(mem, "Call stack overflow in closure invocation");
            return val_void();
        }
        mem->sp = max_needed;
        Value pre_env_vals[64];
        HeapBlock *hb = &mem->heap_blocks[env_hid];
        for (uint8_t k = 0; k < slot_start; ++k) {
            uint16_t abs_s = (uint16_t)(fbase + k);
            Value cv = val_void();
            if (k < hb->cap_slots) {
                memcpy(&cv, &hb->data[k], sizeof(Value));
            }
            if (hb->in_use == 2u && val_get_type(mem, cv) == MEM_TYPE_VOID &&
                (uint16_t)(orig_fbase + k) < saved_sp) {
                cv = mem->stack[orig_fbase + k];
            }
            mem->stack[abs_s] = cv;
            if (k < 64u) pre_env_vals[k] = cv;
            uint32_t lpack = ((uint32_t)(slot_start + k) < hb->cap_slots)
                             ? (uint32_t)hb->data[slot_start + k] : 0u;
            mem->slot_locked_type[abs_s] = (uint8_t)(lpack & 0xFFu);
            mem->slot_locked_unit[abs_s] = (uint8_t)((lpack >> 8u) & 0xFFu);
            mem->slot_is_const[abs_s]    = (uint8_t)((lpack >> 16u) & 0xFFu);
        }
        for (uint8_t k = 0; k < total_slots; ++k) {
            uint16_t abs_s = (uint16_t)(fbase + slot_start + k);
            mem->slot_locked_type[abs_s] = 0;
            mem->slot_locked_unit[abs_s] = 0;
            mem->slot_is_const[abs_s]    = 0;
            mem->stack[abs_s] = (k < argc) ? args[k] : val_make_f64(mem, mem->fast_real_type, 0.0);
        }
        if (argc < param_cnt && def_head != MEM_NO_IDX && def_head != 0) {
            uint16_t cur_d = def_head;
            while (cur_d != MEM_NO_IDX && !mem->has_error) {
                const AstNode *dl = mem_get_ast(mem, cur_d);
                uint8_t p_idx = dl->sub;
                if (p_idx >= argc && p_idx < param_cnt) {
                    Value dval = eval_ast(mem, tab, dl->left, fbase, 0);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) break;
                    mem->stack[fbase + slot_start + p_idx] = dval;
                }
                cur_d = dl->right;
            }
        }
        Value res = val_void();
        if (!mem->has_error && mem->jump_active == JUMP_NONE) {
            uint16_t saved_mod     = mem->cur_mod;
            uint16_t saved_tab_mod = tab->cur_mod;
            if (raw_mod_tag > 0 && (uint16_t)(raw_mod_tag - 1u) < tab->mod_count) {
                mem->cur_mod = (uint16_t)(raw_mod_tag - 1u);
                tab->cur_mod = mem->cur_mod;
            }
            mem->call_depth++;
            res = eval_ast(mem, tab, body_ast, fbase, 0);
            if (mem->call_depth > 0) mem->call_depth--;
            if (mem->jump_active == JUMP_RETURN) {
                res = mem->jump_val;
                mem->jump_val    = val_void();
                mem->jump_active = JUMP_NONE;
            }
            mem->cur_mod = saved_mod;
            tab->cur_mod = saved_tab_mod;
        }
        hb = &mem->heap_blocks[env_hid];
        if (hb->in_use >= 2u && hb->data != NULL) {
            for (uint8_t k = 0; k < slot_start && (uint32_t)k * 2u + 1u < hb->cap_slots; ++k) {
                uint16_t abs_s = (uint16_t)(fbase + k);
                Value uv = mem->stack[abs_s];
                memcpy(&hb->data[k], &uv, sizeof(Value));
                uint32_t lpack = (uint32_t)mem->slot_locked_type[abs_s]
                               | ((uint32_t)mem->slot_locked_unit[abs_s] << 8u)
                               | ((uint32_t)mem->slot_is_const[abs_s] << 16u);
                hb->data[slot_start + k] = (double)lpack;
                if (hb->in_use == 2u && (uint16_t)(orig_fbase + k) < saved_sp &&
                    k < 64u && uv != pre_env_vals[k]) {
                    mem->stack[orig_fbase + k]            = uv;
                    mem->slot_locked_type[orig_fbase + k] = mem->slot_locked_type[abs_s];
                    mem->slot_locked_unit[orig_fbase + k] = mem->slot_locked_unit[abs_s];
                    mem->slot_is_const[orig_fbase + k]    = mem->slot_is_const[abs_s];
                    /* Sync sibling active-frame closures sharing orig_fbase */
                    for (uint16_t shid = 0; shid < MEM_MAX_HEAP_BLOCKS; ++shid) {
                        if (shid == env_hid) continue;
                        HeapBlock *shb = &mem->heap_blocks[shid];
                        if (shb->in_use == 2u && shb->data != NULL && shb->cap_slots >= 3u) {
                            uint32_t smeta = (uint32_t)shb->data[shb->cap_slots - 1u];
                            uint16_t sfb   = (uint16_t)(smeta >> 8u);
                            uint8_t  ss    = (uint8_t)(smeta & 0xFFu);
                            if (sfb == orig_fbase && k < ss) {
                                memcpy(&shb->data[k], &uv, sizeof(Value));
                                shb->data[ss + k] = (double)lpack;
                            }
                        }
                    }
                }
            }
        }
        mem_pop_frame(mem, saved_sp);
        if (!mem->has_error && (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                                mem->heap_active_count >= 64u)) {
            (void)mem_compact_scratch(mem, &res, 1);
        }
        return res;
    }

    uint16_t max_needed = (uint16_t)(fbase + slot_start + total_slots);
    if (max_needed > MEM_MAX_CALL_STACK) {
        mem_err(mem, "Call stack overflow in function invocation");
        return val_void();
    }
    if (mem->sp < max_needed) {
        mem->sp = max_needed;
    }

    /* Fast path for 0/1/2-slot lambdas with no default arguments (hot calculus/vec loops) */
    if ((def_head == MEM_NO_IDX || def_head == 0) && argc == param_cnt && total_slots <= 2u) {
        uint16_t s0 = (uint16_t)(fbase + slot_start);
        Value    sv0 = val_void(), sv1 = val_void();
        uint8_t  lt0 = 0, lt1 = 0, lu0 = 0, lu1 = 0;
        if (total_slots >= 1u) {
            sv0 = mem->stack[s0];
            lt0 = mem->slot_locked_type[s0];
            lu0 = mem->slot_locked_unit[s0];
            mem->slot_locked_type[s0] = 0;
            mem->slot_locked_unit[s0] = 0;
            if (argc >= 1u) mem->stack[s0] = args[0];
        }
        if (total_slots == 2u) {
            uint16_t s1 = (uint16_t)(s0 + 1u);
            sv1 = mem->stack[s1];
            lt1 = mem->slot_locked_type[s1];
            lu1 = mem->slot_locked_unit[s1];
            mem->slot_locked_type[s1] = 0;
            mem->slot_locked_unit[s1] = 0;
            if (argc >= 2u) mem->stack[s1] = args[1];
        }
        Value res = val_void();
        if (!mem->has_error && mem->jump_active == JUMP_NONE) {
            uint16_t saved_mod     = mem->cur_mod;
            uint16_t saved_tab_mod = tab->cur_mod;
            if (raw_mod_tag > 0 && (uint16_t)(raw_mod_tag - 1u) < tab->mod_count) {
                mem->cur_mod = (uint16_t)(raw_mod_tag - 1u);
                tab->cur_mod = mem->cur_mod;
            }
            mem->call_depth++;
            res = eval_ast(mem, tab, body_ast, fbase, 0);
            if (mem->call_depth > 0) mem->call_depth--;
            if (mem->jump_active == JUMP_RETURN) {
                res = mem->jump_val;
                mem->jump_val    = val_void();
                mem->jump_active = JUMP_NONE;
            }
            mem->cur_mod = saved_mod;
            tab->cur_mod = saved_tab_mod;
        }
        if (total_slots >= 1u) {
            mem->stack[s0]            = sv0;
            mem->slot_locked_type[s0] = lt0;
            mem->slot_locked_unit[s0] = lu0;
        }
        if (total_slots == 2u) {
            uint16_t s1 = (uint16_t)(s0 + 1u);
            mem->stack[s1]            = sv1;
            mem->slot_locked_type[s1] = lt1;
            mem->slot_locked_unit[s1] = lu1;
        }
        mem->sp = saved_sp;
        if (!mem->has_error && (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                                mem->heap_active_count >= 64u)) {
            (void)mem_compact_scratch(mem, &res, 1);
        }
        return res;
    }

    Value   saved_vals[64];
    uint8_t saved_ltype[64];
    uint8_t saved_lunit[64];
    for (uint8_t k = 0; k < total_slots; ++k) {
        uint16_t abs_s = (uint16_t)(fbase + slot_start + k);
        saved_vals[k]  = mem->stack[abs_s];
        saved_ltype[k] = mem->slot_locked_type[abs_s];
        saved_lunit[k] = mem->slot_locked_unit[abs_s];
        mem->slot_locked_type[abs_s] = 0;
        mem->slot_locked_unit[abs_s] = 0;
        if (k < argc) {
            mem->stack[abs_s] = args[k];
        }
    }

    /* Evaluate default parameter expressions for omitted trailing parameters */
    if (argc < param_cnt && def_head != MEM_NO_IDX && def_head != 0) {
        uint16_t cur_d = def_head;
        while (cur_d != MEM_NO_IDX && !mem->has_error) {
            const AstNode *dl = mem_get_ast(mem, cur_d);
            uint8_t p_idx = dl->sub;
            if (p_idx >= argc && p_idx < param_cnt) {
                Value dval = eval_ast(mem, tab, dl->left, fbase, 0);
                if (mem->has_error || mem->jump_active != JUMP_NONE) break;
                mem->stack[fbase + slot_start + p_idx] = dval;
            }
            cur_d = dl->right;
        }
    }

    Value res = val_void();
    if (!mem->has_error && mem->jump_active == JUMP_NONE) {
        uint16_t saved_mod     = mem->cur_mod;
        uint16_t saved_tab_mod = tab->cur_mod;
        if (raw_mod_tag > 0 && (uint16_t)(raw_mod_tag - 1u) < tab->mod_count) {
            mem->cur_mod = (uint16_t)(raw_mod_tag - 1u);
            tab->cur_mod = mem->cur_mod;
        }
        mem->call_depth++;
        res = eval_ast(mem, tab, body_ast, fbase, 0);
        if (mem->call_depth > 0) mem->call_depth--;
        if (mem->jump_active == JUMP_RETURN) {
            res = mem->jump_val;
            mem->jump_val    = val_void();
            mem->jump_active = JUMP_NONE;
        }
        mem->cur_mod = saved_mod;
        tab->cur_mod = saved_tab_mod;
    }

    for (uint8_t k = 0; k < total_slots; ++k) {
        uint16_t abs_s = (uint16_t)(fbase + slot_start + k);
        mem->stack[abs_s]            = saved_vals[k];
        mem->slot_locked_type[abs_s] = saved_ltype[k];
        mem->slot_locked_unit[abs_s] = saved_lunit[k];
    }
    mem->sp = saved_sp;
    if (!mem->has_error && (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                            mem->heap_active_count >= 64u)) {
        (void)mem_compact_scratch(mem, &res, 1);
    }
    return res;
}

/* ============================================================================
 * 3. DYNAMIC VECTOR, LIST, STACK & QUEUE (`$vec`) DELEGATE INTRINSICS
 * ============================================================================ */

/*Below section is the C's native element read/write helpers for container delegates @vnew..@vcount*/
/*Construct vec<T>, list<T>, stack<T>, queue<T> element storage and retrieval*/
