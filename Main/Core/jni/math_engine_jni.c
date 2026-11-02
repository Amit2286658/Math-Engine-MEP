/*
 * math_engine_jni.c — Ready-to-Use Android NDK JNI Bridge Implementation
 *
 * Location: Core/jni/math_engine_jni.c
 */

#include "math_engine_jni.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !(defined(__has_include) && __has_include(<jni.h>))
static char s_mock_str_ring[8][65536];
static int  s_mock_str_idx = 0;

static const char *mock_get_str(JniMockEnv **env, jstring s, jboolean *isCopy) {
    (void)env;
    if (isCopy) *isCopy = JNI_FALSE;
    return s;
}
static void mock_rel_str(JniMockEnv **env, jstring s, const char *utf) {
    (void)env; (void)s; (void)utf;
}
static jstring mock_new_str(JniMockEnv **env, const char *bytes) {
    (void)env;
    if (!bytes) return NULL;
    char *dst = s_mock_str_ring[s_mock_str_idx];
    s_mock_str_idx = (s_mock_str_idx + 1) & 7;
    snprintf(dst, sizeof(s_mock_str_ring[0]), "%s", bytes);
    return dst;
}
static jsize mock_arr_len(JniMockEnv **env, jbyteArray arr) {
    (void)env;
    return arr ? arr->len : 0;
}
static jbyte *mock_get_bytes(JniMockEnv **env, jbyteArray arr, jboolean *isCopy) {
    (void)env;
    if (isCopy) *isCopy = JNI_FALSE;
    return arr ? arr->data : NULL;
}
static void mock_rel_bytes(JniMockEnv **env, jbyteArray arr, jbyte *elems, jint mode) {
    (void)env; (void)arr; (void)elems; (void)mode;
}
static jbyteArray mock_new_byte_arr(JniMockEnv **env, jsize len) {
    (void)env;
    if (len <= 0) return NULL;
    JniMockByteArray *a = (JniMockByteArray *)malloc(sizeof(JniMockByteArray));
    if (!a) return NULL;
    a->data = (uint8_t *)malloc((size_t)len);
    if (!a->data) { free(a); return NULL; }
    a->len = len;
    return a;
}
static void mock_set_byte_region(JniMockEnv **env, jbyteArray arr, jsize start, jsize len, const jbyte *buf) {
    (void)env;
    if (arr && arr->data && buf && start >= 0 && start + len <= arr->len) {
        memcpy(arr->data + start, buf, (size_t)len);
    }
}

static JniMockEnv s_mock_vtbl = {
    mock_get_str,
    mock_rel_str,
    mock_new_str,
    mock_arr_len,
    mock_get_bytes,
    mock_rel_bytes,
    mock_new_byte_arr,
    mock_set_byte_region
};
static JniMockEnv *s_mock_env_ptr = &s_mock_vtbl;

JNIEnv *mep_jni_mock_env(void) {
    return &s_mock_env_ptr;
}

void mep_jni_mock_free_byte_array(jbyteArray arr) {
    if (arr) {
        free(arr->data);
        free(arr);
    }
}
#endif

/* Trim any incomplete trailing multi-byte UTF-8 sequence and replace bare NUL/0x80+
 * continuation errors so `NewStringUTF` never aborts under Android CheckJNI. */
static void jni_sanitize_utf8_inplace(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80u) {
            i++;
            continue;
        }
        size_t seq_len = 0;
        if ((c & 0xE0u) == 0xC0u) seq_len = 2;
        else if ((c & 0xF0u) == 0xE0u) seq_len = 3;
        else if ((c & 0xF8u) == 0xF0u) seq_len = 4;
        else {
            s[i++] = '?';
            continue;
        }
        if (i + seq_len > len) {
            /* Truncated multi-byte UTF-8 sequence at end of buffer: trim cleanly */
            s[i] = '\0';
            break;
        }
        bool valid_cont = true;
        for (size_t k = 1; k < seq_len; ++k) {
            if (((unsigned char)s[i + k] & 0xC0u) != 0x80u) {
                valid_cont = false;
                break;
            }
        }
        if (!valid_cont) {
            s[i++] = '?';
            continue;
        }
        i += seq_len;
    }
}

static jstring jni_new_safe_utf(JNIEnv *env, char *buf) {
    if (!buf) return NULL;
    jni_sanitize_utf8_inplace(buf);
    return (*env)->NewStringUTF(env, buf);
}

static jstring jni_format_eval_reply(JNIEnv *env, Engine *eng, bool ok, Value out) {
    char reply[4096];
    if (!ok) {
        snprintf(reply, sizeof(reply), "ERR|%s", eng_last_error(eng));
        return jni_new_safe_utf(env, reply);
    }
    char val_buf[3900], type_buf[32], unit_buf[32];
    eng_format_value(eng, out, val_buf, sizeof(val_buf));
    jni_sanitize_utf8_inplace(val_buf);
    eng_type_name(eng, eng_val_type(eng, out), type_buf, sizeof(type_buf));
    eng_unit_name(eng, eng_val_unit(eng, out), unit_buf, sizeof(unit_buf));
    snprintf(reply, sizeof(reply), "OK|%s|%s|%s", type_buf, unit_buf, val_buf);
    return jni_new_safe_utf(env, reply);
}

JNIEXPORT jlong JNICALL
Java_com_mathengine_MathEngineBridge_nativeCreate(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    Engine *eng = eng_create();
    return (jlong)(intptr_t)eng;
}

JNIEXPORT jlong JNICALL
Java_com_mathengine_MathEngineBridge_nativeCreateBlank(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    Engine *eng = eng_create_blank();
    return (jlong)(intptr_t)eng;
}

JNIEXPORT void JNICALL
Java_com_mathengine_MathEngineBridge_nativeDestroy(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (eng) eng_destroy(eng);
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeEval(JNIEnv *env, jclass clazz,
                                                jlong handle, jstring expr) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !expr) return (*env)->NewStringUTF(env, "ERR|Invalid engine handle or null expression");
    const char *c_expr = (*env)->GetStringUTFChars(env, expr, NULL);
    if (!c_expr) return (*env)->NewStringUTF(env, "ERR|Failed to read UTF-8 expression");
    Value out = val_void();
    bool ok = eng_eval(eng, c_expr, &out);
    (*env)->ReleaseStringUTFChars(env, expr, c_expr);
    return jni_format_eval_reply(env, eng, ok, out);
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeLastError(JNIEnv *env, jclass clazz, jlong handle) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return (*env)->NewStringUTF(env, "Invalid engine handle");
    return (*env)->NewStringUTF(env, eng_last_error(eng));
}

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionOpen(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return -1;
    return (jint)eng_session_open(eng);
}

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionClose(JNIEnv *env, jclass clazz,
                                                        jlong handle, jint sessionId) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return JNI_FALSE;
    return eng_session_close(eng, (int)sessionId) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionReset(JNIEnv *env, jclass clazz,
                                                        jlong handle, jint sessionId) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return JNI_FALSE;
    return eng_session_reset(eng, (int)sessionId) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionEval(JNIEnv *env, jclass clazz,
                                                       jlong handle, jint sessionId,
                                                       jstring expr) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !expr) return (*env)->NewStringUTF(env, "ERR|Invalid engine handle or null expression");
    const char *c_expr = (*env)->GetStringUTFChars(env, expr, NULL);
    if (!c_expr) return (*env)->NewStringUTF(env, "ERR|Failed to read UTF-8 expression");
    Value out = val_void();
    bool ok = eng_session_eval(eng, (int)sessionId, c_expr, &out);
    (*env)->ReleaseStringUTFChars(env, expr, c_expr);
    return jni_format_eval_reply(env, eng, ok, out);
}

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionSnapshotCount(JNIEnv *env, jclass clazz,
                                                                jlong handle, jint sessionId) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return 0;
    return (jint)eng_session_snapshot_count(eng, (int)sessionId);
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionSnapshotAt(JNIEnv *env, jclass clazz,
                                                             jlong handle, jint sessionId,
                                                             jint index) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || index < 0) return NULL;
    const char *snap = eng_session_snapshot_at(eng, (int)sessionId, (uint16_t)index);
    return snap ? (*env)->NewStringUTF(env, snap) : NULL;
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionExportSnapshots(JNIEnv *env, jclass clazz,
                                                                  jlong handle, jint sessionId) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return NULL;
    if (!eng_session_snapshot_rewind(eng, (int)sessionId)) return NULL;

    size_t total_len = 0;
    const char *raw = NULL;
    while ((raw = eng_session_snapshot_next(eng, (int)sessionId)) != NULL) {
        total_len += strlen(raw) + 1u;
    }
    char *joined = (char *)malloc(total_len + 1u);
    if (!joined) return NULL;
    joined[0] = '\0';

    size_t used = 0;
    (void)eng_session_snapshot_rewind(eng, (int)sessionId);
    while ((raw = eng_session_snapshot_next(eng, (int)sessionId)) != NULL) {
        size_t rlen = strlen(raw);
        if (used > 0) joined[used++] = '\n';
        memcpy(joined + used, raw, rlen);
        used += rlen;
        joined[used] = '\0';
    }
    jstring jres = jni_new_safe_utf(env, joined);
    free(joined);
    return jres;
}

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeSessionRestore(JNIEnv *env, jclass clazz,
                                                          jlong handle, jstring newlineJoinedSnapshots) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !newlineJoinedSnapshots) return -1;
    const char *raw = (*env)->GetStringUTFChars(env, newlineJoinedSnapshots, NULL);
    if (!raw) return -1;
    size_t raw_len = strlen(raw);
    char *buf = (char *)malloc(raw_len + 1u);
    if (!buf) {
        (*env)->ReleaseStringUTFChars(env, newlineJoinedSnapshots, raw);
        return -1;
    }
    memcpy(buf, raw, raw_len + 1u);
    (*env)->ReleaseStringUTFChars(env, newlineJoinedSnapshots, raw);

    const char *snaps[ENG_SESSION_MAX_EVALS];
    size_t count = 0;
    char *cur = buf;
    while (*cur != '\0') {
        while (*cur == '\n' || *cur == '\r') cur++;
        if (*cur == '\0') break;
        if (count >= ENG_SESSION_MAX_EVALS) {
            free(buf);
            return -1;
        }
        snaps[count++] = cur;
        while (*cur != '\0' && *cur != '\n' && *cur != '\r') cur++;
        if (*cur != '\0') {
            *cur = '\0';
            cur++;
        }
    }
    if (count == 0) {
        free(buf);
        return -1;
    }
    int sid = eng_session_restore(eng, snaps, count);
    free(buf);
    return (jint)sid;
}

JNIEXPORT jbyteArray JNICALL
Java_com_mathengine_MathEngineBridge_nativeCompileExt(JNIEnv *env, jclass clazz,
                                                      jlong handle, jstring mathSource) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !mathSource) return NULL;
    const char *src = (*env)->GetStringUTFChars(env, mathSource, NULL);
    if (!src) return NULL;
    const uint8_t *mathc_data = NULL;
    size_t mathc_len = 0;
    bool ok = user_compile_ext(eng, src, &mathc_data, &mathc_len);
    (*env)->ReleaseStringUTFChars(env, mathSource, src);
    if (!ok || !mathc_data || mathc_len == 0) return NULL;

    jbyteArray arr = (*env)->NewByteArray(env, (jsize)mathc_len);
    if (!arr) return NULL;
    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)mathc_len, (const jbyte *)mathc_data);
    return arr;
}

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeLoadMathc(JNIEnv *env, jclass clazz,
                                                     jlong handle, jbyteArray mathcBytes) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !mathcBytes) return JNI_FALSE;
    jsize len = (*env)->GetArrayLength(env, mathcBytes);
    if (len <= 0) return JNI_FALSE;
    jbyte *bytes = (*env)->GetByteArrayElements(env, mathcBytes, NULL);
    if (!bytes) return JNI_FALSE;
    bool ok = user_load_mathc(eng, (const uint8_t *)bytes, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, mathcBytes, bytes, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeSetModule(JNIEnv *env, jclass clazz,
                                                     jlong handle, jstring modName,
                                                     jboolean enabled) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !modName) return JNI_FALSE;
    const char *name = (*env)->GetStringUTFChars(env, modName, NULL);
    if (!name) return JNI_FALSE;
    bool ok = eng_set_module(eng, name, enabled != JNI_FALSE);
    (*env)->ReleaseStringUTFChars(env, modName, name);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeIsModuleActive(JNIEnv *env, jclass clazz,
                                                          jlong handle, jstring modName) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !modName) return JNI_FALSE;
    const char *name = (*env)->GetStringUTFChars(env, modName, NULL);
    if (!name) return JNI_FALSE;
    bool active = eng_is_module_active(eng, name);
    (*env)->ReleaseStringUTFChars(env, modName, name);
    return active ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymSimplify(JNIEnv *env, jclass clazz,
                                                       jlong handle, jstring expr) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !expr) return NULL;
    const char *c_expr = (*env)->GetStringUTFChars(env, expr, NULL);
    if (!c_expr) return NULL;
    char out[4096];
    bool ok = eng_sym_simplify(eng, c_expr, out, sizeof(out));
    (*env)->ReleaseStringUTFChars(env, expr, c_expr);
    return ok ? jni_new_safe_utf(env, out) : NULL;
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymDiff(JNIEnv *env, jclass clazz,
                                                   jlong handle, jstring varName,
                                                   jstring expr) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !varName || !expr) return NULL;
    const char *c_var  = (*env)->GetStringUTFChars(env, varName, NULL);
    const char *c_expr = (*env)->GetStringUTFChars(env, expr, NULL);
    if (!c_var || !c_expr) {
        if (c_var)  (*env)->ReleaseStringUTFChars(env, varName, c_var);
        if (c_expr) (*env)->ReleaseStringUTFChars(env, expr, c_expr);
        return NULL;
    }
    char out[4096];
    bool ok = eng_sym_diff(eng, c_var, c_expr, out, sizeof(out));
    (*env)->ReleaseStringUTFChars(env, varName, c_var);
    (*env)->ReleaseStringUTFChars(env, expr, c_expr);
    return ok ? jni_new_safe_utf(env, out) : NULL;
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymInteg(JNIEnv *env, jclass clazz,
                                                    jlong handle, jstring varName,
                                                    jstring expr) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !varName || !expr) return NULL;
    const char *c_var  = (*env)->GetStringUTFChars(env, varName, NULL);
    const char *c_expr = (*env)->GetStringUTFChars(env, expr, NULL);
    if (!c_var || !c_expr) {
        if (c_var)  (*env)->ReleaseStringUTFChars(env, varName, c_var);
        if (c_expr) (*env)->ReleaseStringUTFChars(env, expr, c_expr);
        return NULL;
    }
    char out[4096];
    bool ok = eng_sym_integ(eng, c_var, c_expr, out, sizeof(out));
    (*env)->ReleaseStringUTFChars(env, varName, c_var);
    (*env)->ReleaseStringUTFChars(env, expr, c_expr);
    return ok ? jni_new_safe_utf(env, out) : NULL;
}

JNIEXPORT jstring JNICALL
Java_com_mathengine_MathEngineBridge_nativeSymSolve(JNIEnv *env, jclass clazz,
                                                    jlong handle, jstring varName,
                                                    jstring eq) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng || !varName || !eq) return NULL;
    const char *c_var = (*env)->GetStringUTFChars(env, varName, NULL);
    const char *c_eq  = (*env)->GetStringUTFChars(env, eq, NULL);
    if (!c_var || !c_eq) {
        if (c_var) (*env)->ReleaseStringUTFChars(env, varName, c_var);
        if (c_eq)  (*env)->ReleaseStringUTFChars(env, eq, c_eq);
        return NULL;
    }
    char out[4096];
    bool ok = eng_sym_solve(eng, c_var, c_eq, out, sizeof(out));
    (*env)->ReleaseStringUTFChars(env, varName, c_var);
    (*env)->ReleaseStringUTFChars(env, eq, c_eq);
    return ok ? jni_new_safe_utf(env, out) : NULL;
}

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeCompactScratch(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return 0;
    return (jint)eng_compact_scratch(eng);
}

JNIEXPORT jint JNICALL
Java_com_mathengine_MathEngineBridge_nativeScratchPoolUsed(JNIEnv *env, jclass clazz, jlong handle) {
    (void)env; (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
    if (!eng) return 0;
    return (jint)eng_scratch_pool_used(eng);
}

/* ============================================================================
 * 11. HOST-BRIDGE EXTERNS (Kotlin / Java callback backed `@<name>(...)`)
 * ============================================================================
 * Lets Android apps back `.math` extension patches (e.g. the `[currency]`
 * patch's `@currency_convert`) with app-side callbacks:
 *
 *   Kotlin: engine.registerExtern("currency_convert") { _, nums, strs ->
 *       fxProvider.rateToUsd(strs.first()) * nums[0]
 *   }
 *
 * The bridges callback (`ExternCallback.apply`) receives:
 *   - `name`    : the `@` extern name that was invoked
 *   - `numbers` : every numeric (real) argument, in call order
 *   - `strings` : every string argument, in call order
 * and must return the numeric result (`double`). `void` / unsupported
 * argument types turn into `0.0` / `""` placeholders so arity stays stable. */

#if defined(__has_include) && __has_include(<jni.h>)
#define MEP_JNI_REAL_BRIDGE 1
#include <pthread.h>
#endif

#define MEP_JNI_BRIDGE_MAX_ARGS 16
#define MEP_JNI_JVM_OK 0

#if defined(MEP_JNI_REAL_BRIDGE)
typedef struct JniBridgeCtx {
    JavaVM    *vm;
    jobject    callback;
    jmethodID  method;
    char       ext_name[32];
} JniBridgeCtx;
#endif

#if defined(MEP_JNI_REAL_BRIDGE)

static bool jni_bridge_is_str(const MemArena *mem, Value v) {
    uint16_t tid = val_get_type(mem, v);
    return mem->str_type_id != 0 && tid == mem->str_type_id;
}

/* EngExternFn adapter: marshal engine args -> Java, invoke, unmarshal result. */
static bool jni_bridge_cb(MemArena *mem, const SymTable *tab,
                          const Value *args, uint8_t argc,
                          void *user_ctx, Value *out_val) {
    (void)tab;
    JniBridgeCtx *ctx = (JniBridgeCtx *)user_ctx;
    if (!ctx || !(ctx->vm)) {
        mem_err(mem, "JNI extern bridge missing context");
        return false;
    }
    JNIEnv *env = NULL;
    if ((*ctx->vm)->GetEnv(ctx->vm, (void **)&env, JNI_VERSION_1_4) != MEP_JNI_JVM_OK || !env) {
        mem_err(mem, "JNI extern bridge used outside a Java thread");
        return false;
    }

    uint8_t num_count = 0;
    uint8_t str_count = 0;
    jdouble nums[MEP_JNI_BRIDGE_MAX_ARGS];
    jstring strs[MEP_JNI_BRIDGE_MAX_ARGS];
    memset(nums, 0, sizeof(nums));
    memset(strs, 0, sizeof(strs));

    bool ok = true;
    for (uint8_t i = 0; i < argc && ok; ++i) {
        uint16_t tid = val_get_type(mem, args[i]);
        if (tid == MEM_TYPE_VOID) continue; /* 0.0 / "" placeholder already set */
        if (jni_bridge_is_str(mem, args[i])) {
            if (str_count >= MEP_JNI_BRIDGE_MAX_ARGS) {
                mem_err(mem, "JNI extern bridge supports max %d string args",
                        MEP_JNI_BRIDGE_MAX_ARGS);
                return false;
            }
            StrSlice s = val_get_str(mem, args[i]);
            size_t len = s.len < 224 ? s.len : 224;
            char tmp[256];
            if (len > 0) memcpy(tmp, mem_str_ptr(mem, s), len);
            tmp[len] = '\0';
            strs[str_count] = (*env)->NewStringUTF(env, tmp);
            if ((*env)->ExceptionCheck(env) == JNI_TRUE) {
                (*env)->ExceptionClear(env);
                mem_err(mem, "JNI extern bridge: non-UTF-8 string argument '%.*s'",
                        (int)(len < 60 ? len : 60), mem_str_ptr(mem, s));
                ok = false;
            } else {
                str_count++;
            }
        } else if (tid == mem->fast_real_type) {
            if (num_count >= MEP_JNI_BRIDGE_MAX_ARGS) {
                mem_err(mem, "JNI extern bridge supports max %d numeric args",
                        MEP_JNI_BRIDGE_MAX_ARGS);
                return false;
            }
            nums[num_count++] = val_get_f64(mem, args[i]);
        } else {
            char tid_buf[32];
            snprintf(tid_buf, sizeof(tid_buf), "%u", (unsigned)tid);
            mem_err(mem, "JNI extern bridge: unsupported argument type id %s (numeric/string/void only)",
                    tid_buf);
            ok = false;
        }
    }

    if (ok) {
        jdoubleArray num_arr = (*env)->NewDoubleArray(env, (jsize)num_count);
        if (num_arr && num_count > 0) {
            (*env)->SetDoubleArrayRegion(env, num_arr, 0, (jsize)num_count, nums);
        }
        jclass str_cls = (*env)->FindClass(env, "java/lang/String");
        jobjectArray str_arr = str_cls
            ? (*env)->NewObjectArray(env, (jsize)str_count, str_cls, NULL) : NULL;
        if (str_arr) {
            for (uint8_t j = 0; j < str_count; ++j) {
                (*env)->SetObjectArrayElement(env, str_arr, (jsize)j, strs[j]);
            }
        }
        if (!num_arr || !str_arr) {
            mem_err(mem, "JNI extern bridge array allocation failed");
            ok = false;
        } else {
            jdouble result = (*env)->CallDoubleMethod(env, ctx->callback, ctx->method,
                                                      jni_new_safe_utf(env, ctx->ext_name),
                                                      num_arr, str_arr);
            if ((*env)->ExceptionCheck(env) == JNI_TRUE) {
                (*env)->ExceptionDescribe(env);
                (*env)->ExceptionClear(env);
                mem_err(mem, "Host extern bridge callback threw an exception");
                ok = false;
            } else if (result != result) { /* NaN */
                mem_err(mem, "Host extern bridge returned NaN");
                ok = false;
            } else {
                *out_val = val_from_f64(mem, (double)result);
            }
        }
        if (num_arr) (*env)->DeleteLocalRef(env, num_arr);
        if (str_arr) (*env)->DeleteLocalRef(env, str_arr);
    }
    for (uint8_t j = 0; j < str_count; ++j) {
        if (strs[j]) (*env)->DeleteLocalRef(env, strs[j]);
    }
    return ok;
}
#endif /* MEP_JNI_REAL_BRIDGE */

/* Extern destructor: runs when a registration is replaced or the engine is
 * destroyed. Deletes the Java global ref (attaching a thread if the caller is
 * native-only, e.g. engine teardown on a worker) and frees the bridge ctx. */
static void jni_bridge_free_cb(void *user_ctx) {
    JniBridgeCtx *ctx = (JniBridgeCtx *)user_ctx;
    if (!ctx) return;
    JNIEnv *env = NULL;
    void    *thr_env = NULL;
    bool attached = false;
    if (ctx->vm) {
        if ((*ctx->vm)->GetEnv(ctx->vm, (void **)&env, JNI_VERSION_1_4) != MEP_JNI_JVM_OK || !env) {
            env = NULL;
            if ((*ctx->vm)->AttachCurrentThread(ctx->vm, &thr_env, NULL) == MEP_JNI_JVM_OK && thr_env) {
                env = (JNIEnv *)thr_env;
                attached = true;
            }
        }
    }
    if (env && ctx->callback) {
        (*env)->DeleteGlobalRef(env, ctx->callback);
        ctx->callback = NULL;
    }
    if (attached && ctx->vm) {
        (*ctx->vm)->DetachCurrentThread(ctx->vm);
    }
    free(ctx);
}

JNIEXPORT jboolean JNICALL
Java_com_mathengine_MathEngineBridge_nativeRegisterExtern(JNIEnv *env, jclass clazz,
                                                          jlong handle, jstring name,
                                                          jobject callback) {
    (void)clazz;
    Engine *eng = (Engine *)(intptr_t)handle;
#if defined(MEP_JNI_REAL_BRIDGE)
    if (!eng || !name || !callback) return JNI_FALSE;
    const char *ext_name = (*env)->GetStringUTFChars(env, name, NULL);
    if (!ext_name) return JNI_FALSE;

    JniBridgeCtx *ctx = (JniBridgeCtx *)calloc(1, sizeof(JniBridgeCtx));
    if (!ctx) {
        (*env)->ReleaseStringUTFChars(env, name, ext_name);
        return JNI_FALSE;
    }
    snprintf(ctx->ext_name, sizeof(ctx->ext_name), "%s", ext_name);
    (*env)->ReleaseStringUTFChars(env, name, ext_name);

    jmethodID mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, callback),
                                        "apply",
                                        "(Ljava/lang/String;[D[Ljava/lang/String;)D");
    if (!mid) {
        free(ctx);
        return JNI_FALSE;
    }
    jobject ref = (*env)->NewGlobalRef(env, callback);
    if (!ref) {
        free(ctx);
        return JNI_FALSE;
    }
    if ((*env)->GetJavaVM(env, &ctx->vm) != MEP_JNI_JVM_OK) {
        (*env)->DeleteGlobalRef(env, ref);
        free(ctx);
        return JNI_FALSE;
    }
    ctx->method = mid;
    ctx->callback = ref;

    bool ok = eng_register_extern_full(eng, ctx->ext_name, jni_bridge_cb, ctx,
                                       jni_bridge_free_cb);
    if (!ok) {
        jni_bridge_free_cb(ctx);
    }
    return ok ? JNI_TRUE : JNI_FALSE;
#else
    (void)eng; (void)env; (void)name; (void)callback;
    return JNI_FALSE;
#endif
}
