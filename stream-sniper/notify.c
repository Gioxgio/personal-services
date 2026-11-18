/*
 * notify.c - recording start/stop notifications.
 *
 * Whenever a recording starts or stops, the event is POSTed as
 * {"source": "stream-sniper", "message": "<text>"} to SS_NOTIFY_URL +
 * /notifications. The feature is a silent no-op until notify_init()
 * has seen a usable URL: a missing or dead endpoint must never affect
 * a recording, so every failure path here is at most a log line.
 *
 * The actual HTTP POST is delegated to the shared modules/notify
 * implementation (notify_post()), which uses blocking BSD sockets with
 * fixed timeouts. Failure is ignored here: a notification that never
 * arrives changes nothing about the recording.
 *
 * Thread safety: notify_init() writes the URL once, before any recorder
 * thread exists; afterwards it is read-only, so the recorder threads
 * that fire these notifications need no lock.
 */

#include "notify.h"

#include "../modules/notify/notify.h"
#include "log.h"
#include "util.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one path every notification is POSTed to. */
#define NOTIFY_PATH "/notifications"

/*
 * The POST target notify_init() built ("<base>/notifications"), or
 * NULL when the feature is off. Written once, before any recorder
 * thread exists; read-only afterwards.
 */
static char* g_url;

char* notify_json_quote(const char* src) {
    /* Worst case is six bytes out per byte in (\u00xx), plus the
     * two quotes and the NUL. */
    char* out = xmalloc(strlen(src) * 6 + 3);
    char* w = out;
    *w++ = '"';
    for (const unsigned char* p = (const unsigned char*)src; *p; p++) {
        switch (*p) {
            case '"':
                *w++ = '\\';
                *w++ = '"';
                break;
            case '\\':
                *w++ = '\\';
                *w++ = '\\';
                break;
            case '\b':
                *w++ = '\\';
                *w++ = 'b';
                break;
            case '\f':
                *w++ = '\\';
                *w++ = 'f';
                break;
            case '\n':
                *w++ = '\\';
                *w++ = 'n';
                break;
            case '\r':
                *w++ = '\\';
                *w++ = 'r';
                break;
            case '\t':
                *w++ = '\\';
                *w++ = 't';
                break;
            default:
                if (*p < 0x20)
                    w += sprintf(w, "\\u%04x", (unsigned)*p);
                else
                    *w++ = (char)*p; /* UTF-8 passes through */
        }
    }
    *w++ = '"';
    *w = '\0';
    return out;
}

void notify_init(void) {
    /* Re-resolving replaces rather than leaks, so a caller may
     * re-run init (the tests do); production calls it once. */
    free(g_url);
    g_url = NULL;

    const char* env = getenv("SS_NOTIFY_URL");
    if (!env || !*env)
        return; /* unset or empty: disabled */

    if (strncmp(env, "http://", strlen("http://")) != 0 &&
        strncmp(env, "https://", strlen("https://")) != 0) {
        logmsg(LOG_WARN, "SS_NOTIFY_URL must be an http:// or https:// "
                         "URL - notifications disabled");
        return;
    }

    /* Exactly one trailing slash, if any: the path below starts
     * with its own, and stopping at one keeps a scheme-only value
     * such as "http://" the (rejectable) URL it is instead of
     * trimming it down to "http:". */
    char* base = xstrdup(env);
    size_t n = strlen(base);
    if (n > 0 && base[n - 1] == '/')
        base[n - 1] = '\0';

    g_url = xmalloc(n + sizeof NOTIFY_PATH);
    snprintf(g_url, n + sizeof NOTIFY_PATH, "%s%s", base, NOTIFY_PATH);
    free(base);

    logmsg(LOG_INFO, "notifications: %s", g_url);
}

static void notify_send(const char* message) {
    if (!g_url)
        return;

    /* Every notification carries the originating service as "source". */
    notify_post(g_url, "stream-sniper", message);
}

static void notify_fmt(const char* fmt, ...) {
    if (!g_url)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    char* message = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(message, (size_t)n + 1, fmt, ap);
    va_end(ap);

    notify_send(message);
    free(message);
}

void notify_recording_started(const channel_t* ch) {
    notify_fmt("Started recording %s", ch->slug);
}

void notify_recording_stopped(const channel_t* ch, bool clean, const char* dur) {
    notify_fmt("Stopped recording %s after %s (%s)", ch->slug, dur, clean ? "completed" : "failed");
}
