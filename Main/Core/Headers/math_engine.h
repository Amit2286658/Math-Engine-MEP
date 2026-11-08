/*
 * math_engine.h — Top-Level Engine Wrapper & Module Execution API
 *
 * Architecture Overview:
 *   User Program (`Test/main.c` / Android JNI Host)
 *        │
 *        ├──► [Core/Headers + Core/Code]  (Core Engine Pipeline)
 *        │       ├──► [math_engine.h/.c]  Top-Level Engine Wrapper, Module Eval & Introspection
 *        │       ├──► [mem_arena.h/.c]    Isolated Memory Arena & 8B NaN-Boxed Values
 *        │       ├──► [sym_table.h/.c]    Dynamic Types, Units, Structs, Modules, Overloads, Vars
 *        │       ├──► [lexer.h/.c]        Pattern-Driven Consuming Types & Syntax Tokens
 *        │       ├──► [parser.h/.c]       Top-Down Operator Precedence Pratt Parser
 *        │       └──► [evaluator.h/.c]    Type- & Unit-Dispatched AST Evaluator
 *        │
 *        ├──► [Core/compile]              (Compilation, Linking, `.mathc` Binary & User Loader)
 *        │       ├──► [compiler.h/.c]     Module Unit Stager & Top-to-Bottom Module Evaluator
 *        │       ├──► [linker.h/.c]       Topological `<[dep]` Dependency Linker
 *        │       ├──► [mathc_bin.h/.c]    `.mathc` Binary Serializer, Validator & Loader
 *        │       ├──► [user_loader.h/.c]  Host-Facing User Extension Compile/Link/Load API
 *        │       └──► [ext_loader.h]      Umbrella Header for `compiler`, `linker`, `mathc_bin`
 *        │
 *        └──► [Core/lib]                  (Delegates & Hardcoded Grammar Rules)
 *                ├──► [delegate/delegate.h/.c]  C `@<intrinsic>` Implementations
 *                └──► [hardcode/hardcode.h/.c]  Isolated Hardcoded Grammar & Ergonomics
 */

#ifndef MATH_ENGINE_H
#define MATH_ENGINE_H

#ifndef MATH_ENGINE_API
#  if defined(_WIN32) && defined(MATH_ENGINE_SHARED)
#    define MATH_ENGINE_API __declspec(dllexport)
#  elif defined(__GNUC__) || defined(__clang__)
#    define MATH_ENGINE_API __attribute__((visibility("default")))
#  else
#    define MATH_ENGINE_API
#  endif
#endif

#include "mem_arena.h"
#include "sym_table.h"
#include "lexer.h"
#include "parser.h"
#include "ext_loader.h"
#include "evaluator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified Engine Context combining Arena, Symbol/Module Registry, and
 * Per-Engine Extension Build/Image Buffer (zero shared global/static state).
 * Note: Because `Engine` is large, allocate it on the heap via `eng_create()` /
 * `eng_destroy()` (or in static storage) rather than on a small thread stack.
 */
typedef struct Engine {
    MemArena    mem;
    SymTable    sym;
    uint8_t     mathc_buf[MATHC_MAX_IMAGE_BYTES];
} Engine;

/* ============================================================================
 * 1. CORE ENGINE LIFECYCLE, MODULE TOGGLING & STATELESS EVALUATION
 * ============================================================================ */

/*
 * Heap-allocate & initialize `Engine`. Returns an EMPTY engine: the host is
 * responsible for loading `precompiled/stdlib.mathc` (`user_load_mathc`) or
 * a user image (`user_load_mathc` extensions) before evaluating.
 * Recommended for Android NDK / JNI and multi-threaded hosts so `Engine`
 * never lives on a thread stack.
 */
MATH_ENGINE_API Engine     *eng_create(void);

/* Heap-allocate & initialize a blank-slate `Engine`. Equivalent to
 * `eng_create()`; the host loads `stdlib.mathc` via `user_load_mathc`.
 */
MATH_ENGINE_API Engine     *eng_create_blank(void);
MATH_ENGINE_API void        eng_destroy(Engine *eng);

/*
 * Initialize caller-provided `Engine` storage as a BLANK engine (no standard
 * library). The host loads `precompiled/stdlib.mathc` afterwards (O(1)
 * `memcpy` boot via `user_load_mathc` / JNI `nativeLoadMathc`) before eval.
 */
MATH_ENGINE_API void        eng_init(Engine *eng);

/*
 * Initialize a completely blank-slate Engine (zero built-in types or operations).
 * Used internally by `mathc` or for testing the blank-slate guarantee.
 */
MATH_ENGINE_API void        eng_init_blank(Engine *eng);

/*
 * Register a platform-provided C callback (`fn`) under `@<name>(...)` so `.math`
 * extensions can safely invoke host services without coupling `Core/` to OS libraries.
 */
MATH_ENGINE_API bool        eng_register_extern(Engine *eng, const char *name,
                                                EngExternFn fn, void *user_ctx);
MATH_ENGINE_API bool        eng_register_extern_full(Engine *eng, const char *name,
                                                     EngExternFn fn, void *user_ctx,
                                                     void (*fn_free)(void *user_ctx));
/* Runs `fn_free(user_ctx)` for every registered extern that has a destructor,
 * then empties the extern table. Called by `eng_destroy`; may also be called
 * manually to recycle an engine's extern table without freeing the engine. */
MATH_ENGINE_API void        eng_free_externs(Engine *eng);

/* Enable or disable any loaded module in O(1) */
MATH_ENGINE_API bool        eng_set_module(Engine *eng, const char *mod_name, bool enabled);
MATH_ENGINE_API bool        eng_is_module_active(const Engine *eng, const char *mod_name);

/* Stateless one-shot evaluation: parse and evaluate an expression string */
MATH_ENGINE_API bool        eng_eval(Engine *eng, const char *expr, Value *out_val);

/* ============================================================================
 * 2. TOP-TO-BOTTOM MODULE EXECUTION, RESET & VARIABLE INTROSPECTION
 * ============================================================================ */

/*
 * Execute `.math` source code (declarations, module variables, constants, and
 * top-to-bottom statements/expressions) inside module `mod_name` (defaults to
 * `"default"` if NULL or empty). Creates `[mod_name]` automatically if not yet
 * present, updates module variables in-place, and writes the final expression
 * result to `*out_val` (if non-NULL).
 */
MATH_ENGINE_API bool        eng_mod_eval(Engine *eng, const char *mod_name,
                                         const char *src, Value *out_val);

/*
 * Reset all module variables, constants, and overloads owned by `mod_name`
 * (without affecting other loaded modules) and reclaim unreferenced scratch memory.
 */
MATH_ENGINE_API bool        eng_mod_reset(Engine *eng, const char *mod_name);
MATH_ENGINE_API void        eng_snapshot_base(Engine *eng);
MATH_ENGINE_API bool        eng_flush_user_modules(Engine *eng);
MATH_ENGINE_API uint16_t    eng_sample_plot(Engine *eng, const char *mod_name,
                                            const char *expr, double x_min,
                                            double x_max, uint16_t steps,
                                            double *out_xy, uint16_t max_pairs);
MATH_ENGINE_API size_t      eng_export_catalog(const Engine *eng, char *out_buf,
                                               size_t buf_cap);

/*
 * Inspect and manipulate module-scoped variables and constants owned by `mod_name`:
 */
MATH_ENGINE_API uint16_t    eng_mod_var_count(const Engine *eng, const char *mod_name);
MATH_ENGINE_API bool        eng_mod_var_info(const Engine *eng, const char *mod_name,
                                             uint16_t idx, char *name_buf, size_t name_cap,
                                             Value *out_val, uint16_t *out_lock_type,
                                             uint8_t *out_lock_unit, uint8_t *out_flags);
MATH_ENGINE_API bool        eng_mod_get_var(const Engine *eng, const char *mod_name,
                                            const char *var_name, Value *out_val,
                                            uint8_t *out_flags);
MATH_ENGINE_API bool        eng_mod_set_var(Engine *eng, const char *mod_name,
                                            const char *var_name, Value val,
                                            uint8_t flags);

/*
 * Format all variables/constants in `mod_name` as newline-separated records:
 *   `<name>|<type>|<unit>|<mutability>|<visibility>|<formatted_value>`
 */
MATH_ENGINE_API size_t      eng_mod_format_vars(const Engine *eng, const char *mod_name,
                                                char *out_buf, size_t buf_cap);

/*
 * Export all variables and constants of `mod_name` as valid, self-contained,
 * top-to-bottom executable `.math` module source text (`[mod_name]\n...`)
 * that can be saved or re-evaluated via `eng_mod_eval` / `user_compile_ext`.
 */
MATH_ENGINE_API size_t      eng_mod_export_source(Engine *eng, const char *mod_name,
                                                  char *out_buf, size_t buf_cap);

/* ============================================================================
 * 3. VALUE FORMATTING & TYPE / UNIT / ERROR INTROSPECTION
 * ============================================================================ */

/* Format any Value (primitive, string, vector, matrix, or nested struct) to a C string */
MATH_ENGINE_API void        eng_format_value(const Engine *eng, Value val, char *buf, size_t buf_cap);

/* Format a multidimensional matrix at `hdr_idx` */
MATH_ENGINE_API void        eng_format_mat_at(const Engine *eng, uint16_t hdr_idx, bool high_prec,
                                              char *buf, size_t cap);

/* Inspect Value Entity Type, Unit Type & Engine error state */
MATH_ENGINE_API uint16_t    eng_val_type(const Engine *eng, Value v);
MATH_ENGINE_API uint8_t     eng_val_unit(const Engine *eng, Value v);
MATH_ENGINE_API const char *eng_type_name(const Engine *eng, uint16_t type_id, char *buf, size_t cap);
MATH_ENGINE_API const char *eng_unit_name(const Engine *eng, uint8_t unit_id, char *buf, size_t cap);
MATH_ENGINE_API const char *eng_last_error(const Engine *eng);

/* Host Registry & Module Introspection Helpers */
MATH_ENGINE_API uint16_t    eng_module_count(const Engine *eng);
MATH_ENGINE_API const char *eng_module_name(const Engine *eng, uint16_t mod_id, char *buf, size_t cap);
MATH_ENGINE_API uint16_t    eng_overload_count(const Engine *eng);
MATH_ENGINE_API uint8_t     eng_const_count(const Engine *eng);

/* Scratch Arena Defragmentation / Mark-Sweep Compaction & Symbolic Helpers */
MATH_ENGINE_API uint16_t    eng_compact_scratch(Engine *eng);
MATH_ENGINE_API uint16_t    eng_scratch_pool_used(const Engine *eng);
MATH_ENGINE_API bool        eng_sym_simplify(Engine *eng, const char *expr, char *out_buf, size_t buf_cap);
MATH_ENGINE_API bool        eng_sym_diff(Engine *eng, const char *var_name, const char *expr,
                                         char *out_buf, size_t buf_cap);
MATH_ENGINE_API bool        eng_sym_integ(Engine *eng, const char *var_name, const char *expr,
                                          char *out_buf, size_t buf_cap);
MATH_ENGINE_API bool        eng_sym_solve(Engine *eng, const char *var_name, const char *eq,
                                          char *out_buf, size_t buf_cap);

/* Standard I/O Capture, Module/File Imports & Unified Project Execution API */
MATH_ENGINE_API const char *eng_get_output(const Engine *eng);
MATH_ENGINE_API void        eng_clear_output(Engine *eng);
MATH_ENGINE_API void        eng_set_import_dir(Engine *eng, const char *dir_path);
MATH_ENGINE_API bool        eng_import_file(Engine *eng, const char *file_path);
MATH_ENGINE_API bool        eng_has_entry(const Engine *eng);
MATH_ENGINE_API bool        eng_run_entry(Engine *eng, Value *out_val);
MATH_ENGINE_API bool        eng_build_and_run(Engine *eng, const char *const *sources,
                                              size_t unit_count, Value *out_val);
MATH_ENGINE_API bool        eng_apply_patches(Engine *eng, const char *const *patch_sources,
                                              size_t patch_count);

#ifdef __cplusplus
}
#endif

#endif /* MATH_ENGINE_H */
