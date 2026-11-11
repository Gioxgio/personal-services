#ifndef JOBMAP_H
#define JOBMAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Maximum number of in-flight download jobs the notification service tracks.
 * The download callback (step 6) uses this map to find the chat a job belongs
 * to.
 */
#define JOBMAP_CAPACITY 256

/*
 * Thread-safe job_id -> chat_id mapping. The notification service records the
 * chat that originated each accepted download job so that the result can be
 * delivered back to the right chat when the download-service calls back.
 */
void jobmap_put(const char *job_id, int64_t chat_id);

/* Returns the chat id for `job_id`, or 0 when no such job is tracked. */
int64_t jobmap_get(const char *job_id);

void jobmap_remove(const char *job_id);

#endif
