# Math-Engine-MEP (C11 Symbolic & Numerical Math Engine)

**Math-Engine-MEP** is a zero-dependency C11 mathematical runtime, compiler, linker, and symbolic/numerical engine built around an **Extension-Defined Grammar**. The C core boots as a blank slate (`eng_init_blank`) with zero built-in operators or types; the entire language grammar, type system, 7D SI dimensional unit system, linear algebra, and calculus libraries are defined in `.math` extension modules (`Main/Library/*.math`) and precompiled into a portable binary image (`precompiled/stdlib.mathc`).

---

## Key Capabilities

1. **8-Byte IEEE-754 NaN-Boxed `Value` Representation**:
   - Every runtime value (`real`, `iota`, `complex`, `str`, `vec`, `mat`, `map`, `fn`, and user-defined `struct`s) is encoded in a 64-bit `uint64_t` (`Value`) carrying both its entity `TypeId` and 7D SI `UnitId`.
2. **Small-Buffer Optimization (SBO) Arena + Transparent Heap Spill & Sliding GC**:
   - Small vectors, matrices, maps, structs, and closures (`<= 64` doubles) allocate in O(1) inside the scratch `f64_pool`.
   - Containers exceeding 64 slots automatically spill to `HeapBlock` allocations (`MEM_MAX_HEAP_BLOCKS = 256`) and are reclaimed by `mem_compact_scratch` (Mark-Sweep + Sliding Compaction GC).
3. **Unified Standalone & Multi-File Project Pipeline**:
   - **Standalone Script Mode**: A single `.math` file runs top-to-bottom (`./DebugBuild/myapp script.math`) and also auto-invokes `@entry(fn)` or `main()` if defined.
   - **Multi-File Project Mode**: Passing multiple `.math` files (`./DebugBuild/myapp lib.math sim.math`) stages all compilation units, topologically links `<[dep]` dependencies in declaration-only mode, and executes the project-wide `@entry(fn)` or `main()` entry point.
4. **7D SI Dimensional Algebra & `into` Unit Conversion**:
   - Physical units (`[units]` & `[conversions]`) track 7 SI base exponents `[m, kg, s, A, K, mol, cd]`.
   - Postfix unit construction (`50m`, `25km`, `9.8mps2`, `100N`) and algebraic derivation (`(36 kph) * (10 sec)` -> `100 [length]`, `√((3N)^2 + (4N)^2)` -> `5 [force]`).
   - Explicit unit conversion uses the `into` operator (`2km into m` -> `2000 [length]`, `180deg into rad` -> `3.141592654 [angle]`), while `in` is dedicated to Boolean membership (`5 in 1...10`) and iteration (`(i in 1...5) { ... }`).
5. **Symbolic CAS & Numerical Calculus**:
   - Symbolic simplification (`sym_simplify`), symbolic differentiation (`sym_diff`), symbolic integration (`sym_integ`), Taylor series (`sym_taylor`), and polynomial root solving (`sym_solve`).
   - Numerical differentiation (`diff`), adaptive Simpson integration (`integrate`), limits (`lim`), root finding (`solve`), optimization (`fmin`, `fmax`), and RK4 ODE solvers (`ode_rk4`, `ode_rk4_vec`, `ode_coupled2`, `ode_coupled3`, `ode_rk4_sys`, `ode_rk4_sys_traj`).
6. **OTA Patch Pipeline (`eng_apply_patches` / `nativeApplyPatch`)**:
   - Incremental `.math` patches (such as `patches/currency.math`) can both register new modules (`[currency]`) and hotfix existing `stdlib` functions (`allow_redef = true`), promoting patched modules into `base_mod_count` so project resets preserve applied patches.

---

## Project Directory Structure

```text
Math-Engine-MEP/
├── README.md
├── docs/
│   ├── LANGUAGE_AND_STDLIB.md         # Complete .math language & 17-module stdlib reference
│   └── ARCHITECTURE_AND_API.md        # C11 API, JNI bridge, memory model & .mathc binary spec
└── Main/
    ├── CMakeLists.txt                 # CMake build configuration (static/shared lib, myapp, mathc)
    ├── makefile                       # GNU Make build wrapper (debug, release, stdlib, test, ndk)
    ├── rebuild_all.sh                 # Full rebuild helper script
    ├── Core/
    │   ├── Headers/
    │   │   ├── mem_arena.h            # Arena, SBO/Heap containers & NaN-boxed Value API
    │   │   ├── mem_internal.h         # Internal helpers shared across mem_*.c
    │   │   ├── sym_table.h            # Symbol table, types, 7D SI units, modules & overloads
    │   │   ├── lexer.h                # UTF-8 Maximal-Munch tokenizer
    │   │   ├── parser.h               # Pratt parser & AST builder interface
    │   │   ├── parser_internal.h      # Internal helpers shared across parser_*.c
    │   │   ├── evaluator.h            # Tree-walk AST evaluator & multiple-dispatch interface
    │   │   ├── eval_internal.h        # Internal helpers shared across eval_*.c
    │   │   └── math_engine.h          # Public C11 host API (Engine lifecycle, eval, modules, CAS)
    │   ├── Code/
    │   │   ├── mem_arena.c            # Arena lifecycle, string/AST/frame pools & heap blocks
    │   │   ├── mem_containers.c       # Dynamic $vec, $mat, $map SBO/heap ops & Value boxing
    │   │   ├── mem_gc.c               # Mark-sweep & sliding compaction garbage collector
    │   │   ├── sym_table.c            # Symbol/type/unit/overload registry & @extern table
    │   │   ├── lexer.c                # Tokenizer implementation
    │   │   ├── parser_core.c          # Parser state, local slots, labels & assignment builders
    │   │   ├── parser_postfix.c       # UFCS dot-steps, matrix literals, indexing, calls & lambdas
    │   │   ├── parser_control.c       # if/or/else, pattern-matched switch, jumps & try/catch
    │   │   ├── parser_nud.c           # String interpolation & Pratt prefix/atom (NUD) handler
    │   │   ├── parser_expr.c          # Pratt infix/postfix (LED) precedence loop
    │   │   ├── eval_dispatch.c        # Tagged scope jumps & multiple-dispatch overload resolution
    │   │   ├── eval_index.c           # Container/struct indexing, slicing & matrix literal eval
    │   │   ├── eval_io_json.c         # Sandboxed file I/O & RFC 8259 JSON parser/serializer
    │   │   ├── evaluator.c            # Core AST tree-walk evaluator
    │   │   └── math_engine.c          # High-level Engine API, unified runner & patch loader
    │   ├── compile/
    │   │   ├── compiler.h             # Module stager & slice evaluator interface
    │   │   ├── compiler_internal.h    # Internal helpers shared across compiler_*.c
    │   │   ├── compiler_decl.c        # Type, unit, constant & function/operator overload parsers
    │   │   ├── compiler_slice.c       # Top-to-bottom module slice evaluator & dynamic import
    │   │   ├── compiler_unit.c        # Multi-module compilation unit stager
    │   │   ├── linker.h / linker.c    # Topological <[dep] DAG resolver & linker
    │   │   ├── mathc_bin.h / .c       # Portable .mathc binary serializer & validator ("MTHC" v1)
    │   │   └── user_loader.h / .c     # In-memory user extension & .mathc loader API
    │   ├── lib/
    │   │   ├── delegate/
    │   │   │   ├── delegate.h         # @<intrinsic> opcode registry interface
    │   │   │   ├── delegate_core.c    # Intrinsic table, 7D SI unit algebra & higher-order calls
    │   │   │   ├── delegate_containers.c # Vector, map & matrix intrinsics + value formatting
    │   │   │   ├── delegate_numeric.c # Numeric, bitwise, complex, string/regex & calculus delegates
    │   │   │   └── delegate_symbolic.c # Symbolic CAS simplify, diff, integ & polynomial solver
    │   │   └── hardcode/
    │   │       ├── hardcode.h         # Grammar & ergonomic rule helpers
    │   │       ├── hardcode_internal.h # Internal helpers shared across hardcode_*.c
    │   │       ├── hardcode_parse.c   # Wildcards, lookahead & parameterized container type args
    │   │       └── hardcode_eval.c    # Range chains, pipe |>, for-in loops, unpacking & switch
    │   └── jni/
    │       ├── math_engine_jni.h      # JNI C header (includes standalone mock JNI types for host tests)
    │       └── math_engine_jni.c      # JNI bridge (Java_com_mathengine_MathEngineBridge_*)
    ├── JNI/
    │   ├── MathEngineBridge.java      # Java JNI bridge class
    │   └── MathEngineBridge.kt        # Kotlin JNI bridge & wire-format parser
    ├── Library/
    │   ├── mathc.c                    # Standalone AOT .math -> .mathc compiler tool
    │   └── *.math                     # 15 standard library .math files (17 modules)
    ├── patches/
    │   ├── currency.math              # Live & offline currency conversion patch ([currency])
    │   └── manifest.json              # SHA-256 verified OTA patch manifest
    ├── precompiled/
    │   ├── stdlib.mathc               # Precompiled standard library binary image
    │   └── arm64-v8a/libmathengine.so # Prebuilt Android arm64-v8a shared library
    ├── tools/
    │   ├── build_android_arm64.sh     # NDK arm64-v8a shared library build script
    │   └── generate_patch_manifest.py # Patch manifest generator with topological sorting
    └── Test/
        ├── test_harness.h             # Shared declarations for the verification suite & CLI
        ├── test_cases.c               # Host @extern callbacks & 574-expression test table
        ├── test_cli.c                 # Interactive REPL, CLI evaluator (-e / .math) & benchmarks
        ├── main.c                     # 17-section architectural verification suite
        └── user_physics.math          # Sample user extension module ([user_physics])
```

---

## Building & Running

All build commands are run from `Main/`:

```bash
cd Main

# 1. Compile Library/*.math -> precompiled/stdlib.mathc
make stdlib

# 2. Build Debug binary (with ASan + UBSan) and run the 574-test + 17-section verification suite
make build
./DebugBuild/myapp

# 3. Build optimized Release binary
make build release

# 4. Evaluate expressions directly from the CLI
./DebugBuild/myapp -e "2km into m" "sym_diff(x, x^3 + sin(x))" "(1...5).map(x => x^2)"

# 5. Run a standalone .math script or a multi-file .math project
./DebugBuild/myapp script.math
./DebugBuild/myapp lib_module.math main_app.math

# 6. Launch the interactive REPL or benchmark suite
./DebugBuild/myapp --repl
./DebugBuild/myapp --bench
```

See [`docs/LANGUAGE_AND_STDLIB.md`](docs/LANGUAGE_AND_STDLIB.md) for the complete `.math` language and standard library guide, and [`docs/ARCHITECTURE_AND_API.md`](docs/ARCHITECTURE_AND_API.md) for the C11 and JNI API reference.

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
