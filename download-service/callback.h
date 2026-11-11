#ifndef CALLBACK_H
#define CALLBACK_H

#include <stdbool.h>
#include <stddef.h>

/*
 * POST a download result to the notification-service callback endpoint.
 *
 * Builds the JSON body
 *   { "job_id": "...", "chat_id": 123, "source": "download-service",
 *     "status": "completed|failed", "file_path": "...", "error": "..." }
 * (file_path is included only for completed jobs; error only for failed jobs)
 * and POSTs it to `callback_url`.
 *
 * Returns true when the endpoint answers with a 2xx status code, false on
 * network error, timeout, non-2xx response, or a malformed URL. All socket
 * operations are bounded by fixed timeouts; the function blocks until the
 * request completes or times out.
 *
 * A NULL or empty `callback_url` is a no-op and returns true: no callback
 * endpoint is configured.
 */
bool callback_send(const char* callback_url, const char* job_id, int chat_id, const char* status,
                   const char* file_path, const char* error);

#endif
