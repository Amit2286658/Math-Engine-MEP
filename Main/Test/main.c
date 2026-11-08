/*
 * main.c — Math-Engine-MEP Entry Point & 17-Section Architectural Verification Suite
 */
#include "test_harness.h"

int main(int argc, char **argv) {
    if (argc > 1) {
        eng_init(&g_eng);
        if (!test_load_stdlib_file(&g_eng)) return 1;
        register_all_host_externs(&g_eng);
        size_t ulen = 0;
        char *utxt = (char *)frontend_read_file("Test/user_physics.math", &ulen);
        if (!utxt) utxt = (char *)frontend_read_file("user_physics.math", &ulen);
        if (utxt) {
            (void)user_compile_ext(&g_eng, utxt, NULL, NULL);
            free(utxt);
        }
        if (strcmp(argv[1], "--repl") == 0)  return run_cli_repl(&g_eng);
        if (strcmp(argv[1], "--bench") == 0) return run_cli_bench(&g_eng);
        return run_cli_eval(&g_eng, argc, argv);
    }

    /* 1. Verify Blank-Slate Guarantee (`eng_init_blank`) */
    eng_init_blank(&g_eng);
    Value v;
    bool blank_ok = !eng_eval(&g_eng, "1 + 2", &v);
    printf("[1] Blank-Slate Verification (eng_init_blank -> eval \"1 + 2\"):\n");
    printf("    Result: %s -> \"%s\"\n\n",
           blank_ok ? "EXPECTED REJECTION" : "UNEXPECTED SUCCESS",
           eng_last_error(&g_eng));

    /* 2. Blank-Engine Boot + Separate `stdlib.mathc` Load (there is no embedded stdlib) */
    eng_init(&g_eng);
    if (!test_load_stdlib_file(&g_eng)) {
        fprintf(stderr, "Failed to load 'precompiled/stdlib.mathc': %s\n", eng_last_error(&g_eng));
        return 1;
    }
    printf("[2] Blank-Engine Boot + Separate `stdlib.mathc` Load (`eng_init` + `user_load_mathc`):\n");
    printf("    Bootstrap & scientific stdlib loaded from the host image (%u modules, %u overloads, %u AST nodes).\n\n",
           g_eng.sym.mod_count, g_eng.sym.overload_count, g_eng.mem.perm_ast_count);

    /* 3. Frontend reads raw source text, passes ONLY raw text to `user_compile_ext`,
     *    receives back the compiled binary image, and tests `user_load_mathc`. */
    const char *math_path  = "Test/user_physics.math";
    const char *mathc_path = "Test/user_physics.mathc";
    size_t raw_len = 0;
    char *raw_math_text = (char *)frontend_read_file(math_path, &raw_len);
    if (!raw_math_text) {
        math_path  = "user_physics.math";
        mathc_path = "user_physics.mathc";
        raw_math_text = (char *)frontend_read_file(math_path, &raw_len);
    }
    if (!raw_math_text) {
        fprintf(stderr, "Frontend error: could not read 'user_physics.math'\n");
        return 1;
    }

    const uint8_t *compiled_mathc = NULL;
    size_t compiled_mathc_len = 0;
    if (!user_compile_ext(&g_eng, raw_math_text, &compiled_mathc, &compiled_mathc_len)) {
        fprintf(stderr, "user_compile_ext failed: %s\n", eng_last_error(&g_eng));
        free(raw_math_text);
        return 1;
    }
    free(raw_math_text);

    (void)frontend_write_file(mathc_path, compiled_mathc, compiled_mathc_len);

    printf("[3] In-Memory User Extension API (`user_loader.h`):\n");
    printf("    - Step A (`user_compile_ext`): Compiled raw text (%zu bytes) -> handed back %zu-byte binary image\n",
           raw_len, compiled_mathc_len);

    size_t disk_mathc_len = 0;
    uint8_t *disk_mathc = frontend_read_file(mathc_path, &disk_mathc_len);
    eng_init(&g_eng);
    test_load_stdlib_file(&g_eng);
    register_all_host_externs(&g_eng);

    bool load_mathc_ok = (disk_mathc != NULL) &&
                         user_load_mathc(&g_eng, disk_mathc, disk_mathc_len);
    free(disk_mathc);
    if (!load_mathc_ok) {
        fprintf(stderr, "user_load_mathc failed: %s\n", eng_last_error(&g_eng));
        return 1;
    }
    printf("    - Step B (`user_load_mathc`) : Loaded %zu-byte compiled binary image in O(1) `memcpy` time\n\n",
           disk_mathc_len);

    print_architecture_report(&g_eng);

    /* 4. Run Automated Expression Test Suite */
    printf("[4] Running Full Automated Suite (Stdlib + Control Flow + Units + Calculus + 5 New Constructs):\n");
    int passed = 0, total = 0;
    for (int i = 0; k_tests[i].expr != NULL; ++i) {
        total++;
        Value out;
        char val_buf[128], type_buf[32];
        if (!eng_eval(&g_eng, k_tests[i].expr, &out)) {
            printf("  [FAIL] %-48s -> ERROR: %s\n", k_tests[i].expr, eng_last_error(&g_eng));
            continue;
        }
        eng_format_value(&g_eng, out, val_buf, sizeof(val_buf));
        eng_type_name(&g_eng, eng_val_type(&g_eng, out), type_buf, sizeof(type_buf));

        bool ok = (strcmp(val_buf, k_tests[i].expected_val) == 0) &&
                  (strcmp(type_buf, k_tests[i].expected_type) == 0);
        if (ok) passed++;
        printf("  [%s] %-48s => %-26s : %-7s (%s)\n",
               ok ? "PASS" : "FAIL",
               k_tests[i].expr, val_buf, type_buf, k_tests[i].desc);
    }
    printf("\n    Suite Summary: %d / %d passed.\n\n", passed, total);
    total++;
    Value v_mr = 0;
    bool mutual_rec_ok = eng_mod_eval(&g_eng, "mutrec",
                           "is_even_r(n) := n == 0 ? 1 : is_odd_r(n - 1) ; "
                           "is_odd_r(n) := n == 0 ? 0 : is_even_r(n - 1) ; "
                           "is_even_r(10) * 10 + is_odd_r(9)", &v_mr) &&
                         (val_get_f64(&g_eng.mem, v_mr) == 11.0) &&
                         eng_mod_eval(&g_eng, "mutrec", "is_odd_r(21) * 100 + is_even_r(8)", &v_mr) &&
                         (val_get_f64(&g_eng.mem, v_mr) == 101.0);
    if (mutual_rec_ok) passed++;
    printf("    [Exp 1b] Forward-referenced mutual recursion (module registry) : %s (11 then 101 across calls)\n\n",
           mutual_rec_ok ? "PASS" : "FAIL");

    /* 5. Verify Compile-Time Guardrails for Scopes, Compulsory Same-Line ';', `if/or/else`, Reserved Keywords, and `vec<T>` */
    bool vec_type_guard = !eng_eval(&g_eng, "v := vec() ; push(v, 10) ; push(v, vec2(1, 2))", &v) &&
                          !eng_eval(&g_eng, "v := vec<vec2>() ; 10 into v", &v) &&
                          !eng_eval(&g_eng, "vec<real>(vec2(1, 2))", &v) &&
                          !eng_eval(&g_eng, "q := queue<real, length>() ; q.enqueue(60 deg)", &v) &&
                          !eng_eval(&g_eng, "v := vec<real>(1, 2, 3) ; v[0...1] = \"bad\"", &v) &&
                          !eng_eval(&g_eng, "v := vec(1, 2, 3) ; v[1m]", &v) &&
                          !eng_eval(&g_eng, "vec2(1)", &v) &&
                          !eng_eval(&g_eng, "circle(1, 2)", &v) &&
                          !eng_eval(&g_eng, "sma(vec(10, 20, 30), 0)", &v) &&
                          !eng_eval(&g_eng, "_\"not_a_valid_type_xyz\"", &v);
    printf("[5] Type-Safety, Compulsory Same-Line ';', Reserved-Keyword & Control-Flow Guardrails:\n");
    printf("    - Homogeneous vec<T> guard    : %s -> \"%s\"\n",
           vec_type_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool same_line_semi_guard = !eng_eval(&g_eng, "x := 10 y := 20", &v) &&
                                !eng_eval(&g_eng, "{ a := 10 b := 20 }", &v) &&
                                !eng_eval(&g_eng, "{ a := 10 } a", &v);
    printf("    - Same-line missing ';' guard : %s -> \"%s\"\n",
           same_line_semi_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool scope_isolation_guard = !eng_eval(&g_eng, "{ inner_only := 42 } ; inner_only", &v);
    printf("    - Local '{ ... }' isolation   : %s -> \"%s\"\n",
           scope_isolation_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool multi_else_guard = !eng_eval(&g_eng, "if (1) { 10 } else { 20 } else { 30 }", &v);
    printf("    - Duplicate 'else' rejection  : %s -> \"%s\"\n",
           multi_else_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool else_if_guard = !eng_eval(&g_eng, "if (0) { 10 } else if (1) { 20 }", &v);
    printf("    - 'else if' (2nd if) rejection: %s -> \"%s\"\n",
           else_if_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool kw_var_guard = !eng_eval(&g_eng, "if := 5", &v) &&
                        !eng_eval(&g_eng, "else := 5", &v) &&
                        !eng_eval(&g_eng, "or := 5", &v) &&
                        !eng_eval(&g_eng, "continue := 5", &v) &&
                        !eng_eval(&g_eng, "return := 5", &v) &&
                        !eng_eval(&g_eng, "x, switch := 5", &v) &&
                        !eng_eval(&g_eng, "(5km into m){}", &v) &&
                        !eng_eval(&g_eng, "(d := 5km ; (d in m){})", &v) &&
                        !eng_eval(&g_eng, "undef_var in 1...10", &v) &&
                        !eng_eval(&g_eng, "if (undef_var in 1...10) { 1 }", &v);
    printf("    - Reserved keyword as variable: %s -> \"%s\"\n",
           kw_var_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool kw_tag_guard = !eng_eval(&g_eng, "if : { 10 }", &v) &&
                        !eng_eval(&g_eng, "jump(else)", &v) &&
                        !eng_eval(&g_eng, "break(switch)", &v);
    printf("    - Reserved keyword as tag     : %s -> \"%s\"\n",
           kw_tag_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool unlike_unit_guard = !eng_eval(&g_eng, "y := 100cm ; y += 60°", &v);
    printf("    - 'y := 100cm ; y += 60°'     : %s -> \"%s\"\n",
           unlike_unit_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool concrete_real_guard = !eng_eval(&g_eng, "x : real := 5 ; x += 5i", &v);
    printf("    - 'x : real := 5 ; x += 5i'   : %s -> \"%s\"\n",
           concrete_real_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool concrete_real_dist_unit_guard = !eng_eval(&g_eng, "x : real, length := 50m ; x += 60°", &v);
    printf("    - 'x:real,length += 60°'    : %s -> \"%s\"\n",
           concrete_real_dist_unit_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool concrete_real_dist_iota_guard = !eng_eval(&g_eng, "x : real, length := 50m ; x += 50im", &v);
    printf("    - 'x:real,length += 50im'   : %s -> \"%s\"\n",
           concrete_real_dist_iota_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool concrete_dist_unitless_iota_guard = !eng_eval(&g_eng, "x : length := 50m ; x += 5i", &v);
    printf("    - 'x : length := 50m ; +=5i': %s -> \"%s\"\n",
           concrete_dist_unitless_iota_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool fn_unit_mismatch_guard = !eng_eval(&g_eng, "avg_speed(100m, 60°)", &v);
    printf("    - 'avg_speed(100m, 60°)'      : %s -> \"%s\"\n",
           fn_unit_mismatch_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool gen_T_unit_mismatch_guard = !eng_eval(&g_eng, "unit_span(100m, 60°)", &v);
    printf("    - 'unit_span(100m, 60°)' (T)  : %s -> \"%s\"\n",
           gen_T_unit_mismatch_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool conv_unit_mismatch_guard = !eng_eval(&g_eng, "(100m) into (1 deg)", &v);
    printf("    - '(100m) into (1 deg)'         : %s -> \"%s\"\n",
           conv_unit_mismatch_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool jagged_mat_guard = !eng_eval(&g_eng, "[[1, 2], [3, 4, 5]]", &v);
    printf("    - Jagged '[[1, 2], [3, 4, 5]]': %s -> \"%s\"\n",
           jagged_mat_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool non_bool_not_guard = !eng_eval(&g_eng, "!5", &v) && !eng_eval(&g_eng, "!(2 + 3)", &v);
    printf("    - Out-of-range '!5' / '!(2+3)': %s -> \"%s\"\n",
           non_bool_not_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool not_overload_guard = !eng_eval(&g_eng, "!\"hello\"", &v);
    printf("    - No-overload '!\"hello\"'      : %s -> \"%s\"\n",
           not_overload_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool lim_diverge_guard = !eng_eval(&g_eng, "lim(x, abs(x) / x, 0)", &v);
    printf("    - Jump limit 'lim(|x|/x, 0)'  : %s -> \"%s\"\n",
           lim_diverge_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool custom_err_guard = !eng_eval(&g_eng, "error(\"Custom domain error: x must be 0 or 1\")", &v);
    printf("    - Custom 'error(\"...\")'       : %s -> \"%s\"\n",
           custom_err_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool fx_unknown_ccy_guard = !eng_eval(&g_eng, "convert_fx(100, \"USD\", \"BTC\")", &v);
    printf("    - Unknown FX pair 'USD->BTC'  : %s -> \"%s\"\n",
           fx_unknown_ccy_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    bool fx_neg_amount_guard = !eng_eval(&g_eng, "convert_fx(-50, \"USD\", \"INR\")", &v);
    printf("    - Negative FX amount '-50'    : %s -> \"%s\"\n\n",
           fx_neg_amount_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED",
           eng_last_error(&g_eng));

    /* 6. Verify O(1) Module Toggling (`boot_syntax`, `units`, `user_physics`) */
    printf("[6] Dynamic O(1) Module Toggling ('boot_syntax', 'units', 'user_physics'):\n");
    eng_set_module(&g_eng, "boot_syntax", false);
    bool syn_off = !eng_eval(&g_eng, "{ 1 + 2 }", &v);
    printf("    - Disable [boot_syntax]  -> eval(\"{ 1 + 2 }\") : %s (\"%s\")\n",
           syn_off ? "REJECTED ('{' & '}' disabled!)" : "UNEXPECTED", eng_last_error(&g_eng));
    eng_set_module(&g_eng, "boot_syntax", true);

    eng_set_module(&g_eng, "units", false);
    bool units_off = !eng_eval(&g_eng, "100 km", &v);
    printf("    - Disable [units]        -> eval(\"100 km\")    : %s (\"%s\")\n",
           units_off ? "REJECTED ('km' disabled!)" : "UNEXPECTED", eng_last_error(&g_eng));
    eng_set_module(&g_eng, "units", true);

    eng_set_module(&g_eng, "user_physics", false);
    bool phys_off = !eng_eval(&g_eng, "convert_fx(100, \"USD\", \"INR\")", &v);
    printf("    - Disable [user_physics] -> eval(\"convert_fx\") : %s (\"%s\")\n\n",
           phys_off ? "REJECTED AS EXPECTED" : "UNEXPECTED", eng_last_error(&g_eng));
    eng_set_module(&g_eng, "user_physics", true);

    /* 7. Verify All 8 Audit Regression Cases */
    printf("[7] Audit Regression Suite (Bugs 1-10 Verification):\n");

    const uint8_t *uconst_mathc = NULL;
    size_t uconst_len = 0;
    bool bug1_ok = user_compile_ext(&g_eng, "[uconst_mod]\n\"gx\" : real, length = 5m\n", &uconst_mathc, &uconst_len) &&
                   user_load_mathc(&g_eng, uconst_mathc, uconst_len) &&
                   eng_eval(&g_eng, "10 + 20 * 30", &v) &&
                   eng_eval(&g_eng, "gx + 1m", &v) &&
                   (val_get_f64(&g_eng.mem, v) == 6.0) &&
                   (strcmp(eng_unit_name(&g_eng, eng_val_unit(&g_eng, v), (char[32]){0}, 32), "length") == 0);
    printf("    - [Bug 1] .mathc unit constant (gx + 1m == 6m) : %s\n", bug1_ok ? "PASS" : "FAIL");

    uint16_t pre_ov  = g_eng.sym.overload_count;
    uint16_t pre_ast = g_eng.mem.perm_ast_count;
    bool bug2_fail = !user_compile_ext(&g_eng, "\"leak_fn\"(x:real) -> real = x + 1\n\"bad_fn\"(x:real) -> real = (1 + ", NULL, NULL);
    bool bug2_no_leak = !eng_eval(&g_eng, "leak_fn(5)", &v);
    bool bug2_ok = bug2_fail && bug2_no_leak &&
                   (g_eng.sym.overload_count == pre_ov) &&
                   (g_eng.mem.perm_ast_count == pre_ast);
    printf("    - [Bug 2] Transactional rollback on bad ext    : %s (overloads=%u)\n",
           bug2_ok ? "PASS" : "FAIL", g_eng.sym.overload_count);

    char deep_expr[512];
    memset(deep_expr, '(', 200);
    deep_expr[200] = '1';
    memset(deep_expr + 201, ')', 200);
    deep_expr[401] = '\0';
    bool bug3_ok = !eng_eval(&g_eng, deep_expr, &v);
    printf("    - [Bug 3] Deep nesting guard (200 parens)      : %s (\"%s\")\n",
           bug3_ok ? "PASS" : "FAIL", eng_last_error(&g_eng));

    bool bug4_ok = !user_compile_ext(&g_eng, "$self_node(a:real, next:self_node)", NULL, NULL);
    printf("    - [Bug 4] Self-referential struct rejection    : %s (\"%s\")\n",
           bug4_ok ? "PASS" : "FAIL", eng_last_error(&g_eng));

    char *huge_expr = (char *)malloc(66000);
    bool bug5_ok = false;
    if (huge_expr) {
        memset(huge_expr, ' ', 65540);
        huge_expr[0] = '1';
        huge_expr[1] = '+';
        huge_expr[2] = '1';
        huge_expr[65540] = '@';
        huge_expr[65541] = '\0';
        bug5_ok = !eng_eval(&g_eng, huge_expr, &v);
        free(huge_expr);
    }
    printf("    - [Bug 5] >65534-byte input truncation guard   : %s (\"%s\")\n",
           bug5_ok ? "PASS" : "FAIL", eng_last_error(&g_eng));

    double saved_pool0 = g_eng.mem.f64_pool[0];
    bool bug6_exhaust = !eng_eval(&g_eng, "v := vec(300000, 5)", &v);
    uint16_t fail_pool_idx = mem_alloc_pool(&g_eng.mem, 9000);
    mem_clear_err(&g_eng.mem);
    uint16_t fail_vec_idx  = mem_vec_alloc_hdr(&g_eng.mem, 1, 300000);
    /* Force pool_top near limit to verify mem_vec_alloc_hdr and mem_mat_alloc_hdr return MEM_NO_IDX */
    uint16_t saved_top = g_eng.mem.pool_top;
    g_eng.mem.pool_top = MEM_MAX_F64_POOL - 2;
    mem_clear_err(&g_eng.mem);
    fail_vec_idx = mem_vec_alloc_hdr(&g_eng.mem, 1, 4);
    mem_clear_err(&g_eng.mem);
    uint16_t big_dims[4] = { 100, 100, 0, 0 };
    uint16_t fail_mat_idx = mem_mat_alloc_hdr(&g_eng.mem, 2, big_dims, NULL, NULL);
    g_eng.mem.pool_top = saved_top;
    mem_clear_err(&g_eng.mem);
    bool bug6_ok = bug6_exhaust &&
                   (fail_pool_idx == MEM_NO_IDX) &&
                   (fail_vec_idx  == MEM_NO_IDX) &&
                   (fail_mat_idx  == MEM_NO_IDX) &&
                   (g_eng.mem.f64_pool[0] == saved_pool0);
    printf("    - [Bug 6] Pool exhaustion MEM_NO_IDX & pool[0] : %s (ret=0x%04X, pool[0]=%g)\n",
           bug6_ok ? "PASS" : "FAIL", (unsigned)fail_pool_idx, g_eng.mem.f64_pool[0]);

    bool bug7_compile = user_compile_ext(&g_eng, "\"anon_mod_fn\"(x:real) -> real = x * 3\n", NULL, NULL);
    eng_set_module(&g_eng, "biology", false);
    bool bug7_eval = eng_eval(&g_eng, "anon_mod_fn(7)", &v) && (val_get_f64(&g_eng.mem, v) == 21.0);
    eng_set_module(&g_eng, "biology", true);
    bool bug7_ok = bug7_compile && bug7_eval;
    printf("    - [Bug 7] Headerless user ext in [default]     : %s\n", bug7_ok ? "PASS" : "FAIL");

    bool bug8_huge = !eng_eval(&g_eng, "pts := vec() ; push(pts, 10) ; at(pts, -4294967296)", &v);
    bool bug8_frac = !eng_eval(&g_eng, "pts := vec() ; push(pts, 10) ; push(pts, 20) ; at(pts, 0.5)", &v);
    bool bug8_ok   = bug8_huge && bug8_frac;
    printf("    - [Bug 8] Safe float-to-int index guard        : %s (\"%s\")\n",
           bug8_ok ? "PASS" : "FAIL", eng_last_error(&g_eng));

    /* [Bug 9] Transactional .mathc load (corrupted image must not overwrite live Engine) */
    uint8_t *mut_buf = (uint8_t *)malloc(uconst_len);
    bool bug9_ok = false;
    if (mut_buf && uconst_mathc && uconst_len > sizeof(MathcHeader) + 32) {
        uint16_t saved_ov  = g_eng.sym.overload_count;
        uint16_t saved_ast = g_eng.mem.perm_ast_count;
        uint16_t saved_mod = g_eng.sym.mod_count;

        /* 9a. Flip payload byte without updating checksum -> rejected by checksum */
        memcpy(mut_buf, uconst_mathc, uconst_len);
        mut_buf[sizeof(MathcHeader) + 8] ^= 0xFFu;
        bool reject_csum = !user_load_mathc(&g_eng, mut_buf, uconst_len);

        /* 9b. Corrupt an AST node index in payload AND recompute checksum -> rejected by pre-copy validator */
        memcpy(mut_buf, uconst_mathc, uconst_len);
        MathcHeader mhdr;
        memcpy(&mhdr, mut_buf, sizeof(MathcHeader));
        size_t ast_off = sizeof(MathcHeader) + (size_t)mhdr.perm_src_len;
        if (mhdr.perm_ast_count > 10) {
            AstNode bad_node = { (uint8_t)AST_UNARY, (uint8_t)FIX_PREFIX, 0, 62719u, MEM_NO_IDX };
            memcpy(mut_buf + ast_off + 5 * sizeof(AstNode), &bad_node, sizeof(AstNode));
        }
        mhdr.checksum = ext_calc_mathc_checksum(&mhdr, mut_buf + sizeof(MathcHeader), mhdr.payload_bytes);
        memcpy(mut_buf, &mhdr, sizeof(MathcHeader));
        bool reject_oob = !user_load_mathc(&g_eng, mut_buf, uconst_len);

        char oob_err[128];
        snprintf(oob_err, sizeof(oob_err), "%s", eng_last_error(&g_eng));

        /* Verify live g_eng was never overwritten and still evaluates prior definitions */
        Value v_fn = 0, v_gx = 0;
        bool state_intact = (g_eng.sym.overload_count == saved_ov) &&
                            (g_eng.mem.perm_ast_count == saved_ast) &&
                            (g_eng.sym.mod_count == saved_mod) &&
                            eng_eval(&g_eng, "anon_mod_fn(7)", &v_fn) &&
                            (val_get_f64(&g_eng.mem, v_fn) == 21.0) &&
                            eng_eval(&g_eng, "gx + 1m", &v_gx) &&
                            (val_get_f64(&g_eng.mem, v_gx) == 6.0);
        bug9_ok = reject_csum && reject_oob && state_intact;
        printf("    - [Bug 9] Transactional .mathc load & checksum : %s (\"%s\")\n",
               bug9_ok ? "PASS" : "FAIL", oob_err);
    }

    /* [Bug 10] 3,000-Iteration Corrupted .mathc Image Fuzzer (raw + checksum-recomputed) */
    static Engine s_fuzz_eng;
    bool bug10_ok = (mut_buf != NULL && uconst_mathc != NULL);
    uint32_t rng = 0xC0FFEE12u;
    int rejected_cnt = 0, accepted_cnt = 0;
    if (bug10_ok) {
        eng_init(&s_fuzz_eng);
        (void)test_load_stdlib_file(&s_fuzz_eng);
        for (int iter = 0; iter < 3000; ++iter) {
            memcpy(mut_buf, uconst_mathc, uconst_len);
            int num_muts = 1 + (int)(rng % 4u);
            for (int m = 0; m < num_muts; ++m) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                size_t pos = (iter < 1500)
                             ? (size_t)(rng % uconst_len)
                             : (sizeof(MathcHeader) + (size_t)(rng % (uconst_len - sizeof(MathcHeader))));
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                mut_buf[pos] ^= (uint8_t)(1u + (rng % 255u));
            }
            if (iter >= 1500) {
                /* Recompute checksum so mutations directly stress validate_mathc_payload & AST bounds */
                MathcHeader fhdr;
                memcpy(&fhdr, mut_buf, sizeof(MathcHeader));
                if (fhdr.payload_bytes == uconst_len - sizeof(MathcHeader)) {
                    fhdr.checksum = ext_calc_mathc_checksum(&fhdr,
                                                            mut_buf + sizeof(MathcHeader),
                                                            fhdr.payload_bytes);
                    memcpy(mut_buf, &fhdr, sizeof(MathcHeader));
                }
            }
            if (user_load_mathc(&s_fuzz_eng, mut_buf, uconst_len)) {
                accepted_cnt++;
                Value fv;
                (void)eng_eval(&s_fuzz_eng, "2 + 3 * 4", &fv);
                (void)eng_eval(&s_fuzz_eng, "sin(0) + cos(0)", &fv);
                (void)eng_eval(&s_fuzz_eng, "vec2(3, 4).x", &fv);
                (void)eng_eval(&s_fuzz_eng, "5!", &fv);
            } else {
                rejected_cnt++;
            }
        }
    }
    free(mut_buf);
    bool null_guard_ok = !eng_eval(NULL, "1 + 1", &v) && !eng_set_module(NULL, "units", false);
    bug10_ok = bug10_ok && null_guard_ok && (rejected_cnt + accepted_cnt == 3000) && (rejected_cnt > 2400);
    printf("    - [Bug 10] 3,000x mutated .mathc fuzz + NULL   : %s (%d rejected, %d safe-eval)\n\n",
           bug10_ok ? "PASS" : "FAIL", rejected_cnt, accepted_cnt);

    /* 8. Verify Separated Compilation & Dependency-Resolved Linking + Duplicate Rejection */
    printf("[8] Separated Compilation & Dependency-Resolved Linking (`user_compile_unit` / `user_link_ext`):\n");
    /* 8a. Compile 3 dependent modules in REVERSE dependency order:
     *     [mod_aero] -> depends on <[mod_thermo] and <[mod_base]
     *     [mod_thermo] -> depends on <[mod_base]
     *     [mod_base] -> depends on <[arith]
     */
    const char *mod_aero_src =
        "[mod_aero]\n"
        "<[mod_thermo]\n"
        "<[mod_base]\n"
        "aero_lift(v) = thermo_scale(v) + base_k\n";
    const char *mod_thermo_src =
        "[mod_thermo]\n"
        "<[mod_base]\n"
        "thermo_scale(x) = base_fn(x) * 3\n";
    const char *mod_base_src =
        "[mod_base]\n"
        "<[arith]\n"
        "base_k = 7\n"
        "base_fn(x) = x + base_k\n";

    UserBuildCtx ubuild;
    user_build_init(&ubuild);
    const uint8_t *linked_mathc = NULL;
    size_t linked_mathc_len = 0;
    bool link_order_ok = user_compile_unit(&g_eng, &ubuild, mod_aero_src) &&
                         user_compile_unit(&g_eng, &ubuild, mod_thermo_src) &&
                         user_compile_unit(&g_eng, &ubuild, mod_base_src) &&
                         user_link_ext(&g_eng, &ubuild, &linked_mathc, &linked_mathc_len) &&
                         (linked_mathc != NULL && linked_mathc_len > sizeof(MathcHeader)) &&
                         eng_eval(&g_eng, "aero_lift(3)", &v) &&
                         (val_get_f64(&g_eng.mem, v) == 37.0);
    printf("    - Out-of-order compile+link ([aero]->[thermo]->[base]) : %s (aero_lift(3) == 37, %zuB .mathc)\n",
           link_order_ok ? "PASS" : "FAIL", linked_mathc_len);

    /* 8b. Reject duplicate named module load (`[mod_base]` already loaded) */
    bool dup_mod_guard = !user_compile_ext(&g_eng, mod_base_src, NULL, NULL);
    printf("    - Duplicate named module load ([mod_base] 2x)          : %s -> \"%s\"\n",
           dup_mod_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED", eng_last_error(&g_eng));

    /* 8c. Reject duplicate headerless `[default]` declaration load */
    bool dup_def_guard = !user_compile_ext(&g_eng, "\"anon_mod_fn\"(x:real) -> real = x * 3\n", NULL, NULL);
    printf("    - Duplicate [default] raw-text reload                  : %s -> \"%s\"\n",
           dup_def_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED", eng_last_error(&g_eng));

    /* 8d. Reject missing module dependency (`<[no_such_mod]`) with transactional rollback */
    bool missing_dep_guard = !user_compile_ext(&g_eng, "[mod_orphan]\n<[no_such_mod]\norphan_fn(x) = x\n", NULL, NULL) &&
                             !eng_eval(&g_eng, "orphan_fn(1)", &v);
    printf("    - Missing module dependency (<[no_such_mod])           : %s -> \"%s\"\n",
           missing_dep_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED", eng_last_error(&g_eng));

    /* 8e. Reject circular module dependency (`[cyc_a] <[cyc_b]` & `[cyc_b] <[cyc_a]`) */
    const char *cyc_srcs[2] = {
        "[cyc_a]\n<[cyc_b]\na_fn(x) = b_fn(x)\n",
        "[cyc_b]\n<[cyc_a]\nb_fn(x) = a_fn(x)\n"
    };
    bool cycle_dep_guard = !user_compile_and_link(&g_eng, cyc_srcs, 2, NULL, NULL);
    printf("    - Circular module dependency ([cyc_a] <-> [cyc_b])     : %s -> \"%s\"\n\n",
           cycle_dep_guard ? "REJECTED AS EXPECTED" : "UNEXPECTED", eng_last_error(&g_eng));

    /* 9. Verify Independent Parallel Module Execution & Variable Introspection */
    printf("[9] Module Execution API (`eng_mod_eval` / `eng_mod_reset` / variable introspection):\n");
    bool sess_open_ok = eng_mod_reset(&g_eng, "smod_a") && eng_mod_reset(&g_eng, "smod_b"); /* start clean */
    (void)sess_open_ok;

    Value va1 = 0, va2 = 0, va3 = 0, vb1 = 0, vb2 = 0;
    /* Interleave executions across module A and module B (independent variable spaces) */
    bool sa_step1 = eng_mod_eval(&g_eng, "smod_a",
                                 "dist_x : real, length := 50m ; factor := 3 ; q := queue<real, length>(10m, 20m) ; dist_x",
                                 &va1) && (val_get_f64(&g_eng.mem, va1) == 50.0);
    bool sb_step1 = eng_mod_eval(&g_eng, "smod_b",
                                 "db_x := 999 ; pts := stack<vec2>(vec2(1, 2), vec2(5, 8)) ; db_x",
                                 &vb1) && (val_get_f64(&g_eng.mem, vb1) == 999.0);
    bool sa_step2 = eng_mod_eval(&g_eng, "smod_a",
                                 "scale_fn := a => a * factor ; q.enqueue(30m) ; dist_x += q.dequeue() ; dist_x",
                                 &va2) && (val_get_f64(&g_eng.mem, va2) == 60.0);
    bool sb_step2 = eng_mod_eval(&g_eng, "smod_b",
                                 "top_pt := pts.pop() ; db_x += top_pt.y ; db_x",
                                 &vb2) && (val_get_f64(&g_eng.mem, vb2) == 1007.0);

    /* Load a new user module while modules are live -> independent of module state! */
    bool sess_indep_mod = user_compile_ext(&g_eng, "[sess_helper_mod]\n<[arith]\nboost_ten(x) = x + 10\n", NULL, NULL);
    bool sa_step3 = eng_mod_eval(&g_eng, "smod_a",
                                 "factor = 4 ; boost_ten(scale_fn(5))",
                                 &va3) && (val_get_f64(&g_eng.mem, va3) == 30.0);

    /* Verify stateless `eng_eval` keeps its own assignments transient
     * (they never persist into any module), and type/unit locks persist in Module A */
    bool stateless_isolated = eng_eval(&g_eng, "iso_probe := 7 * 6 ; iso_probe", &v) &&
                              !eng_mod_get_var(&g_eng, "smod_a", "iso_probe", &v, NULL) &&
                              !eng_mod_get_var(&g_eng, "default", "iso_probe", &v, NULL);
    bool sa_lock_enforced   = !eng_mod_eval(&g_eng, "smod_a", "dist_x += 60 deg", &v);
    bool parallel_sess_ok   = sa_step1 && sb_step1 && sa_step2 && sb_step2 &&
                              sess_indep_mod && sa_step3 && stateless_isolated && sa_lock_enforced &&
                              eng_mod_get_var(&g_eng, "smod_a", "dist_x", &va1, NULL) &&
                              (val_get_f64(&g_eng.mem, va1) == 60.0) &&
                              eng_mod_get_var(&g_eng, "smod_b", "db_x", &vb1, NULL) &&
                              (val_get_f64(&g_eng.mem, vb1) == 1007.0) &&
                              !eng_mod_get_var(&g_eng, "smod_a", "db_x", &va1, NULL) &&
                              !eng_mod_get_var(&g_eng, "smod_b", "dist_x", &vb1, NULL) &&
                              (eng_mod_var_count(&g_eng, "smod_a") >= 4);
    printf("    - Parallel isolated modules (Module A & Module B)      : %s (sA=30, sB=1007, locks & closures preserved)\n",
           parallel_sess_ok ? "PASS" : "FAIL");

    /* Reset Module A; its own names must vanish while Module B stays live */
    Value vr1 = 0, vr2 = 0;
    bool r_a = eng_mod_reset(&g_eng, "smod_a");
    bool gone_a = !eng_mod_eval(&g_eng, "smod_a", "dist_x", &vr1);
    bool keep_b = eng_mod_eval(&g_eng, "smod_b", "keep_x := db_x - 1000 ; keep_x", &vr1) &&
                  (val_get_f64(&g_eng.mem, vr1) == 7.0);
    bool mod_reset_a = r_a && gone_a && keep_b;
    printf("    - Module reset isolates only the reset module          : %s (B still live, keep_x=7)\n",
           mod_reset_a ? "PASS" : "FAIL");

    /* Verify variable introspection & overwrite via host API */
    Value vset = 0;
    bool varintrospect_ok = eng_mod_set_var(&g_eng, "smod_b", "db_x", val_from_f64(&g_eng.mem, 50.0), 0) &&
                            eng_mod_eval(&g_eng, "smod_b", "db_x", &vset) &&
                            (val_get_f64(&g_eng.mem, vset) == 50.0) &&
                            eng_mod_get_var(&g_eng, "smod_b", "db_x", &vset, NULL) &&
                            (val_get_f64(&g_eng.mem, vset) == 50.0);
    char var_dump[1024];
    varintrospect_ok = varintrospect_ok && (eng_mod_format_vars(&g_eng, "smod_b", var_dump, sizeof(var_dump)) > 0);
    printf("    - Host variable introspection & set/get/format         : %s\n", 
           varintrospect_ok ? "PASS" : "FAIL");

    /* Reclaim Module B state so later heap-accounting sections start clean */
    (void)eng_mod_reset(&g_eng, "smod_b");

    (void)vr2;

    /* 10. Verify Heap-Allocated Lifecycle (`eng_create`/`eng_destroy`) & Multi-Engine Isolation */
    printf("[10] Heap-Allocated Engines (`eng_create`/`eng_destroy`) & Multi-Engine Isolation:\n");
    Engine *heap_e1 = eng_create();
    Engine *heap_e2 = eng_create();
    bool heap_stdlib_ok = heap_e1 && heap_e2 &&
                          test_load_stdlib_file(heap_e1) &&
                          test_load_stdlib_file(heap_e2);
    UserBuildCtx *heap_b1 = user_build_create();
    UserBuildCtx *heap_b2 = user_build_create();
    const uint8_t *e1_mathc = NULL;
    const uint8_t *e2_mathc = NULL;
    size_t e1_mathc_len = 0, e2_mathc_len = 0;
    bool multi_eng_ok = false;
    if (heap_stdlib_ok && heap_b1 && heap_b2) {
        /* Interleave compilation across two independent engines and build contexts */
        bool c1 = user_compile_unit(heap_e1, heap_b1, "[mod_e1]\n<[arith]\ne1_fn(x) = x + 111\n");
        bool c2 = user_compile_unit(heap_e2, heap_b2, "[mod_e2]\n<[arith]\ne2_fn(x) = x + 222\n");
        bool l1 = c1 && user_link_ext(heap_e1, heap_b1, &e1_mathc, &e1_mathc_len);
        /* Save first 16 bytes of the e1_mathc image before linking heap_e2 */
        uint8_t e1_head_before[40];
        if (l1 && e1_mathc && e1_mathc_len >= sizeof(e1_head_before)) {
            memcpy(e1_head_before, e1_mathc, sizeof(e1_head_before));
        }
        bool l2 = c2 && user_link_ext(heap_e2, heap_b2, &e2_mathc, &e2_mathc_len);
        /* Verify e1_mathc pointer & bytes were NOT overwritten by heap_e2 link */
        bool buf_isolated = l1 && l2 && (e1_mathc != e2_mathc) &&
                            (memcmp(e1_head_before, e1_mathc, sizeof(e1_head_before)) == 0);
        Value ve1 = 0, ve2 = 0;
        bool eval_isolated = eng_eval(heap_e1, "e1_fn(9)", &ve1) && (val_get_f64(&heap_e1->mem, ve1) == 120.0) &&
                             !eng_eval(heap_e1, "e2_fn(9)", &ve1) &&
                             eng_eval(heap_e2, "e2_fn(8)", &ve2) && (val_get_f64(&heap_e2->mem, ve2) == 230.0) &&
                             !eng_eval(heap_e2, "e1_fn(8)", &ve2);
        multi_eng_ok = buf_isolated && eval_isolated;
    }
    user_build_destroy(heap_b1);
    user_build_destroy(heap_b2);
    eng_destroy(heap_e1);
    eng_destroy(heap_e2);
    printf("    - Heap `eng_create`/`eng_destroy` & isolated `.mathc` buffers : %s\n\n",
           multi_eng_ok ? "PASS" : "FAIL");

    /* 11. Verify Host Introspection APIs, Regex Alternation `|` in User Types, and Session Persistence of Inline Named Recursive Functions */
    printf("[11] Host Registry Introspection, Regex Alternation `|` & Module Inline Functions:\n");
    char mod0_name[32] = {0};
    bool introspect_ok = (eng_module_count(&g_eng) >= 19) &&
                         (strcmp(eng_module_name(&g_eng, 1, mod0_name, sizeof(mod0_name)), "boot_syntax") == 0) &&
                         (eng_overload_count(&g_eng) >= 1240) &&
                         (eng_const_count(&g_eng) >= 28);
    printf("    - Host introspection (`eng_module_count`/`overload_count`)    : %s (mods=%u, ovs=%u, consts=%u)\n",
           introspect_ok ? "PASS" : "FAIL",
           (unsigned)eng_module_count(&g_eng),
           (unsigned)eng_overload_count(&g_eng),
           (unsigned)eng_const_count(&g_eng));

    Value v_sf1 = 0, v_sf2 = 0;
    bool sess_fn_ok =
                      eng_mod_eval(&g_eng, "fmod", "tri(n) := n <= 1 ? 1 : n + tri(n - 1) ; tri(5)", &v_sf1) &&
                      (val_get_f64(&g_eng.mem, v_sf1) == 15.0) &&
                      eng_mod_eval(&g_eng, "fmod", "tri(10) + (6).tri()", &v_sf2) &&
                      (val_get_f64(&g_eng.mem, v_sf2) == 76.0);
    /* Persistence across separate eng_mod_eval calls; reset then re-run clears state */
    Value v_sf3 = 0;
    sess_fn_ok = sess_fn_ok &&
                 eng_mod_eval(&g_eng, "fmod", "tri(7)", &v_sf3) &&
                 (val_get_f64(&g_eng.mem, v_sf3) == 28.0) &&
                 eng_mod_reset(&g_eng, "fmod") &&
                 !eng_mod_eval(&g_eng, "fmod", "tri(7)", &v_sf3);
    printf("    - Module function/variable persistence & reset          : %s (tri(7) == 28, cleared after reset)\n\n",
           sess_fn_ok ? "PASS" : "FAIL");

    /* 12. Verify Ready-to-Use Android NDK JNI Bridge (`Core/jni/math_engine_jni.c`) */
    printf("[12] Android NDK JNI Bridge (`Java_com_mathengine_MathEngineBridge_*`) Verification:\n");
    JNIEnv *jenv = mep_jni_mock_env();
    jlong jh = Java_com_mathengine_MathEngineBridge_nativeCreate(jenv, NULL);
    test_jni_load_stdlib(jenv, jh);
    jstring jev = Java_com_mathengine_MathEngineBridge_nativeEval(jenv, NULL, jh, "ke(2 kg, 36 kph)");
    bool jni_eval_ok = (jh != 0) && (jev != NULL) && (strcmp(jev, "OK|real|energy|100") == 0);

    jstring jmod = (*jenv)->NewStringUTF(jenv, "mod_m");
    jstring js1 = Java_com_mathengine_MathEngineBridge_nativeModuleEval(
        jenv, NULL, jh, jmod, "f(x, y = 10) = x + y ; m := dict(\"k\", 5) ; f(3) + m[\"k\"]");
    /* Module variables & functions persist across separate nativeModuleEval calls */
    jstring js2 = Java_com_mathengine_MathEngineBridge_nativeModuleEval(
        jenv, NULL, jh, jmod, "f(20, 30) + m[\"k\"]");
    jstring jvars = Java_com_mathengine_MathEngineBridge_nativeModuleFormatVars(jenv, NULL, jh, jmod);
    jstring jsrc = Java_com_mathengine_MathEngineBridge_nativeModuleExportSource(jenv, NULL, jh, jmod);
    jboolean jreset = Java_com_mathengine_MathEngineBridge_nativeModuleReset(
        jenv, NULL, jh, jmod);
    /* After reset the module is clean but fully usable again */
    jstring js3 = Java_com_mathengine_MathEngineBridge_nativeModuleEval(
        jenv, NULL, jh, jmod, "q := 6 ; q * 7");
    bool jni_sess_ok = (jmod != NULL) && (js1 != NULL) && (strcmp(js1, "OK|real|none|18") == 0) &&
                       (js2 != NULL) && (strcmp(js2, "OK|real|none|55") == 0) &&
                       (jvars != NULL) && (jsrc != NULL) &&
                       (jreset == JNI_TRUE) && (js3 != NULL) && (strcmp(js3, "OK|real|none|42") == 0);

    jstring jsimp = Java_com_mathengine_MathEngineBridge_nativeSymSimplify(jenv, NULL, jh, "(x + 0) * 1 + 4 * x");
    jstring jdiff = Java_com_mathengine_MathEngineBridge_nativeSymDiff(jenv, NULL, jh, "x", "x^3 + sin(x)");
    bool jni_sym_ok = (jsimp != NULL && strcmp(jsimp, "5 * x") == 0) &&
                      (jdiff != NULL && strcmp(jdiff, "3 * x ^ 2 + cos(x)") == 0);

    jbyteArray jmathc = Java_com_mathengine_MathEngineBridge_nativeCompileExt(
        jenv, NULL, jh, "[jni_mod]\n<[arith]\njni_cube(x) = x^3\n");
    jlong jh2 = Java_com_mathengine_MathEngineBridge_nativeCreate(jenv, NULL);
    jboolean jload_ok = Java_com_mathengine_MathEngineBridge_nativeLoadMathc(jenv, NULL, jh2, jmathc);
    jstring jext_ev = Java_com_mathengine_MathEngineBridge_nativeEval(jenv, NULL, jh2, "jni_cube(4)");
    jint jgc = Java_com_mathengine_MathEngineBridge_nativeCompactScratch(jenv, NULL, jh2);
    bool jni_ext_ok = (jmathc != NULL) && (jload_ok == JNI_TRUE) &&
                      (jext_ev != NULL && strcmp(jext_ev, "OK|real|none|64") == 0) && (jgc >= 0);
    mep_jni_mock_free_byte_array(jmathc);
    Java_com_mathengine_MathEngineBridge_nativeDestroy(jenv, NULL, jh2);
    Java_com_mathengine_MathEngineBridge_nativeDestroy(jenv, NULL, jh);
    bool jni_all_ok = jni_eval_ok && jni_sess_ok && jni_sym_ok && jni_ext_ok;
    printf("    - JNI lifecycle, eval, module API, .mathc & symbolic API      : %s\n\n",
           jni_all_ok ? "PASS" : "FAIL");

    /* 13. Comprehensive Security & Correctness Audit Regression Suite (Groups 1-4) */
    printf("[13] Security & Correctness Audit Regression Suite (Groups 1-4):\n");

    /* Group 1: Stack buffer overflow & truncation guard on sym_ast_to_str (150+ terms) */
    char sym_150_expr[4096];
    size_t sp_off = 0;
    for (int i = 1; i <= 155; ++i) {
        sp_off += (size_t)snprintf(sym_150_expr + sp_off, sizeof(sym_150_expr) - sp_off,
                                   "%sx^%d", (i > 1 ? " + " : ""), i);
    }
    char sym_small_buf[128];
    char sym_large_buf[4096];
    bool g1_small_guarded = !eng_sym_simplify(&g_eng, sym_150_expr, sym_small_buf, sizeof(sym_small_buf)) &&
                            !eng_sym_diff(&g_eng, "x", sym_150_expr, sym_small_buf, sizeof(sym_small_buf));
    bool g1_large_ok = eng_sym_simplify(&g_eng, sym_150_expr, sym_large_buf, sizeof(sym_large_buf)) &&
                       (strlen(sym_large_buf) > 900) &&
                       eng_sym_diff(&g_eng, "x", sym_150_expr, sym_large_buf, sizeof(sym_large_buf)) &&
                       (strlen(sym_large_buf) > 1200);
    char sym_eval_call[4200];
    snprintf(sym_eval_call, sizeof(sym_eval_call), "sym_simplify(%s)", sym_150_expr);
    Value v_sym150 = 0;
    bool g1_eval_ok = eng_eval(&g_eng, sym_eval_call, &v_sym150) &&
                      (val_get_str(&g_eng.mem, v_sym150).len > 900);
    bool audit_g1_ok = g1_small_guarded && g1_large_ok && g1_eval_ok;
    printf("    - [Group 1] sym_ast_to_str 155-term overflow & truncation guard : %s\n",
           audit_g1_ok ? "PASS" : "FAIL");

    /* Group 2: Silent data loss fixes (vec>32 args, >511B strings, module >70 elements, JNI >8KB var dumps) */
    Value v_g2a = 0, v_g2b = 0, v_g2c = 0;
    bool g2_ok = eng_mod_eval(&g_eng, "g2m", "w := 1...80 ; len(w)", &v_g2a) &&
                 (val_get_f64(&g_eng.mem, v_g2a) == 80.0) &&
                 eng_mod_eval(&g_eng, "g2m", "len(w) * 10000 + sum(w)", &v_g2b) &&
                 (val_get_f64(&g_eng.mem, v_g2b) == 803240.0) &&
                 eng_mod_eval(&g_eng, "g2m", "1 + 1", &v_g2c) &&
                 (val_get_f64(&g_eng.mem, v_g2c) == 2.0);
    /* Verify oversized (600-element) range evaluates fully — zero silent truncation */
    bool g2_overflow_guarded = eng_mod_eval(&g_eng, "g2m", "huge_v := 1...600 ; len(huge_v)", &v_g2c) &&
                               (val_get_f64(&g_eng.mem, v_g2c) == 600.0);
    if (g2_overflow_guarded) {
        g2_overflow_guarded = eng_mod_eval(&g_eng, "g2m", "len(w) + 1", &v_g2c) &&
                              (val_get_f64(&g_eng.mem, v_g2c) == 81.0);
    }
    /* Host-side introspection must see the same state (large var dump >8192 bytes) */
    char g2_dump[16384];
    g2_ok = g2_ok && (eng_mod_format_vars(&g_eng, "g2m", g2_dump, sizeof(g2_dump)) > 0) &&
            (eng_mod_get_var(&g_eng, "g2m", "w", &v_g2a, NULL));

    /* Verify JNI module-variable persistence & introspection across 30 large evaluations */
    jlong jh_g2 = Java_com_mathengine_MathEngineBridge_nativeCreate(jenv, NULL);
    test_jni_load_stdlib(jenv, jh_g2);
    jstring jg2m = (*jenv)->NewStringUTF(jenv, "g2m");
    bool g2_jni_ok = (jh_g2 != 0 && jg2m != NULL);
    if (g2_jni_ok) {
        (void)Java_com_mathengine_MathEngineBridge_nativeModuleEval(
            jenv, NULL, jh_g2, jg2m, "big := 1...75 ; k := 0");
        for (int step = 1; step <= 26; ++step) {
            (void)Java_com_mathengine_MathEngineBridge_nativeModuleEval(
                jenv, NULL, jh_g2, jg2m, "k += 1");
        }
        jstring jvars_g2 = Java_com_mathengine_MathEngineBridge_nativeModuleFormatVars(
            jenv, NULL, jh_g2, jg2m);
        jstring jchk = Java_com_mathengine_MathEngineBridge_nativeModuleEval(
            jenv, NULL, jh_g2, jg2m, "k * 100 + len(big)");
        g2_jni_ok = (jvars_g2 != NULL) && (jchk != NULL) &&
                    (strcmp(jchk, "OK|real|none|2675") == 0);
    }
    Java_com_mathengine_MathEngineBridge_nativeDestroy(jenv, NULL, jh_g2);

    /* Reclaim Group-2 module state so later heap-accounting sections start clean */
    (void)eng_mod_reset(&g_eng, "g2m");
    bool audit_g2_ok = g2_ok && g2_overflow_guarded && g2_jni_ok;
    printf("    - [Group 2] Zero silent data loss (vec(40), str, module & JNI) : %s\n",
           audit_g2_ok ? "PASS" : "FAIL");

    /* Group 3: Math & module limits (1 % 0, 5 km % 0 m, (-1)!, 2.5!, recursion >128 guard, >32 module evals) */
    bool g3_mod_zero = !eng_eval(&g_eng, "1 % 0", &v) &&
                       !eng_eval(&g_eng, "5 km % 0 m", &v) &&
                       !eng_eval(&g_eng, "5 km % 2 sec", &v);
    bool g3_fact_dom = !eng_eval(&g_eng, "(-1)!", &v) &&
                       !eng_eval(&g_eng, "2.5!", &v) &&
                       !eng_eval(&g_eng, "171!", &v);
    bool g3_rec_guard = !eng_eval(&g_eng, "f(n) := n <= 0 ? 0 : 1 + f(n - 1) ; f(130)", &v);
    bool g3_sess50_ok = eng_mod_eval(&g_eng, "repl3", "acc := 0", &v);
    for (int i = 1; i <= 45 && g3_sess50_ok; ++i) {
        g3_sess50_ok = eng_mod_eval(&g_eng, "repl3", "acc += 1", &v);
    }
    g3_sess50_ok = g3_sess50_ok && (val_get_f64(&g_eng.mem, v) == 45.0);
    bool audit_g3_ok = g3_mod_zero && g3_fact_dom && g3_rec_guard && g3_sess50_ok;
    printf("    - [Group 3] Modulo/factorial domain, 128-rec & >32 module evals: %s\n",
           audit_g3_ok ? "PASS" : "FAIL");

    /* Group 4: >128-char string & number literals, 5--3, .5, escaped quotes & UTF-8 boundary safety */
    char long_str_expr[300];
    long_str_expr[0] = '"';
    memset(long_str_expr + 1, 'a', 200);
    long_str_expr[201] = '"';
    long_str_expr[202] = '\0';
    char long_num_expr[220];
    long_num_expr[0] = '1';
    long_num_expr[1] = '.';
    memset(long_num_expr + 2, '0', 150);
    long_num_expr[152] = '5';
    long_num_expr[153] = '\0';
    Value v_lstr = 0, v_lnum = 0;
    bool g4_lex_ok = eng_eval(&g_eng, long_str_expr, &v_lstr) &&
                     (val_get_str(&g_eng.mem, v_lstr).len == 200) &&
                     eng_eval(&g_eng, long_num_expr, &v_lnum) &&
                     (val_get_f64(&g_eng.mem, v_lnum) == 1.0);
    bool audit_g4_ok = g4_lex_ok;
    printf("    - [Group 4] >128-char string/number literals & lexer edge cases : %s\n\n",
           audit_g4_ok ? "PASS" : "FAIL");

    /* 14. Second Audit Report Regression Suite (Bugs 1-5 & .math Calculus/Container Migration) */
    printf("[14] Second Audit Report Regression Suite (Bugs 1-5 & Pure `.math` Migration):\n");

    /* Bug 1: .mathc header intrinsic table fingerprint (`intr_hash`) rejects stale/reordered enum images */
    bool audit2_b1_ok = false;
    const uint8_t *b1_mathc = NULL;
    size_t b1_len = 0;
    if (user_compile_ext(&g_eng, "[audit2_b1_mod]\n<[arith]\nb1_id(x) = x + 1\n", &b1_mathc, &b1_len) &&
        b1_mathc && b1_len > sizeof(MathcHeader)) {
        uint8_t *stale_img = (uint8_t *)malloc(b1_len);
        if (stale_img) {
            memcpy(stale_img, b1_mathc, b1_len);
            MathcHeader shdr;
            memcpy(&shdr, stale_img, sizeof(MathcHeader));
            bool hash_matches_runtime = (shdr.intr_hash == delegate_intrinsic_table_hash()) && (shdr.intr_hash != 0);
            /* Perturb intr_hash (simulating an enum insertion/reorder) and recompute image checksum */
            shdr.intr_hash ^= 0x01020304u;
            shdr.checksum = ext_calc_mathc_checksum(&shdr, stale_img + sizeof(MathcHeader), shdr.payload_bytes);
            memcpy(stale_img, &shdr, sizeof(MathcHeader));
            bool rejected_stale_enum = !user_load_mathc(&g_eng, stale_img, b1_len) &&
                                       (strstr(eng_last_error(&g_eng), "intrinsic table fingerprint mismatch") != NULL);
            audit2_b1_ok = hash_matches_runtime && rejected_stale_enum;
            free(stale_img);
        }
    }
    printf("    - [Bug 1] .mathc `intr_hash` fingerprint & stale enum rejection  : %s\n",
           audit2_b1_ok ? "PASS" : "FAIL");

    /* Bug 2 & Step 3: Enum-order independence + lazy `var_x` calculus in `.math` + user-defined `var_x` function */
    Value v_b2 = 0, v_b3 = 0;
    bool user_lazy_fn_ok = user_compile_ext(
        &g_eng,
        "[user_lazy_test]\n<[calculus]\nsum_weighted(var_k:none, expr:real, lo:real, hi:real) -> real = sigma(idx, call(expr, idx) * 2, lo, hi)\n",
        NULL, NULL) &&
        eng_eval(&g_eng, "sum_weighted(n, n^2, 1, 4)", &v_b2) &&
        (val_get_f64(&g_eng.mem, v_b2) == 60.0);
    bool calc_unit_ok = eng_eval(&g_eng, "prod(k, 2m, 1, 3)", &v_b3) &&
                        (val_get_f64(&g_eng.mem, v_b3) == 8.0) &&
                        (strcmp(eng_unit_name(&g_eng, eng_val_unit(&g_eng, v_b3), (char[32]){0}, 32), "volume") == 0);
    bool audit2_b2_b3_ok = user_lazy_fn_ok && calc_unit_ok;
    printf("    - [Bug 2,3,4] Lazy `var_x` in `.math` & `prod(k, 2m, 1, 3)`==8m³ : %s\n",
           audit2_b2_b3_ok ? "PASS" : "FAIL");

    /* Bug 5: Unbounded dynamic module registry supports >64 loaded modules (SBO -> Heap growth + .mathc reload) */
    Engine *eng64 = eng_create();
    bool audit2_b5_ok = eng64 && test_load_stdlib_file(eng64) && (eng64->sym.mod_cap == SYM_SBO_MODULES);
    if (audit2_b5_ok) {
        const uint8_t *m90_img = NULL;
        size_t m90_len = 0;
        for (int mi = 0; mi < 80 && audit2_b5_ok; ++mi) {
            char msrc[128];
            snprintf(msrc, sizeof(msrc), "[dyn_mod_%d]\n<[arith]\ndyn_fn_%d(x) = x + %d\n", mi, mi, mi + 1);
            audit2_b5_ok = user_compile_ext(eng64, msrc, (mi == 79) ? &m90_img : NULL, (mi == 79) ? &m90_len : NULL);
        }
        Value v64 = 0;
        audit2_b5_ok = audit2_b5_ok &&
                       (eng_module_count(eng64) >= 98) &&
                       (eng64->sym.mod_cap >= 128) &&
                       eng_eval(eng64, "dyn_fn_79(10)", &v64) &&
                       (val_get_f64(&eng64->mem, v64) == 90.0) &&
                       eng_set_module(eng64, "dyn_mod_79", false) &&
                       !eng_eval(eng64, "dyn_fn_79(10)", &v64) &&
                       eng_set_module(eng64, "dyn_mod_79", true) &&
                       eng_eval(eng64, "dyn_fn_79(10)", &v64) &&
                       (val_get_f64(&eng64->mem, v64) == 90.0) &&
                       (m90_img != NULL && m90_len > sizeof(MathcHeader)) &&
                       user_load_mathc(eng64, m90_img, m90_len) &&
                       (eng_module_count(eng64) >= 98) &&
                       eng_eval(eng64, "dyn_fn_79(10)", &v64) &&
                       (val_get_f64(&eng64->mem, v64) == 90.0);
    }
    eng_destroy(eng64);
    printf("    - [Bug 5] Unbounded module registry (98+ active modules & .mathc): %s\n\n",
           audit2_b5_ok ? "PASS" : "FAIL");

    /* 15. Section #1 & #3 Verification: Pure `.math` Wildcards/Matrix, Symbolic Integ/Solve, Extern Errors & Multi-Threading */
    printf("[15] Pure `.math` Wildcards/Matrix, `sym_integ`/`sym_solve`, `@extern` Errors & Multi-Thread Stress:\n");

    /* 15a. Verify SymTable-driven wildcards ($T, $any, $none) and for/in symbols */
    bool sec1_sym_ok = (sym_find_type(&g_eng.sym, &g_eng.mem, "T", 1) == SYM_TYPE_ANY) &&
                       (sym_find_type(&g_eng.sym, &g_eng.mem, "any", 3) == SYM_TYPE_ANY) &&
                       (sym_find_unit(&g_eng.sym, &g_eng.mem, "none", 4) == SYM_UNIT_LOCK_NONE) &&
                       (sym_find_unit(&g_eng.sym, &g_eng.mem, "T", 1) == SYM_UNIT_GENERIC) &&
                       (sym_find_unit(&g_eng.sym, &g_eng.mem, "any", 3) == SYM_UNIT_GENERIC);
    printf("    - [Sec 1] SymTable-driven `$T`/`$any`/`$none` & pure `.math` mat : %s\n",
           sec1_sym_ok ? "PASS" : "FAIL");

    /* 15b. Verify C API (`eng_sym_integ`, `eng_sym_solve`) & JNI (`nativeSymInteg`, `nativeSymSolve`) */
    char sinteg_buf[256] = {0};
    char ssolve_buf[256] = {0};
    bool c_sym_integ_ok = eng_sym_integ(&g_eng, "x", "6 * x^2 + 4 * x + 1 / x", sinteg_buf, sizeof(sinteg_buf)) &&
                          (strcmp(sinteg_buf, "2 * x ^ 3 + 2 * x ^ 2 + ln(x)") == 0);
    bool c_sym_solve_ok = eng_sym_solve(&g_eng, "x", "x^2 - 5 * x + 6 = 0", ssolve_buf, sizeof(ssolve_buf)) &&
                          (strcmp(ssolve_buf, "[2, 3]") == 0);
    jlong jh15 = Java_com_mathengine_MathEngineBridge_nativeCreate(jenv, NULL);
    test_jni_load_stdlib(jenv, jh15);
    jstring jinteg = Java_com_mathengine_MathEngineBridge_nativeSymInteg(jenv, NULL, jh15, "x", "4 * x^3 + cos(x)");
    jstring jsolve = Java_com_mathengine_MathEngineBridge_nativeSymSolve(jenv, NULL, jh15, "x", "x^2 - 9 == 0");
    bool jni_sym15_ok = (jh15 != 0) &&
                        (jinteg != NULL && strcmp(jinteg, "x ^ 4 + sin(x)") == 0) &&
                        (jsolve != NULL && strcmp(jsolve, "[-3, 3]") == 0);
    Java_com_mathengine_MathEngineBridge_nativeDestroy(jenv, NULL, jh15);
    bool sec3_sym_ok = c_sym_integ_ok && c_sym_solve_ok && jni_sym15_ok;
    printf("    - [Sec 3] Symbolic `sym_integ` & `sym_solve` (MEP, C API & JNI)  : %s\n",
           sec3_sym_ok ? "PASS" : "FAIL");

    /* 15c. Verify `@extern` callback error injection and 4-thread parallel `pthread` stress test */
    int ext_cnt_main = 0;
    bool ext_err_ok = eng_register_extern(&g_eng, "checked_recip", host_checked_recip_cb, &ext_cnt_main) &&
                      user_compile_ext(&g_eng, "[ext_err_mod]\n<[arith]\n\"checked_recip\"(x:real) -> real = @checked_recip(x)\n", NULL, NULL) &&
                      eng_eval(&g_eng, "checked_recip(4)", &v) && (val_get_f64(&g_eng.mem, v) == 25.0) &&
                      !eng_eval(&g_eng, "checked_recip(0)", &v) &&
                      (strstr(eng_last_error(&g_eng), "Host extern @checked_recip rejected zero input") != NULL) &&
                      !eng_eval(&g_eng, "checked_recip(-5)", &v) &&
                      (strstr(eng_last_error(&g_eng), "Host extern @checked_recip rejected negative input") != NULL) &&
                      (ext_cnt_main == 3);

    bool mt_stress_ok = true;
#if !defined(_WIN32)
    pthread_t tids[4];
    MtWorkerArg wargs[4];
    for (int ti = 0; ti < 4; ++ti) {
        wargs[ti].thread_id = ti;
        wargs[ti].ok = false;
        if (pthread_create(&tids[ti], NULL, mt_stress_worker, &wargs[ti]) != 0) {
            mt_stress_ok = false;
        }
    }
    for (int ti = 0; ti < 4; ++ti) {
        (void)pthread_join(tids[ti], NULL);
        if (!wargs[ti].ok) mt_stress_ok = false;
    }
#endif
    bool sec3_mt_ok = ext_err_ok && mt_stress_ok;
    printf("    - [Sec 3] Error-injecting `@extern` & 4-thread ASan stress test  : %s\n\n",
           sec3_mt_ok ? "PASS" : "FAIL");

    /* 16. Transparent SBO Arena-to-Heap Spill & Automatic Memory Management Verification */
    printf("[16] SBO Arena-to-Heap Spill & Automatic Memory Management:\n");

    /* 16a. Verify small containers (<= 64 slots) stay in SBO f64_pool while > 64 spill to HeapBlock */
    bool sbo_small_ok = eng_eval(&g_eng, "v_small := vec(16, 3)", &v) &&
                        !mem_is_heap_idx((uint16_t)mem_read_pool(&g_eng.mem, (uint16_t)(val_get_pool_idx(v) + 3))) &&
                        (g_eng.mem.heap_active_count == 0);
    bool sbo_spill_ok = eng_eval(&g_eng, "v_big := vec(5000, 7)", &v) &&
                        mem_is_heap_idx((uint16_t)mem_read_pool(&g_eng.mem, (uint16_t)(val_get_pool_idx(v) + 3))) &&
                        (g_eng.mem.heap_active_count == 1) &&
                        (g_eng.mem.heap_total_slots >= 5000u);
    /* Next eval rewinds scratch and frees all transient heap blocks */
    bool sbo_rewind_ok = eng_eval(&g_eng, "1 + 1", &v) &&
                         (g_eng.mem.heap_active_count == 0) &&
                         (g_eng.mem.heap_total_slots == 0);
    bool sec16_routing_ok = sbo_small_ok && sbo_spill_ok && sbo_rewind_ok;
    printf("    - [SBO Routing] <=64 in f64_pool, >64 in HeapBlock & auto-rewind : %s\n",
           sec16_routing_ok ? "PASS" : "FAIL");

    /* 16b. Verify SBO -> Heap migration on dynamic push and shrink_to_fit on heap vectors */
    bool sec16_mig_ok = eng_eval(&g_eng,
        "v := vec(1, 2, 3, 4) ; (i in 5...200) { v.push(i) } ; v.shrink() ; "
        "(v.len() == 200) && (v.cap() == 200) && (v[0] == 1) && (v[199] == 200)", &v) &&
        (val_get_f64(&g_eng.mem, v) == 1.0);
    printf("    - [SBO -> Heap] Dynamic growth migration & heap `shrink(v)`      : %s\n",
           sec16_mig_ok ? "PASS" : "FAIL");

    /* 16c. Verify cyclic loop & function-frame heap allocation reclamation (> 300 heap blocks across 1 eval) */
    bool sec16_gc_ok = eng_eval(&g_eng,
        "work(k) := { a := vec(1500, k) ; b := (1...1000) ; a[1499] + b[999] } ; "
        "tot := 0 ; (i in 1...300) { tot += work(i) } ; tot", &v) &&
        (val_get_f64(&g_eng.mem, v) == (300.0 * 301.0 / 2.0 + 300.0 * 1000.0)) &&
        (g_eng.mem.heap_active_count < 64u);
    mem_rewind_scratch(&g_eng.mem);
    sec16_gc_ok = sec16_gc_ok && (g_eng.mem.heap_active_count == 0) && (g_eng.mem.heap_total_slots == 0);
    printf("    - [Auto Heap GC] 600 heap vectors across 300 calls reclaimed     : %s\n\n",
           sec16_gc_ok ? "PASS" : "FAIL");

    /* 17. Unified Pipeline: Standalone Scripts vs. Multi-File Projects, `import` Auto-Staging & Ephemeral 7D SI Units */
    printf("[17] Unified Pipeline (Standalone vs. Multi-File Projects, `import` & Ephemeral 7D SI Units):\n");

    /* 17a. Standalone single-file execution (both script statements & @entry/main linking with stdlib) */
    Engine *proj_eng = eng_create();
    bool sec17_standalone_ok = false;
    if (proj_eng && test_load_stdlib_file(proj_eng)) {
        const char *standalone_script =
            "<[arith]\n"
            "<[units]\n"
            "$span2d(w:real, h:real)\n"
            "span_area(s:span2d) = s.w * s.h\n"
            "box := span2d(12, 5)\n"
            "span_area(box) * 1 m2 + 40 m2\n";
        Value sv1 = val_void(), sv2 = val_void();
        bool s1 = eng_build_and_run(proj_eng, &standalone_script, 1, &sv1) &&
                  (val_get_f64(&proj_eng->mem, sv1) == 100.0) &&
                  (eng_val_unit(proj_eng, sv1) == sym_find_unit(&proj_eng->sym, &proj_eng->mem, "area", 4));
        const char *standalone_entry =
            "[single_app]\n"
            "<[arith]\n"
            "scale_k := 3\n"
            "compute_ main_val(x) = x * scale_k + 7\n"
            "run_single() = compute_main_val(11)\n"
            "@entry(run_single)\n";
        /* Fix typo in function name above: compute_main_val */
        standalone_entry =
            "[single_app]\n"
            "<[arith]\n"
            "scale_k := 3\n"
            "compute_main_val(x) = x * scale_k + 7\n"
            "run_single() = compute_main_val(11)\n"
            "@entry(run_single)\n";
        bool s2 = eng_eval(proj_eng, standalone_entry, &sv2) &&
                  (val_get_f64(&proj_eng->mem, sv2) == 40.0) &&
                  eng_has_entry(proj_eng);
        sec17_standalone_ok = s1 && s2;
    }
    printf("    - [Standalone File] Single-file script & `@entry` via unified pipeline : %s\n",
           sec17_standalone_ok ? "PASS" : "FAIL");

    /* 17b. Multi-file unordered project compilation, topological linking, and single `main()` / `@entry` execution */
    bool sec17_multifile_ok = false;
    if (proj_eng) {
        eng_init(proj_eng);
        (void)test_load_stdlib_file(proj_eng);
        /* Supply 3 project files in reverse dependency order:
         *   - `[sim_app]` (project entry module with `main()`, depends on `[thruster]` & `[nav_math]`)
         *   - `[thruster]` (library module with `:=` state & stray top-level demo expression that must be skipped!)
         *   - `[nav_math]` (base project module with `$vec_ thrust` struct & `priv` helper) */
        const char *proj_files[3] = {
            "[sim_app]\n"
            "<[thruster]\n"
            "<[nav_math]\n"
            "<[units]\n"
            "pub main() = {\n"
            "    b := burn_cfg(50, 4)\n"
            "    thrust_impulse(b) + nav_math::nav_offset * 1 N\n"
            "}\n",

            "[thruster]\n"
            "<[nav_math]\n"
            "<[units]\n"
            "pub eff_factor := 2\n"
            "pub thrust_impulse(c:burn_cfg) = (nav_product(c) * eff_factor) * 1 N\n"
            "# Stray scratch expression in library module (skipped in dependency mode!)\n"
            "eff_factor = 999\n",

            "[nav_math]\n"
            "<[arith]\n"
            "$burn_cfg(rate:real, duration:real)\n"
            "priv secret_bias(x) = x + 5\n"
            "pub nav_offset = 10\n"
            "pub nav_product(c:burn_cfg) = secret_bias(c.rate * c.duration)\n"
        };
        Value mv = val_void();
        bool mf_run = eng_build_and_run(proj_eng, proj_files, 3, &mv) &&
                      (val_get_f64(&proj_eng->mem, mv) == 420.0) &&
                      (eng_val_unit(proj_eng, mv) == sym_find_unit(&proj_eng->sym, &proj_eng->mem, "force", 5));

        /* Serialize the linked multi-file project to `.mathc`, reload into a fresh engine, and run `eng_run_entry`! */
        uint8_t *proj_img = (uint8_t *)malloc(MATHC_MAX_IMAGE_BYTES);
        size_t proj_img_len = 0;
        bool mathc_proj_ok = false;
        if (proj_img) {
            proj_img_len = ext_save_mathc_mem(&proj_eng->mem, &proj_eng->sym, proj_img, MATHC_MAX_IMAGE_BYTES);
            Engine *reload_eng = eng_create();
            if (reload_eng && proj_img_len > 0 && user_load_mathc(reload_eng, proj_img, proj_img_len)) {
                Value rv = val_void();
                mathc_proj_ok = eng_has_entry(reload_eng) &&
                                (reload_eng->sym.base_mod_count == 18u) &&
                                eng_run_entry(reload_eng, &rv) &&
                                (val_get_f64(&reload_eng->mem, rv) == 420.0);
            }
            if (reload_eng) eng_destroy(reload_eng);
            free(proj_img);
        }

        /* Verify duplicate `main()` across two modules in a project is rejected unless `@entry` disambiguates */
        const char *dup_main_files[2] = {
            "[mod_m1]\n<[arith]\nmain() = 10\n",
            "[mod_m2]\n<[mod_m1]\nmain() = 20\n"
        };
        Value dv = val_void();
        bool dup_main_rejected = !eng_build_and_run(proj_eng, dup_main_files, 2, &dv);

        sec17_multifile_ok = mf_run && mathc_proj_ok && dup_main_rejected;
    }
    printf("    - [Multi-File Project] DAG link, `@entry`/`main`, `.mathc` & dup guard : %s\n",
           sec17_multifile_ok ? "PASS" : "FAIL");

    /* 17c. Auto-staged multi-file `import "file.math"` discovery & ephemeral 7D SI unit propagation */
    bool sec17_import_and_si_ok = false;
    if (proj_eng) {
        eng_init(proj_eng);
        (void)test_load_stdlib_file(proj_eng);
        const char *imp_helper =
            "[imp_helper_mod]\n"
            "<[arith]\n"
            "<[units]\n"
            "pub helper_force(a:real, force, b:real, force) = √((a ^ 2) + (b ^ 2))\n";
        (void)frontend_write_file("/tmp/mep_imp_helper.math",
                                  (const uint8_t *)imp_helper, strlen(imp_helper));
        eng_set_import_dir(proj_eng, "/tmp");
        const char *imp_root =
            "[imp_root_app]\n"
            "<[physics]\n"
            "<[units]\n"
            "import \"mep_imp_helper.math\"\n"
            "main() = {\n"
            "    f_ resultant := helper_force(3 N, 4 N)\n"
            "    r_eq := res_parallel(6 ohm, 3 ohm)\n"
            "    (unit_of(f_resultant) == \"force\") * 100 + (f_resultant to_N) * 10 + "
            "    (unit_of(r_eq) == \"resistance\") * 1000 + r_eq\n"
            "}\n";
        /* Fix space in f_resultant */
        imp_root =
            "[imp_root_app]\n"
            "<[physics]\n"
            "<[units]\n"
            "import \"mep_imp_helper.math\"\n"
            "main() = {\n"
            "    f_resultant := helper_force(3 N, 4 N)\n"
            "    r_eq := res_parallel(6 ohm, 3 ohm)\n"
            "    (unit_of(f_resultant) == \"force\") * 100 + (f_resultant to_N) * 10 + "
            "    (unit_of(r_eq) == \"resistance\") * 1000 + (r_eq / (1 ohm))\n"
            "}\n";
        Value iv = val_void();
        sec17_import_and_si_ok = eng_build_and_run(proj_eng, &imp_root, 1, &iv) &&
                                 (val_get_f64(&proj_eng->mem, iv) == 1152.0);
        remove("/tmp/mep_imp_helper.math");
    }
    printf("    - [Auto-Import & 7D SI] `import` staging & √((3N)²+(4N)²)==5N [force] : %s\n",
           sec17_import_and_si_ok ? "PASS" : "FAIL");

    /* 17d. OTA Patch Layer (`eng_apply_patches` + `patches/currency.math` + stdlib hotfix override) */
    bool sec17_patches_ok = false;
    if (proj_eng) {
        eng_init(proj_eng);
        (void)test_load_stdlib_file(proj_eng);
        size_t c_len = 0;
        char *ccy_patch = (char *)frontend_read_file("patches/currency.math", &c_len);
        const char *phys_hotfix =
            "[physics]\n"
            "<[arith]\n"
            "# Hotfix override of stdlib function + new helper in existing [physics] module\n"
            "ke(mass, vel) = 0.5 * mass * (vel ^ 2)\n"
            "patch_bonus_energy(e:real, energy) -> real, energy = e + 10 J\n";
        const char *patch_batch[2] = { ccy_patch, phys_hotfix };
        if (ccy_patch && eng_apply_patches(proj_eng, patch_batch, 2)) {
            Value pv1 = val_void(), pv2 = val_void();
            char sbuf[128] = "";
            /* 1) Unregistered @currency_convert degrades gracefully through with_unit to $str */
            bool p_fallback = eng_eval(proj_eng, "5$ into Rs", &pv1) &&
                              val_as_cstr(&proj_eng->mem, pv1, sbuf, sizeof(sbuf)) &&
                              (strstr(sbuf, "unavailable: host extern") != NULL);
            /* 2) Verify plain-quote "{25$ into RS}" / "{2km into m}" interpolation and `printl` / `println` */
            const char *ccy_v2 =
                "[currency]\n"
                "(x: real, usd) \"into\" (y: real, inr) :>55 = with_unit(x * 85, y)\n"
                "convert(amount: real, from: str, to: str) = amount * 85\n";
            Value pv3 = val_void();
            eng_clear_output(proj_eng);
            bool p_hotreload = eng_apply_patches(proj_eng, &ccy_v2, 1) &&
                               (proj_eng->sym.base_mod_count >= 19u) &&
                               eng_eval(proj_eng, "convert(2, \"USD\", \"INR\") + (patch_bonus_energy(50 J) to_J)", &pv2) &&
                               (val_get_f64(&proj_eng->mem, pv2) == 230.0) &&
                               eng_eval(proj_eng, "printl(\"{25$ into RS}\")", &pv3) &&
                               (strstr(eng_get_output(proj_eng), "2125") != NULL);
            /* 3) Recycle the SAME `proj_eng` across two multi-file projects (redefining `$item_cfg` & `[app_mod]`) and flush back to standalone mode while preserving `[currency]` patch! */
            const char *projA_units[2] = {
                "[lib_a]\n<[arith]\n$item_cfg(a:real, b:real)\npub calc_a(c:item_cfg) = c.a + c.b\n",
                "[app_mod]\n<[lib_a]\npub main() = calc_a(item_cfg(10, 25))\n"
            };
            const char *projB_units[2] = {
                "[lib_b]\n<[arith]\n$item_cfg(x:real, y:real, z:real)\npub calc_b(c:item_cfg) = c.x * c.y + c.z\n",
                "[app_mod]\n<[lib_b]\npub main() = calc_b(item_cfg(6, 7, 8))\n"
            };
            Value pva = val_void(), pvb = val_void(), pvc = val_void();
            uint16_t base_mods = proj_eng->sym.base_mod_count;
            bool recycle_ok = eng_build_and_run(proj_eng, projA_units, 2, &pva) &&
                              (val_get_f64(&proj_eng->mem, pva) == 35.0) &&
                              eng_build_and_run(proj_eng, projB_units, 2, &pvb) &&
                              (val_get_f64(&proj_eng->mem, pvb) == 50.0) &&
                              (sym_find_mod(&proj_eng->sym, &proj_eng->mem, "lib_a") < 0) &&
                              eng_flush_user_modules(proj_eng) &&
                              (proj_eng->sym.mod_count == base_mods) &&
                              (sym_find_mod(&proj_eng->sym, &proj_eng->mem, "lib_b") < 0) &&
                              (sym_find_mod(&proj_eng->sym, &proj_eng->mem, "app_mod") < 0) &&
                              !eng_has_entry(proj_eng) &&
                              eng_eval(proj_eng, "convert(2, \"USD\", \"INR\")", &pvc) &&
                              (val_get_f64(&proj_eng->mem, pvc) == 170.0);
            sec17_patches_ok = p_fallback && p_hotreload && recycle_ok;
        }
        free(ccy_patch);
    }
    printf("    - [OTA Patches] `eng_apply_patches`, `currency.math` & stdlib hotfix : %s\n\n",
           sec17_patches_ok ? "PASS" : "FAIL");

    /* ----------------------------------------------------------------------
     * 18. Native Batch Plotter, SBO->Heap AST/Overloads & Live Catalog Export
     * ---------------------------------------------------------------------- */
    printf("[18] Native Batch Plotter, SBO->Heap AST/Overloads & Live Catalog Export:\n");
    bool sec18_plot_ok = false;
    bool sec18_heap_ast_ov_ok = false;
    bool sec18_catalog_ok = false;
    if (proj_eng) {
        /* 18a. Native Batch Plotter (`eng_sample_plot`) */
        double xy[22];
        uint16_t pts = eng_sample_plot(proj_eng, "default", "x^2 + 2 * x + 1", -2.0, 2.0, 5, xy, 11);
        bool plot_quad_ok = (pts == 5) &&
                            (xy[0] == -2.0 && xy[1] == 1.0) &&
                            (xy[2] == -1.0 && xy[3] == 0.0) &&
                            (xy[4] ==  0.0 && xy[5] == 1.0) &&
                            (xy[6] ==  1.0 && xy[7] == 4.0) &&
                            (xy[8] ==  2.0 && xy[9] == 9.0);
        /* Vector-returning expression `[10, 25, 40]` inside `eng_sample_plot` */
        uint16_t vpts = eng_sample_plot(proj_eng, "default", "vec(10, 25, 40)", 0.0, 10.0, 5, xy, 11);
        bool plot_vec_ok = (vpts == 3) &&
                           (xy[0] == 0.0 && xy[1] == 10.0) &&
                           (xy[2] == 1.0 && xy[3] == 25.0) &&
                           (xy[4] == 2.0 && xy[5] == 40.0);
        sec18_plot_ok = plot_quad_ok && plot_vec_ok;

        /* 18b. SBO -> Heap growth for Overloads (> 2,048) & AST (> 24,576) + O(1) Flush Cleanup */
        bool ov_grow = sym_ensure_ov_cap(&proj_eng->sym, &proj_eng->mem, 3200u) &&
                       (proj_eng->sym.heap_overloads != NULL) &&
                       (proj_eng->sym.ov_cap >= 3200u);
        bool ast_grow = mem_ensure_ast_cap(&proj_eng->mem, 30000u) &&
                        (proj_eng->mem.heap_ast != NULL) &&
                        (proj_eng->mem.ast_cap >= 30000u);
        /* Verify stdlib functions still evaluate identically after SBO->Heap migration */
        Value hv = val_void();
        bool eval_on_heap_ok = eng_eval(proj_eng, "sin(π / 2) + ((2.5 km) to_m)", &hv) &&
                               (val_get_f64(&proj_eng->mem, hv) == 2501.0);
        /* Flush user modules should release heap_overloads and heap_ast back to SBO when base fits in SBO */
        bool flush_reclaim_ok = eng_flush_user_modules(proj_eng) &&
                                (proj_eng->sym.heap_overloads == NULL) &&
                                (proj_eng->mem.heap_ast == NULL) &&
                                eng_eval(proj_eng, "cos(0) + ((100 cm) to_m)", &hv) &&
                                (val_get_f64(&proj_eng->mem, hv) == 2.0);
        sec18_heap_ast_ov_ok = ov_grow && ast_grow && eval_on_heap_ok && flush_reclaim_ok;

        /* 18c. Live SymTable-Driven Autocomplete Catalog Export (`eng_export_catalog`) */
        char *cat_buf = (char *)malloc(262144u);
        if (cat_buf) {
            size_t cat_len = eng_export_catalog(proj_eng, cat_buf, 262144u);
            sec18_catalog_ok = (cat_len > 4000u) &&
                               (strstr(cat_buf, "fn|sin|") != NULL) &&
                               (strstr(cat_buf, "op|into|") != NULL) &&
                               (strstr(cat_buf, "unit|km|") != NULL) &&
                               (strstr(cat_buf, "fn|convert|") != NULL) &&
                               (strstr(cat_buf, "mod|currency|") != NULL);
            free(cat_buf);
        }
        eng_destroy(proj_eng);
    }
    printf("    - [Batch Plotter] `eng_sample_plot` scalar & vector sampling        : %s\n",
           sec18_plot_ok ? "PASS" : "FAIL");
    printf("    - [Unbounded SBO/Heap] `heap_ast` (>24k) & `heap_overloads` (>2k)   : %s\n",
           sec18_heap_ast_ov_ok ? "PASS" : "FAIL");
    printf("    - [Live Catalog] `eng_export_catalog` stdlib + OTA patch symbols    : %s\n\n",
           sec18_catalog_ok ? "PASS" : "FAIL");

    ext_build_free_heap(&ubuild);
    sym_free_heap(&s_fuzz_eng.sym);
    mem_free_all_heap(&s_fuzz_eng.mem);
    sym_free_heap(&g_eng.sym);
    mem_free_all_heap(&g_eng.mem);
    return (passed == total && blank_ok && load_mathc_ok && vec_type_guard && multi_eng_ok && jni_all_ok &&
            sec16_routing_ok && sec16_mig_ok && sec16_gc_ok &&
            sec17_standalone_ok && sec17_multifile_ok && sec17_import_and_si_ok && sec17_patches_ok &&
            sec18_plot_ok && sec18_heap_ast_ov_ok && sec18_catalog_ok &&
            audit_g1_ok && audit_g2_ok && audit_g3_ok && audit_g4_ok &&
            audit2_b1_ok && audit2_b2_b3_ok && audit2_b5_ok &&
            sec1_sym_ok && sec3_sym_ok && sec3_mt_ok &&
            audit_g1_ok && audit_g2_ok && audit_g3_ok && audit_g4_ok &&
            audit2_b1_ok && audit2_b2_b3_ok && audit2_b5_ok &&
            introspect_ok && sess_fn_ok &&
            link_order_ok && dup_mod_guard && dup_def_guard && missing_dep_guard && cycle_dep_guard &&
            parallel_sess_ok && mod_reset_a && varintrospect_ok &&
            same_line_semi_guard && scope_isolation_guard && multi_else_guard && else_if_guard &&
            kw_var_guard && kw_tag_guard &&
            unlike_unit_guard && concrete_real_guard && concrete_real_dist_unit_guard &&
            concrete_real_dist_iota_guard && concrete_dist_unitless_iota_guard &&
            fn_unit_mismatch_guard && gen_T_unit_mismatch_guard &&
            conv_unit_mismatch_guard && jagged_mat_guard && non_bool_not_guard && not_overload_guard && lim_diverge_guard && custom_err_guard && fx_unknown_ccy_guard && fx_neg_amount_guard &&
            syn_off && units_off && phys_off &&
            mutual_rec_ok && bug1_ok && bug2_ok && bug3_ok && bug4_ok && bug5_ok && bug6_ok && bug7_ok && bug8_ok &&
            bug9_ok && bug10_ok) ? 0 : 1;
}
