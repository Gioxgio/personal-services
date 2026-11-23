#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stddef.h>

/*
 * Submit a download job to the download-service (POST /download).
 *
 * Builds {"url": "..."} and POSTs it to the download-service endpoint read
 * from the required DOWNLOAD_SERVICE_URL environment variable.
 *
 * Returns:
 *   0  accepted: job_id_out is filled with the job id (NUL-terminated).
 *   1  rejected: 400 unsupported_domain from the download service.
 *  -1  transport or protocol error.
 *
 * All socket operations are bounded by fixed timeouts; the function blocks
 * until the request completes or times out.
 */
int download_submit(const char* url, char* job_id_out, size_t job_id_cap);

/*
 * Fetch `url` (a file served by the download-service) into a unique temporary
 * directory under /tmp and return its path in `out_path` (capacity `out_cap`).
 * `filename` is the name the recipient should see (normally the basename of
 * the completed job's file_path); it is sanitized with the same rule as the
 * live filenames, so the temp file never ends in ".part".
 * Returns 0 on success, -1 on failure. The caller must release the temp file
 * with download_fetch_cleanup() once it has been sent.
 */
int download_fetch_to_file(const char* url, const char* filename, char* out_path, size_t out_cap);

/* Remove the temp file returned by download_fetch_to_file() and its directory. */
void download_fetch_cleanup(const char* path);

#endif
