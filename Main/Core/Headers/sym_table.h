/*
 * sym_table.h — Dynamic Module, Type, Unit, Struct, Symbol, Variable, Overload & Extern Registry
 *
 * Purpose:
 *   Stores and queries extension modules (with unbounded SBO+heap capacity and O(1) active-state toggling),
 *   consuming lexical types, physical/dimensional unit types (`distance`,
 *   `angle`, `mass`, `time`, `speed`, ...), flat C-style structs, dynamic
 *   vectors (`$vec`), multidimensional matrices (`$mat`), first-class lambdas
 *   (`$fn`), generic entity & unit wildcards (`T` / `any`), static container
 *   element types (`vec<T, U>`, `map<T, U>`, `mat<T, U>`) with inferred defaults,
 *   operator/function overloads (with O(1) per-symbol chain indexing),
 *   module-scoped variables & constants (with privacy, scoping, mutability &
 *   concrete type/unit locks), private C intrinsic IDs, and host-registered
 *   extern callbacks (`@<extern_name>`).
 */

#ifndef SYM_TABLE_H
#define SYM_TABLE_H

#include "mem_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. REGISTRY CAPACITIES & POLYMORPHIC TYPE/UNIT SENTINELS
 * ============================================================================ */

#define SYM_SBO_MODULES        64   /* Inline SBO module slots before heap growth */
#define SYM_HEAP_MOD_MAGIC 0x4D4F4448u /* "MODH" magic for heap-spilled modules  */
#define SYM_SBO_VARS          256   /* Inline SBO module variable slots           */
#define SYM_HEAP_VAR_MAGIC 0x56415248u /* "VARH" magic for heap-spilled vars     */
#define SYM_MAX_TYPES          64
#define SYM_MAX_UNITS         128
#define SYM_EPHEM_UNIT_BASE   112u  /* Slots 112..127: ephemeral 7D SI compound units */
#define SYM_MAX_PAT_ELEMS     256
#define SYM_MAX_FIELDS        256
#define SYM_MAX_SYMS         2048
#define SYM_SBO_OVERLOADS    2048   /* Inline SBO overload slots before heap growth */
#define SYM_MAX_OVERLOADS   65534   /* Max 16-bit overload slots (SBO + heap)       */
#define SYM_HEAP_OV_MAGIC 0x4F564C48u /* "OVLH" magic for heap-spilled overloads   */
#define SYM_MAX_CONSTS        128
#define SYM_MAX_EXTERNS        32
#define SYM_MAX_OV_PARAMS      16

#define SYM_TYPE_ANY        0xFEu   /* Generic `T` / `any` entity type           */
#define SYM_UNIT_OMITTED    0xFFu   /* Unannotated parameter/return unit         */
#define SYM_UNIT_GENERIC    0xFEu   /* Explicit generic unit `T` / `any`         */
#define SYM_UNIT_LOCK_NONE  0xFDu   /* Explicit concrete unitless `none`         */

/*
 * Static Parameterized Container Type Encoding (`uint8_t` / `uint16_t` TypeSpec):
 *   - 0x00         : MEM_TYPE_VOID (unlocked / omitted)
 *   - 0x01 .. 0x3C : Concrete entity type ID (`1..60`, e.g. `real`, `vec`, `map`, `mat`, `vec2`)
 *   - 0x3D         : SYM_ELEM_ANY (`T` / `any` element wildcard inside `<...>`)
 *   - 0x40 | elem6 : Parameterized `vec<elem6>` / `list<elem6>` / `stack<elem6>` / `queue<elem6>`
 *   - 0x80 | elem6 : Parameterized `map<elem6>` / `dict<elem6>`
 *   - 0xC0 | elem6 : Parameterized `mat<elem6>` (`0xC0 .. 0xFD`)
 *   - 0xFE         : SYM_TYPE_ANY (top-level generic entity type `T` / `any`)
 */
#define SYM_ELEM_ANY        0x3Du
#define SYM_TY_KIND_MASK    0xC0u
#define SYM_TY_VEC_BIT      0x40u
#define SYM_TY_MAP_BIT      0x80u
#define SYM_TY_MAT_BIT      0xC0u
#define SYM_TY_ELEM_MASK    0x3Fu

static inline bool sym_type_is_parameterized(uint16_t type_spec) {
    uint8_t b = (uint8_t)(type_spec & 0xFFu);
    return (b >= 0x40u && b < SYM_TYPE_ANY);
}

static inline uint8_t sym_pack_container_type(const MemArena *mem, uint16_t outer_tid, uint16_t elem_tid) {
    uint8_t e = (elem_tid == SYM_TYPE_ANY || elem_tid == SYM_ELEM_ANY)
                ? SYM_ELEM_ANY
                : (uint8_t)(elem_tid & SYM_TY_ELEM_MASK);
    if (mem && outer_tid != 0) {
        if (outer_tid == mem->vec_type_id) return (uint8_t)(SYM_TY_VEC_BIT | e);
        if (outer_tid == mem->map_type_id) return (uint8_t)(SYM_TY_MAP_BIT | e);
        if (outer_tid == mem->mat_type_id) return (uint8_t)(SYM_TY_MAT_BIT | e);
    }
    return (uint8_t)(outer_tid & 0xFFu);
}

static inline uint16_t sym_unpack_outer_type(const MemArena *mem, uint16_t type_spec) {
    uint8_t b = (uint8_t)(type_spec & 0xFFu);
    if (b < 0x40u || b >= SYM_TYPE_ANY) return b;
    if (!mem) return 0;
    uint8_t kind = (uint8_t)(b & SYM_TY_KIND_MASK);
    if (kind == SYM_TY_VEC_BIT) return mem->vec_type_id;
    if (kind == SYM_TY_MAP_BIT) return mem->map_type_id;
    if (kind == SYM_TY_MAT_BIT) return mem->mat_type_id;
    return b;
}

static inline uint16_t sym_unpack_elem_type(const MemArena *mem, uint16_t type_spec) {
    (void)mem;
    uint8_t b = (uint8_t)(type_spec & 0xFFu);
    if (b < 0x40u || b >= SYM_TYPE_ANY) return 0;
    uint8_t e = (uint8_t)(b & SYM_TY_ELEM_MASK);
    return (e == SYM_ELEM_ANY) ? SYM_TYPE_ANY : e;
}

/* Module Variable, Constant, Overload & Type Privacy / Mutability Flags */
#define SYM_VAR_MUTABLE        0x01u /* 1 = mutable (`:=`/`mut`), 0 = immutable (`=`/`const`) */
#define SYM_VAR_PRIVATE        0x02u /* 1 = module-private (`priv`), 0 = public (`pub`)       */
#define SYM_VAR_EXPLICIT_CONST 0x04u /* 1 = explicitly declared with `const`                  */
#define SYM_VAR_REASSIGN_ONLY  0x08u /* AST store flag: plain `=` or compound `+=`            */
#define SYM_VAR_COMPOUND_OP    0x10u /* AST store flag: compound assignment (`+=`, `-=`, ...) */
#define SYM_VAR_QUAL_MOD       0x80u /* AST_CONST_SYM sub bit: explicit module qualifier      */
#define AST_CALL_QUAL_MOD_BIT  0x4000u /* AST_CALL right bit: explicit module qualifier       */

#define SYM_OV_PRIVATE         0x02u /* OpOverload.reserved bit: private to `ov->mod_id`      */
#define SYM_OV_HAS_LABELS      0x04u /* Cached: body_ast contains at least one AST_TAGGED_SCOPE */
#define SYM_OV_LABELS_CHECKED  0x08u /* Cached: body_ast has been scanned for AST_TAGGED_SCOPE  */
#define SYM_TYPE_PRIVATE       0x02u /* TypeDef.reserved bit: private to `td->mod_id`         */

/* High-bit `OpOverload.meta_tag` flags for `.math` lazy bound-variable (`var_x`) functions */
#define SYM_META_LAZY_MASK     0x80u /* Bit 7: lazy bound-variable function (`(V << 3) | E`) */
#define SYM_META_LAZY_EQ_BIT   0x40u /* Bit 6: rewrite top-level `==` / `~` to `-` (`eq`)    */

/* ============================================================================
 * 2. PRIVATE C INTRINSIC OPCODES & AST NODE KINDS
 * ============================================================================ */

typedef enum {
    INTR_NONE = 0,
    /* Lexer & Container Type Intrinsics (`$type ~ pattern => @parser`) */
    INTR_PARSE_F64,
    INTR_PARSE_F64_TRIM1,
    INTR_PARSE_I64,
    INTR_PARSE_STR_SLICE,
    INTR_VEC_TYPE,     /* @vec_type     (dynamic growable/shrinkable vector of any T) */
    INTR_MAT_TYPE,     /* @mat_type     (multidimensional matrix/tensor `[...]`)      */
    INTR_FN_TYPE,      /* @fn_type      (first-class anonymous function `(x) => ...`) */
    INTR_MAP_TYPE,     /* @map_type     (dynamic key-value dictionary `map()`)        */
    INTR_COMPLEX_TYPE, /* @complex_type (2-slot rectangular complex struct `a + bi`)  */
    INTR_ANY_TYPE,     /* @any_type     (generic entity/unit wildcard `$T`, `$any`)   */
    INTR_NONE_UNIT,    /* @none_unit    (concrete unitless sentinel `$none`)          */

    /* Bootstrap Expression Syntax Delimiters (`"sym" => @lparen`) */
    INTR_LPAREN,    /* @lparen */
    INTR_RPAREN,    /* @rparen */
    INTR_LBRACE,    /* @lbrace */
    INTR_RBRACE,    /* @rbrace */
    INTR_LBRACK,    /* @lbrack */
    INTR_RBRACK,    /* @rbrack */
    INTR_COMMA,     /* @comma  */

    /* Bootstrap Meta / Lazy Control Intrinsics */
    INTR_SEQ,           /* @seq(a, b)            */
    INTR_STORE,         /* @store(dst, v)        */
    INTR_WHILE,         /* @while(c, body)       */
    INTR_DO_WHILE,      /* @do_while(body, c)    */
    INTR_IF,            /* @if(c, t, e)          */
    INTR_OR_BRANCH,     /* @or_branch(c, b) / @then_branch(c, b) */
    INTR_ELSE_BRANCH,   /* @else_branch(b)       */
    INTR_SWITCH_BLOCK,  /* @switch_block(v, b)   */
    INTR_CASE_BRANCH,   /* @case_branch(v, b)    */
    INTR_FOR_KW,        /* @for_kw(expr, b)      */
    INTR_TERNARY,       /* @ternary(c, pair)     */
    INTR_COLON,         /* @colon(a, b)          */
    INTR_JUMP_TAG,      /* @jump_tag(tag)        */
    INTR_RUN_TAG,       /* @run_tag(tag)         */
    INTR_BREAK_LOOP,    /* @break_loop(tag)      */
    INTR_CONTINUE_LOOP, /* @continue_loop(tag)   */
    INTR_RETURN_FN,     /* @return_fn(val)       */
    INTR_LAMBDA,        /* @lambda(params, body) */
    INTR_PIPE,          /* @pipe(x, f) (`x |> f` and `x |> f(a, b)`)       */
    INTR_CAND,          /* @cand(a, b) (`&` distributed/short-circuit AND) */
    INTR_COR,           /* @cor(a, b)  (`|` distributed/short-circuit OR)  */
    INTR_CNOT,          /* @cnot(a)    (`!` strict boolean condition NOT)  */
    INTR_SYM_DIFF,      /* @sym_diff(x, expr[, order])                     */
    INTR_SYM_INTEG,     /* @sym_integ(x, expr)                             */
    INTR_SYM_SOLVE,     /* @sym_solve(x, eq)                               */
    INTR_SYM_SIMPLIFY,  /* @sym_simplify(expr)                             */
    INTR_SYM_EVAL,      /* @sym_eval(expr_str, x, val)                     */
    INTR_FIELD,         /* @field(s, f)          */

    /* Dynamic Vector, Stack, Queue & Higher-Order Functional Intrinsics (`$vec`, `$fn`, `$map`) */
    INTR_VNEW,        /* @vnew(cap)            */
    INTR_VFILL,       /* @vfill(cnt, init)     */
    INTR_VPUSH,        /* @vpush(v, item)       */
    INTR_VPOP,         /* @vpop(v)              */
    INTR_VSHIFT,       /* @vshift(v) (FIFO queue dequeue / pop_front)       */
    INTR_VGET,         /* @vget(v, idx)         */
    INTR_VSET,         /* @vset(v, idx, item)   */
    INTR_VLEN,         /* @vlen(v)              */
    INTR_VCAP,         /* @vcap(v)              */
    INTR_VRESIZE,      /* @vresize(v, n)        */
    INTR_VSHRINK,      /* @vshrink(v)           */
    INTR_VCLEAR,       /* @vclear(v)            */
    INTR_VRANGE,       /* @vrange(a, b, mode) (`a...b`, `a..<b`, `a>..b`, `a>..<b`) */
    INTR_VRANGE_CAT,   /* @vrange_cat(v1, v2) (concatenated multi-segment range)   */
    INTR_VRANGE_STEP,  /* @vrange_step(v, step, 0) (`a...b by step`)              */
    INTR_FCALL,        /* @fcall(f, a, b)       */
    INTR_MAP_NEW,      /* @map_new(cap)         */
    INTR_MAP_SET,      /* @map_set(m, k, v)     */
    INTR_MAP_GET,      /* @map_get(m, k, def)   */
    INTR_MAP_HAS,      /* @map_has(m, k)        */
    INTR_MAP_DEL,      /* @map_del(m, k)        */

    /* Multidimensional Matrix Intrinsics (`$mat`) */
    INTR_MRANK,       /* @mrank(m)           */
    INTR_MLEN,        /* @mlen(m)            */
    INTR_MDIM,        /* @mdim(m, axis)      */
    INTR_MRESHAPE,    /* @mreshape(m, r, c)  */

    /* Core Arithmetic Intrinsics */
    INTR_ADD,
    INTR_SUB,
    INTR_MUL,
    INTR_DIV,
    INTR_MOD,
    INTR_NEG,
    INTR_ABS,
    INTR_FLOOR,
    INTR_CEIL,

    /* Powers, Roots & Exponential Intrinsics */
    INTR_POW,
    INTR_SQRT,
    INTR_CBRT,
    INTR_EXP,
    INTR_LN,

    /* Trigonometric & Hyperbolic Intrinsics */
    INTR_SIN,
    INTR_COS,
    INTR_TAN,
    INTR_ASIN,
    INTR_ACOS,
    INTR_ATAN,
    INTR_ATAN2,
    INTR_ERF,
    INTR_GAMMA,
    INTR_LGAMMA,

    /* Comparison & Boolean Logic Intrinsics */
    INTR_EQ,
    INTR_NE,
    INTR_LT,
    INTR_LE,
    INTR_GT,
    INTR_GE,
    INTR_BAND,
    INTR_BOR,
    INTR_BXOR,
    INTR_BNOT,
    INTR_BSHL,
    INTR_BSHR,

    /* Type & Unit Introspection / String Conversion Intrinsics */
    INTR_RAW,       /* @raw(x[, donor]) -> raw value (optionally adopting donor's unit) */
    INTR_TYPE_NAME, /* @type_name(x[, mode]) -> $str (0=outer, 1=elem, 2=full sig)      */
    INTR_UNIT_NAME, /* @unit_name(x[, axis]) -> $str or SI dim exponent                 */
    INTR_TOSTR,     /* @tostr(x)   -> $str (postfix `_` operator) */
    INTR_FROMSTR,   /* @fromstr(s) -> T    (prefix `_` operator)  */
    INTR_STR_FMT,       /* @str_fmt(tmpl, a, b)                   */
    INTR_REGEX_MATCH,   /* @regex_match(s, pat)                   */
    INTR_REGEX_FIND,    /* @regex_find(s, pat, mode)              */
    INTR_REGEX_FINDALL, /* @regex_find_all(s, pat)                */
    INTR_REGEX_REPLACE, /* @regex_replace(s, pat, repl)           */
    INTR_REGEX_SPLIT,   /* @regex_split(s, pat)                   */
    INTR_GC_COMPACT,     /* @gc_compact()                          */
    INTR_POOL_USED,      /* @pool_used()                           */
    INTR_ERR,            /* @err(msg:str) -> raises custom error   */
    INTR_PRINT,          /* @print(args...)                        */
    INTR_PRINTLN,        /* @println(args...)                      */
    INTR_ASSERT,         /* @assert(cond[, msg])                   */
    INTR_CLOCK_MS,       /* @clock_ms()                            */
    INTR_FILE_READ,      /* @file_read(path)                       */
    INTR_FILE_WRITE,     /* @file_write(path, content)             */
    INTR_FILE_APPEND,    /* @file_append(path, content)            */
    INTR_FILE_EXISTS,    /* @file_exists(path)                     */
    INTR_JSON_PARSE,     /* @json_parse(s)                         */
    INTR_JSON_STRINGIFY, /* @json_stringify(v, indent)             */
    INTR_IMPORT,         /* @import(spec)                          */
    INTR_COUNT
} IntrinsicId;

_Static_assert(INTR_COUNT <= 255, "IntrinsicId must fit in uint8_t");
_Static_assert(INTR_FCALL < SYM_META_LAZY_MASK, "Meta IntrinsicIds must fit below SYM_META_LAZY_MASK (0x80)");

typedef enum {
    AST_LIT = 0,
    AST_CONST_SYM,
    AST_SLOT_LOAD,
    AST_FIELD_LOAD,
    AST_UNARY,
    AST_BINARY,
    AST_CALL,
    AST_ARG_LINK,
    AST_INTRINSIC,
    AST_CONSTRUCT,
    AST_EXTERN,        /* Host-registered callback `@<extern_name>(args...)`    */
    AST_TERNARY,       /* Lazy branch `cond ? then_ast : else_ast` & `if/or`    */
    AST_SWITCH_EQ,     /* Pattern/value match for `switch` cases                */
    AST_TAGGED_SCOPE,  /* Labeled scope/loop `Tag : { body }` (sub = label_id)  */
    AST_JUMP,          /* Jump to labeled scope `jump(Tag)` (sub = label_id)    */
    AST_RUN_TAG,       /* Inline reuse of labeled scope `run(Tag)`              */
    AST_BREAK,         /* Break out of loop/scope `break [val]` / `break(Tag)`  */
    AST_CONTINUE,      /* Continue next iteration `continue` / `continue(Tag)`  */
    AST_RETURN,        /* Early return from function/eval `return [val]`        */
    AST_MAT_LIT,       /* Multidimensional matrix literal `[...]`               */
    AST_INDEX,         /* C-style subscript & slicing `base[i]`, `base[a..b]`   */
    AST_FOR_IN,        /* Range/collection loop & comprehension `[e for x in v]`*/
    AST_UNPACK_ELEM,   /* Multi-variable destructuring / scalar broadcast       */
    AST_LAMBDA,        /* First-class anonymous function `(x) => expr`          */
    AST_FN_CALL        /* Direct invocation of a `$fn` handle `f(args...)`      */
} AstKind;

/* ============================================================================
 * 3. TYPE, STRUCT, PATTERN, OPERATOR & HOST EXTERN DESCRIPTORS
 * ============================================================================ */

typedef enum {
    PAT_DIGIT = 1,
    PAT_ALPHA,
    PAT_LIT,
    PAT_NOT_CHARS,
    PAT_GROUP_START,
    PAT_GROUP_END,
    PAT_CHARS,
    PAT_ALT
} PatKind;

/* 8-byte 7D SI base dimensional exponent vector for a unit group (`@dim` in .math) */
typedef struct {
    int8_t  exp[7];   /* (m, kg, s, A, K, mol, cd) */
    uint8_t has_dim;  /* 1 if dimensional exponents are defined, else 0 */
} UnitDimDef;

typedef enum {
    QUANT_ONE = 0,
    QUANT_PLUS,
    QUANT_STAR,
    QUANT_OPT
} PatQuant;

typedef struct {
    uint8_t  kind;
    uint8_t  quant;
    StrSlice chars;
} PatElem;

typedef struct {
    StrSlice name;
    uint16_t type_id;
    uint16_t flat_offset;
} FieldDef;

typedef struct {
    StrSlice name;
    uint16_t mod_id;
    uint8_t  is_consuming;
    uint8_t  parser_intr;
    uint8_t  field_count;
    uint8_t  reserved;
    uint16_t start_idx;
    uint16_t flat_size;
} TypeDef;

typedef enum {
    FIX_PREFIX      = 0,
    FIX_POSTFIX     = 1,
    FIX_INFIX_LEFT  = 2,
    FIX_INFIX_RIGHT = 3,
    FIX_FUNC        = 4,
    FIX_SYNTAX      = 5
} OpFixity;

typedef struct {
    uint16_t sym_id;
    uint16_t mod_id;
    uint8_t  fixity;
    uint8_t  prec;
    uint8_t  param_count;
    uint8_t  local_count;
    uint8_t  ret_type;
    uint8_t  ret_unit;       /* SYM_UNIT_OMITTED, SYM_UNIT_GENERIC, SYM_UNIT_LOCK_NONE, or 1..127 */
    uint8_t  meta_tag;       /* 1 = inlined alias, meta IntrinsicId, or SYM_META_LAZY_MASK        */
    uint8_t  reserved;       /* SYM_OV_PRIVATE when declared with `priv`                          */
    uint8_t  param_types[SYM_MAX_OV_PARAMS];
    uint8_t  param_units[SYM_MAX_OV_PARAMS];
    uint16_t body_ast;
} OpOverload;

typedef struct {
    uint16_t sym_id;
    uint16_t mod_id;
    uint8_t  type_id;
    uint8_t  unit_id;
    uint8_t  val_idx;
    uint8_t  reserved;       /* SYM_VAR_MUTABLE | SYM_VAR_PRIVATE flags in `.mathc`              */
    uint16_t pool_idx;       /* Index into MemArena.f64_pool for ABI-neutral binary image        */
} ConstDef;

/* Runtime Module-Scoped Variable / Constant Descriptor (supports all Value types & SBO+heap growth) */
typedef struct {
    uint16_t sym_id;         /* Interned symbol ID in SymTable.syms                              */
    uint16_t mod_id;         /* Owning module ID in SymTable                                     */
    uint16_t lock_type;      /* Low byte = concrete TypeSpec lock, high byte = inferred TypeSpec */
    uint8_t  lock_unit;      /* 0/SYM_UNIT_OMITTED = unlocked, 1..127 or SYM_UNIT_LOCK_NONE      */
    uint8_t  flags;          /* SYM_VAR_MUTABLE | SYM_VAR_PRIVATE | SYM_VAR_EXPLICIT_CONST       */
    Value    val;            /* Live 8-byte NaN-boxed Value (scalar, $str, $vec, $mat, $map, $fn)*/
} ModVarDef;

_Static_assert(sizeof(StrSlice)   == 4,  "StrSlice must be strictly 4 bytes");
_Static_assert(sizeof(PatElem)    == 6,  "PatElem must be strictly 6 bytes");
_Static_assert(sizeof(FieldDef)   == 8,  "FieldDef must be strictly 8 bytes");
_Static_assert(sizeof(TypeDef)    == 14, "TypeDef must be strictly 14 bytes");
_Static_assert(sizeof(OpOverload) == 46, "OpOverload must be strictly 46 bytes");
_Static_assert(sizeof(ConstDef)   == 10, "ConstDef must be strictly 10 bytes");

struct SymTable;

typedef bool (*EngExternFn)(MemArena *mem, const struct SymTable *tab,
                            const Value *args, uint8_t argc,
                            void *user_ctx, Value *out_val);

typedef struct {
    char        name[32];    /* Self-contained name (survives binary reloads) */
    EngExternFn fn;
    void       *user_ctx;
    void      (*fn_free)(void *user_ctx); /* Optional destructor: runs when the
                                            registration is replaced or the
                                            owning table/engine is destroyed */
} ExternDef;

/* ============================================================================
 * 4. SYMBOL, TYPE, UNIT & MODULE VARIABLE TABLE CONTEXT
 * ============================================================================ */

typedef struct SymTable {
    StrSlice    sbo_mod_names[SYM_SBO_MODULES];
    uint8_t     sbo_mod_active[SYM_SBO_MODULES];
    StrSlice   *heap_mod_names;
    uint8_t    *heap_mod_active;
    uint32_t    heap_mod_magic;
    uint16_t    mod_cap;
    uint16_t    mod_count;
    uint16_t    cur_mod;

    TypeDef     types[SYM_MAX_TYPES];
    uint8_t     type_count;

    StrSlice    units[SYM_MAX_UNITS];
    UnitDimDef  unit_dims[SYM_MAX_UNITS];
    uint8_t     unit_count;

    PatElem     pat_elems[SYM_MAX_PAT_ELEMS];
    uint16_t    pat_elem_count;

    FieldDef    fields[SYM_MAX_FIELDS];
    uint16_t    field_count;

    StrSlice    syms[SYM_MAX_SYMS];
    uint16_t    sym_count;

    /* Unbounded SBO + Heap Operator & Function Overload Table */
    OpOverload  sbo_overloads[SYM_SBO_OVERLOADS];
    uint16_t    sbo_ov_next[SYM_SBO_OVERLOADS];
    OpOverload *heap_overloads;
    uint16_t   *heap_ov_next;
    uint32_t    heap_ov_magic;
    uint16_t    ov_cap;
    uint16_t    overload_count;

    /* Per-symbol overload chains for O(1) symbol dispatch (rebuilt in-memory) */
    uint16_t    sym_first_ov[SYM_MAX_SYMS];
    uint16_t    sym_last_ov[SYM_MAX_SYMS];

    ConstDef    consts[SYM_MAX_CONSTS];
    Value       const_vals[SYM_MAX_CONSTS];
    uint8_t     const_count;

    /* Unbounded SBO + Heap Module-Scoped Variable & Constant Table */
    ModVarDef   sbo_vars[SYM_SBO_VARS];
    ModVarDef  *heap_vars;
    uint32_t    heap_var_magic;
    uint16_t    var_cap;
    uint16_t    var_count;

    /* Base-Layer (`stdlib + patches`) watermarks & Linked Project Entry Point */
    uint16_t    base_mod_count;
    uint8_t     base_type_count;
    uint8_t     base_unit_count;
    UnitDimDef  base_unit_dims[SYM_MAX_UNITS];
    uint16_t    base_pat_elem_count;
    uint16_t    base_field_count;
    uint16_t    base_sym_count;
    uint16_t    base_overload_count;
    uint8_t     base_const_count;
    double      base_const_f64[SYM_MAX_CONSTS];
    uint16_t    entry_mod_id;
    uint16_t    entry_ov_idx;
    uint8_t     ephem_unit_cursor;

    /* Host-registered runtime callbacks (not serialized into binary images) */
    ExternDef   externs[SYM_MAX_EXTERNS];
    uint8_t     extern_count;
} SymTable;

/* ============================================================================
 * 5. SYMBOL TABLE API
 * ============================================================================ */

static inline const StrSlice *sym_mod_names(const SymTable *tab) {
    return (tab->heap_mod_magic == SYM_HEAP_MOD_MAGIC && tab->heap_mod_names != NULL)
           ? tab->heap_mod_names
           : tab->sbo_mod_names;
}

static inline StrSlice *sym_mod_names_mut(SymTable *tab) {
    return (tab->heap_mod_magic == SYM_HEAP_MOD_MAGIC && tab->heap_mod_names != NULL)
           ? tab->heap_mod_names
           : tab->sbo_mod_names;
}

static inline const uint8_t *sym_mod_active(const SymTable *tab) {
    return (tab->heap_mod_magic == SYM_HEAP_MOD_MAGIC && tab->heap_mod_active != NULL)
           ? tab->heap_mod_active
           : tab->sbo_mod_active;
}

static inline uint8_t *sym_mod_active_mut(SymTable *tab) {
    return (tab->heap_mod_magic == SYM_HEAP_MOD_MAGIC && tab->heap_mod_active != NULL)
           ? tab->heap_mod_active
           : tab->sbo_mod_active;
}

static inline StrSlice sym_mod_name(const SymTable *tab, uint16_t mod_id) {
    return sym_mod_names(tab)[mod_id];
}

static inline const ModVarDef *sym_vars(const SymTable *tab) {
    return (tab->heap_var_magic == SYM_HEAP_VAR_MAGIC && tab->heap_vars != NULL)
           ? tab->heap_vars
           : tab->sbo_vars;
}

static inline ModVarDef *sym_vars_mut(SymTable *tab) {
    return (tab->heap_var_magic == SYM_HEAP_VAR_MAGIC && tab->heap_vars != NULL)
           ? tab->heap_vars
           : tab->sbo_vars;
}

static inline const OpOverload *sym_overloads(const SymTable *tab) {
    return (tab->heap_ov_magic == SYM_HEAP_OV_MAGIC && tab->heap_overloads != NULL)
           ? tab->heap_overloads
           : tab->sbo_overloads;
}

static inline OpOverload *sym_overloads_mut(SymTable *tab) {
    return (tab->heap_ov_magic == SYM_HEAP_OV_MAGIC && tab->heap_overloads != NULL)
           ? tab->heap_overloads
           : tab->sbo_overloads;
}

static inline const uint16_t *sym_ov_next(const SymTable *tab) {
    return (tab->heap_ov_magic == SYM_HEAP_OV_MAGIC && tab->heap_ov_next != NULL)
           ? tab->heap_ov_next
           : tab->sbo_ov_next;
}

static inline uint16_t *sym_ov_next_mut(SymTable *tab) {
    return (tab->heap_ov_magic == SYM_HEAP_OV_MAGIC && tab->heap_ov_next != NULL)
           ? tab->heap_ov_next
           : tab->sbo_ov_next;
}

static inline bool sym_mod_enabled(const SymTable *tab, uint16_t mod_id) {
    if (!tab || mod_id >= tab->mod_count) {
        return false;
    }
    return sym_mod_active(tab)[mod_id] != 0;
}

static inline bool sym_ov_is_visible(const SymTable *tab, const OpOverload *ov) {
    if (!sym_mod_enabled(tab, ov->mod_id)) return false;
    if ((ov->reserved & SYM_OV_PRIVATE) != 0 && ov->mod_id != tab->cur_mod) return false;
    return true;
}

void        sym_init(SymTable *tab);
void        sym_free_heap(SymTable *tab);
bool        sym_ensure_mod_cap(SymTable *tab, MemArena *mem, uint16_t needed_cap);
bool        sym_ensure_var_cap(SymTable *tab, MemArena *mem, uint16_t needed_cap);
bool        sym_ensure_ov_cap(SymTable *tab, MemArena *mem, uint16_t needed_cap);
void        sym_rebuild_index(SymTable *tab);
void        sym_link_overload(SymTable *tab, uint16_t ov_idx);

/* Module management & O(1) active state toggling (unbounded module count) */
uint16_t    sym_find_or_add_mod(SymTable *tab, MemArena *mem, StrSlice name);
int         sym_find_mod(const SymTable *tab, const MemArena *mem, const char *mod_name);
int         sym_find_mod_slice(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len);
bool        sym_set_module(SymTable *tab, const MemArena *mem, const char *mod_name, bool enabled);
bool        sym_is_module_active(const SymTable *tab, const MemArena *mem, const char *mod_name);
bool        sym_reset_module(SymTable *tab, MemArena *mem, const char *mod_name);
void        sym_snapshot_base(SymTable *tab, MemArena *mem);
bool        sym_flush_user_modules(SymTable *tab, MemArena *mem);

/* Module-Scoped Variable & Constant Lookup, Privacy & Mutability API */
void        sym_sync_consts_to_vars(SymTable *tab, MemArena *mem);
int         sym_find_own_var(const SymTable *tab, uint16_t sym_id, uint16_t mod_id);
int         sym_find_var(const SymTable *tab, uint16_t sym_id, uint16_t caller_mod,
                         uint16_t *out_priv_mod);
bool        sym_validate_and_stamp_lock(const SymTable *tab, MemArena *mem, uint16_t sym_id, const char *ctx_label, uint16_t req_type, uint8_t req_unit, Value *io_val);
int         sym_set_or_add_var(SymTable *tab, MemArena *mem, uint16_t sym_id, uint16_t mod_id,
                               Value val, uint16_t lock_type, uint8_t lock_unit,
                               uint8_t flags, bool is_decl);
bool        sym_has_priv_ov(const SymTable *tab, uint16_t sym_id, OpFixity fix_filter,
                            uint16_t *out_priv_mod);

/* Symbol Interning & Lookup */
uint16_t    sym_intern(SymTable *tab, MemArena *mem, StrSlice s);
int         sym_find(const SymTable *tab, const MemArena *mem, const char *str, uint16_t len);
bool        sym_has_active_op(const SymTable *tab, uint16_t sym_id);
uint8_t     sym_get_active_syntax(const SymTable *tab, uint16_t sym_id);
int         sym_get_active_prec(const SymTable *tab, uint16_t sym_id, OpFixity fixity);
uint8_t     sym_active_meta_tag(const SymTable *tab, uint16_t sym_id, OpFixity fix_filter);
bool        sym_is_reserved_kw(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len);

/* Entity Type, Unit Type & Flat Struct Queries */
uint16_t    sym_find_type(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len);
uint8_t     sym_find_or_add_unit(SymTable *tab, MemArena *mem, const char *name, uint16_t len);
uint8_t     sym_find_unit(const SymTable *tab, const MemArena *mem, const char *name, uint16_t len);
bool        sym_resolve_field(const SymTable *tab, const MemArena *mem, uint16_t type_id,
                              const char *fname, uint16_t flen,
                              uint16_t *out_field_type, uint16_t *out_flat_off);
const char *sym_type_name(const SymTable *tab, const MemArena *mem, uint16_t type_id,
                          char *buf, size_t cap);
const char *sym_unit_name(const SymTable *tab, const MemArena *mem, uint8_t unit_id,
                          char *buf, size_t cap);

/* Intrinsic Name Lookup & Host Extern Callback Registration */
IntrinsicId      sym_lookup_intrinsic(const char *s, uint16_t len);
bool             sym_register_extern(SymTable *tab, MemArena *mem, const char *name,
                                     EngExternFn fn, void *user_ctx);
bool             sym_register_extern_full(SymTable *tab, MemArena *mem, const char *name,
                                          EngExternFn fn, void *user_ctx,
                                          void (*fn_free)(void *user_ctx));
void             sym_free_externs(SymTable *tab);
const ExternDef *sym_find_extern(const SymTable *tab, const MemArena *mem, uint16_t sym_id);

#ifdef __cplusplus
}
#endif

#endif /* SYM_TABLE_H */
