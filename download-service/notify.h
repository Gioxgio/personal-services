#ifndef NOTIFY_H
#define NOTIFY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * POST a download notification to the notification-service.
 *
 * Builds the JSON body
 *   { "source": "download-service", "message": "<text>" }
 * and POSTs it to <notify_url>/notifications.
 *
 * Returns true when the endpoint answers with a 2xx status code, false on
 * network error, timeout, or non-2xx response. A NULL or empty `notify_url`
 * is a no-op and returns true (notifications disabled).
 *
 * All socket operations are bounded by fixed timeouts; the function blocks
 * until the request completes or times out.
 */
bool notify_send(const char* notify_url, const char* message);

#endif /* NOTIFY_H */
