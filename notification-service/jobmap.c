#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "jobmap.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    char job_id[37]; /* uuid + NUL */
    int64_t chat_id;
    bool used;
} jobmap_entry_t;

static jobmap_entry_t g_entries[JOBMAP_CAPACITY];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Scan for `job_id`. Returns the matching slot index with *found set, or the
 * first free slot index with *found clear, or JOBMAP_CAPACITY when the map is
 * full.
 */
static size_t find_slot(const char* job_id, bool* found) {
    *found = false;
    size_t first_free = JOBMAP_CAPACITY;
    for (size_t i = 0; i < JOBMAP_CAPACITY; i++) {
        if (!g_entries[i].used) {
            if (first_free == JOBMAP_CAPACITY)
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

void jobmap_put(const char* job_id, int64_t chat_id) {
    if (!job_id || job_id[0] == '\0')
        return;

    pthread_mutex_lock(&g_lock);
    bool found = false;
    size_t i = find_slot(job_id, &found);
    if (found) {
        g_entries[i].chat_id = chat_id;
    } else if (i < JOBMAP_CAPACITY) {
        strncpy(g_entries[i].job_id, job_id, sizeof(g_entries[i].job_id) - 1);
        g_entries[i].job_id[sizeof(g_entries[i].job_id) - 1] = '\0';
        g_entries[i].chat_id = chat_id;
        g_entries[i].used = true;
    }
    /* Map full: silently drop. */
    pthread_mutex_unlock(&g_lock);
}

int64_t jobmap_get(const char* job_id) {
    if (!job_id || job_id[0] == '\0')
        return 0;

    pthread_mutex_lock(&g_lock);
    bool found = false;
    size_t i = find_slot(job_id, &found);
    int64_t chat_id = found ? g_entries[i].chat_id : 0;
    pthread_mutex_unlock(&g_lock);
    return chat_id;
}

void jobmap_remove(const char* job_id) {
    if (!job_id || job_id[0] == '\0')
        return;

    pthread_mutex_lock(&g_lock);
    bool found = false;
    size_t i = find_slot(job_id, &found);
    if (found)
        g_entries[i].used = false;
    pthread_mutex_unlock(&g_lock);
}
