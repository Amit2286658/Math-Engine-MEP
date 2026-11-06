/*
 * delegate_symbolic.c — Symbolic CAS Simplification, Differentiation, Integration & Polynomial Solver
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


static uint16_t sym_make_num(MemArena *mem, double v) {
    uint16_t pidx = mem_alloc_pool(mem, 1);
    if (mem->has_error) return MEM_NO_IDX;
    mem_write_pool(mem, pidx, v);
    return mem_alloc_ast(mem, AST_LIT, mem->fast_real_type, pidx, 0, 0);
}

static bool sym_is_num(const MemArena *mem, uint16_t idx, double *out_v) {
    if (idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_LIT && n->sub == mem->fast_real_type) {
        if (out_v) *out_v = mem_read_pool(mem, n->ref);
        return true;
    }
    return false;
}

static bool sym_ast_equal(const MemArena *mem, const SymTable *tab, uint16_t a, uint16_t b) {
    if (a == b) return true;
    if (a == MEM_NO_IDX || b == MEM_NO_IDX) return false;
    const AstNode *na = mem_get_ast(mem, a);
    const AstNode *nb = mem_get_ast(mem, b);
    if (na->kind != nb->kind) return false;
    if (na->kind == AST_LIT) {
        if (na->sub != nb->sub) return false;
        if (na->sub == mem->fast_real_type) {
            return fabs(mem_read_pool(mem, na->ref) - mem_read_pool(mem, nb->ref)) <= 1e-12;
        }
        return (na->left == nb->left && na->right == nb->right);
    }
    if (na->kind == AST_SLOT_LOAD) return (na->sub == nb->sub);
    if (na->kind == AST_CONST_SYM) return (na->ref == nb->ref);
    if (na->kind == AST_UNARY) {
        return (na->sub == nb->sub && na->ref == nb->ref && sym_ast_equal(mem, tab, na->left, nb->left));
    }
    if (na->kind == AST_BINARY) {
        return (na->sub == nb->sub && na->ref == nb->ref &&
                sym_ast_equal(mem, tab, na->left, nb->left) &&
                sym_ast_equal(mem, tab, na->right, nb->right));
    }
    if (na->kind == AST_CALL) {
        if (na->ref != nb->ref || na->sub != nb->sub) return false;
        uint16_t ca = na->left, cb = nb->left;
        while (ca != MEM_NO_IDX && cb != MEM_NO_IDX) {
            const AstNode *la = mem_get_ast(mem, ca);
            const AstNode *lb = mem_get_ast(mem, cb);
            if (!sym_ast_equal(mem, tab, la->left, lb->left)) return false;
            ca = la->right;
            cb = lb->right;
        }
        return (ca == cb);
    }
    return false;
}

static uint16_t sym_find_op(MemArena *mem, SymTable *tab, const char *s) {
    int id = sym_find(tab, mem, s, (uint16_t)strlen(s));
    if (id >= 0) return (uint16_t)id;
    StrSlice sl = mem_intern_str(mem, s, (uint16_t)strlen(s));
    return sym_intern(tab, mem, sl);
}

static uint16_t sym_make_bin(MemArena *mem, SymTable *tab, const char *op, uint16_t l, uint16_t r) {
    uint16_t sid = sym_find_op(mem, tab, op);
    uint8_t fix = (strcmp(op, "^") == 0) ? (uint8_t)FIX_INFIX_RIGHT : (uint8_t)FIX_INFIX_LEFT;
    return mem_alloc_ast(mem, AST_BINARY, fix, sid, l, r);
}

static uint16_t sym_make_neg(MemArena *mem, SymTable *tab, uint16_t x) {
    uint16_t sid = sym_find_op(mem, tab, "-");
    return mem_alloc_ast(mem, AST_UNARY, (uint8_t)FIX_PREFIX, sid, x, MEM_NO_IDX);
}

static uint16_t sym_make_call1(MemArena *mem, SymTable *tab, const char *fn, uint16_t arg) {
    uint16_t sid  = sym_find_op(mem, tab, fn);
    uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, arg, MEM_NO_IDX);
    return mem_alloc_ast(mem, AST_CALL, 1, sid, link, MEM_NO_IDX);
}

static uint16_t sym_simplify_ast(MemArena *mem, SymTable *tab, uint16_t idx, uint8_t depth) {
    if (idx == MEM_NO_IDX || mem->has_error || depth > 64) return idx;
    const AstNode *n = mem_get_ast(mem, idx);

    if (n->kind == AST_UNARY && n->sub == (uint8_t)FIX_PREFIX) {
        StrSlice s = tab->syms[n->ref];
        uint16_t op_l = sym_simplify_ast(mem, tab, n->left, (uint8_t)(depth + 1));
        double lv = 0.0;
        if (mem_slice_eq_cstr(mem, s, "-")) {
            if (sym_is_num(mem, op_l, &lv)) return sym_make_num(mem, -lv);
            const AstNode *ln = mem_get_ast(mem, op_l);
            if (ln->kind == AST_UNARY && ln->sub == (uint8_t)FIX_PREFIX &&
                mem_slice_eq_cstr(mem, tab->syms[ln->ref], "-")) {
                return ln->left;
            }
            if (ln->kind == AST_BINARY && mem_slice_eq_cstr(mem, tab->syms[ln->ref], "-")) {
                return sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "-", ln->right, ln->left), (uint8_t)(depth + 1));
            }
            return sym_make_neg(mem, tab, op_l);
        }
        if (mem_slice_eq_cstr(mem, s, "+")) return op_l;
        if (mem_slice_eq_cstr(mem, s, "√")) {
            if (sym_is_num(mem, op_l, &lv) && lv >= 0.0) {
                double r = sqrt(lv);
                if (fabs(r - floor(r + 0.5)) <= 1e-12) return sym_make_num(mem, floor(r + 0.5));
            }
            return mem_alloc_ast(mem, AST_UNARY, (uint8_t)FIX_PREFIX, n->ref, op_l, MEM_NO_IDX);
        }
        return mem_alloc_ast(mem, AST_UNARY, n->sub, n->ref, op_l, MEM_NO_IDX);
    }

    if (n->kind == AST_BINARY) {
        StrSlice s = tab->syms[n->ref];
        uint16_t l = sym_simplify_ast(mem, tab, n->left,  (uint8_t)(depth + 1));
        uint16_t r = sym_simplify_ast(mem, tab, n->right, (uint8_t)(depth + 1));
        double lv = 0.0, rv = 0.0;
        bool l_num = sym_is_num(mem, l, &lv);
        bool r_num = sym_is_num(mem, r, &rv);

        if (mem_slice_eq_cstr(mem, s, "+")) {
            if (l_num && r_num) return sym_make_num(mem, lv + rv);
            if (l_num && fabs(lv) <= 1e-12) return r;
            if (r_num && fabs(rv) <= 1e-12) return l;
            if (sym_ast_equal(mem, tab, l, r)) {
                return sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", sym_make_num(mem, 2.0), l), (uint8_t)(depth + 1));
            }
            /* Combine c1 * term + c2 * term (including implicit c=1 on bare term) */
            const AstNode *ln = mem_get_ast(mem, l);
            const AstNode *rn = mem_get_ast(mem, r);
            double c1 = 1.0, c2 = 1.0;
            uint16_t b1 = l, b2 = r;
            if (ln->kind == AST_BINARY && mem_slice_eq_cstr(mem, tab->syms[ln->ref], "*") &&
                sym_is_num(mem, ln->left, &c1)) {
                b1 = ln->right;
            }
            if (rn->kind == AST_BINARY && mem_slice_eq_cstr(mem, tab->syms[rn->ref], "*") &&
                sym_is_num(mem, rn->left, &c2)) {
                b2 = rn->right;
            }
            if (!l_num && !r_num && sym_ast_equal(mem, tab, b1, b2)) {
                return sym_simplify_ast(mem, tab,
                    sym_make_bin(mem, tab, "*", sym_make_num(mem, c1 + c2), b1),
                    (uint8_t)(depth + 1));
            }
            if (rn->kind == AST_UNARY && rn->sub == (uint8_t)FIX_PREFIX &&
                mem_slice_eq_cstr(mem, tab->syms[rn->ref], "-")) {
                return sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "-", l, rn->left), (uint8_t)(depth + 1));
            }
            return sym_make_bin(mem, tab, "+", l, r);
        }
        if (mem_slice_eq_cstr(mem, s, "-")) {
            if (l_num && r_num) return sym_make_num(mem, lv - rv);
            if (r_num && fabs(rv) <= 1e-12) return l;
            if (l_num && fabs(lv) <= 1e-12) return sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, r), (uint8_t)(depth + 1));
            if (sym_ast_equal(mem, tab, l, r)) return sym_make_num(mem, 0.0);
            return sym_make_bin(mem, tab, "-", l, r);
        }
        if (mem_slice_eq_cstr(mem, s, "*")) {
            if (l_num && r_num) return sym_make_num(mem, lv * rv);
            if ((l_num && fabs(lv) <= 1e-12) || (r_num && fabs(rv) <= 1e-12)) return sym_make_num(mem, 0.0);
            if (l_num && fabs(lv - 1.0) <= 1e-12) return r;
            if (r_num && fabs(rv - 1.0) <= 1e-12) return l;
            if (l_num && fabs(lv + 1.0) <= 1e-12) return sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, r), (uint8_t)(depth + 1));
            if (r_num && fabs(rv + 1.0) <= 1e-12) return sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, l), (uint8_t)(depth + 1));
            /* Canonicalize constant factor to the left: x * c -> c * x */
            if (!l_num && r_num) {
                uint16_t tmp = l; l = r; r = tmp;
                lv = rv; l_num = true; r_num = false;
            }
            /* Fold c1 * (c2 * x) -> (c1 * c2) * x */
            const AstNode *rn = mem_get_ast(mem, r);
            if (l_num && rn->kind == AST_BINARY && mem_slice_eq_cstr(mem, tab->syms[rn->ref], "*")) {
                double c2 = 0.0;
                if (sym_is_num(mem, rn->left, &c2)) {
                    return sym_simplify_ast(mem, tab,
                        sym_make_bin(mem, tab, "*", sym_make_num(mem, lv * c2), rn->right),
                        (uint8_t)(depth + 1));
                }
            }
            if (l_num && rn->kind == AST_BINARY && mem_slice_eq_cstr(mem, tab->syms[rn->ref], "/")) {
                double d2 = 0.0;
                if (sym_is_num(mem, rn->right, &d2) && fabs(d2) > 1e-12) {
                    double q = lv / d2;
                    if (fabs(q - floor(q + 0.5)) <= 1e-12) {
                        return sym_simplify_ast(mem, tab,
                            sym_make_bin(mem, tab, "*", sym_make_num(mem, floor(q + 0.5)), rn->left),
                            (uint8_t)(depth + 1));
                    }
                }
            }
            if (sym_ast_equal(mem, tab, l, r)) {
                return sym_make_bin(mem, tab, "^", l, sym_make_num(mem, 2.0));
            }
            return sym_make_bin(mem, tab, "*", l, r);
        }
        if (mem_slice_eq_cstr(mem, s, "/")) {
            if (l_num && fabs(lv) <= 1e-12) return sym_make_num(mem, 0.0);
            if (r_num && fabs(rv - 1.0) <= 1e-12) return l;
            if (l_num && r_num && fabs(rv) > 1e-12) {
                double q = lv / rv;
                if (fabs(q - floor(q + 0.5)) <= 1e-12) return sym_make_num(mem, floor(q + 0.5));
            }
            if (sym_ast_equal(mem, tab, l, r)) return sym_make_num(mem, 1.0);
            return sym_make_bin(mem, tab, "/", l, r);
        }
        if (mem_slice_eq_cstr(mem, s, "^")) {
            if (r_num && fabs(rv) <= 1e-12) return sym_make_num(mem, 1.0);
            if (r_num && fabs(rv - 1.0) <= 1e-12) return l;
            if (l_num && fabs(lv) <= 1e-12) return sym_make_num(mem, 0.0);
            if (l_num && fabs(lv - 1.0) <= 1e-12) return sym_make_num(mem, 1.0);
            if (l_num && r_num) return sym_make_num(mem, pow(lv, rv));
            const AstNode *ln = mem_get_ast(mem, l);
            if (r_num && ln->kind == AST_BINARY && mem_slice_eq_cstr(mem, tab->syms[ln->ref], "^")) {
                double p1 = 0.0;
                if (sym_is_num(mem, ln->right, &p1)) {
                    return sym_simplify_ast(mem, tab,
                        sym_make_bin(mem, tab, "^", ln->left, sym_make_num(mem, p1 * rv)),
                        (uint8_t)(depth + 1));
                }
            }
            return sym_make_bin(mem, tab, "^", l, r);
        }
        return mem_alloc_ast(mem, AST_BINARY, n->sub, n->ref, l, r);
    }

    if (n->kind == AST_CALL && n->sub == 1 && n->left != MEM_NO_IDX) {
        StrSlice fn_s = tab->syms[n->ref];
        const AstNode *link = mem_get_ast(mem, n->left);
        uint16_t arg = sym_simplify_ast(mem, tab, link->left, (uint8_t)(depth + 1));
        double av = 0.0;
        if (sym_is_num(mem, arg, &av)) {
            if (mem_slice_eq_cstr(mem, fn_s, "sin") && fabs(av) <= 1e-12) return sym_make_num(mem, 0.0);
            if (mem_slice_eq_cstr(mem, fn_s, "cos") && fabs(av) <= 1e-12) return sym_make_num(mem, 1.0);
            if (mem_slice_eq_cstr(mem, fn_s, "tan") && fabs(av) <= 1e-12) return sym_make_num(mem, 0.0);
            if (mem_slice_eq_cstr(mem, fn_s, "exp") && fabs(av) <= 1e-12) return sym_make_num(mem, 1.0);
            if (mem_slice_eq_cstr(mem, fn_s, "ln")  && fabs(av - 1.0) <= 1e-12) return sym_make_num(mem, 0.0);
            if (mem_slice_eq_cstr(mem, fn_s, "sqrt") && av >= 0.0) {
                double r = sqrt(av);
                if (fabs(r - floor(r + 0.5)) <= 1e-12) return sym_make_num(mem, floor(r + 0.5));
            }
        }
        if (mem_slice_eq_cstr(mem, fn_s, "ln")) {
            const AstNode *an = mem_get_ast(mem, arg);
            if (an->kind == AST_CONST_SYM && mem_slice_eq_cstr(mem, tab->syms[an->ref], "e")) {
                return sym_make_num(mem, 1.0);
            }
        }
        uint16_t nlink = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, arg, MEM_NO_IDX);
        return mem_alloc_ast(mem, AST_CALL, 1, n->ref, nlink, MEM_NO_IDX);
    }

    return idx;
}

static bool sym_depends_on(const MemArena *mem, uint16_t idx, uint8_t var_slot) {
    if (idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_SLOT_LOAD) return (n->sub == var_slot);
    if (n->kind == AST_UNARY) return sym_depends_on(mem, n->left, var_slot);
    if (n->kind == AST_BINARY) {
        return sym_depends_on(mem, n->left, var_slot) || sym_depends_on(mem, n->right, var_slot);
    }
    if (n->kind == AST_CALL) {
        uint16_t cur = n->left;
        while (cur != MEM_NO_IDX) {
            const AstNode *l = mem_get_ast(mem, cur);
            if (sym_depends_on(mem, l->left, var_slot)) return true;
            cur = l->right;
        }
    }
    return false;
}

static uint16_t sym_diff_ast(MemArena *mem, SymTable *tab, uint16_t idx, uint8_t var_slot) {
    if (idx == MEM_NO_IDX || mem->has_error) return MEM_NO_IDX;
    if (!sym_depends_on(mem, idx, var_slot)) {
        return sym_make_num(mem, 0.0);
    }
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_SLOT_LOAD) {
        return sym_make_num(mem, (n->sub == var_slot) ? 1.0 : 0.0);
    }
    if (n->kind == AST_UNARY && n->sub == (uint8_t)FIX_PREFIX) {
        StrSlice s = tab->syms[n->ref];
        uint16_t u  = n->left;
        uint16_t du = sym_diff_ast(mem, tab, u, var_slot);
        if (mem_slice_eq_cstr(mem, s, "-")) return sym_make_neg(mem, tab, du);
        if (mem_slice_eq_cstr(mem, s, "+")) return du;
        if (mem_slice_eq_cstr(mem, s, "√")) {
            uint16_t denom = sym_make_bin(mem, tab, "*", sym_make_num(mem, 2.0),
                                          sym_make_call1(mem, tab, "sqrt", u));
            return sym_make_bin(mem, tab, "/", du, denom);
        }
    }
    if (n->kind == AST_BINARY) {
        StrSlice s = tab->syms[n->ref];
        uint16_t u  = n->left,  v  = n->right;
        uint16_t du = sym_diff_ast(mem, tab, u, var_slot);
        uint16_t dv = sym_diff_ast(mem, tab, v, var_slot);
        if (mem_slice_eq_cstr(mem, s, "+")) return sym_make_bin(mem, tab, "+", du, dv);
        if (mem_slice_eq_cstr(mem, s, "-")) return sym_make_bin(mem, tab, "-", du, dv);
        if (mem_slice_eq_cstr(mem, s, "*")) {
            uint16_t t1 = sym_make_bin(mem, tab, "*", du, v);
            uint16_t t2 = sym_make_bin(mem, tab, "*", u, dv);
            return sym_make_bin(mem, tab, "+", t1, t2);
        }
        if (mem_slice_eq_cstr(mem, s, "/")) {
            uint16_t num = sym_make_bin(mem, tab, "-",
                                        sym_make_bin(mem, tab, "*", du, v),
                                        sym_make_bin(mem, tab, "*", u, dv));
            uint16_t den = sym_make_bin(mem, tab, "^", v, sym_make_num(mem, 2.0));
            return sym_make_bin(mem, tab, "/", num, den);
        }
        if (mem_slice_eq_cstr(mem, s, "^")) {
            if (!sym_depends_on(mem, v, var_slot)) {
                uint16_t vm1  = sym_make_bin(mem, tab, "-", v, sym_make_num(mem, 1.0));
                uint16_t upow = sym_make_bin(mem, tab, "^", u, vm1);
                uint16_t base = sym_make_bin(mem, tab, "*", v, upow);
                return sym_make_bin(mem, tab, "*", base, du);
            }
            if (!sym_depends_on(mem, u, var_slot)) {
                uint16_t lnu  = sym_make_call1(mem, tab, "ln", u);
                uint16_t base = sym_make_bin(mem, tab, "*", idx, lnu);
                return sym_make_bin(mem, tab, "*", base, dv);
            }
            /* General u(x)^v(x) = u^v * (dv * ln(u) + v * du / u) */
            uint16_t lnu = sym_make_call1(mem, tab, "ln", u);
            uint16_t p1  = sym_make_bin(mem, tab, "*", dv, lnu);
            uint16_t p2  = sym_make_bin(mem, tab, "/", sym_make_bin(mem, tab, "*", v, du), u);
            return sym_make_bin(mem, tab, "*", idx, sym_make_bin(mem, tab, "+", p1, p2));
        }
    }
    if (n->kind == AST_CALL && n->sub == 1 && n->left != MEM_NO_IDX) {
        StrSlice s = tab->syms[n->ref];
        uint16_t u  = mem_get_ast(mem, n->left)->left;
        uint16_t du = sym_diff_ast(mem, tab, u, var_slot);
        uint16_t outer = MEM_NO_IDX;
        if (mem_slice_eq_cstr(mem, s, "sin")) {
            outer = sym_make_call1(mem, tab, "cos", u);
        } else if (mem_slice_eq_cstr(mem, s, "cos")) {
            outer = sym_make_neg(mem, tab, sym_make_call1(mem, tab, "sin", u));
        } else if (mem_slice_eq_cstr(mem, s, "tan")) {
            uint16_t cos_u = sym_make_call1(mem, tab, "cos", u);
            outer = sym_make_bin(mem, tab, "/", sym_make_num(mem, 1.0),
                                 sym_make_bin(mem, tab, "^", cos_u, sym_make_num(mem, 2.0)));
        } else if (mem_slice_eq_cstr(mem, s, "exp")) {
            outer = sym_make_call1(mem, tab, "exp", u);
        } else if (mem_slice_eq_cstr(mem, s, "ln")) {
            outer = sym_make_bin(mem, tab, "/", sym_make_num(mem, 1.0), u);
        } else if (mem_slice_eq_cstr(mem, s, "sqrt")) {
            uint16_t den = sym_make_bin(mem, tab, "*", sym_make_num(mem, 2.0),
                                        sym_make_call1(mem, tab, "sqrt", u));
            outer = sym_make_bin(mem, tab, "/", sym_make_num(mem, 1.0), den);
        } else if (mem_slice_eq_cstr(mem, s, "sinh")) {
            outer = sym_make_call1(mem, tab, "cosh", u);
        } else if (mem_slice_eq_cstr(mem, s, "cosh")) {
            outer = sym_make_call1(mem, tab, "sinh", u);
        } else if (mem_slice_eq_cstr(mem, s, "tanh")) {
            uint16_t ch = sym_make_call1(mem, tab, "cosh", u);
            outer = sym_make_bin(mem, tab, "/", sym_make_num(mem, 1.0),
                                 sym_make_bin(mem, tab, "^", ch, sym_make_num(mem, 2.0)));
        } else if (mem_slice_eq_cstr(mem, s, "asin")) {
            uint16_t rad = sym_make_bin(mem, tab, "-", sym_make_num(mem, 1.0),
                                        sym_make_bin(mem, tab, "^", u, sym_make_num(mem, 2.0)));
            outer = sym_make_bin(mem, tab, "/", sym_make_num(mem, 1.0),
                                 sym_make_call1(mem, tab, "sqrt", rad));
        } else if (mem_slice_eq_cstr(mem, s, "acos")) {
            uint16_t rad = sym_make_bin(mem, tab, "-", sym_make_num(mem, 1.0),
                                        sym_make_bin(mem, tab, "^", u, sym_make_num(mem, 2.0)));
            outer = sym_make_neg(mem, tab, sym_make_bin(mem, tab, "/", sym_make_num(mem, 1.0),
                                                        sym_make_call1(mem, tab, "sqrt", rad)));
        } else if (mem_slice_eq_cstr(mem, s, "atan")) {
            uint16_t den = sym_make_bin(mem, tab, "+", sym_make_num(mem, 1.0),
                                        sym_make_bin(mem, tab, "^", u, sym_make_num(mem, 2.0)));
            outer = sym_make_bin(mem, tab, "/", sym_make_num(mem, 1.0), den);
        }
        if (outer != MEM_NO_IDX) {
            return sym_make_bin(mem, tab, "*", outer, du);
        }
    }
    return sym_make_num(mem, 0.0);
}


static bool sym_extract_linear(const MemArena *mem, const SymTable *tab,
                               uint16_t idx, uint8_t var_slot,
                               double *out_a, double *out_b) {
    if (idx == MEM_NO_IDX) return false;
    double nv = 0.0;
    if (sym_is_num(mem, idx, &nv)) {
        *out_a = 0.0;
        *out_b = nv;
        return true;
    }
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_SLOT_LOAD && n->sub == var_slot) {
        *out_a = 1.0;
        *out_b = 0.0;
        return true;
    }
    if (!sym_depends_on(mem, idx, var_slot)) return false;
    if (n->kind == AST_UNARY && n->sub == (uint8_t)FIX_PREFIX) {
        StrSlice s = tab->syms[n->ref];
        double a = 0.0, b = 0.0;
        if (mem_slice_eq_cstr(mem, s, "-") && sym_extract_linear(mem, tab, n->left, var_slot, &a, &b)) {
            *out_a = -a;
            *out_b = -b;
            return true;
        }
        if (mem_slice_eq_cstr(mem, s, "+") && sym_extract_linear(mem, tab, n->left, var_slot, &a, &b)) {
            *out_a = a;
            *out_b = b;
            return true;
        }
        return false;
    }
    if (n->kind == AST_BINARY) {
        StrSlice s = tab->syms[n->ref];
        double al = 0.0, bl = 0.0, ar = 0.0, br = 0.0;
        if (mem_slice_eq_cstr(mem, s, "+")) {
            if (sym_extract_linear(mem, tab, n->left, var_slot, &al, &bl) &&
                sym_extract_linear(mem, tab, n->right, var_slot, &ar, &br)) {
                *out_a = al + ar;
                *out_b = bl + br;
                return true;
            }
        } else if (mem_slice_eq_cstr(mem, s, "-")) {
            if (sym_extract_linear(mem, tab, n->left, var_slot, &al, &bl) &&
                sym_extract_linear(mem, tab, n->right, var_slot, &ar, &br)) {
                *out_a = al - ar;
                *out_b = bl - br;
                return true;
            }
        } else if (mem_slice_eq_cstr(mem, s, "*")) {
            if (sym_is_num(mem, n->left, &al) &&
                sym_extract_linear(mem, tab, n->right, var_slot, &ar, &br)) {
                *out_a = al * ar;
                *out_b = al * br;
                return true;
            }
            if (sym_is_num(mem, n->right, &ar) &&
                sym_extract_linear(mem, tab, n->left, var_slot, &al, &bl)) {
                *out_a = al * ar;
                *out_b = bl * ar;
                return true;
            }
        } else if (mem_slice_eq_cstr(mem, s, "/")) {
            if (sym_is_num(mem, n->right, &ar) && fabs(ar) > 1e-12 &&
                sym_extract_linear(mem, tab, n->left, var_slot, &al, &bl)) {
                *out_a = al / ar;
                *out_b = bl / ar;
                return true;
            }
        }
    }
    return false;
}

static uint16_t sym_integ_ast(MemArena *mem, SymTable *tab, uint16_t idx, uint8_t var_slot, uint8_t depth) {
    if (idx == MEM_NO_IDX || mem->has_error || depth > 48) return MEM_NO_IDX;
    uint16_t var_ref = mem_alloc_ast(mem, AST_SLOT_LOAD, var_slot, 0, 0, 0);

    if (!sym_depends_on(mem, idx, var_slot)) {
        return sym_make_bin(mem, tab, "*", idx, var_ref);
    }

    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_SLOT_LOAD && n->sub == var_slot) {
        return sym_make_bin(mem, tab, "/",
                            sym_make_bin(mem, tab, "^", var_ref, sym_make_num(mem, 2.0)),
                            sym_make_num(mem, 2.0));
    }

    if (n->kind == AST_UNARY && n->sub == (uint8_t)FIX_PREFIX) {
        StrSlice s = tab->syms[n->ref];
        if (mem_slice_eq_cstr(mem, s, "-")) {
            uint16_t sub_i = sym_integ_ast(mem, tab, n->left, var_slot, (uint8_t)(depth + 1));
            return (sub_i != MEM_NO_IDX) ? sym_make_neg(mem, tab, sub_i) : MEM_NO_IDX;
        }
        if (mem_slice_eq_cstr(mem, s, "+")) {
            return sym_integ_ast(mem, tab, n->left, var_slot, (uint8_t)(depth + 1));
        }
        if (mem_slice_eq_cstr(mem, s, "√")) {
            double a = 0.0, b = 0.0;
            if (sym_extract_linear(mem, tab, n->left, var_slot, &a, &b) && fabs(a) > 1e-12) {
                uint16_t num = sym_make_bin(mem, tab, "*", sym_make_num(mem, 2.0),
                                            sym_make_bin(mem, tab, "^", n->left, sym_make_num(mem, 1.5)));
                return sym_make_bin(mem, tab, "/", num, sym_make_num(mem, 3.0 * a));
            }
        }
    }

    if (n->kind == AST_BINARY) {
        StrSlice s = tab->syms[n->ref];
        uint16_t u = n->left, v = n->right;
        if (mem_slice_eq_cstr(mem, s, "+")) {
            uint16_t iu = sym_integ_ast(mem, tab, u, var_slot, (uint8_t)(depth + 1));
            uint16_t iv = sym_integ_ast(mem, tab, v, var_slot, (uint8_t)(depth + 1));
            if (iu != MEM_NO_IDX && iv != MEM_NO_IDX) return sym_make_bin(mem, tab, "+", iu, iv);
            return MEM_NO_IDX;
        }
        if (mem_slice_eq_cstr(mem, s, "-")) {
            uint16_t iu = sym_integ_ast(mem, tab, u, var_slot, (uint8_t)(depth + 1));
            uint16_t iv = sym_integ_ast(mem, tab, v, var_slot, (uint8_t)(depth + 1));
            if (iu != MEM_NO_IDX && iv != MEM_NO_IDX) return sym_make_bin(mem, tab, "-", iu, iv);
            return MEM_NO_IDX;
        }
        if (mem_slice_eq_cstr(mem, s, "*")) {
            if (!sym_depends_on(mem, u, var_slot)) {
                uint16_t iv = sym_integ_ast(mem, tab, v, var_slot, (uint8_t)(depth + 1));
                return (iv != MEM_NO_IDX) ? sym_make_bin(mem, tab, "*", u, iv) : MEM_NO_IDX;
            }
            if (!sym_depends_on(mem, v, var_slot)) {
                uint16_t iu = sym_integ_ast(mem, tab, u, var_slot, (uint8_t)(depth + 1));
                return (iu != MEM_NO_IDX) ? sym_make_bin(mem, tab, "*", v, iu) : MEM_NO_IDX;
            }
            /* Integration by parts: \int u dv = u*v_int - \int (du * v_int) dx when du is constant wrt var_slot */
            uint16_t du = sym_simplify_ast(mem, tab, sym_diff_ast(mem, tab, u, var_slot), 0);
            if (!mem->has_error && !sym_depends_on(mem, du, var_slot)) {
                uint16_t iv = sym_integ_ast(mem, tab, v, var_slot, (uint8_t)(depth + 1));
                if (iv != MEM_NO_IDX && !mem->has_error) {
                    uint16_t du_iv = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", du, iv), 0);
                    uint16_t rem   = sym_integ_ast(mem, tab, du_iv, var_slot, (uint8_t)(depth + 1));
                    if (rem != MEM_NO_IDX && !mem->has_error) {
                        return sym_make_bin(mem, tab, "-", sym_make_bin(mem, tab, "*", u, iv), rem);
                    }
                }
                mem->has_error = false;
                mem->err_msg[0] = 0;
            }
            uint16_t dv = sym_simplify_ast(mem, tab, sym_diff_ast(mem, tab, v, var_slot), 0);
            if (!mem->has_error && !sym_depends_on(mem, dv, var_slot)) {
                uint16_t iu = sym_integ_ast(mem, tab, u, var_slot, (uint8_t)(depth + 1));
                if (iu != MEM_NO_IDX && !mem->has_error) {
                    uint16_t dv_iu = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", dv, iu), 0);
                    uint16_t rem   = sym_integ_ast(mem, tab, dv_iu, var_slot, (uint8_t)(depth + 1));
                    if (rem != MEM_NO_IDX && !mem->has_error) {
                        return sym_make_bin(mem, tab, "-", sym_make_bin(mem, tab, "*", v, iu), rem);
                    }
                }
                mem->has_error = false;
                mem->err_msg[0] = 0;
            }
        }
        if (mem_slice_eq_cstr(mem, s, "/")) {
            if (!sym_depends_on(mem, v, var_slot)) {
                uint16_t iu = sym_integ_ast(mem, tab, u, var_slot, (uint8_t)(depth + 1));
                return (iu != MEM_NO_IDX) ? sym_make_bin(mem, tab, "/", iu, v) : MEM_NO_IDX;
            }
            if (!sym_depends_on(mem, u, var_slot)) {
                double a = 0.0, b = 0.0;
                if (sym_extract_linear(mem, tab, v, var_slot, &a, &b) && fabs(a) > 1e-12) {
                    uint16_t lnv = sym_make_call1(mem, tab, "ln", v);
                    uint16_t num = sym_make_bin(mem, tab, "*", u, lnv);
                    return sym_make_bin(mem, tab, "/", num, sym_make_num(mem, a));
                }
            }
        }
        if (mem_slice_eq_cstr(mem, s, "^")) {
            double p = 0.0;
            if (sym_is_num(mem, v, &p)) {
                double a = 0.0, b = 0.0;
                if (sym_extract_linear(mem, tab, u, var_slot, &a, &b) && fabs(a) > 1e-12) {
                    if (fabs(p + 1.0) <= 1e-12) {
                        return sym_make_bin(mem, tab, "/", sym_make_call1(mem, tab, "ln", u), sym_make_num(mem, a));
                    }
                    uint16_t np1  = sym_make_num(mem, p + 1.0);
                    uint16_t upow = sym_make_bin(mem, tab, "^", u, np1);
                    return sym_make_bin(mem, tab, "/", upow, sym_make_num(mem, a * (p + 1.0)));
                }
            }
            const AstNode *un = mem_get_ast(mem, u);
            if (un->kind == AST_CONST_SYM && mem_slice_eq_cstr(mem, tab->syms[un->ref], "e")) {
                double a = 0.0, b = 0.0;
                if (sym_extract_linear(mem, tab, v, var_slot, &a, &b) && fabs(a) > 1e-12) {
                    return sym_make_bin(mem, tab, "/", idx, sym_make_num(mem, a));
                }
            }
        }
    }

    if (n->kind == AST_CALL && n->sub == 1 && n->left != MEM_NO_IDX) {
        StrSlice s = tab->syms[n->ref];
        uint16_t u = mem_get_ast(mem, n->left)->left;
        double a = 0.0, b = 0.0;
        if (sym_extract_linear(mem, tab, u, var_slot, &a, &b) && fabs(a) > 1e-12) {
            uint16_t ant = MEM_NO_IDX;
            if (mem_slice_eq_cstr(mem, s, "sin")) {
                ant = sym_make_neg(mem, tab, sym_make_call1(mem, tab, "cos", u));
            } else if (mem_slice_eq_cstr(mem, s, "cos")) {
                ant = sym_make_call1(mem, tab, "sin", u);
            } else if (mem_slice_eq_cstr(mem, s, "exp")) {
                ant = sym_make_call1(mem, tab, "exp", u);
            } else if (mem_slice_eq_cstr(mem, s, "sinh")) {
                ant = sym_make_call1(mem, tab, "cosh", u);
            } else if (mem_slice_eq_cstr(mem, s, "cosh")) {
                ant = sym_make_call1(mem, tab, "sinh", u);
            } else if (mem_slice_eq_cstr(mem, s, "sqrt")) {
                ant = sym_make_bin(mem, tab, "/",
                                   sym_make_bin(mem, tab, "*", sym_make_num(mem, 2.0),
                                                sym_make_bin(mem, tab, "^", u, sym_make_num(mem, 1.5))),
                                   sym_make_num(mem, 3.0));
            } else if (mem_slice_eq_cstr(mem, s, "ln")) {
                ant = sym_make_bin(mem, tab, "-",
                                   sym_make_bin(mem, tab, "*", u, sym_make_call1(mem, tab, "ln", u)),
                                   u);
            }
            if (ant != MEM_NO_IDX) {
                return sym_make_bin(mem, tab, "/", ant, sym_make_num(mem, a));
            }
        }
    }

    mem_err(mem, "Symbolic antiderivative does not have a elementary table rule for this expression");
    return MEM_NO_IDX;
}

static bool sym_is_zero_ast(const MemArena *mem, uint16_t idx) {
    double v = 0.0;
    return sym_is_num(mem, idx, &v) && fabs(v) <= 1e-12;
}

/* Decompose expression wrt `var_slot` into quadratic A*x^2 + B*x + C = 0 where A, B, C do not depend on `var_slot` */
static bool sym_decompose_poly(MemArena *mem, SymTable *tab, uint16_t idx, uint8_t var_slot,
                               uint16_t *out_a, uint16_t *out_b, uint16_t *out_c, uint8_t depth) {
    if (idx == MEM_NO_IDX || mem->has_error || depth > 48) return false;
    if (!sym_depends_on(mem, idx, var_slot)) {
        *out_a = sym_make_num(mem, 0.0);
        *out_b = sym_make_num(mem, 0.0);
        *out_c = idx;
        return true;
    }
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_SLOT_LOAD && n->sub == var_slot) {
        *out_a = sym_make_num(mem, 0.0);
        *out_b = sym_make_num(mem, 1.0);
        *out_c = sym_make_num(mem, 0.0);
        return true;
    }
    if (n->kind == AST_UNARY && n->sub == (uint8_t)FIX_PREFIX) {
        StrSlice s = tab->syms[n->ref];
        uint16_t a1 = 0, b1 = 0, c1 = 0;
        if (mem_slice_eq_cstr(mem, s, "-") &&
            sym_decompose_poly(mem, tab, n->left, var_slot, &a1, &b1, &c1, (uint8_t)(depth + 1))) {
            *out_a = sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, a1), 0);
            *out_b = sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, b1), 0);
            *out_c = sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, c1), 0);
            return true;
        }
        if (mem_slice_eq_cstr(mem, s, "+")) {
            return sym_decompose_poly(mem, tab, n->left, var_slot, out_a, out_b, out_c, (uint8_t)(depth + 1));
        }
        return false;
    }
    if (n->kind == AST_BINARY) {
        StrSlice s = tab->syms[n->ref];
        uint16_t al = 0, bl = 0, cl = 0, ar = 0, br = 0, cr = 0;
        if (mem_slice_eq_cstr(mem, s, "+") || mem_slice_eq_cstr(mem, s, "-")) {
            const char *op = mem_slice_eq_cstr(mem, s, "+") ? "+" : "-";
            if (!sym_decompose_poly(mem, tab, n->left,  var_slot, &al, &bl, &cl, (uint8_t)(depth + 1)) ||
                !sym_decompose_poly(mem, tab, n->right, var_slot, &ar, &br, &cr, (uint8_t)(depth + 1))) {
                return false;
            }
            *out_a = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, op, al, ar), 0);
            *out_b = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, op, bl, br), 0);
            *out_c = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, op, cl, cr), 0);
            return true;
        }
        if (mem_slice_eq_cstr(mem, s, "*")) {
            if (!sym_decompose_poly(mem, tab, n->left,  var_slot, &al, &bl, &cl, (uint8_t)(depth + 1)) ||
                !sym_decompose_poly(mem, tab, n->right, var_slot, &ar, &br, &cr, (uint8_t)(depth + 1))) {
                return false;
            }
            if (sym_is_zero_ast(mem, al) && sym_is_zero_ast(mem, bl)) {
                *out_a = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", cl, ar), 0);
                *out_b = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", cl, br), 0);
                *out_c = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", cl, cr), 0);
                return true;
            }
            if (sym_is_zero_ast(mem, ar) && sym_is_zero_ast(mem, br)) {
                *out_a = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", al, cr), 0);
                *out_b = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", bl, cr), 0);
                *out_c = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", cl, cr), 0);
                return true;
            }
            if (sym_is_zero_ast(mem, al) && sym_is_zero_ast(mem, ar)) {
                *out_a = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", bl, br), 0);
                uint16_t t1 = sym_make_bin(mem, tab, "*", bl, cr);
                uint16_t t2 = sym_make_bin(mem, tab, "*", cl, br);
                *out_b = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "+", t1, t2), 0);
                *out_c = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "*", cl, cr), 0);
                return true;
            }
            return false;
        }
        if (mem_slice_eq_cstr(mem, s, "/")) {
            if (!sym_depends_on(mem, n->right, var_slot) &&
                sym_decompose_poly(mem, tab, n->left, var_slot, &al, &bl, &cl, (uint8_t)(depth + 1))) {
                *out_a = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "/", al, n->right), 0);
                *out_b = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "/", bl, n->right), 0);
                *out_c = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "/", cl, n->right), 0);
                return true;
            }
            return false;
        }
        if (mem_slice_eq_cstr(mem, s, "^")) {
            double p = 0.0;
            if (sym_is_num(mem, n->right, &p) && fabs(p - 2.0) <= 1e-12 &&
                sym_decompose_poly(mem, tab, n->left, var_slot, &al, &bl, &cl, (uint8_t)(depth + 1)) &&
                sym_is_zero_ast(mem, al)) {
                *out_a = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "^", bl, sym_make_num(mem, 2.0)), 0);
                *out_b = sym_simplify_ast(mem, tab,
                             sym_make_bin(mem, tab, "*", sym_make_num(mem, 2.0),
                                          sym_make_bin(mem, tab, "*", bl, cl)), 0);
                *out_c = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "^", cl, sym_make_num(mem, 2.0)), 0);
                return true;
            }
        }
    }
    return false;
}

static int sym_op_prec(const MemArena *mem, const SymTable *tab, uint16_t sym_id) {
    StrSlice s = tab->syms[sym_id];
    if (mem_slice_eq_cstr(mem, s, "+") || mem_slice_eq_cstr(mem, s, "-")) return 50;
    if (mem_slice_eq_cstr(mem, s, "*") || mem_slice_eq_cstr(mem, s, "/")) return 60;
    if (mem_slice_eq_cstr(mem, s, "^")) return 85;
    return 40;
}

static void sym_buf_append(MemArena *mem, char *buf, size_t cap, size_t *used, const char *fmt, ...) {
    if (!buf || cap == 0 || mem->has_error) return;
    if (*used + 1 >= cap) {
        mem_err(mem, "Symbolic expression output exceeds buffer capacity (%zu bytes)", cap);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used) {
        *used = cap - 1;
        buf[cap - 1] = '\0';
        mem_err(mem, "Symbolic expression output exceeds buffer capacity (%zu bytes)", cap);
        return;
    }
    *used += (size_t)n;
}

static void sym_ast_to_str(MemArena *mem, const SymTable *tab, uint16_t idx,
                           int parent_prec, char *buf, size_t cap, size_t *used) {
    if (idx == MEM_NO_IDX || mem->has_error) return;
    if (*used + 1 >= cap) {
        mem_err(mem, "Symbolic expression output exceeds buffer capacity (%zu bytes)", cap);
        return;
    }
    const AstNode *n = mem_get_ast(mem, idx);
    if (n->kind == AST_LIT) {
        double v = mem_read_pool(mem, n->ref);
        if (fabs(v) < 1e-15) v = 0.0;
        sym_buf_append(mem, buf, cap, used, "%.10g", v);
        return;
    }
    if (n->kind == AST_SLOT_LOAD) {
        uint8_t slot = n->sub;
        if (slot < 64 && mem->local_slot_names[slot].len > 0) {
            StrSlice nm = mem->local_slot_names[slot];
            sym_buf_append(mem, buf, cap, used, "%.*s",
                           (int)nm.len, mem_str_ptr(mem, nm));
        } else {
            sym_buf_append(mem, buf, cap, used, "x");
        }
        return;
    }
    if (n->kind == AST_CONST_SYM) {
        StrSlice cs = tab->syms[n->ref];
        sym_buf_append(mem, buf, cap, used, "%.*s",
                       (int)cs.len, mem_str_ptr(mem, cs));
        return;
    }
    if (n->kind == AST_UNARY && n->sub == (uint8_t)FIX_PREFIX) {
        StrSlice os = tab->syms[n->ref];
        bool wrap = (parent_prec > 80);
        if (wrap) sym_buf_append(mem, buf, cap, used, "(");
        sym_buf_append(mem, buf, cap, used, "%.*s",
                       (int)os.len, mem_str_ptr(mem, os));
        sym_ast_to_str(mem, tab, n->left, 80, buf, cap, used);
        if (wrap) sym_buf_append(mem, buf, cap, used, ")");
        return;
    }
    if (n->kind == AST_BINARY) {
        StrSlice os = tab->syms[n->ref];
        int prec = sym_op_prec(mem, tab, n->ref);
        bool wrap = (prec < parent_prec);
        if (wrap) sym_buf_append(mem, buf, cap, used, "(");
        sym_ast_to_str(mem, tab, n->left, prec, buf, cap, used);
        sym_buf_append(mem, buf, cap, used, " %.*s ",
                       (int)os.len, mem_str_ptr(mem, os));
        int r_prec = (mem_slice_eq_cstr(mem, os, "-") || mem_slice_eq_cstr(mem, os, "/"))
                     ? (prec + 1) : prec;
        sym_ast_to_str(mem, tab, n->right, r_prec, buf, cap, used);
        if (wrap) sym_buf_append(mem, buf, cap, used, ")");
        return;
    }
    if (n->kind == AST_CALL) {
        StrSlice fs = tab->syms[n->ref];
        sym_buf_append(mem, buf, cap, used, "%.*s(",
                       (int)fs.len, mem_str_ptr(mem, fs));
        uint16_t cur = n->left;
        bool first = true;
        while (cur != MEM_NO_IDX && !mem->has_error) {
            const AstNode *l = mem_get_ast(mem, cur);
            if (!first) sym_buf_append(mem, buf, cap, used, ", ");
            sym_ast_to_str(mem, tab, l->left, 0, buf, cap, used);
            first = false;
            cur = l->right;
        }
        sym_buf_append(mem, buf, cap, used, ")");
    }
}

static void sym_extract_var_arg(MemArena *mem, uint16_t frame_base, uint16_t var_ast,
                                char *vname, size_t vcap, uint8_t *out_slot) {
    snprintf(vname, vcap, "x");
    if (out_slot) *out_slot = 0;
    const AstNode *vn = mem_get_ast(mem, var_ast);
    if (vn->kind == AST_SLOT_LOAD) {
        uint8_t s = vn->sub;
        if (out_slot) *out_slot = s;
        Value sv = mem_get_slot(mem, frame_base, s);
        if (mem->str_type_id != 0 && val_get_type(mem, sv) == mem->str_type_id) {
             (void)val_as_cstr(mem, sv, vname, vcap);
             return;
        }
        if (s < 64 && mem->local_slot_names[s].len > 0) {
            StrSlice nm = mem->local_slot_names[s];
            uint16_t cl = (nm.len < vcap - 1) ? nm.len : (uint16_t)(vcap - 1);
            memcpy(vname, mem_str_ptr(mem, nm), cl);
            vname[cl] = '\0';
        }
    } else if (vn->kind == AST_LIT && vn->sub == mem->str_type_id) {
        StrSlice nm = { vn->left, vn->right };
        uint16_t cl = (nm.len < vcap - 1) ? nm.len : (uint16_t)(vcap - 1);
        memcpy(vname, mem_str_ptr(mem, nm), cl);
        vname[cl] = '\0';
    }
}

static bool sym_parse_str_arg(MemArena *mem, SymTable *tab, uint16_t arg_ast,
                              uint16_t frame_base, uint8_t cur_ret_type,
                              char *io_vname, size_t vname_cap, uint8_t *io_var_slot,
                              uint16_t *out_ast) {
    const AstNode *an = mem_get_ast(mem, arg_ast);
    StrSlice s = { 0, 0 };
    bool is_str = false;
    if (an->kind == AST_LIT && an->sub == mem->str_type_id) {
        s.off  = an->left;
        s.len  = an->right;
        is_str = true;
    } else if (an->kind == AST_SLOT_LOAD) {
        Value sv = mem_get_slot(mem, frame_base, an->sub);
        if (mem->str_type_id != 0 && val_get_type(mem, sv) == mem->str_type_id) {
            s      = val_get_str(mem, sv);
            is_str = true;
        } else if (mem->fn_type_id != 0 && val_get_type(mem, sv) == mem->fn_type_id) {
            uint16_t fhdr  = val_get_pool_idx(sv);
            uint32_t raw0  = (uint32_t)mem_read_pool(mem, fhdr + 0);
            uint8_t  fslot = (uint8_t)mem_read_pool(mem, fhdr + 5);
            *out_ast = (uint16_t)(raw0 & 0xFFFFu);
            if (io_var_slot) *io_var_slot = fslot;
            if (io_vname && vname_cap > 0 && fslot < 64 && mem->local_slot_names[fslot].len > 0) {
                StrSlice nm = mem->local_slot_names[fslot];
                uint16_t cl = (nm.len < vname_cap - 1) ? nm.len : (uint16_t)(vname_cap - 1);
                memcpy(io_vname, mem_str_ptr(mem, nm), cl);
                io_vname[cl] = '\0';
            }
            return true;
        }
    }
    if (is_str) {
        char sbuf[4096];
        if (s.len >= sizeof(sbuf)) {
            mem_err(mem, "Symbolic expression string exceeds maximum length (%zu bytes)", sizeof(sbuf) - 1);
            return false;
        }
        uint16_t slen = s.len;
        memcpy(sbuf, mem_str_ptr(mem, s), slen);
        sbuf[slen] = '\0';
        Parser SP;
        parser_init(&SP, mem, tab, sbuf, slen, 0, false, false);
        SP.in_symbolic_expr = 1;
        if (io_vname && io_vname[0] != '\0') {
            int sslot = parser_add_local(&SP, io_vname, (uint16_t)strlen(io_vname), mem->fast_real_type);
            if (io_var_slot) *io_var_slot = (uint8_t)sslot;
        }
        lex_next(&SP.lex);
        uint16_t r = parser_parse_expr(&SP, 0);
        if (mem->has_error || r == MEM_NO_IDX) return false;
        *out_ast = r;
        return true;
    }
    (void)cur_ret_type;
    *out_ast = arg_ast;
    return true;
}

Value delegate_eval_sym_meta(MemArena *mem, SymTable *tab,
                             uint8_t mtag, const AstNode *n,
                             uint16_t frame_base, uint8_t cur_ret_type) {
    uint16_t arg_asts[16];
    uint8_t  argc = 0;
    uint16_t cur  = n->left;
    while (cur != MEM_NO_IDX && argc < 16) {
        const AstNode *link = mem_get_ast(mem, cur);
        arg_asts[argc++] = link->left;
        cur = link->right;
    }

    if (mtag == INTR_SYM_SIMPLIFY) {
        if (argc < 1) {
            mem_err(mem, "sym_simplify(expr) requires an expression");
            return val_void();
        }
        uint16_t expr_ast = MEM_NO_IDX;
        if (!sym_parse_str_arg(mem, tab, arg_asts[0], frame_base, cur_ret_type, NULL, 0, NULL, &expr_ast)) {
            return val_void();
        }
        uint16_t simp = sym_simplify_ast(mem, tab, expr_ast, 0);
        simp = sym_simplify_ast(mem, tab, simp, 0);
        char out[4096];
        size_t used = 0;
        out[0] = '\0';
        sym_ast_to_str(mem, tab, simp, 0, out, sizeof(out), &used);
        if (mem->has_error) return val_void();
        return val_from_cstr(mem, out);
    }

    if (mtag == INTR_SYM_DIFF) {
        if (argc < 2) {
            mem_err(mem, "sym_diff(x, expr[, order]) requires variable and expression");
            return val_void();
        }
        char vname[32];
        uint8_t var_slot = 0;
        sym_extract_var_arg(mem, frame_base, arg_asts[0], vname, sizeof(vname), &var_slot);
        uint16_t expr_ast = MEM_NO_IDX;
        if (!sym_parse_str_arg(mem, tab, arg_asts[1], frame_base, cur_ret_type, vname, sizeof(vname), &var_slot, &expr_ast)) {
            return val_void();
        }
        int order = 1;
        if (argc >= 3) {
            Value vord = eval_ast(mem, tab, arg_asts[2], frame_base, cur_ret_type);
            if (mem->has_error) return val_void();
            order = (int)val_get_f64(mem, vord);
            if (order == -1) {
                return val_from_cstr(mem, vname);
            }
            if (order < 0) order = 0;
            if (order > 8) order = 8;
        }
        uint16_t cur_ast = sym_simplify_ast(mem, tab, expr_ast, 0);
        for (int k = 0; k < order && !mem->has_error; ++k) {
            cur_ast = sym_diff_ast(mem, tab, cur_ast, var_slot);
            cur_ast = sym_simplify_ast(mem, tab, cur_ast, 0);
            cur_ast = sym_simplify_ast(mem, tab, cur_ast, 0);
        }
        char out[4096];
        size_t used = 0;
        out[0] = '\0';
        sym_ast_to_str(mem, tab, cur_ast, 0, out, sizeof(out), &used);
        if (mem->has_error) return val_void();
        return val_from_cstr(mem, out);
    }

    if (mtag == INTR_SYM_INTEG) {
        if (argc < 2) {
            mem_err(mem, "sym_integ(x, expr) requires variable and expression");
            return val_void();
        }
        char vname[32];
        uint8_t var_slot = 0;
        sym_extract_var_arg(mem, frame_base, arg_asts[0], vname, sizeof(vname), &var_slot);
        uint16_t expr_ast = MEM_NO_IDX;
        if (!sym_parse_str_arg(mem, tab, arg_asts[1], frame_base, cur_ret_type, vname, sizeof(vname), &var_slot, &expr_ast)) {
            return val_void();
        }
        uint16_t cur_ast = sym_simplify_ast(mem, tab, expr_ast, 0);
        cur_ast = sym_integ_ast(mem, tab, cur_ast, var_slot, 0);
        if (mem->has_error || cur_ast == MEM_NO_IDX) return val_void();
        cur_ast = sym_simplify_ast(mem, tab, cur_ast, 0);
        cur_ast = sym_simplify_ast(mem, tab, cur_ast, 0);
        char out[4096];
        size_t used = 0;
        out[0] = '\0';
        sym_ast_to_str(mem, tab, cur_ast, 0, out, sizeof(out), &used);
        if (mem->has_error) return val_void();
        return val_from_cstr(mem, out);
    }

    if (mtag == INTR_SYM_SOLVE) {
        if (argc < 2) {
            mem_err(mem, "sym_solve(x, eq) requires variable and equation");
            return val_void();
        }
        char vname[32];
        uint8_t var_slot = 0;
        sym_extract_var_arg(mem, frame_base, arg_asts[0], vname, sizeof(vname), &var_slot);
        uint16_t eq_ast = MEM_NO_IDX;
        if (!sym_parse_str_arg(mem, tab, arg_asts[1], frame_base, cur_ret_type, vname, sizeof(vname), &var_slot, &eq_ast)) {
            return val_void();
        }
        const AstNode *en = mem_get_ast(mem, eq_ast);
        if (en->kind == AST_BINARY) {
            StrSlice os = tab->syms[en->ref];
            if (mem_slice_eq_cstr(mem, os, "==") || mem_slice_eq_cstr(mem, os, "=") ||
                mem_slice_eq_cstr(mem, os, ":=") || mem_slice_eq_cstr(mem, os, "~")) {
                eq_ast = sym_make_bin(mem, tab, "-", en->left, en->right);
            }
        }
        eq_ast = sym_simplify_ast(mem, tab, eq_ast, 0);
        uint16_t a_ast = 0, b_ast = 0, c_ast = 0;
        if (!sym_decompose_poly(mem, tab, eq_ast, var_slot, &a_ast, &b_ast, &c_ast, 0)) {
            mem_err(mem, "sym_solve(x, eq) supports linear and quadratic symbolic/numeric equations in '%s'", vname);
            return val_void();
        }
        a_ast = sym_simplify_ast(mem, tab, a_ast, 0);
        b_ast = sym_simplify_ast(mem, tab, b_ast, 0);
        c_ast = sym_simplify_ast(mem, tab, c_ast, 0);

        char out[4096];
        size_t used = 0;
        out[0] = '\0';

        if (sym_is_zero_ast(mem, a_ast)) {
            if (sym_is_zero_ast(mem, b_ast)) {
                mem_err(mem, "Equation in sym_solve does not depend on '%s' (or is degenerate)", vname);
                return val_void();
            }
            uint16_t neg_c = sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, c_ast), 0);
            uint16_t sol   = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "/", neg_c, b_ast), 0);
            sol = sym_simplify_ast(mem, tab, sol, 0);
            sym_ast_to_str(mem, tab, sol, 0, out, sizeof(out), &used);
            if (mem->has_error) return val_void();
            return val_from_cstr(mem, out);
        }

        double av = 0.0, bv = 0.0, cv = 0.0;
        if (sym_is_num(mem, a_ast, &av) && sym_is_num(mem, b_ast, &bv) && sym_is_num(mem, c_ast, &cv)) {
            double disc = bv * bv - 4.0 * av * cv;
            if (disc < -1e-12) {
                mem_err(mem, "Quadratic equation in sym_solve has no real roots (disc = %.6g)", disc);
                return val_void();
            }
            if (fabs(disc) <= 1e-12) {
                double r0 = -bv / (2.0 * av);
                if (fabs(r0) < 1e-15) r0 = 0.0;
                snprintf(out, sizeof(out), "%.10g", r0);
                return val_from_cstr(mem, out);
            }
            double sd = sqrt(disc);
            double r1 = (-bv - sd) / (2.0 * av);
            double r2 = (-bv + sd) / (2.0 * av);
            if (r1 > r2) { double t = r1; r1 = r2; r2 = t; }
            if (fabs(r1) < 1e-15) r1 = 0.0;
            if (fabs(r2) < 1e-15) r2 = 0.0;
            snprintf(out, sizeof(out), "[%.10g, %.10g]", r1, r2);
            return val_from_cstr(mem, out);
        }

        if (sym_is_zero_ast(mem, b_ast)) {
            uint16_t neg_c = sym_simplify_ast(mem, tab, sym_make_neg(mem, tab, c_ast), 0);
            uint16_t quot  = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "/", neg_c, a_ast), 0);
            uint16_t sol   = sym_simplify_ast(mem, tab, sym_make_call1(mem, tab, "sqrt", quot), 0);
            sym_ast_to_str(mem, tab, sol, 0, out, sizeof(out), &used);
            if (mem->has_error) return val_void();
            return val_from_cstr(mem, out);
        }
        uint16_t b2   = sym_make_bin(mem, tab, "^", b_ast, sym_make_num(mem, 2.0));
        uint16_t ac4  = sym_make_bin(mem, tab, "*", sym_make_num(mem, 4.0),
                                     sym_make_bin(mem, tab, "*", a_ast, c_ast));
        uint16_t disc = sym_make_call1(mem, tab, "sqrt", sym_make_bin(mem, tab, "-", b2, ac4));
        uint16_t num  = sym_make_bin(mem, tab, "+", sym_make_neg(mem, tab, b_ast), disc);
        uint16_t den  = sym_make_bin(mem, tab, "*", sym_make_num(mem, 2.0), a_ast);
        uint16_t sol  = sym_simplify_ast(mem, tab, sym_make_bin(mem, tab, "/", num, den), 0);
        sym_ast_to_str(mem, tab, sol, 0, out, sizeof(out), &used);
        if (mem->has_error) return val_void();
        return val_from_cstr(mem, out);
    }

    if (mtag == INTR_SYM_EVAL) {
        if (argc < 3) {
            mem_err(mem, "sym_eval(expr_str, x, val) requires 3 arguments");
            return val_void();
        }
        Value s_val = eval_ast(mem, tab, arg_asts[0], frame_base, cur_ret_type);
        Value x_val = eval_ast(mem, tab, arg_asts[2], frame_base, cur_ret_type);
        if (mem->has_error) return val_void();
        char vname[32];
        uint8_t var_slot = 0;
        const AstNode *vn = mem_get_ast(mem, arg_asts[1]);
        if (vn->kind != AST_SLOT_LOAD && !(vn->kind == AST_LIT && vn->sub == mem->str_type_id)) {
            mem_err(mem, "Second argument to sym_eval must be a variable name");
            return val_void();
        }
        sym_extract_var_arg(mem, frame_base, arg_asts[1], vname, sizeof(vname), &var_slot);
        char expr_buf[4096];
        if (!val_as_cstr(mem, s_val, expr_buf, sizeof(expr_buf))) {
            mem_err(mem, "First argument to sym_eval must be a string expression");
            return val_void();
        }
        Parser SP;
        parser_init(&SP, mem, tab, expr_buf, (uint16_t)strlen(expr_buf), 0, false, false);
        int sslot = parser_add_local(&SP, vname, (uint16_t)strlen(vname), mem->fast_real_type);
        lex_next(&SP.lex);
        uint16_t r = parser_parse_expr(&SP, 0);
        if (mem->has_error || r == MEM_NO_IDX) return val_void();
        uint16_t sub_frame = mem_push_frame(mem, SP.local_count > 0 ? SP.local_count : 1);
        if (mem->has_error) return val_void();
        mem_set_slot(mem, sub_frame, (uint8_t)sslot, x_val);
        Value res = eval_ast(mem, tab, r, sub_frame, cur_ret_type);
        mem_pop_frame(mem, sub_frame);
        return res;
    }

    return val_void();
}
