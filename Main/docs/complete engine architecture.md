# Math Engine (MEP) — Complete Engine Architecture Reference

## 1. Executive Summary & Architectural Philosophy

The **Math Engine (MEP)** is a self-contained, zero-external-dependency C11 scientific, symbolic, and dimensional computation engine designed for deterministic execution across desktop, server, embedded, and mobile (Android NDK / JNI) environments.

### 1.1 Core Architectural Pillars

1. **Zero Built-In Grammar ("Blank-Slate Core")**:
   An uninitialized engine created via `eng_init_blank(&eng)` or `eng_create_blank()` possesses **zero** built-in types, **zero** operators, **zero** functions, **zero** constants, and **zero** expression delimiters. Even fundamental tokens such as `+`, `-`, `(`, `)`, `{`, `}`, `[`, `]`, `,`, `;`, `:=`, `if`, `switch`, `...`, and literal types (`$real`, `$str`, `$iota`) are registered dynamically from `.math` language definition modules (or loaded in $O(1)$ time from a precompiled `.mathc` binary image).
2. **8-Byte IEEE-754 NaN-Boxed `Value` (`sizeof(Value) == 8`)**:
   Every runtime value—whether a 64-bit IEEE-754 floating-point number, an interned string slice, a flat C-style struct (`$vec2`, `$vec3`, `$complex`, `$circle`, `$mat2`), a dynamic vector (`$vec`), a multidimensional tensor/matrix (`$mat`), a hash-indexed dictionary (`$map`), or a first-class lexical closure (`$fn`)—is packed into a single 64-bit unsigned integer (`uint64_t`) carrying orthogonal **Entity Type (`type_id`, 8 bits)** and **Physical Unit (`unit_id`, 8 bits)** tags inside the quiet-NaN payload space.
3. **Strictly 8-Byte AST Nodes (`sizeof(AstNode) == 8`)**:
   Every node in the Abstract Syntax Tree is a compact 8-byte record (`uint8_t kind`, `uint8_t sub`, `uint16_t ref`, `uint16_t left`, `uint16_t right`) indexed by 16-bit arena offsets (`0 .. 24,575`) instead of raw 64-bit pointers. This cuts AST memory footprint by 75% compared to pointer-based trees, guarantees cache-line locality (8 AST nodes per 64-byte L1 cache line), and makes compiled ASTs directly serializable into position-independent `.mathc` binary images without pointer relocation.
4. **Dual-Watermark Arena with Transparent Small-Buffer-Optimized (SBO) Heap Spill & Mark-Sweep GC**:
   All permanent symbols, AST nodes, and constants reside below a permanent watermark (`perm_*`), while per-evaluation allocations happen above the watermark and rewind in $O(1)$ time (`mem_rewind_scratch`). Containers up to `MEM_SBO_MAX_SLOTS` (64 `double` slots = 512 bytes) allocate directly inside `f64_pool` with zero `malloc` overhead; containers exceeding 64 slots transparently spill into managed `HeapBlock` allocations (`MEM_HEAP_IDX_BIT = 0x8000u`) and are automatically reclaimed on scope exit, scratch rewind, or explicit sliding Mark-Sweep compaction (`mem_compact_scratch`).
5. **7-Dimensional SI Dimensional Algebra & Orthogonal Type/Unit Multiple Dispatch**:
   Every value simultaneously carries an entity type (`real`, `iota`, `complex`, `vec`, `mat`, etc.) and a physical unit group (`distance`, `mass`, `time`, `force`, `energy`, `molarity`, `illuminance`, etc.). Unit groups are backed by 7-dimensional SI exponent vectors $(\text{m}, \text{kg}, \text{s}, \text{A}, \text{K}, \text{mol}, \text{cd}) \in \mathbb{Z}^7$, allowing products, quotients, powers, roots, derivatives, integrals, and ODE solvers to synthesize derived physical dimensions automatically at runtime.
6. **Zero Shared Mutable Global State**:
   Every `Engine` instance encapsulates its own `MemArena`, `SymTable`, `EngSession[8]` table, and `mathc_buf` serialization buffer. Multiple threads can safely run independent `Engine` instances concurrently without locks or thread-local storage.

---

## 2. Complete Directory & Source File Layout

```text
MathEngine_Isolated_Core_Lib/
├── CMakeLists.txt                       # CMake build configuration (static/shared lib, CLI, Android NDK)
├── makefile                             # GNU Make build pipeline (Release, Debug+ASan+UBSan, stdlib precompile)
├── Core/
│   ├── Headers/
│   │   ├── math_engine.h                # Top-level Engine struct & public C11 host API
│   │   ├── mem_arena.h                  # Isolated memory arena, 8B NaN-boxed Value, 8B AstNode, SBO/Heap spill
│   │   ├── sym_table.h                  # Dynamic module, type, unit, struct, overload, constant & extern registry
│   │   ├── lexer.h                      # Pattern-driven UTF-8 lexer & token stream definitions
│   │   ├── parser.h                     # Top-down Pratt parser & local variable scope structures
│   │   ├── evaluator.h                  # AST evaluator entry point
│   ├── Core/Headers/                     # (No stdlib_precompiled.h: engine boots blank, stdlib loads separately)
│   ├── Code/
│   │   ├── math_engine.c                # Engine lifecycle, stateless eval, value formatting, symbolic C wrappers
│   │   ├── mem_arena.c                  # NaN-boxing, pool/AST/stack allocators, SBO/Heap spill, Mark-Sweep GC
│   │   ├── sym_table.c                  # Unbounded SBO+heap module registry, O(1) toggling, overload chain indexing
│   │   ├── lexer.c                      # Maximal-munch regex pattern matcher & operator/delimiter tokenizer
│   │   ├── parser.c                     # Pratt expression parser, blocks, control flow, lambdas, destructuring
│   │   └── evaluator.c                  # Type/unit multiple-dispatch scorer, sequence runner, AST evaluator
│   ├── compile/
│   │   ├── ext_loader.h                 # Umbrella header for compilation, linking, and binary loading
│   │   ├── compiler.h / compiler.c      # `.math` module stager, declaration parser, default-arg wrapper synthesizer
│   │   ├── linker.h / linker.c          # Kahn's topological DAG dependency resolver (`<[dep]`)
│   │   ├── mathc_bin.h / mathc_bin.c    # 48-byte `MathcHeader` serializer, FNV-1a checksum & structural validator
│   │   └── user_loader.h / user_loader.c# Public host API for compiling, linking, and loading user `.math`/`.mathc`
│   ├── session/
│   │   ├── session.h / session.c        # 8 parallel stateful sessions (`eng_session_open`, `eng_session_eval`)
│   │   └── snapshot.h / snapshot.c      # Order-tagged `MSP1` raw-text state snapshots & tamper-evident restore
│   ├── jni/
│   │   └── math_engine_jni.c            # Android NDK JNI bridge (`com.mathengine.MathEngineBridge`)
│   └── lib/
│       ├── delegate/
│       │   └── delegate.h / delegate.c  # C `@<intrinsic>` implementations (math, vec, mat, map, regex, CAS)
│       └── hardcode/
│           └── hardcode.h / hardcode.c  # Isolated language ergonomics (ranges, UFCS, pipes, switch, loops)
├── Library/
│   ├── bootstrap.math                   # [boot_syntax], [boot_types], [boot_control]
│   ├── arith.math                       # [arith] Core arithmetic, bitwise, combinatorics, statistics
│   ├── vec.math                         # [vec] Generic `$vec`, `$map`, ranges (`...`, `>..`, `..<`, `>..<`), HOFs
│   ├── string.math                      # [string] String slicing, search, formatting, regex helpers
│   ├── mat.math                         # [mat] N-D tensors, LU, QR, Cholesky, Eigen, SVD, pseudoinverse, lstsq
│   ├── trig.math                        # [trig] Circular, inverse, hyperbolic & special functions
│   ├── complex.math                     # [complex_ext] Rectangular complex numbers (`$iota`, `$complex`)
│   ├── geom.math                        # [geom_ext] `$vec2`, `$vec3`, `$circle`, `$mat2`
│   ├── algebra.math                     # [algebra] Quadratic/polynomial solvers, progressions, finance/work
│   ├── calculus.math                    # [calculus] Numerical & symbolic calculus, multivariable, ODE solvers
│   ├── units.math                       # [units] 7D SI base & derived dimensional unit groups and suffixes
│   ├── conversions.math                 # [conversions] `in` and `to_<unit>` dimensional conversions
│   ├── physics.math                     # [physics] Classical mechanics, relativity, electromagnetism, optics
│   ├── chemistry.math                   # [chemistry] Gas laws, stoichiometry, acid-base pH, electrochemistry
│   ├── biology.math                     # [biology] Hardy-Weinberg, enzyme kinetics, physiology, ecology
│   └── stdlib.mathc                     # Precompiled binary image of all 18 standard library modules
├── JNI/
│   ├── MathEngineBridge.java            # Java JNI wrapper class for Android integration
│   └── CMakeLists.txt                   # Standalone Android NDK shared library build script
├── Test/
│   ├── main.c                           # Standalone CLI, `.mathc` compiler driver & 16-section verification suite
│   ├── user_physics.math                # Sample host-loaded extension module (`[user_physics]`)
│   └── user_physics.mathc               # Precompiled binary image of `user_physics.math`
└── docs/
    ├── complete engine architecture.md  # Complete internal C11 engine architecture reference
    └── comprehensive language guide.md  # Complete language reference with annotated & tested overloads
```

---

## 3. Exact Memory Footprint & Compile-Time Capacity Constants

All core structures are statically sized and verified at compile time via C11 `_Static_assert` checks:

| Structure / Type | Exact Size | Alignment / Layout Verification | Description |
| :--- | :--- | :--- | :--- |
| `Value` | **8 bytes** | `uint64_t` (`sizeof(Value) == 8`) | IEEE-754 NaN-boxed runtime value with 8-bit `type_id` + 8-bit `unit_id` |
| `AstNode` | **8 bytes** | `sizeof(AstNode) == 8` | Compact 5-field AST node (`kind`, `sub`, `ref`, `left`, `right`) |
| `StrSlice` | **4 bytes** | `sizeof(StrSlice) == 4` | Zero-copy UTF-8 string slice (`uint16_t off`, `uint16_t len`) into `src_buf` |
| `PatElem` | **6 bytes** | `sizeof(PatElem) == 6` | Lexer regex pattern atom (`kind`, `quant`, `StrSlice chars`) |
| `FieldDef` | **8 bytes** | `sizeof(FieldDef) == 8` | Struct field descriptor (`StrSlice name`, `type_id`, `flat_offset`) |
| `UnitDimDef` | **8 bytes** | `sizeof(UnitDimDef) == 8` | 7D SI exponent vector `int8_t exp[7]` + `uint8_t has_dim` |
| `TypeDef` | **14 bytes** | `sizeof(TypeDef) == 14` | Registered entity type descriptor (with 16-bit `mod_id`) |
| `OpOverload` | **46 bytes** | `sizeof(OpOverload) == 46` | Operator/function overload signature, precedence, and body AST root (with 16-bit `mod_id`) |
| `ConstDef` | **10 bytes** | `sizeof(ConstDef) == 10` | Global constant descriptor (`sym_id`, 16-bit `mod_id`, `type_id`, `unit_id`, `pool_idx`) |
| `MathcHeader` | **48 bytes** | `sizeof(MathcHeader) == 48` | Little-endian `.mathc` binary header with FNV-1a checksum & `intr_hash` |
| `MemArena` | **362,280 bytes** | `353.79 KB` | Complete string, AST, `f64_pool`, call stack, SBO/Heap & GC state |
| `SymTable` | **124,152 bytes** | `121.24 KB` | Complete unbounded module, type, unit, struct, symbol, overload & extern registry |
| `Engine` | **2,079,904 bytes** | `2,031.16 KB` (~2.03 MB) | Full isolated engine (`MemArena` + `SymTable` + 8 `EngSession`s + `mathc_buf`) |

### 3.1 Arena & Symbol Table Capacity Tunables

| Constant | Value | Purpose |
| :--- | :--- | :--- |
| `MEM_SRC_BUF_CAP` | `49,152` bytes (48 KB) | Interned UTF-8 string and identifier buffer (`MemArena.src_buf`) |
| `MEM_MAX_AST_NODES` | `24,576` nodes (192 KB) | Total 8-byte AST nodes (stdlib uses ~18,499 permanent nodes) |
| `MEM_MAX_F64_POOL` | `8,192` doubles (64 KB) | Flat 64-bit `double` pool for literals, structs, headers, and SBO containers |
| `MEM_MAX_CALL_STACK` | `2,048` slots (16 KB) | Evaluation local variable & function call stack frames |
| `MEM_MAX_LABELS` | `16` labels | Max tagged scopes (`Tag : { ... }`) per scope/function |
| `MEM_MAX_EVAL_DEPTH` | `640` levels | Hard C-stack AST recursion guard (supports 128+ user function recursion levels) |
| `MEM_MAX_EVAL_STEPS` | `2,000,000` steps | Deterministic per-evaluation instruction/AST step budget (prevents infinite loops) |
| `MEM_SBO_MAX_SLOTS` | `64` doubles (512 B) | Threshold up to which `$vec`/`$mat`/`$map` payloads allocate in `f64_pool` |
| `MEM_MAX_HEAP_BLOCKS` | `256` blocks | Max concurrent heap-spilled large containers (`> 64` doubles) |
| `MEM_MAX_HEAP_SLOTS` | `262,144` doubles (2 MB) | Maximum payload capacity for a single heap-spilled container |
| `SYM_SBO_MODULES` | `64` SBO (`Unbounded` heap) | Inline SBO module slots (`0..63`); transparently expands on the heap (`uint16_t mod_id`, up to `65,534` modules) |
| `SYM_MAX_TYPES` | `64` types | Maximum registered entity types (`$real`, `$vec2`, `$complex`, etc.) |
| `SYM_MAX_UNITS` | `64` units | Maximum registered physical unit groups (`distance`, `force`, `energy`, etc.) |
| `SYM_MAX_SYMS` | `2,048` symbols | Maximum distinct operator/function/constant names |
| `SYM_MAX_OVERLOADS` | `2,048` overloads | Maximum registered operator and function overloads |
| `SYM_MAX_CONSTS` | `128` constants | Maximum registered global constants (`π`, `e`, `c0`, `G_const`, etc.) |
| `SYM_MAX_EXTERNS` | `32` externs | Maximum host-registered C callbacks (`@<extern_name>`) |
| `SYM_MAX_OV_PARAMS` | `16` parameters | Maximum parameters per function or lambda overload |

---

## 4. 8-Byte IEEE-754 NaN-Boxed `Value` Representation (`mem_arena.c`)

Every runtime value is represented as a 64-bit integer (`typedef uint64_t Value;`). The engine exploits the IEEE-754 double-precision quiet-NaN bit pattern (`0x7FF8000000000000ULL`) to multiplex unboxed Dimensionless `real` floats and tagged values without heap boxing.

### 4.1 Bit Layout

```text
 63  62          52 51   48 47       40 39       32 31                            0
┌───┬──────────────┬───────┬───────────┬───────────┬───────────────────────────────┐
│ 1 │ 11111111111  │ 1 1 0 │  unit_id  │  type_id  │     32-bit Payload / Index    │
└───┴──────────────┴───────┴───────────┴───────────┴───────────────────────────────┘
  ▲        ▲           ▲         ▲           ▲                     ▲
  │        │           │         │           │                     └── Bits 0..31:
  │        │           │         │           │                         - For boxed scalars/structs/containers:
  │        │           │         │           │                           uint16_t f64_pool index (0..8191)
  │        │           │         │           │                         - For $str (`str_type_id`):
  │        │           │         │           │                           packed `StrSlice` (`(len << 16) | off`)
  │        │           │         │           └── Bits 32..39: 8-bit Entity Type (`type_id`, 1..63)
  │        │           │         └── Bits 40..47: 8-bit Physical Unit (`unit_id`, 0..63)
  │        ┴───────────┴── Bits 48..63: `NANBOX_SIGNATURE = 0xFFF8ULL` (16 bits)
```

### 4.2 Encoding Cases

1. **Fast Unboxed Dimensionless Real (`type_id == fast_real_type` and `unit_id == 0`)**:
   - Any finite or non-tagged IEEE-754 `double` whose top 16 bits `(bits >> 48) != 0xFFF8u` is stored directly as its raw 64-bit IEEE-754 representation!
   - Arithmetic on dimensionless `real` numbers reads and writes raw IEEE-754 `double` bits in registers with **zero** pool allocations.
   - Canonical hardware NaNs that happen to match `0xFFF8` in their top 16 bits are normalized to `0x7FF8000000000000ULL` so they never collide with `NANBOX_SIGNATURE`.
2. **Void / Uninitialized Sentinel (`val_void()`)**:
   - Encoded as `NANBOX_PREFIX | 0ULL` (`type_id = 0`, `unit_id = 0`, payload = `0`).
3. **Interned String Value (`type_id == str_type_id`)**:
   - Encoded directly inside the 32-bit payload without touching `f64_pool`:
     $$\text{payload}_{32} = (\text{slice.len} \ll 16) \mid \text{slice.off}$$
   - String slicing (`"abcdef"[1...4]`) and indexing (`"Planck"[0]`) construct new `StrSlice` views in $O(1)$ time with zero byte copying when slicing contiguous substrings.
4. **Unit-Tagged Scalars & Non-Real Consuming Scalars (`real` with `unit_id != 0`, or `$iota`)**:
   - Allocates 1 `double` slot at `pool_idx` in `MemArena.f64_pool`, writes the `double` magnitude there, and packs `(unit_id << 40) | (type_id << 32) | pool_idx` under `NANBOX_PREFIX`.
5. **Composite Values (Flat Structs, `$vec`, `$mat`, `$map`, `$fn`)**:
   - Packs the starting `pool_idx` of the struct's flat fields or container's header in `f64_pool`, alongside `type_id` and `unit_id` (for unit-tagged structs/matrices like `vec2(3m, 4m)` or `[10m, 20m ; 30m, 40m]`).

---

## 5. Isolated Memory Arena, SBO/Heap Spill & Mark-Sweep Compaction (`mem_arena.c`)

### 5.1 Permanent vs. Scratch Dual-Watermark Lifecycle

`MemArena` divides its three primary pools (`src_buf`, `ast`, `f64_pool`) into two regions separated by watermarks:

```text
  src_buf  : [ 0 .............. perm_src_len  | perm_src_len  .......... src_len   ... 49152 ]
  ast      : [ 0 .............. perm_ast_count| perm_ast_count ......... ast_count ... 24576 ]
  f64_pool : [ 0 .............. perm_pool_top | perm_pool_top .......... pool_top  ...  8192 ]
             ◄────── PERMANENT REGION ────────►◄────────── SCRATCH REGION ───────────────────►
             (stdlib + loaded .math modules)    (rewound in O(1) after every eng_eval call)
```

- **`mem_snapshot_perm(mem)`**: Called after `eng_init` or after loading a `.math` / `.mathc` extension to lock the current `src_len`, `ast_count`, and `pool_top` as the permanent baseline.
- **`mem_rewind_scratch(mem)`**: Called at the start of every `eng_eval` (or after persisting session variables in `eng_session_eval`). Resets `src_len = perm_src_len`, `ast_count = perm_ast_count`, `pool_top = perm_pool_top`, `sp = 0`, and frees any scratch `HeapBlock`s in $O(1)$ time.

### 5.2 Container Header Layouts in `f64_pool`

All four container types store small fixed-size headers in `f64_pool`:

1. **Dynamic Vector (`$vec`, `MEM_VEC_HDR_SLOTS = 4` doubles)**:
   - `hdr[0]`: Packed `(elem_unit_id << 8) | elem_type_id` (`0` if untyped empty vector).
   - `hdr[1]`: Current element count (`len`).
   - `hdr[2]`: Allocated element capacity (`cap`).
   - `hdr[3]`: Payload `data_idx` (either an `f64_pool` offset `0..8191`, or `MEM_HEAP_IDX_BIT | heap_id`).
2. **Multidimensional Matrix/Tensor (`$mat`, `MEM_MAT_HDR_SLOTS = 8` doubles)**:
   - `hdr[0]`: Tensor rank (`1..4`).
   - `hdr[1]`: Total flat element count (`total_len = d0 * d1 * ...`).
   - `hdr[2]`: Payload `data_idx` (SBO `f64_pool` index or `MEM_HEAP_IDX_BIT | heap_id`).
   - `hdr[3]`: Reserved flags (`0`).
   - `hdr[4..7]`: Dimension sizes `dims[0..3]` along axes $0, 1, 2, 3$.
3. **First-Class Closure / Lambda (`$fn`, `MEM_FN_HDR_SLOTS = 4 + 2 * param_cnt` doubles)**:
   - `hdr[0]`: Packed `(req_param_cnt << 16) | body_ast_idx`.
   - `hdr[1]`: Total `param_count` (`0..16`).
   - `hdr[2]`: Packed `(self_slot_plus_1 << 8) | total_local_slots` (enables recursion by binding the closure handle into its own `self_slot` on invocation).
   - `hdr[3]`: Captured lexical `frame_base` in `MemArena.stack`.
   - `hdr[4 .. 4 + 2*param_cnt - 1]`: Per-parameter `(default_expr_ast_idx, local_slot_idx)` pairs.
4. **Dynamic Key-Value Dictionary (`$map`, `MEM_MAP_HDR_SLOTS = 4` doubles)**:
   - `hdr[0]`: Packed value type/unit metadata.
   - `hdr[1]`: Active entry count (`len`).
   - `hdr[2]`: Allocated entry capacity (`cap`).
   - `hdr[3]`: Payload `data_idx` storing `3` doubles per entry (`key_off_len`, `val_lo_u32`, `val_hi_u32`).

### 5.3 Small-Buffer Optimization (SBO) & Transparent Arena-to-Heap Spill

To combine the cache speed of a flat arena with the ability to process large datasets (such as `1...20000` vectors or $120 \times 120$ matrices with $14,400$ elements):

- **SBO Fast Path (`needed_slots <= MEM_SBO_MAX_SLOTS` = 64 doubles)**:
  Payloads up to 64 doubles (and enough remaining room in `f64_pool`) are allocated directly inside `MemArena.f64_pool`.
- **Transparent Heap Spill (`needed_slots > 64` or `f64_pool` full)**:
  Payloads exceeding 64 doubles (up to `MEM_MAX_HEAP_SLOTS = 262,144` doubles = 2 MB per container) allocate a `HeapBlock` slot (`0..255`) backed by `malloc`/`realloc`, and store `data_idx = MEM_HEAP_IDX_BIT | heap_id` (`0x8000u | heap_id`) in the container header.
- **Unified Accessors**:
  `mem_data_ptr`, `mem_data_read`, and `mem_data_write` check `mem_is_heap_idx(data_idx)` in a single bitwise test (`data_idx & 0x8000u`), routing transparently between `mem->f64_pool + data_idx` and `mem->heap_blocks[data_idx & 0x7FFFu].data`.
- **Automatic Growth Migration & Scope Cleanup**:
  When a small SBO vector grows past 64 doubles via `push`, `mem_vec_reserve` automatically migrates its elements from `f64_pool` into a `HeapBlock`. Conversely, `shrink(v)` can migrate a shrunk heap vector back into SBO storage. At the exit of any lexical `{ ... }` scope (`eval_scope_block`) or function call (`eval_user_overload`), `mem_sweep_unreachable_heap(mem, saved_sp)` frees any heap blocks not reachable from surviving stack slots (`0..saved_sp-1`) or temporary C roots.

### 5.4 6-Stage Sliding Mark-Sweep Scratch Compactor (`mem_compact_scratch`)

When long-running loops repeatedly reallocate SBO containers or mutate variables, `mem_compact_scratch` (invoked automatically on pool pressure or explicitly via `gc_compact()`) performs an exact, order-preserving sliding compaction of the scratch region (`perm_pool_top .. pool_top - 1`):

1. **Stage 1 (Clear Marks)**: Clears `gc_mark[]` across `perm_pool_top .. pool_top - 1` and clears `.marked` on all active `HeapBlock`s.
2. **Stage 2 (Root Tracing)**: Recursively traces all live `Value` roots on the evaluation stack (`mem->stack[0 .. sp - 1]`), C temporary roots (`mem->temp_roots`), and extra caller roots (`gc_mark_value`), marking reachable scalar slots, struct blocks, container headers, SBO payloads, and `HeapBlock`s.
3. **Stage 3 (Sweep Unmarked Heap Blocks)**: Frees any `HeapBlock` with `marked == 0`.
4. **Stage 4 (Compute Sliding Forwarding Addresses)**: Scans `perm_pool_top .. pool_top - 1` linearly; each marked slot $i$ receives `gc_fwd[i] = write_cursor++`, preserving monotonic allocation order.
5. **Stage 5 (Relocate Internal Pointers & Roots)**: Updates internal `data_idx` fields inside marked `$vec`, `$mat`, and `$map` headers, updates `HeapBlock.owner_hdr` back-pointers, and rewrites all root `Value` pool indices to their forwarded `gc_fwd[]` addresses.
6. **Stage 6 (Slide Live Doubles)**: Compacts live `double` values downward in `f64_pool` and sets `mem->pool_top = write_cursor`.

---

## 6. Dynamic Symbol, Type, Unit & Overload Registry (`sym_table.c`)

### 6.1 Unbounded SBO+Heap Module Registry & O(1) Toggling

Every declaration in `.math` files belongs to a module (`[default]`, `[boot_syntax]`, `[arith]`, `[vec]`, etc.) assigned a 16-bit `mod_id` (`uint16_t`, supporting up to `65,534` modules with no fixed module count limit):
- **Small-Buffer Optimization (`SYM_SBO_MODULES = 64`)**: The first 64 modules are stored inline inside `SymTable.sbo_mod_names` and `SymTable.sbo_mod_active` with zero heap allocations. When `mod_count` grows beyond 64, `sym_ensure_mod_cap()` transparently doubles capacity on the heap (`heap_mod_names` and `heap_mod_active`).
- **`O(1)` Active-State Check**: `sym_mod_enabled(tab, mod_id)` is a direct single-byte lookup:
  ```c
  return sym_mod_active(tab)[mod_id] != 0;
  ```
- Calling `eng_set_module(eng, "units", false)` flips `sym_mod_active_mut(tab)[mod_id]` in $O(1)$ time, immediately disabling all types, unit suffixes, constants, and operator overloads from `[units]` without touching or recompiling any AST nodes.

### 6.2 Consuming Pattern Types vs. Composite Struct/Container Types

The symbol table supports two categories of entity types (`TypeDef`):

1. **Consuming Lexical Types (`is_consuming == 1`)**:
   Defined in `.math` via regex-like pattern rules (`$real ~ \d+("."\d+)? => @parse_f64`, `$iota ~ \d+("."\d+)?"i" => @parse_f64_trim1`, `$str ~ "\""[^"]*"\"" => @parse_str_slice`).
   - Compiled into a flat sequence of `PatElem` records (`PAT_DIGIT`, `PAT_ALPHA`, `PAT_LIT`, `PAT_CHARS`, `PAT_NOT_CHARS`, `PAT_GROUP_START`, `PAT_GROUP_END`, `PAT_ALT`) with quantifiers (`QUANT_ONE`, `QUANT_PLUS`, `QUANT_STAR`, `QUANT_OPT`).
   - Matched directly by the lexer using backtracking maximal munch.
2. **Non-Consuming Composite & Intrinsic Types (`is_consuming == 0`)**:
   - **Flat C-Style Structs**: Defined via `$vec2 { x:real, y:real }` or nested `$circle { center:vec2, radius:real }`. Compiler recursively flattens nested struct fields (`flat_offset`, `flat_size`) so `c.center.y` resolves at compile time to a single flat offset (`offset = 1`) in `f64_pool`.
   - **Intrinsic Container/Wildcard Types**: Bound to `@vec_type`, `@mat_type`, `@map_type`, `@fn_type`, `@complex_type`, `@any_type` (`$T`, `$any`), and `@none_unit` (`$none`).

### 6.3 7-Dimensional SI Unit System (`UnitDimDef`)

Physical unit groups (`distance`, `mass`, `time`, `current`, `temperature`, `substance`, `luminous_intensity`, `force`, `energy`, `power`, `pressure`, `voltage`, `molarity`, etc.) are declared in `Library/units.math` using `@dim(m, kg, s, A, K, mol, cd)`:

$$\vec{d}(U) = \left(e_{\text{m}},\, e_{\text{kg}},\, e_{\text{s}},\, e_{\text{A}},\, e_{\text{K}},\, e_{\text{mol}},\, e_{\text{cd}}\right) \in [-128, 127]^7$$

When two unit-tagged values $v_a$ (unit $u_a$) and $v_b$ (unit $u_b$) interact via multiplication, division, power, or root:
- **Multiplication (`*`)**: $\vec{d}_{\text{res}} = \vec{d}(u_a) + \vec{d}(u_b)$
- **Division (`/`)**: $\vec{d}_{\text{res}} = \vec{d}(u_a) - \vec{d}(u_b)$
- **Power (`^ k`)**: $\vec{d}_{\text{res}} = k \cdot \vec{d}(u_a)$
- **Root (`sqrt`, `cbrt`)**: $\vec{d}_{\text{res}} = \frac{1}{n} \vec{d}(u_a)$ (when all 7 exponents are divisible by $n$)

`delegate_combine_si_units` looks up the resulting 7-tuple in `tab->unit_dims[1 .. unit_count - 1]`:
- If all 7 exponents are zero ($\vec{d}_{\text{res}} = \vec{0}$), the unit cancels to `MEM_UNIT_NONE` (`"none"`, e.g., `(50 Hz) * (0.02 sec)` or `(2 km) / (500 m)`).
- If a registered unit group matches $\vec{d}_{\text{res}}$, the result automatically adopts that `unit_id` (e.g., `(10 kg) * (9.8 mps2)` $\to$ `98` with unit `"force"`).

### 6.4 Per-Symbol Overload Chains (`sym_first_ov` / `ov_next`)

Instead of scanning all `1,385+` overloads on every operator or function call, `SymTable` maintains singly-linked per-symbol overload chains (`sym_first_ov[sym_id]`, `sym_last_ov[sym_id]`, `ov_next[ov_idx]`). Dispatched lookups visit only the overloads registered for that exact `sym_id` in $O(k)$ time (where $k$ is typically $1 \dots 8$).

---

## 7. Pattern-Driven UTF-8 Lexer (`lexer.c`)

Unlike traditional lexers with hardcoded token tables, `lexer.c` is driven by the active modules in `SymTable`:

1. **Whitespace, Comments & Newline Statement Separation**:
   - Skips horizontal whitespace (`' '`, `'\t'`, `'\r'`) and `# ...` line comments.
   - Records `lex->had_newline = true` whenever `\n` is crossed between tokens. The parser uses `had_newline` to synthesize implicit `;` sequence operators between statements on separate lines without breaking multiline expressions after binary/prefix operators.
2. **Candidate Competing Match (`lex_next`)**:
   At cursor position `lex->pos`, the lexer evaluates two competing matchers and picks the longer match (maximal munch):
   - **Consuming Type Matcher**: Runs `match_pattern` across all active consuming `TypeDef`s (`$real`, `$iota`, `$str`), plus numeric extensions (hexadecimal `0x...`, binary `0b...`, scientific notation `1.5e-3`, and underscore digit separators `1_000_000`).
     - *Smart Scientific Notation Disambiguation*: `1.5e3` is lexed as a single `$real` literal, whereas `2e` (followed by a non-digit) stops before `e` so the parser interprets `2e` via implicit multiplication as `2 * e`!
     - *Postfix `_` Disambiguation*: `42_` stops before trailing `_` so `_` is lexed as the postfix stringification operator (`42_` $\to$ `"42"`).
   - **Active Symbol / Syntax Operator Matcher**: Scans `tab->syms` for the longest active operator or syntax delimiter matching `src + pos`, enforcing word-boundary checks so word operators like `in`, `or`, `by`, or `and` never match prefixes of identifiers like `integrate` or `orbit`.
   - ** Binary vs. Prefix Minus Disambiguation (`5--3`)**: When a multi-character operator like `--` is encountered immediately after a value expression (`prev_was_val == true`) where `--` has both postfix and prefix overloads but `-` is an active infix binary operator, the lexer splits the first `-` as binary subtraction so `5--3` parses naturally as `5 - (-3) = 8`.

---

## 8. Top-Down Pratt Parser & AST Encoding (`parser.c` & `hardcode.c`)

### 8.1 Complete 8-Byte `AstNode` Encoding Table (All 25 `AstKind`s)

Every AST node is 8 bytes (`uint8_t kind`, `uint8_t sub`, `uint16_t ref`, `uint16_t left`, `uint16_t right`):

| `AstKind` | `sub` (8-bit) | `ref` (16-bit) | `left` (16-bit) | `right` (16-bit) | Meaning |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `AST_LIT` (0) | `unit_id` | `type_id` | `pool_idx` (or `StrSlice.off`) | `0` (or `StrSlice.len`) | Literal constant in `f64_pool` or `src_buf` |
| `AST_CONST_SYM` (1) | `0` | `sym_id` | `0` | `0` | Global constant reference (`π`, `e`, `c0`) |
| `AST_SLOT_LOAD` (2) | `slot_idx` | `type_id` | `0` | `0` | Local variable / parameter load (`frame_base + slot_idx`) |
| `AST_FIELD_LOAD` (3) | `0` | `field_type_id` | `base_ast` | `flat_offset` | Compile-time resolved struct field offset load (`v.x`) |
| `AST_UNARY` (4) | `FIX_PREFIX` / `FIX_POSTFIX` | `sym_id` | `operand_ast` | `0` | Unary prefix (`-x`, `√x`, `_s`) or postfix (`x!`, `x_`, `10km`) |
| `AST_BINARY` (5) | `FIX_INFIX_LEFT` / `RIGHT` | `sym_id` | `lhs_ast` | `rhs_ast` | Binary operator (`+`, `*`, `:=`, `;`, `??`, `...`, `in`) |
| `AST_CALL` (6) | `arg_count` | `sym_id` | `first_arg_link` | `MEM_NO_IDX` | Named function call `fn(a, b, ...)` |
| `AST_ARG_LINK` (7) | `0` | `0` | `arg_expr_ast` | `next_arg_link` | Singly-linked argument list node |
| `AST_INTRINSIC` (8) | `IntrinsicId` | `aux_ast` / flags | `arg0_ast` | `arg1_ast` | Direct C `@<intrinsic>` node inside `.math` bodies |
| `AST_CONSTRUCT` (9) | `field_count` | `struct_type_id` | `first_arg_link` | `0` | Flat struct constructor `vec2(x, y)` |
| `AST_EXTERN` (10) | `arg_count` | `sym_id` | `first_arg_link` | `0` | Host C callback `@<extern_name>(args...)` |
| `AST_TERNARY` (11) | `0` | `cond_ast` | `then_ast` | `else_ast` | Lazy conditional branch `cond ? then : else` / `if/or/else` |
| `AST_SWITCH_EQ` (12) | `mode` (`0..3`) | `type_id` / `slot` / `len` | `target_ast` | `pattern_ast` | Pattern-matched `switch` case test or field binding |
| `AST_TAGGED_SCOPE` (13) | `label_id` (`0..15`) | `0` | `body_ast` | `0` | Labeled block or loop `Tag : { ... }` |
| `AST_JUMP` (14) | `label_id` (`0..15`) | `0` | `0` | `0` | Non-local scope jump `jump(Tag)` |
| `AST_RUN_TAG` (15) | `label_id` (`0..15`) | `0` | `0` | `0` | Inline execution of labeled block `run(Tag)` or `Tag()` |
| `AST_BREAK` (16) | `label_id` (`0xFF`=anon) | `0` | `val_ast` (`MEM_NO_IDX`=none) | `0` | Loop/switch/scope exit `break`, `break val`, `break(Tag, val)` |
| `AST_CONTINUE` (17) | `label_id` (`0xFF`=anon) | `0` | `0` | `0` | Next loop iteration `continue` / `continue(Tag)` |
| `AST_RETURN` (18) | `0` | `0` | `val_ast` (`MEM_NO_IDX`=none) | `0` | Early function/evaluation return `return [expr]` |
| `AST_MAT_LIT` (19) | `elem_count` | `col_stride` (`0`=1D) | `first_arg_link` | `0` | Matrix/tensor literal `[1, 2 ; 3, 4]` |
| `AST_INDEX` (20) | `index_dim_count` | `0` | `base_ast` | `first_idx_link` | Subscript & slice `v[i]`, `v[a...b by s]`, `M[r, c]` |
| `AST_FOR_IN` (21) | `var_slot` | `flags \| idx_slot \| lbl` | `iter_ast` | `body_or_pair_ast` | $O(1)$ range/collection loop & comprehension |
| `AST_UNPACK_ELEM` (22) | `elem_idx` | `total_vars` | `tmp_slot` | `0` | Destructuring/broadcast element extraction |
| `AST_LAMBDA` (23) | `param_count` | `(self_p1<<8)\|locals` | `body_ast` | `meta_link` | First-class lexical closure `(x, y = 1) => expr` |
| `AST_FN_CALL` (24) | `arg_count` | `0` | `callee_ast` | `first_arg_link` | Invocation of first-class `$fn` handle |

### 8.2 Pratt Operator Precedence Hierarchy

Operators declared in `.math` specify an integer precedence (`0..100`). During Pratt parsing (`parser_parse_expr(P, min_prec)`):
- Left-associative infix operators (`FIX_INFIX_LEFT`) recurse with `next_min = prec + 1`.
- Right-associative infix operators (`FIX_INFIX_RIGHT`, such as `^`, `.^`, `:=`, `=`, `+=`, `;`, `??`, `into`) recurse with `next_min = prec`.

| Precedence | Operators | Fixity | Purpose |
| :--- | :--- | :--- | :--- |
| **95** | `.` (`@field`), `[...]` (`AST_INDEX`) | Infix Left / Postfix | Struct field access, UFCS dot-chaining, array/matrix/map indexing & slicing |
| **86** | `i`, `deg`, `°`, `km`, `m`, `kg`, `sec`, `N`, `J`, ... | Postfix | Imaginary unit suffix and physical SI/imperial unit suffixes |
| **85** | `!`, `++`, `--`, `_`, `to_m`, `to_deg`, `to_inr`, ... | Postfix | Factorial, postfix inc/dec, stringification `x_`, postfix unit conversions |
| **80** | `^`, `.^` | Infix Right | Exponentiation and elementwise Hadamard power |
| **75** | `...`, `>..`, `..<`, `>..<` | Infix Left | Generic inclusive/exclusive ranges and nested exclusion range chains |
| **72** | `by` | Infix Left | Range step stride (`1...10 by 2`) |
| **70** | `-`, `√`, `++`, `--`, `!`, `not`, `~`, `_` | Prefix | Unary negation, square root, logical/bitwise NOT, string parsing `_s` |
| **65** | `**` | Infix Left | Vector dot product |
| **60** | `*`, `/`, `%`, `.*`, `./` | Infix Left | Multiplication (and implicit multiplication), division, modulo, Hadamard ops |
| **50** | `+`, `-`, `plus`, `minus` | Infix Left | Addition and subtraction |
| **45** | `<<`, `>>` | Infix Left | Bitwise left and right shifts |
| **42** | `in` | Infix Left | Unit conversion (`25km in m`), membership (`x in v`), and loop iterator header |
| **40** | `~` | Infix Left | Approximate equality (`\|a - b\| <= 0.001`) |
| **35** | `into` | Infix Right | Entity synthesis (`5 into 5i`), promotion (`7 into vec2`), vector push (`x into v`) |
| **32** | `\|>` | Infix Left | Functional pipeline operator (`x \|> f` / `x \|> f(a, b)`) |
| **30** | `==`, `!=`, `<`, `<=`, `>`, `>=` | Infix Left | Relational & equality comparisons |
| **25** | `&`, `&&`, `and`, `nand` | Infix Left | Distributed/short-circuit AND and logical AND/NAND |
| **20** | `\|`, `\|\|`, `or`, `xor`, `nor` | Infix Left | Distributed/short-circuit OR and logical OR/XOR/NOR |
| **15** | `?`, `:` | Infix Right | Lazy ternary conditional (`cond ? then : else`) and tag/slice colon |
| **12** | `=>` | Infix Right | First-class lambda closure definition (`(x, y) => expr`) |
| **10** | `:=`, `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `^=` | Infix Right | Variable declaration, mutation, destructuring, and compound assignments |
| **5** | `??`, `?!?` | Infix Right | Pre-tested while/range loop (`??`) and post-tested do-while loop (`?!?`) |
| **1** | `;` (and implicit newline `;`) | Infix Right | Statement sequencing |

### 8.3 Generic `...` Range & Nested Exclusion Range AST Rewriting (`hardcode_rewrite_range_chain`)

The language uses a single unified 3-dot range family where `...` is inclusive-inclusive (`[a, b]`) and angle brackets `<` and `>` attach to `...` to express open bounds and nested skip intervals:
- `a...b` $\to$ `@vrange(a, b, 0)` (mode `0`: inclusive start, inclusive end $[a, b]$)
- `a..<b` $\to$ `@vrange(a, b, 1)` (mode `1`: inclusive start, exclusive end $[a, b)$)
- `a>..b` $\to$ `@vrange(a, b, 2)` (mode `2`: exclusive start, inclusive end $(a, b]$)
- `a>..<b` $\to$ `@vrange(a, b, 3)` (mode `3`: exclusive start, exclusive end $(a, b)$)

When the user writes a nested exclusion (skip) range or multi-segment range such as:
- `1..<25...75>..100` (skip `25..75`, yielding `1..24` and `76..100`)
- `1..<25...50>..<75...90>..<100` (skip `25..50` and `75..90`, yielding `1..24`, `51..74`, and `91..99`)
- `1..<5>..10` (skip single point `5`, yielding `1..4` and `6..10`)

Because `...`, `..<`, `>..`, and `>..<` all share precedence `75` (`FIX_INFIX_LEFT`), the Pratt parser reduces them left-to-right and invokes `hardcode_rewrite_range_chain(P, fix, sym_id, left, right)`:
1. **Open Skip Start (`Case 1`)**:
   - When `left` is an exclusive-end range `(a..<s1)` (or `(a>..<s1)`) and the current operator is `...` with right operand `e1`, the parser emits a **pending skip node** (`AST_INTRINSIC` with `sub = INTR_VRANGE_CAT` and `ref = 0xFFFFu`) pairing the prefix segment `(a..<s1)` with the skip-end bound `e1`.
   - When `left` is `(a..<s1)` and the current operator is `>..` or `>..<` with right operand `b` (as in `1..<5>..10`), it immediately constructs a completed `INTR_VRANGE_CAT(ref = 0)` node concatenating `(a..<s1)` and `(s1>..b)`.
2. **Closing Skip Interval (`Case 2`)**:
   - When `left` is a pending skip node `(prefix_seg, e1)` and the current operator is `>..` or `>..<` with right operand `b`, the parser closes the excluded interval `[s1, e1]` by constructing `next_seg = (e1 >.. b)` (or `e1 >..< b`) and returning `INTR_VRANGE_CAT(ref = 0, left = prefix_seg, right = next_seg)`.
3. **Chained Multi-Skip Intervals (`Case 3`)**:
   - When `left` is already a completed `INTR_VRANGE_CAT` tree whose right-most segment ends with `<` (e.g., `50>..<75` in `1..<25...50>..<75...90>..<100`), subsequent `... 90` and `>..< 100` operators recursively open and close additional pending skip nodes on the right spine of the `INTR_VRANGE_CAT` tree.
4. **Zero-Allocation $O(1)$ Fast Paths**:
   - `extract_vrange_expr_info` / `collect_vrange_segs` flattens any `INTR_VRANGE` / `INTR_VRANGE_CAT` tree into up to 8 `VRangeSegAst` descriptors (`lo_ast`, `hi_ast`, `mode`).
   - Loops (`(x in 1..<25...75>..100) { ... }`), comprehensions (`[f(x) for x in 1..<25...75>..100]`), membership checks (`x in 1..<25...75>..100`), and `switch` cases (`case 1..<25...75>..100:`) iterate or test the `VRangeSegAst` segments directly in $O(1)$ scratch memory **without** allocating any intermediate vector!
   - When materialized as a vector (e.g., `v := 1..<25...75>..100` or `(1..<4...7>..10) by 2`), `INTR_VRANGE_CAT` concatenates the segment vectors and merges their segment bounds into `MemArena`'s 8-segment `vrange` metadata cache so a trailing `by step` operator steps cleanly across all segments.

---

## 9. Type- & Unit-Dispatched AST Evaluator (`evaluator.c`, `delegate.c`, `hardcode.c`)

### 9.1 Two-Pass Orthogonal Multiple-Dispatch Scoring (`dispatch_overload`)

When evaluating an `AST_UNARY`, `AST_BINARY`, or `AST_CALL` node for symbol `sym_id` with $N$ evaluated arguments $(v_0, \dots, v_{N-1})$ having runtime entity types $(t_0, \dots, t_{N-1})$ and unit tags $(u_0, \dots, u_{N-1})$:

1. **Candidate Filtering**: Iterates `tab->sym_first_ov[sym_id]` via `ov_next`, considering only overloads where `sym_mod_enabled(tab, ov->mod_id)` is true, `ov->fixity == fix`, and `ov->param_count == argc`.
2. **Entity Type & Unit Compatibility Scoring**:
   For each parameter $k \in [0, N-1]$ with declared `(pt = ov->param_types[k], pu = ov->param_units[k])`:
   - **Entity Type Score**:
     - Exact concrete type match (`pt == t_k`): **`+10`**
     - Generic entity wildcard (`pt == SYM_TYPE_ANY`, i.e., `$T` / `$any`): **`+2`** (and binds `bound_any_type = t_k`, enforcing homogeneous `$T` if all parameters share `$T`)
     - Otherwise: candidate rejected (`score = -1`).
   - **Physical Unit Score**:
     - Explicit concrete unit match (`pu == u_k` where `pu` is `1..63`): **`+20`**
     - Explicit unitless lock (`pu == SYM_UNIT_LOCK_NONE`, i.e., `$none` when `u_k == 0`): **`+15`**
     - Explicit generic unit wildcard (`pu == SYM_UNIT_GENERIC`, i.e., `, T` / `, any`): **`+8`** (binds `bound_gen_unit = u_k`, enforcing matching units across all `, T` parameters)
     - Omitted unit annotation (`pu == SYM_UNIT_OMITTED`): **`+5`** if `u_k == 0`, or **`+4`** if `u_k != 0` (allows unit-agnostic functions like `sin`, `abs`, or `vec.push` to accept unit-tagged values when no unit-specific overload matches).
3. **Return Unit Propagation**:
   - Concrete return unit (`ov->ret_unit` in `1..63`): attaches `ov->ret_unit`.
   - Generic return unit (`ov->ret_unit == SYM_UNIT_GENERIC`): propagates `bound_gen_unit`.
   - Unitless lock (`ov->ret_unit == SYM_UNIT_LOCK_NONE`): strips unit to `MEM_UNIT_NONE`.
   - Omitted return unit (`ov->ret_unit == SYM_UNIT_OMITTED`): for arithmetic/power/calculus intrinsics (`+`, `-`, `*`, `/`, `^`, `sqrt`, `cbrt`), synthesizes the 7D SI unit via `delegate_combine_si_units`.

### 9.2 Sequence Evaluation & Non-Local Control Flow (`eval_seq_chain`)

Statements separated by `;` (or newlines) right-associate into a spine of `AST_BINARY(INTR_SEQ)` nodes. `eval_seq_chain` flattens up to 128 sequential statements into a local array `seq_nodes[128]` with an explicit Program Counter (`pc`):
- **Pre-Scan Pass**: Scans `seq_nodes` for `AST_TAGGED_SCOPE` nodes (`Tag : { ... }`) and records `label_pc[label_id] = i` and `mem->label_ast[label_id] = body_ast`.
- **`jump(Tag)` (`JUMP_GOTO`)**: Sets `pc = label_pc[target_id]`, enabling both backward loops and forward jumps across statements in $O(1)$ time without growing the C call stack.
- **`break` / `continue` / `return`**: Sets `mem->jump_active` (`JUMP_BREAK_ANON`, `JUMP_BREAK_TAG`, `JUMP_CONTINUE_ANON`, `JUMP_CONTINUE_TAG`, `JUMP_RETURN`) and optional `mem->jump_val`, unwinding cleanly to the enclosing loop, `switch` block, tagged scope, or function frame.

### 9.3 Symbolic Computer Algebra System (CAS) Engine (`delegate.c`)

The symbolic engine operates directly on the 8-byte `AstNode` graph without external CAS libraries:
- **`sym_simplify(expr)` / `simplify(expr)`**: Recursively folds constant subtrees, applies algebraic identities ($x + 0 \to x$, $x \cdot 1 \to x$, $x \cdot 0 \to 0$, $x^1 \to x$, $x^0 \to 1$, $(x^a)^b \to x^{a \cdot b}$, $x - x \to 0$), and collects like polynomial/monomial terms ($2x + 3x \to 5x$).
- **`sym_diff(x, expr[, order])`**: Exact analytical differentiation implementing the sum, product, quotient, power, logarithmic, exponential, trigonometric, inverse-trig, and hyperbolic chain rules, followed by multi-pass algebraic simplification.
- **`sym_integ(x, expr)`**: Analytical indefinite integration supporting polynomials, power rules, $1/x \to \ln(x)$, linear substitutions $f(ax + b)$, and integration by parts ($\int x e^{ax}\,dx$, $\int x \sin(ax)\,dx$, $\int x \cos(ax)\,dx$, $\int x \ln(x)\,dx$).
- **`sym_solve(x, eq)` & `_sym_solve(x, eq)`**: Extracts polynomial degree in $x$ up to degree 2 (both numeric and symbolic coefficients $ax + b = c$ and $ax^2 + bx + c = 0$), returning exact closed-form symbolic formulas (`"(c - b) / a"`) or numeric root vectors.
- **`sym_taylor(x, expr, x0, order)`**: Repeatedly differentiates `expr` symbolically up to `order`, evaluates derivatives at `x0`, and synthesizes the simplified Taylor polynomial string.

---

## 10. Compiler, Topological Linker & `.mathc` Binary Image (`Core/compile/`)

### 10.1 Two-Stage Module Compilation & Kahn's Topological Linker (`compiler.c`, `linker.c`)

1. **Module Header & Dependency Syntax**:
   A `.math` file can declare multiple `[module_name]` sections and explicit dependencies via `<[dep_a, dep_b]`.
2. **Topological DAG Resolution (`linker.c`)**:
   When linking multiple `UserUnit`s via `user_link_ext`, the linker builds an adjacency matrix over all modules, checks for missing dependencies or duplicate module names, and runs **Kahn's in-degree topological sort** to determine a valid compilation order (rejecting circular dependencies like `[cyc_a] <-> [cyc_b]` with a diagnostic error).
3. **Default Parameter Overload Synthesis (`hardcode_synthesize_default_overloads`)**:
   When a `.math` function declares default parameter expressions such as `round(x:real, digits:real = 0) -> real => ...`, the compiler registers the full $N$-parameter overload and automatically synthesizes $N - R$ wrapper overloads (for `argc = req_count .. N - 1`) whose AST bodies evaluate the default expressions into parameter slots `m .. N - 1` before jumping to `full_body`.

### 10.2 48-Byte `.mathc` Binary Image Format & Pre-Load Structural Validator (`mathc_bin.c`)

A compiled `.mathc` image consists of a 48-byte `MathcHeader` followed by 10 packed contiguous arrays:

```text
┌────────────────────────────────────────────────────────────────────────────┐
│ 0x00..0x2F : MathcHeader (48 bytes: magic="MTHC", version=1, 16-bit        │
│              mod_count, counts, payload_bytes, intr_hash, FNV-1a checksum) │
├────────────────────────────────────────────────────────────────────────────┤
│ 1. MemArena.src_buf[0 .. perm_src_len - 1]        (1 byte  * perm_src_len) │
│ 2. MemArena.ast[0 .. perm_ast_count - 1]          (8 bytes * perm_ast_cnt) │
│ 3. MemArena.f64_pool[0 .. perm_pool_top - 1]      (8 bytes * perm_pool_top)│
│ 4. sym_mod_names[0 .. mod_count - 1]              (4 bytes * mod_count)    │
│ 5. sym_mod_active[0 .. mod_count - 1]             (1 byte  * mod_count)    │
│ 6. SymTable.types[0 .. type_count - 1]            (14 bytes * type_count)  │
│ 7. SymTable.units[0 .. unit_count - 1]            (4 bytes * unit_count)   │
│ 8. SymTable.unit_dims[0 .. unit_count - 1]        (8 bytes * unit_count)   │
│ 9. SymTable.pat_elems[0 .. pat_elem_count - 1]    (6 bytes * pat_elem_cnt) │
│ 10. SymTable.fields[0 .. field_count - 1]         (8 bytes * field_count)  │
│ 11. SymTable.syms[0 .. sym_count - 1]             (4 bytes * sym_count)    │
│ 12. SymTable.overloads[0 .. overload_count - 1]   (46 bytes * overload_cnt)│
│ 13. SymTable.consts[0 .. const_count - 1]         (10 bytes * const_count) │
└────────────────────────────────────────────────────────────────────────────┘
```

Before copying a single byte into `MemArena` or `SymTable`, `ext_validate_mathc_buf` performs a full security audit on the untrusted buffer:
1. Verifies `magic == 0x4348544D` (`"MTHC"`), `version == 1`, and exact buffer length `sizeof(MathcHeader) + payload_bytes`.
2. Verifies `hdr.intr_hash == calc_intr_table_hash()`, guaranteeing that the `.mathc` image was compiled against the exact same `IntrinsicId` enum table.
3. Verifies the 32-bit **FNV-1a checksum** over the header and payload.
4. Bounds-checks every `StrSlice`, `TypeDef`, `FieldDef`, `OpOverload`, `ConstDef`, and every single `AstNode` index (`left`, `right`, `ref`, `sub`) against the header counts so corrupted or maliciously fuzzed `.mathc` files can never trigger out-of-bounds memory reads.

---

## 11. Parallel In-Memory Sessions & Order-Tagged Snapshots (`Core/session/`)

### 11.1 Parallel Session Execution (`session.c`)

Each `Engine` embeds `ENG_MAX_SESSIONS = 8` independent `EngSession` slots (`eng_session_open`, `eng_session_eval`, `eng_session_reset`, `eng_session_close`).
During `eng_session_eval(eng, session_id, expr, &out_val)`:
1. Reconstructs a prologue AST that restores all persisted session variables (`sess->vars[0 .. var_count - 1]`), including their runtime values, concrete entity type locks (`x : real`), concrete unit locks (`x : distance`), containers (`$vec`, `$mat`, `$map`), structs (`$vec2`, `$complex`), and closure/inline functions (`f(x) := ...`).
2. Parses and appends the caller's `expr` to the prologue in the same lexical scope and evaluates the combined AST.
3. Extracts all surviving top-level variables, canonicalizes their values into self-contained MEP expressions (`val_text`), updates `sess->vars`, and records an `MSP1` snapshot.

### 11.2 Tamper-Evident Chained `MSP1` Snapshot Format (`snapshot.c`)

Every evaluation $k$ in a session produces a self-contained text snapshot line:

```text
MSP1|seq:2|prev:1659369F|hash:0F7E856E|vars:4|dist_x:real:distance:real:distance=60;factor:real:none:*:*=3;q:vec:distance:*:*=list<real>(20, 30);scale_fn:fn:none:*:*=a => a * factor|res:real:distance=60
```

- **`seq:<N>`**: 1-based monotonic sequence number.
- **`prev:<HEX8>`**: 32-bit FNV-1a hash of snapshot `seq:N-1` (`00000000` for `seq:1`), forming a cryptographic-style hash chain across the session's history.
- **`hash:<HEX8>`**: 32-bit FNV-1a checksum covering `prev_hash`, `seq`, `vars`, and `res`.
- **`eng_session_restore(eng, snapshots, count)`**: Accepts an array of snapshot strings in **any order**, sorts them by `seq`, verifies that `seq:1 .. seq:count` has no gaps, verifies every `prev` link and `hash` checksum, and restores the final session state.

---

## 12. Host C API & Android NDK JNI Bridge (`math_engine_jni.c`)

### 12.1 Host `@extern` Callback Mechanism

Host applications can expose platform services (such as live foreign-exchange rates, database/key-value stores, hardware sensors, or periodic table lookups) to `.math` scripts via `eng_register_extern`:

```c
bool my_host_callback(MemArena *mem, const SymTable *tab,
                      const Value *args, uint8_t argc,
                      void *user_ctx, Value *out_val);
```

Inside `.math` files, the function is bound to `@<extern_name>(...)`:
```text
fx_rate(from:str, to:str) -> real => @fx_rate(from, to)
```
Because `ExternDef` callbacks are stored in `SymTable.externs` by name (`char name[32]`), host registrations survive `.mathc` binary reloads automatically.

### 12.2 Android NDK JNI Bridge (`Core/jni/math_engine_jni.c`)

`math_engine_jni.c` exposes 25 JNI methods under `com.mathengine.MathEngineBridge`, storing the heap-allocated `Engine*` pointer as a Java `long` handle (`jlong`):
- **Lifecycle**: `nativeCreate()`, `nativeCreateBlank()`, `nativeDestroy(handle)`
- **Stateless & Formatted Eval**: `nativeEval(handle, expr)`, `nativeEvalFormatted(handle, expr)` (`"value|type|unit"`), `nativeLastError(handle)`
- **Module Control & Introspection**: `nativeSetModule`, `nativeIsModuleActive`, `nativeModuleCount`, `nativeModuleName`, `nativeOverloadCount`, `nativeConstCount`, `nativeCompactScratch`, `nativeScratchPoolUsed`
- **Symbolic CAS**: `nativeSymSimplify`, `nativeSymDiff`, `nativeSymInteg`, `nativeSymSolve`
- **Extension Compilation & Binary `.mathc` Loading**: `nativeLoadExtText`, `nativeCompileExtToMathc`, `nativeLoadMathcBytes`
- **Parallel Sessions & Snapshots**: `nativeSessionOpen`, `nativeSessionClose`, `nativeSessionReset`, `nativeSessionEval`, `nativeSessionSnapshotCount`, `nativeSessionSnapshotNext`, `nativeSessionRestore`
