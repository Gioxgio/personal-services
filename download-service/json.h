#ifndef JSON_H
#define JSON_H

#include <stdbool.h>

#define MAX_DOWNLOAD_URL 4096
#define MAX_DOWNLOAD_FORMAT 16

/*
 * A download request payload:
 *   { "url": "<youtube url>", "format": "audio" | "video" }
 */
typedef struct {
    char url[MAX_DOWNLOAD_URL + 1];
    char format[MAX_DOWNLOAD_FORMAT + 1]; /* "audio" or "video"; defaults to "video" */
    int chat_id;                        /* originating chat; defaults to 0 */
} download_payload_t;

bool json_parse_download(const char *json_raw, download_payload_t *out_payload);

#endif
