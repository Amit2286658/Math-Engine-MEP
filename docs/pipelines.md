# Math-Engine-MEP Execution Pipelines — All Ways of Running `.math` Code

Every workflow in this document is exercised by `Test/main.c` (§17a/§17b) and is the exact
route the Android app (`Ecalc`) drives through the JNI bridge. Cross-references:
language constructs live in `LANGUAGE_AND_STDLIB.md`, engine internals and the full API
table in `ARCHITECTURE_AND_API.md`.

**The one idea behind everything:** there is a *single* underlying execution engine
(`eng_build_and_run` → stage → topological link → run), and the three user-facing
"modes" are entry points that either use the pipeline or deliberately skip it:

| # | Mode | API call | Engine sees |
|---|------|----------|-------------|
| 1 | Plain expression (calculator one-shot) | `eng_eval` | One expression, fast path, **no pipeline** |
| 2 | Single user module / script | `eng_eval` (auto-promoted) or `eng_build_and_run` with `unit_count == 1` | One `[module]` unit, run top-to-bottom |
| 3 | Multi-file project | `eng_build_and_run` with `unit_count > 1` | Many unordered `[module]` units, topologically linked, one entry point |

---

## 0. Lifecycle Prerequisites (every mode)

Regardless of mode, an `Engine` always starts the same way:

```c
Engine *eng = eng_create();                     /* heap-allocate (Engine is large) */
uint8_t *img = read_file("precompiled/stdlib.mathc");
user_load_mathc(eng, img, img_len);             /* O(1) binary stdlib load        */
```

- `user_load_mathc` (`Core/compile/user_loader.h`) validates the `MTHC` v1 image
  (checksum + intrinsic-table hash), then installs it into the **permanent region**.
- It then **snapshots the Base-Layer watermarks** (`eng_snapshot_base` /
  `sym_snapshot_base`). Everything the stdlib image defined — modules, operators,
  unit groups, the bootstrap `$type`s — becomes the immutable base layer.
- All flush/recycling guarantees in this document rest on that snapshot: `eng_flush_user_modules`
  rolls the engine back to the watermark positions in O(1) time, erasing *user* state only.

The engine owns **no files**. Filesystem reading, path resolution, and directory listing
belong to the frontend (`eng_set_import_dir` / `eng_import_file` are the only file-touching
entry points, and they exist purely for the `import "file.math"` convenience
described in §5.2).

---

## 1. Mode 1 — Plain Expression (the calculator fast path)

**Trigger:** any input with *no* module constructs. The router is the first thing
`eng_eval` (`Core/Code/math_engine.c:100`) does:

```c
if (ext_src_has_module_constructs(...))     /* [module], <[dep], import, @dim,   */
    return eng_build_and_run(eng, &expr, 1, out_val);   /* @entry, pub/priv, $type */
/* ...otherwise: plain expression fast path */
Parser P; parser_init(&P, &eng->mem, &eng->sym, expr, (uint16_t)raw_len, ...);
uint16_t root = parser_parse_expr(&P, 0);
Value res = eval_ast(&eng->mem, &eng->sym, root, frame_base, 0);
```

**Semantics:**

- One expression, parsed and evaluated in the caller's module scope over the base layer.
- **No staging, no linking, no entry-point resolution.** Nothing persists: local
  variables and any `pub`-less scratch live only in a transient call frame.
- Compiled state (stdlib + anything previously linked) is *reused*, never touched.
- `out_val` receives the value; `eng_last_error(eng)` carries the message. Errors on
  this fast path are not line-suffixed — the whole input *is* the context (the
  ` (line N)` mechanics of §8 apply to the chunk pipeline, i.e. Modes 2 and 3).

**Example** (radians-first engine; the degree-mode wrapping shown here is exactly what
the app's `toEngineSyntax` performs before the string ever reaches the engine):

```math
sin(deg2rad(45)) + 2 * 3
```

```c
Value out;
if (!eng_eval(eng, "sin(deg2rad(45)) + 2 * 3", &out)) {
    fprintf(stderr, "error: %s\n", eng_last_error(eng));
} else {
    printf("= %g\n", val_get_f64(&eng->mem, out));   /* 6.7071... (verified on host) */
}
```

What the engine did: parse → single `eval_ast` call → return. No staging, no stdout;
the output buffer (`eng_get_output`) stays empty.

**When code outgrows Mode 1:** any module construct promotes the same string to the
unified pipeline (§2). The caller does *not* need to know — `eng_eval` routes both.

---

## 2. Mode 2 — Single User Module / Script

A *unit* is one string of `.math` source. A single unit may contain a module header
(`[name]`), dependency directives (`<[arith]`), declarations, and top-level statements —
all in one buffer.

**Trigger A — automatic:** the text contains module constructs, so `eng_eval` promotes it:

```c
eng_eval(eng, "[single_app]\n<[arith]\nscale_k := 3\nrun_single() = ...\n@entry(run_single)\n", &out);
```

**Trigger B — explicit:** the frontend already knows it is a script (e.g. the Studio
"Run" button on one open file):

```c
const char *unit = "[single_app]\n<[arith]\n... ;
Value out;
bool ok = eng_build_and_run(eng, &unit, 1, &out);
```

**Pipeline steps for `unit_count == 1`:**

1. **Stage** (`ext_compile_unit`): the raw text lands in a staging context
   (`UserBuildCtx` / `ExtLoader`), where the module header, `<[dep]` metadata, and
   declarations are parsed and registered. Units are order-independent by design —
   irrelevant here, but the same code serves Mode 3.
2. **Link** (`user_link_ext` → `ext_link_units`, `Core/compile/linker.c`): dependencies
   (`<[units]` pulls library modules) are resolved, then execution order is decided
   by the **entry ladder** (§3, step 4 — for a single unit this mostly means
   "run everything top-to-bottom").
3. **Run:** top-level statements execute in order. If `@entry(fn)` or `main()` is
   declared, the entry function is invoked and **its return value is the result**
   (Example B, `40.0`); otherwise the **value of the last non-`void` statement is the
   result** (Example A, `100.0`).

**Example A — script, no entry function, last statement wins** (`Test/main.c` §17a,
verified on host: `100.0` with unit group `area`):

```math
<[arith]
<[units]

$span2d(w:real, h:real)
span_area(s:span2d) = s.w * s.h
box := span2d(12, 5)
span_area(box) * 1 m2 + 40 m2
```

```c
Value out;
bool ok = eng_build_and_run(eng, &unit, 1, &out);   /* 60 + 40 = 100, unit [area] */
```

Note: the `[module]` header is optional for a single script — dependency directives
alone are enough to promote it into the pipeline.

**Example B — script with `@entry`, routed via `eng_eval` promotion** (§17a, verified
on host: `40.0`, and `eng_has_entry()` then reports `true`):

```math
[single_app]
<[arith]

scale_k := 3
compute_main_val(x) = x * scale_k + 7
run_single() = compute_main_val(11)
@entry(run_single)
```

```c
Value out;
bool ok = eng_eval(eng, unit, &out);   /* promoted to the pipeline; entry's return is the result */
```

**State rules:** everything defined here — modules, operators, unit groups created by
`@dim`, structs — **persists in the user region** of the shared `Engine` until
`eng_flush_user_modules(eng)` is called (§5). This is what the calculator's flush-first
rule protects against.

---

## 3. Mode 3 — Multi-File Project

The frontend holds N files open; it hands all N strings to one call, **in any order**:

```c
/* char *files[N] — each file's full text, headers and all, unordered */
Value out;
bool ok = eng_build_and_run(eng, files, N, &out);
```

**Pipeline steps for `unit_count > 1`:**

1. **Stage:** each string goes through `ext_compile_unit` into the staging context.
   Duplicate modules (already loaded in `eng`, or staged twice) are rejected with a
   `Duplicate declaration` error at this step.
2. **Topological sort** (`ext_link_units`, Kahn's algorithm over the staged `<[dep]`
   graph by *module name* — never by file name, position, or declaration order).
   This is why callers may pass files in any order.
3. **Dependency-mode linking:** every unit that some other unit depends on is linked in
   *declaration-only* mode (`is_dep_load` / `skip_exprs = true`): its type/function/
   operator/struct declarations are assigned to the shared `SymTable`, but its
   **top-level statements are skipped entirely**. A library module's scratch lines
   (`eff_factor = 999` in the §17b test) never execute — dependency modules are
   declarations to you, not programs.
4. **Entry resolution — the entry ladder.** Exactly one entry is picked, first match wins:
   - an explicit **`@entry(fn)`** directive anywhere in the project;
   - a function named **`main()`** in any project module;
   - the **last statement value** of the *sink* unit (the one nothing depends on)
     as a fallback for scripts without function entries;
   - as a final fallback, the linked modules behave as a pure library bundle.
   The winner runs via `eng_run_entry(eng, &out)`; `out_val` is its return value.
   Guard: two modules may declare differently-*named* helpers freely, but a
   **duplicate `main()`** across modules is rejected at link time — unless an
   explicit `@entry` disambiguates (tested in §17b with `10` vs `20` from
   `[mod_m1]` / `[mod_m2]`).
5. **All-or-nothing linking:** staging, sorting, and linking are transactional. Any
   failure past the first stage rolls the `SymTable` back — a half-linked project is
   never observable.

**Worked example** (`Test/main.c` §17b, three files supplied in *reverse* dependency
order, verified on host: result `420.0` with unit group `force`):

```math
# ── file app.math ──────────────
[sim_app]
<[thruster]
<[nav_math]
<[units]

pub main() = {
    b := burn_cfg(50, 4)
    thrust_impulse(b) + nav_math::nav_offset * 1 N
}
```

```math
# ── file thruster.math ──────────────
[thruster]
<[nav_math]
<[units]

pub eff_factor := 2
pub thrust_impulse(c:burn_cfg) = (nav_product(c) * eff_factor) * 1 N

# Library-mode scratch — NEVER executes in dependency mode:
eff_factor = 999
```

```math
# ── file nav.math ──────────────
[nav_math]
<[arith]

$burn_cfg(rate:real, duration:real)
priv secret_bias(x) = x + 5
pub nav_offset = 10
pub nav_product(c:burn_cfg) = secret_bias(c.rate * c.duration)
```

```c
const char *files[3] = { SRC_APP, SRC_THRUSTER, SRC_NAV };   /* deliberately unordered */
Value out;
if (!eng_build_and_run(eng, files, 3, &out)) {
    fprintf(stderr, "project error: %s\n", eng_last_error(eng));  /* carries (line N) */
} else {
    /* out = 420 N (unit group [force]); main() was picked off the entry ladder */
}
```

What happened: `[nav_math]` and `[thruster]` were both *required* by `[sim_app]`, so
both were linked in declaration-only mode — `eff_factor = 999` never executed — and
`main()` was selected from the entry ladder, ran in the `[sim_app]` scope, and returned
`burn_cfg(50,4)` → `nav_product` = `secret_bias(50·4) + 5` = `205`, `thrust_impulse` =
`205 · 2 N` = `410 N`, plus `nav_math::nav_offset · 1 N` = `10 N`: **`420.0`, group
`force`**.

---

## 4. The One Pipeline, Side by Side

| Step | Mode 1 | Mode 2 (`unit_count == 1`) | Mode 3 (`unit_count > 1`) |
|------|--------|---------------------------|---------------------------|
| Route-in | `eng_eval` fast path | `eng_eval` auto-promote, or direct `eng_build_and_run` | `eng_build_and_run` |
| Stage (`ext_compile_unit`) | — | 1 unit | N units, order irrelevant |
| Topological sort (`ext_link_units`) | — | trivial | Kahn's, by `<[dep]` name |
| Dependency-mode decl-only linking | — | refs stdlib only | every depended-on module |
| Run strategy | one `eval_ast` | top-to-bottom, last value | entry ladder → `eng_run_entry` |
| `@entry`/`main()` | impossible | wins over last value | wins over sinks |
| Persists in user region | nothing | everything defines | everything defines, all modules |
| Rollback on failure | n/a | transactional | transactional |

---

## 5. Shared-State & Flush Rules (modes interacting over one `Engine`)

Because modes 2 and 3 mutate the *same* `SymTable` the fast path reads, the frontend
owns the discipline:

### 5.1 Flush-first (`eng_flush_user_modules`)
The calculator rule (verified in `Ecalc` `MathEngineNormalizer.kt:522`): before any
Mode 1 evaluation, if a project/module was previously linked, call

```c
eng_flush_user_modules(eng);   /* O(1): rolls back to base-layer watermarks */
```

otherwise the engine is left untouched (stdlib + patches only). The result: the
calculator always evaluates on a clean base, and never accidentally binds to a
user-defined operator left over from a Studio session.

### 5.2 `import "file.math"` inside running code
`ext_import_module_or_file` (`Core/compile/compiler_slice.c`) resolves an `import`
during chunk execution: unquoted identifiers resolve against already-loaded modules
(O(1)); quoted paths fall to the frontend via `eng_import_file`/`eng_set_import_dir`
after a `.math` extension is appended if missing. Verified pipeline (`Test/main.c`
§17c): a helper file

```math
[imp_helper_mod]
<[arith]
<[units]

pub helper_force(a:real, force, b:real) = √((a ^ 2) + (b ^ 2))
```

is written to the import dir, then imported by another project file mid-script
(`helper_force(3 N, 4 N)` → `5 N`) whose `main()` also mixes in pure stdlib units:
the import loaded *inside* the running pipeline, during chunk execution.

### 5.3 Hybrid: loaded module + loaded project
A module file loaded earlier (via `user_compile_ext`) and a project linked later may
coexist in the user region; `import` may bridge them. Only `eng_flush_user_modules`
tears either down.

---

## 6. The `.mathc` Replay Path (off-line compilation)

Everything above compiles *live*. The same staging/linking machinery can also emit a
binary image, which the app or a host can later load in O(1) instead of recompiling
source:

```c
/* compile + link N sources and emit one binary image */
if (!user_compile_and_link(eng, ctx, files, N, out_buf, out_len)) { ... }

/* later, any engine, any process: */
user_load_mathc(eng2, out_buf, out_len);
```

This is exactly how the shipped `precompiled/stdlib.mathc` is produced (`make stdlib`,
byte-identical by design) and re-loaded by every Mode in §0. End-to-end replay is
exercised by `Test/main.c` §17b: a linked multi-file project is serialized with
`ext_save_mathc_mem`, loaded into a *fresh* engine with `user_load_mathc`, and
`eng_has_entry` → `eng_run_entry` replays it (`420.0` again) with no source after that
first load.

---

## 7. Patches (`eng_apply_patches`) and the Flush Boundary

Operator-level patches (e.g. the App's currency patch, `patches/currency.math`) sit
**below the flush line** — they belong to the *baseline*, not the user region:

```c
const char *patches[1] = { CURRENCY_PATCH_TEXT };
bool ok = eng_apply_patches(eng, patches, 1);
bool still_ok = eng_flush_user_modules(eng)      /* flush erases Modes 2/3 state, */
             && eng_eval(eng, "triforce(7)", &out) /*  ... but patches survive.  */;

```

Verified on the host engine: a `pub` helper defined via `eng_apply_patches` still
resolves after `eng_flush_user_modules`. The App relies on the same guarantee by a
different route — it bakes the bundled patches together with the stdlib into one
baseline image (`filesDir/math_patches/compiled_state.mathc`, built by
`getUniversalPatchedStdlibBytes`) and loads *that* as the base layer, so flushes
(`nativeFlushUserModules`, with a re-load of the patched image as fallback) can never
undo patches, only modules and projects. After a flush the App re-registers
host externs (`@currency_convert` etc.) since callbacks live outside the image.

---

## 8. Error Reporting Across Pipelines

Every mode reports failure the same way — `false` return + `eng_last_error(eng)`.
Messages produced by the chunk pipeline (Modes 2 and 3) may carry a ` (line N)` suffix
appended by `mem_err_attach_line` (`Core/Code/mem_arena.c`) at the chunk-execution
sites of `compiler_slice.c`:

- **runtime error** in a top-level chunk → the chunk's 1-based file-absolute start line;
- **parse error** (e.g. trailing token) → the **token-precise** line of the offending token;
- a message already containing `line ` is never re-suffixed.

Line numbers are file-absolute within each unit: line 1 is that unit's first line.
Frontends extract with `(?i)\bline\s+(\d+)\b` (the App's `errorLineExtractRegex`).

Examples, verified on the host engine:

```
Unit mismatch for '+' with (real [length], real [mass]) (line 6)
Unexpected trailing token '4' (line 9)
```

Chunk granularity note: consecutive statements with no blank line between them form
one chunk group; a runtime error inside a group attributes the group's start line.
Separate statements with blank lines get exact lines — the normal editor style.

---

## 9. Quick Reference — Which API For Which Job

| Goal | Call |
|------|------|
| Evaluate one expression | `eng_eval(eng, expr, &out)` |
| Run one `.math` file (script or module) | `eng_build_and_run(eng, &src, 1, &out)` or let `eng_eval` promote |
| Run N project files unordered | `eng_build_and_run(eng, sources, N, &out)` |
| Reset to the `stdlib + patches` baseline | `eng_flush_user_modules(eng)` |
| Run a previously-linked project again | `eng_has_entry(eng)` → `eng_run_entry(eng, &out)` |
| Import one file on demand | `eng_import_file(eng, path)` (frontend resolves the path) |
| Apply on-top patches | `eng_apply_patches(eng, patches, count)` |
| Precompile to binary & re-load | `user_compile_and_link(...)` → `user_load_mathc(eng, img, len)` |
| Read captured `println`-style output | `eng_get_output(eng)` / `eng_clear_output(eng)` |
| Human-readable failure | `eng_last_error(eng)` (` (line N)` suffix where applicable) |

The mental model in one sentence: **Mode 1 parses and evaluates one expression without
ever touching the pipeline; Mode 2 is that same unified pipeline handed one string;
Mode 3 is that same pipeline handed many strings plus a topological sort and an
entry function.**
