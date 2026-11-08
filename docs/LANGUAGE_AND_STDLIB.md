# Math-Engine-MEP `.math` Language & Standard Library Reference

Every construct and example in this document has been verified directly against the C11 engine (`./DebugBuild/myapp`).

---

## 1. Module Structure, Visibility & Mutability

### Module Headers & Dependencies
A `.math` file can declare one or more named modules with `[module_name]` and import public declarations from other modules with `<[dependency_name]`:

```math
[custom_ops]
<[arith]
<[trig]

priv const DEG_SCALE = π / 180
priv deg_to_r(x: real) = x * DEG_SCALE

pub const G_STD = 9.80665
pub hypot3(a: real, b: real, c: real) = √(a^2 + b^2 + c^2)
pub deg_sin(x: real) = sin(deg_to_r(x))
```

- **Visibility (`pub` vs. `priv`)**: Top-level declarations default to `pub`. Prefixing a declaration with `priv` restricts that variable, constant, type, or function overload to the declaring module. Cross-module qualified access is supported via `mod_name::symbol`.
- **Mutability (`:=` / `mut` vs. `=` / `const`)**:
  - `x := 10` or `mut x = 10` creates a mutable variable that can be reassigned (`x = 20`, `x += 5`).
  - `const c = 10` or `c = 10` (on a new module binding) creates an immutable binding that rejects reassignment and in-place container mutation.
- **3-Tier Lexical Scoping**:
  1. Local `{ ... }` scope blocks and function/lambda parameters
  2. Current module variables and constants
  3. Imported public module variables and constants (`<[dep]`)

---

## 2. Standalone Scripts vs. Multi-File Projects

Math-Engine-MEP uses a single unified compilation and linking pipeline (`eng_build_and_run` / `eng_eval`) that handles both single-file scripts and multi-file projects:

1. **Standalone Single-File Execution (`unit_count == 1`)**:
   - All top-level declarations (`struct`, `@dim`, functions, operators, module variables) and top-level expressions execute top-to-bottom.
   - If the file also defines `@entry(fn_name)` or a zero-argument `main()` function, the engine automatically invokes that entry function after top-level initialization and returns its result.
2. **Multi-File Project Execution (`unit_count > 1`)**:
   - All files are staged into `ExtBuildCtx` and topologically sorted by `<[dep]` dependencies (detecting missing dependencies, cyclic imports, and duplicate module names).
   - Library modules are linked in **declaration-only mode** (`skip_exprs = true`), initializing types, functions, operators, and module variables while skipping stray scratch expressions.
   - Finally, the single project-wide `@entry(fn_name)` or `pub main()` function is executed via `eng_run_entry`.

**Tested Multi-File Example:**
```math
# File 1: custom_ops.math
[custom_ops]
<[arith]
<[trig]
priv const DEG_SCALE = π / 180
pub const G_STD = 9.80665
pub deg_sin(x: real) = sin(x * DEG_SCALE)
```
```math
# File 2: physics_sim.math
[physics_sim]
<[custom_ops]
<[units]
<[conversions]

pub v0 := 25.0
pub angle := 45.0
pub range_m := (v0^2 * deg_sin(2 * angle)) / G_STD
pub range_km := (range_m * 1m) into km

pub height_at(t: real) = v0 * deg_sin(angle) * t - 0.5 * G_STD * t^2

pub main() = {
    println($"Projectile range={range_m} m ({range_km} km)")
    height_at(1.8)
}
```
Output:
```text
Projectile range=63.73226331 m (0.06373226331 km)
15.93303215
```

---

## 3. Physical Units (`length`, 7D SI Algebra) & `into` Conversion

### 7D SI Dimensional System (`Library/units.math`)
Units are defined via `@dim(unit_name, m, kg, s, A, K, mol, cd)` and bound as postfix operators:
- **`length`**: `m`, `km`, `cm`, `mm`, `um`, `nm`, `pm`, `fm`, `angstrom`, `mi`, `nmi`, `yd`, `ft`, `inch`, `mil`, `au`, `ly`, `pc`
- **`area`**: `m2`, `km2`, `cm2`, `mm2`, `ha`, `acre`, `ft2`, `in2`
- **`volume`**: `m3`, `cm3`, `mm3`, `L`, `mL`, `uL`, `gal`, `qt`, `pt`, `cup`, `fl_oz`
- **`time`**: `s`, `sec`, `ms`, `us`, `ns`, `ps`, `min`, `hr`, `day`, `week`, `yr`
- **`mass`**: `kg`, `g`, `mg`, `ug`, `ng`, `ton`, `lb`, `oz`, `ct`, `amu`, `u`
- **`speed` & `accel`**: `mps`, `kph`, `mph`, `kn`, `fps`, `c_0`, `mps2`, `g_0`, `Gal`
- **`force`, `energy`, `power`, `pressure`**: `N`, `kN`, `MN`, `dyn`, `lbf`, `kgf`, `J`, `kJ`, `MJ`, `mJ`, `erg`, `cal`, `kcal`, `Wh`, `kWh`, `eV`, `keV`, `MeV`, `GeV`, `W`, `kW`, `MW`, `GW`, `mW`, `hp`, `Pa`, `hPa`, `kPa`, `MPa`, `GPa`, `bar`, `mbar`, `atm`, `torr`, `mmHg`, `psi`
- **`electricity`, `temperature`, `amount`, `angle`, `data`**: `A`, `mA`, `uA`, `C`, `mC`, `uC`, `V`, `mV`, `kV`, `ohm`, `kohm`, `Mohm`, `S`, `F`, `mF`, `uF`, `nF`, `pF`, `H`, `mH`, `uH`, `Wb`, `T`, `G`, `K`, `mK`, `degC`, `degF`, `mol`, `mmol`, `umol`, `nmol`, `M_conc`, `mM_conc`, `rad`, `deg`, `°`, `arcmin`, `arcsec`, `turn`, `sr`, `bit`, `byte`, `B`, `KB`, `MB`, `GB`, `TB`, `KiB`, `MiB`, `GiB`, `TiB`

### Unit Conversion with `into` (`Library/conversions.math`)
The `into` operator converts any unit-tagged scalar or vector into a compatible target unit of the same physical dimension, whereas `in` is used strictly for Boolean membership and loop iteration:

| Expression | Verified Result | Type & Unit |
| :--- | :--- | :--- |
| `2km into m` | `2000` | `real [length]` |
| `(2km + 500m) into m` | `2500` | `real [length]` |
| `180deg into rad` | `3.141592654` | `real [angle]` |
| `(36 kph) * (10 sec)` | `100` | `real [length]` |
| `unit_of((36 kph) * (10 sec))` | `"length"` | `str` |
| `((1000 kg) * (9.8 mps2)) into kN` | `9.8` | `real [force]` |
| `x : real, length := 50m ; x += 50cm ; x` | `50.5` | `real [length]` |
| `(212 degF) into K` | `373.15` | `real [temperature]` |

---

## 4. Ranges, Boolean Membership (`in`), Comprehensions & Loops

- **Ranges**:
  - Inclusive: `1...5` -> `[1, 2, 3, 4, 5]`
  - Half-open: `1..<5` -> `[1, 2, 3, 4]`, `1>..5` -> `[2, 3, 4, 5]`, `1>..<5` -> `[2, 3, 4]`
  - Stepped: `(0...10) by 2` -> `[0, 2, 4, 6, 8, 10]`
  - Exclusion/Skip ranges: `1..<5>..10` (skips `5`), `1..<25...75>..100` (skips `25...75`)
- **Boolean Membership (`in`)**:
  - `5 in 1...10` -> `1`
  - `10 in 1..<10` -> `0`
  - `"k" in map("k", 42)` -> `1`
- **Loops & Comprehensions**:
  - Range/collection loop: `s := 0 ; (i in 1...5) { s += i } ; s` -> `15`
  - Indexed loop: `v := vec(10, 20, 30) ; s := 0 ; (i, x in v) { s += (i + 1) * x } ; s` -> `140`
  - While loop: `x := 1 ; (x < 100) ?? { x *= 2 } ; x` -> `128`
  - Comprehension: `[x^2 for x in 1...6 if x % 2 == 0]` -> `[4, 16, 36]`

---

## 5. Control Flow, Pattern Matching (`switch`), Structs & Exceptions

- **Multi-Branch Conditionals (`if ... or ... else`)**:
  ```math
  x := 15
  if (x < 0) { "neg" } or (x == 0) { "zero" } else { "pos" }   # -> "pos"
  ```
- **Pattern-Matched `switch`**:
  Supports exact values, ranges (`1...3`), relational guards (`case > 10:`), struct/vector destructuring (`case vec2(0, y):`, `case [a, b]:`), and `if` guards:
  ```math
  switch (5) {
      case 1...3: "low"
      case 4...6: "mid"
      else: "high"
  }   # -> "mid"
  ```
- **Composite Structs (`struct Name(...)` / `$Name(...)`)**:
  ```math
  struct Point(x: real, y: real)
  p := Point(3, 4)
  √(p.x^2 + p.y^2)   # -> 5
  ```
- **Exceptions (`try / catch / throw`)**:
  ```math
  try { 1 / 0 } catch (e) { $"Caught: {e}" }   # -> "Caught: Division by zero"
  ```

---

## 6. Vectors, Matrices, Maps & Functional Pipelines (`|>`)

| Category | Tested Example | Result |
| :--- | :--- | :--- |
| **Pipe & HOFs** | `(1...10) \|> filter(x => x % 2 == 0) \|> map(x => x^2) \|> sum()` | `220` |
| **Matrix Det & Trace** | `A := [1, 2 ; 3, 4] ; det(A) * 10 + trace(A)` | `-15` |
| **Linear Solver** | `linsolve([2, 1 ; 5, 3], [4, 11])` | `[1, 2]` |
| **Eigenvalues** | `eigvals([2, 1 ; 1, 2])` | `[3, 1]` |
| **Dictionary (`$map`)** | `m := map("a", 10, "b", 20) ; m["c"] = 30 ; m["a"] + m["c"]` | `40` |

---

## 7. Symbolic CAS & Numerical Calculus (`Library/calculus.math`)

| Operation | Tested Expression | Verified Output |
| :--- | :--- | :--- |
| **Symbolic Simplify** | `sym_simplify((x + 0) * 1 + 2 * x)` | `"3 * x"` |
| **Symbolic Derivative** | `sym_diff(x, x^3 + sin(x))` | `"3 * x ^ 2 + cos(x)"` |
| **Symbolic Integral** | `sym_integ(x, 3 * x^2 + cos(x))` | `"x ^ 3 + sin(x)"` |
| **Symbolic Roots** | `sym_solve(x, x^2 - 5 * x + 6 == 0)` | `"[2, 3]"` |
| **Taylor Series** | `sym_taylor(x, exp(x), 0, 3)` | `"1 + x + 0.5 * x ^ 2 + 0.1666666667 * x ^ 3"` |
| **Numerical Integral** | `integrate(x, x^2, 0, 3)` | `9` |
| **Root Finder** | `solve(x, x^2 - 2 == 0, 1.5)` | `1.414213562` |
| **RK4 ODE Solver** | `ode_rk4(t, y, y, 0, 1, 1)` | `2.718281828` |

---

## 8. Standard Library Modules Overview (`Main/Library/`)

| File | Module(s) | Purpose |
| :--- | :--- | :--- |
| `bootstrap.math` | `[boot_syntax]`, `[boot_types]`, `[boot_control]` | Core delimiters, `$real`/`$iota`/`$str`/`$vec`/`$mat`/`$map`/`$fn` types, control flow, `type_of`, `unit_of`, `unit_dim`, file & JSON helpers |
| `arith.math` | `[arith]` | Arithmetic operators, bitwise ops, combinatorics (`ncr`, `npr`), `gcd`, `lcm`, `is_prime`, complex numbers (`i`) |
| `trig.math` | `[trig]` | Trigonometric, inverse, and hyperbolic functions (`sin`, `cos`, `tan`, `sec`, `cosec`, `cot`, `asin`, `sinh`, etc.) |
| `complex.math` | `[complex_ext]` | Complex conjugates, polar `cis`, `phase`, complex `exp`/`ln`/`sin`/`cos`/`sqrt` |
| `vec.math` | `[vec]` | Dynamic vectors, `list`/`stack`/`queue`, statistics (`mean`, `stddev`, `median`, `variance`), `map`/`filter`/`reduce`/`sort`, `map()` dictionaries |
| `mat.math` | `[mat]` | 1D-4D matrices/tensors, `det`, `trace`, `inv`, `linsolve`, `lu_*`, `qr_*`, `chol`, `eigvals`, `eigvecs`, `svd_*`, `pinv`, `lstsq` |
| `algebra.math` | `[algebra]` | Quadratic/cubic roots (`quad_roots`, `cubic_disc`), `horner`, `polyval`, `polyderiv`, `polyinteg`, `polyfit` |
| `calculus.math` | `[calculus]` | Symbolic CAS (`sym_*`) and numerical calculus (`diff`, `integrate`, `lim`, `sigma`, `prod`, `solve`, `fmin`, `fmax`, `ode_*`) |
| `units.math` | `[units]` | 7D SI physical dimensions (`length`, `mass`, `time`, `force`, `energy`, `power`, `pressure`, etc.) and unit algebra |
| `conversions.math` | `[conversions]` | Universal `into` unit conversion operator (`2km into m`) and `to_<unit>` postfix converters |
| `physics.math` | `[physics]` | Physical constants (`c_light`, `G_grav`, `h_planck`, `k_boltz`, `N_avogadro`) and mechanics/electromagnetism/relativity formulas |
| `chemistry.math` | `[chemistry]` | Ideal gas law, pH/pOH, Arrhenius, Nernst, half-life, molarity, and `@extern` periodic table hooks |
| `biology.math` | `[biology]` | Logistic growth, Michaelis-Menten kinetics, Hardy-Weinberg, Shannon diversity, and BMI/BMR equations |
| `geom.math` | `[geom_ext]` | `$vec2`, `$vec3`, `$mat2`, `$circle`, `$line2`, `$plane3`, `$sphere` structs and geometric intersection/projection ops |
| `string.math` | `[string]` | String manipulation (`len`, `trim`, `upper`, `lower`, `split`, `join`, `replace`), regex (`regex_match`, `regex_find`, `regex_replace`), `csv_to_maps` |

## String Interpolation & Currency Sugar (`"{25$ into RS}"`)

Both standard double-quoted strings (`"..."`) and explicit `$"..."` / `f"..."` strings support inline `{expr}` and `{expr:spec}` expression evaluation, including numeric literals, physical units, and currency conversions:

```math
println("{25$ into RS}")              # Evaluates 25$ into RS and prints the converted INR value
printl("{25$ into RS}")               # `printl` is supported as a convenience alias for `println`
println("Distance: {2km into m} m")   # Prints: Distance: 2000 m
println("Formatted: {π:.4f}")         # Prints: Formatted: 3.1416
```

Supported currency identifiers in `[currency]` (`patches/currency.math`):
- **USD**: `$`, `USD`, `usd`
- **INR**: `₹`, `RS`, `Rs`, `rs`, `INR`, `inr`
- **EUR**: `€`, `EUR`, `eur`
- **GBP**: `£`, `GBP`, `gbp`
- **JPY**: `¥`, `JPY`, `jpy`


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
