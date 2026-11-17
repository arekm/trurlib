/*
  TRURLib

  libcheck helpers
*/
#ifndef TRURLIB_N_CHECK_H
#define TRURLIB_N_CHECK_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/param.h>          /* for PATH_MAX */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <check.h>
#if CHECK_MAJOR_VERSION == 0 && CHECK_MINOR_VERSION < 13
# error "n_check.h needs check 0.13.0 or later, for TTest"
#endif
#include "nassert.h"
#include "n_snprintf.h"

#define expect_int(a, b)  ck_assert_int_eq(a, b)
#define expect_str(a, b)  ck_assert_str_eq(a, b)
#define expect_null(a)       ck_assert_ptr_null(a)
#define expect_notnull(a)    ck_assert_ptr_nonnull(a)

static inline char *__ncheck_make_test_tmp_path(char *path, size_t size,
                                                const char *filename)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || *dir == '\0')
        dir = getenv("TMP");

    if (dir == NULL || *dir == '\0')
        dir = "/tmp";

    snprintf(path, size, "%s/test_n_check-%ld-%s", dir, (long)getpid(),
             filename);
    path[size - 1] = '\0';
    return path;
}

/* Each invocation gets its own buffer: NTEST_TMPPATH() may be used more than
   once in one expression, and test binaries may run in parallel. */
#define NTEST_TMPPATH(name) \
    __ncheck_make_test_tmp_path((char[PATH_MAX]){0}, PATH_MAX, (name))

// Overloading Macro on Number of Arguments
// taken from https://stackoverflow.com/a/11994395
#define GET_MACRO(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, NAME, ...) NAME

static int __ncheck_verbosity = CK_ENV;
static int __ncheck_log_verbose = 0;

static inline TN_FORMAT_PRINTF(1, 2) void __ncheck_log(const char *fmt, ...)
{
    if (__ncheck_log_verbose < 2) /* enabled with -vv */
        return;

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
#define NTEST_LOG(fmt, args...)  __ncheck_log("[%s] " fmt, __FUNCTION__ , ## args)

static inline int __ncheck_runner(Suite *suite) {
    int nerr = 0;
    SRunner *sr = srunner_create(suite);
    srunner_run_all(sr, __ncheck_verbosity);
    nerr += srunner_ntests_failed(sr);
    srunner_free(sr);

    return (nerr == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static inline void __ncheck_process_args(int argc, char *argv[],
                                         const TTest *const tests[]) {

    if (argc < 2)
        return;

    for( int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-l") == 0) {
            for (int n = 0; tests[n] != NULL; n++)
                printf("%s\n", tests[n]->name);

            exit(EXIT_SUCCESS);
        }

        if (strncmp(arg, "-v", 2) == 0) {
            __ncheck_verbosity = CK_VERBOSE;
            __ncheck_log_verbose++;
            arg += 2;
            while (*arg++ == 'v')
                __ncheck_log_verbose++;
        }
    }
}

static inline const char *__ncheck_test_name(const char *name) {
    if (strncmp(name, "test_", 5) == 0)
        return name + 5;

    return name;
}


static inline void __ncheck_add_tests(Suite *suite, const TTest *const tests[]) {
    for (int i = 0; tests[i] != NULL; i++) {
        TCase *tc = tcase_create(__ncheck_test_name(tests[i]->name));
        tcase_add_test(tc, tests[i]);
        suite_add_tcase(suite, tc);
    }
}

/* the tests are the names given to START_TEST(), which check makes
   pointers to the test and its name, so any number goes */
#define NTEST_RUNNER(name, ...)                               \
    int main(int argc, char *argv[]) {                        \
        const TTest *__tests[] = { __VA_ARGS__, NULL };       \
        Suite *__suite = suite_create(name);                  \
        __ncheck_add_tests(__suite, __tests);                 \
        __ncheck_process_args(argc, argv, __tests);           \
        return __ncheck_runner(__suite);                      \
    }
#endif


/* redefine (deprecated) fail_if/fail_unless with variable arguments */
#undef fail_if
#undef fail_unless

#define fail_if_1(cond) ck_assert_msg(!(cond), NULL)
#define fail_if_2p(cond, ...) ck_assert_msg(!(cond), __VA_ARGS__)

#define fail_unless_1(cond) ck_assert_msg((cond), NULL)
#define fail_unless_2p(cond, ...) ck_assert_msg((cond), __VA_ARGS__)

#define fail_if(cond, ...) \
    GET_MACRO(_0, ##__VA_ARGS__, \
        fail_if_2p, fail_if_2p, fail_if_2p, fail_if_2p, fail_if_2p, \
        fail_if_2p, fail_if_2p, fail_if_2p, fail_if_2p, fail_if_1)(cond, ##__VA_ARGS__)

#define fail_unless(cond, ...) \
    GET_MACRO(_0, ##__VA_ARGS__, \
        fail_unless_2p, fail_unless_2p, fail_unless_2p, fail_unless_2p, fail_unless_2p, \
        fail_unless_2p, fail_unless_2p, fail_unless_2p, fail_unless_2p, fail_unless_1)(cond, ##__VA_ARGS__)
