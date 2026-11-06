/*
 * test_harness.h — Shared declarations for the Math-Engine-MEP test suite & CLI runner
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include "math_engine.h"
#include "user_loader.h"
#include "math_engine_jni.h"
#include "delegate.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif

typedef struct {
    const char *expr;
    const char *expected_val;
    const char *expected_type;
    const char *desc;
} TestCase;

#if !defined(_WIN32)
typedef struct {
    int  thread_id;
    bool ok;
} MtWorkerArg;
void *mt_stress_worker(void *arg);
#endif

extern Engine g_eng;
extern const TestCase k_tests[];

uint8_t *frontend_read_file(const char *path, size_t *out_len);
bool frontend_write_file(const char *path, const uint8_t *data, size_t len);
bool test_load_stdlib_file(Engine *eng);
void test_jni_load_stdlib(JNIEnv *jenv, jlong handle);
bool host_checked_recip_cb(MemArena *mem, const SymTable *tab,
                           const Value *args, uint8_t argc,
                           void *user_ctx, Value *out);
void register_all_host_externs(Engine *eng);

void print_architecture_report(const Engine *eng);
int run_cli_eval(Engine *eng, int argc, char **argv);
int run_cli_repl(Engine *eng);
int run_cli_bench(Engine *eng);

#endif /* TEST_HARNESS_H */
