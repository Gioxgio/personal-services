#ifndef MODULES_NOTIFY_H
#define MODULES_NOTIFY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * POST {"source": "<source>", "message": "<message>"} as JSON to `url`.
 *
 * Returns true when the endpoint answers with a 2xx status code, false on
 * network error, timeout, or non-2xx response. A NULL or empty `url` is a
 * no-op and returns true (notifications disabled). Only http:// is supported
 * (no TLS). The `source` string identifies the originating service
 * (e.g. "download-service" or "stream-sniper").
 */
bool notify_post(const char* url, const char* source, const char* message);

/*
 * POST the file at `file_path` to `url` as multipart/form-data, using
 * `field_name` as the form field name. The file's last path component is
 * sent as the filename.
 *
 * Returns true on 2xx, false on error/non-2xx. A NULL or empty `url`,
 * `file_path`, or `field_name` is a no-op and returns true. Only http:// is
 * supported (no TLS). The whole file is read into memory, so this is meant
 * for small-to-moderate files.
 */
bool send_file(const char* url, const char* file_path, const char* field_name);

#endif /* MODULES_NOTIFY_H */
