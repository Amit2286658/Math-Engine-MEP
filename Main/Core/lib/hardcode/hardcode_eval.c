/*
 * hardcode_eval.c — Hardcoded Range Chains, Pipe Rewrite, For-In Loops, Unpacking & Switch Eval
 */
#include "hardcode_internal.h"

bool hardcode_try_eval_variadic_vec(MemArena *mem, SymTable *tab,
                                    const AstNode *n, uint16_t frame_base,
                                    uint8_t cur_ret_type, Value *out_val) {
    if (n->ref >= tab->sym_count || mem->vec_type_id == 0) return false;
    int kind_code = container_kind_from_sym_hc(mem, tab, n->ref);
    if (kind_code < 0) return false;

    bool is_vec_call = (kind_code == 0);
    bool is_map_call = (kind_code == 4 || kind_code == 5);
    bool has_spec    = (n->right != 0 && n->right != MEM_NO_IDX && (n->right & 0x8000u) != 0);

    if (is_map_call) {
        if (mem->map_type_id == 0) return false;
        /* Distinguish 2-arg HOF `map(coll, fn)` from `map(k1, v1, ...)` */
        if (kind_code == 4 && !has_spec && n->sub == 2) {
            const AstNode *l1 = mem_get_ast(mem, n->left);
            Value a0 = eval_ast(mem, tab, l1->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) {
                *out_val = val_void();
                return true;
            }
            uint16_t t0 = val_get_type(mem, a0);
            if (t0 == mem->vec_type_id || t0 == mem->mat_type_id) {
                const AstNode *l2 = mem_get_ast(mem, l1->right);
                Value a1 = eval_ast(mem, tab, l2->left, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) {
                    *out_val = val_void();
                    return true;
                }
                Value map_args[2] = { a0, a1 };
                *out_val = eval_dispatch_overload(mem, tab, n->ref, FIX_FUNC, 2, map_args);
                return true;
            }
            if ((n->sub & 1u) != 0) {
                mem_err(mem, "map() constructor requires key-value pairs (even argument count)");
                *out_val = val_void();
                return true;
            }
            const AstNode *l2 = mem_get_ast(mem, l1->right);
            Value a1 = eval_ast(mem, tab, l2->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) {
                *out_val = val_void();
                return true;
            }
            uint16_t m_hdr = mem_map_alloc_hdr(mem, 0, 2);
            if (mem->has_error) { *out_val = val_void(); return true; }
            Value mval = val_make_comp(mem, mem->map_type_id, m_hdr, MEM_MAP_HDR_SLOTS);
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_SET, mval, a0, a1);
            *out_val = mem->has_error ? val_void() : mval;
            return true;
        }
        if ((n->sub & 1u) != 0) {
            mem_err(mem, "map()/dict() constructor requires an even number of key-value arguments");
            *out_val = val_void();
            return true;
        }
        uint8_t  raw_ltid   = has_spec ? (uint8_t)(n->right & 0x7Fu) : 0;
        uint16_t locked_tid = (raw_ltid == SYM_ELEM_ANY) ? SYM_TYPE_ANY : raw_ltid;
        uint8_t  locked_uid = has_spec ? (uint8_t)((n->right >> 7u) & 0x7Fu) : 0;
        uint16_t hdr0       = (uint16_t)(((uint32_t)locked_uid << 8u) | (uint32_t)(locked_tid & 0xFFu));
        uint16_t m_hdr      = mem_map_alloc_hdr(mem, hdr0, (uint16_t)(n->sub / 2u));
        if (mem->has_error) { *out_val = val_void(); return true; }
        Value mval = val_make_comp(mem, mem->map_type_id, m_hdr, MEM_MAP_HDR_SLOTS);
        uint16_t cur_link = n->left;
        while (cur_link != MEM_NO_IDX && !mem->has_error) {
            const AstNode *lk = mem_get_ast(mem, cur_link);
            const AstNode *lv = mem_get_ast(mem, lk->right);
            Value kval = eval_ast(mem, tab, lk->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) { *out_val = val_void(); return true; }
            Value vval = eval_ast(mem, tab, lv->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) { *out_val = val_void(); return true; }
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_MAP_SET, mval, kval, vval);
            cur_link = lv->right;
        }
        *out_val = mem->has_error ? val_void() : mval;
        return true;
    }

    bool has_active_ctor = false;
    for (uint16_t i = tab->sym_first_ov[n->ref]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        if (sym_overloads(tab)[i].fixity == FIX_FUNC && sym_mod_enabled(tab, sym_overloads(tab)[i].mod_id)) {
            has_active_ctor = true;
            break;
        }
    }
    if (!has_active_ctor) return false;

    if (!has_spec) {
        if (!is_vec_call && n->sub < 2) return false;
        if (is_vec_call && n->sub == 0) return false;
    }

    uint8_t  raw_ltid   = has_spec ? (uint8_t)(n->right & 0x7Fu) : 0;
    uint16_t locked_tid = (raw_ltid == SYM_ELEM_ANY) ? SYM_TYPE_ANY : raw_ltid;
    uint8_t  locked_uid = has_spec ? (uint8_t)((n->right >> 7u) & 0x7Fu) : 0;
    uint16_t hdr0       = (uint16_t)(((uint32_t)locked_uid << 8u) | (uint32_t)(locked_tid & 0xFFu));

    uint16_t total_args = 0;
    for (uint16_t c = n->left; c != MEM_NO_IDX; c = mem_get_ast(mem, c)->right) {
        total_args++;
        if (total_args > MEM_MAX_F64_POOL) {
            mem_err(mem, "Container constructor argument count exceeds pool capacity");
            *out_val = val_void();
            return true;
        }
    }

    /* Special `vec<T>()` / `vec(cap)` / `vec(count, init)` variations when total_args <= 2 and first arg is unitless real */
    if (is_vec_call && total_args <= 2) {
        if (total_args == 0) {
            uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, 0);
            *out_val = mem->has_error ? val_void()
                                      : val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
            return true;
        }
        const AstNode *l0 = mem_get_ast(mem, n->left);
        Value a0 = eval_ast(mem, tab, l0->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            *out_val = val_void();
            return true;
        }
        if (total_args == 1 && val_get_type(mem, a0) == mem->fast_real_type &&
            val_get_unit(mem, a0) == MEM_UNIT_NONE) {
            int cap_in = 0;
            if (!delegate_val_to_exact_int(mem, tab, a0, "vector capacity", &cap_in)) {
                *out_val = val_void();
                return true;
            }
            if (cap_in < 0 || (uint32_t)cap_in > MEM_MAX_HEAP_SLOTS) {
                mem_err(mem, "Vector capacity out of range (%d)", cap_in);
                *out_val = val_void();
                return true;
            }
            uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, (uint32_t)cap_in);
            *out_val = mem->has_error ? val_void()
                                      : val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
            return true;
        }
        Value a1 = val_void();
        if (total_args == 2) {
            const AstNode *l1 = mem_get_ast(mem, l0->right);
            a1 = eval_ast(mem, tab, l1->left, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) {
                *out_val = val_void();
                return true;
            }
            if (val_get_type(mem, a0) == mem->fast_real_type &&
                val_get_unit(mem, a0) == MEM_UNIT_NONE) {
                int count = 0;
                if (!delegate_val_to_exact_int(mem, tab, a0, "vector fill count", &count)) {
                    *out_val = val_void();
                    return true;
                }
                if (count < 0 || (uint32_t)count > MEM_MAX_HEAP_SLOTS) {
                    mem_err(mem, "Vector fill count out of range (%d)", count);
                    *out_val = val_void();
                    return true;
                }
                uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, (uint32_t)count);
                if (mem->has_error) {
                    *out_val = val_void();
                    return true;
                }
                Value vhandle = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
                for (int i = 0; i < count && !mem->has_error; ++i) {
                    (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, a1, val_void());
                }
                *out_val = mem->has_error ? val_void() : vhandle;
                return true;
            }
        }
        uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, total_args);
        if (mem->has_error) {
            *out_val = val_void();
            return true;
        }
        Value vhandle = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
        (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, a0, val_void());
        if (total_args == 2 && !mem->has_error) {
            (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, a1, val_void());
        }
        *out_val = mem->has_error ? val_void() : vhandle;
        return true;
    }

    uint16_t hdr_idx = mem_vec_alloc_hdr(mem, hdr0, total_args);
    if (mem->has_error) {
        *out_val = val_void();
        return true;
    }
    Value vhandle = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    uint16_t root_mark = mem_push_temp_root(mem, &vhandle);
    uint16_t cur = n->left;
    while (cur != MEM_NO_IDX && !mem->has_error) {
        const AstNode *link = mem_get_ast(mem, cur);
        Value item = eval_ast(mem, tab, link->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            mem_pop_temp_roots(mem, root_mark);
            *out_val = val_void();
            return true;
        }
        (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, vhandle, item, val_void());
        cur = link->right;
    }
    mem_pop_temp_roots(mem, root_mark);
    *out_val = mem->has_error ? val_void() : vhandle;
    return true;
}

/*code section below manages hardcoding O(1) range, nested exclusion range, & stepped-range AST rewriting, evaluation, and membership without allocating a range vector*/
/*Construct a...b, a..<b, a>..b, a>..<b, a..<x...y>..b, a..<x...y>..<z...w>..<b, x in a...b, x in (a...b by step), a...b by step*/
static bool is_direct_vrange_binary(const MemArena *mem, const SymTable *tab,
                                    uint16_t ast_idx, int *out_mode) {
    if (out_mode) *out_mode = 0;
    if (ast_idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, ast_idx);
    if (n->kind != AST_BINARY) return false;
    uint16_t sid = n->ref;
    if (sid >= tab->sym_count) return false;
    for (uint16_t i = tab->sym_first_ov[sid]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
        const OpOverload *ov = &sym_overloads(tab)[i];
        if (ov->fixity == n->sub && sym_mod_enabled(tab, ov->mod_id) &&
            ov->body_ast != MEM_NO_IDX) {
            const AstNode *b = mem_get_ast(mem, ov->body_ast);
            if (b->kind == AST_INTRINSIC && b->sub == INTR_VRANGE) {
                if (out_mode && b->ref != 0 && b->ref != MEM_NO_IDX) {
                    const AstNode *mn = mem_get_ast(mem, b->ref);
                    if (mn->kind == AST_LIT) {
                        *out_mode = (int)mem_read_pool(mem, mn->ref);
                    }
                }
                return true;
            }
        }
    }
    return false;
}

static bool is_vrange_cat_ast(const MemArena *mem, uint16_t ast_idx) {
    if (ast_idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, ast_idx);
    return (n->kind == AST_INTRINSIC && n->sub == INTR_VRANGE_CAT && n->ref == 0);
}

typedef struct {
    uint16_t lo_ast;
    uint16_t hi_ast;
    int      mode;
} VRangeSegAst;

static bool collect_vrange_segs(const MemArena *mem, const SymTable *tab,
                                uint16_t ast_idx, VRangeSegAst *segs,
                                uint8_t *cnt, uint8_t max_segs) {
    if (ast_idx == MEM_NO_IDX) return false;
    int mode = 0;
    if (is_direct_vrange_binary(mem, tab, ast_idx, &mode)) {
        if (*cnt >= max_segs) return false;
        const AstNode *n = mem_get_ast(mem, ast_idx);
        segs[*cnt].lo_ast = n->left;
        segs[*cnt].hi_ast = n->right;
        segs[*cnt].mode   = mode;
        (*cnt)++;
        return true;
    }
    if (is_vrange_cat_ast(mem, ast_idx)) {
        const AstNode *n = mem_get_ast(mem, ast_idx);
        return collect_vrange_segs(mem, tab, n->left, segs, cnt, max_segs) &&
               collect_vrange_segs(mem, tab, n->right, segs, cnt, max_segs);
    }
    return false;
}

static bool extract_vrange_expr_info(const MemArena *mem, const SymTable *tab,
                                     uint16_t ast_idx, VRangeSegAst *segs,
                                     uint8_t *out_seg_cnt, uint8_t max_segs,
                                     uint16_t *out_step_ast) {
    if (out_seg_cnt)  *out_seg_cnt  = 0;
    if (out_step_ast) *out_step_ast = MEM_NO_IDX;
    if (ast_idx == MEM_NO_IDX) return false;

    uint8_t cnt = 0;
    if (collect_vrange_segs(mem, tab, ast_idx, segs, &cnt, max_segs) && cnt > 0) {
        if (out_seg_cnt) *out_seg_cnt = cnt;
        return true;
    }

    const AstNode *n = mem_get_ast(mem, ast_idx);
    if (n->kind == AST_BINARY && n->ref < tab->sym_count) {
        bool is_by_op = false;
        for (uint16_t i = tab->sym_first_ov[n->ref]; i != MEM_NO_IDX; i = sym_ov_next(tab)[i]) {
            const OpOverload *ov = &sym_overloads(tab)[i];
            if (ov->fixity == n->sub && sym_mod_enabled(tab, ov->mod_id) &&
                ov->body_ast != MEM_NO_IDX) {
                const AstNode *b = mem_get_ast(mem, ov->body_ast);
                if (b->kind == AST_INTRINSIC && b->sub == INTR_VRANGE_STEP) {
                    is_by_op = true;
                    break;
                }
            }
        }
        if (is_by_op) {
            cnt = 0;
            if (collect_vrange_segs(mem, tab, n->left, segs, &cnt, max_segs) && cnt > 0) {
                if (out_seg_cnt)  *out_seg_cnt  = cnt;
                if (out_step_ast) *out_step_ast = n->right;
                return true;
            }
        }
    }
    return false;
}

static bool is_pending_skip_node(const MemArena *mem, uint16_t ast_idx) {
    if (ast_idx == MEM_NO_IDX) return false;
    const AstNode *n = mem_get_ast(mem, ast_idx);
    return (n->kind == AST_INTRINSIC && n->sub == INTR_VRANGE_CAT && n->ref == 0xFFFFu);
}

uint16_t hardcode_rewrite_range_chain(Parser *P, OpFixity fix, uint16_t sym_id,
                                      uint16_t left, uint16_t right) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    uint16_t raw_node = mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, sym_id, left, right);
    if (raw_node == MEM_NO_IDX || mem->has_error) return raw_node;

    int cur_mode = 0;
    if (!is_direct_vrange_binary(mem, tab, raw_node, &cur_mode)) {
        return raw_node;
    }

    int left_mode = 0;
    bool left_is_vrange  = is_direct_vrange_binary(mem, tab, left, &left_mode);
    bool left_is_pending = is_pending_skip_node(mem, left);
    bool left_is_cat     = !left_is_pending && is_vrange_cat_ast(mem, left);

    if (!left_is_vrange && !left_is_pending && !left_is_cat) {
        return raw_node;
    }

    /* Case 1: `left` is an open skip-start segment (`a..<s1` or `a>..<s1`) */
    if (left_is_vrange && (left_mode & 1) != 0) {
        if ((cur_mode & 2) == 0) {
            /* `(a..<s1) ... e1` -> pending skip node (ref == 0xFFFFu) */
            return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0xFFFFu,
                                 left, right);
        }
        /* Otherwise `(a..<b) >.. c` is a chained multi-segment range without skipping `b`:
           segment 1 is `a..<b`, segment 2 is `b>..c` (or `b>..<c`). */
        const AstNode *ln = mem_get_ast(mem, left);
        uint16_t b_ast    = ln->right;
        uint16_t seg2_ast = mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, sym_id, b_ast, right);
        return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0, left, seg2_ast);
    }

    /* Case 2: `left` is a pending skip node `(prefix_seg, e1)` closed by `>..` or `>..<` */
    if (left_is_pending) {
        const AstNode *ln = mem_get_ast(mem, left);
        uint16_t prefix_seg = ln->left;
        uint16_t e1_ast     = ln->right;
        if ((cur_mode & 2) == 0) {
            mem_err(mem, "Expected '>..' or '>..<' to close nested exclusion range");
            return MEM_NO_IDX;
        }
        uint16_t next_seg = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                          sym_id, e1_ast, right);
        return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                             prefix_seg, next_seg);
    }

    /* Case 3: `left` is a completed `INTR_VRANGE_CAT` (`cat(prev, seg_k)`) */
    if (left_is_cat) {
        const AstNode *cat_n = mem_get_ast(mem, left);
        if (is_pending_skip_node(mem, cat_n->right)) {
            const AstNode *rn = mem_get_ast(mem, cat_n->right);
            uint16_t prev_cat = mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                              cat_n->left, rn->left);
            uint16_t ek_ast   = rn->right;
            if ((cur_mode & 2) == 0) {
                mem_err(mem, "Expected '>..' or '>..<' to close nested exclusion range");
                return MEM_NO_IDX;
            }
            uint16_t next_seg = mem_alloc_ast(mem, AST_BINARY, (uint8_t)FIX_INFIX_LEFT,
                                              sym_id, ek_ast, right);
            return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                 prev_cat, next_seg);
        }
        int r_mode = 0;
        if (is_direct_vrange_binary(mem, tab, cat_n->right, &r_mode)) {
            if ((r_mode & 1) != 0 && (cur_mode & 2) == 0) {
                /* `cat(prev, e1 >..< s2) ... e2` -> pending skip node on right child */
                uint16_t pend = mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0xFFFFu,
                                              cat_n->right, right);
                return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                     cat_n->left, pend);
            }
            if ((r_mode & 1) != 0 && (cur_mode & 2) != 0) {
                /* `cat(prev, a..<b) >.. c` */
                const AstNode *rn = mem_get_ast(mem, cat_n->right);
                uint16_t b_ast    = rn->right;
                uint16_t seg2_ast = mem_alloc_ast(mem, AST_BINARY, (uint8_t)fix, sym_id, b_ast, right);
                return mem_alloc_ast(mem, AST_INTRINSIC, (uint8_t)INTR_VRANGE_CAT, 0,
                                     left, seg2_ast);
            }
        }
    }

    return raw_node;
}

/*code section below manages hardcoding lazy variable parameter positions for symbolic constructs and pipe operator AST rewriting*/
/*Construct sym_diff(x, ...), sym_integ(x, ...), sym_solve(x, ...), and x |> f / x |> f(a, b) / x |> (a => ...)*/
static bool hc_peek_is_lparen(const Lexer *lex) {
    uint16_t p = lex->pos;
    while (p < lex->len && (lex->src[p] == ' ' || lex->src[p] == '\t' || lex->src[p] == '\r')) p++;
    return (p < lex->len && lex->src[p] == '(');
}

static bool hc_peek_is_arrow(const Lexer *lex) {
    uint16_t p = lex->pos;
    while (p < lex->len && (lex->src[p] == ' ' || lex->src[p] == '\t' || lex->src[p] == '\r')) p++;
    return (p + 1 < lex->len && lex->src[p] == '=' && lex->src[p + 1] == '>');
}

static int hc_find_local_idx(const Parser *P, const char *name, uint16_t len) {
    for (int i = (int)P->vis_count - 1; i >= 0; --i) {
        StrSlice s = P->locals[i].name;
        if (s.len == len && memcmp(mem_str_ptr(P->lex.mem, s), name, len) == 0) {
            return i;
        }
    }
    return -1;
}

uint16_t hardcode_parse_pipe_rhs(Parser *P, uint16_t left, int next_min) {
    MemArena *mem = P->lex.mem;
    SymTable *tab = P->lex.tab;
    if ((P->lex.cur.kind == TOK_IDENT ||
         (P->lex.cur.kind == TOK_OP && lex_is_ident_start((unsigned char)P->lex.src[P->lex.cur.off]))) &&
        !hc_peek_is_lparen(&P->lex) && !hc_peek_is_arrow(&P->lex)) {
        const char *fname = P->lex.src + P->lex.cur.off;
        uint16_t    flen  = P->lex.cur.len;
        if (hc_find_local_idx(P, fname, flen) < 0) {
            int fn_sid = sym_find(tab, mem, fname, flen);
            if (fn_sid >= 0) {
                lex_next(&P->lex);
                uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, MEM_NO_IDX);
                return mem_alloc_ast(mem, AST_CALL, 1, (uint16_t)fn_sid, link, MEM_NO_IDX);
            }
        }
    }
    uint16_t rhs_pipe = parser_parse_expr(P, next_min);
    if (mem->has_error || rhs_pipe == MEM_NO_IDX) return MEM_NO_IDX;
    const AstNode *rn = mem_get_ast(mem, rhs_pipe);
    if (rn->kind == AST_CALL) {
        uint16_t head_link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, rn->left);
        AstNode *mut_call  = mem_get_ast_mut(mem, rhs_pipe);
        mut_call->left     = head_link;
        mut_call->sub++;
        return rhs_pipe;
    }
    if (rn->kind == AST_FN_CALL) {
        uint16_t head_link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, rn->right);
        AstNode *mut_fcall = mem_get_ast_mut(mem, rhs_pipe);
        mut_fcall->right   = head_link;
        mut_fcall->sub++;
        return rhs_pipe;
    }
    uint16_t link = mem_alloc_ast(mem, AST_ARG_LINK, 0, 0, left, MEM_NO_IDX);
    return mem_alloc_ast(mem, AST_FN_CALL, 1, 0, rhs_pipe, link);
}

bool hardcode_eval_in_vrange_fast(MemArena *mem, SymTable *tab,
                                  const AstNode *n, uint16_t frame_base,
                                  uint8_t cur_ret_type, Value *out_val) {
    if (!hardcode_is_in_op_sym(mem, tab, n->ref)) return false;
    VRangeSegAst segs[8];
    uint8_t      seg_cnt  = 0;
    uint16_t     step_ast = MEM_NO_IDX;
    if (!extract_vrange_expr_info(mem, tab, n->right, segs, &seg_cnt, 8, &step_ast)) {
        return false;
    }
    Value item = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) {
        *out_val = val_void();
        return true;
    }
    Value vs = (step_ast != MEM_NO_IDX) ? eval_ast(mem, tab, step_ast, frame_base, cur_ret_type) : val_void();
    if (mem->has_error || mem->jump_active != JUMP_NONE) {
        *out_val = val_void();
        return true;
    }

    for (uint8_t s_i = 0; s_i < seg_cnt; ++s_i) {
        int vrange_mode = segs[s_i].mode;
        Value va = eval_ast(mem, tab, segs[s_i].lo_ast, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            *out_val = val_void();
            return true;
        }
        Value vb = eval_ast(mem, tab, segs[s_i].hi_ast, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) {
            *out_val = val_void();
            return true;
        }
        uint8_t elem_uid = MEM_UNIT_NONE;
        if (!delegate_check_additive_unit_compat(mem, tab, "...", 3, va, vb, &elem_uid)) {
            *out_val = val_void();
            return true;
        }
        if (step_ast == MEM_NO_IDX) {
            int lo = 0, hi = 0;
            if (!delegate_val_to_exact_int(mem, tab, va, "range start", &lo) ||
                !delegate_val_to_exact_int(mem, tab, vb, "range end",   &hi)) {
                *out_val = val_void();
                return true;
            }
            if (val_get_type(mem, item) != mem->fast_real_type || val_get_unit(mem, item) != elem_uid) {
                continue;
            }
            double ditem = val_get_f64(mem, item);
            double ritem = round(ditem);
            if (fabs(ditem - ritem) > 1e-9 || ritem < -2147483647.0 || ritem > 2147483647.0) {
                continue;
            }
            long long k    = (long long)ritem;
            long long step = (lo <= hi) ? 1LL : -1LL;
            long long llo  = (long long)lo + ((vrange_mode & 2) ? step : 0LL);
            long long lhi  = (long long)hi - ((vrange_mode & 1) ? step : 0LL);
            bool hit = (step > 0) ? (k >= llo && k <= lhi) : (k <= llo && k >= lhi);
            if (hit) {
                *out_val = val_make_f64(mem, mem->fast_real_type, 1.0);
                return true;
            }
        } else {
            double da    = val_get_f64(mem, va);
            double db    = val_get_f64(mem, vb);
            double dstep = val_get_f64(mem, vs);
            if (!isfinite(dstep) || fabs(dstep) <= 1e-15) {
                mem_err(mem, "Range step cannot be zero");
                *out_val = val_void();
                return true;
            }
            if (val_get_type(mem, item) != mem->fast_real_type || val_get_unit(mem, item) != elem_uid) {
                continue;
            }
            double ditem = val_get_f64(mem, item);
            double eps   = 1e-11 * (1.0 + fabs(da) + fabs(db) + fabs(dstep));
            double start_v = (vrange_mode & 2) ? (da + dstep) : da;
            bool in_bounds = false;
            if (dstep > 0.0) {
                bool lo_ok = (ditem >= start_v - eps);
                bool hi_ok = (vrange_mode & 1) ? (ditem < db - eps) : (ditem <= db + eps);
                in_bounds  = lo_ok && hi_ok;
            } else {
                bool lo_ok = (ditem <= start_v + eps);
                bool hi_ok = (vrange_mode & 1) ? (ditem > db + eps) : (ditem >= db - eps);
                in_bounds  = lo_ok && hi_ok;
            }
            if (in_bounds) {
                double q  = (ditem - start_v) / dstep;
                double rq = floor(q + 0.5);
                if (rq >= 0.0 && fabs(q - rq) <= 1e-9) {
                    *out_val = val_make_f64(mem, mem->fast_real_type, 1.0);
                    return true;
                }
            }
        }
    }
    *out_val = val_make_f64(mem, mem->fast_real_type, 0.0);
    return true;
}

/*code section below manages hardcoding range, nested exclusion range, stepped-range, and collection iteration loops (with optional index variable) and vector comprehensions*/
/*Construct (i in a...b) { ... }, (i in a..<x...y>..b) { ... }, (i in a...b by s) { ... }, (idx, x in vec) { ... }, [expr for idx, x in iter]*/
Value hardcode_eval_for_in_ast(MemArena *mem, SymTable *tab,
                               const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type) {
    uint8_t  var_slot   = n->sub;
    uint16_t iter_ast   = n->left;
    bool     is_comp    = (n->ref & 0x8000u) != 0;
    bool     has_idx    = (n->ref & 0x4000u) != 0;
    uint8_t  idx_slot   = (uint8_t)((n->ref >> 8u) & 0x3Fu);
    uint16_t raw_lbl    = (uint16_t)(n->ref & 0x00FFu);
    uint8_t  loop_label = (raw_lbl > 0) ? (uint8_t)(raw_lbl - 1) : 0xFFu;

    uint16_t body_ast = n->right;
    uint16_t cond_ast = MEM_NO_IDX;
    Value    comp_vec = val_void();

    if (is_comp) {
        const AstNode *pair = mem_get_ast(mem, n->right);
        body_ast = pair->left;
        cond_ast = pair->right;
        uint16_t hdr_idx = mem_vec_alloc_hdr(mem, 0, 8);
        if (mem->has_error) return val_void();
        comp_vec = val_make_comp(mem, mem->vec_type_id, hdr_idx, MEM_VEC_HDR_SLOTS);
    }

    Value    last  = is_comp ? comp_vec : val_make_f64(mem, mem->fast_real_type, 0.0);
    uint32_t guard = 0;

    /* Fast path: `(i in a...b [by step]) { ... }` or multi-segment `(i in 1..<25...75>..100) { ... }` without allocating range vector */
    VRangeSegAst segs[8];
    uint8_t      seg_cnt  = 0;
    uint16_t     step_ast = MEM_NO_IDX;
    if (extract_vrange_expr_info(mem, tab, iter_ast, segs, &seg_cnt, 8, &step_ast)) {
        Value vs = (step_ast != MEM_NO_IDX) ? eval_ast(mem, tab, step_ast, frame_base, cur_ret_type) : val_void();
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

        long long global_idx = 0;
        bool broke_loop = false;

        for (uint8_t s_i = 0; s_i < seg_cnt && !mem->has_error && !broke_loop; ++s_i) {
            int vrange_mode = segs[s_i].mode;
            Value va = eval_ast(mem, tab, segs[s_i].lo_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value vb = eval_ast(mem, tab, segs[s_i].hi_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

            uint8_t u = MEM_UNIT_NONE;
            if (!delegate_check_additive_unit_compat(mem, tab, "...", 3, va, vb, &u)) {
                return val_void();
            }

            double da   = val_get_f64(mem, va);
            double db   = val_get_f64(mem, vb);
            double step = (da <= db) ? 1.0 : -1.0;

            if (step_ast == MEM_NO_IDX) {
                int lo = 0, hi = 0;
                if (!delegate_val_to_exact_int(mem, tab, va, "range start", &lo) ||
                    !delegate_val_to_exact_int(mem, tab, vb, "range end",   &hi)) {
                    return val_void();
                }
                da   = (double)lo;
                db   = (double)hi;
                step = (lo <= hi) ? 1.0 : -1.0;
            } else {
                uint8_t us = val_get_unit(mem, vs);
                if (us != MEM_UNIT_NONE && us != u) {
                    mem_err(mem, "Unit mismatch in stepped range 'by'");
                    return val_void();
                }
                step = val_get_f64(mem, vs);
                if (!isfinite(step) || fabs(step) <= 1e-15) {
                    mem_err(mem, "Range step cannot be zero");
                    return val_void();
                }
            }

            double    eps       = 1e-11 * (1.0 + fabs(da) + fabs(db) + fabs(step));
            long long start_k   = (vrange_mode & 2) ? 1LL : 0LL;
            double    first_val = da + (double)start_k * step;
            long long cnt_ll    = 0;

            if (step > 0.0) {
                bool ok = (vrange_mode & 1) ? (first_val < db - eps) : (first_val <= db + eps);
                if (ok) cnt_ll = (long long)floor(((db - first_val) + ((vrange_mode & 1) ? -eps : eps)) / step) + 1LL;
            } else {
                bool ok = (vrange_mode & 1) ? (first_val > db + eps) : (first_val >= db - eps);
                if (ok) cnt_ll = (long long)floor(((first_val - db) + ((vrange_mode & 1) ? -eps : eps)) / (-step)) + 1LL;
            }

            for (long long iter_i = 0; iter_i < cnt_ll && !mem->has_error; ++iter_i) {
                double cur_v = da + (double)(start_k + iter_i) * step;
                if (fabs(cur_v) < 1e-15) cur_v = 0.0;
                if (has_idx) {
                    mem_set_slot(mem, frame_base, idx_slot,
                                 val_make_f64(mem, mem->fast_real_type, (double)global_idx));
                }
                global_idx++;
                mem_set_slot(mem, frame_base, var_slot,
                             val_make_f64_unit(mem, mem->fast_real_type, u, cur_v));
                if (cond_ast != MEM_NO_IDX) {
                    Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
                }
                Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
                if (is_comp) {
                    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                    (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
                } else {
                    last = item_res;
                    if (mem->jump_active == JUMP_CONTINUE_ANON ||
                        (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                        mem->jump_active = JUMP_NONE;
                        continue;
                    }
                    if (mem->jump_active == JUMP_BREAK_ANON ||
                        (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                        mem->jump_active = JUMP_NONE;
                        if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                            last = mem->jump_val;
                            mem->jump_val = val_void();
                        }
                        broke_loop = true;
                        break;
                    }
                    if (mem->jump_active != JUMP_NONE) {
                        broke_loop = true;
                        break;
                    }
                }
                if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                    mem->heap_active_count >= 64u) {
                    Value roots[2] = { last, comp_vec };
                    (void)mem_compact_scratch(mem, roots, 2);
                    last     = roots[0];
                    comp_vec = roots[1];
                }
                if (++guard > 1000000u) {
                    mem_err(mem, "Loop iteration limit exceeded");
                    broke_loop = true;
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    /* Collection path: `(v in vec) { ... }`, `(x in mat) { ... }`, `(c in str) { ... }`, or `[expr for v in coll]` */
    Value coll = eval_ast(mem, tab, iter_ast, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    uint16_t coll_tid = val_get_type(mem, coll);

    if (coll_tid == mem->vec_type_id && mem->vec_type_id != 0) {
        for (uint32_t idx = 0; !mem->has_error; ++idx) {
            uint16_t hdr_idx  = val_get_pool_idx(coll);
            uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
            uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
            uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
            uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                                ? tab->types[elem_tid].flat_size : 1;
            if (idx >= len) break;

            Value item = delegate_vec_read_elem(mem, tab, elem_tid, elem_uid,
                                                data_idx, idx * (uint32_t)stride);
            if (elem_tid != 0 && elem_tid < tab->type_count &&
                !tab->types[elem_tid].is_consuming &&
                elem_tid != mem->vec_type_id && elem_tid != mem->mat_type_id &&
                elem_tid != mem->fn_type_id && elem_tid != mem->map_type_id) {
                item = val_clone_comp(mem, item, stride);
            }
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot,
                             val_make_f64(mem, mem->fast_real_type, (double)idx));
            }
            mem_set_slot(mem, frame_base, var_slot, item);
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
            if (mem->pool_top > (uint16_t)(MEM_MAX_F64_POOL - 1024u) ||
                mem->heap_active_count >= 64u) {
                Value roots[3] = { coll, last, comp_vec };
                (void)mem_compact_scratch(mem, roots, 3);
                coll     = roots[0];
                last     = roots[1];
                comp_vec = roots[2];
            }
            if (++guard > 1000000u) {
                mem_err(mem, "Loop iteration limit exceeded");
                break;
            }
        }
        return is_comp ? comp_vec : last;
    }

    if (coll_tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, coll, &rank, &tot_len, &data_idx, dims);
        uint8_t u = val_get_unit(mem, coll);
        for (uint16_t idx = 0; idx < tot_len && !mem->has_error; ++idx) {
            Value item = val_make_f64_unit(mem, mem->fast_real_type, u,
                                           mem_data_read(mem, data_idx, idx));
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot,
                             val_make_f64(mem, mem->fast_real_type, (double)idx));
            }
            mem_set_slot(mem, frame_base, var_slot, item);
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    if (coll_tid == mem->str_type_id && mem->str_type_id != 0) {
        StrSlice s = val_get_str(mem, coll);
        for (uint16_t idx = 0; idx < s.len && !mem->has_error; ++idx) {
            char ch = mem_str_ptr(mem, s)[idx];
            Value item = val_make_str(mem, mem->str_type_id, mem_intern_str(mem, &ch, 1));
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot,
                             val_make_f64(mem, mem->fast_real_type, (double)idx));
            }
            mem_set_slot(mem, frame_base, var_slot, item);
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    if (coll_tid == mem->map_type_id && mem->map_type_id != 0) {
        uint16_t hdr_idx = val_get_pool_idx(coll);
        for (uint16_t idx = 0; !mem->has_error; ++idx) {
            uint16_t len      = (uint16_t)mem_read_pool(mem, hdr_idx + 1);
            uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
            if (idx >= len) break;

            StrSlice kslice = mem_map_entry_key(mem, data_idx, idx);
            Value    kval   = val_make_str(mem, mem->str_type_id, kslice);
            Value    vval   = mem_map_entry_val(mem, data_idx, idx);
            if (has_idx) {
                mem_set_slot(mem, frame_base, idx_slot, kval);
                mem_set_slot(mem, frame_base, var_slot, vval);
            } else {
                mem_set_slot(mem, frame_base, var_slot, kval);
            }
            if (cond_ast != MEM_NO_IDX) {
                Value cv = eval_ast(mem, tab, cond_ast, frame_base, cur_ret_type);
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                if (fabs(val_get_f64(mem, cv)) <= 1e-15) continue;
            }
            Value item_res = eval_ast(mem, tab, body_ast, frame_base, cur_ret_type);
            if (is_comp) {
                if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
                (void)delegate_eval_vec_intrinsic(mem, tab, INTR_VPUSH, comp_vec, item_res, val_void());
            } else {
                last = item_res;
                if (mem->jump_active == JUMP_CONTINUE_ANON ||
                    (mem->jump_active == JUMP_CONTINUE_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    continue;
                }
                if (mem->jump_active == JUMP_BREAK_ANON ||
                    (mem->jump_active == JUMP_BREAK_TAG && mem->jump_target_id == loop_label)) {
                    mem->jump_active = JUMP_NONE;
                    if (val_get_type(mem, mem->jump_val) != MEM_TYPE_VOID) {
                        last = mem->jump_val;
                        mem->jump_val = val_void();
                    }
                    break;
                }
                if (mem->jump_active != JUMP_NONE) {
                    break;
                }
            }
        }
        return is_comp ? comp_vec : last;
    }

    mem_err(mem, "Loop target after 'in' must be a range (a...b / a..<b / a>..b / a>..<b), vector ($vec), matrix ($mat), map ($map), or string ($str)");
    return val_void();
}

/*code section below manages hardcoding multi-variable scalar broadcast and struct/vector/matrix destructuring*/
/*Construct x, y := 5 ; r1, r2 := quad_roots(1, -5, 6) ; a, b, c := vec(1, 2, 3)*/
Value hardcode_eval_unpack_elem_ast(MemArena *mem, const SymTable *tab,
                                    const AstNode *n, uint16_t frame_base) {
    uint8_t elem_idx = n->sub;
    uint8_t tot_vars = (uint8_t)n->ref;
    uint8_t tmp_slot = (uint8_t)n->left;
    Value   src      = mem_get_slot(mem, frame_base, tmp_slot);
    uint16_t tid     = val_get_type(mem, src);
    uint8_t  uid     = val_get_unit(mem, src);

    if (tid == 0 || tid >= tab->type_count) return val_void();

    /* 1. Consuming scalar (`real`, `iota`, `str`): broadcast to all variables (`x, y := 5`) */
    if (tab->types[tid].is_consuming) {
        return src;
    }

    /* 2. Dynamic vector (`$vec`): destructure element `elem_idx` (`a, b, c := vec(10, 20, 30)`) */
    if (tid == mem->vec_type_id && mem->vec_type_id != 0) {
        uint16_t hdr_idx  = val_get_pool_idx(src);
        uint16_t raw_hdr0 = (uint16_t)mem_read_pool(mem, hdr_idx + 0);
        uint16_t elem_tid = (uint16_t)(raw_hdr0 & 0xFFu);
        uint8_t  elem_uid = (uint8_t)((raw_hdr0 >> 8) & 0x7Fu);
        uint32_t len      = (uint32_t)mem_read_pool(mem, hdr_idx + 1);
        uint16_t data_idx = (uint16_t)mem_read_pool(mem, hdr_idx + 3);
        uint16_t stride   = (elem_tid > 0 && elem_tid < tab->type_count)
                            ? tab->types[elem_tid].flat_size : 1;
        if (elem_idx >= len) {
            mem_err(mem, "Cannot destructure %u variables from vector of length %u",
                    (unsigned)tot_vars, (unsigned)len);
            return val_void();
        }
        return delegate_vec_read_elem(mem, tab, elem_tid, elem_uid,
                                      data_idx, (uint32_t)elem_idx * (uint32_t)stride);
    }

    /* 3. Matrix (`$mat`): destructure element `elem_idx` (`a, b, c := [10, 20, 30]`) */
    if (tid == mem->mat_type_id && mem->mat_type_id != 0) {
        uint8_t  rank = 0;
        uint16_t tot_len = 0, data_idx = 0, dims[4] = { 0 };
        delegate_mat_get_info(mem, src, &rank, &tot_len, &data_idx, dims);
        if (elem_idx >= tot_len) {
            mem_err(mem, "Cannot destructure %u variables from matrix of length %u",
                    (unsigned)tot_vars, (unsigned)tot_len);
            return val_void();
        }
        return val_make_f64_unit(mem, mem->fast_real_type, uid,
                                 mem_data_read(mem, data_idx, elem_idx));
    }

    /* 4. Flat C-style struct (`$vec2`, `$vec3`, `$complex`, `$circle`): destructure field `elem_idx` */
    const TypeDef *td = &tab->types[tid];
    if (elem_idx >= td->field_count) {
        char tbuf[32];
        sym_type_name(tab, mem, tid, tbuf, sizeof(tbuf));
        mem_err(mem, "Cannot destructure %u variables from '%s' (has %u fields)",
                (unsigned)tot_vars, tbuf, (unsigned)td->field_count);
        return val_void();
    }
    const FieldDef *fd = &tab->fields[td->start_idx + elem_idx];
    uint16_t f_pidx    = (uint16_t)(val_get_pool_idx(src) + fd->flat_offset);
    uint16_t f_tid     = sym_unpack_outer_type(mem, fd->type_id);
    if (!tab->types[f_tid].is_consuming) {
        return val_with_unit(mem,
                             val_make_comp(mem, f_tid, f_pidx, tab->types[f_tid].flat_size),
                             uid);
    }
    if (f_tid == mem->str_type_id && mem->str_type_id != 0) {
        uint32_t packed = (uint32_t)mem_read_pool(mem, f_pidx);
        StrSlice s = { (uint16_t)(packed >> 16u), (uint16_t)(packed & 0xFFFFu) };
        return val_make_str(mem, f_tid, s);
    }
    return val_make_f64_unit(mem, f_tid ? f_tid : mem->fast_real_type, uid,
                             mem_read_pool(mem, f_pidx));
}

/*code section below manages hardcoding pattern-matched 'switch' case evaluation (exact values, ranges, struct/vector destructuring)*/
/*Construct switch (expr) { case pat if guard : { ... } else : { ... } }*/
Value hardcode_eval_switch_eq_ast(MemArena *mem, SymTable *tab,
                                  const AstNode *n, uint16_t frame_base, uint8_t cur_ret_type) {
    /* sub == 2: Pattern variable binding (`slot` in `n->ref`, value expr in `n->left`), returns 1.0 */
    if (n->sub == 2) {
        Value fval = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
        if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
        mem_set_slot(mem, frame_base, (uint8_t)n->ref, fval);
        return val_make_f64(mem, mem->fast_real_type, 1.0);
    }

    Value a = eval_ast(mem, tab, n->left, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    uint16_t ta = val_get_type(mem, a);
    uint8_t  ua = val_get_unit(mem, a);

    /* sub == 1: Struct type check (`ta == n->ref`) */
    if (n->sub == 1) {
        return val_make_f64(mem, mem->fast_real_type, (ta == n->ref) ? 1.0 : 0.0);
    }


    /* sub == 3: Vector / 1D matrix length check (`len == n->ref`) */
    if (n->sub == 3) {
        bool ok = false;
        if (ta == mem->vec_type_id && mem->vec_type_id != 0) {
            uint16_t hdr_idx = val_get_pool_idx(a);
            uint16_t len     = (uint16_t)mem_read_pool(mem, hdr_idx + 1);
            ok = (len == n->ref);
        } else if (ta == mem->mat_type_id && mem->mat_type_id != 0) {
            uint8_t  rank = 0;
            uint16_t tot_len = 0, dims[4] = { 0 };
            if (delegate_mat_get_info(mem, a, &rank, &tot_len, NULL, dims) && rank == 1) {
                ok = (tot_len == n->ref);
            }
        }
        return val_make_f64(mem, mem->fast_real_type, ok ? 1.0 : 0.0);
    }

    /* sub == 0: Fast path for range & nested exclusion range patterns `case a...b:` / `case 1..<25...75>..100:` */
    VRangeSegAst sw_segs[8];
    uint8_t      sw_seg_cnt = 0;
    if (collect_vrange_segs(mem, tab, n->right, sw_segs, &sw_seg_cnt, 8) && sw_seg_cnt > 0) {
        if (ta == 0 || ta >= tab->type_count || !tab->types[ta].is_consuming || ta == mem->str_type_id) {
            return val_make_f64(mem, mem->fast_real_type, 0.0);
        }
        double da = val_get_f64(mem, a);
        for (uint8_t s_i = 0; s_i < sw_seg_cnt; ++s_i) {
            int sw_vrange_mode = sw_segs[s_i].mode;
            Value vlo = eval_ast(mem, tab, sw_segs[s_i].lo_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
            Value vhi = eval_ast(mem, tab, sw_segs[s_i].hi_ast, frame_base, cur_ret_type);
            if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();

            uint8_t ulo = val_get_unit(mem, vlo);
            uint8_t uhi = val_get_unit(mem, vhi);
            if ((ulo != MEM_UNIT_NONE && ua != ulo) || (uhi != MEM_UNIT_NONE && ua != uhi)) {
                continue;
            }
            double dlo = val_get_f64(mem, vlo);
            double dhi = val_get_f64(mem, vhi);
            bool in_rng = false;
            if (dlo <= dhi) {
                bool lo_ok = (sw_vrange_mode & 2) ? (da > dlo + 1e-12) : (da >= dlo - 1e-12);
                bool hi_ok = (sw_vrange_mode & 1) ? (da < dhi - 1e-12) : (da <= dhi + 1e-12);
                in_rng = lo_ok && hi_ok;
            } else {
                bool lo_ok = (sw_vrange_mode & 2) ? (da < dlo - 1e-12) : (da <= dlo + 1e-12);
                bool hi_ok = (sw_vrange_mode & 1) ? (da > dhi + 1e-12) : (da >= dhi - 1e-12);
                in_rng = lo_ok && hi_ok;
            }
            if (in_rng) {
                return val_make_f64(mem, mem->fast_real_type, 1.0);
            }
        }
        return val_make_f64(mem, mem->fast_real_type, 0.0);
    }

    Value b = eval_ast(mem, tab, n->right, frame_base, cur_ret_type);
    if (mem->has_error || mem->jump_active != JUMP_NONE) return val_void();
    uint16_t tb = val_get_type(mem, b);
    uint8_t  ub = val_get_unit(mem, b);

    bool eq = false;
    if (ta == mem->str_type_id && tb == mem->str_type_id && mem->str_type_id != 0) {
        eq = mem_slice_eq(mem, val_get_str(mem, a), val_get_str(mem, b));
    } else if (tb == mem->vec_type_id && ta != mem->vec_type_id && mem->vec_type_id != 0) {
        int in_sid = sym_find(tab, mem, "in", 2);
        if (in_sid >= 0) {
            Value in_args[2] = { a, b };
            Value vin_res = eval_dispatch_overload(mem, tab, (uint16_t)in_sid, FIX_INFIX_LEFT, 2, in_args);
            eq = (!mem->has_error && fabs(val_get_f64(mem, vin_res)) > 1e-15);
        }
    } else if (ta == tb && ua == ub) {
        if (ta > 0 && ta < tab->type_count && !tab->types[ta].is_consuming) {
            uint16_t sz = tab->types[ta].flat_size;
            uint16_t pa = val_get_pool_idx(a);
            uint16_t pb = val_get_pool_idx(b);
            eq = true;
            for (uint16_t k = 0; k < sz; ++k) {
                if (fabs(mem_read_pool(mem, pa + k) - mem_read_pool(mem, pb + k)) > 1e-12) {
                    eq = false;
                    break;
                }
            }
        } else {
            eq = (fabs(val_get_f64(mem, a) - val_get_f64(mem, b)) <= 1e-12);
        }
    }
    return val_make_f64(mem, mem->fast_real_type, eq ? 1.0 : 0.0);
}
