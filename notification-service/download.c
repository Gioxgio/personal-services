#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "download.h"

#include "../modules/filename/filename.h"
#include "json.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DOWNLOAD_SUBMIT_BODY_MAX 4096
#define DOWNLOAD_SUBMIT_RESPONSE_MAX 1024

/* Response body capture (same pattern as telegram.c). */
typedef struct {
    char* buffer;
    size_t capacity;
    size_t written;
} response_buf_t;

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    response_buf_t* buf = (response_buf_t*)userp;
    size_t total = size * nmemb;

    if (buf->buffer && buf->capacity > 0) {
        size_t remaining = (buf->written < buf->capacity) ? (buf->capacity - 1 - buf->written) : 0;
        size_t to_copy = (total < remaining) ? total : remaining;
        memcpy(buf->buffer + buf->written, contents, to_copy);
        buf->written += to_copy;
        buf->buffer[buf->written] = '\0';
    }

    /* Always return full total to satisfy the libcurl contract. */
    return total;
}

int download_submit(const char* url, char* job_id_out, size_t job_id_cap) {
    if (!url || !job_id_out || job_id_cap == 0)
        return -1;

    const char* service_url = getenv("DOWNLOAD_SERVICE_URL");
    if (!service_url || service_url[0] == '\0') {
        fprintf(stderr, "notification-service: DOWNLOAD_SERVICE_URL is not set\n");
        return -1;
    }

    char e_url[DOWNLOAD_SUBMIT_BODY_MAX];
    if (json_escape_string(url, e_url, sizeof(e_url)) < 0)
        return -1;

    char body[DOWNLOAD_SUBMIT_BODY_MAX];
    int n = snprintf(body, sizeof(body), "{\"url\": \"%s\"}", e_url);
    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;

    char resp_storage[DOWNLOAD_SUBMIT_RESPONSE_MAX + 1];
    resp_storage[0] = '\0';
    response_buf_t resp = {resp_storage, sizeof(resp_storage), 0};

    CURL* curl = curl_easy_init();
    if (!curl)
        return -1;

    struct curl_slist* headers = NULL;
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
            if (json_find_top_level_string(resp_storage, "job_id", job_id_out, job_id_cap)) {
                rc = 0;
            } else {
                rc = -1;
                fprintf(stderr, "download-service: accepted download but no job_id in "
                                "response\n");
            }
        } else if (http_code == 400) {
            char error[128];
            if (json_find_top_level_string(resp_storage, "error", error, sizeof(error)) &&
                strcmp(error, "unsupported_domain") == 0) {
                rc = 1;
            } else {
                rc = -1;
            }
        } else {
            rc = -1;
        }
    } else {
        fprintf(stderr, "download-service: POST %s failed: %s\n", service_url,
                curl_easy_strerror(cres));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc;
}

/*
 * Fetch `url` (a downloaded file served by the download-service) into a
 * temporary file named after the recipient-visible filename. A unique parent
 * directory keeps concurrent fetches of the same title apart, so the base name
 * can stay exactly what the user should see.
 */
int download_fetch_to_file(const char* url, const char* filename, char* out_path, size_t out_cap) {
    if (!url || !out_path || out_cap == 0)
        return -1;

    char* clean = sanitize_filename_keep_extension(filename && filename[0] ? filename : "download");
    if (!clean)
        return -1;
    size_t clean_len = strlen(clean);
    if (clean_len > 200) {
        /* Keep the extension when trimming an unusually long title. */
        const char* dot = strrchr(clean, '.');
        size_t ext_len = (dot && dot != clean) ? strlen(dot) : 0;
        if (ext_len > 10)
            ext_len = 0;
        memmove(clean + 200 - ext_len, clean + clean_len - ext_len, ext_len + 1);
    }

    pid_t pid = getpid();
    bool created = false;
    for (int i = 0; i < 1000; i++) {
        char dir[PATH_MAX];
        int n = snprintf(dir, sizeof dir, "/tmp/dl-fetch.%ld.%d", (long)pid, i);
        if (n < 0 || (size_t)n >= sizeof dir)
            break;
        if (mkdir(dir, 0700) != 0) {
            if (errno == EEXIST)
                continue;
            break;
        }
        n = snprintf(out_path, out_cap, "%s/%s", dir, clean);
        if (n < 0 || (size_t)n >= out_cap) {
            (void)rmdir(dir);
            break;
        }
        FILE* probe = fopen(out_path, "wx");
        if (probe) {
            fclose(probe);
            created = true;
            break;
        }
        int saved_errno = errno;
        (void)rmdir(dir);
        if (saved_errno != EEXIST)
            break;
    }
    free(clean);
    if (!created)
        return -1;

    CURL* curl = curl_easy_init();
    if (!curl) {
        download_fetch_cleanup(out_path);
        return -1;
    }

    FILE* f = fopen(out_path, "wb");
    if (!f) {
        curl_easy_cleanup(curl);
        download_fetch_cleanup(out_path);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    fclose(f);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        download_fetch_cleanup(out_path);
        return -1;
    }
    return 0;
}

void download_fetch_cleanup(const char* path) {
    if (!path || path[0] == '\0')
        return;

    (void)remove(path);
    const char* slash = strrchr(path, '/');
    if (slash && slash != path) {
        char dir[PATH_MAX];
        size_t len = (size_t)(slash - path);
        if (len < sizeof dir) {
            memcpy(dir, path, len);
            dir[len] = '\0';
            (void)rmdir(dir);
        }
    }
}
