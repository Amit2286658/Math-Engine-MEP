package com.mathengine

import com.ecalc.CurrencyConverter
import java.io.Closeable

/**
 * Thread-Safe Android Kotlin Idiomatic Wrapper for the C11 Math Engine (MEP).
 *
 * If [stdlibMathcBytes] is provided, boots from the separate precompiled
 * `stdlib.mathc` binary image (`nativeCreateBlank` + `nativeLoadMathc`);
 * otherwise falls back to `nativeCreate()`. Automatically hooks up the
 * real-time [CurrencyConverter] extern callbacks (`@currency_convert`, etc.).
 */
class MathEngineKt(stdlibMathcBytes: ByteArray? = null) : Closeable {
    private var handle: Long = if (stdlibMathcBytes != null && stdlibMathcBytes.isNotEmpty()) {
        val h = MathEngineBridge.nativeCreateBlank()
        if (h != 0L && MathEngineBridge.nativeLoadMathc(h, stdlibMathcBytes)) {
            h
        } else {
            if (h != 0L) MathEngineBridge.nativeDestroy(h)
            MathEngineBridge.nativeCreate()
        }
    } else {
        MathEngineBridge.nativeCreate()
    }

    init {
        check(handle != 0L) { "Failed to initialize native MEP Engine" }
        runCatching { CurrencyConverter.registerWithEngine(handle) }
    }

    private fun requireOpenHandle(): Long {
        check(handle != 0L) { "MathEngineKt has already been closed" }
        return handle
    }

    @Synchronized
    fun eval(expr: String): MathEngineBridge.EvalResult =
        MathEngineBridge.EvalResult.parseWire(MathEngineBridge.nativeEval(requireOpenHandle(), expr))

    @Synchronized
    fun evalModule(moduleName: String, expr: String): MathEngineBridge.EvalResult =
        MathEngineBridge.EvalResult.parseWire(
            MathEngineBridge.nativeModuleEval(requireOpenHandle(), moduleName, expr)
        )

    @Synchronized
    fun resetModule(moduleName: String): Boolean =
        MathEngineBridge.nativeModuleReset(requireOpenHandle(), moduleName)

    @Synchronized
    fun flushUserModules(): Boolean =
        MathEngineBridge.nativeFlushUserModules(requireOpenHandle())

    @Synchronized
    fun buildAndRun(sources: Array<String>): MathEngineBridge.EvalResult =
        MathEngineBridge.EvalResult.parseWire(
            MathEngineBridge.nativeBuildAndRun(requireOpenHandle(), sources)
        )

    @Synchronized
    fun samplePlot(modName: String = "default", expr: String, xMin: Double, xMax: Double, samples: Int = 180): DoubleArray? =
        MathEngineBridge.nativeSamplePlot(requireOpenHandle(), modName, expr, xMin, xMax, samples)

    @Synchronized
    fun exportCatalog(): String =
        MathEngineBridge.nativeExportCatalog(requireOpenHandle()) ?: "" 

    @Synchronized
    fun formatModuleVars(moduleName: String): String? =
        MathEngineBridge.nativeModuleFormatVars(requireOpenHandle(), moduleName)

    @Synchronized
    fun exportModuleSource(moduleName: String): String? =
        MathEngineBridge.nativeModuleExportSource(requireOpenHandle(), moduleName)

    @Synchronized
    fun compileExt(mathSource: String): ByteArray? =
        MathEngineBridge.nativeCompileExt(requireOpenHandle(), mathSource)

    @Synchronized
    fun applyPatch(patchSource: String): ByteArray? =
        MathEngineBridge.nativeApplyPatch(requireOpenHandle(), patchSource)

    @Synchronized
    fun hasEntry(): Boolean =
        MathEngineBridge.nativeHasEntry(requireOpenHandle())

    @Synchronized
    fun runEntry(): MathEngineBridge.EvalResult =
        MathEngineBridge.EvalResult.parseWire(MathEngineBridge.nativeRunEntry(requireOpenHandle()))

    @Synchronized
    fun loadMathc(bytes: ByteArray): Boolean =
        MathEngineBridge.nativeLoadMathc(requireOpenHandle(), bytes)

    @Synchronized
    fun setModule(name: String, enabled: Boolean): Boolean =
        MathEngineBridge.nativeSetModule(requireOpenHandle(), name, enabled)

    @Synchronized
    fun isModuleActive(name: String): Boolean =
        MathEngineBridge.nativeIsModuleActive(requireOpenHandle(), name)

    @Synchronized
    fun simplify(expr: String): String? =
        MathEngineBridge.nativeSymSimplify(requireOpenHandle(), expr)

    /**
     * Registers an app-backed extern callable as `@<name>(...)` from `.math`
     * code. The callback receives `(name, numbers, strings)` — numeric and
     * string arguments in original call order — and returns the numeric
     * result (`double`).
     */
    fun registerExtern(name: String, callback: MathEngineBridge.ExternCallback): Boolean =
        MathEngineBridge.nativeRegisterExtern(requireOpenHandle(), name, callback)

    @Synchronized
    fun diff(varName: String, expr: String): String? =
        MathEngineBridge.nativeSymDiff(requireOpenHandle(), varName, expr)

    @Synchronized
    fun integ(varName: String, expr: String): String? =
        MathEngineBridge.nativeSymInteg(requireOpenHandle(), varName, expr)

    @Synchronized
    fun solve(varName: String, eq: String): String? =
        MathEngineBridge.nativeSymSolve(requireOpenHandle(), varName, eq)

    @Synchronized
    fun compactScratch(): Int =
        MathEngineBridge.nativeCompactScratch(requireOpenHandle())

    @Synchronized
    fun setImportDir(dirPath: String) {
        MathEngineBridge.nativeSetImportDir(requireOpenHandle(), dirPath)
    }

    @Synchronized
    fun getOutput(): String =
        MathEngineBridge.nativeGetOutput(requireOpenHandle()) ?: ""

    @Synchronized
    fun clearOutput() {
        MathEngineBridge.nativeClearOutput(requireOpenHandle())
    }

    @Synchronized
    override fun close() {
        if (handle != 0L) {
            MathEngineBridge.nativeDestroy(handle)
            handle = 0L
        }
    }
}
