# Math Engine (MEP) — Comprehensive Android Integration Guide

This reference documents every supported way to integrate the C11 **Math Engine (MEP)** into an Android application or library project, how to invoke core engine and symbolic capabilities from Kotlin, Java, and native C/C++, how to pass scalars, physical units, vectors, matrices, structs, and dictionaries back and forth across the JNI boundary, how to expose Android platform services (sensors, storage, network data) into `.math` scripts via `@extern` callbacks, and how to manage stateful sessions, `.mathc` binary caching, threading, and Android `ViewModel` lifecycles.

## 1. Android Runtime Architecture & Design Guarantees

MEP is engineered specifically to avoid the common pitfalls of embedding native scientific or symbolic engines on Android:

| Architectural Property | Implementation in `Core/` & `JNI/` | Android Benefit |
| :--- | :--- | :--- |
| **Zero External Native Dependencies** | Pure ISO C11 linking only `-lm` (`libm`). | No Boost, GMP, Eigen, or C++ STL ABI mismatches (`libc++_shared.so`) across NDK versions. |
| **Zero-I/O Instant Boot (`~0.4 ms`)** | The engine boots blank (`eng_init()` / `eng_create()`), and the host app loads `assets/precompiled/stdlib.mathc` via `nativeLoadMathc()` in one `memcpy` pass — the standard library ships as a separate `.mathc` image, not an embedded header. | No runtime `.math` text parsing at app startup; stdlib image and engine binary can ship/refresh independently. |
| **Heap-Allocated Engine Context (`eng_create`)** | `sizeof(Engine)` is `2,079,904` bytes (`~2.03 MB`). `nativeCreate()` calls `eng_create()` on the native heap and returns the pointer as a 64-bit `jlong` handle. | Never places large arenas on Android's `1 MB`–`8 MB` JVM or `pthread` stacks. |
| **Zero Shared Mutable Global State** | Every `Engine` instance owns its own `MemArena`, `SymTable`, `EngSession[8]` table, and `mathc_buf` serialization buffer. | Multiple `Engine` instances can run in parallel across Kotlin coroutine worker threads without global locks. |
| **Synchronized Use-After-Free Protection** | `MathEngineBridge` (Java) and `MathEngineKt` (Kotlin) mark all instance methods `synchronized` / `@Synchronized` and validate `requireOpenHandle()` (`handle != 0L`). | Concurrent `close()` and `eval()` calls across UI and background threads cannot race or trigger native use-after-free faults. |
| **Android CheckJNI UTF-8 Sanitization** | `jni_sanitize_utf8_inplace()` in `Core/jni/math_engine_jni.c` validates multi-byte UTF-8 sequences and trims truncated trailing bytes before calling `NewStringUTF`. | Prevents Android CheckJNI abort crashes on malformed or truncated multi-byte UTF-8 strings. |
| **SBO Arena + Bounded Heap Spill** | Containers up to `64` doubles (`512 B`) stay in `MemArena.f64_pool`; larger containers (`> 64` up to `262,144` doubles) spill to managed `HeapBlock`s and auto-reclaim on evaluation exit. | Predictable native memory footprint with automatic garbage collection on scope exit or `compactScratch()`. |

## 2. Four Ways to Integrate MEP into an Existing Android Project

Depending on your project structure, build pipeline, and whether you also write custom C/C++ code, you can integrate MEP using any of four patterns.

| Integration Method | Best For | Build Speed | Native Customization |
| :--- | :--- | :--- | :--- |
| **Method 1: Source-Level CMake (`externalNativeBuild`)** | Apps that want single-step Gradle builds or custom `@extern` C callbacks | Compiles C sources once per ABI (cached by Gradle) | Full access to `Core/` C API and `eng_register_extern` |
| **Method 2: Prebuilt `.so` in `jniLibs/`** | Existing Kotlin/Java apps that do not want NDK/CMake in their Gradle build | Instant (zero native compilation during Gradle sync/build) | Uses standard `MathEngineBridge` JNI API |
| **Method 3: Multi-Module Android Library (`.aar`)** | Multi-module architectures (`:app`, `:core:math`, `:feature:calculator`) or internal SDK distribution | Prepackaged `.aar` with bundled `.so` + Kotlin/Java classes + ProGuard rules | Encapsulated behind module boundary |
| **Method 4: Direct Native C/C++ Static Linking (`libmathengine.a`)** | NDK games, audio/DSP engines (Oboe), physics simulations, or existing C++ libraries | Linked directly into your existing `libapp.so` | Zero JNI string overhead; direct 8-byte `Value` C API |

### 2.1 Method 1: Source-Level CMake Integration (`externalNativeBuild`)

This is the most flexible setup. Android Studio and Gradle compile the C11 sources directly into `libmathengine.so` for every target ABI (`arm64-v8a`, `armeabi-v7a`, `x86_64`) and package it automatically into your APK/AAB.

#### Step 1 — Project File Placement

Copy the `Core/` directory and `CMakeLists.txt` into `app/src/main/cpp/mathengine/` (or reference the repository root directly), and copy `JNI/MathEngineBridge.java` and `JNI/MathEngineBridge.kt` into `app/src/main/java/com/mathengine/`:

```text
your-android-project/
└── app/
    ├── build.gradle.kts
    ├── proguard-rules.pro
    └── src/main/
        ├── cpp/
        │   ├── CMakeLists.txt                   # Or point directly to repo CMakeLists.txt
        │   └── mathengine/
        │       ├── CMakeLists.txt               # Root CMakeLists.txt from MathEngine_Isolated_Core_Lib
        │       └── Core/
        │           ├── Code/*.c
        │           ├── Headers/*.h
        │           ├── compile/*.c, *.h
        │           ├── jni/math_engine_jni.c, .h
        │           ├── lib/delegate/delegate.c, .h
        │           ├── lib/hardcode/hardcode.c, .h
        │           └── session/*.c, *.h
        └── java/
            └── com/mathengine/
                ├── MathEngineBridge.java
                └── MathEngineBridge.kt
```

> **Package Name Requirement**: `Core/jni/math_engine_jni.c` exports JNI symbols named `Java_com_mathengine_MathEngineBridge_native*`. Keep `MathEngineBridge.java` and `MathEngineBridge.kt` in `package com.mathengine` so the JVM resolves all 23 native methods automatically without manual `RegisterNatives` boilerplate.

#### Step 2 — Configure `app/build.gradle.kts`

Enable `externalNativeBuild` and pass `-DMATH_ENGINE_BUILD_SHARED=ON` so `CMakeLists.txt` builds `libmathengine.so` as a `SHARED` library instead of a static archive:

```kotlin
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.example.scientificapp"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.example.scientificapp"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        ndk {
            // Ship 64-bit ARM, 32-bit ARM, and x86_64 emulator ABIs
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // Instructs MathEngine's CMakeLists.txt to produce libmathengine.so
                arguments += listOf(
                    "-DMATH_ENGINE_BUILD_SHARED=ON",
                    "-DCMAKE_BUILD_TYPE=Release"
                )
                cFlags += listOf("-O2", "-fPIC", "-fvisibility=hidden")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/mathengine/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}
```

### 2.2 Method 2: Prebuilt Shared Library (`.so`) + `jniLibs/` Integration

If your Android team does not want NDK/CMake running during Gradle builds, you can cross-compile `libmathengine.so` ahead of time using the repository's built-in `make ndk` target and check the resulting `.so` binaries into `app/src/main/jniLibs/`.

#### Step 1 — Cross-Compile `libmathengine.so` for All Android ABIs

Run the following commands from the `MathEngine_Isolated_Core_Lib` root using any Android NDK installation (`r23+` through `r27+`):

```bash
export ANDROID_NDK=/path/to/android-sdk/ndk/27.2.12479018

# Build optimized Release shared libraries for each target ABI
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
    cmake -S . -B "AndroidBuild/${ABI}" \
        -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="${ABI}" \
        -DANDROID_PLATFORM=android-24 \
        -DMATH_ENGINE_BUILD_SHARED=ON \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "AndroidBuild/${ABI}" -j4
done
```

Alternatively, using the GNU `makefile` directly (which builds both `libmathengine.a` and `libmathengine.so` when invoked without CMake, or via CMake when `-DMATH_ENGINE_BUILD_SHARED=ON` is set):

```bash
make ndk release ANDROID_NDK=/path/to/ndk ANDROID_ABI=arm64-v8a ANDROID_PLATFORM=24
```

#### Step 2 — Copy `.so` Binaries into `app/src/main/jniLibs/`

Place each `libmathengine.so` into its corresponding ABI directory and copy `MathEngineBridge.java` and `MathEngineBridge.kt` into `src/main/java/com/mathengine/`:

```text
app/src/main/
├── jniLibs/
│   ├── arm64-v8a/
│   │   └── libmathengine.so
│   ├── armeabi-v7a/
│   │   └── libmathengine.so
│   ├── x86_64/
│   │   └── libmathengine.so
│   └── x86/
│       └── libmathengine.so
└── java/com/mathengine/
    ├── MathEngineBridge.java
    └── MathEngineBridge.kt
```

No `externalNativeBuild` block is required in `build.gradle.kts`—Gradle automatically packages `src/main/jniLibs/<ABI>/libmathengine.so` into the APK/AAB, and `MathEngineBridge`'s `static { System.loadLibrary("mathengine"); }` initializer loads the right ABI at runtime.

### 2.3 Method 3: Standalone Android Library Module (`:mathengine` / `.aar`)

In multi-module Android projects, create a dedicated `:mathengine` Gradle library module (`com.android.library`) that encapsulates `Core/`, `MathEngineBridge.java`, `MathEngineBridge.kt`, and `consumer-rules.pro`.

#### `:mathengine/build.gradle.kts`

```kotlin
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.mathengine"
    compileSdk = 35

    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DMATH_ENGINE_BUILD_SHARED=ON")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
        }
    }
}
```

#### `:mathengine/consumer-rules.pro`

```proguard
# Keep JNI bridge class and all native method signatures for R8 / ProGuard
-keep class com.mathengine.MathEngineBridge { *; }
-keep class com.mathengine.MathEngineBridge$EvalResult { *; }
-keep class com.mathengine.MathEngineKt { *; }
```

Any feature module can then depend on `implementation(project(":mathengine"))` (or `implementation(files("libs/mathengine-release.aar"))`).

### 2.4 Method 4: Direct C/C++ Static Linking (`libmathengine.a`) in an Existing Native Library

If your app already has a C/C++ `CMakeLists.txt` (for example, `libapp_native.so` for a game engine, CAD viewer, audio synthesizer, or sensor filter), include MEP as a subdirectory and link `mathengine` statically (`MATH_ENGINE_BUILD_SHARED=OFF`, the default) into your native target:

```cmake
cmake_minimum_required(VERSION 3.22.1)
project("app_native" LANGUAGES C CXX)

# 1. Build MEP as a static library (libmathengine.a) + include its JNI entry points
set(MATH_ENGINE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(mathengine)

# 2. Build your app's own shared library and link mathengine into it
add_library(app_native SHARED
    app_native_bridge.cpp
)

# Force the linker to retain MathEngineBridge JNI symbols if you also use MathEngineBridge.java
target_link_libraries(app_native PRIVATE
    -Wl,--whole-archive mathengine -Wl,--no-whole-archive
    log
    m
)
```

> **Important Note on Library Name**: If you link `mathengine` inside `libapp_native.so` instead of producing a separate `libmathengine.so`, either load `System.loadLibrary("app_native")` before referencing `MathEngineBridge` (and update `System.loadLibrary("mathengine")` in `MathEngineBridge.java` to `"app_native"`), or build `mathengine` as its own shared library (`MATH_ENGINE_BUILD_SHARED=ON`) and link against it dynamically.

## 3. Calling Functions from App to Core (Complete JNI & Wrapper Reference)

MEP provides two ready-to-use JVM wrappers in `JNI/`:
- **`com.mathengine.MathEngineBridge`** (`JNI/MathEngineBridge.java`): Thread-safe `AutoCloseable` Java class exposing both `synchronized` instance methods and `public static native` JNI entry points.
- **`com.mathengine.MathEngineKt`** (`JNI/MathEngineBridge.kt`): Thread-safe `Closeable` Kotlin wrapper delegating to `MathEngineBridge`.

### 3.1 Complete API Mapping: Kotlin/Java Wrapper ↔ JNI C Entry Point ↔ Core C11 Function

| Category | `MathEngineKt` (Kotlin) | `MathEngineBridge` (Java) | JNI C Symbol (`Core/jni/math_engine_jni.c`) | Underlying Core C11 API |
| :--- | :--- | :--- | :--- | :--- |
| **Lifecycle** | `MathEngineKt()` (`init`) | `new MathEngineBridge()` / `nativeCreate()` | `Java_com_mathengine_MathEngineBridge_nativeCreate` | `eng_create()` |
| **Blank Init** | *(via `MathEngineBridge.nativeCreateBlank()`)* | `MathEngineBridge.nativeCreateBlank()` | `Java_com_mathengine_MathEngineBridge_nativeCreateBlank` | `eng_create_blank()` |
| **Teardown** | `close()` (or `.use { ... }`) | `close()` / `nativeDestroy(handle)` | `Java_com_mathengine_MathEngineBridge_nativeDestroy` | `eng_destroy(eng)` |
| **Stateless Eval** | `eval(expr: String): EvalResult` | `eval(String expr)` | `Java_com_mathengine_MathEngineBridge_nativeEval` | `eng_eval(eng, expr, &out)` |
| **Error Query** | *(via `EvalResult.error`)* | `lastError()` | `Java_com_mathengine_MathEngineBridge_nativeLastError` | `eng_last_error(eng)` |
| **Open Session** | `openSession(): Int` | `openSession()` | `Java_com_mathengine_MathEngineBridge_nativeSessionOpen` | `eng_session_open(eng)` |
| **Close Session** | `closeSession(sessionId: Int): Boolean` | `closeSession(int sessionId)` | `Java_com_mathengine_MathEngineBridge_nativeSessionClose` | `eng_session_close(eng, sid)` |
| **Reset Session** | `resetSession(sessionId: Int): Boolean` | `resetSession(int sessionId)` | `Java_com_mathengine_MathEngineBridge_nativeSessionReset` | `eng_session_reset(eng, sid)` |
| **Session Eval** | `evalSession(sessionId: Int, expr: String): EvalResult` | `evalSession(int sessionId, String expr)` | `Java_com_mathengine_MathEngineBridge_nativeSessionEval` | `eng_session_eval(eng, sid, expr, &out)` |
| **Snapshot Count** | *(via `MathEngineBridge.nativeSessionSnapshotCount`)* | `nativeSessionSnapshotCount(handle, sid)` | `Java_com_mathengine_MathEngineBridge_nativeSessionSnapshotCount` | `eng_session_snapshot_count(eng, sid)` |
| **Snapshot by Index** | *(via `MathEngineBridge.nativeSessionSnapshotAt`)* | `nativeSessionSnapshotAt(handle, sid, idx)` | `Java_com_mathengine_MathEngineBridge_nativeSessionSnapshotAt` | `eng_session_snapshot_at(eng, sid, idx)` |
| **Export Snapshots** | `exportSnapshots(sessionId: Int): String?` | `exportSessionSnapshots(int sessionId)` | `Java_com_mathengine_MathEngineBridge_nativeSessionExportSnapshots` | `eng_session_snapshot_rewind` + `eng_session_snapshot_next` |
| **Restore Session** | `restoreSession(snapshots: String): Int` | `restoreSession(String newlineJoinedSnapshots)` | `Java_com_mathengine_MathEngineBridge_nativeSessionRestore` | `eng_session_restore(eng, snaps, count)` |
| **Compile `.math`** | `compileExt(mathSource: String): ByteArray?` | `compileExtension(String mathSource)` | `Java_com_mathengine_MathEngineBridge_nativeCompileExt` | `user_compile_ext(eng, src, &data, &len)` |
| **Load `.mathc`** | `loadMathc(bytes: ByteArray): Boolean` | `loadMathc(byte[] mathcBytes)` | `Java_com_mathengine_MathEngineBridge_nativeLoadMathc` | `user_load_mathc(eng, bytes, len)` |
| **Toggle Module** | `setModule(name: String, enabled: Boolean): Boolean` | `setModule(String moduleName, boolean enabled)` | `Java_com_mathengine_MathEngineBridge_nativeSetModule` | `eng_set_module(eng, name, enabled)` |
| **Query Module** | *(via `MathEngineBridge`)* | `isModuleActive(String moduleName)` | `Java_com_mathengine_MathEngineBridge_nativeIsModuleActive` | `eng_is_module_active(eng, name)` |
| **CAS Simplify** | `simplify(expr: String): String?` | `simplify(String expr)` | `Java_com_mathengine_MathEngineBridge_nativeSymSimplify` | `eng_sym_simplify(eng, expr, out, cap)` |
| **CAS Derivative** | `diff(varName: String, expr: String): String?` | `diff(String varName, String expr)` | `Java_com_mathengine_MathEngineBridge_nativeSymDiff` | `eng_sym_diff(eng, var, expr, out, cap)` |
| **CAS Integral** | `integ(varName: String, expr: String): String?` | `integ(String varName, String expr)` | `Java_com_mathengine_MathEngineBridge_nativeSymInteg` | `eng_sym_integ(eng, var, expr, out, cap)` |
| **CAS Solve** | `solve(varName: String, eq: String): String?` | `solve(String varName, String eq)` | `Java_com_mathengine_MathEngineBridge_nativeSymSolve` | `eng_sym_solve(eng, var, eq, out, cap)` |
| **Scratch GC** | `compactScratch(): Int` | `compactScratch()` | `Java_com_mathengine_MathEngineBridge_nativeCompactScratch` | `eng_compact_scratch(eng)` |
| **Scratch Used** | *(via `MathEngineBridge`)* | `scratchPoolUsed()` | `Java_com_mathengine_MathEngineBridge_nativeScratchPoolUsed` | `eng_scratch_pool_used(eng)` |

### 3.2 How the JNI Wire Protocol & `EvalResult` Work

Every call to `nativeEval` or `nativeSessionEval` returns a single pipe-delimited UTF-8 string constructed by `jni_format_eval_reply()` in `Core/jni/math_engine_jni.c`:

- **On Success**: `"OK|<type>|<unit>|<value>"`
- **On Error**: `"ERR|<exact diagnostic message>"`

`MathEngineBridge.EvalResult.parseWire(String wire)` parses this wire response in $O(1)$ substring slices into an immutable `EvalResult` object with five fields:

| `EvalResult` Field | Java / Kotlin Type | Meaning | Tested Example (`eval("ke(2 kg, 36 kph)")`) | Tested Example (`eval("100m + 60 deg")`) |
| :--- | :--- | :--- | :--- | :--- |
| `res.ok` | `boolean` / `Boolean` | `true` if evaluation succeeded, `false` on syntax/type/unit/domain error | `true` | `false` |
| `res.type` | `String` | Runtime entity type name (`"real"`, `"iota"`, `"complex"`, `"str"`, `"vec"`, `"mat"`, `"map"`, `"fn"`, `"vec2"`, `"vec3"`, `"circle"`, `"mat2"`, `"void"`, or custom `$struct`) | `"real"` | `"void"` |
| `res.unit` | `String` | Runtime physical unit group (`"none"`, `"distance"`, `"energy"`, `"speed"`, `"force"`, `"pressure"`, etc., or custom unit group) | `"energy"` | `"none"` |
| `res.value` | `String` | Canonical formatted value string (`"100"`, `"[1, 2, 3]"`, `"vec2(x=3, y=4)"`, `"3 + 4i"`) | `"100"` | `""` |
| `res.error` | `String?` | `null` on success; exact engine error string on failure | `null` | `"Unit mismatch for '+' with (real [distance], real [angle])"` |

#### Live-Tested Wire Format Examples

| Expression Sent from Kotlin/Java | Raw JNI Wire String Returned by C Core | Parsed `EvalResult` (`ok`, `type`, `unit`, `value`) |
| :--- | :--- | :--- |
| `"2 + 3 * 4"` | `"OK\|real\|none\|14"` | `ok=true, type="real", unit="none", value="14"` |
| `"ke(2 kg, 36 kph)"` | `"OK\|real\|energy\|100"` | `ok=true, type="real", unit="energy", value="100"` |
| `"(3 + 4i) * (1 - 2i)"` | `"OK\|complex\|none\|11 - 2i"` | `ok=true, type="complex", unit="none", value="11 - 2i"` |
| `"vec2(3, 4) + vec2(1, 2)"` | `"OK\|vec2\|none\|vec2(x=4, y=6)"` | `ok=true, type="vec2", unit="none", value="vec2(x=4, y=6)"` |
| `"linsolve([[2, 1], [5, 3]], [4, 11])"` | `"OK\|mat\|none\|[1, 2]"` | `ok=true, type="mat", unit="none", value="[1, 2]"` |
| `"100m + 60 deg"` | `"ERR\|Unit mismatch for '+' with (real [distance], real [angle])"` | `ok=false, error="Unit mismatch for '+' with (real [distance], real [angle])"` |

### 3.3 Calling Built-In, Symbolic, and Custom Functions from Kotlin & Java

#### Kotlin Example (`MathEngineKt`)

```kotlin
import com.mathengine.MathEngineKt

fun runCalculations() {
    MathEngineKt().use { engine ->
        // 1. Stateless physical-unit-aware calculation
        val keRes = engine.eval("ke(2 kg, 36 kph)")
        if (keRes.ok) {
            val joules = keRes.value.toDouble() // 100.0
            println("Kinetic Energy: $joules [${keRes.unit}] (type=${keRes.type})")
        } else {
            println("Error: ${keRes.error}")
        }

        // 2. Calling multi-argument numerical calculus / ODE solvers
        val odeRes = engine.eval("ode_rk4(t, y, -y, 0, 1, 1, 100)")
        println("y(1) = ${odeRes.value.toDouble()}") // 0.3678794412

        // 3. Direct Symbolic CAS calls (simplify, diff, integ, solve)
        val simplified = engine.simplify("(x + 0) * 1 + 4 * x")          // "5 * x"
        val derivative = engine.diff("x", "x^3 + sin(x)")               // "3 * x ^ 2 + cos(x)"
        val integral   = engine.integ("x", "4 * x^3 + cos(x)")          // "x ^ 4 + sin(x)"
        val roots      = engine.solve("x", "x^2 - 5 * x + 6 == 0")      // "[2, 3]"
        println("Symbolic: simp=$simplified, d/dx=$derivative, int=$integral, roots=$roots")
    }
}
```

#### Java Example (`MathEngineBridge`)

```java
import com.mathengine.MathEngineBridge;
import com.mathengine.MathEngineBridge.EvalResult;

public class CalculatorService {
    public void runDemo() {
        try (MathEngineBridge engine = new MathEngineBridge()) {
            // 1. Evaluate linear system solution
            EvalResult res = engine.eval("linsolve([[2, 1, -1], [-3, -1, 2], [-2, 1, 2]], [8, -11, -3])");
            if (res.ok) {
                System.out.println("Solution vector: " + res.value); // "[2, 3, -1]"
            }

            // 2. Toggle a module in O(1) time
            engine.setModule("biology", false);
            boolean active = engine.isModuleActive("biology"); // false
            engine.setModule("biology", true);
        }
    }
}
```

## 4. Passing Data Back and Forth Between Android and Core

There are three complementary ways to exchange structured data between your Android app and MEP:
1. **Expression & Wire Marshaling (Zero C Changes)**: Format Kotlin/Java inputs into MEP expressions or session variables, and parse `EvalResult` (`type`, `unit`, `value`) into Kotlin data models.
2. **Core-to-Host `@extern` Callbacks (Pulling Live Android Data into `.math`)**: Register C callbacks via `eng_register_extern()` so `.math` functions can directly read Android sensors, databases, or app state—either via a fast shared C struct updated from JNI or via a direct JNI upcall into Kotlin/Java.
3. **Custom Binary/Array JNI Extensions (Zero-Parse `DoubleArray` Transfer)**: Add custom JNI methods that construct `$vec` or `$mat` values directly in `MemArena` via `val_from_f64_vec()` and read contiguous `double*` memory via `mem_data_ptr_ro()`.

### 4.1 Passing Primitives, Units, Complex Numbers, Vectors, Matrices, Structs & Maps via `MathEngineKt`

Below is a complete, production-ready Kotlin marshaling helper (`MepDataCodec.kt`) showing how to pass every MEP data type into the engine and parse every returned `EvalResult` back into strongly typed Kotlin objects:

```kotlin
package com.mathengine

data class ComplexVal(val re: Double, val im: Double, val unit: String = "none")
data class Vec2Val(val x: Double, val y: Double, val unit: String = "none")
data class Vec3Val(val x: Double, val y: Double, val z: Double, val unit: String = "none")

object MepDataCodec {

    // -------------------------------------------------------------------------
    // 1. App -> Core: Formatting Kotlin types into safe MEP expressions
    // -------------------------------------------------------------------------

    fun formatScalar(value: Double, unitSuffix: String? = null): String =
        if (unitSuffix.isNullOrBlank()) value.toString() else "($value $unitSuffix)"

    fun formatString(text: String): String {
        val escaped = text
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\t", "\\t")
        return "\"$escaped\""
    }

    fun formatDoubleArray(values: DoubleArray): String =
        if (values.isEmpty()) "vec()" else values.joinToString(prefix = "vec(", postfix = ")")

    fun formatMatrix2D(rows: Array<DoubleArray>): String =
        rows.joinToString(prefix = "[", postfix = "]", separator = " ; ") { row ->
            row.joinToString(separator = ", ")
        }

    fun formatMap(entries: Map<String, Double>): String {
        if (entries.isEmpty()) return "map()"
        val args = entries.entries.joinToString(", ") { (k, v) -> "${formatString(k)}, $v" }
        return "dict($args)"
    }

    // -------------------------------------------------------------------------
    // 2. Core -> App: Parsing EvalResult back into Kotlin types
    // -------------------------------------------------------------------------

    fun parseDouble(res: MathEngineBridge.EvalResult): Double {
        require(res.ok) { "Engine error: ${res.error}" }
        return res.value.toDouble()
    }

    fun parseString(res: MathEngineBridge.EvalResult): String {
        require(res.ok && res.type == "str") { "Expected str, got ${res.type} (${res.error})" }
        val v = res.value
        return if (v.length >= 2 && v.startsWith("\"") && v.endsWith("\"")) {
            v.substring(1, v.length - 1)
        } else v
    }

    fun parseDoubleList(res: MathEngineBridge.EvalResult): List<Double> {
        require(res.ok && (res.type == "vec" || res.type == "mat")) {
            "Expected vec or 1D mat, got ${res.type}"
        }
        val body = res.value.trim().removeSurrounding("[", "]").trim()
        if (body.isEmpty()) return emptyList()
        return body.split(",").map { it.trim().toDouble() }
    }

    fun parseMatrix2D(res: MathEngineBridge.EvalResult): List<List<Double>> {
        require(res.ok && res.type == "mat") { "Expected mat, got ${res.type}" }
        val outer = res.value.trim().removePrefix("[").removeSuffix("]").trim()
        if (!outer.startsWith("[")) {
            // 1D matrix returned as single row
            return listOf(parseDoubleList(res))
        }
        return outer.split("],").map { rowStr ->
            rowStr.replace("[", "").replace("]", "").trim()
                .split(",")
                .filter { it.isNotBlank() }
                .map { it.trim().toDouble() }
        }
    }

    fun parseComplex(res: MathEngineBridge.EvalResult): ComplexVal {
        require(res.ok) { "Engine error: ${res.error}" }
        return when (res.type) {
            "real" -> ComplexVal(res.value.toDouble(), 0.0, res.unit)
            "iota" -> ComplexVal(0.0, res.value.removeSuffix("i").trim().toDouble(), res.unit)
            "complex" -> {
                // Format is "re + imi" or "re - imi"
                val norm = res.value.removeSuffix("i").trim()
                val plusIdx = norm.indexOf(" + ")
                if (plusIdx > 0) {
                    ComplexVal(
                        norm.substring(0, plusIdx).toDouble(),
                        norm.substring(plusIdx + 3).toDouble(),
                        res.unit
                    )
                } else {
                    val minusIdx = norm.indexOf(" - ", startIndex = 1)
                    ComplexVal(
                        norm.substring(0, minusIdx).toDouble(),
                        -norm.substring(minusIdx + 3).toDouble(),
                        res.unit
                    )
                }
            }
            else -> throw IllegalArgumentException("Cannot parse ${res.type} as ComplexVal")
        }
    }

    fun parseVec2(res: MathEngineBridge.EvalResult): Vec2Val {
        require(res.ok && res.type == "vec2") { "Expected vec2, got ${res.type}" }
        // Format: "vec2(x=3, y=4)"
        val inside = res.value.removePrefix("vec2(").removeSuffix(")")
        val parts = inside.split(",").associate {
            val (k, v) = it.trim().split("=")
            k.trim() to v.trim().toDouble()
        }
        return Vec2Val(parts.getValue("x"), parts.getValue("y"), res.unit)
    }
}
```

#### Passing Variables into a Session and Calling Functions Repeatedly

When calling a function repeatedly with different inputs from Kotlin, bind large inputs (like vectors, matrices, or dictionaries) once in a stateful session and invoke functions against those session variables:

```kotlin
MathEngineKt().use { engine ->
    val sid = engine.openSession()

    // 1. Pass a Kotlin DoubleArray and a custom inline function into the session
    val samples = doubleArrayOf(12.5, 15.0, 14.2, 18.9, 21.3, 19.8)
    engine.evalSession(sid, "data := ${MepDataCodec.formatDoubleArray(samples)}")
    engine.evalSession(sid, "z_score(v:vec, x) := (x - mean(v)) / stddev(v)")

    // 2. Call our session function from Kotlin and read back the result
    val zRes = engine.evalSession(sid, "round(z_score(data, 21.3), 4)")
    val zValue = MepDataCodec.parseDouble(zRes) // 1.3849

    // 3. Run OLS linear regression on (x, y) vectors and destructure slope & intercept
    engine.evalSession(sid, "xs := 1...6 ; slope := linreg_slope(xs, data) ; intercept := linreg_intercept(xs, data)")
    val predAt10 = MepDataCodec.parseDouble(engine.evalSession(sid, "linreg_predict(xs, data, 10)"))

    engine.closeSession(sid)
}
```

### 4.2 High-Speed Binary `DoubleArray` Transfer via Custom JNI (`val_from_f64_vec` & `mem_data_ptr_ro`)

If your Android app processes large arrays (e.g., 4,096-sample audio FFT frames or accelerometer time series) and you want to bypass string formatting completely, you can add a small JNI helper in `Core/jni/math_engine_jni.c` using MEP's C API (`val_from_f64_vec`, `mem_read_pool`, and `mem_data_ptr_ro`):

```c
/* Add to Core/jni/math_engine_jni.c */
JNIEXPORT jdoubleArray JNICALL
Java_com_mathengine_MathEngineBridge_nativeEvalVec(JNIEnv *env, jclass clazz,
                                                   jlong handle, jstring expr,
                                                   jdoubleArray inputVec) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !expr) return NULL;

    /* 1. Expose caller's jdoubleArray as `@android_vec()` extern in O(1) */
    /* Or evaluate `expr` directly and extract a returned `$vec` or `$mat` into a jdoubleArray: */
    const char *c_expr = (*env)->GetStringUTFChars(env, expr, NULL);
    if (!c_expr) return NULL;
    Value out = val_void();
    bool ok = eng_eval(eng, c_expr, &out);
    (*env)->ReleaseStringUTFChars(env, expr, c_expr);
    if (!ok) return NULL;

    uint16_t tid = eng_val_type(eng, out);
    uint16_t hdr_idx = val_get_pool_idx(out);

    if (tid == eng->mem.vec_type_id && tid != 0) {
        uint32_t len      = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 1));
        uint16_t data_idx = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 3));
        const double *ptr = mem_data_ptr_ro(&eng->mem, data_idx, 0);
        if (!ptr && len > 0) return NULL;

        jdoubleArray jarr = (*env)->NewDoubleArray(env, (jsize)len);
        if (jarr && len > 0) {
            (*env)->SetDoubleArrayRegion(env, jarr, 0, (jsize)len, ptr);
        }
        return jarr;
    } else if (tid == eng->mem.mat_type_id && tid != 0) {
        uint32_t total_len = (uint32_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 1));
        uint16_t data_idx  = (uint16_t)mem_read_pool(&eng->mem, (uint16_t)(hdr_idx + 2));
        const double *ptr  = mem_data_ptr_ro(&eng->mem, data_idx, 0);
        if (!ptr && total_len > 0) return NULL;

        jdoubleArray jarr = (*env)->NewDoubleArray(env, (jsize)total_len);
        if (jarr && total_len > 0) {
            (*env)->SetDoubleArrayRegion(env, jarr, 0, (jsize)total_len, ptr);
        }
        return jarr;
    }
    return NULL;
}
```

Notice how `mem_data_ptr_ro(&eng->mem, data_idx, 0)` transparently handles both **Small-Buffer-Optimized (`<= 64` doubles)** vectors stored inside `f64_pool` and **Heap-Spilled (`> 64` doubles)** vectors stored in `HeapBlock`s!

## 5. Calling from Core Back to Android (`@extern` Host Callbacks)

MEP allows `.math` code to call host functions written in C, C++, Java, or Kotlin via the `@<extern_name>(args...)` syntax and `eng_register_extern`:

```c
typedef bool (*EngExternFn)(MemArena *mem, const struct SymTable *tab,
                            const Value *args, uint8_t argc,
                            void *user_ctx, Value *out_val);

MATH_ENGINE_API bool eng_register_extern(Engine *eng, const char *name,
                                         EngExternFn fn, void *user_ctx);
```

### 5.1 Value Construction & Extraction Helpers Inside `@extern` Callbacks

Inside an `EngExternFn` callback, use the public helpers declared in `Core/Headers/mem_arena.h` and `Core/Headers/sym_table.h`:

| Direction | C Helper Function | Signature & Behavior |
| :--- | :--- | :--- |
| **Read `real` Arg** | `val_get_f64(mem, args[i])` | Extracts the `double` magnitude from a scalar `Value`. |
| **Read `str` Arg** | `val_as_cstr(mem, args[i], buf, sizeof(buf))` | Copies a `$str` argument into a NUL-terminated C buffer `buf`; returns `false` if `args[i]` is not a `$str`. |
| **Read Arg Type / Unit** | `val_get_type(mem, args[i])` / `val_get_unit(mem, args[i])` | Returns the 16-bit `type_id` or 8-bit `unit_id` of `args[i]`. |
| **Return `real`** | `*out_val = val_from_f64(mem, double_val)` | Constructs a dimensionless `real` `Value`. |
| **Return Unit-Tagged `real`** | `*out_val = val_make_f64_unit(mem, mem->fast_real_type, unit_id, d)` | Constructs a `real` `Value` tagged with `unit_id` (looked up via `sym_find_unit(tab, mem, "pressure", 8)`). |
| **Return `str`** | `*out_val = val_from_cstr(mem, "text")` | Interns `"text"` in `MemArena.src_buf` and returns a `$str` `Value`. |
| **Return `vec<real>`** | `*out_val = val_from_f64_vec(mem, double_arr, count)` | Allocates a `$vec` (in SBO `f64_pool` if `count <= 64`, or `HeapBlock` if `count > 64`) and copies `count` doubles into it. |
| **Raise Host Error** | `mem_err(mem, "Reason: %s", detail); return false;` | Aborts the current evaluation cleanly and surfaces `"ERR|Reason: ..."` to Kotlin/Java. |

### 5.2 Pattern A: Zero-Overhead Native Telemetry Cache (Recommended for Android Sensors / Audio / FX)

When bridging high-frequency Android sources (such as `SensorManager` accelerometer/barometer events, GPS coordinates, or live FX quotes), push updates from Kotlin into a lightweight per-engine or static C struct via JNI, and read that struct inside your `@extern` callback with **zero JNI upcall overhead** during expression evaluation.

#### C Implementation (`Core/jni/math_engine_jni.c` or your native bridge)

```c
typedef struct {
    double accel_xyz[3];
    double pressure_hpa;
} AndroidSensorState;

static AndroidSensorState g_sensor_state = { { 0.0, 0.0, 9.80665 }, 1013.25 };

static bool host_sensor_accel_cb(MemArena *mem, const SymTable *tab,
                                 const Value *args, uint8_t argc,
                                 void *user_ctx, Value *out_val) {
    (void)tab; (void)args;
    if (argc != 0) {
        mem_err(mem, "@sensor_accel expects 0 arguments");
        return false;
    }
    const AndroidSensorState *st = (const AndroidSensorState *)user_ctx;
    *out_val = val_from_f64_vec(mem, st->accel_xyz, 3);
    return !mem->has_error;
}

static bool host_sensor_pressure_cb(MemArena *mem, const SymTable *tab,
                                    const Value *args, uint8_t argc,
                                    void *user_ctx, Value *out_val) {
    (void)tab; (void)args;
    if (argc != 0) {
        mem_err(mem, "@sensor_pressure_pa expects 0 arguments");
        return false;
    }
    const AndroidSensorState *st = (const AndroidSensorState *)user_ctx;
    *out_val = val_from_f64(mem, st->pressure_hpa * 100.0);
    return true;
}

JNIEXPORT void JNICALL
Java_com_mathengine_MathEngineBridge_nativeUpdateSensors(
        JNIEnv *env, jclass clazz, jlong handle,
        jdouble ax, jdouble ay, jdouble az, jdouble pressureHpa) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    g_sensor_state.accel_xyz[0] = ax;
    g_sensor_state.accel_xyz[1] = ay;
    g_sensor_state.accel_xyz[2] = az;
    g_sensor_state.pressure_hpa = pressureHpa;
    if (eng) {
        eng_register_extern(eng, "sensor_accel",       host_sensor_accel_cb,    &g_sensor_state);
        eng_register_extern(eng, "sensor_pressure_pa", host_sensor_pressure_cb, &g_sensor_state);
    }
}
```

#### `.math` Extension Binding (`assets/sensors.math`)

```math
[android_sensors]
<[arith]
<[trig]
<[vec]
<[units]
<[conversions]
<[physics]

accel_vec() -> vec = @sensor_accel()

accel_g_mag() = {
    a := @sensor_accel()
    hypot(hypot(a[0], a[1]), a[2]) / g0
}

baro_pressure() -> real, pressure = with_unit(@sensor_pressure_pa(), 1 Pa)
```

#### Tested Output When Evaluated from Kotlin

| Expression | Engine Output (`EvalResult`) |
| :--- | :--- |
| `accel_vec()` | `OK\|vec\|none\|[0, 0, 9.80665]` |
| `accel_g_mag()` | `OK\|real\|none\|1` |
| `baro_pressure() to_atm` | `OK\|real\|none\|1` |

### 5.3 Pattern B: Direct Synchronous Upcall from C `@extern` into a Kotlin/Java Callback Interface

When a `.math` script needs to query an on-demand Kotlin service (such as Room/SQLite database lookups, `SharedPreferences`, or a Kotlin lambda), pass a `JNIEnv*` and a Kotlin callback `jobject` through `user_ctx`:

```c
typedef struct {
    JNIEnv  *env;
    jobject  kotlin_provider;
    jmethodID on_query_mid; /* double onQueryMetric(String key) */
} JniUpcallCtx;

static bool host_kotlin_query_cb(MemArena *mem, const SymTable *tab,
                                 const Value *args, uint8_t argc,
                                 void *user_ctx, Value *out_val) {
    (void)tab;
    if (argc != 1) {
        mem_err(mem, "@app_metric expects 1 string argument");
        return false;
    }
    char key_buf[64];
    if (!val_as_cstr(mem, args[0], key_buf, sizeof(key_buf))) {
        mem_err(mem, "@app_metric argument must be a string");
        return false;
    }
    JniUpcallCtx *ctx = (JniUpcallCtx *)user_ctx;
    if (!ctx || !ctx->env || !ctx->kotlin_provider) {
        mem_err(mem, "Kotlin metric provider is not attached");
        return false;
    }
    JNIEnv *env = ctx->env;
    jstring jkey = (*env)->NewStringUTF(env, key_buf);
    jdouble res = (*env)->CallDoubleMethod(env, ctx->kotlin_provider, ctx->on_query_mid, jkey);
    (*env)->DeleteLocalRef(env, jkey);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        mem_err(mem, "Kotlin exception thrown in @app_metric('%s')", key_buf);
        return false;
    }
    *out_val = val_from_f64(mem, (double)res);
    return true;
}
```

> **Note on `.mathc` Binary Reloads**: Because `SymTable.externs` stores host callbacks by their C string name (`char name[32]`), any `@extern` registered via `eng_register_extern()` automatically survives `loadMathc()` reloads without needing to be re-registered.

## 6. Compiling, Caching & Hot-Swapping User `.math` Extensions on Android

MEP's `Core/` never performs file I/O directly. Instead, your Android app reads `.math` source strings (from `context.assets`, `context.filesDir`, or a user code editor) and passes them in-memory to `compileExt()`, which compiles and links the extension into the live `Engine` **and** returns a self-contained `.mathc` binary image (`ByteArray`) containing the **entire permanent engine state** (all stdlib modules + your newly linked user modules).

### 6.1 Fast `.mathc` Binary Caching Workflow on Android

Compiling a `.math` extension parses declarations and builds ASTs (~1–3 ms), whereas loading a compiled `.mathc` image via `loadMathc()` takes ~0.4 ms (`memcpy` + FNV-1a checksum and `intr_hash` validation).

```kotlin
import android.content.Context
import com.mathengine.MathEngineKt
import java.io.File

class ExtensionCacheManager(private val context: Context) {

    /**
     * Loads a cached .mathc binary image if valid; otherwise compiles the .math
     * asset source, links it into [engine], and writes the .mathc cache file.
     */
    fun loadOrCompileAssetExtension(
        engine: MathEngineKt,
        assetFileName: String,
        cacheFileName: String = "$assetFileName.mathc"
    ): Boolean {
        val cacheFile = File(context.codeCacheDir, cacheFileName)

        // 1. Try fast O(1) .mathc binary load first
        if (cacheFile.exists()) {
            val cachedBytes = cacheFile.readBytes()
            if (engine.loadMathc(cachedBytes)) {
                return true
            }
            // If checksum or intr_hash changed after an app update, delete stale cache
            cacheFile.delete()
        }

        // 2. Fallback: read .math text from assets, compile & link into engine
        val mathSource = context.assets.open(assetFileName).bufferedReader().use { it.readText() }
        val compiledBytes = engine.compileExt(mathSource) ?: return false

        // 3. Persist the validated .mathc binary for future app launches
        cacheFile.writeBytes(compiledBytes)
        return true
    }
}
```

### 6.2 Security Guarantees When Loading `.mathc` Files from Disk

Before `loadMathc()` (`ext_load_mathc_mem`) copies a single byte into `MemArena` or `SymTable`, `ext_validate_mathc_buf` performs a complete pre-copy audit:
1. Checks the 48-byte `MathcHeader` magic (`"MTHC" = 0x4348544D`), version (`1`), and exact payload size.
2. Verifies `hdr.intr_hash == delegate_intrinsic_table_hash()`, rejecting stale `.mathc` files if an app update changed the C `IntrinsicId` table.
3. Verifies the 32-bit **FNV-1a checksum** over the header and payload.
4. Bounds-checks every `StrSlice`, `TypeDef`, `FieldDef`, `OpOverload`, `ConstDef`, and every single `AstNode` index (`left`, `right`, `ref`, `sub`). If a cached `.mathc` file is corrupted on disk, `loadMathc()` returns `false` transactionally without modifying the live `Engine`.

## 7. Stateful Sessions, `MSP1` Snapshots & Android `ViewModel` Lifecycle

Each `Engine` supports up to **8 independent parallel sessions** (`ENG_MAX_SESSIONS = 8`, IDs `0..7`), where each session persists:
- Up to `16` top-level variables (`ENG_SESSION_MAX_VARS = 16`), including scalars, strings, structs (`$vec2`, `$complex`), containers (`$vec`, `$mat`, `$map`), and first-class closures/inline recursive functions (`f(x) := ...`).
- Concrete entity type locks (`x : real`) and physical unit locks (`d : distance`).
- Up to `32` hash-chained **`MSP1` evaluation snapshots** (`ENG_SESSION_MAX_EVALS = 32`).

### 7.1 Tamper-Evident `MSP1` Snapshot Format

Calling `engine.exportSnapshots(sessionId)` returns a newline-joined string of order-tagged `MSP1` records:

```text
MSP1|seq:1|prev:00000000|hash:1659369F|vars:3|dist_x:real:distance:real:distance=50;factor:real:none:*:*=3;q:vec:distance:*:*=list<real>(10, 20)|res:real:distance=50
MSP1|seq:2|prev:1659369F|hash:0F7E856E|vars:4|dist_x:real:distance:real:distance=60;factor:real:none:*:*=3;q:vec:distance:*:*=list<real>(20, 30);scale_fn:fn:none:*:*=a => a * factor|res:real:distance=60
```

Calling `engine.restoreSession(snapshots)` verifies the contiguous sequence (`seq:1 .. seq:N`), the `prev` FNV-1a chain hashes, and each line's `hash` checksum, allocates a fresh session slot populated with all variables, locks, containers, and closures, and returns the new `sessionId` (or `-1` if tampered or incomplete).

### 7.2 Complete Android `ViewModel` + `SavedStateHandle` + Coroutines Integration

```kotlin
package com.example.scientificapp

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.mathengine.MathEngineBridge
import com.mathengine.MathEngineKt
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

data class WorksheetUiState(
    val history: List<Pair<String, MathEngineBridge.EvalResult>> = emptyList(),
    val lastSymbolicResult: String? = null,
    val isBusy: Boolean = false
)

class MathWorksheetViewModel(
    private val savedStateHandle: SavedStateHandle,
    private val computeDispatcher: CoroutineDispatcher = Dispatchers.Default
) : ViewModel() {

    companion object {
        private const val KEY_SESSION_SNAPSHOTS = "mep_session_snapshots_v1"
    }

    private val engine = MathEngineKt()
    private var sessionId: Int = -1

    private val _uiState = MutableStateFlow(WorksheetUiState())
    val uiState: StateFlow<WorksheetUiState> = _uiState.asStateFlow()

    init {
        // Restore prior session across process death / configuration change if available
        val savedSnapshots = savedStateHandle.get<String>(KEY_SESSION_SNAPSHOTS)
        sessionId = if (!savedSnapshots.isNullOrBlank()) {
            val restoredId = engine.restoreSession(savedSnapshots)
            if (restoredId >= 0) restoredId else engine.openSession()
        } else {
            engine.openSession()
        }
    }

    fun evaluateLine(expr: String) {
        viewModelScope.launch {
            _uiState.value = _uiState.value.copy(isBusy = true)
            val result = withContext(computeDispatcher) {
                val res = engine.evalSession(sessionId, expr)
                if (res.ok) {
                    // Save updated MSP1 snapshot chain into SavedStateHandle
                    engine.exportSnapshots(sessionId)?.let { snaps ->
                        savedStateHandle[KEY_SESSION_SNAPSHOTS] = snaps
                    }
                }
                res
            }
            _uiState.value = _uiState.value.copy(
                history = _uiState.value.history + (expr to result),
                isBusy = false
            )
        }
    }

    fun differentiate(variable: String, expr: String) {
        viewModelScope.launch {
            val d = withContext(computeDispatcher) {
                engine.diff(variable, expr)
            }
            _uiState.value = _uiState.value.copy(lastSymbolicResult = d)
        }
    }

    fun resetWorksheet() {
        viewModelScope.launch(computeDispatcher) {
            engine.resetSession(sessionId)
            savedStateHandle.remove<String>(KEY_SESSION_SNAPSHOTS)
            _uiState.value = WorksheetUiState()
        }
    }

    override fun onCleared() {
        // Deterministic native heap cleanup (~2.03 MB Engine + any spilled HeapBlocks)
        if (sessionId >= 0) {
            engine.closeSession(sessionId)
            sessionId = -1
        }
        engine.close()
        super.onCleared()
    }
}
```

## 8. Threading, Memory Compaction, ProGuard/R8 & Troubleshooting Checklist

### 8.1 Concurrency & Threading Model

1. **Single Shared `MathEngineKt` Across Coroutines**:
   Every method on `MathEngineKt` and `MathEngineBridge` is `@Synchronized` / `synchronized`. You can safely call `engine.eval(...)` or `engine.evalSession(...)` from `Dispatchers.Default` or `Dispatchers.IO` without external mutexes.
2. **Parallel Worker Pool (Multi-Engine)**:
   Because MEP has **zero mutable static/global state**, if your app performs heavy parallel batch evaluations (e.g., plotting a 2D surface or running Monte Carlo simulations across 4 CPU cores), instantiate one `MathEngineKt()` per worker coroutine. All 4 engines will execute simultaneously with zero lock contention.

### 8.2 Memory Limits & Scratch Arena Compaction

- **Per-Evaluation Scratch Rewind**: Every `eval()` and `evalSession()` call automatically rewinds the scratch region (`perm_pool_top .. pool_top`) and frees any temporary `HeapBlock`s in $O(1)$ time.
- **Explicit Mark-Sweep Compaction**: Inside long-running single expressions, the evaluator automatically triggers sliding Mark-Sweep compaction (`mem_compact_scratch`) when `pool_top` approaches `MEM_MAX_F64_POOL` (`8,192` doubles) or `heap_active_count >= 64`. You can also trigger compaction manually from Kotlin via `engine.compactScratch()` or inside `.math` code via `gc_compact()`.
- **Session Rollover**: Each session stores up to `ENG_SESSION_MAX_EVALS = 32` snapshots. In an interactive calculator that exceeds 32 evaluations, either export snapshots and call `resetSession(sid)` (or re-seed the latest variables) when `nativeSessionSnapshotCount(handle, sid) == 32`.

### 8.3 Common Android Integration Pitfalls & Fixes

| Symptom / Error | Root Cause | Solution |
| :--- | :--- | :--- |
| `java.lang.UnsatisfiedLinkError: dlopen failed: library "libmathengine.so" not found` | CMake built `libmathengine.a` (static) instead of `.so`, or ABI is missing from `abiFilters`. | Pass `-DMATH_ENGINE_BUILD_SHARED=ON` in `build.gradle.kts` `externalNativeBuild.cmake.arguments`. |
| `java.lang.UnsatisfiedLinkError: No implementation found for long com.mathengine.MathEngineBridge.nativeCreate()` | `MathEngineBridge.java` was moved to a different package, or R8 stripped/renamed the class. | Keep `MathEngineBridge` in `package com.mathengine` and add `-keep class com.mathengine.MathEngineBridge { *; }` to `proguard-rules.pro`. |
| Stack overflow (`SIGSEGV`) when allocating `Engine` in custom C/C++ JNI code | Declaring `Engine eng;` (`2.03 MB`) as a local variable on a C stack frame. | Always allocate via `Engine *eng = eng_create();` on the heap and free via `eng_destroy(eng);`. |
| `loadMathc()` returns `false` after updating C intrinsics in a new app version | `.mathc` header `intr_hash` detected that the `IntrinsicId` table changed. | Delete the cached `.mathc` file in `context.codeCacheDir` and re-run `compileExt(mathSource)`. |
