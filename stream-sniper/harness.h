/*
 * tests/harness.h - the whole test framework: two checks
 * and an exit code, no dependencies beyond the C library.
 *
 * Each test binary is a single translation unit that
 * includes this header, runs its checks inside main()
 * and returns harness_exit().
 */

#ifndef TESTS_HARNESS_H
#define TESTS_HARNESS_H

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failed;

/* Fails when cond is false; the condition is printed verbatim. */
#define CHECK(cond) do {                                          \
    g_checks++;                                                     \
    if (!(cond)) {                                                  \
        g_failed++;                                                 \
        fprintf(stderr, "FAIL %s:%d: %s\n",                \
                __FILE__, __LINE__, #cond);                       \
    }                                                               \
} while (0)

/* Fails unless got and want are the same string (either may be NULL). */
#define CHECK_STR_EQ(got, want) do {                            \
    g_checks++;                                                     \
    const char *_got = (got);                                     \
    const char *_want = (want);                                   \
    if (_got == NULL || _want == NULL ||                        \
        strcmp(_got, _want) != 0) {                             \
        g_failed++;                                                 \
        fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", \
                __FILE__, __LINE__, #got,                       \
                _got ? _got : "(null)",                         \
                _want ? _want : "(null)");                      \
    }                                                               \
} while (0)

/*
 * Print the run's summary and return the process exit
 * code: 0 when every check passed, 1 otherwise.
 */
static int harness_exit(const char *name)
{
    if (g_failed > 0) {
        fprintf(stderr, "%s: %d of %d check(s) FAILED\n",
                name, g_failed, g_checks);
        return 1;
    }
    printf("%s: %d check(s) passed\n", name, g_checks);
    return 0;
}

#endif /* TESTS_HARNESS_H */
