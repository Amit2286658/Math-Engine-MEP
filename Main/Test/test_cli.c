/*
 * test_cli.c — Interactive REPL, CLI Evaluator (`-e` / `.math` projects) & Benchmark Runner
 */
#include "test_harness.h"

void print_architecture_report(const Engine *eng) {
    printf("========================================================================\n");
    printf("  ENGINE MODE : 8-Byte IEEE-754 NaN-Boxing (sizeof(Value) == 8)\n");
    printf("========================================================================\n");
    printf("  sizeof(Value)        : %5zu bytes\n", sizeof(Value));
    printf("  sizeof(AstNode)      : %5zu bytes\n", sizeof(AstNode));
    printf("  sizeof(MathcHeader)  : %5zu bytes (ABI- & OS-neutral binary header)\n",
           sizeof(MathcHeader));
    printf("  sizeof(MemArena)     : %5zu bytes (%.2f KB isolated memory module)\n",
           sizeof(MemArena), (double)sizeof(MemArena) / 1024.0);
    printf("  sizeof(SymTable)     : %5zu bytes (%.2f KB symbol/type/extern registry)\n",
           sizeof(SymTable), (double)sizeof(SymTable) / 1024.0);
    printf("  sizeof(Engine) Total : %5zu bytes (%.2f KB)\n",
           sizeof(Engine), (double)sizeof(Engine) / 1024.0);
    uint16_t active_cnt = 0;
    for (uint16_t i = 0; i < eng->sym.mod_count; ++i) {
        if (sym_mod_enabled(&eng->sym, i)) active_cnt++;
    }
    printf("  Hooked Modules       : %u (%u active, unbounded capacity)\n    -> ",
           (unsigned)eng->sym.mod_count, (unsigned)active_cnt);
    for (uint16_t i = 0; i < eng->sym.mod_count; ++i) {
        StrSlice m = sym_mod_name(&eng->sym, i);
        printf("[%.*s]%s", (int)m.len, mem_str_ptr(&eng->mem, m),
               (i + 1 < eng->sym.mod_count) ? " " : "\n");
    }
    printf("  Registered Types     : %u (%u pat elems, %u struct fields)\n",
           eng->sym.type_count - 1, eng->sym.pat_elem_count, eng->sym.field_count);
    printf("  Registered Overloads : %u / %u (%u / %u syms, %u / %u consts)\n",
           eng->sym.overload_count, (unsigned)SYM_MAX_OVERLOADS,
           eng->sym.sym_count, (unsigned)SYM_MAX_SYMS,
           eng->sym.const_count, (unsigned)SYM_MAX_CONSTS);
    printf("  Permanent Arena Used : %u / %u AST nodes, %u / %u f64 pool, %u / %u src bytes\n",
           eng->mem.perm_ast_count, (unsigned)MEM_MAX_AST_NODES,
           eng->mem.perm_pool_top, (unsigned)MEM_MAX_F64_POOL,
           eng->mem.perm_src_len, (unsigned)MEM_SRC_BUF_CAP);
    printf("  Registered Externs   : %u (@fx_rate, @fx_history, @kv_set, @kv_get, @elem_mass)\n",
           eng->sym.extern_count);
    printf("========================================================================\n\n");
}

static bool str_ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s), xlen = strlen(suffix);
    return (slen >= xlen && strcmp(s + slen - xlen, suffix) == 0);
}

int run_cli_eval(Engine *eng, int argc, char **argv) {
    char val_buf[256], unit_buf[32];

    /* 1. If all non-flag arguments are `.math` files, compile, link & run as Standalone or Multi-File Project */
    bool all_math_files = (argc > 1);
    for (int i = 1; i < argc; ++i) {
        if (!str_ends_with(argv[i], ".math")) {
            all_math_files = false;
            break;
        }
    }
    if (all_math_files) {
        size_t unit_cnt = (size_t)(argc - 1);
        char **srcs = (char **)calloc(unit_cnt, sizeof(char *));
        if (!srcs) return 1;
        for (size_t u = 0; u < unit_cnt; ++u) {
            size_t flen = 0;
            srcs[u] = (char *)frontend_read_file(argv[u + 1], &flen);
            if (!srcs[u]) {
                fprintf(stderr, "Error: could not read file '%s'\n", argv[u + 1]);
                for (size_t k = 0; k < u; ++k) free(srcs[k]);
                free(srcs);
                return 1;
            }
        }
        eng_clear_output(eng);
        Value out = val_void();
        bool ok = eng_build_and_run(eng, (const char *const *)srcs, unit_cnt, &out);
        for (size_t u = 0; u < unit_cnt; ++u) free(srcs[u]);
        free(srcs);
        if (!ok) {
            fprintf(stderr, "Error: %s\n", eng_last_error(eng));
            return 1;
        }
        const char *pout = eng_get_output(eng);
        if (pout && pout[0] != '\0') {
            printf("%s", pout);
        }
        if (eng_val_type(eng, out) != MEM_TYPE_VOID) {
            eng_format_value(eng, out, val_buf, sizeof(val_buf));
            uint8_t uid = eng_val_unit(eng, out);
            if (uid != 0) {
                eng_unit_name(eng, uid, unit_buf, sizeof(unit_buf));
                printf("%s [%s]\n", val_buf, unit_buf);
            } else {
                printf("%s\n", val_buf);
            }
        }
        return 0;
    }

    /* 2. If single `.mathc` binary image argument, load & execute linked project entry */
    if (argc == 2 && str_ends_with(argv[1], ".mathc")) {
        size_t blen = 0;
        uint8_t *bdata = frontend_read_file(argv[1], &blen);
        if (!bdata) {
            fprintf(stderr, "Error: could not read .mathc image '%s'\n", argv[1]);
            return 1;
        }
        bool ok = user_load_mathc(eng, bdata, blen);
        free(bdata);
        if (!ok) {
            fprintf(stderr, "Error: %s\n", eng_last_error(eng));
            return 1;
        }
        if (eng_has_entry(eng)) {
            Value out = val_void();
            if (!eng_run_entry(eng, &out)) {
                fprintf(stderr, "Error: %s\n", eng_last_error(eng));
                return 1;
            }
            eng_format_value(eng, out, val_buf, sizeof(val_buf));
            uint8_t uid = eng_val_unit(eng, out);
            if (uid != 0) {
                eng_unit_name(eng, uid, unit_buf, sizeof(unit_buf));
                printf("%s [%s]\n", val_buf, unit_buf);
            } else {
                printf("%s\n", val_buf);
            }
        }
        return 0;
    }

    /* 3. Otherwise evaluate sequential CLI expression strings */
    for (int i = 1; i < argc; ++i) {
        const char *expr = argv[i];
        if (strcmp(expr, "-e") == 0 || strcmp(expr, "--eval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: '%s' requires an expression argument\n", expr);
                return 1;
            }
            expr = argv[++i];
        }
        Value out;
        if (!eng_mod_eval(eng, "repl", expr, &out)) {
            fprintf(stderr, "Error: %s\n", eng_last_error(eng));
            return 1;
        }
        eng_format_value(eng, out, val_buf, sizeof(val_buf));
        uint8_t uid = eng_val_unit(eng, out);
        if (uid != 0) {
            eng_unit_name(eng, uid, unit_buf, sizeof(unit_buf));
            printf("%s [%s]\n", val_buf, unit_buf);
        } else {
            printf("%s\n", val_buf);
        }
    }
    return 0;
}

int run_cli_repl(Engine *eng) {
    char cur_mod[32] = "repl";
    printf("========================================================================\n");
    printf("  MEP Interactive Module REPL (%u modules, %u overloads, %u constants)\n",
           eng_module_count(eng), eng_overload_count(eng), eng_const_count(eng));
    printf("  Commands: :help, :vars, :use <mod>, :modules, :enable <mod>, :disable <mod>,\n");
    printf("            :diff <x> <expr>, :integ <x> <expr>, :solve <x> <eq>, :simplify <expr>,\n");
    printf("            :load <file.math>, :reset, :quit\n");
    printf("========================================================================\n");
    char line[2048];
    char val_buf[512], type_buf[32], unit_buf[32];
    while (1) {
        printf("%s> ", cur_mod);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
            line[--n] = '\0';
        }
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (*cmd == '\0') continue;

        if (strcmp(cmd, ":q") == 0 || strcmp(cmd, ":quit") == 0 ||
            strcmp(cmd, ":exit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        }
        if (strcmp(cmd, ":help") == 0 || strcmp(cmd, ":h") == 0) {
            printf("  :vars               List variables, locks, and values of the current module\n");
            printf("  :use <mod>          Switch to module <mod> (created on first use)\n");
            printf("  :compact            Run Mark-Sweep scratch pool compaction\n");
            printf("  :modules / :mods    List all loaded modules and active status\n");
            printf("  :enable <mod>       Enable module <mod> in O(1)\n");
            printf("  :disable <mod>      Disable module <mod> in O(1)\n");
            printf("  :diff <x> <expr>    Symbolically differentiate <expr> w.r.t. <x>\n");
            printf("  :integ <x> <expr>   Symbolically integrate <expr> w.r.t. <x>\n");
            printf("  :solve <x> <eq>     Symbolically solve equation <eq> for <x>\n");
            printf("  :simplify <expr>    Symbolically simplify <expr>\n");
            printf("  :load <path.math>   Compile & link a .math extension file\n");
            printf("  :reset              Reset the current module's variables & functions\n");
            printf("  :quit / :q          Exit the interactive REPL\n");
            continue;
        }
        if (strncmp(cmd, ":use ", 5) == 0) {
            const char *m = cmd + 5;
            while (*m == ' ') m++;
            if (*m != '\0' && strlen(m) < sizeof(cur_mod)) {
                snprintf(cur_mod, sizeof(cur_mod), "%s", m);
            }
            continue;
        }
        if (strcmp(cmd, ":reset") == 0) {
            eng_mod_reset(eng, cur_mod);
            printf("Module '%s' reset.\n", cur_mod);
            continue;
        }
        if (strcmp(cmd, ":vars") == 0) {
            uint16_t vc = eng_mod_var_count(eng, cur_mod);
            if (vc == 0) {
                printf("  (no variables in module '%s' yet)\n", cur_mod);
            } else {
                char dump[4096];
                size_t used = eng_mod_format_vars(eng, cur_mod, dump, sizeof(dump));
                char *p = dump, *nxt;
                while (p != NULL && *p != '\0' && (size_t)(p - dump) < used) {
                    nxt = strchr(p, '\n');
                    if (nxt != NULL) *nxt = '\0';
                    printf("  %s\n", p);
                    if (nxt == NULL) break;
                    p = nxt + 1;
                }
            }
            continue;
        }
        if (strcmp(cmd, ":compact") == 0) {
            eng_compact_scratch(eng);
            printf("Scratch pool compacted.\n");
            continue;
        }
        if (strcmp(cmd, ":mods") == 0 || strcmp(cmd, ":modules") == 0) {
            /* Simple in-test module listing from the module registry */
            uint16_t mc = eng_module_count(eng);
            printf("  %u loaded module(s)\n", mc);
            continue;
        }
        if (strncmp(cmd, ":enable ", 8) == 0 || strncmp(cmd, ":disable ", 9) == 0) {
            bool en = (strncmp(cmd, ":enable ", 8) == 0);
            const char *m = cmd + (en ? 8 : 9);
            while (*m == ' ') m++;
            if (eng_set_module(eng, m, en)) {
                printf("Module '%s' %s.\n", m, en ? "enabled" : "disabled");
            } else {
                printf("Error: %s\n", eng_last_error(eng));
            }
            continue;
        }
        if (strncmp(cmd, ":simplify ", 10) == 0) {
            char out_s[512];
            if (eng_sym_simplify(eng, cmd + 10, out_s, sizeof(out_s))) {
                printf("= %s\n", out_s);
            } else {
                printf("Error: %s\n", eng_last_error(eng));
            }
            continue;
        }
        if (strncmp(cmd, ":diff ", 6) == 0) {
            const char *p = cmd + 6;
            while (*p == ' ') p++;
            char vname[32];
            size_t vi = 0;
            while (*p != '\0' && *p != ' ' && *p != '\t' && vi + 1 < sizeof(vname)) {
                vname[vi++] = *p++;
            }
            vname[vi] = '\0';
            while (*p == ' ' || *p == '\t') p++;
            char out_s[512];
            if (vi > 0 && *p != '\0' && eng_sym_diff(eng, vname, p, out_s, sizeof(out_s))) {
                printf("= %s\n", out_s);
            } else {
                printf("Usage: :diff <var> <expr>\n");
            }
            continue;
        }
        if (strncmp(cmd, ":integ ", 7) == 0) {
            const char *p = cmd + 7;
            while (*p == ' ') p++;
            char vname[32];
            size_t vi = 0;
            while (*p != '\0' && *p != ' ' && *p != '\t' && vi + 1 < sizeof(vname)) {
                vname[vi++] = *p++;
            }
            vname[vi] = '\0';
            while (*p == ' ' || *p == '\t') p++;
            char out_s[512];
            if (vi > 0 && *p != '\0' && eng_sym_integ(eng, vname, p, out_s, sizeof(out_s))) {
                printf("= %s\n", out_s);
            } else {
                printf("Usage: :integ <var> <expr>\n");
            }
            continue;
        }
        if (strncmp(cmd, ":solve ", 7) == 0) {
            const char *p = cmd + 7;
            while (*p == ' ') p++;
            char vname[32];
            size_t vi = 0;
            while (*p != '\0' && *p != ' ' && *p != '\t' && vi + 1 < sizeof(vname)) {
                vname[vi++] = *p++;
            }
            vname[vi] = '\0';
            while (*p == ' ' || *p == '\t') p++;
            char out_s[512];
            if (vi > 0 && *p != '\0' && eng_sym_solve(eng, vname, p, out_s, sizeof(out_s))) {
                printf("= %s\n", out_s);
            } else {
                printf("Usage: :solve <var> <eq>\n");
            }
            continue;
        }
        if (strncmp(cmd, ":load ", 6) == 0) {
            const char *path = cmd + 6;
            while (*path == ' ') path++;
            size_t flen = 0;
            char *fsrc = (char *)frontend_read_file(path, &flen);
            if (!fsrc) {
                printf("Error: could not read file '%s'\n", path);
                continue;
            }
            if (user_compile_ext(eng, fsrc, NULL, NULL)) {
                printf("Loaded extension '%s' (%zu bytes).\n", path, flen);
            } else {
                printf("Compile error: %s\n", eng_last_error(eng));
            }
            free(fsrc);
            continue;
        }

        Value out;
        if (!eng_mod_eval(eng, cur_mod, cmd, &out)) {
            printf("Error: %s\n", eng_last_error(eng));
            continue;
        }
        eng_format_value(eng, out, val_buf, sizeof(val_buf));
        eng_type_name(eng, eng_val_type(eng, out), type_buf, sizeof(type_buf));
        uint8_t uid = eng_val_unit(eng, out);
        if (uid != 0) {
            eng_unit_name(eng, uid, unit_buf, sizeof(unit_buf));
            printf("= %s : %s [%s]\n", val_buf, type_buf, unit_buf);
        } else {
            printf("= %s : %s\n", val_buf, type_buf);
        }

        /* Bind `ans` as a module variable for convenient follow-up calculations */
        uint16_t out_tid = eng_val_type(eng, out);
        if (out_tid != MEM_TYPE_VOID && out_tid != eng->mem.fn_type_id) {
            (void)eng_mod_set_var(eng, cur_mod, "ans", out, 0);
        }
    }
    return 0;
}

int run_cli_bench(Engine *eng) {
    printf("========================================================================\n");
    printf("  MEP ENGINE THROUGHPUT BENCHMARK\n");
    printf("========================================================================\n");
    Value out;
    /* Benchmark the actual O(1) stdlib boot: blank engine + `user_load_mathc`
     * on a pre-slurped image (no disk I/O inside the loop). */
    static size_t bench_stdlib_len = 0;
    static uint8_t *bench_stdlib = NULL;
    size_t blen = 0;
    uint8_t *bdata = NULL;
    static bool bench_stdlib_ready = false;
    if (!bench_stdlib_ready) {
        const char *bpaths[] = {
#if defined(MEP_TEST_STDLIBC_PATH)
            MEP_TEST_STDLIBC_PATH,
#endif
            "precompiled/stdlib.mathc", "Main/precompiled/stdlib.mathc", "../precompiled/stdlib.mathc", NULL
        };
        for (int i = 0; bpaths[i]; ++i) {
            bdata = frontend_read_file(bpaths[i], &blen);
            if (bdata) break;
        }
        if (!bdata) return 1;
        bench_stdlib_ready = true;
        bench_stdlib = bdata;
        bench_stdlib_len = blen;
    }
    clock_t t0 = clock();
    const int boot_iters = 5000;
    for (int i = 0; i < boot_iters; ++i) {
        eng_init(eng);
        (void)user_load_mathc(eng, bench_stdlib, bench_stdlib_len);
    }
    double boot_ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    printf("  [1] O(1) .mathc stdlib boot (%d iters)     : %7.2f ms (%.2f us/boot)\n",
           boot_iters, boot_ms, (boot_ms * 1000.0) / boot_iters);

    register_all_host_externs(eng);
    const int eval_iters = 5000;

    t0 = clock();
    for (int i = 0; i < eval_iters; ++i) {
        (void)eng_eval(eng, "2 * sin(π / 4)^2 + 2 * cos(π / 4)^2 + √16 + 5!", &out);
    }
    double arith_ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    printf("  [2] Scalar + trig + factorial (%d evals)   : %7.2f ms (%.2f us/eval)\n",
           eval_iters, arith_ms, (arith_ms * 1000.0) / eval_iters);

    t0 = clock();
    for (int i = 0; i < eval_iters; ++i) {
        (void)eng_eval(eng, "(1...20).map(x => x^2).filter(x => x % 2 == 0).sum()", &out);
    }
    double hof_ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    printf("  [3] Vector HOF map/filter/sum (%d evals)   : %7.2f ms (%.2f us/eval)\n",
           eval_iters, hof_ms, (hof_ms * 1000.0) / eval_iters);

    t0 = clock();
    for (int i = 0; i < eval_iters; ++i) {
        (void)eng_eval(eng, "linsolve([[2, 1, -1], [-3, -1, 2], [-2, 1, 2]], [8, -11, -3])", &out);
    }
    double lin_ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    printf("  [4] 3x3 Gaussian linsolve (%d evals)       : %7.2f ms (%.2f us/eval)\n",
           eval_iters, lin_ms, (lin_ms * 1000.0) / eval_iters);

    t0 = clock();
    for (int i = 0; i < 1000; ++i) {
        (void)eng_eval(eng, "ode_rk4(t, y, y, 0, 1, 1, 50)", &out);
    }
    double ode_ms = 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    printf("  [5] 50-step RK4 ODE solver (1000 evals)      : %7.2f ms (%.2f us/eval)\n",
           ode_ms, (ode_ms * 1000.0) / 1000.0);
    printf("========================================================================\n");
    return 0;
}

