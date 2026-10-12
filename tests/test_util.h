/*
 * Minimal assertion harness for stlpreview's unit tests.
 *
 * No framework dependency: a failing check prints file:line and marks its test
 * as failed, and main() returns EXIT_FAILURE so CTest reports the failure.
 * Include this after glib.h -- T_EQ_STR uses g_strcmp0().
 */
#ifndef STLVIEW_TEST_UTIL_H
#define STLVIEW_TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int t_tests, t_failed, t_cur_failed;

/* Run one test function and report its result. */
#define T_RUN(fn)                                                \
    do {                                                         \
        t_cur_failed = 0;                                        \
        fn();                                                    \
        t_tests++;                                               \
        if (t_cur_failed)                                        \
            t_failed++;                                          \
        printf("%-44s %s\n", #fn, t_cur_failed ? "FAIL" : "ok"); \
    } while (0)

#define T_REPORT()                                                  \
    (printf("\n%d/%d tests passed\n", t_tests - t_failed, t_tests), \
     t_failed ? EXIT_FAILURE : EXIT_SUCCESS)

#define T_FAILF(fmt, ...)                                           \
    do {                                                            \
        t_cur_failed = 1;                                           \
        fprintf(stderr, "    %s:%d: " fmt "\n", __FILE__, __LINE__, \
                __VA_ARGS__);                                       \
    } while (0)

#define T_CHECK(cond)                      \
    do {                                   \
        if (!(cond))                       \
            T_FAILF("expected %s", #cond); \
    } while (0)

#define T_EQ_INT(got, want)                                 \
    do {                                                    \
        long g_ = (long)(got), w_ = (long)(want);           \
        if (g_ != w_)                                       \
            T_FAILF("%s: got %ld, want %ld", #got, g_, w_); \
    } while (0)

#define T_EQ_STR(got, want)                                                  \
    do {                                                                     \
        const char *g_ = (got), *w_ = (want);                                \
        if (g_strcmp0(g_, w_) != 0)                                          \
            T_FAILF("%s: got \"%s\", want \"%s\"", #got, g_ ? g_ : "(null)", \
                    w_ ? w_ : "(null)");                                     \
    } while (0)

#define T_NEAR(got, want, eps)                                   \
    do {                                                         \
        double g_ = (double)(got), w_ = (double)(want);          \
        if (fabs(g_ - w_) > (double)(eps))                       \
            T_FAILF("%s: got %g, want %g (+/-%g)", #got, g_, w_, \
                    (double)(eps));                              \
    } while (0)

#endif /* STLVIEW_TEST_UTIL_H */
