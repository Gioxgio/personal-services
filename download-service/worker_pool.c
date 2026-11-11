#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "worker_pool.h"
#include "callback.h"
#include "downloader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- helpers ---------- */

/* Create every component of `path` that does not yet exist. */
static bool mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return false;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

/* Fill `out` (capacity `cap`) with a random uuid: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx. */
static bool make_uuid(char *out, size_t cap) {
    if (cap < 37)
        return false;
    FILE *f = fopen("/dev/urandom", "re");
    if (!f)
        return false;
    unsigned char b[16];
    bool ok = fread(b, 1, sizeof b, f) == sizeof b;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        return false;
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40); /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80); /* variant 10 */
    int n = snprintf(out, cap,
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                     "%02x%02x%02x%02x%02x%02x",
                     b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                     b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return n > 0 && (size_t)n < cap;
}

/* ---------- global engine state ---------- */

/* The queue is guarded by g_queue_mutex; workers sleep on g_queue_cond. */
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;
static job_t *g_queue_head = NULL;
static job_t *g_queue_tail = NULL;

/* Registry of every dispatched job, keyed implicitly (lookup scans by id).
 * Guarded by g_registry_mutex. Jobs stay in the registry until the process
 * exits, so results remain readable after completion. */
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static job_t *g_registry = NULL;

static pthread_t *g_workers = NULL;
static unsigned int g_worker_count = 0;
static volatile bool g_stop = false;
static bool g_started = false;

static worker_pool_config_t g_config;

/* g_running / g_max_running are guarded by g_queue_mutex. */
static unsigned int g_running = 0;
static unsigned int g_max_running = 0;

/*
 * Context handed to the per-job callback thread. Fields are snapshotted from
 * the job under its lock once it reaches a terminal state, so the thread can
 * POST the result without contending on the job or touching engine state.
 */
typedef struct {
    const char *callback_url;
    char job_id[37];       /* uuid + NUL */
    int chat_id;           /* originating chat, echoed in the callback */
    char status[10];       /* "completed" or "failed" */
    char file_path[PATH_MAX]; /* set when status == completed */
    char error[256];            /* set when status == failed */
} callback_ctx_t;

/* POST the download result for one finished job, then release the context.
 * Runs on a detached thread so a slow notification service never holds a
 * download worker. */
static void *callback_thread(void *arg) {
    callback_ctx_t *ctx = arg;
    if (!callback_send(ctx->callback_url, ctx->job_id, ctx->chat_id,
                       ctx->status, ctx->file_path, ctx->error)) {
        fprintf(stderr, "download-service: callback for job %s failed\n",
                ctx->job_id);
    }
    free(ctx);
    return NULL;
}

/* ---------- worker ---------- */

/* Download `job` with yt-dlp, streaming straight to disk under storage_path.
 * yt-dlp writes the media file itself; this process never holds file content
 * in memory, so multi-gigabyte downloads are fine. */
static void run_job(job_t *job) {
    pthread_mutex_lock(&job->lock);
    job->status = JOB_RUNNING;
    pthread_mutex_unlock(&job->lock);

    pthread_mutex_lock(&g_queue_mutex);
    g_running++;
    if (g_running > g_max_running)
        g_max_running = g_running;
    pthread_mutex_unlock(&g_queue_mutex);

    int rc = download_media(g_config.yt_dlp_path, g_config.storage_path,
                            job->url, job->format, job->file_path,
                            sizeof(job->file_path));

    pthread_mutex_lock(&g_queue_mutex);
    g_running--;
    pthread_mutex_unlock(&g_queue_mutex);

    pthread_mutex_lock(&job->lock);
    if (rc == 0) {
        job->status = JOB_COMPLETED;
    } else {
        job->status = JOB_FAILED;
        int n = snprintf(job->error, sizeof(job->error),
                         "download failed for %s", job->url);
        if (n < 0 || (size_t)n >= sizeof(job->error))
            job->error[sizeof(job->error) - 1] = '\0';
    }

    /* Snapshot the terminal result for the callback. A job whose download
     * failed carries an error; a completed job carries its on-disk file
     * path. The callback thread posts the result to the notification
     * service without keeping this worker occupied. */
    callback_ctx_t *ctx = NULL;
    if (g_config.callback_url && g_config.callback_url[0] != '\0') {
        ctx = calloc(1, sizeof(*ctx));
        if (ctx) {
            ctx->callback_url = g_config.callback_url;
            snprintf(ctx->job_id, sizeof(ctx->job_id), "%s", job->job_id);
            ctx->chat_id = job->chat_id;
            snprintf(ctx->status, sizeof(ctx->status), "%s",
                     job->status == JOB_COMPLETED ? "completed" : "failed");
            snprintf(ctx->file_path, sizeof(ctx->file_path), "%s",
                     job->file_path);
            snprintf(ctx->error, sizeof(ctx->error), "%s", job->error);
        }
    }

    job->done = true;
    pthread_cond_broadcast(&job->cond);
    pthread_mutex_unlock(&job->lock);

    if (ctx) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, callback_thread, ctx) == 0) {
            pthread_detach(tid);
        } else {
            free(ctx);
            fprintf(stderr,
                    "download-service: failed to spawn callback thread "
                    "for job %s\n", job->job_id);
        }
    }
}

static void *worker_loop(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_queue_mutex);
        while (!g_stop && g_queue_head == NULL)
            pthread_cond_wait(&g_queue_cond, &g_queue_mutex);

        job_t *job = g_queue_head;
        if (job != NULL) {
            g_queue_head = job->queue_next;
            if (g_queue_head == NULL)
                g_queue_tail = NULL;
        }
        pthread_mutex_unlock(&g_queue_mutex);

        if (job == NULL) /* stopped and drained */
            break;

        run_job(job);
    }
    return NULL;
}

/* ---------- public API ---------- */

int worker_pool_start(const worker_pool_config_t *config) {
    if (!config || config->concurrency == 0 || !config->yt_dlp_path ||
        !config->storage_path || !config->storage_path[0])
        return -1;
    if (g_started)
        return -1;

    if (!mkdir_p(config->storage_path))
        return -1;

    g_stop = false;
    g_config = *config;
    g_worker_count = config->concurrency;
    g_workers = calloc(g_worker_count, sizeof(*g_workers));
    if (!g_workers)
        return -1;

    for (unsigned int i = 0; i < g_worker_count; i++) {
        if (pthread_create(&g_workers[i], NULL, worker_loop, NULL) != 0) {
            /* Roll back: stop the workers created so far. */
            g_stop = true;
            pthread_cond_broadcast(&g_queue_cond);
            for (unsigned int j = 0; j < i; j++)
                pthread_join(g_workers[j], NULL);
            free(g_workers);
            g_workers = NULL;
            g_worker_count = 0;
            return -1;
        }
    }

    g_started = true;
    return 0;
}

void worker_pool_stop(void) {
    if (!g_started)
        return;

    pthread_mutex_lock(&g_queue_mutex);
    g_stop = true;
    pthread_cond_broadcast(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);

    for (unsigned int i = 0; i < g_worker_count; i++)
        pthread_join(g_workers[i], NULL);

    free(g_workers);
    g_workers = NULL;
    g_worker_count = 0;
    g_started = false;
    /* Jobs left in the queue stay pending; completed jobs stay in the
     * registry so earlier results remain readable. */
}

int worker_pool_dispatch(job_t *job) {
    if (!job || !g_started)
        return -1;

    pthread_mutex_lock(&job->lock);
    job->status = JOB_PENDING;
    job->done = false;
    pthread_mutex_unlock(&job->lock);

    pthread_mutex_lock(&g_queue_mutex);
    if (g_stop) {
        pthread_mutex_unlock(&g_queue_mutex);
        return -1;
    }
    job->queue_next = NULL;
    if (g_queue_tail != NULL)
        g_queue_tail->queue_next = job;
    else
        g_queue_head = job;
    g_queue_tail = job;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);

    pthread_mutex_lock(&g_registry_mutex);
    job->registry_next = g_registry;
    g_registry = job;
    pthread_mutex_unlock(&g_registry_mutex);

    return 0;
}

job_t *job_create(const char *url, int chat_id, const char *format) {
    if (!url || url[0] == '\0')
        return NULL;

    job_t *job = calloc(1, sizeof(*job));
    if (!job)
        return NULL;

    if (!make_uuid(job->job_id, sizeof(job->job_id))) {
        free(job);
        return NULL;
    }

    size_t url_len = strlen(url);
    if (url_len >= sizeof(job->url))
        url_len = sizeof(job->url) - 1;
    memcpy(job->url, url, url_len);
    job->url[url_len] = '\0';

    job->chat_id = chat_id;
    const char *fmt = format && format[0] ? format : "video";
    size_t fmt_len = strlen(fmt);
    if (fmt_len >= sizeof(job->format))
        fmt_len = sizeof(job->format) - 1;
    memcpy(job->format, fmt, fmt_len);
    job->format[fmt_len] = '\0';

    job->status = JOB_PENDING;
    if (pthread_mutex_init(&job->lock, NULL) != 0 ||
        pthread_cond_init(&job->cond, NULL) != 0) {
        pthread_mutex_destroy(&job->lock);
        pthread_cond_destroy(&job->cond);
        free(job);
        return NULL;
    }
    return job;
}

void job_free(job_t *job) {
    if (!job)
        return;
    pthread_mutex_destroy(&job->lock);
    pthread_cond_destroy(&job->cond);
    free(job);
}

job_t *worker_pool_find_job(const char *job_id) {
    if (!job_id || job_id[0] == '\0')
        return NULL;

    pthread_mutex_lock(&g_registry_mutex);
    for (job_t *job = g_registry; job != NULL; job = job->registry_next) {
        if (strcmp(job->job_id, job_id) == 0) {
            pthread_mutex_unlock(&g_registry_mutex);
            return job;
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return NULL;
}

void worker_pool_wait_job(job_t *job) {
    if (!job)
        return;
    pthread_mutex_lock(&job->lock);
    while (!job->done)
        pthread_cond_wait(&job->cond, &job->lock);
    pthread_mutex_unlock(&job->lock);
}

unsigned int worker_pool_max_running(void) {
    pthread_mutex_lock(&g_queue_mutex);
    unsigned int max = g_max_running;
    pthread_mutex_unlock(&g_queue_mutex);
    return max;
}
