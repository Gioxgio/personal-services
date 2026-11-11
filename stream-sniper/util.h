/*
 * util.h - allocation, string and filesystem helpers.
 *
 * Pure helpers, no knowledge of channels or recording. Every helper either
 * returns a heap-allocated string (caller frees) or operates in place.
 */

#ifndef UTIL_H
#define UTIL_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

/* Allocation wrappers that abort the process on failure. */
void *xmalloc(size_t n);
char *xstrdup(const char *s);
char *lower_dup(const char *s);   /* lowercase copy, caller frees */

/* In-place string cleanup. */
void trim(char *s);               /* strip trailing whitespace/newlines   */
void strip_spaces(char *s);       /* remove ALL spaces/tabs in place      */

/* Build a filesystem-safe copy of s, caller frees. */
char *sanitise(const char *s);

/*
 * Cut url at the first '?' or '#' in place - a query string or fragment is
 * not part of the path - and return a pointer to what is left of its last
 * path segment, or NULL if there is none. The result points into url.
 */
char *last_path_segment(char *url);

/* Recursively create dir (like mkdir -p). Returns true on success/exists. */
bool mkdir_p(const char *dir);

/*
 * Sleep seconds, measured against a CLOCK_MONOTONIC deadline so an
 * interrupted nap never shortens the wait, and waking as soon as *stop
 * becomes 0. The wait is chunked, so a signal is acted on well inside the
 * interval rather than after it.
 */
void interruptible_sleep(long seconds, const volatile sig_atomic_t *stop);

/* Render a duration in seconds as "H:MM:SS" / "M:SS", e.g. "1:23:45". */
void human_duration(char *buf, size_t bufsz, long secs);

#endif /* UTIL_H */