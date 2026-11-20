#ifndef JOBSET_H
#define JOBSET_H

#include <stdbool.h>

/*
 * Maximum number of in-flight download jobs the notification service tracks.
 * The download callback uses this set to verify the job is known.
 */
#define JOBSET_CAPACITY 256

/* Thread-safe set of in-flight job ids. */
void jobset_add(const char* job_id);
bool jobset_contains(const char* job_id);
void jobset_remove(const char* job_id);

#endif
