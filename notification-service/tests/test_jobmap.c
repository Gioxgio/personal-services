#include "jobmap.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        printf("PASS: %s\n", name);
        passed++;
    } else {
        printf("FAIL: %s\n", name);
        failed++;
    }
}

int main(void) {
    printf("=== jobmap tests ===\n");

    /* Unknown job -> 0 */
    check(jobmap_get("does-not-exist") == 0, "get: unknown job is 0");

    /* Put then get */
    jobmap_put("job-1", 42);
    check(jobmap_get("job-1") == 42, "get: returns stored chat id");

    /* Put overrides */
    jobmap_put("job-1", 99);
    check(jobmap_get("job-1") == 99, "get: put overrides chat id");

    /* Remove */
    jobmap_remove("job-1");
    check(jobmap_get("job-1") == 0, "get: removed job is 0");

    /* Multiple distinct jobs */
    jobmap_put("job-a", 10);
    jobmap_put("job-b", 20);
    jobmap_put("job-c", 30);
    check(jobmap_get("job-a") == 10, "get: job-a");
    check(jobmap_get("job-b") == 20, "get: job-b");
    check(jobmap_get("job-c") == 30, "get: job-c");

    /* NULL / empty are no-ops and never crash */
    jobmap_put(NULL, 5);
    jobmap_put("", 5);
    check(jobmap_get(NULL) == 0, "get: NULL is 0");
    check(jobmap_get("") == 0, "get: empty is 0");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
