#ifndef WORKER_POOL_H
#define WORKER_POOL_H

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * A download job accepted by the download-service.
 *
 * The API layer (POST /download) creates a job with job_create() and hands it
 * to the pool with worker_pool_dispatch(). A worker thread downloads the
 * content with yt-dlp, streaming it straight to disk under the configured
 * storage path, and updates the job in place: COMPLETED jobs carry file_path,
 * FAILED jobs carry error. The engine retains completed jobs in a registry so
 * the callback layer can look a job up by id with worker_pool_find_job().
 *
 * All fields that can change after dispatch (status, file_path, error, done)
 * are guarded by job->lock; callers must hold it when reading them.
 */
typedef enum {
    JOB_PENDING,   /* accepted, waiting for a free worker */
    JOB_RUNNING,   /* a worker is downloading it right now */
    JOB_COMPLETED, /* download finished; file_path holds the on-disk path */
    JOB_FAILED     /* download failed; error holds a message */
} job_status_t;

struct job {
    char job_id[37];     /* uuid + NUL */
    char url[4096 + 1];  /* request url */
    char format[16 + 1]; /* "audio" or "video" */
    int chat_id;         /* originating chat, echoed in the callback */
    job_status_t status;
    char file_path[PATH_MAX];  /* set when status == JOB_COMPLETED */
    char error[256];           /* set when status == JOB_FAILED */
    bool done;                 /* true once the terminal state was stored */
    pthread_mutex_t lock;      /* guards status/file_path/error/done */
    pthread_cond_t cond;       /* broadcast when done flips to true */
    struct job* queue_next;    /* engine-owned: queue linkage */
    struct job* registry_next; /* engine-owned: registry linkage */
};
typedef struct job job_t;

/* Pool configuration. concurrency must be >= 1. */
typedef struct {
    unsigned int concurrency; /* max parallel downloads (download.concurrency) */
    const char* yt_dlp_path;  /* path to the yt-dlp executable */
    const char* storage_path; /* downloads are streamed to disk here */
    const char* callback_url; /* download.callback.url; empty disables callbacks */
} worker_pool_config_t;

/*
 * Spawn `concurrency` worker threads and create storage_path if missing.
 * Returns 0 on success, -1 on failure. Calling twice fails.
 */
int worker_pool_start(const worker_pool_config_t* config);

/* Tell workers to stop once the queue drains, join them, release resources.
 * Jobs still queued at shutdown remain pending. Safe to call on a pool that
 * was never started. */
void worker_pool_stop(void);

/*
 * Hand `job` to the pool: a worker downloads it and updates the job in place.
 * Returns 0 on success, -1 if the pool is not running. The engine never frees
 * `job`; look up results later with worker_pool_find_job().
 */
int worker_pool_dispatch(job_t* job);

/* Allocate and initialize a job in the JOB_PENDING state. Returns NULL on
 * allocation failure; the caller owns the returned job until it is dispatched
 * (job_free) or hands it to the pool (the engine keeps it). */
job_t* job_create(const char* url, int chat_id, const char* format);

/* Release a job created with job_create() that was never dispatched. Safe on
 * NULL. Must not be called on a dispatched job while the pool still owns it. */
void job_free(job_t* job);

/* Look up a dispatched job by id. Returns NULL when unknown. The returned job
 * stays owned by the engine; synchronize on job->lock before reading its
 * fields. */
job_t* worker_pool_find_job(const char* job_id);

/* Block until the job reaches a terminal state. Safe to call multiple times. */
void worker_pool_wait_job(job_t* job);

/* Largest number of jobs observed running at the same time (i.e. the observed
 * peak concurrency). Mostly useful for tests. */
unsigned int worker_pool_max_running(void);

#endif /* WORKER_POOL_H */
