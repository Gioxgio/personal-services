#include "http.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "log.h"

#define HTTP_MAX_BYTES   (8u << 20)
#define HTTP_TIMEOUT_SEC 30L

static const char *HTTP_UA =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";

void http_global_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup(void)
{
    curl_global_cleanup();
}

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    http_buffer *buf = userdata;
    size_t n = size * nmemb;

    if (buf->len + n > HTTP_MAX_BYTES)
        return 0; /* abort: we only need a marker */

    char *p = realloc(buf->data, buf->len + n + 1);
    if (!p)
        return 0;

    buf->data = p;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

static int perform(const char *method, const char *url,
                   const char *const *headers, const char *postfields,
                   http_buffer *out, long *status_out)
{
    /*
     * Deterministic buffer contract: whatever the caller handed us is
     * dropped up front, so *out is empty unless we return success. A partial
     * body is never left behind for a caller to misread as a whole response.
     */
    *out = (http_buffer){ 0 };
    if (status_out)
        *status_out = 0;

    CURL *cur = curl_easy_init();
    if (!cur) {
        logmsg(LOG_ERROR, "curl_easy_init failed");
        return -1;
    }

    struct curl_slist *hdrs = NULL;
    for (const char *const *h = headers; h && *h; h++)
        hdrs = curl_slist_append(hdrs, *h);

    curl_easy_setopt(cur, CURLOPT_URL, url);
    curl_easy_setopt(cur, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(cur, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(cur, CURLOPT_TIMEOUT, HTTP_TIMEOUT_SEC);
    curl_easy_setopt(cur, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(cur, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(cur, CURLOPT_USERAGENT, HTTP_UA);
    curl_easy_setopt(cur, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(cur, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(cur, CURLOPT_WRITEDATA, out);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(cur, CURLOPT_POST, 1L);
        curl_easy_setopt(cur, CURLOPT_POSTFIELDS, postfields ? postfields : "");
    }

    CURLcode res = curl_easy_perform(cur);

    long status = 0;
    curl_easy_getinfo(cur, CURLINFO_RESPONSE_CODE, &status);
    if (status_out)
        *status_out = status;

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(cur);

    if (res != CURLE_OK) {
        logmsg(LOG_ERROR, "%s %s failed: %s", method, url, curl_easy_strerror(res));
        http_buffer_free(out);
        return -1;
    }
    if (status != 200) {
        logmsg(LOG_DEBUG, "%s %s -> HTTP %ld", method, url, status);
        http_buffer_free(out);
        return -1;
    }

    return 0;
}

int http_get_ex(const char *url, const char *const *headers,
                http_buffer *out, long *status_out)
{
    return perform("GET", url, headers, NULL, out, status_out);
}

int http_get(const char *url, const char *const *headers, http_buffer *out)
{
    return perform("GET", url, headers, NULL, out, NULL);
}

int http_post(const char *url, const char *const *headers,
              const char *postfields, http_buffer *out)
{
    return perform("POST", url, headers, postfields, out, NULL);
}

void http_buffer_free(http_buffer *buf)
{
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}