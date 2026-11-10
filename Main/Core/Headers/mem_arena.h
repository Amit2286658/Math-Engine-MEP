/*
 * mem_arena.h — Isolated Memory Management & Value Representation Module
 *
 * Purpose:
 *   Encapsulates ALL memory allocation, string interning, float/struct/vector
 *   pools, transparent Small-Buffer-Optimized (SBO) Arena-to-Heap spill blocks,
 *   AST node storage, call stack frames, scratch watermarks, and Value
 *   bit packing (8-byte IEEE-754 NaN-boxing), including
 *   orthogonal Entity Type (`type_id`) and Unit Type (`unit_id`) tags.
 *
 *   To customize memory sizes, switch to a heap/slab/embedded SRAM allocator,
 *   or change Value encoding in the future, you ONLY edit `mem_arena.h` and
 *   `mem_arena.c` — zero changes needed in lexer, parser, or evaluator.
 */

#ifndef MEM_ARENA_H
#define MEM_ARENA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 1. COMPILE-TIME VALUE ENCODING & ARENA CAPACITY TUNABLES
 * ============================================================================ */

#define MEM_SRC_BUF_CAP     49152   /* UTF-8 source buffer for interned strings   */
#define MEM_SBO_AST_NODES   24576   /* Inline SBO 8-byte AST nodes (192 KB)       */
#define MEM_MAX_AST_NODES   65534   /* Max 16-bit AST nodes (SBO + heap growth)   */
#define MEM_HEAP_AST_MAGIC 0x41535448u /* "ASTH" magic for heap-spilled AST pool  */
#define MEM_MAX_F64_POOL     8192   /* Flat double pool for literals/structs/vecs */
#define MEM_MAX_CALL_STACK   2048   /* Evaluation & local variable stack slots    */
#define MEM_MAX_LABELS         16   /* Max tagged scopes (`Tag : { ... }`)        */
#define MEM_MAX_EVAL_DEPTH    640   /* Max recursive AST evaluation depth         */
#define MEM_MAX_EVAL_STEPS 2000000u /* Global AST evaluation step budget per eval */

/* Small-Buffer Optimization (SBO) & Transparent Heap Spill Tunables */
#define MEM_SBO_MAX_SLOTS      64u  /* Containers <= 64 doubles use fast f64_pool */
#define MEM_MAX_HEAP_BLOCKS   256u  /* Max concurrent heap-spilled containers     */
#define MEM_MAX_HEAP_SLOTS 262144u  /* Max doubles per heap allocation (2 MB cap) */
#define MEM_HEAP_IDX_BIT   0x8000u  /* Tag bit in `data_idx` for HeapBlock handle */
#define MEM_HEAP_ID_MASK   0x7FFFu  /* Mask to extract `0..255` HeapBlock index   */

#define MEM_HDR_IMMUTABLE_BIT 0x8000u /* Bit 15 in container header for `const`   */

#define MEM_NO_IDX        0xFFFFu  /* Null / failed 16-bit index sentinel        */
#define MEM_TYPE_VOID          0u  /* Invalid / void entity type ID              */
#define MEM_UNIT_NONE          0u  /* Dimensionless / no unit tag                */
#define MEM_VEC_HDR_SLOTS      4u  /* [elem_type_id, len, cap, data_pool_idx]    */
#define MEM_MAT_HDR_SLOTS      8u  /* [rank, total_len, data_idx, flags, d0..d3] */
#define MEM_FN_HDR_SLOTS       4u  /* [body_ast, param_cnt, slots, frame_base]   */
#define MEM_MAP_HDR_SLOTS      4u  /* [val_type_unit, len, cap, data_pool_idx]   */
#define MEM_FN_MAX_PARAMS     16u  /* Max parameters for lambdas & module fns    */
#define MEM_MAX_TEMP_ROOTS    64u  /* C stack temporary Value* roots for GC      */
#define MEM_OUT_BUF_CAP     4096u  /* Captured print/println stdout buffer bytes */
#define MEM_STR_HASH_BUCKETS 512u  /* Open-addressed string interning hash table */
#define MEM_MAX_ACTIVE_CLOSURES 16u /* Tracked active-frame lexical closure IDs  */

typedef enum {
    JUMP_NONE          = 0,
    JUMP_GOTO          = 1,  /* `jump(Tag)`        — transfer PC to `Tag`            */
    JUMP_BREAK_ANON    = 2,  /* `break [val]`      — exit innermost loop or scope    */
    JUMP_BREAK_TAG     = 3,  /* `break(Tag[,val])` — exit enclosing `Tag` loop/scope */
    JUMP_CONTINUE_ANON = 4,  /* `continue`         — next iteration of innermost loop*/
    JUMP_CONTINUE_TAG  = 5,  /* `continue(Tag)`    — next iteration of `Tag` loop    */
    JUMP_RETURN        = 6,  /* `return [val]`     — early return from function/eval */
    JUMP_THROW         = 7   /* `throw [val]`      — structured exception unwind     */
} JumpKind;

/* ============================================================================
 * 2. PRIMITIVE STORAGE TYPES (`StrSlice`, `Value`, `AstNode`, `HeapBlock`)
 * ============================================================================ */

/* 4-byte zero-copy UTF-8 slice into `MemArena.src_buf` */
typedef struct {
    uint16_t off;
    uint16_t len;
} StrSlice;

/* 8-Byte IEEE-754 NaN-Boxed Value (bits 32..39 = type_id, bits 40..47 = unit_id) */
typedef uint64_t Value;

_Static_assert(sizeof(Value) == 8, "8-byte NaN-Boxed Value must be strictly 8 bytes");

/* Strictly 8-byte AST node stored in `MemArena.ast` */
typedef struct {
    uint8_t  kind;
    uint8_t  sub;
    uint16_t ref;
    uint16_t left;
    uint16_t right;
} AstNode;

_Static_assert(sizeof(AstNode) == 8, "AstNode must be strictly 8 bytes");


/* Dynamically spilled heap payload block for large `$vec`, `$mat`, and `$map` containers */
typedef struct {
    double  *data;       /* malloc-backed array of `cap_slots` doubles (or NULL) */
    uint32_t cap_slots;  /* Allocated capacity in 64-bit `double` slots          */
    uint16_t owner_hdr;  /* Owning header index in `f64_pool`                    */
    uint8_t  in_use;     /* 1 = active heap block, 0 = free slot                 */
    uint8_t  marked;     /* Mark bit during `mem_compact_scratch` / scope GC     */
} HeapBlock;

struct SymTable;

/* ============================================================================
 * 3. MEMORY ARENA CONTEXT
 * ============================================================================ */

typedef struct {
    /* Interned UTF-8 String Pool */
    char        src_buf[MEM_SRC_BUF_CAP];
    uint16_t    src_len;
    uint16_t    perm_src_len;

    /* 8-Byte AST Node Pool (Unbounded SBO + Heap Growth) + Fallback Sentinel */
    AstNode     sbo_ast[MEM_SBO_AST_NODES];
    AstNode    *heap_ast;
    uint32_t    heap_ast_magic;
    uint16_t    ast_cap;
    AstNode     invalid_ast_mut;
    uint16_t    ast_count;
    uint16_t    perm_ast_count;

    /* Flat 64-Bit Float, C-Struct & SBO Container Pool */
    double      f64_pool[MEM_MAX_F64_POOL];
    uint16_t    pool_top;
    uint16_t    perm_pool_top;

    /* Transparent SBO-to-Heap Spill Table (for payloads > MEM_SBO_MAX_SLOTS) */
    HeapBlock   heap_blocks[MEM_MAX_HEAP_BLOCKS];
    uint16_t    heap_active_count;
    uint32_t    heap_total_slots;

    /* Call & Local Variable Stack + Optional Concrete Type/Unit/Const Slot Locks */
    Value       stack[MEM_MAX_CALL_STACK];
    uint8_t     slot_locked_type[MEM_MAX_CALL_STACK];
    uint8_t     slot_locked_unit[MEM_MAX_CALL_STACK];
    uint8_t     slot_is_const[MEM_MAX_CALL_STACK];
    uint16_t    sp;

    /* Special Bootstrap Type IDs */
    uint8_t     fast_real_type;
    uint8_t     str_type_id;
    uint8_t     vec_type_id;
    uint8_t     mat_type_id;
    uint8_t     fn_type_id;
    uint8_t     map_type_id;
    uint8_t     type_stride[64];

    /* Control-Flow Jump, Break, Continue, Return & Tagged Scope Runtime State */
    uint8_t     jump_active;       /* JumpKind (JUMP_NONE..JUMP_RETURN)          */
    uint8_t     jump_target_id;
    Value       jump_val;          /* Payload for `break expr` / `return expr`   */
    uint16_t    label_ast[MEM_MAX_LABELS];

    /* Lambda source slices for module variable export */
    uint16_t    fn_body_ast[64];
    StrSlice    fn_src_slice[64];
    uint8_t     fn_src_count;
    uint8_t     perm_fn_src_count;

    /* Base-Layer (`stdlib + patches`) Arena Watermarks for O(1) User-Module Flush */
    uint16_t    base_src_len;
    uint16_t    base_ast_count;
    uint16_t    base_pool_top;
    uint8_t     base_fn_src_count;
    uint8_t     base_fast_real_type;
    uint8_t     base_str_type_id;
    uint8_t     base_vec_type_id;
    uint8_t     base_mat_type_id;
    uint8_t     base_fn_type_id;
    uint8_t     base_map_type_id;

    /* Local variable slot names for symbolic math & introspection */
    StrSlice    local_slot_names[64];

    /* Owning SymTable back-pointer for module-variable GC root tracing & active module context */
    struct SymTable *sym_tab;
    uint16_t         cur_mod;
    Value            last_mod_val;

    /* Scratch Pool Mark-Sweep / Sliding Compaction State, String Hash & Active Closure Index */
    Value      *temp_roots[MEM_MAX_TEMP_ROOTS];
    uint16_t    temp_root_count;
    uint8_t     gc_mark[MEM_MAX_F64_POOL / 2];      /* 4,096 B: packed 4-bit mark nibbles per slot */
    StrSlice    str_hash[MEM_STR_HASH_BUCKETS];     /* 2,048 B: O(1) interned string lookup table  */
    uint8_t     active_closure_hids[MEM_MAX_ACTIVE_CLOSURES];
    uint8_t     active_closure_count;
    uint8_t     closure_list_overflow;
    uint16_t    gc_fwd[MEM_MAX_F64_POOL];

    /* Scratch range metadata cache for `@vrange` -> `@vrange_step` (`a...b by step`) */
    uint16_t    vrange_hdr[4];
    uint8_t     vrange_unit[4];
    uint8_t     vrange_seg_cnt[4];
    uint8_t     vrange_mode[4][8];
    double      vrange_a[4][8];
    double      vrange_b[4][8];
    uint8_t     vrange_cnt;

    /* Recursion & Global Step Budget State */
    uint16_t    eval_depth;
    uint16_t    call_depth;
    uint32_t    eval_steps;

    /* Phase 5: Standard Output Capture Buffer & Module/File Import State */
    char        out_buf[MEM_OUT_BUF_CAP];
    uint16_t    out_len;
    char        import_dir[128];
    char        imported_files[16][64];
    uint8_t     imported_file_count;


    /* Error state */
    bool        has_error;
    char        err_msg[128];
} MemArena;

/* ============================================================================
 * 4. MEMORY LIFECYCLE, POOL ALLOCATION & ERROR API
 * ============================================================================ */

static inline const AstNode *mem_ast_base(const MemArena *mem) {
    return (mem->heap_ast_magic == MEM_HEAP_AST_MAGIC && mem->heap_ast != NULL)
           ? mem->heap_ast
           : mem->sbo_ast;
}

static inline AstNode *mem_ast_base_mut(MemArena *mem) {
    return (mem->heap_ast_magic == MEM_HEAP_AST_MAGIC && mem->heap_ast != NULL)
           ? mem->heap_ast
           : mem->sbo_ast;
}

void     mem_init(MemArena *mem);
void     mem_free_all_heap(MemArena *mem);
void     mem_free_ast_heap(MemArena *mem);
bool     mem_ensure_ast_cap(MemArena *mem, uint16_t needed_cap);
void     mem_err(MemArena *mem, const char *fmt, ...);
void     mem_err_attach_line(MemArena *mem, int line);
void     mem_clear_err(MemArena *mem);
void     mem_out_append(MemArena *mem, const char *s, size_t len);
void     mem_out_clear(MemArena *mem);

/* Snapshot permanent watermarks (after loading extensions) & rewind scratch per eval */
void     mem_snapshot_perm(MemArena *mem);
void     mem_snapshot_base(MemArena *mem);
void     mem_rewind_to_base(MemArena *mem);
void     mem_rewind_scratch(MemArena *mem);

/* UTF-8 String Interning & Comparison */
StrSlice mem_intern_str(MemArena *mem, const char *str, uint16_t len);
void     mem_rebuild_str_hash(MemArena *mem);
const char *mem_str_ptr(const MemArena *mem, StrSlice s);
bool     mem_slice_eq(const MemArena *mem, StrSlice a, StrSlice b);
bool     mem_slice_eq_cstr(const MemArena *mem, StrSlice s, const char *cstr);

/* Flat Float Pool Allocation & Access (returns `MEM_NO_IDX` on failure) */
uint16_t mem_alloc_pool(MemArena *mem, uint16_t count);
double   mem_read_pool(const MemArena *mem, uint16_t idx);
void     mem_write_pool(MemArena *mem, uint16_t idx, double val);

/* Unified SBO Arena / Heap Payload Accessors (transparently handles `MEM_HEAP_IDX_BIT`) */
static inline bool mem_is_heap_idx(uint16_t data_idx) {
    return (data_idx & MEM_HEAP_IDX_BIT) != 0 && data_idx != MEM_NO_IDX;
}
uint16_t      mem_heap_alloc_block(MemArena *mem, uint16_t owner_hdr, uint32_t slots);
void          mem_compact_strings(MemArena *mem);
double       *mem_data_ptr(MemArena *mem, uint16_t data_idx, uint32_t offset);
const double *mem_data_ptr_ro(const MemArena *mem, uint16_t data_idx, uint32_t offset);
double        mem_data_read(const MemArena *mem, uint16_t data_idx, uint32_t offset);
void          mem_data_write(MemArena *mem, uint16_t data_idx, uint32_t offset, double val);

/* Dynamic Vector Header & Resizable Payload Management (`$vec` of any `T`) */
uint16_t mem_vec_alloc_hdr(MemArena *mem, uint16_t elem_tid, uint32_t init_cap);
bool     mem_vec_reserve(MemArena *mem, uint16_t hdr_idx, uint16_t stride, uint32_t min_cap);
void     mem_vec_shrink_to_fit(MemArena *mem, uint16_t hdr_idx, uint16_t stride);

/* Multidimensional Matrix (`$mat`) Header & Contiguous Payload Allocation */
uint16_t mem_mat_alloc_hdr(MemArena *mem, uint8_t rank, const uint16_t *dims,
                           uint16_t *out_total_len, uint16_t *out_data_idx);

/* Dynamic Map / Dictionary (`$map`) Header & Key-Value Entry Management */
uint16_t mem_map_alloc_hdr(MemArena *mem, uint16_t packed_val_type, uint16_t init_cap);
bool     mem_map_reserve(MemArena *mem, uint16_t hdr_idx, uint16_t min_cap);
StrSlice mem_map_entry_key(const MemArena *mem, uint16_t data_idx, uint16_t entry_i);
Value    mem_map_entry_val(const MemArena *mem, uint16_t data_idx, uint16_t entry_i);
void     mem_map_set_entry(MemArena *mem, uint16_t data_idx, uint16_t entry_i,
                           StrSlice key, Value val);

/* Container Immutability Helpers (`const` / immutable module bindings) */
void     mem_val_mark_immutable(MemArena *mem, Value v);
bool     mem_val_is_immutable(const MemArena *mem, Value v);

/* Arena Scratch-Pool & Heap-Block Mark-Sweep / Sliding Defragmentation Compaction */
uint16_t mem_compact_scratch(MemArena *mem, Value *extra_roots, uint16_t extra_root_count);
uint16_t mem_scratch_pool_used(const MemArena *mem);
uint16_t mem_push_temp_root(MemArena *mem, Value *vptr);
void     mem_pop_temp_roots(MemArena *mem, uint16_t saved_count);

/* AST Node Pool Allocation & Access */
uint16_t mem_alloc_ast(MemArena *mem, uint8_t kind, uint8_t sub,
                       uint16_t ref, uint16_t left, uint16_t right);
const AstNode *mem_get_ast(const MemArena *mem, uint16_t idx);
AstNode       *mem_get_ast_mut(MemArena *mem, uint16_t idx);

/* Call Stack Frame Management */
uint16_t mem_push_frame(MemArena *mem, uint8_t slot_count);
void     mem_pop_frame(MemArena *mem, uint16_t frame_base);
Value    mem_get_slot(const MemArena *mem, uint16_t frame_base, uint8_t slot);
void     mem_set_slot(MemArena *mem, uint16_t frame_base, uint8_t slot, Value v);

/* ============================================================================
 * 5. VALUE CONSTRUCTION & INSPECTION API (ENTITY TYPE + UNIT TYPE)
 * ============================================================================ */

Value    val_void(void);
Value    val_make_f64(MemArena *mem, uint16_t type_id, double d);
Value    val_make_f64_unit(MemArena *mem, uint16_t type_id, uint8_t unit_id, double d);
Value    val_make_f64_at_pool(const MemArena *mem, uint16_t type_id, uint8_t unit_id, uint16_t pool_idx);
double   val_get_f64(const MemArena *mem, Value v);
Value    val_make_str(MemArena *mem, uint16_t type_id, StrSlice s);
StrSlice val_get_str(const MemArena *mem, Value v);
Value    val_make_comp(MemArena *mem, uint16_t type_id, uint16_t pool_idx, uint16_t flat_size);
uint16_t val_get_pool_idx(Value v);
uint16_t val_get_type(const MemArena *mem, Value v);
uint8_t  val_get_unit(const MemArena *mem, Value v);
Value    val_with_unit(MemArena *mem, Value v, uint8_t unit_id);
Value    val_clone_comp(MemArena *mem, Value v, uint16_t flat_size);

/* Host Callback Convenience Helpers */
Value    val_from_f64(MemArena *mem, double d);
Value    val_from_cstr(MemArena *mem, const char *cstr);
Value    val_from_f64_vec(MemArena *mem, const double *vals, uint16_t count);
bool     val_as_cstr(const MemArena *mem, Value v, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MEM_ARENA_H */
