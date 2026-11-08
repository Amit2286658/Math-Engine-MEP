package com.mathengine;

/**
 * Thread-Safe Android Java Wrapper for the C11 Math Engine (MEP).
 *
 * All instance methods are `synchronized` and verify that the native handle is
 * open before invoking JNI so concurrent `close()` and `eval()` calls across
 * threads can never race or trigger a native use-after-free.
 */
public final class MathEngineBridge implements AutoCloseable {
    static {
        System.loadLibrary("mathengine");
    }

    public static final class EvalResult {
        public final boolean ok;
        public final String type;
        public final String unit;
        public final String value;
        public final String error;

        private EvalResult(boolean ok, String type, String unit, String value, String error) {
            this.ok    = ok;
            this.type  = type;
            this.unit  = unit;
            this.value = value;
            this.error = error;
        }

        public static EvalResult parseWire(String wire) {
            if (wire == null) {
                return new EvalResult(false, "void", "none", "", "Null JNI response");
            }
            if (wire.startsWith("OK|")) {
                int p1 = wire.indexOf('|', 3);
                int p2 = (p1 > 0) ? wire.indexOf('|', p1 + 1) : -1;
                if (p1 > 0 && p2 > p1) {
                    return new EvalResult(
                        true,
                        wire.substring(3, p1),
                        wire.substring(p1 + 1, p2),
                        wire.substring(p2 + 1),
                        null
                    );
                }
            }
            String err = wire.startsWith("ERR|") ? wire.substring(4) : wire;
            return new EvalResult(false, "void", "none", "", err);
        }
    }

    private long handle;

    public MathEngineBridge() {
        this.handle = nativeCreate();
        if (this.handle == 0L) {
            throw new IllegalStateException("Failed to initialize native MEP Engine");
        }
    }

    /**
     * Initializes a blank native MEP Engine and loads a separate precompiled
     * `stdlib.mathc` binary image in O(1) `memcpy` time.
     */
    public MathEngineBridge(byte[] stdlibMathcBytes) {
        if (stdlibMathcBytes == null || stdlibMathcBytes.length == 0) {
            throw new IllegalArgumentException("stdlib.mathc byte array must not be null or empty");
        }
        long h = nativeCreateBlank();
        if (h == 0L) {
            throw new IllegalStateException("Failed to allocate blank native MEP Engine");
        }
        if (!nativeLoadMathc(h, stdlibMathcBytes)) {
            String err = nativeLastError(h);
            nativeDestroy(h);
            throw new IllegalStateException("Failed to load stdlib.mathc: " + err);
        }
        this.handle = h;
    }

    private long requireOpenHandle() {
        if (handle == 0L) {
            throw new IllegalStateException("MathEngineBridge has already been closed");
        }
        return handle;
    }

    public synchronized EvalResult eval(String expr) {
        return EvalResult.parseWire(nativeEval(requireOpenHandle(), expr));
    }

    public synchronized EvalResult evalModule(String moduleName, String expr) {
        return EvalResult.parseWire(nativeModuleEval(requireOpenHandle(), moduleName, expr));
    }

    public synchronized boolean resetModule(String moduleName) {
        return nativeModuleReset(requireOpenHandle(), moduleName);
    }

    public synchronized boolean flushUserModules() {
        return nativeFlushUserModules(requireOpenHandle());
    }

    public synchronized EvalResult buildAndRun(String[] sources) {
        return EvalResult.parseWire(nativeBuildAndRun(requireOpenHandle(), sources));
    }

    public synchronized double[] samplePlot(String modName, String expr, double xMin, double xMax, int samples) {
        return nativeSamplePlot(requireOpenHandle(), modName, expr, xMin, xMax, samples);
    }

    public synchronized String exportCatalog() {
        return nativeExportCatalog(requireOpenHandle());
    }

    public synchronized String formatModuleVars(String moduleName) {
        return nativeModuleFormatVars(requireOpenHandle(), moduleName);
    }

    public synchronized String exportModuleSource(String moduleName) {
        return nativeModuleExportSource(requireOpenHandle(), moduleName);
    }

    public synchronized byte[] compileExtension(String mathSource) {
        return nativeCompileExt(requireOpenHandle(), mathSource);
    }

    public synchronized byte[] applyPatch(String patchSource) {
        return nativeApplyPatch(requireOpenHandle(), patchSource);
    }

    public synchronized boolean hasEntry() {
        return nativeHasEntry(requireOpenHandle());
    }

    public synchronized EvalResult runEntry() {
        return EvalResult.parseWire(nativeRunEntry(requireOpenHandle()));
    }

    public synchronized boolean loadMathc(byte[] mathcBytes) {
        return nativeLoadMathc(requireOpenHandle(), mathcBytes);
    }

    public synchronized boolean setModule(String moduleName, boolean enabled) {
        return nativeSetModule(requireOpenHandle(), moduleName, enabled);
    }

    public synchronized boolean isModuleActive(String moduleName) {
        return nativeIsModuleActive(requireOpenHandle(), moduleName);
    }

    public synchronized String simplify(String expr) {
        return nativeSymSimplify(requireOpenHandle(), expr);
    }

    public synchronized String diff(String varName, String expr) {
        return nativeSymDiff(requireOpenHandle(), varName, expr);
    }

    public synchronized String integ(String varName, String expr) {
        return nativeSymInteg(requireOpenHandle(), varName, expr);
    }

    public synchronized String solve(String varName, String eq) {
        return nativeSymSolve(requireOpenHandle(), varName, eq);
    }

    public synchronized int compactScratch() {
        return nativeCompactScratch(requireOpenHandle());
    }

    public synchronized int scratchPoolUsed() {
        return nativeScratchPoolUsed(requireOpenHandle());
    }

    public synchronized void setImportDir(String dirPath) {
        nativeSetImportDir(requireOpenHandle(), dirPath);
    }

    public synchronized String getOutput() {
        return nativeGetOutput(requireOpenHandle());
    }

    public synchronized void clearOutput() {
        nativeClearOutput(requireOpenHandle());
    }

    public synchronized String lastError() {
        return nativeLastError(requireOpenHandle());
    }

    @Override
    public synchronized void close() {
        if (handle != 0L) {
            nativeDestroy(handle);
            handle = 0L;
        }
    }

    public static native long    nativeCreate();
    public static native long    nativeCreateBlank();
    public static native void    nativeDestroy(long handle);
    public static native String  nativeEval(long handle, String expr);
    public static native String  nativeLastError(long handle);
    public static native String  nativeModuleEval(long handle, String modName, String expr);
    public static native boolean nativeModuleReset(long handle, String modName);
    public static native boolean nativeFlushUserModules(long handle);
    public static native String  nativeBuildAndRun(long handle, String[] sources);
    public static native double[] nativeSamplePlot(long handle, String modName, String expr, double xMin, double xMax, int samples);
    public static native String  nativeExportCatalog(long handle);
    public static native String  nativeModuleFormatVars(long handle, String modName);
    public static native String  nativeModuleExportSource(long handle, String modName);
    public static native byte[]  nativeCompileExt(long handle, String mathSource);
    public static native byte[]  nativeApplyPatch(long handle, String patchSource);
    public static native boolean nativeHasEntry(long handle);
    public static native String  nativeRunEntry(long handle);
    public static native boolean nativeLoadMathc(long handle, byte[] mathcBytes);
    public static native boolean nativeSetModule(long handle, String modName, boolean enabled);
    public static native boolean nativeIsModuleActive(long handle, String modName);
    public static native String  nativeSymSimplify(long handle, String expr);
    public static native String  nativeSymDiff(long handle, String varName, String expr);
    public static native String  nativeSymInteg(long handle, String varName, String expr);
    public static native String  nativeSymSolve(long handle, String varName, String eq);
    public static native int     nativeCompactScratch(long handle);
    public static native int     nativeScratchPoolUsed(long handle);
    public static native void    nativeSetImportDir(long handle, String dirPath);
    public static native String  nativeGetOutput(long handle);
    public static native void    nativeClearOutput(long handle);
    public static native boolean nativeRegisterExtern(long handle, String name, ExternCallback callback);

    /**
     * Kotlin/Java-backed host extern callback, invoked by `@<name>(...)` calls
     * inside `.math` extensions (e.g. a pulled `[currency]` patch).
     *
     * `numbers` holds every numeric argument, `strings` every string argument,
     * both in original call order; return the numeric result as a `double`.
     * Returning `NaN` (or throwing) surfaces as an engine error string.
     */
    public interface ExternCallback {
        double apply(String name, double[] numbers, String[] strings);
    }
}
