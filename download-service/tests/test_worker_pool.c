#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "worker_pool.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---------- fake yt-dlp helpers ---------- */

/* Write a fake yt-dlp into `dir` that sleeps `sleep_sec` and then writes a
 * marker file next to its -o output. Returns the script path. */
static const char* ok_script = "#!/bin/sh\n"
                               "while [ $# -gt 0 ]; do\n"
                               "  case \"$1\" in -o) shift; out=\"$1\";; esac; shift\n"
                               "done\n"
                               "dir=\"${out%/*}\"\n"
                               "mkdir -p \"$dir\"\n"
                               "sleep \"${FAKE_YTDLP_SLEEP:-0.3}\"\n"
                               "printf 'FAKE-POOL-CONTENT' > \"$dir/dummy.mp4\"\n"
                               "exit 0\n";

static const char* fail_script = "#!/bin/sh\nexit 1\n";

static bool write_fake_ytdlp(const char* dir, const char* script) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/yt-dlp", dir);
    if (n < 0 || (size_t)n >= sizeof path)
        return false;
    FILE* f = fopen(path, "w");
    if (!f)
        return false;
    fputs(script, f);
    fclose(f);
    return chmod(path, 0755) == 0;
}

static bool prepend_path(const char* dir) {
    char buf[PATH_MAX];
    const char* old = getenv("PATH");
    int n = snprintf(buf, sizeof buf, "%s:%s", dir, old ? old : "/usr/bin:/bin");
    if (n < 0 || (size_t)n >= sizeof buf)
        return false;
    return setenv("PATH", buf, 1) == 0;
}

static bool file_contains(const char* path, const char* marker) {
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    char buf[256];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    buf[n] = '\0';
    return strstr(buf, marker) != NULL;
}

/* ---------- tests ---------- */

static void test_job_create(void) {
    job_t* job = job_create(NULL, 0, "video");
    check(job == NULL, "create: NULL url rejected");

    job = job_create("", 7, "video");
    check(job == NULL, "create: empty url rejected");

    job = job_create("https://youtu.be/abc", 123, "audio");
    check(job != NULL, "create: valid job allocated");
    if (job != NULL) {
        check(strlen(job->job_id) == 36, "create: job_id is 36 chars");
        check(job->job_id[8] == '-' && job->job_id[13] == '-' && job->job_id[18] == '-' &&
                  job->job_id[23] == '-',
              "create: job_id has uuid hyphens");
        check(strcmp(job->url, "https://youtu.be/abc") == 0, "create: url stored");
        check(job->chat_id == 123, "create: chat_id stored");
        check(strcmp(job->format, "audio") == 0, "create: format stored");
        check(job->status == JOB_PENDING, "create: initial state is pending");
        job_free(job);
    }

    job = job_create("https://youtu.be/abc", 0, NULL);
    check(job != NULL, "create: NULL format defaults to video");
    if (job != NULL) {
        check(strcmp(job->format, "video") == 0, "create: default format video");
        job_free(job);
    }
}

static void test_concurrent_downloads(void) {
    const char* dir = "/tmp/download-service-pool-test";
    (void)mkdir(dir, 0755);
    if (!write_fake_ytdlp(dir, ok_script) || !prepend_path(dir)) {
        printf("FAIL: cannot stage fake yt-dlp\n");
        failed++;
        return;
    }

    worker_pool_config_t config = {.concurrency = 2,
                                   .yt_dlp_path = "yt-dlp",
                                   .storage_path = "/tmp/download-service-pool-storage"};
    check(worker_pool_start(&config) == 0, "pool: start with concurrency 2");

    job_t* jobs[4];
    for (int i = 0; i < 4; i++) {
        char url[64];
        snprintf(url, sizeof url, "https://youtu.be/job%d", i);
        jobs[i] = job_create(url, 100 + i, "video");
        check(jobs[i] != NULL, "pool: job created");
    }

    double start = now_seconds();
    for (int i = 0; i < 4; i++)
        check(worker_pool_dispatch(jobs[i]) == 0, "pool: job dispatched");

    for (int i = 0; i < 4; i++) {
        worker_pool_wait_job(jobs[i]);
        pthread_mutex_lock(&jobs[i]->lock);
        bool completed = jobs[i]->status == JOB_COMPLETED && jobs[i]->file_path[0] != '\0';
        pthread_mutex_unlock(&jobs[i]->lock);
        char msg[96];
        snprintf(msg, sizeof msg, "pool: job %d completed", i);
        check(completed, msg);
    }
    double elapsed = now_seconds() - start;

    check(worker_pool_max_running() >= 2,
          "pool: two workers ran concurrently (peak concurrency reached)");
    char msg[96];
    snprintf(msg, sizeof msg,
             "pool: 4 jobs with concurrency 2 finished in "
             "%.2fs (parallel, not serialized)",
             elapsed);
    check(elapsed < 1.5, msg);

    /* Distinct ids, files on disk under storage_path with streamed content. */
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) {
            char m[96];
            snprintf(m, sizeof m, "pool: job ids %.*s... and %.*s... differ", 8, jobs[i]->job_id, 8,
                     jobs[j]->job_id);
            check(strcmp(jobs[i]->job_id, jobs[j]->job_id) != 0, m);
        }

    for (int i = 0; i < 4; i++) {
        pthread_mutex_lock(&jobs[i]->lock);
        const char* path = jobs[i]->file_path;
        bool have = path[0] != '\0';
        pthread_mutex_unlock(&jobs[i]->lock);
        char m[96];
        snprintf(m, sizeof m, "pool: job %d file under storage path", i);
        check(have && strncmp(path, config.storage_path, strlen(config.storage_path)) == 0, m);
        check(have && file_contains(path, "FAKE-POOL-CONTENT"),
              "pool: downloaded content streamed to disk");
    }

    worker_pool_stop();

    /* find_job works after stop; unknown id returns NULL. */
    check(worker_pool_find_job(jobs[0]->job_id) != NULL, "pool: find_job by id");
    check(worker_pool_find_job("does-not-exist") == NULL, "pool: find_job unknown id is NULL");
}

static void test_failed_download(void) {
    const char* dir = "/tmp/download-service-pool-fail";
    (void)mkdir(dir, 0755);
    if (!write_fake_ytdlp(dir, fail_script) || !prepend_path(dir)) {
        printf("FAIL: cannot stage fake yt-dlp\n");
        failed++;
        return;
    }

    worker_pool_config_t config = {.concurrency = 1,
                                   .yt_dlp_path = "yt-dlp",
                                   .storage_path = "/tmp/download-service-pool-fail-storage"};
    check(worker_pool_start(&config) == 0, "pool: start with concurrency 1");

    job_t* job = job_create("https://youtu.be/bad", 55, "video");
    check(job != NULL, "pool: failing job created");
    check(worker_pool_dispatch(job) == 0, "pool: failing job dispatched");
    worker_pool_wait_job(job);

    pthread_mutex_lock(&job->lock);
    bool failed_job = job->status == JOB_FAILED && job->error[0] != '\0';
    pthread_mutex_unlock(&job->lock);
    check(failed_job, "pool: failed download ends in JOB_FAILED with error");

    worker_pool_stop();
}

static void test_serial_with_concurrency_one(void) {
    const char* dir = "/tmp/download-service-pool-serial";
    (void)mkdir(dir, 0755);
    if (!write_fake_ytdlp(dir, ok_script) || !prepend_path(dir)) {
        printf("FAIL: cannot stage fake yt-dlp\n");
        failed++;
        return;
    }

    setenv("FAKE_YTDLP_SLEEP", "0.3", 1);

    worker_pool_config_t config = {.concurrency = 1,
                                   .yt_dlp_path = "yt-dlp",
                                   .storage_path = "/tmp/download-service-pool-serial-storage"};
    check(worker_pool_start(&config) == 0, "pool: start with concurrency 1");

    job_t* job_a = job_create("https://youtu.be/serial-a", 1, "video");
    job_t* job_b = job_create("https://youtu.be/serial-b", 2, "video");
    double start = now_seconds();
    worker_pool_dispatch(job_a);
    worker_pool_dispatch(job_b);
    worker_pool_wait_job(job_a);
    worker_pool_wait_job(job_b);
    double elapsed = now_seconds() - start;

    check(elapsed > 0.45, "pool: concurrency 1 serializes downloads (two 0.3s sleeps)");
    pthread_mutex_lock(&job_a->lock);
    bool a_ok = job_a->status == JOB_COMPLETED;
    pthread_mutex_unlock(&job_a->lock);
    pthread_mutex_lock(&job_b->lock);
    bool b_ok = job_b->status == JOB_COMPLETED;
    pthread_mutex_unlock(&job_b->lock);
    check(a_ok && b_ok, "pool: both serialized jobs completed");

    worker_pool_stop();
    unsetenv("FAKE_YTDLP_SLEEP");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== worker pool: job creation ===\n");
    test_job_create();

    printf("\n=== worker pool: concurrent downloads ===\n");
    test_concurrent_downloads();

    printf("\n=== worker pool: failed download ===\n");
    test_failed_download();

    printf("\n=== worker pool: concurrency 1 serializes ===\n");
    test_serial_with_concurrency_one();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
