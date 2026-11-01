/*
 * math_engine_jni.h — Ready-to-Use Android NDK JNI Bridge for Cmath Engine (MEP)
 *
 * Location: Core/jni/math_engine_jni.h
 *
 * Purpose:
 *   Provides JNI entry points (`Java_com_mathengine_MathEngineBridge_*`) for
 *   Android NDK (Java / Kotlin) host applications, plus a lightweight host-testable
 *   fallback JNI environment when compiled outside the Android NDK (`<jni.h>`
 *   auto-detected via `__has_include(<jni.h>)`).
 */

#ifndef MATH_ENGINE_JNI_H
#define MATH_ENGINE_JNI_H

#include "../Headers/math_engine.h"
#include "../compile/user_loader.h"
#include "../session/session.h"
#include "../session/snapshot.h"

#if defined(__has_include) && __has_include(<jni.h>)
#include <jni.h>
#else
/* Portable fallback JNI types for standalone desktop / CI verification builds */
#include <stdint.h>
typedef int32_t  jint;
typedef int64_t  jlong;
typedef uint8_t  jboolean;
typedef uint8_t  jbyte;
typedef int32_t  jsize;
typedef void    *jobject;
typedef void    *jclass;
typedef const char *jstring;
typedef struct { jsize len; uint8_t *data; } JniMockByteArray;
typedef JniMockByteArray *jbyteArray;

typedef struct JniMockEnv {
    const char *(*GetStringUTFChars)(struct JniMockEnv **env, jstring s, jboolean *isCopy);
    void        (*ReleaseStringUTFChars)(struct JniMockEnv **env, jstring s, const char *utf);
    jstring     (*NewStringUTF)(struct JniMockEnv **env, const char *bytes);
    jsize       (*GetArrayLength)(struct JniMockEnv **env, jbyteArray arr);
    jbyte      *(*GetByteArrayElements)(struct JniMockEnv **env, jbyteArray arr, jboolean *isCopy);
    void        (*ReleaseByteArrayElements)(struct JniMockEnv **env, jbyteArray arr, jbyte *elems, jint mode);
    jbyteArray  (*NewByteArray)(struct JniMockEnv **env, jsize len);
    void        (*SetByteArrayRegion)(struct JniMockEnv **env, jbyteArray arr, jsize start, jsize len, const jbyte *buf);
} JniMockEnv;

typedef JniMockEnv *JNIEnv;
#define JNI_TRUE  1
#define JNI_FALSE 0
#define JNI_ABORT 2
#define JNIEXPORT
#define JNICALL

/* Initialize a portable mock JNIEnv for desktop unit testing of all JNI entry points */
JNIEnv *mep_jni_mock_env(void);
void    mep_jni_mock_free_byte_array(jbyteArray arr);
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 1. Engine Lifecycle */
JNIEXPORT jlong JNICALL
Java_com_mathengine_MathEngineBridge_nativeCreate(JNIEnv *env, jclass clazz);

JNIEXPORT jlong JNICALL
Java_com_mathengine_MathEngineBridge_nativeCreateBlank(JNIEnv *env, jclass clazz);

JNIEXPORT void JNICALL
Java_com_mathengine_MathEngineBridge_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle);

/* 2. Stateless Evaluation (`"OK|<type>|<unit>|<value>"` or `"ERR|<message>"`) */
JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeEval(JNIEnv *env, jclass clazz,
                                                jlong handle, jstring expr);

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeLastError(JNIEnv *env, jclass clazz, jlong handle);

/* 3. Parallel In-Memory Sessions & Order-Tagged Snapshots */
JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionOpen(JNIEnv *env, jclass clazz, jlong handle);

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionClose(JNIEnv *env, jclass clazz,
                                                        jlong handle, jint sessionId);

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionReset(JNIEnv *env, jclass clazz,
                                                        jlong handle, jint sessionId);

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionEval(JNIEnv *env, jclass clazz,
                                                       jlong handle, jint sessionId,
                                                       jstring expr);

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionSnapshotCount(JNIEnv *env, jclass clazz,
                                                                jlong handle, jint sessionId);

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionSnapshotAt(JNIEnv *env, jclass clazz,
                                                             jlong handle, jint sessionId,
                                                             jint index);

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionExportSnapshots(JNIEnv *env, jclass clazz,
                                                                  jlong handle, jint sessionId);

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionRestore(JNIEnv *env, jclass clazz,
                                                          jlong handle, jstring newlineJoinedSnapshots);

/* 4. User Extension Compilation, `.mathc` Binary Cache & O(1) Module Toggling */
JNIEXPORT jbyteArray JNICALL
Java_com_mathengine_MathEngineBridge_nativeCompileExt(JNIEnv *env, jclass clazz,
                                                      jlong handle, jstring mathSource);

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeLoadMathc(JNIEnv *env, jclass clazz,
                                                     jlong handle, jbyteArray mathcBytes);

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeSetModule(JNIEnv *env, jclass clazz,
                                                     jlong handle, jstring modName,
                                                     jboolean enabled);

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeIsModuleActive(JNIEnv *env, jclass clazz,
                                                          jlong handle, jstring modName);

/* 5. Symbolic Algebra & Scratch Arena Mark-Sweep Compaction */
JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymSimplify(JNIEnv *env, jclass clazz,
                                                       jlong handle, jstring expr);

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymDiff(JNIEnv *env, jclass clazz,
                                                   jlong handle, jstring varName,
                                                   jstring expr);

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymInteg(JNIEnv *env, jclass clazz,
                                                    jlong handle, jstring varName,
                                                    jstring expr);

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymSolve(JNIEnv *env, jclass clazz,
                                                    jlong handle, jstring varName,
                                                    jstring eq);

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeCompactScratch(JNIEnv *env, jclass clazz, jlong handle);

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeScratchPoolUsed(JNIEnv *env, jclass clazz, jlong handle);

/* 5b. Host-Bridge Externs: app-backed Kotlin/Java `@<name>(...)` functions.
 * `callback.apply(name, numbers, strings)` returns the numeric result. */
JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeRegisterExtern(JNIEnv *env, jclass clazz,
                                                          jlong handle, jstring name,
                                                          jobject callback);

#ifdef __cplusplus
}
#endif

#endif /* MATH_ENGINE_JNI_H */
