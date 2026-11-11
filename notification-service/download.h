#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Submit a download job to the download-service (POST /download).
 *
 * Builds {"url": "...", "chat_id": N} and POSTs it to the download-service
 * endpoint read from the DOWNLOAD_SERVICE_URL environment variable (default:
 * http://127.0.0.1:8080/download).
 *
 * Returns:
 *   0  accepted: job_id_out is filled with the job id (NUL-terminated).
 *   1  rejected: 400 unsupported_domain from the download service.
 *  -1  transport or protocol error.
 *
 * All socket operations are bounded by fixed timeouts; the function blocks
 * until the request completes or times out.
 */
int download_submit(int64_t chat_id, const char *url,
                    char *job_id_out, size_t job_id_cap);

#endif
