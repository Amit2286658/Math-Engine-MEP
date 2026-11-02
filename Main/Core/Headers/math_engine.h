/*
 * math_engine.h — Top-Level Engine Wrapper (Auto-Boots Precompiled Standard Library)
 *
 * Architecture Overview:
 *   User Program (`Test/main.c`)
 *        │
 *        ├──► [Core/Headers + Core/Code]  (Core Engine Pipeline)
 *        │       ├──► [math_engine.h/.c]  Top-Level Engine Wrapper & Stateless Eval
 *        │       ├──► [mem_arena.h/.c]    Isolated Memory Arena & 8B NaN-Boxed Values
 *        │       ├──► [sym_table.h/.c]    Dynamic Types, Units, Structs, Modules, Overloads
 *        │       ├──► [lexer.h/.c]        Pattern-Driven Consuming Types & Syntax Tokens
 *        │       ├──► [parser.h/.c]       Top-Down Operator Precedence Pratt Parser
 *        │       └──► [evaluator.h/.c]    Type- & Unit-Dispatched AST Evaluator
 *        │
 *        ├──► [Core/compile]              (Compilation, Linking, `.mathc` Binary & User Loader)
 *        │       ├──► [compiler.h/.c]     Module Unit Stager & Declaration Compiler
 *        │       ├──► [linker.h/.c]       Topological `<[dep]` Dependency Linker
 *        │       ├──► [mathc_bin.h/.c]    `.mathc` Binary Serializer, Validator & Loader
 *        │       ├──► [user_loader.h/.c]  Host-Facing User Extension Compile/Link/Load API
 *        │       └──► [ext_loader.h]      Umbrella Header for `compiler`, `linker`, `mathc_bin`
 *        │
 *        ├──► [Core/session]              (Parallel In-Memory Sessions & Snapshots)
 *        │       ├──► [session.h/.c]      Parallel Session Lifecycle & `eng_session_eval`
 *        │       └──► [snapshot.h/.c]     Order-Tagged Raw-Text Snapshots & `eng_session_restore`
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
#include "session.h"
#include "snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified Engine Context combining Arena, Symbol Registry, Parallel Sessions,
 * and Per-Engine Extension Build/Image Buffers (zero shared global/static state).
 * Note: Because `Engine` is large, allocate it on the heap via `eng_create()` /
 * `eng_destroy()` (or in static storage) rather than on a small thread stack.
 */
struct Engine {
    MemArena    mem;
    SymTable    sym;
    EngSession  sessions[ENG_MAX_SESSIONS];
    uint8_t     mathc_buf[MATHC_MAX_IMAGE_BYTES];
};

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
 * 2. VALUE FORMATTING & TYPE / UNIT / ERROR INTROSPECTION
 * ============================================================================ */

/* Format any Value (primitive, string, vector, matrix, or nested struct) to a C string */
MATH_ENGINE_API void        eng_format_value(const Engine *eng, Value val, char *buf, size_t buf_cap);

/* Format a multidimensional matrix at `hdr_idx` (shared with `Core/session/snapshot.c`) */
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

#ifdef __cplusplus
}
#endif

#endif /* MATH_ENGINE_H */
