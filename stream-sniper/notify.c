/*
 * notify.c - recording start/stop notifications.
 *
 * One POST per recording boundary, to the base URL SS_NOTIFY_URL
 * names (the fixed path /notifications is appended by
 * notify_init(), not by the caller, so the env var holds a bare
 * base like http://192.168.1.50:9000).
 *
 * Failure semantics: http.c already logs every transport and
 * status failure, and a notification that never arrives changes
 * nothing about the recording - so the failures are ignored here.
 * The cost is this thread's HTTP timeouts, which is the known
 * tradeoff (a shorter-timeout http_post_ex() is the follow-up).
 *
 * Thread safety: notify_init() writes the URL once, before any
 * recorder thread exists; afterwards it is read-only, so the
 * recorder threads that fire these notifications need no lock.
 * http_post() takes a fresh curl easy handle per call, and
 * curl_global_init() ran in main().
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "log.h"
#include "notify.h"
#include "util.h"

/* The one path every notification is POSTed to. */
#define NOTIFY_PATH "/notifications"

/*
 * The POST target notify_init() built ("<base>/notifications"), or
 * NULL when the feature is off. Written once, before any recorder
 * thread exists; read-only afterwards.
 */
static char *g_url;

char *notify_json_quote(const char *src)
{
    /* Worst case is six bytes out per byte in (\u00xx), plus the
     * two quotes and the NUL. */
    char *out = xmalloc(strlen(src) * 6 + 3);
    char *w = out;
    *w++ = '"';
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        switch (*p) {
        case '"':  *w++ = '\\'; *w++ = '"';  break;
        case '\\': *w++ = '\\'; *w++ = '\\'; break;
        case '\b': *w++ = '\\'; *w++ = 'b';  break;
        case '\f': *w++ = '\\'; *w++ = 'f';  break;
        case '\n': *w++ = '\\'; *w++ = 'n';  break;
        case '\r': *w++ = '\\'; *w++ = 'r';  break;
        case '\t': *w++ = '\\'; *w++ = 't';  break;
        default:
            if (*p < 0x20)
                w += sprintf(w, "\\u%04x", (unsigned)*p);
            else
                *w++ = (char)*p;             /* UTF-8 passes through */
        }
    }
    *w++ = '"';
    *w = '\0';
    return out;
}

void notify_init(void)
{
    /* Re-resolving replaces rather than leaks, so a caller may
     * re-run init (the tests do); production calls it once. */
    free(g_url);
    g_url = NULL;

    const char *env = getenv("SS_NOTIFY_URL");
    if (!env || !*env)
        return;                          /* unset or empty: disabled */

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
    char *base = xstrdup(env);
    size_t n = strlen(base);
    if (n > 0 && base[n - 1] == '/')
        base[n - 1] = '\0';

    g_url = xmalloc(n + sizeof NOTIFY_PATH);
    snprintf(g_url, n + sizeof NOTIFY_PATH, "%s%s", base, NOTIFY_PATH);
    free(base);

    logmsg(LOG_INFO, "notifications: %s", g_url);
}

static void notify_send(const char *message)
{
    if (!g_url)
        return;

    /* Every notification carries the originating service as "source". */
    char *quoted_src = notify_json_quote("stream-sniper");
    char *quoted_msg = notify_json_quote(message);
    size_t need = strlen(quoted_src) + strlen(quoted_msg) +
                  sizeof("{\"source\": , \"message\": }");
    char *body = xmalloc(need);
    snprintf(body, need, "{\"source\": %s, \"message\": %s}",
             quoted_src, quoted_msg);
    free(quoted_src);
    free(quoted_msg);

    const char *hdrs[] = { "Content-Type: application/json", NULL };
    http_buffer out = { 0 };
    http_post(g_url, hdrs, body, &out);
    http_buffer_free(&out);
    free(body);
}

static void notify_fmt(const char *fmt, ...)
{
    if (!g_url)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    char *message = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(message, (size_t)n + 1, fmt, ap);
    va_end(ap);

    notify_send(message);
    free(message);
}

void notify_recording_started(const channel_t *ch)
{
    notify_fmt("started recording %s", ch->slug);
}

void notify_recording_stopped(const channel_t *ch, bool clean,
                              const char *dur)
{
    notify_fmt("stopped recording %s after %s (%s)",
               ch->slug, dur, clean ? "completed" : "failed");
}
