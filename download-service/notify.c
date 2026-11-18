#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * notify.c - download-service notification wrapper.
 *
 * Delegates to the shared modules/notify implementation, hardcoding the
 * source as "download-service". Keeps the service-specific API
 * notify_send(notify_url, message) used by worker_pool.c.
 */

#include "notify.h"

#include "../modules/notify/notify.h"

bool notify_send(const char* notify_url, const char* message) {
    return notify_post(notify_url, "download-service", message);
}
