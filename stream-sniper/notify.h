/*
 * notify.h - recording start/stop notifications.
 *
 * Whenever a recording starts or stops, the event is POSTed as
 * {"message": "<text>"} to SS_NOTIFY_URL + /notifications. The
 * feature is a silent no-op until notify_init() has seen a usable
 * URL: a missing or dead endpoint must never affect a recording,
 * so every failure path here is at most a log line.
 */

#ifndef NOTIFY_H
#define NOTIFY_H

#include <stdbool.h>

#include "urlparse.h"

/*
 * Resolve SS_NOTIFY_URL once, from main() after http_global_init().
 *
 * Unset or empty: disabled. Not an http:// or https:// URL: warned
 * and disabled. A trailing slash is trimmed off the base, and the
 * fixed path /notifications is appended here, so the send path is
 * a plain string. What lands in the module is written once, before
 * any recorder thread exists, and read-only afterwards.
 */
void notify_init(void);

/* "stream-sniper: started recording <slug>" */
void notify_recording_started(const channel_t *ch);

/*
 * "stream-sniper: stopped recording <slug> after <dur> (completed|failed)"
 *
 * "completed" when clean (the recorder's child exited 0), "failed"
 * for every other outcome - a non-zero exit or a signal, which is
 * the same WIFEXITED/WEXITSTATUS check that picks DONE vs FAIL/STOP.
 */
void notify_recording_stopped(const channel_t *ch, bool clean,
                              const char *dur);

/*
 * JSON-escape src into a freshly allocated, quoted JSON string
 * ("..."), caller frees. Internal to notify.c - not a facility for
 * other modules, jsonlite reads JSON but nothing else writes it -
 * declared here only so the tests can exercise the escaper on its
 * own.
 */
char *notify_json_quote(const char *src);

#endif /* NOTIFY_H */
