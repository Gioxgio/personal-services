#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "jobset.h"

#include <pthread.h>
#include <string.h>

typedef struct {
    char job_id[37]; /* uuid + NUL */
    bool used;
} jobset_entry_t;

static jobset_entry_t g_entries[JOBSET_CAPACITY];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Scan for `job_id`. Returns the matching slot index with *found set, or the
 * first free slot index with *found clear, or JOBSET_CAPACITY when the set is
 * full.
 */
static size_t find_slot(const char* job_id, bool* found) {
    *found = false;
    size_t first_free = JOBSET_CAPACITY;
    for (size_t i = 0; i < JOBSET_CAPACITY; i++) {
        if (!g_entries[i].used) {
            if (first_free == JOBSET_CAPACITY)
                first_free = i;
            continue;
        }
        if (strcmp(g_entries[i].job_id, job_id) == 0) {
            *found = true;
            return i;
        }
    }
    return first_free;
}

void jobset_add(const char* job_id) {
    if (!job_id || job_id[0] == '\0')
        return;

    pthread_mutex_lock(&g_lock);
    bool found = false;
    size_t i = find_slot(job_id, &found);
    if (!found && i < JOBSET_CAPACITY) {
        strncpy(g_entries[i].job_id, job_id, sizeof(g_entries[i].job_id) - 1);
        g_entries[i].job_id[sizeof(g_entries[i].job_id) - 1] = '\0';
        g_entries[i].used = true;
    }
    /* Already present or set full: nothing to do. */
    pthread_mutex_unlock(&g_lock);
}

bool jobset_contains(const char* job_id) {
    if (!job_id || job_id[0] == '\0')
        return false;

    pthread_mutex_lock(&g_lock);
    bool found = false;
    (void)find_slot(job_id, &found);
    pthread_mutex_unlock(&g_lock);
    return found;
}

void jobset_remove(const char* job_id) {
    if (!job_id || job_id[0] == '\0')
        return;

    pthread_mutex_lock(&g_lock);
    bool found = false;
    size_t i = find_slot(job_id, &found);
    if (found)
        g_entries[i].used = false;
    pthread_mutex_unlock(&g_lock);
}
