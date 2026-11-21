#include "telegram.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Response buffer

typedef struct {
    char* buffer;
    size_t capacity;
    size_t written;
} http_response_buf_t;

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    http_response_buf_t* buf = (http_response_buf_t*)userp;
    size_t total = size * nmemb;

    if (buf->buffer && buf->capacity > 0) {
        // Reserve 1 byte for null-termination
        size_t remaining = (buf->written < buf->capacity) ? (buf->capacity - 1 - buf->written) : 0;
        size_t to_copy = (total < remaining) ? total : remaining;
        memcpy(buf->buffer + buf->written, contents, to_copy);
        buf->written += to_copy;
        buf->buffer[buf->written] = '\0';
    }

    // Always return full total to satisfy libcurl contract
    return total;
}

/* ---------- helpers ---------- */

/* Build a Telegram API URL for `method`. TELEGRAM_API_URL overrides the host
 * so tests can point at a local mock server. */
static int telegram_api_url(const char* token, const char* method, char* out, size_t cap) {
    const char* api_base = getenv("TELEGRAM_API_URL");
    int n;
    if (api_base && api_base[0]) {
        n = snprintf(out, cap, "%s/bot%s/%s", api_base, token, method);
    } else {
        n = snprintf(out, cap, "https://api.telegram.org/bot%s/%s", token, method);
    }
    return (n >= 0 && (size_t)n < cap) ? 0 : -1;
}

// Public API

/* Internal text transport used by telegram_send_message(). File uploads use
 * telegram_send_document(); both go directly to the Telegram API. */
static int telegram_send_raw(const char* token, const char* chat_id, const char* text,
                             char* response_out, size_t response_capacity);

int telegram_send_message(const telegram_config_t* config, const notification_payload_t* payload,
                          char* response_out, size_t response_capacity) {
    /* Message format: "[source] message". Source is always set and
     * non-empty by the sender (validated by the JSON parser). */
    char formatted[MAX_TELEGRAM_MSG_LEN + 1];
    int n = snprintf(formatted, sizeof(formatted), "[%s] %s", payload->source, payload->message);
    if (n < 0 || (size_t)n >= sizeof(formatted)) {
        n = snprintf(formatted, sizeof(formatted), "%s", payload->message);
    }
    /* All messages go to the chat configured for this service. */
    return telegram_send_raw(config->token, config->chat_id, formatted, response_out,
                             response_capacity);
}

static int telegram_send_raw(const char* token, const char* chat_id, const char* text,
                             char* response_out, size_t response_capacity) {
    CURL* curl;
    CURLcode res;
    long http_code = 0;

    char url[512];
    if (telegram_api_url(token, "sendMessage", url, sizeof(url)) < 0) {
        return -1;
    }

    /* Build JSON body with escaped message */
    char escaped_msg[MAX_TELEGRAM_MSG_LEN * 2];
    if (json_escape_string(text, escaped_msg, sizeof(escaped_msg)) < 0) {
        return -1;
    }

    char json_body[MAX_TELEGRAM_MSG_LEN * 2 + 256];
    snprintf(json_body, sizeof(json_body), "{\"chat_id\": \"%s\", \"text\": \"%s\"}", chat_id,
             escaped_msg);

    /* Set up response buffer */
    http_response_buf_t resp_buf = {response_out, response_capacity, 0};
    if (response_out && response_capacity > 0) {
        response_out[0] = '\0';
    }

    /* Build headers */
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headers);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    return (int)http_code;
}

/*
 * Upload a local file as a Telegram document. The multipart body carries the
 * chat id, the file itself and an optional caption.
 */
int telegram_send_document(const telegram_config_t* config, const char* file_path,
                           const char* caption, char* response_out, size_t response_capacity) {
    if (!config || !config->token || !config->chat_id || !file_path || file_path[0] == '\0') {
        return -1;
    }

    char url[512];
    if (telegram_api_url(config->token, "sendDocument", url, sizeof(url)) < 0) {
        return -1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    struct curl_slist* headers = curl_slist_append(NULL, "Content-Type: multipart/form-data");
    curl_mime* mime = curl_mime_init(curl);
    if (!headers || !mime) {
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, config->chat_id, CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "document");
    if (curl_mime_filedata(part, file_path) != CURLE_OK) {
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    if (caption && caption[0] != '\0') {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "caption");
        curl_mime_data(part, caption, CURL_ZERO_TERMINATED);
    }

    http_response_buf_t resp_buf = {response_out, response_capacity, 0};
    if (response_out && response_capacity > 0) {
        response_out[0] = '\0';
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK ? (int)http_code : -1;
}

/* ---------- polling ---------- */

/* Fetch updates from Telegram's getUpdates endpoint.
 *
 * Writes the raw JSON response body into `out` (capacity `out_cap`) and
 * returns the HTTP status code, or -1 on transport error. `offset` is the
 * update_id to start from (updates with id <= offset are skipped).
 *
 * The response body is the full Telegram API response (with "ok", "result",
 * etc.); callers parse the "result" array themselves.
 */
int telegram_get_updates(const char* token, long offset, int timeout_sec,
                         char* out, size_t out_cap) {
    if (!token || !out || out_cap == 0)
        return -1;

    /* Build URL: https://api.telegram.org/bot<token>/getUpdates?offset=...&timeout=...&allowed_updates=["message"] */
    char url[512];
    const char* api_base = getenv("TELEGRAM_API_URL");
    int n;
    if (api_base && api_base[0]) {
        n = snprintf(url, sizeof(url), "%s/bot%s/getUpdates?offset=%ld&timeout=%d&allowed_updates=[\"message\"]",
                     api_base, token, offset, timeout_sec);
    } else {
        n = snprintf(url, sizeof(url),
                     "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=%d&allowed_updates=[\"message\"]",
                     token, offset, timeout_sec);
    }
    if (n < 0 || (size_t)n >= sizeof(url))
        return -1;

    http_response_buf_t resp_buf = {out, out_cap, 0};
    if (out_cap > 0)
        out[0] = '\0';

    CURL* curl = curl_easy_init();
    if (!curl)
        return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec + 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return -1;
    return (int)http_code;
}
