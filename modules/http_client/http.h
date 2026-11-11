/*
 * http.h - minimal HTTP client built on libcurl.
 *
 * Provides blocking GET/POST used by the Twitch auth helper and the two
 * live-state probes. Callers pass response bodies to http_buffer_free().
 */

#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

typedef struct {
    char  *data;   /* NUL-terminated, heap allocated */
    size_t len;
} http_buffer;

void http_global_init(void);
void http_global_cleanup(void);

/*
 * Perform a GET. "headers" is a NULL-terminated array of "Name: value"
 * strings (or NULL for none).
 *
 * Contract: *out is reset to empty on entry and only filled on HTTP 200; on
 * any failure it is left empty and a negative error code is returned. The
 * caller passes a zero-initialised http_buffer and releases it with
 * http_buffer_free().
 */
int http_get(const char *url, const char *const *headers, http_buffer *out);

/*
 * Same as http_get, but also reports the HTTP status code through
 * *status_out so a caller can tell a 401 from a timeout. 0 means "no HTTP
 * response was received" (DNS failure, connect timeout, TLS error), which is
 * what distinguishes a dead host from a rejecting one.
 */
int http_get_ex(const char *url, const char *const *headers,
                http_buffer *out, long *status_out);

/*
 * Same as http_get but POSTs "postfields" (may be NULL for an empty body).
 */
int http_post(const char *url, const char *const *headers,
              const char *postfields, http_buffer *out);

void http_buffer_free(http_buffer *buf);

#endif /* HTTP_H */