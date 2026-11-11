#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "callback.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* Timeouts for the callback request, in seconds. */
#define CALLBACK_CONNECT_TIMEOUT_SEC 5
#define CALLBACK_TIMEOUT_SEC 10
/* Maximum size of the JSON request body we are willing to send. */
#define CALLBACK_BODY_MAX 16384
/* Default HTTP port when the URL omits one. */
#define CALLBACK_DEFAULT_PORT 80

/* Maximum lengths of the URL authority (host[:port]) and path we accept. */
#define CALLBACK_AUTHORITY_MAX 256
#define CALLBACK_PATH_MAX 1024

typedef struct {
    char host[CALLBACK_AUTHORITY_MAX];
    unsigned short port;
    char path[CALLBACK_PATH_MAX];
} callback_url_t;

/* ---------- URL parsing ---------- */

/* Split `url` ("[http://]host[:port][/path]") into host, port (default 80)
 * and path (always starting with "/"). https is rejected: the download
 * service has no TLS support. Returns true on success. */
static bool parse_callback_url(const char* url, callback_url_t* out) {
    if (!url || !out)
        return false;
    memset(out, 0, sizeof(*out));
    out->port = CALLBACK_DEFAULT_PORT;

    const char* rest = url;
    if (strncasecmp(rest, "http://", 7) == 0)
        rest += 7;
    else if (strncasecmp(rest, "https://", 8) == 0) {
        fprintf(stderr, "callback: https is not supported (no TLS): %s\n", url);
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
            case '"':
                rep = "\\\"";
                break;
            case '\\':
                rep = "\\\\";
                break;
            case '\n':
                rep = "\\n";
                break;
            case '\r':
                rep = "\\r";
                break;
            case '\t':
                rep = "\\t";
                break;
            case '\b':
                rep = "\\b";
                break;
            case '\f':
                rep = "\\f";
                break;
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

/* Build the callback request body into `out` (capacity `cap`).
 * file_path is included only when non-empty; error only when non-empty.
 * Returns the body length (excluding NUL), or -1 if it does not fit. */
static int build_callback_body(const char* job_id, int chat_id, const char* status,
                               const char* file_path, const char* error, char* out, size_t cap) {
    char e_job_id[128];
    char e_status[16];
    char e_file_path[PATH_MAX * 2 + 16];
    char e_error[256 * 2 + 16];

    if (json_escape_string(job_id, e_job_id, sizeof(e_job_id)) < 0)
        return -1;
    if (json_escape_string(status, e_status, sizeof(e_status)) < 0)
        return -1;

    int have_file_path = -1;
    if (file_path && file_path[0] != '\0') {
        have_file_path = json_escape_string(file_path, e_file_path, sizeof(e_file_path));
        if (have_file_path < 0)
            return -1;
    }
    int have_error = -1;
    if (error && error[0] != '\0') {
        have_error = json_escape_string(error, e_error, sizeof(e_error));
        if (have_error < 0)
            return -1;
    }

    size_t pos = 0;
    int n;

    n = snprintf(out + pos, cap - pos,
                 "{\"job_id\": \"%s\", \"chat_id\": %d, \"source\": "
                 "\"download-service\", \"status\": \"%s\"",
                 e_job_id, chat_id, e_status);
    if (n < 0 || (size_t)n >= cap - pos)
        return -1;
    pos += (size_t)n;

    if (have_file_path >= 0) {
        n = snprintf(out + pos, cap - pos, ", \"file_path\": \"%s\"", e_file_path);
        if (n < 0 || (size_t)n >= cap - pos)
            return -1;
        pos += (size_t)n;
    }
    if (have_error >= 0) {
        n = snprintf(out + pos, cap - pos, ", \"error\": \"%s\"", e_error);
        if (n < 0 || (size_t)n >= cap - pos)
            return -1;
        pos += (size_t)n;
    }

    n = snprintf(out + pos, cap - pos, "}");
    if (n < 0 || (size_t)n >= cap - pos)
        return -1;
    pos += (size_t)n;
    out[pos] = '\0';
    return (int)pos;
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
static int callback_connect(const char* host, unsigned short port, int timeout_sec) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "callback: getaddrinfo(%s): %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        perror("callback: socket");
        freeaddrinfo(res);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        perror("callback: connect");
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
                fprintf(stderr, "callback: connect to %s:%u timed out\n", host, port);
            else
                perror("callback: select");
            close(fd);
            freeaddrinfo(res);
            return -1;
        }
        int soerr = 0;
        socklen_t soerr_len = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) < 0 || soerr != 0) {
            fprintf(stderr, "callback: connect to %s:%u failed: %s\n", host, port,
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

/* Read header bytes until "\r\n\r\n" or until `cap` is exhausted.
 * Returns 0 on success, -1 on error, timeout, or oversized headers. */
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
            return -1; /* peer closed before headers completed */
        total += (size_t)n;
        if (total >= 4 && buf[total - 4] == '\r' && buf[total - 3] == '\n' &&
            buf[total - 2] == '\r' && buf[total - 1] == '\n') {
            buf[total] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Extract the status code from an "HTTP/1.1 200 OK" status line. */
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

/* ---------- public API ---------- */

bool callback_send(const char* callback_url, const char* job_id, int chat_id, const char* status,
                   const char* file_path, const char* error) {
    if (!callback_url || callback_url[0] == '\0')
        return true; /* no callback configured */
    if (!job_id || !status)
        return false;

    callback_url_t parsed;
    if (!parse_callback_url(callback_url, &parsed))
        return false;

    char body[CALLBACK_BODY_MAX];
    int body_length =
        build_callback_body(job_id, chat_id, status, file_path, error, body, sizeof(body));
    if (body_length < 0) {
        fprintf(stderr, "callback: failed to build request body\n");
        return false;
    }

    int fd = callback_connect(parsed.host, parsed.port, CALLBACK_CONNECT_TIMEOUT_SEC);
    if (fd < 0)
        return false;

    bool ok = false;
    if (!set_timeouts(fd, CALLBACK_TIMEOUT_SEC) ||
        !send_request(fd, parsed.path, parsed.host, parsed.port, body, (size_t)body_length)) {
        goto done;
    }

    char headers[2048];
    if (read_headers(fd, headers, sizeof(headers)) < 0)
        goto done;

    int status_code = parse_status(headers);
    ok = (status_code >= 200 && status_code < 300);
    if (!ok)
        fprintf(stderr, "callback: endpoint returned HTTP %d\n", status_code);

done:
    close(fd);
    return ok;
}
