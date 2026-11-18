#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * notify.c - shared outbound HTTP notifications and file uploads.
 *
 * Implements notify_post() (POST {"source","message"} JSON) and send_file()
 * (POST multipart/form-data file upload) over plain HTTP using blocking
 * BSD sockets with fixed timeouts. No TLS, no libcurl: any service in this
 * repo can use it without extra build dependencies.
 *
 * Both functions are silent no-ops for NULL/empty URLs and report success,
 * so a misconfigured or dead endpoint never blocks the caller.
 */

#include "notify.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* Timeouts for the outbound request, in seconds. */
#define NOTIFY_CONNECT_TIMEOUT_SEC 5
#define NOTIFY_TIMEOUT_SEC 10
/* Maximum size of the JSON request body we are willing to send. */
#define NOTIFY_BODY_MAX 8192
/* Default HTTP port when the URL omits one. */
#define NOTIFY_DEFAULT_PORT 80
/* Multipart/form-data boundary for send_file(). */
#define NOTIFY_BOUNDARY "----NotifyBoundaryPb34567890"

typedef struct {
    char host[256];
    unsigned short port;
    char path[1024];
} notify_url_t;

/* ---------- URL parsing ---------- */

/* Split `url` ("[http://]host[:port][/path]") into host, port (default 80)
 * and path (always starting with "/"). https is rejected: no TLS support.
 * Returns true on success. */
static bool parse_notify_url(const char* url, notify_url_t* out) {
    if (!url || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->port = NOTIFY_DEFAULT_PORT;

    const char* rest = url;
    if (strncasecmp(rest, "http://", 7) == 0)
        rest += 7;
    else if (strncasecmp(rest, "https://", 8) == 0) {
        fprintf(stderr, "notify: https is not supported (no TLS): %s\n", url);
        return false;
    }

    const char* slash = strchr(rest, '/');
    size_t auth_len = slash ? (size_t)(slash - rest) : strlen(rest);
    if (auth_len == 0 || auth_len >= sizeof(out->host))
        return false;
    memcpy(out->host, rest, auth_len);
    out->host[auth_len] = '\0';

    if (slash) {
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(out->path))
            return false;
        memcpy(out->path, slash, path_len + 1);
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    char* colon = strrchr(out->host, ':');
    if (colon) {
        *colon = '\0';
        if (out->host[0] == '\0' || colon[1] == '\0')
            return false;
        char* end = NULL;
        errno = 0;
        unsigned long port = strtoul(colon + 1, &end, 10);
        if (errno != 0 || end == NULL || *end != '\0' || port < 1 || port > 65535)
            return false;
        out->port = (unsigned short)port;
    }
    return true;
}

/* ---------- JSON ---------- */

/* Escape `src` as a JSON string into `dst` (capacity `dst_cap`).
 * Returns the number of bytes written (excluding NUL), or -1 if it does not
 * fit. */
static int json_escape_string(const char* src, char* dst, size_t dst_cap) {
    size_t j = 0;
    for (const char* s = src; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        const char* rep = NULL;
        switch (c) {
        case '"': rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        case '\b': rep = "\\b"; break;
        case '\f': rep = "\\f"; break;
        default:
            if (c < 0x20) {
                int n = snprintf(dst + j, dst_cap - j, "\\u%04x", c);
                if (n < 0 || (size_t)n >= dst_cap - j)
                    return -1;
                j += (size_t)n;
                continue;
            }
            if (dst_cap - j < 2)
                return -1;
            dst[j++] = (char)c;
            continue;
        }
        size_t len = strlen(rep);
        if (dst_cap - j < len + 1)
            return -1;
        memcpy(dst + j, rep, len);
        j += len;
    }
    dst[j] = '\0';
    return (int)j;
}

/* ---------- socket I/O ---------- */

static bool send_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool set_timeouts(int fd, int sec) {
    struct timeval tv = {.tv_sec = sec, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return false;
    return true;
}

/* Connect to host:port with a timeout. Returns a socket fd (blocking mode
 * restored) or -1 on failure. */
static int notify_connect(const char* host, unsigned short port, int timeout_sec) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "notify: getaddrinfo(%s): %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        perror("notify: socket");
        freeaddrinfo(res);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        perror("notify: connect");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    if (rc < 0) { /* EINPROGRESS: wait for the socket to become writable */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = {.tv_sec = timeout_sec, .tv_usec = 0};
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            if (rc == 0)
                fprintf(stderr, "notify: connect to %s:%u timed out\n", host, port);
            else
                perror("notify: select");
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
        int soerr = 0;
        socklen_t soerr_len = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) < 0 || soerr != 0) {
            fprintf(stderr, "notify: connect to %s:%u failed: %s\n", host, port,
                    soerr ? strerror(soerr) : "unknown error");
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
    }

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags); /* back to blocking */

    freeaddrinfo(res);
    return fd;
}

static bool send_request(int fd, const char* path, const char* host, unsigned short port,
                         const char* body, size_t body_length) {
    char head[512];
    int head_len = snprintf(head, sizeof(head),
                            "POST %s HTTP/1.1\r\n"
                            "Host: %s:%u\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            path, host, (unsigned int)port, body_length);
    if (head_len < 0 || (size_t)head_len >= sizeof(head))
        return false;
    if (!send_all(fd, head, (size_t)head_len))
        return false;
    if (body_length > 0 && !send_all(fd, body, body_length))
        return false;
    return true;
}

static int read_headers(int fd, char* buf, size_t cap) {
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        total += (size_t)n;
        if (total >= 4 && buf[total - 4] == '\r' && buf[total - 3] == '\n' &&
            buf[total - 2] == '\r' && buf[total - 1] == '\n') {
            buf[total] = '\0';
            return 0;
        }
    }
    return -1;
}

static int parse_status(const char* headers) {
    const char* line = strstr(headers, "HTTP/");
    if (!line)
        return -1;
    line += 5;
    while (*line == ' ')
        line++;
    while (*line && *line != ' ')
        line++;
    while (*line == ' ')
        line++;
    return atoi(line);
}

/* ---------- notify_post ---------- */

bool notify_post(const char* url, const char* source, const char* message) {
    if (!url || url[0] == '\0')
        return true; /* no notification endpoint configured */
    if (!source || source[0] == '\0' || !message || message[0] == '\0')
        return true;

    notify_url_t parsed;
    if (!parse_notify_url(url, &parsed))
        return false;

    char e_source[NOTIFY_BODY_MAX / 4];
    char e_msg[NOTIFY_BODY_MAX / 2];
    if (json_escape_string(source, e_source, sizeof(e_source)) < 0 ||
        json_escape_string(message, e_msg, sizeof(e_msg)) < 0) {
        fprintf(stderr, "notify: failed to escape source or message\n");
        return false;
    }

    char body[NOTIFY_BODY_MAX];
    int body_length = snprintf(body, sizeof(body),
                               "{\"source\": \"%s\", \"message\": \"%s\"}",
                               e_source, e_msg);
    if (body_length < 0 || (size_t)body_length >= sizeof(body)) {
        fprintf(stderr, "notify: failed to build request body\n");
        return false;
    }

    int fd = notify_connect(parsed.host, parsed.port, NOTIFY_CONNECT_TIMEOUT_SEC);
    if (fd < 0)
        return false;

    bool ok = false;
    if (!set_timeouts(fd, NOTIFY_TIMEOUT_SEC) ||
        !send_request(fd, parsed.path, parsed.host, parsed.port, body, (size_t)body_length)) {
        goto done;
    }

    char headers[2048];
    if (read_headers(fd, headers, sizeof(headers)) < 0)
        goto done;

    int status_code = parse_status(headers);
    ok = (status_code >= 200 && status_code < 300);
    if (!ok)
        fprintf(stderr, "notify: endpoint returned HTTP %d\n", status_code);

done:
    close(fd);
    return ok;
}

/* ---------- send_file ---------- */

/* POST the file at `file_path` to `url` as multipart/form-data. */
bool send_file(const char* url, const char* file_path, const char* field_name) {
    if (!url || url[0] == '\0' || !file_path || file_path[0] == '\0' ||
        !field_name || field_name[0] == '\0')
        return true;

    notify_url_t parsed;
    if (!parse_notify_url(url, &parsed))
        return false;

    /* Base name (last path component) for the filename field. */
    const char* base = file_path;
    const char* slash = strrchr(file_path, '/');
    if (slash)
        base = slash + 1;

    /* Read the whole file into memory. */
    FILE* f = fopen(file_path, "rb");
    if (!f) {
        fprintf(stderr, "send_file: cannot open %s: %s\n", file_path, strerror(errno));
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    char* fdata = malloc((size_t)fsize + 1);
    if (!fdata) {
        fclose(f);
        return false;
    }
    size_t nread = fread(fdata, 1, (size_t)fsize, f);
    fclose(f);
    fdata[nread] = '\0';

    /* Build the multipart/form-data body.
     * --boundary\r\n
     * Content-Disposition: form-data; name="field"; filename="base"\r\n
     * Content-Type: application/octet-stream\r\n
     * \r\n
     * <file bytes>\r\n
     * --boundary--\r\n */
    char head_part[1024];
    int head_len = snprintf(head_part, sizeof(head_part),
                            "--%s\r\n"
                            "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
                            "Content-Type: application/octet-stream\r\n"
                            "\r\n",
                            NOTIFY_BOUNDARY, field_name, base);
    if (head_len < 0 || (size_t)head_len >= sizeof(head_part)) {
        free(fdata);
        fprintf(stderr, "send_file: multipart header too large\n");
        return false;
    }

    size_t tail_len = strlen(NOTIFY_BOUNDARY) + 8; /* "--" + boundary + "--\r\n" */
    size_t body_len = (size_t)head_len + nread + tail_len;

    char* body = malloc(body_len + 1);
    if (!body) {
        free(fdata);
        return false;
    }
    char* p = body;
    memcpy(p, head_part, (size_t)head_len);
    p += head_len;
    memcpy(p, fdata, nread);
    p += nread;
    int n = snprintf(p, tail_len + 1, "\r\n--%s--\r\n", NOTIFY_BOUNDARY);
    if (n < 0 || (size_t)n >= tail_len + 1) {
        free(fdata);
        free(body);
        fprintf(stderr, "send_file: failed to build multipart tail\n");
        return false;
    }
    body[body_len] = '\0';
    free(fdata);

    int fd = notify_connect(parsed.host, parsed.port, NOTIFY_CONNECT_TIMEOUT_SEC);
    if (fd < 0) {
        free(body);
        return false;
    }

    bool ok = false;
    if (!set_timeouts(fd, NOTIFY_TIMEOUT_SEC))
        goto done;

    char head[512];
    int head_len2 = snprintf(head, sizeof(head),
                             "POST %s HTTP/1.1\r\n"
                             "Host: %s:%u\r\n"
                             "Content-Type: multipart/form-data; boundary=%s\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             parsed.path, parsed.host, (unsigned int)parsed.port,
                             NOTIFY_BOUNDARY, body_len);
    if (head_len2 < 0 || (size_t)head_len2 >= sizeof(head))
        goto done;
    if (!send_all(fd, head, (size_t)head_len2))
        goto done;
    if (!send_all(fd, body, body_len))
        goto done;

    char headers[2048];
    if (read_headers(fd, headers, sizeof(headers)) < 0)
        goto done;

    int status_code = parse_status(headers);
    ok = (status_code >= 200 && status_code < 300);
    if (!ok)
        fprintf(stderr, "send_file: endpoint returned HTTP %d\n", status_code);

done:
    close(fd);
    free(body);
    return ok;
}