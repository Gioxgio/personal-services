#include "jobset.h"

#include <stdbool.h>
#include <stdio.h>

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
    printf("=== jobset tests ===\n");

    check(!jobset_contains("does-not-exist"), "contains: unknown job is false");

    jobset_add("job-1");
    check(jobset_contains("job-1"), "contains: added job is tracked");

    jobset_add("job-1");
    check(jobset_contains("job-1"), "contains: adding twice is harmless");

    jobset_remove("job-1");
    check(!jobset_contains("job-1"), "contains: removed job is not tracked");

    jobset_add("job-a");
    jobset_add("job-b");
    jobset_add("job-c");
    check(jobset_contains("job-a"), "contains: job-a");
    check(jobset_contains("job-b"), "contains: job-b");
    check(jobset_contains("job-c"), "contains: job-c");

    jobset_add(NULL);
    jobset_add("");
    check(!jobset_contains(NULL), "contains: NULL is false");
    check(!jobset_contains(""), "contains: empty is false");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
