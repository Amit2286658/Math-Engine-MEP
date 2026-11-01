# Math Engine (MEP)

A self-contained, zero-dependency C11 scientific / symbolic / dimensional
computation engine. C owns the grammar, English-free data owns the meaning:
the entire scientific library is plain **`.math`** source compiled into a
binary `.mathc` image and embedded at boot — so the numbers you ship are the
numbers the host typed.

This readme is just a map. The real documentation lives in `docs/` (under `Main/`):

| doc | what it is |
|---|---|
| [`docs/complete engine architecture.md`](Main/docs/complete%20engine%20architecture.md) | full architecture: runtime model, memory, sessions/snapshots, compile+link pipeline, C API |
| [`docs/comprehensive language guide.md`](Main/docs/comprehensive%20language%20guide.md) | full language: every construct, entity type, unit group, constant, and **all 1385 overloads** with live-tested examples |

---

## 1. Getting started

All project source and build steps live inside `Main/` — run the commands
below from that directory.

Build (CMake, static library + CLI):

```console
$ cmake -S . -B DebugBuild
$ cmake --build DebugBuild -j4
```

Evaluate expressions from the command line (one session, sequential lines):

```console
$ ./DebugBuild/myapp "1 + 2 * 3" "u := 5 km; u in m" "(1...6).filter(x => x % 2 == 0)"
7
5000 [distance]
[2, 4, 6]
```

Interactive REPL — with `:vars :diff :integ :solve :simplify :load :modules`
and friends:

```console
$ ./DebugBuild/myapp --repl
MEP Interactive Session REPL (19 modules, 1385 overloads, 30 constants)
mep> 25km in m
mep> exit
```

Throughput benchmark:

```console
$ ./DebugBuild/myapp --bench
```

Test suite (574 expression cases + engine-lifecycle/memory/thread suites):

```console
$ ./DebugBuild/myapp            # runs the full internal suite
```

Embedding in C: include `Main/Core/Headers/math_engine.h`, link the built
`libmathengine.a`, then `eng_init` → `eng_eval` / `eng_session_open` → …
(see the architecture doc, §C API).

A 30-second taste of the language (each line below right off the CLI):

```console
$ ./DebugBuild/myapp "25km in m" "(50m)i" "ode_rk4(t, y, -y, 0, 1, 1)"
25000 [distance]
50i [distance]
0.3678794412
```

That third line is RK4 solving `dy/dt = -y` from `y(0) = 1` to `t = 1` —
`e^-1` from a unit-aware solver. Passing several expressions runs them in
the same session, in order.

---

## 2. Architecture in five sentences

1. Zero built-ins: even `+` and `( )` are registered by `.math` modules at
   boot — the precompiled **`.mathc` image** loads in O(1) `memcpy` time
   (boot ≈ 0.4–0.7 ms for 19 modules).
2. Everything is 8 bytes: `sizeof(Value) == sizeof(AstNode) == 8` (NaN-boxed
   values with orthogonal type + physical-unit tags; 16-bit-offset AST).
3. Values carry **7-D SI dimensional algebra** (`@dim(m, kg, s, A, K, mol,
   cd)`), so `10 N * 2 m` is dimension-checked against `5 J` arithmetically.
4. State comes in 8 session slots with a **hash-chained MSP1 snapshot** per
   evaluated line and full-history replay restore.
5. Compilation is data flow, not magic: source text → AST → serialized
   `.mathc` (48-byte header) → topological link → live symbols; user
   extensions compile and hot-swap through the exact same path
   (`user_compile_ext` / `user_load_mathc`).

Details, memory layout, snapshot format, and the C API:
→ [`docs/complete engine architecture.md`](Main/docs/complete%20engine%20architecture.md)

---

## 3. The language in five paragraphs

- **Core syntax**: statement `;`, assignment `:=` (+ `+= -= *= /= %= ^=`),
  pipeline `x |> f`, containers `vec() list<T>() stack queue map`,
  comprehensions `[f(i) for i in 1..n]` with `if` filters, loop statement
  `(i, x in coll) { ... }`, `switch` pattern matching, lambdas `x => ...`
  and `((a, b) => ...)`.
- **Units** are metadata on every number: `25km`, `10 mps`, `310.15 K`;
  convert with the `in` operator or `to_*` members (`5km in mi`),
  inspect with `unit_of / unit_dim / same_dim`.
- **Scientific shelves**: arithmetic/number theory/bitwise, trig +
  Gamma/error functions, complex algebra (`i` postfix, `cexp_i`), 1D–4D
  tensors with LU/QR/SVD/eig/pinv, Cramer/quad/poly algebra (symbolic CAS
  included: `sym_diff sym_integ sym_solve sym_taylor`), calculus with
  RK4 ODEs, statistics/OLS, strings + regex + `fmt`, and the classroom
  four — physics, chemistry, biology, conversions.
- **Polymorphism** is table-driven: each function carries its overloads
  with expected types/units/defaults (`norm_pdf(x, mu = 0, s = 1)`), and
  `.math` can pin its own precedence/associativity (`:<N` / `:>N`).
- **Robustness set**: errors are exact-message strings
  (`error: Undefined symbol or constant 'mod'`), `0/0` traps loudly,
  session snapshots keep a tamper-evident audit trail.

Full details, every overload, and per-module catalogs:
→ [`docs/comprehensive language guide.md`](Main/docs/comprehensive%20language%20guide.md)

---

## 4. Repository layout

```
Main/Core/       engine core (parser, evaluator, compiler/linker, sessions,
                 snapshots, hardcode grammar layer, @-intrinsics registry)
Main/Library/    the .math standard library source files + mathc compiler
Main/Test/       myapp CLI, 574-case test suite, user extension sample
Main/docs/       the two deep references (architecture, language)
Main/precompiled/ separated precompiled Android engine (arm64-v8a/libmathengine.so) + stdlib.mathc
Main/patches/     incremental .math extension patches & manifest.json
```
