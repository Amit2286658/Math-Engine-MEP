package com.mathengine

import java.io.Closeable

/**
 * Thread-Safe Android Kotlin Idiomatic Wrapper for the C11 Math Engine (MEP).
 *
 * If [stdlibMathcBytes] is provided, boots from the separate precompiled
 * `stdlib.mathc` binary image (`nativeCreateBlank` + `nativeLoadMathc`);
 * otherwise falls back to `nativeCreate()`.
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
    }

    private fun requireOpenHandle(): Long {
        check(handle != 0L) { "MathEngineKt has already been closed" }
        return handle
    }

    @Synchronized
    fun eval(expr: String): MathEngineBridge.EvalResult =
        MathEngineBridge.EvalResult.parseWire(MathEngineBridge.nativeEval(requireOpenHandle(), expr))

    @Synchronized
    fun openSession(): Int = MathEngineBridge.nativeSessionOpen(requireOpenHandle())

    @Synchronized
    fun closeSession(sessionId: Int): Boolean =
        MathEngineBridge.nativeSessionClose(requireOpenHandle(), sessionId)

    @Synchronized
    fun resetSession(sessionId: Int): Boolean =
        MathEngineBridge.nativeSessionReset(requireOpenHandle(), sessionId)

    @Synchronized
    fun evalSession(sessionId: Int, expr: String): MathEngineBridge.EvalResult =
        MathEngineBridge.EvalResult.parseWire(
            MathEngineBridge.nativeSessionEval(requireOpenHandle(), sessionId, expr)
        )

    @Synchronized
    fun exportSnapshots(sessionId: Int): String? =
        MathEngineBridge.nativeSessionExportSnapshots(requireOpenHandle(), sessionId)

    @Synchronized
    fun restoreSession(snapshots: String): Int =
        MathEngineBridge.nativeSessionRestore(requireOpenHandle(), snapshots)

    @Synchronized
    fun compileExt(mathSource: String): ByteArray? =
        MathEngineBridge.nativeCompileExt(requireOpenHandle(), mathSource)

    @Synchronized
    fun loadMathc(bytes: ByteArray): Boolean =
        MathEngineBridge.nativeLoadMathc(requireOpenHandle(), bytes)

    @Synchronized
    fun setModule(name: String, enabled: Boolean): Boolean =
        MathEngineBridge.nativeSetModule(requireOpenHandle(), name, enabled)

    @Synchronized
    fun simplify(expr: String): String? =
        MathEngineBridge.nativeSymSimplify(requireOpenHandle(), expr)

    /**
     * Registers an app-backed extern callable as `@<name>(...)` from `.math`
     * code. The callback receives `(name, numbers, strings)` — numeric and
     * string arguments in original call order — and returns the numeric
     * result (`double`).
     *
     * Example (currency patch): register the tiny 2-argument host extern
     *   engine.registerExtern("currency_convert") { _, nums, strs ->
     *       fxProvider.rate(strs.first()) * nums[0]
     *   }
     * after which `convert(100, "EUR")` from the `[currency]` patch converts
     * 100 units of the app's base currency into EUR live.
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
    override fun close() {
        if (handle != 0L) {
            MathEngineBridge.nativeDestroy(handle)
            handle = 0L
        }
    }
}
