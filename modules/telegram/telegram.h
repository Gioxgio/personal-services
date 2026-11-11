#ifndef TELEGRAM_H
#define TELEGRAM_H

#include "json.h"

#include <stddef.h>

typedef struct {
    const char* token;
    const char* chat_id;
} telegram_config_t;

int telegram_send_message(const telegram_config_t* config, const notification_payload_t* payload,
                          char* response_out, size_t response_capacity);

/*
 * Send a text message to an arbitrary chat. `chat_id` may be a numeric id or a
 * @channelusername. Returns the Telegram HTTP status code, or -1 on transport
 * error.
 */
int telegram_send_message_to(const char* token, const char* chat_id, const char* text,
                             char* response_out, size_t response_capacity);

/*
 * Send a document (file) to a chat via Telegram's sendDocument API.
 *
 * `token`     - bot token
 * `chat_id`   - target chat (numeric id or @channelusername)
 * `file_path` - path to the file on disk to upload
 * `caption`   - optional caption (may be NULL or empty)
 *
 * Returns the Telegram HTTP status code, or -1 on transport error.
 */
int telegram_send_document(const char* token, const char* chat_id,
                           const char* file_path, const char* caption,
                           char* response_out, size_t response_capacity);

/*
 * Poll Telegram's getUpdates endpoint for new updates.
 *
 * Writes the raw JSON response body into `out` (capacity `out_cap`) and
 * returns the Telegram HTTP status code, or -1 on transport error. `offset` is
 * the update_id to start from (updates with id <= offset are skipped). A long
 * `timeout_sec` enables Telegram's long polling so the call blocks until an
 * update arrives or the timeout expires.
 *
 * The response body is the full Telegram API response (top-level "ok" and
 * "result" array); callers parse it themselves.
 */
int telegram_get_updates(const char* token, long offset, int timeout_sec,
                         char* out, size_t out_cap);

#endif /* TELEGRAM_H */
