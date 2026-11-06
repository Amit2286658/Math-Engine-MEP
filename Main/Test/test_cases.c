/*
 * test_cases.c — Host I/O Helpers, @extern Callbacks & 574-Expression Test Table
 */
#include "test_harness.h"

Engine g_eng; /* Single static engine context */

/* ============================================================================
 * 1. HOST PLATFORM FILE I/O HELPERS (KEPT STRICTLY OUTSIDE `Core/`)
 * ============================================================================ */

uint8_t *frontend_read_file(const char *path, size_t *out_len) {
    if (out_len) *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    buf[n] = '\0';
    fclose(fp);
    if (out_len) *out_len = n;
    return buf;
}

bool frontend_write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = (fwrite(data, 1, len, fp) == len);
    fclose(fp);
    return ok;
}

/* ============================================================================
 * 2. SEPARATE-STDLIB BOOT HELPER (hosts own the stdlib image, not the engine)
 * ============================================================================ */

bool test_load_stdlib_file(Engine *eng) {
    if (!eng) return false;
    const char *paths[] = {
#if defined(MEP_TEST_STDLIBC_PATH)
        MEP_TEST_STDLIBC_PATH,
#endif
        "precompiled/stdlib.mathc",
        "Main/precompiled/stdlib.mathc",
        "../precompiled/stdlib.mathc",
        NULL
    };
    for (int i = 0; paths[i]; ++i) {
        size_t len = 0;
        uint8_t *data = frontend_read_file(paths[i], &len);
        if (data) {
            bool ok = user_load_mathc(eng, data, len);
            free(data);
            if (ok) return true;
        }
    }
    fprintf(stderr, "test boot: could not load 'precompiled/stdlib.mathc'\n");
    return false;
}

/* JNI counterpart for the separate-stdlib flow (mock env only; Android
 * devices do not run this suite): wrap `precompiled/stdlib.mathc` in a mock
 * byte array so the tests can `nativeLoadMathc` the blank engine. */
#if !(defined(__has_include) && __has_include(<jni.h>))
static jbyteArray test_mock_stdlib_array(JNIEnv *jenv) {
    static uint8_t *std_bytes = NULL;
    static size_t std_len = 0;
    static bool std_ready = false;
    if (!std_ready) {
        const char *paths[] = {
#if defined(MEP_TEST_STDLIBC_PATH)
            MEP_TEST_STDLIBC_PATH,
#endif
            "precompiled/stdlib.mathc",
            "Main/precompiled/stdlib.mathc",
            "../precompiled/stdlib.mathc",
            NULL
        };
        for (int i = 0; paths[i]; ++i) {
            std_bytes = frontend_read_file(paths[i], &std_len);
            if (std_bytes) break;
        }
        std_ready = true;
    }
    if (!jenv || !std_bytes || std_len == 0 || !*jenv) return NULL;
    jbyteArray arr = (*jenv)->NewByteArray(jenv, (jsize)std_len);
    if (arr) (*jenv)->SetByteArrayRegion(jenv, arr, 0, (jsize)std_len, (const jbyte *)std_bytes);
    return arr;
}

void test_jni_load_stdlib(JNIEnv *jenv, jlong handle) {
    if (handle == 0) return;
    jbyteArray arr = test_mock_stdlib_array(jenv);
    if (!arr) return;
    (void)Java_com_mathengine_MathEngineBridge_nativeLoadMathc(jenv, NULL, handle, arr);
    mep_jni_mock_free_byte_array(arr);
}
#endif /* mock-only helper */

/* ============================================================================
 * 2. HOST EXTERN CALLBACK IMPLEMENTATIONS (`eng_register_extern`)
 * ============================================================================ */

typedef struct {
    const char *ccy;
    double      per_usd;
} FxQuote;

typedef struct {
    FxQuote *quotes;
    size_t   count;
} FxRateCache;

static FxQuote g_live_fx_quotes[] = {
    { "USD",   1.00 },
    { "EUR",   0.92 },
    { "GBP",   0.79 },
    { "INR",  84.00 },
    { "JPY", 150.00 }
};

static FxRateCache g_fx_cache = {
    g_live_fx_quotes,
    sizeof(g_live_fx_quotes) / sizeof(g_live_fx_quotes[0])
};

static bool lookup_usd_rate(const FxRateCache *cache, const char *ccy, double *out_per_usd) {
    for (size_t i = 0; i < cache->count; ++i) {
        if (strcmp(cache->quotes[i].ccy, ccy) == 0) {
            *out_per_usd = cache->quotes[i].per_usd;
            return true;
        }
    }
    return false;
}

static bool host_fx_rate_cb(MemArena *mem, const SymTable *tab,
                            const Value *args, uint8_t argc,
                            void *user_ctx, Value *out_val) {
    (void)tab;
    if (argc != 2) {
        mem_err(mem, "@fx_rate expects 2 currency string arguments (from, to)");
        return false;
    }
    char from_ccy[16], to_ccy[16];
    if (!val_as_cstr(mem, args[0], from_ccy, sizeof(from_ccy)) ||
        !val_as_cstr(mem, args[1], to_ccy,   sizeof(to_ccy))) {
        mem_err(mem, "@fx_rate arguments must be currency code strings");
        return false;
    }

    const FxRateCache *cache = (const FxRateCache *)user_ctx;
    double from_per_usd = 0.0, to_per_usd = 0.0;
    if (!lookup_usd_rate(cache, from_ccy, &from_per_usd) ||
        !lookup_usd_rate(cache, to_ccy,   &to_per_usd)) {
        mem_err(mem, "Unsupported currency pair '%s' -> '%s'", from_ccy, to_ccy);
        return false;
    }
    *out_val = val_from_f64(mem, to_per_usd / from_per_usd);
    return true;
}

static bool host_fx_set_rate_cb(MemArena *mem, const SymTable *tab,
                                const Value *args, uint8_t argc,
                                void *user_ctx, Value *out_val) {
    (void)tab;
    if (argc != 2) {
        mem_err(mem, "@fx_set_rate expects (ccy:str, per_usd:real)");
        return false;
    }
    char ccy[16];
    if (!val_as_cstr(mem, args[0], ccy, sizeof(ccy))) {
        mem_err(mem, "@fx_set_rate first argument must be a currency code string");
        return false;
    }
    double new_rate = val_get_f64(mem, args[1]);
    if (new_rate <= 0.0) {
        mem_err(mem, "@fx_set_rate rate must be positive (got %g)", new_rate);
        return false;
    }
    FxRateCache *cache = (FxRateCache *)user_ctx;
    for (size_t i = 0; i < cache->count; ++i) {
        if (strcmp(cache->quotes[i].ccy, ccy) == 0) {
            cache->quotes[i].per_usd = new_rate;
            *out_val = val_from_f64(mem, new_rate);
            return true;
        }
    }
    mem_err(mem, "Cannot update unknown currency %s", ccy);
    return false;
}

static bool host_fx_history_cb(MemArena *mem, const SymTable *tab,
                               const Value *args, uint8_t argc,
                               void *user_ctx, Value *out_val) {
    (void)tab;
    if (argc != 3) {
        mem_err(mem, "@fx_history expects (from:str, to:str, days:real)");
        return false;
    }
    char from_ccy[16], to_ccy[16];
    if (!val_as_cstr(mem, args[0], from_ccy, sizeof(from_ccy)) ||
        !val_as_cstr(mem, args[1], to_ccy,   sizeof(to_ccy))) {
        mem_err(mem, "@fx_history currency codes must be strings");
        return false;
    }
    int days = (int)val_get_f64(mem, args[2]);
    if (days <= 0 || days > 64) {
        mem_err(mem, "@fx_history days must be in 1..64 (got %d)", days);
        return false;
    }

    const FxRateCache *cache = (const FxRateCache *)user_ctx;
    double from_per_usd = 0.0, to_per_usd = 0.0;
    if (!lookup_usd_rate(cache, from_ccy, &from_per_usd) ||
        !lookup_usd_rate(cache, to_ccy,   &to_per_usd)) {
        mem_err(mem, "Unsupported currency pair '%s' -> '%s'", from_ccy, to_ccy);
        return false;
    }
    double base_rate = to_per_usd / from_per_usd;
    double series[64];
    for (int i = 0; i < days; ++i) {
        series[i] = base_rate - (double)(days - 1 - i) * 0.5;
    }
    *out_val = val_from_f64_vec(mem, series, (uint16_t)days);
    return !mem->has_error;
}

typedef struct {
    char   key[32];
    double val;
    bool   used;
} KvSlot;

typedef struct {
    KvSlot slots[16];
} HostKvStore;

static HostKvStore g_kv_store;

static bool host_kv_set_cb(MemArena *mem, const SymTable *tab,
                           const Value *args, uint8_t argc,
                           void *user_ctx, Value *out_val) {
    (void)tab;
    if (argc != 2) {
        mem_err(mem, "@kv_set expects (key:str, val:real)");
        return false;
    }
    char key[32];
    if (!val_as_cstr(mem, args[0], key, sizeof(key))) {
        mem_err(mem, "@kv_set first argument must be a string key");
        return false;
    }
    double v = val_get_f64(mem, args[1]);
    HostKvStore *store = (HostKvStore *)user_ctx;
    int free_idx = -1;
    for (int i = 0; i < 16; ++i) {
        if (store->slots[i].used && strcmp(store->slots[i].key, key) == 0) {
            store->slots[i].val = v;
            *out_val = val_from_f64(mem, v);
            return true;
        }
        if (!store->slots[i].used && free_idx < 0) free_idx = i;
    }
    if (free_idx < 0) {
        mem_err(mem, "Host KV store full");
        return false;
    }
    size_t klen = strlen(key);
    if (klen >= sizeof(store->slots[0].key)) klen = sizeof(store->slots[0].key) - 1;
    memcpy(store->slots[free_idx].key, key, klen);
    store->slots[free_idx].key[klen] = '\0';
    store->slots[free_idx].val  = v;
    store->slots[free_idx].used = true;
    *out_val = val_from_f64(mem, v);
    return true;
}

static bool host_kv_get_cb(MemArena *mem, const SymTable *tab,
                           const Value *args, uint8_t argc,
                           void *user_ctx, Value *out_val) {
    (void)tab;
    if (argc != 1) {
        mem_err(mem, "@kv_get expects (key:str)");
        return false;
    }
    char key[32];
    if (!val_as_cstr(mem, args[0], key, sizeof(key))) {
        mem_err(mem, "@kv_get argument must be a string key");
        return false;
    }
    const HostKvStore *store = (const HostKvStore *)user_ctx;
    for (int i = 0; i < 16; ++i) {
        if (store->slots[i].used && strcmp(store->slots[i].key, key) == 0) {
            *out_val = val_from_f64(mem, store->slots[i].val);
            return true;
        }
    }
    mem_err(mem, "Key '%s' not found in host KV store", key);
    return false;
}

typedef struct {
    const char *sym;
    double      atomic_mass;
} ElemEntry;

static const ElemEntry k_periodic_table[] = {
    { "H",   1.008 }, { "He",  4.0026}, { "Li",  6.94  }, { "Be",  9.0122},
    { "B",  10.81  }, { "C",  12.011 }, { "N",  14.007 }, { "O",  15.999 },
    { "F",  18.998 }, { "Ne", 20.180 }, { "Na", 22.990 }, { "Mg", 24.305 },
    { "Al", 26.982 }, { "Si", 28.085 }, { "P",  30.974 }, { "S",  32.06  },
    { "Cl", 35.45  }, { "K",  39.098 }, { "Ca", 40.078 }, { "Fe", 55.845 },
    { "Cu", 63.546 }, { "Zn", 65.38  }, { "Ag",107.868 }, { "Au",196.967 },
    { NULL,  0.0   }
};

static bool host_elem_mass_cb(MemArena *mem, const SymTable *tab,
                              const Value *args, uint8_t argc,
                              void *user_ctx, Value *out_val) {
    (void)tab;
    (void)user_ctx;
    if (argc != 1) {
        mem_err(mem, "@elem_mass expects 1 element symbol string");
        return false;
    }
    char sym[8];
    if (!val_as_cstr(mem, args[0], sym, sizeof(sym))) {
        mem_err(mem, "@elem_mass argument must be an element symbol string");
        return false;
    }
    for (int i = 0; k_periodic_table[i].sym != NULL; ++i) {
        if (strcmp(k_periodic_table[i].sym, sym) == 0) {
            *out_val = val_from_f64(mem, k_periodic_table[i].atomic_mass);
            return true;
        }
    }
    mem_err(mem, "Unknown chemical element symbol '%s'", sym);
    return false;
}

bool host_checked_recip_cb(MemArena *mem, const SymTable *tab,
                                  const Value *args, uint8_t argc,
                                  void *user_ctx, Value *out_val) {
    (void)tab;
    int *call_counter = (int *)user_ctx;
    if (call_counter) (*call_counter)++;
    if (argc != 1) {
        mem_err(mem, "@checked_recip expects 1 real argument");
        return false;
    }
    double x = val_get_f64(mem, args[0]);
    if (x == 0.0) {
        mem_err(mem, "Host extern @checked_recip rejected zero input");
        return false;
    }
    if (x < 0.0) {
        mem_err(mem, "Host extern @checked_recip rejected negative input (%g)", x);
        return false;
    }
    *out_val = val_from_f64(mem, 100.0 / x);
    return true;
}

#if !defined(_WIN32)
void *mt_stress_worker(void *arg) {
    MtWorkerArg *w = (MtWorkerArg *)arg;
    w->ok = false;
    Engine *eng = eng_create();
    if (!eng || !test_load_stdlib_file(eng)) {
        eng_destroy(eng);
        return NULL;
    }

    int ext_calls = 0;
    if (!eng_register_extern(eng, "checked_recip", host_checked_recip_cb, &ext_calls)) {
        eng_destroy(eng);
        return NULL;
    }
    char mod_src[256];
    snprintf(mod_src, sizeof(mod_src),
             "[mt_mod_%d]\n<[arith]\n"
             "\"mt_recip\"(x:real) -> real = @checked_recip(x)\n"
             "mt_scale(x) = x * %d\n",
             w->thread_id, w->thread_id + 2);
    if (!user_compile_ext(eng, mod_src, NULL, NULL)) {
        eng_destroy(eng);
        return NULL;
    }

    bool all_ok = true;
    Value v = 0;
    /* Module-scoped state replaces the legacy per-thread sessions: each
     * worker executes inside its own `[mt_mod_<id>]` module. */
    char mod_name[32];
    snprintf(mod_name, sizeof(mod_name), "mt_mod_%d", w->thread_id);
    all_ok = all_ok && eng_mod_eval(eng, mod_name, "acc := 0 ; v := vec(1, 2, 3, 4)", &v);
    for (int iter = 1; iter <= 20 && all_ok; ++iter) {
        all_ok = all_ok && eng_mod_eval(
            eng, mod_name,
            "acc += sigma(k, k^2, 1, 5) + mt_recip(4) + ([1, 2 ; 3, 4] + [2, 3 ; 4, 5])[0, 1]",
            &v);
        /* Also test symbolic integration and equation solving concurrently */
        char sym_buf[256];
        all_ok = all_ok && eng_sym_integ(eng, "x", "3 * x^2 + 2 * x", sym_buf, sizeof(sym_buf)) &&
                 (strcmp(sym_buf, "x ^ 3 + x ^ 2") == 0);
        all_ok = all_ok && eng_sym_solve(eng, "x", "x^2 - 5 * x + 6 == 0", sym_buf, sizeof(sym_buf)) &&
                 (strcmp(sym_buf, "[2, 3]") == 0);
    }
    /* Expected per-iteration increment: sigma(55) + mt_recip(4)(25) + mat[0,1](5) = 85; 20 * 85 = 1700 */
    all_ok = all_ok && (val_get_f64(&eng->mem, v) == 1700.0) && (ext_calls == 20);
    /* Verify error-injecting @extern callback propagates error without corrupting module state */
    all_ok = all_ok && !eng_mod_eval(eng, mod_name, "mt_recip(0)", &v) &&
             (strstr(eng_last_error(eng), "Host extern @checked_recip rejected zero input") != NULL) &&
             eng_mod_eval(eng, mod_name, "acc + mt_scale(10)", &v) &&
             (val_get_f64(&eng->mem, v) == 1700.0 + (double)(w->thread_id + 2) * 10.0);

    eng_destroy(eng);
    w->ok = all_ok;
    return NULL;
}
#endif

void register_all_host_externs(Engine *eng) {
    eng_register_extern(eng, "fx_rate",     host_fx_rate_cb,     (void *)&g_fx_cache);
    eng_register_extern(eng, "fx_set_rate", host_fx_set_rate_cb, (void *)&g_fx_cache);
    eng_register_extern(eng, "fx_history",  host_fx_history_cb,  (void *)&g_fx_cache);
    eng_register_extern(eng, "kv_set",      host_kv_set_cb,      (void *)&g_kv_store);
    eng_register_extern(eng, "kv_get",      host_kv_get_cb,      (void *)&g_kv_store);
    eng_register_extern(eng, "elem_mass",   host_elem_mass_cb,   NULL);
}

/* ============================================================================
 * 3. AUTOMATED TEST SUITE
 * ============================================================================ */

const TestCase k_tests[] = {
    /* A. Core Arithmetic, Combinatorics, Number Theory & Stats (`10_arith.math`) */
    { "2 + 3 * 4",             "14",         "real",    "Infix precedence (* over +)" },
    { "10 - 3 - 2",            "5",          "real",    "Left-associative binary subtraction '-'" },
    { "-10 - --3 minus 1",     "-13",        "real",    "Prefix '-', infix '-', prefix '--', word 'minus'" },
    { "2 ^ 3 ^ 2",             "512",        "real",    "Right-associative infix '^' (2^(3^2))" },
    { "√16 + 5!",              "124",        "real",    "Prefix '√' + postfix '!' (bootstrap ; := ?\? loop)" },
    { "++5",                   "6",          "real",    "Multi-char prefix '++'" },
    { "+++5",                  "7",          "real",    "Multi-char prefix '+++' (Maximal Munch)" },
    { "5++ + +++10",           "18",         "real",    "Combined postfix '++', infix '+', prefix '+++'" },
    { "10 plus 25",            "35",         "real",    "Word operator 'plus' (inlined passthrough to '+')" },
    { "abs_diff(4, 15)",       "11",         "real",    "Bootstrap lazy 'if(cond, then, else)' branch" },
    { "gcd(1071, 462)",        "21",         "real",    "Multi-instruction Euclidean GCD" },
    { "lcm(12, 18)",           "36",         "real",    "Least Common Multiple lcm(12, 18)" },
    { "ncr(10, 3) + npr(5, 2)","140",        "real",    "Combinatorics: 10C3 (120) + 5P2 (20)" },
    { "fib(10)",               "55",         "real",    "Iterative Fibonacci fib(10)" },
    { "is_prime(97) * 10 + is_prime(100)", "10", "real","Primality test is_prime(97)=1, is_prime(100)=0" },
    { "mod_pow(2, 10, 1000)",  "24",         "real",    "Fast modular exponentiation 2^10 mod 1000" },
    { "clamp(15, 0, 10)",      "10",         "real",    "Range clamping clamp(15, 0, 10)" },

    /* B. Trigonometric, Hyperbolic & Logarithmic Functions (`20_trig.math`) */
    { "sin(π / 2)",            "1",          "real",    "UTF-8 constant 'π' + trig intrinsic @sin" },
    { "ln(e ^ 4)",             "4",          "real",    "Constant 'e' + transcendental @ln" },
    { "log2(1024) + cbrt(27)", "13",         "real",    "Base-2 logarithm log2(1024) + cube root cbrt(27)" },
    { "cosh(0) + sinh(0)",     "1",          "real",    "Hyperbolic cosh(0) + sinh(0)" },

    /* C. Postfix `"i"` Operator (`$iota`) & Complex Arithmetic (`30_complex.math`) */
    { "1 + 1i",                "1 + 1i",     "complex", "Postfix 'i' on literal: real + iota -> complex" },
    { "(3 + 4)i",              "7i",         "iota",    "Postfix 'i' on parenthesized expression -> iota" },
    { "2 + sin(π / 2)i",       "2 + 1i",     "complex", "Postfix 'i' on function call sin(π/2)i" },
    { "((3 + 4)i)i",           "-7",         "real",    "Chained postfix 'i' on iota: (7i)i = -7 (i^2 = -1)" },
    { "(1 + 2i)i",             "-2 + 1i",    "complex", "Postfix 'i' on complex: 90-deg rotation in C-plane" },
    { "3i * 4i",               "-12",        "real",    "Imaginary * imaginary: 3i * 4i = -12" },
    { "(5 + 6i) - (2 + 4i)",   "3 + 2i",     "complex", "Overloaded complex - complex subtraction" },
    { "(1 + 2i) * (3 + 4i)",   "-5 + 10i",   "complex", "Overloaded complex * complex" },
    { "(-5 + 10i) / (3 + 4i)", "1 + 2i",     "complex", "Overloaded complex / complex division" },
    { "cabs(3 - 4i)",          "5",          "real",    "Complex magnitude on '3 - 4i'" },

    /* D. 2D/3D Vectors & Geometry (`40_geom.math`) */
    { "\"Euler Formula\"",     "\"Euler Formula\"", "str", "Consuming string type ($str)" },
    { "vec2(5, 7) - vec2(1, 2)", "vec2(x=4, y=5)", "vec2", "C-style struct subtraction overload" },
    { "dot(vec2(3, 4), vec2(2, 1))", "10",   "real",    "Struct field access via bootstrap '.' (@field)" },
    { "move_circle(circle(vec2(1, 2), 5), 10, 20)",
      "circle(center=vec2(x=11, y=22), radius=5)", "circle",
      "Nested struct field mutation via bootstrap '.', '=', and ';'" },
    { "circle(vec2(1, 2), 5).center.y", "2", "real",    "Chained compile-time nested struct offset read" },
    { "vec3(1, 2, 3) ** vec3(4, 5, 6)", "32", "real",   "$vec3 '**' 3D dot product" },
    { "cross3(vec3(1, 0, 0), vec3(0, 1, 0))", "vec3(x=0, y=0, z=1)", "vec3", "3D vector cross product i x j = k" },
    { "vec3_mag(vec3(3, 4, 12))", "13",      "real",    "3D Euclidean norm hypot(hypot(3,4),12)" },
    { "lerp(10, 20, 0.25)",    "12.5",       "real",    "Linear interpolation lerp(10, 20, 0.25)" },

    /* E. Algebra: Matrices, Linear Systems, Quadratics & Polynomials (`50_algebra.math`) */
    { "det(mat2(4, 7, 2, 6))", "10",         "real",    "2x2 matrix determinant (4*6 - 7*2)" },
    { "inv_mat2(mat2(4, 7, 2, 6)) * vec2(10, 10)", "vec2(x=-1, y=2)", "vec2", "Matrix inverse * vector solution" },
    { "solve_2x2(2, 3, 8, 5, -1, 3)", "vec2(x=1, y=2)", "vec2", "Cramer's rule 2x2 system: 2x+3y=8, 5x-y=3" },
    { "quad_roots(1, -5, 6)",  "vec2(x=3, y=2)", "vec2","Real roots of x^2 - 5x + 6 = 0 -> (3, 2)" },
    { "quad_croot(1, 2, 5)",   "-1 + 2i",    "complex", "Complex root of x^2 + 2x + 5 = 0 -> -1 + 2i" },
    { "p := vec() ; push(p, 6) ; push(p, -5) ; push(p, 1) ; poly_val(p, 4)", "2", "real",
      "Horner polynomial evaluation P(x)=x^2-5x+6 at x=4 -> 2" },
    { "p := vec() ; push(p, 6) ; push(p, -5) ; push(p, 1) ; poly_deriv(p)", "[-5, 2]", "vec",
      "Symbolic polynomial derivative d/dx(6 - 5x + x^2) -> [-5, 2]" },
    { "p := vec() ; push(p, -2) ; push(p, 1) ; q := vec() ; push(q, -3) ; push(q, 1) ; poly_mul(p, q)",
      "[6, -5, 1]", "vec",
      "Symbolic polynomial product (x - 2)*(x - 3) -> 6 - 5x + x^2" },
    { "p := vec() ; push(p, -9) ; push(p, 0) ; push(p, 1) ; poly_newton(p, 5, 8)", "3", "real",
      "Newton-Raphson algebraic root finder for x^2 - 9 = 0 -> 3" },

    /* F. Dimensional Unit Conversions & Postfix Suffixes (`60_units.math`) */
    { "sin(30 deg) + cos(60°)", "1",         "real",    "Postfix angle operators '30 deg' & '60°'" },
    { "20 dB",                 "10",         "real",    "Postfix decibel amplitude ratio 20 dB -> 10x" },
    { "1.5 km + 25000 cm + 500 mm", "1750.5","real",    "Postfix length suffixes (km + cm + mm -> meters)" },
    { "m_to_ft(304.8)",        "1000",       "real",    "Meters to feet conversion m_to_ft(304.8)" },
    { "108 kph",               "30",         "real",    "Postfix speed suffix 108 kph -> 30 m/s" },
    { "2 atm + 1 bar",         "302650",     "real",    "Postfix pressure suffixes (2 atm + 1 bar -> Pa)" },
    { "1 kcal + 1 kJ",         "5184",       "real",    "Postfix energy suffixes (1 kcal + 1 kJ -> Joules)" },
    { "c_to_f(100) + f_to_c(32) + c_to_k(0)", "485.15", "real", "Temperature conversions (212 + 0 + 273.15)" },

    /* G. Classical, Relativistic, Circuit & Quantum Physics (`70_physics.math`) */
    { "ke(2 kg, 36 kph)",      "100",        "real",    "Kinetic energy of 2 kg at 36 kph (10 m/s) -> 100 J" },
    { "mech_energy(2, 10, 5)", "198.0665",   "real",    "Total mechanical energy ke(2,10) + pe(2,5)" },
    { "proj_range(9.80665, 45)", "9.80665",  "real",    "Projectile range at 45 deg" },
    { "res_parallel(6, 3) + res_series(4, 4)", "10", "real", "Parallel (6||3 = 2) + series (4+4 = 8) resistors" },
    { "lorentz_gamma(0.6 * c0)", "1.25",     "real",    "Relativistic Lorentz factor gamma at v = 0.6c" },
    { "carnot_eff(300, 600)",  "0.5",        "real",    "Carnot heat engine efficiency (1 - 300/600)" },

    /* H. Chemistry: Gas Laws, Acid-Base pH, Kinetics & Decay (`80_chemistry.math`) */
    { "ph_from_h(0.001) + poh_from_ph(3)", "14", "real","Acid-base pH(10^-3 M) = 3, pOH = 11 -> sum 14" },
    { "henderson_hasselbalch(4.76, 0.2, 0.2)", "4.76", "real", "Buffer pH when [A-] == [HA] equals pKa" },
    { "decay_rem(100, 15, 5)", "12.5",       "real",    "Radioactive decay after 3 half-lives (100 -> 12.5)" },
    { "graham_rate_ratio(4, 16)", "2",       "real",    "Graham effusion ratio sqrt(16 / 4) = 2" },

    /* I. Dynamic Vector (`$vec` of any `T` type) & Statistical Suite */
    { "v := vec() ; push(v, 10) ; push(v, 20) ; push(v, 30) ; v",
      "[10, 20, 30]", "vec",
      "Growable vec<real>: vec(), push(v,x)" },
    { "v := vec(2) ; push(v, 5) ; push(v, 15) ; push(v, 25) ; cap(v)",
      "4", "real",
      "Auto-doubling capacity (cap 2 -> 4 on 3rd push)" },
    { "v := vec(3, 7) ; set(v, 1, 99) ; pop(v) + at(v, 1) + len(v)",
      "108", "real",
      "vec(cnt,init), set(v,i,x), pop(v) -> 7 + 99 + 2" },
    { "pts := vec() ; push(pts, vec2(1, 2)) ; push(pts, vec2(3, 4)) ; at(pts, -1)",
      "vec2(x=3, y=4)", "vec2",
      "Generic vec<vec2> + negative index at(pts, -1)" },
    { "cvec := vec(2, 1 + 2i) ; set(cvec, 1, 3 - 4i) ; cvec",
      "[1 + 2i, 3 - 4i]", "vec",
      "Generic vec<complex> fill & indexed mutation" },
    { "v := vec() ; push(v, 2) ; push(v, 4) ; push(v, 4) ; push(v, 4) ; push(v, 5) ; push(v, 5) ; push(v, 7) ; push(v, 9) ; stddev(v)",
      "2", "real",
      "Standard deviation of [2,4,4,4,5,5,7,9] -> 2" },
    { "p := vec() ; push(p, vec3(0,0,0)) ; push(p, vec3(3,4,0)) ; push(p, vec3(3,4,12)) ; vec3_path_len(p)",
      "17", "real",
      "3D trajectory path length over vec<vec3> (5 + 12)" },

    /* J. Host-Registered Extern Callbacks (`@fx_rate`, `@fx_set_rate`, `@fx_history`, `@kv_set`/`@kv_get`, `@elem_mass`) */
    { "fx_rate(\"USD\", \"INR\")",         "84",   "real", "Host extern '@fx_rate(\"USD\", \"INR\")'" },
    { "convert_fx(250, \"USD\", \"EUR\")", "230",  "real", "User 'convert_fx(250, USD, EUR)'" },
    { "convert_fx(92, \"EUR\", \"INR\")",  "8400", "real", "Cross-currency triangulation EUR -> USD -> INR" },
    { "50 to_inr + (92 to_usd) to_inr",    "12600","real", "Postfix currency ops 'to_inr' and 'to_usd'" },
    { "fx_with_fee(100, \"USD\", \"INR\", 1.5)", "8274", "real",
      "Multi-instruction 'fx_with_fee(100, USD, INR, 1.5%)' -> 8274" },
    { "fx_set_rate(\"INR\", 86.5) ; r := 100 to_inr ; fx_set_rate(\"INR\", 84) ; r", "8650", "real",
      "Realtime rate tick via '@fx_set_rate' updates 100 to_inr -> 8650 live" },
    { "p := vec() ; push(p, 10) ; push(p, 25) ; push(p, 15) ; vec_sum_fx(p, \"INR\")", "4200", "real",
      "USD price vector sum converted to INR via 'vec_sum_fx(p, INR)' -> 4200" },
    { "fx_history(\"USD\", \"INR\", 5)",   "[82, 82.5, 83, 83.5, 84]", "vec",
      "Vector-returning host extern '@fx_history' -> vec<real>" },
    { "fx_sma(\"USD\", \"INR\", 5, 3)",    "83.5", "real",
      "3-day Simple Moving Average over host '@fx_history' vector" },
    { "save_var(\"portfolio_usd\", 150) ; (load_var(\"portfolio_usd\") + 50) to_inr",
      "16800", "real",
      "Host persistent KV store '@kv_set' & '@kv_get' + 'to_inr'" },
    { "molar_mass_h2o() + molar_mass_co2()", "62.024", "real",
      "Host '@elem_mass' Periodic Table: H2O (18.015) + CO2 (44.009)" },

    /* K. Local Scopes `{ ... }`, `if ... or ... else`, `switch`, `? :` Ternary & `Tag : { ... }` */
    { "x = { a := 3 ; b := 4 ; a^2 + b^2 } ; { x = x + 5 ; 9999 } ; x",
      "30", "real",
      "Value-returning 'x = { ... }' + isolated '{ ... ; 9999 }' discarding unused value" },
    { "x := 10 ; y := { x := 99 ; tmp := 5 ; x + tmp } ; x * 1000 + y",
      "10104", "real",
      "Lexical scope shadowing: inner 'x := 99' leaves outer 'x = 10' intact" },
    { "{}",
      "<void>", "void",
      "Empty '{}' local scope block with no return value (<void>)" },
    { "classify_mach(0.5)",    "\"subsonic\"",   "str",  "if/or/else chain: 1st 'if' branch matched" },
    { "classify_mach(1.0)",    "\"transonic\"",  "str",  "if/or/else chain: 1st 'or' (else-if) matched" },
    { "classify_mach(2.5)",    "\"supersonic\"", "str",  "if/or/else chain: 2nd 'or' (else-if) matched" },
    { "classify_mach(6.0)",    "\"hypersonic\"", "str",  "if/or/else chain: final 'else' branch matched" },
    { "cnt := 0 ; r := if (0) { cnt = cnt + 100 ; 1 } or (1) { cnt = cnt + 10 ; 2 } or (1) { cnt = cnt + 1000 ; 3 } else { cnt = cnt + 5000 ; 4 } ; r * 100 + cnt",
      "210", "real",
      "Single-branch evaluation guarantee: only matching 'or' is evaluated" },
    { "planet_g(3) + planet_g(4) + planet_g(99)",
      "13.52765", "real",
      "switch block on numeric cases (Earth 9.80665 + Mars 3.721 + else 0)" },
    { "switch (\"Au\") { case \"Ag\", \"Cu\" : { 10 } case \"Au\" : { 79 } else : { 0 } }",
      "79", "real",
      "switch block on string cases with multi-value 'case \"Ag\", \"Cu\"'" },
    { "(10 > 5) ? 42 : 99",
      "42", "real",
      "Standard ternary operator 'cond ? x : y'" },
    { "s := 85 ; (s >= 90) ? 4 : (s >= 80) ? 3 : (s >= 70) ? 2 : 0",
      "3", "real",
      "Right-associative chained ternary 'c1 ? v1 : c2 ? v2 : c3 ? v3 : v4'" },
    { "k := 1 ; (k == 1) ? (k = k + 10) : (k = k + 1000) ; k",
      "11", "real",
      "Ternary lazy evaluation (untaken ':' branch is not evaluated)" },
    { "i := 0 ; x = Loop : { i = i + 1 ; if (i < 5) { jump(Loop) } ; i * 10 } ; x",
      "50", "real",
      "Assigned tagged scope 'x = Loop : { ... }' with self-jump" },
    { "x := 1 ; StepA : { x = x + 2 ; if (x < 10) { jump(StepB) } } ; StepB : { x = x * 2 ; if (x < 20) { jump(StepA) } } ; x",
      "36", "real",
      "Jumping forward & backward between 'StepA : { ... }' and 'StepB : { ... }'" },
    { "x := 5 ; if (x > 0) { jump(Done) } ; x = 999 ; Done : { x = x + 100 } ; x",
      "105", "real",
      "Forward 'jump(Done)' skipping intermediate assignment 'x = 999'" },
    { "collatz_steps(6)",
      "8", "real",
      "Tagged scopes 'Tick', 'Loop', 'Finish' with 'run(Tick)', 'Tick()', & 'jump'" },

    /* L. Loop `break` & Tagged `break(Tag)`, Chained/Distributed Booleans,
          Multi-Variable Declarations `x, y := 5`, Compound `+=`, and Dynamic Complex Promotion */
    { "s := 0 ; i := 1 ; (i <= 10) ?? { if (i == 5) { break } ; s += i ; i += 1 } ; s",
      "10", "real",
      "Unlabeled 'break' exiting while loop at i == 5 (1+2+3+4 = 10)" },
    { "tot := 0 ; Outer : { i := 1 ; (i <= 5) ?? { j := 1 ; (j <= 5) ?? { if (i == 2 and j == 3) { break(Outer) } ; tot += 10 ; j += 1 } ; i += 1 } ; tot = 9999 } ; tot",
      "70", "real",
      "Tagged 'break(Outer)' breaking out of nested inner & outer loops (7 * 10 = 70)" },
    { "tot := 0 ; i := 1 ; OuterLoop : (i <= 5) ?? { j := 1 ; (j <= 5) ?? { if (i == 2 and j == 2) { break(OuterLoop) } ; tot += 1 ; j += 1 } ; i += 1 } ; tot",
      "6", "real",
      "Directly tagged while loop 'OuterLoop : (i <= 5) ?? { ... break(OuterLoop) }'" },
    { "x := 1 ; y := 9 ; z := 3 ; (x == 1 && y == 2 || z == 3)",
      "1", "real",
      "Chained boolean precedence '(x == 1 && y == 2 || z == 3)' -> 1" },
    { "(1 and not 0) * 100 + (0 nor 0) * 10 + (1 xor 1) + (1 nand 1)",
      "110", "real",
      "Word boolean operators: 'and', 'not', 'nor', 'xor', 'nand'" },
    { "x := 1 ; y := 2 ; (x & y == 1 | 2)",
      "1", "real",
      "Distributed comparison '(x & y == 1 | 2)' when x=1, y=2 -> 1 (true)" },
    { "x := 1 ; y := 3 ; (x & y == 1 | 2)",
      "0", "real",
      "Distributed comparison '(x & y == 1 | 2)' when x=1, y=3 -> 0 (false)" },
    { "x := 2 ; y := 1 ; z := 4 ; ((x & y == 1 | 2) && (z == 3 | 4))",
      "1", "real",
      "Parenthesized chained distributed comparisons '((x & y == 1 | 2) && (z == 3 | 4))'" },
    { "x, y := 5 ; x * 10 + y",
      "55", "real",
      "Multi-variable declaration 'x, y := 5' (both x and y initialized to 5)" },
    { "a, b, c := 7 ; a + b + c",
      "21", "real",
      "3-variable declaration 'a, b, c := 7' -> 21" },
    { "x := 5 ; x += 1 ; x == 6",
      "1", "real",
      "Compound addition assignment 'x := 5 ; x += 1 ; x == 6' -> 1" },
    { "x := 10 ; x += 5 ; x -= 3 ; x *= 2 ; x /= 4 ; x %= 4 ; x ^= 3 ; x",
      "8", "real",
      "Chained compound assignments '+=', '-=', '*=', '/=', '%=', '^=' -> 8" },
    { "x, y := 5 ; x, y += 3 ; x * 10 + y",
      "88", "real",
      "Multi-variable compound assignment 'x, y := 5 ; x, y += 3' -> 88" },
    { "x := 5 ; x = x + 5i ; x",
      "5 + 5i", "complex",
      "Dynamic slot promotion: 'x := 5 ; x = x + 5i' promotes x from real to complex" },
    { "x := 5 ; x += 5i ; x.re * 10 + x.im",
      "55", "real",
      "Dynamic promotion via 'x += 5i' + struct field access 'x.re' and 'x.im'" },

    /* M. Concrete Entity Types, Concrete Unit Types, Complex Unit Promotion & Generic Unit `T` */
    { "x := 5m ; x *= 10m ; x += 1000cm2 ; x",
      "50.1", "real",
      "Compound unit arithmetic: 5m * 10m (50m2) + 1000cm2 (0.1m2) -> 50.1 [area] in SI m^2" },
    { "x := 5m ; x *= 10 ; x += 10cm ; x",
      "50.1", "real",
      "Postfix unit scaled by plain 10: 5m * 10 + 10cm -> 50.1m [length]" },
    { "x := 5m ; x *= 10m ; x += 1000cm2 ; unit_of(x)",
      "\"area\"", "str",
      "Compound unit product 5m * 10m promotes unit tag to \"area\"" },
    { "unit_of(2 + 5)",
      "\"none\"", "str",
      "Simple expression '2 + 5' has generic UnitType T bound to \"none\"" },
    { "x : real := 5 ; x += 10 ; x = 50m ; x",
      "50", "real",
      "Concrete 'x : real := 5' allows real operations & unit changes while type stays real" },
    { "x : real, length := 50m ; x += 10cm ; x *= 10 ; x",
      "501", "real",
      "Concrete 'x : real, length := 50m' allows like-unit '10cm' and scalar '*= 10'" },
    { "x : length := 50m ; x += 50im ; x",
      "50 + 50i", "complex",
      "Unit-locked 'x : length := 50m' + '50im' promotes entity type to complex (50 + 50i)" },
    { "x : length := 50m ; x += 50im ; unit_of(x)",
      "\"length\"", "str",
      "Promoted 'x : length := 50m ; x += 50im' retains concrete unit \"length\"" },
    { "x : length := 50m ; x += 50im ; type_of(x)",
      "\"complex\"", "str",
      "Promoted 'x : length := 50m ; x += 50im' has entity type \"complex\"" },
    { "x : length := 50m ; x += 50im ; x += 10cm ; x += 20icm ; x",
      "50.1 + 50.2i", "complex",
      "Complex length variable accumulates both real '10cm' and imaginary '20icm'" },
    { "x, y : real, length := 25m ; x += 50cm ; y += 150cm ; x + y",
      "52", "real",
      "Multi-variable concrete 'x, y : real, length := 25m' declaration & addition" },
    { "100m / 10sec",
      "10", "real",
      "Cross-unit operator '(d:real,length) / (t:real,time) -> real,speed'" },
    { "unit_of(100m / 10sec)",
      "\"speed\"", "str",
      "100m / 10sec produces concrete unit \"speed\"" },
    { "avg_speed(1.5 km, 50 sec)",
      "30", "real",
      "Unit-typed function 'avg_speed(d:real,length, t:real,time) -> real,speed'" },
    { "unit_of(avg_speed(1.5 km, 50 sec))",
      "\"speed\"", "str",
      "avg_speed(1.5 km, 50 sec) returns unit \"speed\"" },
    { "scale_unit(50m, 3)",
      "150", "real",
      "Generic UnitType T function 'scale_unit(x:real,T, k:real) -> real,T'" },
    { "unit_of(scale_unit(50m, 3))",
      "\"length\"", "str",
      "Generic UnitType T function preserves input unit \"length\"" },
    { "unit_span(100m, 2500cm)",
      "75", "real",
      "Generic UnitType T 'unit_span(a:real,T, b:real,T)' on length (100m, 2500cm)" },
    { "unit_span(10, 25)",
      "15", "real",
      "Generic UnitType T 'unit_span(a:real,T, b:real,T)' on unitless numbers (10, 25)" },
    { "2 MB + 500 KB", "2609152", "real",
      "User-defined custom unit group 'data_size' (2 MB + 500 KB in bytes)" },
    { "(10 MB) / (2 sec)", "5242880", "real",
      "User-defined cross-group operator: data_size / time -> bandwidth" },
    { "unit_of((10 MB) / (2 sec))", "\"bandwidth\"", "str",
      "Introspect custom user unit group via unit_of((10 MB) / (2 sec))" },
    { "buf : real, data_size := 1 MB ; buf += 512 KB ; buf", "1572864", "real",
      "Variable locked to custom user unit group 'real, data_size'" },

    /* 11. Prefix '!' Logical Negation Overload & Approximate Equality '~' Operator Tests */
    { "!0 * 10 + !1", "10", "real",
      "Prefix '!' operator overload on 0/1: !0 = 1, !1 = 0 -> 10" },
    { "x := 5 ; !(x == 5) * 10 + !(x == 4)", "1", "real",
      "Prefix '!' flips boolean conditions: !(x == 5) is 0, !(x == 4) is 1" },
    { "x := 3 ; (x == !(1 | 2))", "1", "real",
      "Distributed '!' in comparison: (x == !(1 | 2)) -> !((x==1) | (x==2)) = 1" },
    { "x := 2 ; (x == !(1 | 2))", "0", "real",
      "Distributed '!' in comparison when x=2 -> 0 (false)" },
    { "x := 1 ; y := 3 ; (x & !y == 1 | 2)", "1", "real",
      "Combined distributed '&', '|', and '!': (x & !y == 1 | 2) -> 1" },
    { "x := 4 ; (x == !4)", "0", "real",
      "Single distributed '!': (x == !4) -> !(x == 4) = 0" },
    { "!is_prime(100) * 10 + !is_even(5)", "11", "real",
      "Prefix '!' on boolean predicate functions !is_prime(100) and !is_even(5)" },
    { "5.001 ~ 5", "1", "real",
      "Approximate operator '~': 5.001 ~ 5 is true (|diff| = 0.001 <= 0.001)" },
    { "5.000001 ~ 5", "1", "real",
      "Approximate operator '~': 5.000001 ~ 5 is true (|diff| = 1e-6 <= 0.001)" },
    { "4.999 ~ 5", "1", "real",
      "Approximate operator '~': 4.999 ~ 5 is true (|diff| = 0.001 <= 0.001)" },
    { "5.01 ~ 5", "0", "real",
      "Approximate operator '~': 5.01 ~ 5 is false (|diff| = 0.01 > 0.001)" },
    { "5.002 ~ 5", "0", "real",
      "Approximate operator '~': 5.002 ~ 5 is false (|diff| = 0.002 > 0.001)" },
    { "!(5.01 ~ 5)", "1", "real",
      "Prefix '!' flipping approximate condition: !(5.01 ~ 5) -> 1 (true)" },

    /* 12. Standard Same-Unit-Type Conversions (`Library/65_conversions.math`) */
    { "(2.5 km) to_m", "2500", "real",
      "Postfix same-unit conversion: (2.5 km) to_m -> 2500" },
    { "(1.5 m) to_nm", "1500000000", "real",
      "Postfix same-unit conversion: (1.5 m) to_nm -> 1.5e+09" },
    { "(180 deg) to_rad", "3.141592654", "real",
      "Postfix angle conversion: (180 deg) to_rad -> pi" },
    { "(π rad) to_deg", "180", "real",
      "Postfix angle conversion: (pi rad) to_deg -> 180" },
    { "(2.5 km) into (1 m)", "2500", "real",
      "Universal infix 'in' same-unit conversion: (2.5 km) into (1 m) -> 2500" },
    { "(1.5 m) into (1 nm)", "1500000000", "real",
      "Universal infix 'in' same-unit conversion: (1.5 m) into (1 nm) -> 1.5e+09" },
    { "km_to_m(3.2) + m_to_cm(0.8)", "3280", "real",
      "Pairwise same-unit conversion functions km_to_m(3.2) + m_to_cm(0.8)" },
    { "rad_to_deg(deg_to_rad(135))", "135", "real",
      "Round-trip angle conversion rad_to_deg(deg_to_rad(135)) -> 135" },
    { "(2 hr) to_sec + (5000 ms) to_sec", "7205", "real",
      "Postfix time conversions: (2 hr) to_sec + (5000 ms) to_sec -> 7205" },

    /* 13. C-Style Array Notation `v[i]` for Vectors (`$vec2`, `$vec3`, Structs, `$vec`) */
    { "v := vec2(10, 20) ; v[0] * 100 + v[1]", "1020", "real",
      "C-style array indexing on $vec2: v[0]=v.x, v[1]=v.y" },
    { "v := vec3(2, 4, 6) ; v[0] = 10 ; v[1] += 5 ; v[2] *= 3 ; v", "vec3(x=10, y=9, z=18)", "vec3",
      "C-style array mutation & compound assignment on $vec3: v[0]=10, v[1]+=5, v[2]*=3" },
    { "c := circle(vec2(3, 7), 5) ; c[0][1] = 99 ; c[1] += 10 ; c", "circle(center=vec2(x=3, y=99), radius=15)", "circle",
      "Chained C-style array indexing on nested struct $circle: c[0][1] = 99" },
    { "a := vec(3, 10) ; a[0] = 5 ; a[1] += 7 ; a[-1] = 30 ; a[0] + a[1] + a[2]", "52", "real",
      "C-style array read/write/compound/negative index on dynamic $vec (5 + 17 + 30)" },
    { "pts := vec() ; push(pts, vec2(1, 2)) ; push(pts, vec2(3, 4)) ; pts[1][0] += 10 ; pts[1]", "vec2(x=13, y=4)", "vec2",
      "Chained C-style indexing pts[1][0] += 10 on dynamic vec<vec2>" },

    /* 14. Separate Multidimensional Matrix Data Type (`$mat`) Using `[...]` */
    { "M := [[1, 2, 3], [4, 5, 6]] ; M", "[[1, 2, 3], [4, 5, 6]]", "mat",
      "2D multidimensional matrix literal [[1, 2, 3], [4, 5, 6]] -> $mat" },
    { "M := [1, 2, 3 ; 4, 5, 6] ; M", "[[1, 2, 3], [4, 5, 6]]", "mat",
      "2D semicolon row-separated matrix literal [1, 2, 3 ; 4, 5, 6] -> $mat" },
    { "M := [10, 20 ; 30, 40] ; M[0, 1] + M[1][0]", "50", "real",
      "Matrix indexing via both multi-index M[0, 1] and chained M[1][0]" },
    { "M := [[1, 2], [3, 4]] ; M[0, 1] = 9 ; M[1][0] += 10 ; M", "[[1, 9], [13, 4]]", "mat",
      "In-place matrix element mutation via M[0, 1] = 9 and M[1][0] += 10" },
    { "A := [1, 2 ; 3, 4] ; B := [2, 0 ; 1, 2] ; A * B", "[[4, 4], [10, 8]]", "mat",
      "2D matrix multiplication A * B on $mat" },
    { "M := [4, 7 ; 2, 6] ; det(M) * 100 + trace(M)", "1010", "real",
      "det(M) and trace(M) on $mat (det=10, trace=10)" },
    { "M := [4, 7 ; 2, 6] ; inv(M) * [10, 10]", "[-1, 2]", "mat",
      "Gauss-Jordan matrix inverse inv(M) * 1D vector [10, 10] -> [-1, 2]" },
    { "M := [1, 2, 3 ; 4, 5, 6] ; transpose(M)", "[[1, 4], [2, 5], [3, 6]]", "mat",
      "Matrix transpose of 2x3 $mat -> 3x2 $mat" },
    { "T := [[[1, 2], [3, 4]], [[5, 6], [7, 8]]] ; T[1][0][1] += 10 ; T[1, 0, 1] + mat_rank(T)", "19", "real",
      "3D tensor $mat indexing, mutation T[1][0][1] += 10 (16), and mat_rank(T)=3 -> 19" },
    { "M := [[[1, 2], [3, 4]], [[5, 6], [7, 8]]] ; mat_shape(M)", "[2, 2, 2]", "vec",
      "3D tensor shape introspection mat_shape(M) -> [2, 2, 2]" },

    /* 15. High School Calculus Suite (`00_bootstrap.math` & `55_calculus.math`) */
    { "diff(x, x^3 + 2*x, 3)", "29", "real",
      "First derivative d/dx(x^3 + 2x) at x = 3 -> 29" },
    { "diff2(x, x^4, 2)", "48", "real",
      "Second derivative d^2/dx^2(x^4) at x = 2 -> 48" },
    { "diff_n(x, x^4, 2, 3)", "48", "real",
      "Third derivative d^3/dx^3(x^4) at x = 2 -> 48" },
    { "integrate(x, 3 * (x^2), 0, 2)", "8", "real",
      "Definite integral int_0^2 3x^2 dx -> 8" },
    { "ntg(x, sin(x), 0, π) ~ 2", "1", "real",
      "Definite integral int_0^pi sin(x) dx ~ 2" },
    { "lim(x, sin(x) / x, 0)", "1", "real",
      "Removable singularity limit: lim_{x->0} sin(x)/x -> 1" },
    { "lim(x, (x^2 - 4) / (x - 2), 2)", "4", "real",
      "Algebraic limit: lim_{x->2} (x^2 - 4)/(x - 2) -> 4" },
    { "lim_right(x, abs(x) / x, 0) * 10 + lim_left(x, abs(x) / x, 0)", "9", "real",
      "One-sided limits at 0 for |x|/x: right (+1)*10 + left (-1) = 9" },
    { "sigma(k, k^2, 1, 5)", "55", "real",
      "Discrete summation sigma_{k=1..5} k^2 -> 55" },
    { "prod(k, k, 1, 5)", "120", "real",
      "Discrete product prod_{k=1..5} k -> 120" },
    { "c := vec() ; push(c, 1) ; push(c, -3) ; push(c, 2) ; poly_def_integ(c, 0, 3)", "7.5", "real",
      "Exact polynomial definite integral int_0^3 (1 - 3x + 2x^2) dx -> 7.5" },
    { "c := vec() ; push(c, 0) ; push(c, 0) ; push(c, 1) ; tangent_slope(c, 3) * 10 + normal_slope(c, 3) * -6", "61", "real",
      "Tangent slope (6) & normal slope (-1/6) of y = x^2 at x = 3" },
    { "taylor_exp(1, 12) ~ e", "1", "real",
      "Maclaurin series taylor_exp(1, 12) ~ e" },

    /* 16. Expanded Commercial & General Arithmetic (`10_arith.math`) */
    { "pct_of(45, 180) + pct_val(15, 200)", "55", "real",
      "Percentages: pct_of(45, 180)=25% + pct_val(15, 200)=30 -> 55" },
    { "pct_inc(500, 20) + pct_dec(500, 20)", "1000", "real",
      "Percentage increase (600) + decrease (400) -> 1000" },
    { "profit_pct(400, 500) + successive_discount(20, 10)", "53", "real",
      "Profit % (25%) + successive discount (28%) -> 53" },
    { "simple_interest(1000, 10, 2) + compound_interest(1000, 10, 2)", "410", "real",
      "Simple interest (200) + compound interest (210) -> 410" },
    { "ci_si_diff_2yr(5000, 10)", "50", "real",
      "2-year CI - SI difference on 5000 at 10% -> 50" },
    { "work_together_2(6, 12) + work_together_3(6, 12, 4)", "6", "real",
      "Work & time: 2-person (4 days) + 3-person (2 days) -> 6" },
    { "pipe_fill_with_leak(4, 12) + man_days_solve(10, 6, 1, 15, 2)", "14", "real",
      "Pipe cistern with leak (6 hr) + chain rule man-days (8 days) -> 14" },
    { "ap_sum(2, 3, 10) + gp_sum(2, 3, 4)", "235", "real",
      "Arithmetic progression sum (155) + geometric progression sum (80) -> 235" },
    { "arith_mean(4, 16) + geom_mean(4, 16) + harm_mean(4, 16)", "24.4", "real",
      "AM (10) + GM (8) + HM (6.4) of 4 and 16 -> 24.4" },
    { "avg_speed_2(40, 60) + boat_still_speed(15, 9) + stream_speed(15, 9)", "63", "real",
      "Harmonic average speed (48) + boat in still water (12) + stream (3) -> 63" },

    /* 17. Expanded High School Physics, Chemistry & Quantitative Biology */
    { "suvat_v(10, 2, 5) + suvat_s(10, 2, 5) + suvat_nth_sec(10, 2, 5)", "114", "real",
      "SUVAT kinematics: v(20) + s(75) + s_5th(19) -> 114" },
    { "elastic_v1(2, 10, 2, 0) + elastic_v2(2, 10, 2, 0)", "10", "real",
      "1D elastic collision velocity exchange -> 0 + 10 = 10" },
    { "shm_vel(2, 5, 3) + abs(shm_acc(2, 3))", "20", "real",
      "SHM velocity (8) + acceleration magnitude (12) -> 20" },
    { "mirror_image_v(10, 30) + lens_power_dioptre(0.25)", "19", "real",
      "Ray optics mirror image length (15) + lens power (4 D) -> 19" },
    { "rlc_impedance(30, 60, 20) + transformer_v2(220, 100, 50)", "160", "real",
      "AC RLC impedance (50 ohm) + step-down transformer (110 V) -> 160" },
    { "bohr_energy_ev(2, 1)", "-3.4", "real",
      "Bohr hydrogen n=2 energy level -> -3.4 eV" },
    { "percent_yield(42, 50) + normality_from_molarity(1.5, 2)", "87", "real",
      "Chemistry % yield (84%) + normality (3 N) -> 87" },
    { "boyle_p2(2, 6, 3) + charles_v2(10, 300, 450)", "19", "real",
      "Boyle P2 (4) + Charles V2 (15) -> 19" },
    { "bp_elevation(2, 0.52, 1.5) + fp_depression(2, 1.86, 1.5)", "7.14", "real",
      "Colligative boiling elevation (1.56) + freezing depression (5.58) -> 7.14" },
    { "weak_acid_ph(10 ^ -5, 0.1) + pka_from_ka(10 ^ -5)", "8", "real",
      "Weak acid pH (3) + pKa (5) -> 8" },
    { "cell_emf(0.34, -0.76)", "1.1", "real",
      "Electrochemical Daniell cell EMF (0.34 - (-0.76)) -> 1.1 V" },
    { "hw_homo_dom(0.6) + hw_hetero(0.6) + hw_homo_rec(0.6)", "1", "real",
      "Hardy-Weinberg equilibrium p^2 + 2pq + q^2 = 1" },
    { "hw_carrier_count(0.04, 1000)", "320", "real",
      "Hardy-Weinberg heterozygous carriers (q^2=0.04, N=1000) -> 320" },
    { "chargaff_g_from_a(20) + dna_turns(100) + pcr_copies(10)", "1064", "real",
      "Chargaff %G (30) + B-DNA helical turns (10) + PCR 2^10 (1024) -> 1064" },
    { "michaelis_menten(100, 5, 5) + q10_rate(10, 20, 30, 2)", "70", "real",
      "Michaelis-Menten v0 at S=Km (50) + Q10 rate (20) -> 70" },
    { "water_potential(-8, 3) + diffusion_pressure_deficit(12, 5)", "2", "real",
      "Plant physiology water potential (-5) + DPD (7) -> 2" },
    { "total_magnification(10, 40) + mitotic_index(18, 200)", "409", "real",
      "Microscopy magnification (400x) + mitotic index (9%) -> 409" },
    { "(cardiac_output(72, 70) + mean_arterial_pressure(120, 80)) ~ 98.3733", "1", "real",
      "Cardiac output (5.04 L/min) + MAP (93.3333 mmHg) ~ 98.3733" },
    { "lincoln_petersen_pop(40, 60, 12) + trophic_energy_at(10000, 2)", "300", "real",
      "Mark-recapture population (200) + 10% trophic energy law (100) -> 300" },

    /* 18. All 8 Streamlined Ergonomic Features + Keywordless `(i in 1...5){}` / `(v in vec){}` & `loop` */
    { "tau_const ~ (2 * π)", "1", "real",
      "[Feat 1] Shorthand unquoted constant 'tau_const = 2π'" },
    { "hyp_3d(3, 4) * 100 + hyp_3d(3, 4, 12)", "513", "real",
      "[Feat 1 & 6] Shorthand 'hyp_3d(x, y, z = 0)' with default z=0 (5) and z=12 (13)" },
    { "log_base(1000) * 10 + log_base(8, 2)", "33", "real",
      "[Feat 1 & 6] Shorthand 'log_base(x, base = 10)' with default base=10 (3) and base=2 (3)" },
    { "round(3.6) * 100 + round(3.14159, 2)", "403.14", "real",
      "[Feat 6] Default parameter 'round(x, digits = 0)': round(3.6)=4, round(3.14159, 2)=3.14" },
    { "fx_with_fee(100, \"USD\", \"INR\")", "8274", "real",
      "[Feat 6] Default fee_pct=1.5 in 'fx_with_fee(100, \"USD\", \"INR\")' -> 8274" },
    { "vec(10, 20, 30, 40)", "[10, 20, 30, 40]", "vec",
      "[Feat 2] Variadic 'vec(10, 20, 30, 40)' constructor -> [10, 20, 30, 40]" },
    { "1>..<5", "[2, 3, 4]", "vec",
      "[Feat 2] Exclusive-exclusive range operator '1>..<5' -> [2, 3, 4]" },
    { "1...5", "[1, 2, 3, 4, 5]", "vec",
      "[Feat 2] Inclusive-inclusive 3-dot range operator '1...5' -> [1, 2, 3, 4, 5]" },
    { "(5 in 1>..<100) * 1000 + (1 in 1>..<100) * 100 + (100 in 1...100) * 10 + (v := vec(1,3,5,7) ; 8 in v)", "1010", "real",
      "[Feat 2] Boolean 'in' operator: 5 in 1>..<100 (1), 1 in 1>..<100 (0), 100 in 1...100 (1), 8 in v (0)" },
    { "s := 0 ; (i in 1...5) { s += i } ; s", "15", "real",
      "[Feat 3] Context-aware iterator '(i in 1...5) { s += i }' -> 15" },
    { "s := 0 ; (i in 1>..<5) { s += i^2 } ; s", "29", "real",
      "[Feat 3] Exclusive-exclusive range loop '(i in 1>..<5) { s += i^2 }' (2^2+3^2+4^2 = 29)" },
    { "arr := vec(10, 20, 30, 40) ; tot := 0 ; (v in arr) { tot += v } ; tot", "100", "real",
      "[Feat 3] Context-aware vector iterator '(v in arr) { tot += v }' -> 100" },
    { "pts := list(vec2(1, 2), vec2(3, 4), vec2(5, 6)) ; sx := 0 ; (p in pts) { sx += p.x * p.y } ; sx", "44", "real",
      "[Feat 3] Struct-vector iterator '(p in pts) { sx += p.x * p.y }' -> 2+12+30 = 44" },
    { "if (5 in 1...100) { (25km into m) } else { 0m }", "25000", "real",
      "[Feat 3] Context-aware '(5 in 1...100)' conditional + '(25km into m)' bare-unit conversion -> 25000" },
    { "unit_of(25km into m)", "\"length\"", "str",
      "[Feat 3] '(25km into m)' preserves entity type 'real' and unit type 'length'" },
    { "s := 0 ; i in 1...5 { s += i } ; k in 1..<4 ?? { s += k * 10 } ; s", "75", "real",
      "[Feat 3] Unparenthesized undefined iterator variable 'i in 1...5 { ... }' (15) + 'k in 1..<4 ?\? { ... }' (60)" },
    { "m := 5 ; (25km into m) * m", "125000", "real",
      "[Feat 3] Dual variable/unit disambiguation: scalar 'm := 5' still allows '25km into m' (25000m * 5 = 125000m)" },
    { "km := vec(10, 25, 30) ; 25m into km", "0.025", "real",
      "[Feat 3] Dual 'km := vec(10,25,30)': '25m into km' auto-resolves to unit conversion (0.025 km)" },
    { "km := vec(10, 25, 30) ; (25 in km) * 10 + (25m in (km))", "10", "real",
      "[Feat 3] Dual 'km := vec(10,25,30)': '25 in km' (1) and parenthesized '25m in (km)' forced vec lookup (0)" },
    { "km := vec(10m, 25m, 50m) ; 25m in km", "1", "real",
      "[Feat 3] Dual length vector 'km := vec(10m,25m,50m)': '25m into km' resolves to membership (1)" },
    { "km := vec(10m, 25m, 50m) ; 25m into 1km", "0.025", "real",
      "[Feat 3] Dual length vector 'km := vec(10m,25m,50m)': '25m in 1km' explicitly forces unit conversion (0.025)" },
    { "len(1..<5) * 10 + len(1>..5)", "44", "real",
      "[Feat 2] Half-open ranges '1..<5' ([1,2,3,4]) and '1>..5' ([2,3,4,5])" },
    { "k := 0 ; loop : { k += 1 ; if (k == 7) { break(loop, k * 10) } ; jump(loop) }", "70", "real",
      "[Feat 3] 'loop' used freely as user tag 'loop : { ... break(loop, k*10) }' -> 70" },
    { "(φ ^ 2 - φ) ~ 1", "1", "real",
      "[Feat 3] Golden ratio symbol 'φ' identity φ^2 - φ == 1" },
    { "(φ == phi) * 10 + (ϕ == phi)", "11", "real",
      "[Feat 3] Golden ratio symbols 'φ' and 'ϕ' equal 'phi'" },
    { "cnt := 0 ; Outer : (i in 1...5) ?? { (j in 1...5) ?? { if (i * j > 6) { break(Outer) } ; cnt += 1 } } ; cnt", "8", "real",
      "[Feat 3] Tagged nested loop 'Outer : (i in 1...5) ?? { ... break(Outer) }' -> 8" },
    { "sum_squares_to(5) + vec_dot_self(vec(2, 3, 6))", "104", "real",
      "[Feat 1 & 3] User functions 'sum_squares_to(5)' (55) + 'vec_dot_self(vec(2,3,6))' (49) -> 104" },
    { "x := 4 ; 2x^2 + 3x + 1", "45", "real",
      "[Feat 4] Implicit multiplication '2x^2 + 3x + 1' at x=4 -> 32 + 12 + 1 = 45" },
    { "x := 5 ; 3(x + 1) + (x + 1)(x - 1)", "42", "real",
      "[Feat 4] Implicit multiplication '3(x + 1) + (x + 1)(x - 1)' at x=5 -> 18 + 24 = 42" },
    { "2 sin(π / 2) + 2 √16 + (2π / π)", "12", "real",
      "[Feat 4] Implicit multiplication '2 sin(π/2) + 2 √16 + (2π / π)' -> 2 + 8 + 2 = 12" },
    { "v := vec() ; v.push(10).push(20).push(30).vec_sum() + v.len()", "63", "real",
      "[Feat 5] UFCS dot-chaining 'v.push(10).push(20).push(30).vec_sum() + v.len()' -> 63" },
    { "M := [4, 7 ; 2, 6] ; M.transpose().det() + (3 - 4i).cabs()", "15", "real",
      "[Feat 5] UFCS dot-chaining 'M.transpose().det() + (3 - 4i).cabs()' -> 10 + 5 = 15" },
    { "solve(x, x^2 == 9, 2) * 10 + solve(x, 2x + 5 == 17)", "36", "real",
      "[Feat 7] General equation solver 'solve(x, x^2 == 9, 2)' (3) and 'solve(x, 2x + 5 == 17)' (6)" },
    { "solve(x, cos(x) == x, 0.5) ~ 0.739085", "1", "real",
      "[Feat 7] Transcendental equation solver 'solve(x, cos(x) == x, 0.5)' -> Dottie number ~0.739085" },
    { "fmin(x, (x - 3)^2 + 5, 0, 10) * 10 + fmax(x, -(x - 4)^2 + 12, 0, 10)", "34", "real",
      "[Feat 7] Extrema finders 'fmin(x, (x-3)^2+5, 0, 10)' (3) and 'fmax(x, -(x-4)^2+12, 0, 10)' (4)" },
    { "r1, r2 := quad_roots(1, -5, 6) ; r1 * 10 + r2", "32", "real",
      "[Feat 8] Struct destructuring 'r1, r2 := quad_roots(1, -5, 6)' -> r1=3, r2=2" },
    { "a, b, c := vec(10, 20, 30) ; p, q := [100, 200] ; a + b + c + p + q", "360", "real",
      "[Feat 8] Vector & matrix destructuring 'a, b, c := vec(10,20,30)' and 'p, q := [100,200]'" },

    /* 19. All 5 New Language Constructs:
           #1 Default Newline `;` Inside & Outside Scopes (Compulsory `;` on Single Line)
           #2 `continue`, `continue(Tag)`, `return expr`, Value-Returning `break expr`
           #3 Vector & Matrix Slicing (`v[1..3]`, `M[0..1, 1..2]`) & Comprehensions (`[x^2 for x in ...]`)
           #4 First-Class Lambdas (`(x) => x^2`) & Higher-Order `.map()`, `.filter()`, `.reduce()`
           #5 Pattern-Matched `switch` (Ranges, Relational Guards, Struct/Vector Destructuring & `if` Guards) */
    { "x := 10\ny := 20\nx + y", "30", "real",
      "[Construct 1] Top-level newline as default ';' operator outside any scope" },
    { "{\n  a := 5\n  b := 7\n  a * b\n}", "35", "real",
      "[Construct 1] Newline as default ';' operator inside '{ ... }' scope" },
    { "(\n  u := 4\n  v := 6\n  u^2 + v^2\n)", "52", "real",
      "[Construct 1] Newline as default ';' operator inside '( ... )' block" },
    { "s := 0\n(i in 1...6) ?? {\n  if (i % 2 == 0) { continue }\n  s += i\n}\ns", "9", "real",
      "[Construct 1 & 2] Multi-line '(i in 1...6) ?\?' with 'continue' skipping even numbers (1+3+5 = 9)" },
    { "s := 0 ; i := 0 ; (i < 6) ?? { i += 1 ; if (i == 3 || i == 5) { continue } ; s += i } ; s", "13", "real",
      "[Construct 2] 'continue' inside '?\?' while loop (1+2+4+6 = 13)" },
    { "tot := 0 ; Outer : (i in 1...3) ?? { (j in 1...3) ?? { if (j == 2) { continue(Outer) } ; tot += i * 10 + j } } ; tot", "63", "real",
      "[Construct 2] Tagged 'continue(Outer)' jumping to next outer loop iteration (11+21+31 = 63)" },
    { "found := (x in 1...20) ?? { if (x^2 > 50) { break x } }\nfound", "8", "real",
      "[Construct 2] Value-returning 'break x' from range loop (first x with x^2 > 50 is 8)" },
    { "res := Outer : (i in 1...5) ?? { (j in 1...5) ?? { if (i * j == 12) { break(Outer, i * 100 + j) } } } ; res", "304", "real",
      "[Construct 2] Tagged value-returning 'break(Outer, val)' from nested loop (i=3, j=4 -> 304)" },
    { "k := 10 ; { k += 1 } ?? (k < 5) ; k", "11", "real",
      "[Do-While] Post-tested '{ k += 1 } ?\? (k < 5)' runs body once even when cond is initially false" },
    { "k := 0 ; s := 0 ; { k += 1 ; s += k } ?? (k < 5) ; s", "15", "real",
      "[Do-While] Post-tested '{ k += 1 ; s += k } ?\? (k < 5)' accumulates 1+2+3+4+5 = 15" },
    { "k := 0 ; s := 0 ; { k += 1 ; if (k == 3) { continue } ; s += k } ?? (k < 5) ; s", "12", "real",
      "[Do-While] 'continue' inside '{ ... } ?\? (cond)' jumps to bottom condition check (1+2+4+5 = 12)" },
    { "k := 10 ; (k += 5) ?!? (k < 5) ; k", "15", "real",
      "[Do-While] Dedicated '?!?' do-while operator '(k += 5) ?!? (k < 5)' runs single-expr body once -> 15" },
    { "tot := 0 ; i := 0 ; Outer : { i += 1 ; j := 0 ; { j += 1 ; if (j == 2) { continue(Outer) } ; tot += i * 10 + j } ?? (j < 3) } ?? (i < 3) ; tot", "63", "real",
      "[Do-While] Tagged 'Outer : { ... } ?\? (i < 3)' with 'continue(Outer)' (11+21+31 = 63)" },
    { "safe_sqrt_ret(-4) * 100 + safe_sqrt_ret(0) * 10 + safe_sqrt_ret(9)", "-70", "real",
      "[Construct 2] Early 'return expr' inside user function 'safe_sqrt_ret' (-100 + 0 + 30 = -70)" },
    { "x := 10\nreturn x * 5\nx = 999", "50", "real",
      "[Construct 2] Top-level early 'return expr' halting evaluation immediately -> 50" },
    { "v := vec(10, 20, 30, 40, 50) ; v[1...3]", "[20, 30, 40]", "vec",
      "[Construct 3] Inclusive vector range slicing 'v[1...3]' -> [20, 30, 40]" },
    { "v := vec(10, 20, 30, 40, 50) ; v[0>..<4]", "[20, 30, 40]", "vec",
      "[Construct 3] Exclusive vector range slicing 'v[0>..<4]' -> [20, 30, 40]" },
    { "v := vec(10, 20, 30, 40, 50) ; v[-3...-1]", "[30, 40, 50]", "vec",
      "[Construct 3] Negative-index inclusive vector slicing 'v[-3...-1]' -> [30, 40, 50]" },
    { "v := vec(1, 2, 3, 4, 5) ; v[1...3] = 99 ; v", "[1, 99, 99, 99, 5]", "vec",
      "[Construct 3] In-place scalar slice assignment 'v[1...3] = 99'" },
    { "M := [1, 2, 3 ; 4, 5, 6 ; 7, 8, 9] ; M[0...1, 1...2]", "[[2, 3], [5, 6]]", "mat",
      "[Construct 3] 2D submatrix slicing 'M[0...1, 1...2]' -> [[2, 3], [5, 6]]" },
    { "M := [1, 2, 3 ; 4, 5, 6 ; 7, 8, 9] ; M[2, 0...1]", "[7, 8]", "mat",
      "[Construct 3] Row-to-1D-slice indexing 'M[2, 0...1]' -> [7, 8]" },
    { "[x^2 for x in 1...5]", "[1, 4, 9, 16, 25]", "vec",
      "[Construct 3] Vector comprehension '[x^2 for x in 1...5]'" },
    { "[x^2 for x in 1...10 if x % 2 == 0]", "[4, 16, 36, 64, 100]", "vec",
      "[Construct 3] Filtered vector comprehension '[x^2 for x in 1...10 if x % 2 == 0]'" },
    { "[p.x + p.y for p in list(vec2(1, 2), vec2(3, 4), vec2(5, 6)) if p.x > 1]", "[7, 11]", "vec",
      "[Construct 3] Comprehension over struct vector with 'if' guard" },
    { "f := (x) => x^2 + 1 ; f(5) + call(f, 3)", "36", "real",
      "[Construct 4] First-class lambda 'f := (x) => x^2 + 1' invoked via 'f(5)' (26) and 'call(f, 3)' (10)" },
    { "add_fn := (a, b) => a + b * 10 ; add_fn(4, 3) + ((x) => x * 3)(7)", "55", "real",
      "[Construct 4] 2-arg lambda 'add_fn(4, 3)' (34) + immediate IIFE '((x) => x * 3)(7)' (21)" },
    { "k := 10 ; scale := x => x * k ; scale(7)", "70", "real",
      "[Construct 4] Single-ident lambda 'x => x * k' capturing lexical variable 'k = 10'" },
    { "(1...5).map(x => x^2).filter(x => x > 5).sum()", "50", "real",
      "[Construct 4] UFCS higher-order pipeline '(1...5).map(x => x^2).filter(x => x > 5).sum()' (9+16+25 = 50)" },
    { "(1...5).reduce((a, b) => a + b) * 1000 + (1...4).fold(10, (acc, x) => acc + x)", "15020", "real",
      "[Construct 4] '.reduce((a, b) => a + b)' (15) and '.fold(10, (acc, x) => acc + x)' (20)" },
    { "v := vec(2, 4, 7, 8) ; v.any(x => x % 2 != 0) * 100 + v.all(x => x > 0) * 10 + v.count_if(x => x >= 4)", "113", "real",
      "[Construct 4] Higher-order predicates '.any()' (1), '.all()' (1), and '.count_if()' (3)" },
    { "v := vec(3, 8, 15, 22) ; v.find(x => x > 10)", "15", "real",
      "[Construct 4] Higher-order '.find(x => x > 10)' returning first matching element (15)" },
    { "M := [1, 2 ; 3, 4] ; M.map(x => x * 10)", "[[10, 20], [30, 40]]", "mat",
      "[Construct 4] Higher-order '.map(x => x * 10)' preserving 2D matrix shape" },
    { "grade_band(95) == \"A\" && grade_band(84) == \"B\" && grade_band(72) == \"C\" && grade_band(50) == \"F\" && grade_band(120) == \"invalid\"", "1", "real",
      "[Construct 5] Pattern-matched 'switch' with relational 'case >= 90:' and range 'case 80..89:'" },
    { "describe_vec2(vec2(0, 0)) == \"origin\" && describe_vec2(vec2(0, 9)) == \"y-axis\" && describe_vec2(vec2(4, 4)) == \"diagonal\" && describe_vec2(vec2(2, 5)) == \"general\"", "1", "real",
      "[Construct 5] Pattern-matched 'switch' with struct destructuring 'case vec2(0, y):' & 'if x == y'" },
    { "p := vec2(0, 7) ; switch (p) { case vec2(0, y) : { y * 10 } case vec2(x, y) : { x + y } else : { 0 } }", "70", "real",
      "[Construct 5] Struct destructuring in 'switch' binding field variable 'y = 7' -> 70" },
    { "arr := [0, 12] ; switch (arr) { case [0, y] if y > 5 : { y * 3 } case [a, b] : { a + b } else : { -1 } }", "36", "real",
      "[Construct 5] Vector/1D-matrix destructuring 'case [0, y] if y > 5:' binding 'y = 12' -> 36" },

    /* 20. Postfix "_" (@tostr), Prefix "_" (@fromstr), "into" Operator, and Parameterized
           Containers `vec<T>()`, `list<T>()`, `stack<T>()`, `queue<T>()` */
    { "42_", "\"42\"", "str",
      "[Op _] Postfix \"_\" on real literal 42_ -> \"42\" ($str)" },
    { "(3 + 4i)_", "\"3 + 4i\"", "str",
      "[Op _] Postfix \"_\" on complex (3 + 4i)_ -> \"3 + 4i\"" },
    { "vec2(3, 4)_", "\"vec2(x=3, y=4)\"", "str",
      "[Op _] Postfix \"_\" on struct vec2(3, 4)_ -> \"vec2(x=3, y=4)\"" },
    { "vec(10, 20, 30)_", "\"[10, 20, 30]\"", "str",
      "[Op _] Postfix \"_\" on dynamic vector vec(10, 20, 30)_ -> \"[10, 20, 30]\"" },
    { "[1, 2 ; 3, 4]_", "\"[[1, 2], [3, 4]]\"", "str",
      "[Op _] Postfix \"_\" on 2D matrix [1, 2 ; 3, 4]_ -> \"[[1, 2], [3, 4]]\"" },
    { "_\"42\" + 8", "50", "real",
      "[Op _] Prefix \"_\" converting \"42\" -> real 42 (+ 8 = 50)" },
    { "_\"3 + 4i\"", "3 + 4i", "complex",
      "[Op _] Prefix \"_\" converting \"3 + 4i\" -> complex 3 + 4i" },
    { "_\"vec2(3, 4)\"", "vec2(x=3, y=4)", "vec2",
      "[Op _] Prefix \"_\" converting \"vec2(3, 4)\" -> struct vec2(x=3, y=4)" },
    { "_\"[10, 20, 30]\"", "[10, 20, 30]", "vec",
      "[Op _] Prefix \"_\" converting \"[10, 20, 30]\" -> dynamic $vec [10, 20, 30]" },
    { "_\"[[1, 2], [3, 4]]\"", "[[1, 2], [3, 4]]", "mat",
      "[Op _] Prefix \"_\" converting \"[[1, 2], [3, 4]]\" -> 2D $mat" },
    { "unit_of(_\"100 km\")", "\"length\"", "str",
      "[Op _] Prefix \"_\" converting \"100 km\" preserves unit tag \"length\"" },
    { "_(vec3(1, 2, 3)_)", "vec3(x=1, y=2, z=3)", "vec3",
      "[Op _] Round-trip string conversion _(vec3(1, 2, 3)_) -> vec3(x=1, y=2, z=3)" },
    { "5 into 5i", "5 + 5i", "complex",
      "[Op into] '5 into 5i' combines real and iota -> complex 5 + 5i" },
    { "5i into 5", "5 + 5i", "complex",
      "[Op into] '5i into 5' combines iota and real -> complex 5 + 5i" },
    { "5 into 5", "10", "real",
      "[Op into] Likewise entities/units '5 into 5' returns sum -> 10" },
    { "50m into cm", "5000", "real",
      "[Op into] Likewise entities/units '50m into 20cm' returns sum -> 50.2 [length]" },
    { "5i into 3i", "8i", "iota",
      "[Op into] Likewise imaginary '5i into 3i' returns sum -> 8i" },
    { "(1 + 2i) into (3 + 4i)", "4 + 6i", "complex",
      "[Op into] Likewise complex '(1 + 2i) into (3 + 4i)' returns sum -> 4 + 6i" },
    { "vec2(1, 2) into vec2(3, 4)", "vec2(x=4, y=6)", "vec2",
      "[Op into] Likewise struct 'vec2(1, 2) into vec2(3, 4)' returns sum -> vec2(x=4, y=6)" },
    { "vec3(1, 2, 3) into vec3(4, 5, 6)", "vec3(x=5, y=7, z=9)", "vec3",
      "[Op into] Likewise struct 'vec3(1, 2, 3) into vec3(4, 5, 6)' returns sum -> vec3(x=5, y=7, z=9)" },
    { "2 into (3 + 4i)", "5 + 4i", "complex",
      "[Op into] '2 into (3 + 4i)' adds real component into complex -> 5 + 4i" },
    { "3i into (3 + 4i)", "3 + 7i", "complex",
      "[Op into] '3i into (3 + 4i)' adds imaginary component into complex -> 3 + 7i" },
    { "7 into vec2(3, 4)", "vec3(x=3, y=4, z=7)", "vec3",
      "[Op into] '7 into vec2(3, 4)' promotes vec2 + z -> vec3(x=3, y=4, z=7)" },
    { "vec2(3, 4) into 7", "vec3(x=3, y=4, z=7)", "vec3",
      "[Op into] 'vec2(3, 4) into 7' promotes vec2 + z -> vec3(x=3, y=4, z=7)" },
    { "40 into vec(10, 20, 30)", "[10, 20, 30, 40]", "vec",
      "[Op into] '40 into vec(10, 20, 30)' pushes element into vector (alternate for vec.push(T))" },
    { "30 into list(10, 20)", "[10, 20, 30]", "vec",
      "[Op into] '30 into list(10, 20)' pushes element into list -> [10, 20, 30]" },
    { "v := vec<real>() ; 10 into v ; 20 into v ; 30 into v ; v", "[10, 20, 30]", "vec",
      "[Op into & vec<T>] Sequential 'T into v' on typed 'vec<real>()' -> [10, 20, 30]" },
    { "10 into 20 into vec<real>()", "[20, 10]", "vec",
      "[Op into & vec<T>] Right-associative chained '10 into 20 into vec<real>()' -> [20, 10]" },
    { "vec<real>(4).cap() * 100 + vec<real>(3, 7).sum()", "421", "real",
      "[vec<T>] Typed capacity 'vec<real>(4)' (cap 4) and fill 'vec<real>(3, 7)' (sum 21)" },
    { "vec<vec2>(vec2(1, 2), vec2(3, 4))", "[vec2(x=1, y=2), vec2(x=3, y=4)]", "vec",
      "[vec<T>] Variadic typed struct vector 'vec<vec2>(vec2(1, 2), vec2(3, 4))'" },
    { "d := vec<real, length>(1 km, 500 m) ; d[0] + d[1]", "1500", "real",
      "[vec<T, unit>] Typed & unit-locked 'vec<real, length>(1 km, 500 m)' -> 1500 m" },
    { "list<complex>(1 + 2i, 3 - 4i)", "[1 + 2i, 3 - 4i]", "vec",
      "[list<T>] Parameterized 'list<complex>(1 + 2i, 3 - 4i)'" },
    { "s := stack<real>(10, 20) ; s.push(30) ; s.peek() * 1000 + s.pop() * 10 + s.len()", "30302", "real",
      "[stack<T>] LIFO 'stack<real>(10, 20)' with .push(30), .peek() (30), .pop() (30), .len() (2)" },
    { "q := queue<real>(10, 20) ; q.enqueue(30) ; q.front() * 1000 + q.dequeue() * 10 + q.front()", "10120", "real",
      "[queue<T>] FIFO 'queue<real>(10, 20)' with .enqueue(30), .front() (10), .dequeue() (10), .front() (20)" },
    { "v : vec<vec2> := vec<vec2>() ; vec2(5, 6) into v ; v[0].x + v[0].y", "11", "real",
      "[vec<T> annot] Variable type lock 'v : vec<vec2> := vec<vec2>()' + 'vec2(5, 6) into v' -> 11" },

    /* 21. Compound Unit Cancellation & SI Dimensional Algebra (`F=ma`, `s=d/t`, `W=Fd`, `P=W/t`, `Pa=F/A`, `u/u->none`) */
    { "m := 10 kg ; a := 9.8 mps2 ; F := m * a ; F", "98", "real",
      "[Compound Units] F = m * a: (10 kg) * (9.8 mps2) -> 98 N [force]" },
    { "unit_of((10 kg) * (9.8 mps2))", "\"force\"", "str",
      "[Compound Units] unit_of((10 kg) * (9.8 mps2)) -> \"force\" (Newton)" },
    { "((1000 kg) * (9.8 mps2)) into kN", "9.8", "real",
      "[Compound Units] F = m * a converted via 'in kN' -> 9.8 [force]" },
    { "unit_of((98 N) / (10 kg))", "\"acceleration\"", "str",
      "[Compound Units] a = F / m: (98 N) / (10 kg) -> \"acceleration\" (m/s^2)" },
    { "unit_of((98 N) / (9.8 mps2))", "\"mass\"", "str",
      "[Compound Units] m = F / a: (98 N) / (9.8 mps2) -> \"mass\" (kg)" },
    { "d := 100 m ; t := 10 sec ; s := d / t ; s", "10", "real",
      "[Compound Units] s = d / t: (100 m) / (10 sec) -> 10 [speed] (m/s)" },
    { "unit_of((36 kph) * (10 sec))", "\"length\"", "str",
      "[Compound Units] d = v * t: (36 kph) * (10 sec) -> 100m [length]" },
    { "unit_of((100 m) / (20 mps))", "\"time\"", "str",
      "[Compound Units] t = d / v: (100 m) / (20 mps) -> 5s [time]" },
    { "unit_of((20 mps) / (4 sec))", "\"acceleration\"", "str",
      "[Compound Units] a = v / t: (20 mps) / (4 sec) -> 5 [acceleration]" },
    { "unit_of((100 m) / ((5 sec) ^ 2))", "\"acceleration\"", "str",
      "[Compound Units] a = d / t^2: (100 m) / ((5 sec)^2) -> 4 [acceleration]" },
    { "W_work := (50 N) * (4 m) ; W_work", "200", "real",
      "[Compound Units] Work W = F * d: (50 N) * (4 m) -> 200 J [energy]" },
    { "unit_of((50 N) * (4 m))", "\"energy\"", "str",
      "[Compound Units] unit_of((50 N) * (4 m)) -> \"energy\" (Joule)" },
    { "unit_of((200 J) / (10 sec))", "\"power\"", "str",
      "[Compound Units] P = W / t: (200 J) / (10 sec) -> 20 W [power]" },
    { "unit_of((50 N) * (4 mps))", "\"power\"", "str",
      "[Compound Units] P = F * v: (50 N) * (4 mps) -> 200 W [power]" },
    { "unit_of((100 N) / (2 m2))", "\"pressure\"", "str",
      "[Compound Units] P = F / A: (100 N) / (2 m2) -> 50 Pa [pressure]" },
    { "unit_of((5 m) ^ 2)", "\"area\"", "str",
      "[Compound Units] (5 m)^2 -> 25 m2 [area]" },
    { "unit_of(√(25 m2))", "\"length\"", "str",
      "[Compound Units] √(25 m2) -> 5 m [length]" },
    { "unit_of((2 m) ^ 3)", "\"volume\"", "str",
      "[Compound Units] (2 m)^3 -> 8 m3 [volume]" },
    { "unit_of(cbrt(27 m3))", "\"length\"", "str",
      "[Compound Units] cbrt(27 m3) -> 3 m [length]" },
    { "unit_of((2 km) / (500 m))", "\"none\"", "str",
      "[Compound Units] Same-unit division (2 km) / (500 m) cancels unit to \"none\"" },
    { "unit_of(1 / (0.02 sec))", "\"frequency\"", "str",
      "[Compound Units] Reciprocal time 1 / (0.02 sec) -> 50 Hz [frequency]" },
    { "unit_of((50 Hz) * (0.02 sec))", "\"none\"", "str",
      "[Compound Units] Frequency * time (50 Hz) * (0.02 sec) cancels to \"none\"" },
    { "unit_of(ke(2 kg, 10 mps))", "\"energy\"", "str",
      "[Compound Units] Physics ke(2 kg, 10 mps) = 0.5*m*v^2 auto-derives \"energy\" (100 J)" },
    { "unit_of(suvat_s(10 mps, 2 mps2, 5 sec))", "\"length\"", "str",
      "[Compound Units] Physics suvat_s(10 mps, 2 mps2, 5 sec) auto-derives \"length\" (75 m)" },
    { "unit_of(suvat_v2(0 mps, 2 mps2, 25 m))", "\"speed\"", "str",
      "[Compound Units] Physics suvat_v2(0 mps, 2 mps2, 25 m) auto-derives \"speed\" (10 m/s)" },
    { "unit_of(ohm_v(3 A, 4 ohm))", "\"voltage\"", "str",
      "[Compound Units] Ohm's law (3 A) * (4 ohm) -> 12 V [voltage]" },
    { "unit_of(elec_power(12 V, 3 A))", "\"power\"", "str",
      "[Compound Units] Electrical power (12 V) * (3 A) -> 36 W [power]" },

    /* 22. Switch Early `break` / `break(val)` & `()` vs `{}` Scoping Tests */
    { "switch (2) { case 1 : { 10 } case 2 : { x := 25 ; if (x > 20) { break(x * 2) } ; 999 } else : { 0 } }", "50", "real",
      "[switch break] Early parameterized 'break(x * 2)' exits switch case with 50" },
    { "switch (1) { case 1 : { a := 15 ; a += 5 ; break ; a = 999 } else : { 0 } }", "20", "real",
      "[switch break] Bare 'break' inside switch case exits early preserving prior expression (20)" },
    { "(a := 3 ; b := 4 ; a + b) * 10 + a", "73", "real",
      "[() vs {}] '(a := 3 ; b := 4 ; a + b)' runs multiple statements in current scope (a=3 remains visible -> 73)" },
    { "x := 1 ; { x := 99 ; y := 2 ; x + y } + x", "102", "real",
      "[() vs {}] '{ x := 99 ; y := 2 ; x + y }' opens lexical scope (101 + outer x=1 -> 102)" },

    /* 23. Feature 1: Stepped Ranges (`a...b by step`, `range(a, b, step)`) & Strided Slicing */
    { "1...10 by 2", "[1, 3, 5, 7, 9]", "vec",
      "[Feature 1] Inclusive stepped range '1...10 by 2' -> [1, 3, 5, 7, 9]" },
    { "1>..<10 by 2", "[3, 5, 7, 9]", "vec",
      "[Feature 1] Exclusive stepped range '1>..<10 by 2' -> [3, 5, 7, 9]" },
    { "0..<10 by 3", "[0, 3, 6, 9]", "vec",
      "[Feature 1] Right-open stepped range '0..<10 by 3' -> [0, 3, 6, 9]" },
    { "0>..10 by 3", "[3, 6, 9]", "vec",
      "[Feature 1] Left-open stepped range '0>..10 by 3' -> [3, 6, 9]" },
    { "10...2 by -3", "[10, 7, 4]", "vec",
      "[Feature 1] Descending stepped range '10...2 by -3' -> [10, 7, 4]" },
    { "0...1 by 0.25", "[0, 0.25, 0.5, 0.75, 1]", "vec",
      "[Feature 1] Fractional-step range '0...1 by 0.25' -> [0, 0.25, 0.5, 0.75, 1]" },
    { "((0 m)...(1 km) by (250 m))[2]", "500", "real",
      "[Feature 1] Unit-aware stepped range '(0 m)...(1 km) by (250 m)' -> 500 m at index 2" },
    { "unit_of(((0 m)...(1 km) by (250 m))[2])", "\"length\"", "str",
      "[Feature 1] Unit-aware stepped range preserves unit tag 'length'" },
    { "range(1, 9, 2)", "[1, 3, 5, 7, 9]", "vec",
      "[Feature 1] Function form 'range(1, 9, 2)' -> [1, 3, 5, 7, 9]" },
    { "range(1, 4)", "[1, 2, 3, 4]", "vec",
      "[Feature 1] Default step=1 'range(1, 4)' -> [1, 2, 3, 4]" },
    { "s := 0 ; (x in 1...100 by 2) { s += x } ; s", "2500", "real",
      "[Feature 1] O(1)-memory stepped range loop '(x in 1...100 by 2) { s += x }' -> 2500" },
    { "[x^2 for x in 1...9 by 2]", "[1, 9, 25, 49, 81]", "vec",
      "[Feature 1] O(1)-memory stepped range comprehension '[x^2 for x in 1...9 by 2]'" },
    { "(7 in 1...100 by 2) * 10 + (8 in 1...100 by 2)", "10", "real",
      "[Feature 1] O(1) stepped range membership: 7 in 1...100 by 2 (1), 8 in 1...100 by 2 (0)" },
    { "v := vec(10, 20, 30, 40, 50, 60) ; v[0...5 by 2]", "[10, 30, 50]", "vec",
      "[Feature 1] Strided vector slicing 'v[0...5 by 2]' -> [10, 30, 50]" },
    { "v := vec(10, 20, 30, 40, 50, 60) ; v[5...0 by -2]", "[60, 40, 20]", "vec",
      "[Feature 1] Reverse strided vector slicing 'v[5...0 by -2]' -> [60, 40, 20]" },

    /* 24. Feature 2 (Full 7D SI Base Units): K (Kelvin), mol (Mole), cd (Candela/Lumen) */
    { "25 degC", "298.15", "real",
      "[7D SI: K] '25 degC' normalizes to 298.15 K [temperature]" },
    { "unit_of(300 K)", "\"temperature\"", "str",
      "[7D SI: K] '300 K' has unit group 'temperature'" },
    { "(373.15 K) to_degC * 10 + ((212 degF) into K == 373.15 K)", "1001", "real",
      "[7D SI: K] '(373.15 K) to_degC' (100) and '(212 degF) into K == 373.15 K' (1)" },
    { "unit_of((1000 J) / (250 K))", "\"heat_capacity\"", "str",
      "[7D SI: K] Entropy / heat capacity (1000 J) / (250 K) -> 4 J/K [heat_capacity]" },
    { "unit_of((4 JpK) * (250 K))", "\"energy\"", "str",
      "[7D SI: K] (4 JpK) * (250 K) -> 1000 J [energy]" },
    { "unit_of(((4184 J) / (1 kg)) / (1 K))", "\"specific_heat\"", "str",
      "[7D SI: K] ((4184 J) / (1 kg)) / (1 K) -> 4184 J/(kg*K) [specific_heat]" },
    { "Q := (2 kg) * (4184 JpkgK) * (10 K) ; Q", "83680", "real",
      "[7D SI: K] Calorimetry Q = m * c * dT: (2 kg) * (4184 JpkgK) * (10 K) -> 83680 J" },
    { "unit_of((2 kg) * (4184 JpkgK) * (10 K))", "\"energy\"", "str",
      "[7D SI: K] Calorimetry (kg * J/(kg*K) * K) cancels kg and K -> 'energy' (J)" },
    { "2 mol + 500 mmol", "2.5", "real",
      "[7D SI: mol] '2 mol + 500 mmol' -> 2.5 [substance]" },
    { "unit_of((36 gram) / (2 mol))", "\"molar_mass\"", "str",
      "[7D SI: mol] Mass / substance: (36 gram) / (2 mol) -> 0.018 kg/mol [molar_mass]" },
    { "((36 gram) / (18 gpmol))", "2", "real",
      "[7D SI: mol] Stoichiometry n = m / M: (36 gram) / (18 gpmol) -> 2 mol [substance]" },
    { "unit_of((36 gram) / (18 gpmol))", "\"substance\"", "str",
      "[7D SI: mol] unit_of((36 gram) / (18 gpmol)) -> 'substance' (mol)" },
    { "unit_of((2 mol) * (18 gpmol))", "\"mass\"", "str",
      "[7D SI: mol] m = n * M: (2 mol) * (18 gpmol) -> 0.036 kg [mass]" },
    { "(((0.5 mol) / (250 mL)) into M_conc)", "2", "real",
      "[7D SI: mol] Molarity c = n / V: ((0.5 mol) / (250 mL)) in M_conc -> 2 M [molarity]" },
    { "unit_of((0.5 mol) / (250 mL))", "\"molarity\"", "str",
      "[7D SI: mol] unit_of((0.5 mol) / (250 mL)) -> 'molarity' (mol/m^3)" },
    { "unit_of((2 M_conc) * (250 mL))", "\"substance\"", "str",
      "[7D SI: mol] n = c * V: (2 M_conc) * (250 mL) -> 0.5 mol [substance]" },
    { "unit_of((50 kJ) / (2 mol))", "\"molar_energy\"", "str",
      "[7D SI: mol] Molar enthalpy (50 kJ) / (2 mol) -> 25000 J/mol [molar_energy]" },
    { "unit_of(((8.314462618 J) / (1 mol)) / (1 K))", "\"molar_heat_capacity\"", "str",
      "[7D SI: mol & K] Gas constant R = J / (mol * K) -> 'molar_heat_capacity'" },
    { "unit_of(((2 mol) * (8.314462618 JpmolK) * (300 K)) / (50 L))", "\"pressure\"", "str",
      "[7D SI: Ideal Gas Law] P = n*R*T / V: (mol * J/(mol*K) * K) / L -> 'pressure' (Pa)" },
    { "unit_of((0.5 mol) / (10 sec))", "\"catalytic_activity\"", "str",
      "[7D SI: mol] Enzyme activity (0.5 mol) / (10 sec) -> 0.05 kat [catalytic_activity]" },
    { "(800 lm) / (4 m2)", "200", "real",
      "[7D SI: cd] Photometry E_v = Phi_v / A: (800 lm) / (4 m2) -> 200 lx [illuminance]" },
    { "unit_of((800 lm) / (4 m2))", "\"illuminance\"", "str",
      "[7D SI: cd] unit_of((800 lm) / (4 m2)) -> 'illuminance' (lux = lm/m^2)" },
    { "unit_of((200 lx) * (4 m2))", "\"luminous_intensity\"", "str",
      "[7D SI: cd] (200 lx) * (4 m2) -> 800 lm [luminous_intensity]" },

    /* 25. Feature 3: Indexed `i, x in v` Loops/Comprehensions & Pipe Operator `|>` */
    { "v := vec(10, 20, 30) ; s := 0 ; (i, x in v) { s += i * 100 + x } ; s", "360", "real",
      "[Feature 3] Parenthesized 2-var indexed loop '(i, x in v) { s += i * 100 + x }' -> 360" },
    { "v := vec(10, 20, 30) ; s := 0 ; i, x in v { s += (i + 1) * x } ; s", "140", "real",
      "[Feature 3] Unparenthesized 2-var indexed loop 'i, x in v { s += (i + 1) * x }' -> 140" },
    { "s := 0 ; (idx, val in 10...30 by 10) { s += idx * 100 + val } ; s", "360", "real",
      "[Feature 3] 2-var indexed loop over stepped range '(idx, val in 10...30 by 10)' -> 360" },
    { "[i * 100 + x for i, x in vec(5, 6, 7)]", "[5, 106, 207]", "vec",
      "[Feature 3] 2-var indexed comprehension '[i * 100 + x for i, x in vec(5, 6, 7)]'" },
    { "[i * 10 + x for i, x in vec(3, 4, 5, 6) if i % 2 == 1]", "[14, 36]", "vec",
      "[Feature 3] Filtered 2-var indexed comprehension '[i * 10 + x for i, x in ... if i % 2 == 1]'" },
    { "5 |> (x => x^2 + 1) |> (x => x * 2)", "52", "real",
      "[Feature 3] Pipe operator '|>' through lambda closures: 5 -> 26 -> 52" },
    { "vec(1, 2, 3, 4, 5) |> map(x => x^2) |> filter(x => x > 5) |> sum", "50", "real",
      "[Feature 3] Pipe operator '|>' collection pipeline: map -> filter -> sum -> 50" },
    { "(16 |> sqrt |> round) * 100 + (12 |> gcd(18)) * 10 + (2 |> clamp(5, 10))", "465", "real",
      "[Feature 3] Pipe operator '|>' into 1-arg ('sqrt'), 2-arg ('gcd(18)'), and 3-arg ('clamp(5,10)') functions" },

    /* 26. Feature 4: Scientific Notation, Digit Separators & First-Class String Operations */
    { "1_000_000 + 2_500.75", "1002500.75", "real",
      "[Feature 4] Numeric digit separators '1_000_000 + 2_500.75' -> 1002500.75" },
    { "1.5e3 + 2.5e-1", "1500.25", "real",
      "[Feature 4] Scientific exponent notation '1.5e3 + 2.5e-1' -> 1500.25" },
    { "(6.022e23 * 1e-23) ~ 6.022 && (2e ~ (2 * e)) && (42_ == \"42\")", "1", "real",
      "[Feature 4] Scientific notation '6.022e23 * 1e-23' coexists with '2e' (2*e) and '42_' (\"42\")" },
    { "\"Hello, \" + \"MEP!\"", "\"Hello, MEP!\"", "str",
      "[Feature 4] String concatenation '\"Hello, \" + \"MEP!\"' -> \"Hello, MEP!\"" },
    { "len(\"Planck\") * 10 + (\"Planck\"[0] == \"P\") + (\"Planck\"[-1] == \"k\")", "62", "real",
      "[Feature 4] String length 'len(\"Planck\")' (6) and character indexing s[0], s[-1]" },
    { "\"abcdef\"[1...4] + \"_\" + \"abcdef\"[0...5 by 2] + \"_\" + \"stressed\"[7...0 by -1]", "\"bcde_ace_desserts\"", "str",
      "[Feature 4] Substring range slicing, strided slicing, and string reversal" },
    { "(\"MEP\" in \"Hello MEP\") * 100 + [i for i, c in \"abacab\" if c == \"a\"].sum()", "106", "real",
      "[Feature 4] Substring 'in' check (100) + indexed character comprehension over string (0+2+4 = 6)" },

    /* 27. Feature 5: `linsolve(A, b)`, Elementwise Hadamard `.*` `./` `.^` & `ode_rk4` */
    { "linsolve([[2, 1, -1], [-3, -1, 2], [-2, 1, 2]], [8, -11, -3])", "[2, 3, -1]", "mat",
      "[Feature 5] 3x3 Gaussian elimination 'linsolve(A, b)' -> [2, 3, -1]" },
    { "linsolve([2, 3 ; 5, -1], list(8, 3))", "[1, 2]", "vec",
      "[Feature 5] 'linsolve(A, b)' with $vec right-hand side -> vec [1, 2]" },
    { "[1, 2 ; 3, 4] .* [10, 20 ; 30, 40]", "[[10, 40], [90, 160]]", "mat",
      "[Feature 5] Elementwise matrix Hadamard product '[1, 2 ; 3, 4] .* [10, 20 ; 30, 40]'" },
    { "([10, 40 ; 90, 160] ./ [1, 2 ; 3, 4]) + ([1, 2 ; 3, 4] .^ 2)", "[[11, 24], [39, 56]]", "mat",
      "[Feature 5] Elementwise matrix Hadamard division './' and power '.^'" },
    { "(vec(2, 3, 4) .* vec(5, 6, 7)) + (vec(2, 3, 4) .^ 2)", "[14, 27, 44]", "vec",
      "[Feature 5] Elementwise vector Hadamard product '.*' and power '.^'" },
    { "ode_rk4(t, y, y, 0, 1, 1) ~ e", "1", "real",
      "[Feature 5] 4th-order Runge-Kutta 'ode_rk4(t, y, y, 0, 1, 1)' solves dy/dt = y -> e" },
    { "ode_rk4(t, y, 2 * t, 0, 0, 3, 100) ~ 9", "1", "real",
      "[Feature 5] 4th-order Runge-Kutta 'ode_rk4(t, y, 2 * t, 0, 0, 3, 100)' -> 9" },
    { "ode_rk4(t, y, t + y, 0, 1, 1, 200) ~ (2 * e - 2)", "1", "real",
      "[Feature 5] Coupled ODE 'ode_rk4(t, y, t + y, 0, 1, 1, 200)' -> 2e - 2" },
    /* 28. Pure MEP String Module (`Library/string.math` — `[string]`) Tests */
    { "str_repeat(\"ab\", 3) + \"_\" + str_reverse(\"reward\") + \"_\" + substr(\"scientist\", 0, 3)",
      "\"ababab_drawer_scie\"", "str",
      "[string.math] Pure MEP string helpers str_repeat, str_reverse, and substr" },
    { "starts_with(\"MathEngine\", \"Math\") * 100 + ends_with(\"MathEngine\", \"Engine\") * 10 + is_empty(\"\")",
      "111", "real",
      "[string.math] Pure MEP predicates starts_with, ends_with, and is_empty" },
    { "str_join(chars(\"M-E-P\").filter(c => c != \"-\"), \".\")",
      "\"M.E.P\"", "str",
      "[string.math] Pure MEP chars(s) -> vec<str> + filter + str_join(v, sep)" },
    /* 29. Edge-Case Hardening & Complete Overload Verification Tests */
    { "substr(\"\", 0, 0) + substr(\"hello\", 3, 1) + substr(\"hello\", -3, -1)",
      "\"llo\"", "str",
      "[Edge Case] substr empty string, inverted start>stop bounds, and negative indices" },
    { "str_join(vec(\"a\", \"b\"), \",\") + \":\" + str_join(vec(\"x\"), \",\")",
      "\"a,b:x\"", "str",
      "[Edge Case] 1-arg and 2-arg non-real vec(\"x\"), vec(\"a\", \"b\") element constructors" },
    { "v := (vec(10m, 20m) + vec(5m, 15m)) ./ vec(3sec, 7sec) ; unit_of(v[0]) + \":\" + (v[0] + v[1])_",
      "\"speed:10\"", "str",
      "[Edge Case] 2-arg unit-tagged vec(10m, 20m) + Hadamard ./ preserving SI speed unit" },
    { "(-(3 + 4i) + 2 * (3 + 4i) - (2 + 4i) / 2) + (5 - (1 + 2i)) + (3i + (1 + 2i)) - (3i - (1 + 2i))",
      "8 + 4i", "complex",
      "[Edge Case] Complex unary minus, scalar * and /, and real/iota +/- complex overloads" },
    { "(-vec2(3, -4) + vec2(6, 8) / 2 == vec2(0, 8)) && (-vec3(1, -2, 3) + vec3(3, 6, 9) / 3 == vec3(0, 4, 0)) && (-mat2(1, 2, 3, 4) + mat2(2, 4, 6, 8) / 2 == mat2(0, 0, 0, 0))",
      "1", "real",
      "[Edge Case] Unary minus, scalar division /, and == on vec2, vec3, and mat2" },
    { "((6i / 2) == 3i) && ((6i / 2i) == 3) && ((6 / 2i) == -3i) && (5.0005i ~ 5i) && (mat2(1, 2, 3, 4.0005) ~ mat2(1, 2, 3, 4))",
      "1", "real",
      "[Edge Case] Pure imaginary iota division (/), equality (==, !=, ~), and mat2 ~ mat2" },
    { "2 .^ [1, 2 ; 3, 4]",
      "[[2, 4], [8, 16]]", "mat",
      "[Edge Case] Scalar-base elementwise matrix power 's .^ A' -> [[2, 4], [8, 16]]" },

    /* 30. Suggestion 1: Inline Named Functions `f(x, y) := expr`, Recursive Closures, UFCS on `$fn` & Multi-Arg `call` */
    { "sq(x) := x^2 + 1 ; sq(5) + sq(3)",
      "36", "real",
      "[Sugg 1] Inline named 1-arg function 'sq(x) := x^2 + 1' -> 26 + 10 = 36" },
    { "add3(a, b, c) := a * 100 + b * 10 + c ; add3(2, 4, 6)",
      "246", "real",
      "[Sugg 1] Inline named 3-arg function 'add3(a, b, c) := ...' -> 246" },
    { "clamp4(x, lo, hi, scale) := min(max(x, lo), hi) * scale ; clamp4(15, 0, 10, 3) + call(clamp4, -5, 2, 8, 10)",
      "50", "real",
      "[Sugg 1] Inline named 4-arg function 'clamp4' + 5-arg 'call(f, a, b, c, d)' -> 30 + 20 = 50" },
    { "k := 10 ; scale_add(x, y) := (x + y) * k ; 5.scale_add(3) + (7).scale_add(2)",
      "170", "real",
      "[Sugg 1] Lexical capture + UFCS dot-call on local closure '5.scale_add(3)' -> 80 + 90 = 170" },
    { "fact(n) := n <= 1 ? 1 : n * fact(n - 1) ; fact(6)",
      "720", "real",
      "[Sugg 1] Recursive inline named function 'fact(n) := n <= 1 ? 1 : n * fact(n - 1)' -> 720" },
    { "f_rec := n => n <= 1 ? 1 : n * f_rec(n - 1) ; f_rec(5)",
      "120", "real",
      "[Sugg 1] Recursive anonymous lambda 'f_rec := n => ...' self-binding -> 120" },
    { "fib_r(n) := { if (n <= 1) { return n } ; a := fib_r(n - 1) ; b := fib_r(n - 2) ; a + b } ; fib_r(8)",
      "21", "real",
      "[Sugg 1] Re-entrant block recursion 'fib_r(8)' preserving body-local slots 'a' & 'b' -> 21" },
    { "const_fn() := 42 ; const_fn() + call(const_fn)",
      "84", "real",
      "[Sugg 1] 0-arg inline function 'const_fn() := 42' + 1-arg 'call(const_fn)' -> 84" },

    /* 31. Suggestion 2: Complex Exponentiation, Roots, Transcendentals & Mixed Scalar/Iota/Complex Ops */
    { "re(3 + 4i) * 10 + im(3 + 4i)",
      "34", "real",
      "[Sugg 2] Complex accessors 're(3 + 4i)' (3) and 'im(3 + 4i)' (4) -> 34" },
    { "((1 + 1i) ^ 4) ~ (-4 + 0i)",
      "1", "real",
      "[Sugg 2] Complex-real power '(1 + 1i) ^ 4' -> -4 + 0i" },
    { "((1i) ^ (1i)) ~ complex(exp(-π / 2), 0)",
      "1", "real",
      "[Sugg 2] Euler identity i^i = e^(-pi/2) via complex-complex power '(1i) ^ (1i)'" },
    { "(2 ^ (1i)) ~ cexp_i(ln(2))",
      "1", "real",
      "[Sugg 2] Real-to-complex power '2 ^ (1i)' == cos(ln 2) + i sin(ln 2)" },
    { "(√(3 + 4i) == 2 + 1i) && (sqrt(-4 + 0i) ~ (0 + 2i))",
      "1", "real",
      "[Sugg 2] Principal complex square root '√(3 + 4i) == 2 + 1i' and 'sqrt(-4 + 0i) == 2i'" },
    { "exp(0 + π * 1i) ~ (-1 + 0i) && ln(exp(1 + 2i)) ~ (1 + 2i)",
      "1", "real",
      "[Sugg 2] Complex 'exp(i*pi) == -1' and principal branch 'ln(exp(1 + 2i)) == 1 + 2i'" },
    { "(sin(1 + 2i)^2 + cos(1 + 2i)^2) ~ (1 + 0i)",
      "1", "real",
      "[Sugg 2] Complex Pythagorean identity sin(z)^2 + cos(z)^2 == 1" },
    { "(cosh(1 + 2i)^2 - sinh(1 + 2i)^2) ~ (1 + 0i) && (tan(1 + 1i) ~ (sin(1 + 1i) / cos(1 + 1i))) && (tanh(1 + 1i) ~ (sinh(1 + 1i) / cosh(1 + 1i)))",
      "1", "real",
      "[Sugg 2] Complex hyperbolic identity cosh(z)^2 - sinh(z)^2 == 1 and tan(z), tanh(z)" },
    { "(10 / (1 + 2i) == 2 - 4i) && (2i * (3 + 4i) == -8 + 6i) && ((3 + 4i) * 2i == -8 + 6i) && ((4 + 2i) / 2i == 1 - 2i) && (10i / (1 + 2i) == 4 + 2i)",
      "1", "real",
      "[Sugg 2] Mixed real/iota/complex multiplication and division overloads" },

    /* 32. Suggestion 3: Vector & Matrix Sorting, Reshaping, Norms & Matrix Powers */
    { "vec(5, 1, 4, 2, 3).sort()",
      "[1, 2, 3, 4, 5]", "vec",
      "[Sugg 3] Ascending vector sort 'vec(5, 1, 4, 2, 3).sort()' -> [1, 2, 3, 4, 5]" },
    { "vec(5, 1, 4, 2, 3).sort_desc()",
      "[5, 4, 3, 2, 1]", "vec",
      "[Sugg 3] Descending vector sort 'vec(5, 1, 4, 2, 3).sort_desc()' -> [5, 4, 3, 2, 1]" },
    { "list(\"delta\", \"alpha\", \"charlie\", \"bravo\").sort()",
      "[\"alpha\", \"bravo\", \"charlie\", \"delta\"]", "vec",
      "[Sugg 3] Lexicographical string vector sort" },
    { "v := vec(30m, 10m, 20m).sort() ; unit_of(v[0]) + \":\" + (v[0] to_m)_ + \",\" + (v[2] to_m)_",
      "\"length:10,30\"", "str",
      "[Sugg 3] Unit-preserving sort on 'vec<real, length>'" },
    { "v := vec(10, 50, 20, 40) ; v.min() * 1000 + v.max() * 10 + v.argmin() * 4 + v.argmax()",
      "10501", "real",
      "[Sugg 3] Vector reductions min(v)=10, max(v)=50, argmin(v)=0, argmax(v)=1" },
    { "vec(1, 2, 3, 4).cumsum() + vec(1, 2, 3, 4).cumprod()",
      "[2, 5, 12, 34]", "vec",
      "[Sugg 3] Prefix sums 'cumsum' ([1,3,6,10]) + prefix products 'cumprod' ([1,2,6,24])" },
    { "vec(10, 13, 19, 28).diff_vec() + vec(1, 2, 3).reverse()",
      "[6, 8, 10]", "vec",
      "[Sugg 3] First differences 'diff_vec' ([3, 6, 9]) + 'reverse' ([3, 2, 1])" },
    { "dot(vec(1, 2, 3), vec(4, 5, 6)) * 100 + norm(list(3, 4))",
      "3205", "real",
      "[Sugg 3] General N-D vector dot product (32) and Euclidean L2 norm (5)" },
    { "linspace(0, 1, 5)",
      "[0, 0.25, 0.5, 0.75, 1]", "vec",
      "[Sugg 3] Evenly spaced grid 'linspace(0, 1, 5)' -> [0, 0.25, 0.5, 0.75, 1]" },
    { "vec(3, 1, 2, 3, 1, 4, 2).unique()",
      "[3, 1, 2, 4]", "vec",
      "[Sugg 3] Order-preserving deduplication 'unique(v)' -> [3, 1, 2, 4]" },
    { "zip_with(vec(1, 2, 3), vec(10, 20, 30), (a, b) => a * b)",
      "[10, 40, 90]", "vec",
      "[Sugg 3] Higher-order 'zip_with(a, b, f)' -> [10, 40, 90]" },
    { "(1...6).reshape(2, 3)",
      "[[1, 2, 3], [4, 5, 6]]", "mat",
      "[Sugg 3] Reshape 1D vector '(1...6).reshape(2, 3)' -> 2x3 matrix" },
    { "[1, 2, 3 ; 4, 5, 6].reshape(3, 2).flatten()",
      "[1, 2, 3, 4, 5, 6]", "vec",
      "[Sugg 3] Matrix 'reshape(3, 2)' followed by 'flatten()' -> 1D vector" },
    { "diag(vec(2, 5, 9)).diag()",
      "[2, 5, 9]", "vec",
      "[Sugg 3] 'diag(vec)' constructs diagonal matrix and 'diag(mat)' extracts diagonal vector" },
    { "M := [1, 2 ; 3, 4] ; M.mat_sum() * 1000 + M.mat_mean() * 100 + M.mat_min() * 10 + M.mat_max()",
      "10264", "real",
      "[Sugg 3] Matrix reductions mat_sum (10), mat_mean (2.5), mat_min (1), mat_max (4)" },
    { "frobenius_norm([3, 0 ; 0, 4])",
      "5", "real",
      "[Sugg 3] Matrix Frobenius norm 'frobenius_norm([3, 0 ; 0, 4])' -> 5" },
    { "([1, 1 ; 1, 0] ^ 6)[0, 0] * 100 + ([2, 5 ; 1, 3] ^ 0)[0, 0] * 10 + (([2, 1 ; 5, 3] ^ -1)[0, 0])",
      "1313", "real",
      "[Sugg 3] Matrix exponentiation 'M ^ n': Fibonacci [1,1;1,0]^6->13, M^0->I (1), M^-1->inv(M) (3)" },

    /* 33. Suggestion 4: Special Functions, Probability Distributions & Linear Regression */
    { "(erf(0) == 0) && (erfc(0) == 1) && (erf(1) + erfc(1) ~ 1) && (gamma(5) ~ 24) && (lgamma(5) ~ ln(24)) && (beta_fn(2, 3) ~ (1 / 12))",
      "1", "real",
      "[Sugg 4] Special functions erf, erfc, gamma, lgamma, and beta_fn" },
    { "(norm_cdf(0) ~ 0.5) && (norm_cdf(1.959964) ~ 0.975) && (norm_pdf(0) ~ (1 / √(2π)))",
      "1", "real",
      "[Sugg 4] Gaussian normal distribution 'norm_pdf' and 'norm_cdf'" },
    { "(binom_pmf(10, 5, 0.5) ~ 0.24609375) && (poisson_pmf(3, 3) ~ (27 * exp(-3) / 6)) && (geom_pmf(3, 0.5) == 0.125) && (exp_pdf(0, 2) == 2) && (exp_cdf(ln(2), 1) ~ 0.5)",
      "1", "real",
      "[Sugg 4] Discrete & continuous distributions binom_pmf, poisson_pmf, geom_pmf, exp_pdf, exp_cdf" },
    { "vec(10, 2, 8, 4, 6).median() * 10 + vec(1, 3, 5, 7).median()",
      "64", "real",
      "[Sugg 4] Odd-length median (6) and even-length median (4) -> 64" },
    { "v := vec(2, 4, 4, 4, 5, 5, 7, 9) ; (sample_variance(v) ~ 4.571428571) && (sample_stddev(v) ~ √4.571428571)",
      "1", "real",
      "[Sugg 4] Unbiased Bessel-corrected sample_variance and sample_stddev" },
    { "xs := vec(1, 2, 3, 4, 5) ; ys := vec(5, 8, 11, 14, 17) ; linreg_slope(xs, ys) * 100 + linreg_intercept(xs, ys) * 10 + linreg_r2(xs, ys) + corr(xs, ys)",
      "322", "real",
      "[Sugg 4] OLS linear regression y = 3x + 2: slope=3, intercept=2, R^2=1, corr=1 -> 322" },

    /* 34. Suggestion 5: Multivariate Calculus, Gradients, Hessians, `ode_rk4_vec` & Unit-Aware Calculus */
    { "f := (x, y) => x^2 * y + 3 * y^2 ; grad2(f, 2, 3)",
      "vec2(x=12, y=22)", "vec2",
      "[Sugg 5] 2D numerical gradient 'grad2((x, y) => x^2*y + 3*y^2, 2, 3)' -> vec2(12, 22)" },
    { "f3 := (x, y) => x^2 + y^2 ; laplacian2(f3, 1, 2) ~ 4",
      "1", "real",
      "[Sugg 5] 2D Laplacian del^2(x^2 + y^2) = 4" },
    { "H := hessian2((x, y) => x^3 + 2 * x * y + y^2, 2, 3) ; (H.a ~ 12) && (H.b ~ 2) && (H.c ~ 2) && (H.d ~ 2)",
      "1", "real",
      "[Sugg 5] 2D Hessian matrix 'hessian2(f, 2, 3)' -> mat2(12, 2, 2, 2)" },
    { "dir_deriv2((x, y) => x^2 + y^2, 3, 4, vec2(3, 4)) ~ 10",
      "1", "real",
      "[Sugg 5] Directional derivative of x^2 + y^2 at (3, 4) along (3, 4) -> 10" },
    { "(grad3(v => v.x^2 + 2 * v.y^2 + 3 * v.z^2, vec3(1, 2, 3)) ~ vec3(2, 8, 18)) && (grad3((x, y, z) => x^2 + 2 * y^2 + 3 * z^2, 1, 2, 3) ~ vec3(2, 8, 18))",
      "1", "real",
      "[Sugg 5] 3D gradient 'grad3(f, vec3)' and 'grad3(f, x, y, z)' -> vec3(2, 8, 18)" },
    { "grad_descent_1d(x => (x - 4)^2 + 1, 0, 0.2, 60) ~ 4",
      "1", "real",
      "[Sugg 5] 1D gradient descent optimizer 'grad_descent_1d' converges to x = 4" },
    { "grad_descent_2d((x, y) => (x - 3)^2 + (y + 2)^2, vec2(0, 0), 0.2, 60) ~ vec2(3, -2)",
      "1", "real",
      "[Sugg 5] 2D gradient descent optimizer 'grad_descent_2d' converges to vec2(3, -2)" },
    { "tr := ode_rk4_vec(t, y, y, 0, 1, 1, 10) ; (tr.len() == 11) && (tr[0] == 1) && (tr[10] ~ e)",
      "1", "real",
      "[Sugg 5] Trajectory ODE solver 'ode_rk4_vec(t, y, y, 0, 1, 1, 10)' returns 11-point vector" },
    { "v_inst := diff(t, 0.5 * (4 mps2) * (t ^ 2), 3 sec) ; unit_of(v_inst) + \":\" + (v_inst to_mps)_",
      "\"speed:12\"", "str",
      "[Sugg 5] Unit-aware 'diff': d/dt(0.5 * a * t^2) propagates m / s -> 12 [speed]" },
    { "a_inst := diff2(t, 0.5 * (4 mps2) * (t ^ 2), 3 sec) ; unit_of(a_inst) + \":\" + (a_inst to_mps2)_",
      "\"acceleration:4\"", "str",
      "[Sugg 5] Unit-aware 'diff2': d^2/dt^2(0.5 * a * t^2) propagates m / s^2 -> 4 [acceleration]" },
    { "d_tot := integrate(t, (6 mps2) * t, 0 sec, 4 sec) ; unit_of(d_tot) + \":\" + (d_tot to_m)_",
      "\"length:48\"", "str",
      "[Sugg 5] Unit-aware 'integrate': int (a * t) dt propagates speed * time -> 48 [length]" },
    { "d_ode := ode_rk4(t, y, 10 mps, 0 sec, 5 m, 4 sec) ; unit_of(d_ode) + \":\" + (d_ode to_m)_",
      "\"length:45\"", "str",
      "[Sugg 5] Unit-aware 'ode_rk4': dy/dt = 10 m/s over 4 s from 5 m -> 45 [length]" },

    /* 35. Suggestion 6: Hex (`0x...`) & Binary (`0b...`) Literals, Bitwise Ops & Base Formatters */
    { "0xFF + 0b1010 + 0x10_00",
      "4361", "real",
      "[Sugg 6] Hexadecimal '0xFF' (255), binary '0b1010' (10), and '0x10_00' (4096) -> 4361" },
    { "bit_and(0b1100, 0b1010) * 100 + bit_or(0b1100, 0b1010) * 10 + bit_xor(0b1100, 0b1010)",
      "946", "real",
      "[Sugg 6] Bitwise AND (8*100), OR (14*10), and XOR (6) -> 946" },
    { "((1 << 8) == 256) && ((256 >> 3) == 32) && ((~0) == -1) && (bit_not(0) == -1)",
      "1", "real",
      "[Sugg 6] Bitwise shift operators '<<', '>>' and prefix '~' / 'bit_not'" },
    { "popcount(0b1011_0111) * 100 + bit_len(255)",
      "608", "real",
      "[Sugg 6] Hamming weight 'popcount(0b1011_0111)' (6) and 'bit_len(255)' (8) -> 608" },
    { "to_hex(255) + \"_\" + to_bin(10) + \"_\" + (_\"0x1A\" + _\"0b110\")_",
      "\"0xFF_0b1010_32\"", "str",
      "[Sugg 6] Base formatters 'to_hex(255)', 'to_bin(10)', and prefix '_' on hex/binary strings" },

    /* 36. Expansion 1: Arbitrary-Arity & Default-Param Lambdas, `f(x) = expr` Session Functions & Mutual Recursion */
    { "sum6(a, b, c, d, e, g) := a + b + c + d + e + g ; sum6(1, 2, 3, 4, 5, 6)",
      "21", "real",
      "[Exp 1] 6-parameter inline function 'sum6(a, b, c, d, e, g) := ...' -> 21" },
    { "fn8 := (a, b, c, d, e, g, h, k) => a*10 + b + c + d + e + g + h + k ; fn8(2, 1, 1, 1, 1, 1, 1, 3)",
      "29", "real",
      "[Exp 1] 8-parameter anonymous lambda closure 'fn8(2, 1, 1, 1, 1, 1, 1, 3)' -> 29" },
    { "f(x, y = 10, z = 100) := x + y + z ; f(5) * 1000 + f(5, 20) + f(5, 20, 30)",
      "115180", "real",
      "[Exp 1] Inline function with optional default parameters 'f(x, y = 10, z = 100)' -> 115180" },
    { "scale_def := (x, k = 3, off = 1) => x * k + off ; scale_def(4) * 100 + scale_def(4, 5) * 10 + scale_def(4, 5, 2)",
      "1532", "real",
      "[Exp 1] Anonymous lambda with default parameters '(x, k = 3, off = 1) => ...' -> 1300 + 210 + 22 = 1532" },
    { "quad_eq(a, b = 2, c = 1) = a^2 + b * a + c ; quad_eq(3) * 100 + quad_eq(3, 4, 5)",
      "1626", "real",
      "[Exp 1] Session-style '=' function definition 'quad_eq(a, b = 2, c = 1) = ...' -> 1600 + 26 = 1626" },
    /* Mutual (forward-referenced) recursion moved to the module-based
     * `[Exp 1b]` check below: persistent function registries live in
     * module scope under the Module Execution API. */
    { "is_even_s(n) := n <= 1 ? 1 : n * is_even_s(n - 2) ; is_even_s(6) * 10 + is_even_s(5)",
      "495", "real",
      "[Exp 1] Self-referencing recursive inline function 'is_even_s' -> 480 + 15" },
    { "ack(m, n) = m == 0 ? n + 1 : (n == 0 ? ack(m - 1, 1) : ack(m - 1, ack(m, n - 1))) ; ack(2, 3)",
      "9", "real",
      "[Exp 1] Deeply recursive 2-argument Ackermann function 'ack(2, 3)' -> 9" },

    /* 37. Expansion 2: Linear Algebra Decompositions (LU, QR, Cholesky, Eigen, SVD, Pinv, Lstsq) & Coupled ODEs */
    { "M := [1, 2 ; 3, 4] ; v := list(5, 6) ; (M * v) + (v * M)",
      "[40, 73]", "vec",
      "[Exp 2] Matrix-vector 'M * v' ([17, 39]) and vector-matrix 'v * M' ([23, 34]) -> [40, 73]" },
    { "outer(list(1, 2), vec(3, 4, 5))",
      "[[3, 4, 5], [6, 8, 10]]", "mat",
      "[Exp 2] Vector outer product 'outer(list(1, 2), vec(3, 4, 5))' -> 2x3 matrix" },
    { "vstack([1, 2], [3, 4]) + hstack([1 ; 3], [2 ; 4])",
      "[[2, 4], [6, 8]]", "mat",
      "[Exp 2] Matrix vertical 'vstack' and horizontal 'hstack' concatenation" },
    { "kron(mat_eye(2), [1, 2 ; 3, 4])[2, 3] * 10 + tril([1, 2 ; 3, 4])[0, 1] + triu([1, 2 ; 3, 4])[1, 0]",
      "20", "real",
      "[Exp 2] Kronecker product 'kron(I_2, A)', 'tril(A)', and 'triu(A)'" },
    { "A := [2, 1, 1 ; 4, -6, 0 ; -2, 7, 2] ; (lu_p(A) * A) ~ (lu_l(A) * lu_u(A))",
      "1", "real",
      "[Exp 2] PA = LU decomposition identity '(lu_p(A) * A) ~ (lu_l(A) * lu_u(A))'" },
    { "A := [12, -51, 4 ; 6, 167, -68 ; -4, 24, -41] ; Q := qr_q(A) ; R := qr_r(A) ; ((Q * R) ~ A) && ((transpose(Q) * Q) ~ mat_eye(3))",
      "1", "real",
      "[Exp 2] Householder QR decomposition: Q*R ~ A and Q^T*Q ~ I" },
    { "A := [4, 12, -16 ; 12, 37, -43 ; -16, -43, 98] ; L := chol(A) ; (L * transpose(L)) == A",
      "1", "real",
      "[Exp 2] Cholesky decomposition L * L^T == A on symmetric positive-definite matrix" },
    { "A := [2, 1 ; 1, 2] ; ev := eigvals(A) ; V := eigvecs(A) ; (ev ~ list(3, 1)) && ((A * V) ~ (V * diag(ev)))",
      "1", "real",
      "[Exp 2] Symmetric eigenvalue & eigenvector decomposition A*V ~ V*diag(eigvals)" },
    { "A := [3, 0 ; 0, 4] ; (svd_s(A) ~ list(4, 3)) && ((svd_u(A) * diag(svd_s(A)) * transpose(svd_v(A))) ~ A)",
      "1", "real",
      "[Exp 2] Singular Value Decomposition U * diag(S) * V^T ~ A" },
    { "A := [1, 2 ; 3, 4 ; 5, 6] ; P := pinv(A) ; (A * P * A) ~ A",
      "1", "real",
      "[Exp 2] Moore-Penrose pseudoinverse identity A * pinv(A) * A ~ A on 3x2 matrix" },
    { "A := [1, 1 ; 1, 2 ; 1, 3] ; b := vec(6, 5, 7) ; lstsq(A, b)",
      "[5, 0.5]", "vec",
      "[Exp 2] Overdetermined linear least squares 'lstsq(A, b)' -> [5, 0.5]" },
    { "M := [1, 2 ; 2, 4] ; matrix_rank(M) * 100 + mat_norm1([1, -2 ; 3, 4]) * 10 + mat_norm_inf([1, -2 ; 3, 4])",
      "167", "real",
      "[Exp 2] Numerical SVD rank 'matrix_rank' (1), 'mat_norm1' (6), and 'mat_norm_inf' (7) -> 167" },
    { "power_iter([2, 1 ; 1, 2], 40) ~ 3 && cond([3, 0 ; 0, 1]) ~ 3 && spectral_norm([3, 0 ; 0, 4]) ~ 4",
      "1", "real",
      "[Exp 2] Dominant eigenvalue 'power_iter', condition number 'cond', and 'spectral_norm'" },
    { "y_end := ode_coupled2(t, x, y, y, -x, 0, 1, 0, π, 200) ; (y_end[0] ~ -1) && (y_end[1] ~ 0)",
      "1", "real",
      "[Exp 2] 2D coupled harmonic oscillator 'ode_coupled2(t, x, y, y, -x, 0, 1, 0, pi)' -> [-1, 0]" },
    { "y3 := ode_coupled3(t, x, y, z, y, -x, -z, 0, 1, 0, 1, π, 200) ; (y3[0] ~ -1) && (y3[2] ~ exp(-π))",
      "1", "real",
      "[Exp 2] 3D coupled ODE system 'ode_coupled3(t, x, y, z, ...)' -> [-1, 0, e^-pi]" },
    { "ys := ode_rk4_sys((t, y) => [y[1], -y[0]], 0, list(1, 0), π, 200) ; (ys[0] ~ -1) && (ys[1] ~ 0)",
      "1", "real",
      "[Exp 2] N-dimensional closure ODE solver 'ode_rk4_sys((t, y) => [y[1], -y[0]], ...)' -> [-1, 0]" },
    { "tr := ode_rk4_sys_traj(t, y, [y[1], -y[0]], 0, [1, 0], π, 20) ; (mat_rows(tr) == 21) && (mat_cols(tr) == 2) && (tr[20, 0] ~ -1)",
      "1", "real",
      "[Exp 2] N-dimensional ODE trajectory matrix 'ode_rk4_sys_traj' -> 21x2 matrix" },

    /* 38. Expansion 4: Symbolic Differentiation, Algebraic Simplification, Evaluation & Taylor Series */
    { "sym_simplify((x + 0) * 1 + 2 * x + 3 * x - 0)",
      "\"6 * x\"", "str",
      "[Exp 4] Symbolic algebraic simplification & like-term folding '(x+0)*1 + 2*x + 3*x' -> '6 * x'" },
    { "simplify(((x ^ 2) ^ 3) * (x ^ 0) + (y - y) + 0 * sin(x))",
      "\"x ^ 6\"", "str",
      "[Exp 4] Symbolic power/zero/cancellation simplification -> 'x ^ 6'" },
    { "sym_diff(x, x^3 + 2 * sin(x))",
      "\"3 * x ^ 2 + 2 * cos(x)\"", "str",
      "[Exp 4] Symbolic first derivative 'sym_diff(x, x^3 + 2 * sin(x))' -> '3 * x ^ 2 + 2 * cos(x)'" },
    { "sym_diff_n(x, x^4 + 5 * x^2, 2)",
      "\"12 * x ^ 2 + 10\"", "str",
      "[Exp 4] Symbolic 2nd derivative 'sym_diff_n(x, x^4 + 5 * x^2, 2)' -> '12 * x ^ 2 + 10'" },
    { "sym_diff(x, exp(2 * x) + ln(x))",
      "\"2 * exp(2 * x) + 1 / x\"", "str",
      "[Exp 4] Symbolic chain rule on exp(2*x) and ln(x) -> '2 * exp(2 * x) + 1 / x'" },
    { "df := sym_diff(x, x^3 + 4 * x) ; sym_eval(df, x, 3)",
      "31", "real",
      "[Exp 4] Symbolic derivative evaluated numerically via 'sym_eval(df, x, 3)' -> 3*9 + 4 = 31" },
    { "sym_taylor(x, exp(x), 0, 3)",
      "\"1 + x + 0.5 * x ^ 2 + 0.1666666667 * x ^ 3\"", "str",
      "[Exp 4] Symbolic Maclaurin expansion 'sym_taylor(x, exp(x), 0, 3)'" },

    /* 39. Expansion 5: String Formatting, Case/Trim/Pad, Regex Engine & `$map` Dictionary Type */
    { "fmt(\"pi={:.2f}, hex={:X}, name={}\", π, 255, \"MEP\")",
      "\"pi=3.14, hex=FF, name=MEP\"", "str",
      "[Exp 5] Python-style string formatting 'fmt(\"pi={:.2f}, hex={:X}, name={}\", ...)'" },
    { "format(\"%s=%04d (%.1f%%)\", \"score\", 42, 98.6)",
      "\"score=0042 (98.6%)\"", "str",
      "[Exp 5] Printf-style string formatting 'format(\"%s=%04d (%.1f%%)\", ...)'" },
    { "upper(\"mep\") + \"_\" + lower(\"MATH\") + \"_\" + trim(\"  hi  \") + \"_\" + pad_left(\"7\", 3, \"0\")",
      "\"MEP_math_hi_007\"", "str",
      "[Exp 5] String helpers 'upper', 'lower', 'trim', and 'pad_left'" },
    { "index_of(\"banana\", \"na\") * 100 + str_count(\"banana\", \"na\") * 10 + contains(\"banana\", \"nan\")",
      "221", "real",
      "[Exp 5] String search 'index_of' (2), 'str_count' (2), and 'contains' (1) -> 221" },
    { "replace(\"a-b-c\", \"-\", \":\") + \"_\" + replace_first(\"a-b-c\", \"-\", \":\") + \"_\" + split(\"x,y,z\", \",\").str_join(\"+\")",
      "\"a:b:c_a:b-c_x+y+z\"", "str",
      "[Exp 5] 'replace', 'replace_first', and 'split' + 'str_join'" },
    { "regex_match(\"user_42@mep.org\", \"^[a-z]+_\\d+@[a-z]+\\.org$\") * 10 + (regex_find(\"id=987;\", \"\\d+\") == \"987\")",
      "11", "real",
      "[Exp 5] Backtracking regex 'regex_match' and 'regex_find'" },
    { "regex_find_all(\"x1=12, y2=345, z3=67\", \"\\d+\").len() * 10 + (regex_replace(\"a1b22c333\", \"\\d+\", \"#\") == \"a#b#c#\")",
      "61", "real",
      "[Exp 5] 'regex_find_all' (6 matches) and 'regex_replace' -> 61" },
    { "regex_split(\"one, two;three  four\", \"[,;\\s]+\")",
      "[\"one\", \"two\", \"three\", \"four\"]", "vec",
      "[Exp 5] 'regex_split' over character class quantifier '[,;\\s]+'" },
    { "m := dict(\"a\", 10, \"b\", 20) ; m[\"c\"] = 30 ; m[\"a\"] += 5 ; (\"b\" in m) * 100 + m[\"a\"] + m[\"c\"] + m.get_or(\"z\", 9)",
      "154", "real",
      "[Exp 5] First-class '$map' dictionary: dict(), m[k] read/write/+=, 'in', and get_or -> 154" },
    { "m := map(\"x\", 2, \"y\", 3, \"z\", 4) ; m.remove(\"y\") ; s := 0 ; (k, v in m) { s += v * 10 } ; s + m.len()",
      "62", "real",
      "[Exp 5] '$map' key removal 'm.remove(\"y\")' + 2-var '(k, v in m)' iteration -> 60 + 2 = 62" },
    { "m := dict(\"k1\", 7, \"k2\", 8) ; m2 := _(m_) ; m2[\"k1\"] * 10 + m2[\"k2\"]",
      "78", "real",
      "[Exp 5] '$map' JSON-style stringifier 'm_' (\"{\"k1\": 7, \"k2\": 8}\") & '_s' parser round-trip -> 78" },

    /* 40. Expansion 6: Arena Defragmentation / Mark-Sweep Compaction on Scratch Pool */
    { "v := vec(1, 2, 3) ; (i in 1...120) { v.push(i) } ; reclaimed := gc_compact() ; (reclaimed > 0) && (v.len() == 123) && (v[122] == 120) && (pool_used() > 0)",
      "1", "real",
      "[Exp 6] Mark-Sweep scratch pool compaction 'gc_compact()' reclaims realloc blocks while preserving live vector" },

    /* 41. Security & Correctness Audit Fixes (Groups 1-4) */
    { "sum(vec(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40))",
      "820", "real",
      "[Audit 2.1] Variadic vec(1,...,40) preserves all 40 arguments (sum == 820, not 528)" },
    { "len(str_repeat(\"abcdefghij\", 80))",
      "800", "real",
      "[Audit 2.2] String concatenation supports >511-byte strings without truncation (len == 800)" },
    { "inf := 1e308 * 10 ; n := 0 ^ -1 - 0 ^ -1 ; (inf == inf) * 10000 + (inf != inf) * 1000 + (n != n) * 100 + (1e-20 == 0) * 10 + (1e-13 == 2e-13)",
      "10100", "real",
      "[Audit 3.2] IEEE-aware == and != for inf, nan, 1e-20 == 0 (0), and 1e-13 == 2e-13 (0)" },
    { "n := 0 ^ -1 - 0 ^ -1 ; is_nan(min(1, n)) * 1000 + is_nan(max(n, 1)) * 100 + (round(1, 400) == 1) * 10 + (round(1, -400) == 0)",
      "1111", "real",
      "[Audit 3.4 & 3.5] NaN-propagating min/max and overflow-safe round(1, 400) & round(1, -400)" },
    { "f(n) := n <= 0 ? 0 : 1 + f(n - 1) ; f(128)",
      "128", "real",
      "[Audit 3.6] Full documented user recursion depth of 128 levels (f(128) == 128)" },
    { "(5--3) * 100 + (.5 + .25) * 100 + len(\"say \\\"hi\\\"\")",
      "883", "real",
      "[Audit 4.2] Binary-minus before unary-minus (5--3 == 8), leading-dot (.5+.25), and escaped quotes" },

    /* 42. Second Audit Report Fixes: Unit Preservation in Calculus (`prod`, `sigma`, `lim`, `fmin`, `fmax`, `solve`) & Pure `.math` Map/Hadamard */
    { "p := prod(k, 2m, 1, 3) ; unit_of(p) + \":\" + (p to_m3)_",
      "\"volume:8\"", "str",
      "[Audit2 Bug 3] prod(k, 2m, 1, 3) preserves compound unit m * m * m -> 8 [volume] (m^3)" },
    { "s := sigma(k, k * 2m, 1, 4) ; unit_of(s) + \":\" + (s to_m)_",
      "\"length:20\"", "str",
      "[Audit2 Bug 3] sigma(k, k * 2m, 1, 4) preserves unit -> 20 [length] (m)" },
    { "l := lim(x, (x^2 - (4 m2)) / (x - 2m), 2m) ; unit_of(l) + \":\" + (l to_m)_",
      "\"length:4\"", "str",
      "[Audit2 Bug 3] lim(x, (x^2 - 4m^2)/(x - 2m), 2m) preserves unit -> 4 [length]" },
    { "xm := fmin(x, (x - 3m)^2 + 5 m2, 0m, 10m) ; xM := fmax(x, -((x - 4m)^2) + 9 m2, 0m, 10m) ; unit_of(xm) + \":\" + (xm to_m)_ + \",\" + (xM to_m)_",
      "\"length:3,4\"", "str",
      "[Audit2 Bug 3] fmin and fmax preserve bound-variable unit [length] -> 3m, 4m" },
    { "xs := solve(x, 2 * x == 10m, 1m) ; unit_of(xs) + \":\" + (xs to_m)_",
      "\"length:5\"", "str",
      "[Audit2 Bug 3] solve(x, 2*x == 10m, 1m) preserves unit [length] -> 5m" },
    { "m := dict(\"a\", 10, \"b\", 20) ; m.keys().len() * 100 + m.vals().sum() + m.entries().len()",
      "232", "real",
      "[Audit2 Migration] Pure .math map keys(), vals()/values(), and entries() in vec.math -> 232" },

    /* 43. Section #1 & #3: Pure `.math` Matrix Algebra, Symbolic Integration (`sym_integ`) & Symbolic Solver (`sym_solve`) */
    { "A := [10m, 20m ; 30m, 40m] ; B := [2m, 4m ; 6m, 8m] ; C := (A + B - (-B)) / 2 ; unit_of(C) + \":\" + (C[0, 1] to_m)_",
      "\"length:14\"", "str",
      "[Sec 1] Pure .math matrix +, -, unary -, and scalar / preserving SI unit 'length' -> 14m" },
    { "T1 := [[[1, 2], [3, 4]], [[5, 6], [7, 8]]] ; T2 := (T1 * 3 - T1) / 2 ; (T2 == T1) * 10 + mat_rank(T2)",
      "13", "real",
      "[Sec 1] Pure .math N-D tensor scaling & subtraction via shape-preserving @mreshape (rank 3)" },
    { "sym_integ(x, 3 * x^2 + 4 * x + 5)",
      "\"x ^ 3 + 2 * x ^ 2 + 5 * x\"", "str",
      "[Sec 3] Symbolic polynomial integration 'sym_integ(x, 3*x^2 + 4*x + 5)' -> 'x ^ 3 + 2 * x ^ 2 + 5 * x'" },
    { "sym_integ(x, cos(2 * x) + exp(x))",
      "\"sin(2 * x) / 2 + exp(x)\"", "str",
      "[Sec 3] Symbolic linear-substitution integration 'sym_integ(x, cos(2*x) + exp(x))' -> 'sin(2 * x) / 2 + exp(x)'" },
    { "F := sym_integ(x, x * exp(x)) ; sym_eval(F, x, 2) ~ exp(2)",
      "1", "real",
      "[Sec 3] Symbolic integration by parts 'sym_integ(x, x * exp(x))' -> x*exp(x) - exp(x)" },
    { "(_sym_solve(x, 2 * x + 5 == 17)) * 100 + (_sym_solve(x, x^2 - 5 * x + 6 == 0)).sum()",
      "605", "real",
      "[Sec 3] Symbolic linear 'sym_solve(x, 2*x + 5 == 17)' (6) and quadratic 'x^2 - 5*x + 6 == 0' ([2, 3])" },
    { "sym_solve(x, a * x + b == c)",
      "\"(c - b) / a\"", "str",
      "[Sec 3] Symbolic linear equation solver 'sym_solve(x, a * x + b == c)' -> '(c - b) / a'" },
    { "sym_solve(x, a * x^2 - b == 0)",
      "\"sqrt(b / a)\"", "str",
      "[Sec 3] Symbolic pure quadratic solver 'sym_solve(x, a * x^2 - b == 0)' -> 'sqrt(b / a)'" },
    { "sym_solve(x, x^2 + 2 * b * x + c == 0)",
      "\"(-(2 * b) + sqrt((2 * b) ^ 2 - 4 * c)) / 2\"", "str",
      "[Sec 3] Symbolic general quadratic formula 'sym_solve(x, x^2 + 2*b*x + c == 0)'" },

    /* 44. Section #2: Pure `.math` `mat_dim`/`mat_rows`/`mat_cols`/`mat_shape`/`mat_zeros` & 7D SI `unit_dim`/`dim_vec`/`same_dim` */
    { "T3 := [[[1, 2, 3], [4, 5, 6]], [[7, 8, 9], [10, 11, 12]]] ; mat_dim(T3, 0) * 100 + mat_dim(T3, 1) * 10 + mat_dim(T3, -1)",
      "223", "real",
      "[Sec 2] 'mat_dim(m, axis)' with positive & negative axes on 2x2x3 3D tensor -> 223" },
    { "T3 := [[[1, 2, 3], [4, 5, 6]], [[7, 8, 9], [10, 11, 12]]] ; mat_shape(T3)",
      "[2, 2, 3]", "vec",
      "[Sec 2] Pure .math 'mat_shape(T3)' via comprehension over @mdim -> [2, 2, 3]" },
    { "Z := mat_zeros(2, 3) ; mat_rows(Z) * 100 + mat_cols(Z) * 10 + Z.mat_sum()",
      "230", "real",
      "[Sec 2] Pure .math 'mat_zeros(2, 3)', 'mat_rows(Z)', and 'mat_cols(Z)' -> 230" },
    { "dim_vec(10 N)",
      "[1, 1, -2, 0, 0, 0, 0]", "vec",
      "[Sec 2] Pure .math 7D SI base dimensional exponent vector 'dim_vec(10 N)' -> [1, 1, -2, 0, 0, 0, 0]" },
    { "unit_dim(5 J, 0) * 100 + unit_dim(5 J, 1) * 10 + (-unit_dim(5 J, 2)) + same_dim(10 N * 2 m, 5 J) * 1000",
      "1212", "real",
      "[Sec 2] 'unit_dim(5 J, axis)' (L=2, M=1, T=-2) and 'same_dim(10 N * 2 m, 5 J)' -> 1212" },

    /* 45. SBO Arena-to-Heap Spill & Automatic Memory Management (Zero New Keywords) */
    { "v := 1...20000 ; v.len() * 10 + (v[0] == 1) + (v[19999] == 20000)",
      "200002", "real",
      "[SBO/Heap] 20,000-element range vector '1...20000' transparently spills to heap (> 8,192 f64_pool)" },
    { "v := vec(9000, 5) ; v[0] + v[8999] + v.len()",
      "9010", "real",
      "[SBO/Heap] 'vec(9000, 5)' succeeds via transparent heap spill -> 5 + 5 + 9000 = 9010" },
    { "M := (1...14400).reshape(120, 120) ; M[0, 0] + M[119, 119] + mat_rows(M) + mat_cols(M)",
      "14641", "real",
      "[SBO/Heap] 120x120 matrix (14,400 doubles) reshaped from range vector spills to heap -> 14641" },
    { "v := vec() ; (i in 1...100) { v.push(i) } ; v.len() * 1000 + v[0] + v[99]",
      "100101", "real",
      "[SBO/Heap] Dynamic vector starts in SBO f64_pool (<=64) and auto-migrates to heap on growth (>64)" },
    { "s := 0 ; (i in 1...150) { tmp := vec(2000, i) ; s += tmp[0] + tmp[1999] } ; s",
      "22650", "real",
      "[SBO/Heap] 150 loop iterations each allocating a 2,000-slot heap vector auto-GC with zero leaks" },

    /* 46. Generic '...' Range, Angle-Bracket Exclusivity ('>..', '..<', '>..<') & Nested Skip Ranges */
    { "1...5", "[1, 2, 3, 4, 5]", "vec",
      "[Generic Range] Inclusive-inclusive '1...5' -> [1, 2, 3, 4, 5]" },
    { "1>..5", "[2, 3, 4, 5]", "vec",
      "[Generic Range] Exclusive-inclusive '1>..5' -> [2, 3, 4, 5]" },
    { "1..<5", "[1, 2, 3, 4]", "vec",
      "[Generic Range] Inclusive-exclusive '1..<5' -> [1, 2, 3, 4]" },
    { "1>..<5", "[2, 3, 4]", "vec",
      "[Generic Range] Exclusive-exclusive '1>..<5' -> [2, 3, 4]" },
    { "v := 1..<25...75>..100 ; v.len() * 100000 + v[0] * 1000 + v[23] * 10 + (v[24] - 50)",
      "4901266", "real",
      "[Nested Range] '1..<25...75>..100' skips 25..75 -> 1..24 and 76..100 (len=49, v[0]=1, v[23]=24, v[24]=76)" },
    { "s := 0 ; (x in 1..<25...75>..100) { s += x } ; s",
      "2500", "real",
      "[Nested Range Loop] '(x in 1..<25...75>..100) { s += x }' -> sum(1..24)=300 + sum(76..100)=2200 = 2500" },
    { "v := 1..<25...50>..<75...90>..<100 ; v.len() * 10000 + v[0] * 100 + v[23] + (v[24] == 51) * 1000 + (v[47] == 74) * 100 + (v[48] == 91) * 10 + (v[56] == 99)",
      "571235", "real",
      "[Multi-Skip Range] '1..<25...50>..<75...90>..<100' -> 1..24 (24), 51..74 (24), 91..99 (9) -> len=57" },
    { "s := 0 ; (x in 1..<25...50>..<75...90>..<100) { s += x } ; s",
      "2655", "real",
      "[Multi-Skip Loop] '(x in 1..<25...50>..<75...90>..<100) { s += x }' -> 300 + 1500 + 855 = 2655" },
    { "1>..<5...8>..<12", "[2, 3, 4, 9, 10, 11]", "vec",
      "[Nested Range] Outer exclusive bounds '1>..<5...8>..<12' -> [2, 3, 4, 9, 10, 11]" },
    { "[x for x in 1..<4...7>..10]", "[1, 2, 3, 8, 9, 10]", "vec",
      "[Nested Range Comprehension] '[x for x in 1..<4...7>..10]' -> [1, 2, 3, 8, 9, 10]" },
    { "(24 in 1..<25...75>..100) * 1000 + (25 in 1..<25...75>..100) * 100 + (75 in 1..<25...75>..100) * 10 + (76 in 1..<25...75>..100)",
      "1001", "real",
      "[Nested Range Membership] O(1) 'in' check on '1..<25...75>..100': 24->1, 25->0, 75->0, 76->1" },
    { "1..<5>..10", "[1, 2, 3, 4, 6, 7, 8, 9, 10]", "vec",
      "[Single-Point Skip Range] '1..<5>..10' skips 5 -> [1..4, 6..10]" },
    { "(1..<4...7>..10) by 2", "[1, 3, 9]", "vec",
      "[Nested Stepped Range] '(1..<4...7>..10) by 2' -> [1, 3, 9]" },
    { "switch (50) { case 1..<25...75>..100 : { 1 } else : { 0 } } * 10 + switch (80) { case 1..<25...75>..100 : { 1 } else : { 0 } }",
      "1", "real",
      "[Nested Range Switch] 'switch' pattern matching on 'case 1..<25...75>..100:' (50->0, 80->1)" },
    { NULL, NULL, NULL, NULL }
};

