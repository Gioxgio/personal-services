#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        logmsg(LOG_ERROR, "out of memory (%zu bytes)", n);
        exit(EXIT_FAILURE);
    }
    return p;
}

char *xstrdup(const char *s)
{
    char *p = strdup(s);
    if (!p) {
        logmsg(LOG_ERROR, "out of memory");
        exit(EXIT_FAILURE);
    }
    return p;
}

char *lower_dup(const char *s)
{
    char *p = xstrdup(s);
    for (char *q = p; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    return p;
}

void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
}

void strip_spaces(char *s)
{
    /* Single pass: the naive memmove-per-space version is O(n^2). */
    char *w = s;
    for (const char *r = s; *r; r++)
        if (*r != ' ' && *r != '\t')
            *w++ = *r;
    *w = '\0';
}

char *sanitise(const char *s)
{
    /* +2, not +1: the '_' fallback below can add a character the
     * source string did not have, and its NUL must still fit. */
    char *out = xmalloc(strlen(s) + 2);
    size_t j = 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        out[j++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }
    if (j == 0)
        out[j++] = '_';
    out[j] = '\0';
    return out;
}

char *last_path_segment(char *url)
{
    /*
     * A query string or a fragment belongs to the URL, not to its last path
     * segment: twitch.tv/foo?lang=en must yield "foo", not "foo?lang=en".
     * Cut here rather than at the end, so the returned pointer is NUL
     * terminated for the caller.
     */
    char *cut = strpbrk(url, "?#");
    if (cut)
        *cut = '\0';
    char *end = url + strlen(url);
    while (end > url && end[-1] == '/')
        end--;
    *end = '\0';                     /* a trailing slash is not the segment */

    char *start = end;
    while (start > url && start[-1] != '/')
        start--;
    return (start >= end) ? NULL : start;
}

bool mkdir_p(const char *dir)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", dir) >= (int)sizeof tmp)
        return false;

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

/* Longest single nap: bounds how long *stop goes unnoticed. */
#define SLEEP_CHUNK_NS 500000000L

void interruptible_sleep(long seconds, const volatile sig_atomic_t *stop)
{
    if (seconds <= 0 || !*stop)
        return;

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        return;
    deadline.tv_sec += seconds;

    while (*stop) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return;

        /* Time left until the deadline. Measured against a monotonic clock and
         * recomputed each round, so a nap cut short by a signal does not cost
         * the caller the remainder of the interval. */
        struct timespec remain;
        remain.tv_sec  =  deadline.tv_sec  - now.tv_sec;
        remain.tv_nsec =  deadline.tv_nsec - now.tv_nsec;
        if (remain.tv_nsec < 0) {
            remain.tv_nsec += 1000000000L;
            remain.tv_sec--;
        }
        if (remain.tv_sec < 0)
            return;                     /* deadline passed */

        struct timespec nap = remain;
        if (remain.tv_sec > 0 || remain.tv_nsec > SLEEP_CHUNK_NS)
            nap = (struct timespec){ .tv_sec = 0, .tv_nsec = SLEEP_CHUNK_NS };

        /* nanosleep() writes the unslept remainder back into nap on EINTR. */
        while (nanosleep(&nap, &nap) != 0 && errno == EINTR && *stop)
            ;
    }
}

void human_duration(char *buf, size_t bufsz, long secs)
{
    if (secs < 0)
        secs = 0;

    long h = secs / 3600;
    long m = (secs % 3600) / 60;
    long s = secs % 60;

    if (h > 0)
        snprintf(buf, bufsz, "%ld:%02ld:%02ld", h, m, s);
    else
        snprintf(buf, bufsz, "%ld:%02ld", m, s);
}