# Math-Engine-MEP C11 Architecture, Memory Model & API Reference

---

## 1. Engine Architecture & Boot Pipeline

`Math-Engine-MEP` separates its immutable C11 execution mechanisms from its `.math`-defined language grammar:

1. **Blank-Slate Initialization (`eng_init_blank` / `eng_init`)**:
   - Initializes `Engine` (`MemArena` + `SymTable`) with zero pre-populated operators, keywords, or types.
2. **O(1) Binary Standard Library Load (`user_load_mathc` / `nativeLoadMathc`)**:
   - Loads `precompiled/stdlib.mathc` (`MTHC` v1 binary image) into the permanent region of `MemArena` and `SymTable` after validating the 48-byte `MathcHeader`, `delegate_intrinsic_table_hash()`, and 32-bit FNV-1a payload checksum.
   - Snapshots the Base-Layer watermarks (`eng_snapshot_base` / `sym_snapshot_base`: `base_mod_count`, `base_type_count`, `base_unit_count`, `base_overload_count`, `base_ast_count`, `base_pool_top`, `base_src_len`, `base_const_count`) so a single `Engine` instance can be recycled in O(1) time across standalone calculations and multi-file projects via `eng_flush_user_modules(eng)`.
3. **Unified Execution (`eng_eval`, `eng_build_and_run`, `eng_mod_eval`)**:
   - **Expression Fast Path**: Pure expressions evaluate directly in the caller's module scope.
   - **Standalone Script Mode (`unit_count == 1`)**: Evaluates all declarations and top-level expressions top-to-bottom, then automatically invokes `@entry(fn)` or `main()` if defined in the script.
   - **Multi-File Project Mode (`unit_count > 1`)**: Topologically sorts all `[module]` units by `<[dep]`, links dependencies in declaration-only mode (`skip_exprs = true`), and executes the single project-wide `@entry(fn)` or `main()` entry point via `eng_run_entry`.

---

## 2. Memory Model: 8-Byte NaN-Boxing, SBO Arena & Sliding GC

### 8-Byte NaN-Boxed `Value` (`Core/Headers/mem_arena.h`)
Every `Value` is an IEEE-754 `uint64_t`:
- **Standard `double` scalars without units (`type_id == fast_real_type`, `unit_id == 0`)**: Stored as raw 64-bit IEEE-754 doubles (canonicalizing signaling NaNs).
- **Tagged values (`0xFFF8000000000000ULL` prefix)**:
  - `NB_TAG_VOID` (`0xFFF8`): `void` sentinel (`val_void()`).
  - `NB_TAG_POOL` (`0xFFF9`): Pool-backed scalar with units, struct, closure (`$fn`), or container (`$vec`, `$mat`, `$map`), encoding `(unit_id << 40) | (type_id << 32) | pool_idx`.
  - `NB_TAG_STR` (`0xFFFA`): Interned UTF-8 string slice in `mem->src_buf`, encoding `(len << 16) | off`.

### Small-Buffer Optimization (SBO) & Heap Spill (`Core/Code/mem_containers.c`)
- Container payloads with `<= 64` doubles (`MEM_SBO_MAX_SLOTS`) allocate directly in `mem->f64_pool` (`8,192` doubles).
- Larger vectors, matrices, or maps (`> 64` doubles or when `f64_pool` is near capacity) allocate a dynamic `HeapBlock` (`MEM_HEAP_IDX_BIT | heap_id`, up to `256,000` doubles per block).
- `mem_compact_scratch` (`Core/Code/mem_gc.c`) performs a unified Mark-Sweep + Sliding Compaction pass over the scratch portion of `f64_pool` and sweeps unreachable `HeapBlock`s.

---

## 3. Public C11 API (`Core/Headers/math_engine.h` & `Core/compile/user_loader.h`)

### Lifecycle & Evaluation
- `void eng_init(Engine *eng)` / `void eng_init_blank(Engine *eng)`
- `Engine *eng_create(void)` / `Engine *eng_create_blank(void)` / `void eng_destroy(Engine *eng)`
- `void eng_free_heap(Engine *eng)`
- `bool eng_eval(Engine *eng, const char *expr, Value *out)`
- `bool eng_build_and_run(Engine *eng, const char *const *files, size_t file_count, Value *out)`
- `bool eng_has_entry(const Engine *eng)` / `bool eng_run_entry(Engine *eng, Value *out)`
- `bool eng_apply_patches(Engine *eng, const char *const *patch_srcs, size_t patch_count)`

### Module-Scoped State & Introspection
- `bool eng_mod_eval(Engine *eng, const char *mod_name, const char *expr, Value *out)`
- `bool eng_mod_reset(Engine *eng, const char *mod_name)`
- `void eng_snapshot_base(Engine *eng)`
- `bool eng_flush_user_modules(Engine *eng)` (O(1) rewind of all user modules, types, units, overloads, variables & heap blocks above the `stdlib + patches` base watermark)
- `uint16_t eng_mod_var_count(const Engine *eng, const char *mod_name)`
- `bool eng_mod_get_var(const Engine *eng, const char *mod_name, const char *var_name, Value *out_val, uint8_t *out_flags)`
- `size_t eng_mod_format_vars(const Engine *eng, const char *mod_name, char *buf, size_t cap)`
- `size_t eng_mod_export_source(const Engine *eng, const char *mod_name, char *buf, size_t cap)`
- `bool eng_set_module(Engine *eng, const char *mod_name, bool enabled)`

### Symbolic CAS & Formatting
- `bool eng_sym_simplify(Engine *eng, const char *expr, char *buf, size_t cap)`
- `bool eng_sym_diff(Engine *eng, const char *var, const char *expr, char *buf, size_t cap)`
- `bool eng_sym_integ(Engine *eng, const char *var, const char *expr, char *buf, size_t cap)`
- `bool eng_sym_solve(Engine *eng, const char *var, const char *eq_expr, char *buf, size_t cap)`
- `char *eng_format_value(const Engine *eng, Value v, char *buf, size_t cap)`

### In-Memory Extension & `.mathc` Loader (`user_loader.h`)
- `bool user_compile_ext(Engine *eng, const char *ext_src, const uint8_t **out_mathc, size_t *out_len)`
- `bool user_load_mathc(Engine *eng, const uint8_t *mathc_buf, size_t len)`
- `ExtBuildCtx *user_build_create(void)` / `void user_build_destroy(ExtBuildCtx *ctx)`
- `bool user_compile_unit(Engine *eng, ExtBuildCtx *ctx, const char *unit_name, const char *ext_src)`
- `bool user_link_ext(Engine *eng, ExtBuildCtx *ctx, const uint8_t **out_mathc, size_t *out_len)`

---

## 4. Android JNI Bridge (`Core/jni/math_engine_jni.c` & `JNI/MathEngineBridge.kt`)

`MathEngineBridge` exposes the C11 engine to Android/JVM hosts:
- `nativeCreate()` / `nativeCreateBlank()` / `nativeDestroy(handle)` / `nativeReset(handle)`
- `nativeLoadMathc(handle, mathcBytes)` / `nativeCompileExt(handle, extSource)` / `nativeApplyPatch(handle, patchSource)`
- `nativeEval(handle, expr)` / `nativeModuleEval(handle, modName, expr)` / `nativeBuildAndRun(handle, sources)` -> returns wire string `"OK|<type>|<unit>|<value>"` or `"ERR|<message>"`
- `nativeFlushUserModules(handle)` -> O(1) user-module flush back to the `stdlib + patches` baseline on a single recycled `Engine*` handle
- `nativeHasEntry(handle)` / `nativeRunEntry(handle)`
- `nativeModuleReset(handle, modName)` / `nativeModuleVarCount(handle, modName)` / `nativeModuleFormatVars(handle, modName)` / `nativeModuleExportSource(handle, modName)`
- `nativeSymSimplify(handle, expr)` / `nativeSymDiff(handle, varName, expr)` / `nativeSymInteg(handle, varName, expr)` / `nativeSymSolve(handle, varName, eqExpr)`
- `nativeRegisterExtern(handle, externName, callback)` / `nativeGetOutput(handle)` / `nativeClearOutput(handle)` / `nativeSetSandboxRoot(handle, rootDir)`


## Recent Engine Upgrades (Base-Layer Watermarks, SBO+Heap AST/Overloads, Batch Plotter & Live Catalog)

1. **Single Recyclable `Engine*` via Base-Layer Watermarks (`eng_snapshot_base` & `eng_flush_user_modules`)**
   - After loading `precompiled/stdlib.mathc` and applying global OTA `.math` patches (`eng_apply_patches`), the engine records base-layer watermarks across `SymTable` and `MemArena`.
   - `eng_flush_user_modules(eng)` (`nativeFlushUserModules` over JNI) rewinds all user project modules, types, ephemeral 7D SI units, variables, AST nodes, and overloads back to the `stdlib + patches` baseline in $O(1)$ time (`< 5 µs`) without reloading `stdlib.mathc` or reallocating `Engine*`.
2. **Unbounded SBO + Dynamic Heap Growth for `ast` & `overloads`**
   - `MemArena` keeps `MEM_SBO_AST_NODES = 24,576` AST nodes inline (zero heap allocations for `stdlib.mathc` + normal scripts) and transparently spills to `heap_ast` up to `MEM_MAX_AST_NODES = 65,534` (`0xFFFE`) for large multi-file projects.
   - `SymTable` keeps `SYM_SBO_OVERLOADS = 2,048` operator/function overloads inline and transparently spills to `heap_overloads` up to `SYM_MAX_OVERLOADS = 65,534`, reclaiming heap buffers automatically on `eng_flush_user_modules` and `eng_destroy`.
3. **Native C11 Batch Curve Sampler (`eng_sample_plot` / `nativeSamplePlot`)**
   - Parses a plotted expression, 1-arg function, `vec<real>`, or `vec<vec2>` trajectory once into a temporary AST and evaluates all `N` sample points across `[x_min, x_max]` in a single C loop, returning a flat `double[]` (`[x0, y0, x1, y1, ...]`) over a single JNI call.
4. **Live `SymTable` Catalog Export (`eng_export_catalog` / `nativeExportCatalog`)**
   - Exports all active functions, operators, units, types/structs, constants, and modules directly from the live C `SymTable` (`kind|name|sig|retType|retUnit|modName`) so host IDEs and editors automatically autocomplete newly loaded OTA `.math` patches and linked project modules.
5. **Wider Unit Registry & 1 MiB `.mathc` Images (`SYM_MAX_UNITS = 128`, `MATHC_MAX_IMAGE_BYTES = 1 MiB`)**
   - `Core/Headers/sym_table.h`: `SYM_MAX_UNITS` raised `64` -> `128`; owned named unit-group slots are now `1..111`, and `SYM_EPHEM_UNIT_BASE` moved `48` -> `112` (ephemeral 7D SI compound units occupy slots `112..127`). All call sites route through these two defines (sym_table, delegate_core, delegate_numeric, mathc_bin), so no other code changed.
   - `Core/compile/mathc_bin.h`: `MATHC_MAX_IMAGE_BYTES` raised `524288` -> `1048576`; the cumulative `.mathc` image (stdlib + every linked patch) now caps at 1 MiB.
   - Motivation: the 55-currency OTA patch family (base `currency.math` + generated `currency_pairs_1..6.math`) owns all 55 currency unit groups and spells the full 55×55 `into` matrix (~3,000 additional overloads; total load lands ~89 unit groups / ~4,563 overloads, comfortably inside the SBO+heap overload table). `precompiled/stdlib.mathc` stays **byte-identical** — group *count* and content are unchanged, so nothing needed a re-seat.

6. **Compact 1-Byte `TypeSpec` Encoding for Static Element Typing & Multiple Dispatch (`Core/Headers/sym_table.h`)**
   - Parameterized container types (`vec<T>`, `map<T>`, `mat<T>`) are encoded directly inside the existing 1-byte (`uint8_t`) and 2-byte (`uint16_t`) `TypeSpec` fields using high-bit container tags (`SYM_TY_VEC_BIT = 0x40`, `SYM_TY_MAP_BIT = 0x80`, `SYM_TY_MAT_BIT = 0xC0`) paired with a 6-bit element type ID (`0x01..0x3C` for concrete types, `SYM_ELEM_ANY = 0x3D` for `<any>` / `<T>`).
   - Preserves exact binary layout and zero memory bloat: `sizeof(Value) == 8`, `sizeof(AstNode) == 8`, `sizeof(FieldDef) == 8`, `sizeof(OpOverload) == 46`, and `sizeof(MathcHeader) == 48`.
   - Overload dispatch (`Core/Code/eval_dispatch.c`) scores exact parameterized element matches (`vec<vec2>` on `vec<vec2>`) at `+8`, default `vec<real>` matches on empty unlocked `vec()` at `+7`, explicit wildcard `vec<T>` at `+6`, and unparameterized `vec` at `+4`.


## Balanced Memory & Runtime Performance Architecture

1. **Lossless Inline 32-Bit NaN-Boxing (`NB_TAG_I32 = 0xFFFB`, `NB_TAG_F32 = 0xFFFC`)**:
   - In addition to unboxed unitless `real` (`double`), any exact signed 32-bit integer (`[-2^31, 2^31 - 1]`) or lossless IEEE-754 `binary32` scalar (`(double)(float)d == d`) with an entity type (`int`, `real`, etc.) and/or physical unit (`m`, `s`, `kg`, etc.) is encoded directly inside the low 32 bits of the 8-byte `Value` without allocating a slot in `MemArena.f64_pool`.
2. **Packed 4-Bit `gc_mark` Nibbles & O(1) String Interning Cache (`str_hash[512]`)**:
   - `MemArena.gc_mark` packs two 4-bit mark tags (`0..5`) per byte (`4,096 B` instead of `8,192 B`), while `MemArena.str_hash[512]` (`2,048 B`) memoizes interned `StrSlice` entries with `memchr`-accelerated substring lookup, reducing `sizeof(MemArena)` by `2,032 B` net while accelerating string and symbol interning.
3. **Frame-Free Direct Intrinsic Bypass & Lazy Label Scanning (`SYM_OV_HAS_LABELS`)**:
   - Single-node `AST_INTRINSIC` operator and stdlib wrappers execute directly via `delegate_eval_intrinsic()` without allocating call frames or scanning ASTs for tagged scopes; user functions cache `SYM_OV_LABELS_CHECKED | SYM_OV_HAS_LABELS` on first call.
4. **Active Closure Environment Tracking (`active_closure_hids[16]`)**:
   - Tracks active-frame lexical closure `HeapBlock` IDs directly so local variable stores and `mem_pop_frame()` skip 256-slot `heap_blocks` scans when no closure upvalues are active.
