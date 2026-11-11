#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "download.h"
#include "json.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_DOWNLOAD_SERVICE_URL "http://127.0.0.1:8080/download"
#define DOWNLOAD_SUBMIT_BODY_MAX 4096
#define DOWNLOAD_SUBMIT_RESPONSE_MAX 1024

/* Response body capture (same pattern as telegram.c). */
typedef struct {
    char *buffer;
    size_t capacity;
    size_t written;
} response_buf_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
    response_buf_t *buf = (response_buf_t *)userp;
    size_t total = size * nmemb;

    if (buf->buffer && buf->capacity > 0) {
        size_t remaining = (buf->written < buf->capacity)
                               ? (buf->capacity - 1 - buf->written)
                               : 0;
        size_t to_copy = (total < remaining) ? total : remaining;
        memcpy(buf->buffer + buf->written, contents, to_copy);
        buf->written += to_copy;
        buf->buffer[buf->written] = '\0';
    }

    /* Always return full total to satisfy the libcurl contract. */
    return total;
}

int download_submit(int64_t chat_id, const char *url,
                    char *job_id_out, size_t job_id_cap) {
    if (!url || !job_id_out || job_id_cap == 0)
        return -1;

    const char *service_url = getenv("DOWNLOAD_SERVICE_URL");
    if (!service_url || service_url[0] == '\0')
        service_url = DEFAULT_DOWNLOAD_SERVICE_URL;

    char e_url[DOWNLOAD_SUBMIT_BODY_MAX];
    if (json_escape_string(url, e_url, sizeof(e_url)) < 0)
        return -1;

    char body[DOWNLOAD_SUBMIT_BODY_MAX];
    int n = snprintf(body, sizeof(body),
                     "{\"url\": \"%s\", \"chat_id\": %lld}", e_url,
                     (long long)chat_id);
    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;

    char resp_storage[DOWNLOAD_SUBMIT_RESPONSE_MAX + 1];
    resp_storage[0] = '\0';
    response_buf_t resp = {resp_storage, sizeof(resp_storage), 0};

    CURL *curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    long http_code = 0;
    int rc = -1;

    curl_easy_setopt(curl, CURLOPT_URL, service_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode cres = curl_easy_perform(curl);
    if (cres == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200) {
            if (json_find_top_level_string(resp_storage, "job_id",
                                           job_id_out, job_id_cap)) {
                rc = 0;
            } else {
                rc = -1;
                fprintf(stderr,
                        "download-service: accepted download but no job_id in "
                        "response\n");
            }
        } else if (http_code == 400) {
            char error[128];
            if (json_find_top_level_string(resp_storage, "error", error,
                                           sizeof(error)) &&
                strcmp(error, "unsupported_domain") == 0) {
                rc = 1;
            } else {
                rc = -1;
            }
        } else {
            rc = -1;
        }
    } else {
        fprintf(stderr,
                "download-service: POST %s failed: %s\n", service_url,
                curl_easy_strerror(cres));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc;
}
