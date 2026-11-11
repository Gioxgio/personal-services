#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_TELEGRAM_MSG_LEN 4096
#define MAX_NOTIFICATION_SOURCE_LEN 64

typedef struct {
    char source[MAX_NOTIFICATION_SOURCE_LEN + 1]; /* who sent the notification */
    char message[MAX_TELEGRAM_MSG_LEN + 1];
} notification_payload_t;

bool json_parse_request(const char* json_raw, notification_payload_t* out_payload);

/*
 * Find a top-level object member named `key` and copy its string value,
 * unescaped, into `out` (capacity `cap`). Only members of the outermost object
 * are considered. Returns true on success. `out` is always NUL-terminated.
 */
bool json_find_top_level_string(const char* json_raw, const char* key, char* out, size_t cap);

/*
 * Find a top-level object member named `key` and parse its value as a JSON
 * number into `out`. Only members of the outermost object are considered.
 * Returns true on success; `out` is set only on success.
 */
bool json_find_top_level_int64(const char* json_raw, const char* key, long long* out);

/*
 * Extract the sender's chat id and the message text from a Telegram Update:
 *   {"update_id":N,"message":{"chat":{"id":N},"text":"..."}}
 * Returns true when both were found (member order is not assumed). Outputs are
 * NUL-terminated decimal (chat id) / raw (text) strings; either output may be
 * NULL to skip it. `chat_id` is written as a decimal string.
 */
bool json_extract_telegram_update(const char* json_raw, char* chat_id_out, size_t chat_id_cap,
                                  char* text_out, size_t text_cap);

/* Escape `input` as a JSON string into `output` (capacity `output_size`).
 * Returns the number of bytes written (excluding NUL), or -1 if it does not
 * fit. */
int json_escape_string(const char* input, char* output, size_t output_size);

#endif
