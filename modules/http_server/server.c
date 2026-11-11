#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "json.h"
#include "download.h"
#include "jobmap.h"

/* Cleared by the SIGINT/SIGTERM handler to unwind the accept loop. */
volatile sig_atomic_t g_keep_running = 1;

/*
 * Returned by read_http_request() when the connection must be dropped without
 * a response: socket timeout, peer closed early, or a socket error. Any other
 * non-negative value is an HTTP status the caller should send back.
 */
#define READ_ABORT (-1)

#define MAX_METHOD_LEN 16
#define MAX_PATH_LEN 256
#define MAX_TELEGRAM_RESPONSE 1024

/* ---------- status helpers ---------- */

static const char *status_reason(int code) {
    switch (code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Payload Too Large";
    case 429:
        return "Too Many Requests";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    default:
        return "Unknown";
    }
}

static void set_response(http_response_t *response, int status) {
    int written = snprintf(response->body, sizeof(response->body),
                           "{\"status\": %d, \"message\": \"%s\"}", status,
                           status_reason(status));
    if (written < 0) {
        written = 0;
    } else if ((size_t)written >= sizeof(response->body)) {
        written = (int)sizeof(response->body) - 1;
    }
    response->body_length = (size_t)written;
    response->status_code = status;
}

/* ---------- header parsing ---------- */

/* Locate the "\r\n\r\n" terminator within the first `len` bytes of `buf`. */
static char *find_header_end(char *buf, size_t len) {
    if (len < 4)
        return NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

/*
 * Parse "METHOD SP /path SP HTTP/x.y" into its method and path components.
 * Any query string or fragment is stripped; `line` must be NUL-terminated.
 */
static bool parse_request_line(const char *line, char *method,
                               size_t method_cap, char *path, size_t path_cap) {
    const char *sp1 = strchr(line, ' ');
    if (!sp1 || sp1 == line)
        return false;

    size_t method_len = (size_t)(sp1 - line);
    if (method_len >= method_cap)
        return false;
    memcpy(method, line, method_len);
    method[method_len] = '\0';

    const char *target = sp1 + 1;
    const char *sp2 = strchr(target, ' ');
    size_t path_len = sp2 ? (size_t)(sp2 - target) : strlen(target);

    /* Strip the query string / fragment so "/x?y=1" routes as "/x". */
    for (size_t i = 0; i < path_len; i++) {
        if (target[i] == '?' || target[i] == '#') {
            path_len = i;
            break;
        }
    }
    if (path_len == 0 || path_len >= path_cap)
        return false;

    memcpy(path, target, path_len);
    path[path_len] = '\0';
    return path[0] == '/';
}

typedef enum {
    CL_ABSENT,   /* no Content-Length header present */
    CL_PRESENT,  /* parsed into *out_value */
    CL_MALFORMED /* header present but not a valid non-negative integer */
} content_length_state_t;

static content_length_state_t parse_content_length(const char *headers,
                                                   long *out_value) {
    const char *eol = strstr(headers, "\r\n");
    if (!eol)
        return CL_ABSENT;
    const char *line = eol + 2; /* skip the request line */

    while (*line) {
        /*
         * A line ends at a CRLF or at the end of the block: the caller may
         * have stripped the final CRLF when terminating the buffer, so the
         * last header line must still be recognised.
         */
        const char *crlf = strstr(line, "\r\n");
        size_t line_len = crlf ? (size_t)(crlf - line) : strlen(line);
        if (line_len == 0)
            break; /* empty line: end of the header block */

        const char *colon = memchr(line, ':', line_len);
        if (colon) {
            size_t name_len = (size_t)(colon - line);
            while (name_len > 0 &&
                   (line[name_len - 1] == ' ' || line[name_len - 1] == '\t')) {
                name_len--;
            }
            if (name_len == 14 &&
                strncasecmp(line, "Content-Length", 14) == 0) {
                const char *value = colon + 1;
                size_t value_len = line_len - name_len - 1;
                while (value_len > 0 && (*value == ' ' || *value == '\t')) {
                    value++;
                    value_len--;
                }
                while (value_len > 0 && (value[value_len - 1] == ' ' ||
                                         value[value_len - 1] == '\t')) {
                    value_len--;
                }
                if (value_len == 0)
                    return CL_MALFORMED;

                long parsed = 0;
                for (size_t i = 0; i < value_len; i++) {
                    if (value[i] < '0' || value[i] > '9')
                        return CL_MALFORMED;
                    if (parsed > (LONG_MAX - (value[i] - '0')) / 10)
                        return CL_MALFORMED;
                    parsed = parsed * 10 + (value[i] - '0');
                }
                *out_value = parsed;
                return CL_PRESENT;
            }
        }

        if (!crlf)
            break; /* last line had no trailing CRLF */
        line = crlf + 2;
    }

    return CL_ABSENT;
}

/*
 * Send a reply to the chat that originated the webhook update. Failures are
 * logged only; the webhook still acknowledges 200 to Telegram so it does not
 * retry.
 */
static void send_telegram_reply(const server_config_t *config,
                                const char *chat_id, const char *text) {
    char telegram_response[MAX_TELEGRAM_RESPONSE];
    int status = telegram_send_message_to(config->telegram_config.token,
                                          chat_id, text, telegram_response,
                                          sizeof(telegram_response));
    if (status < 0) {
        fprintf(stderr, "notification-service: telegram reply failed\n");
    } else if (status != 200 && telegram_response[0] != '\0') {
        fprintf(stderr, "notification-service: telegram replied %d: %s\n",
                status, telegram_response);
    }
}

/*
 * Handle a Telegram webhook update. This step handles the /download command:
 * extract the chat id and text, forward the url to the download-service, reply
 * to the user, and record the job_id -> chat_id mapping for the callback.
 */
static void handle_webhook(const server_config_t *config,
                           const char *body, size_t body_length,
                           http_response_t *response) {
    (void)body_length;

    char body_copy[MAX_HTTP_BODY + 1];
    size_t copy_len =
        body_length > (size_t)MAX_HTTP_BODY ? (size_t)MAX_HTTP_BODY
                                            : body_length;
    if (copy_len > 0 && body) {
        memcpy(body_copy, body, copy_len);
    }
    body_copy[copy_len] = '\0';

    char chat_id_str[32];
    char text[MAX_TELEGRAM_MSG_LEN + 1];
    if (!json_extract_telegram_update(body_copy, chat_id_str,
                                      sizeof(chat_id_str), text,
                                      sizeof(text))) {
        set_response(response, 400);
        return;
    }

    /* Only the /download command is handled in this step. */
    if (strncmp(text, "/download", 9) != 0) {
        set_response(response, 200);
        response->body_length = 0;
        return;
    }

    const char *arg = text + 9;
    if (arg[0] == '\0' || arg[0] != ' ') {
        const char *usage = "Usage: /download <url>";
        send_telegram_reply(config, chat_id_str, usage);
        set_response(response, 200);
        response->body_length = 0;
        return;
    }
    do {
        arg++;
    } while (*arg == ' ');
    if (*arg == '\0') {
        const char *usage = "Usage: /download <url>";
        send_telegram_reply(config, chat_id_str, usage);
        set_response(response, 200);
        response->body_length = 0;
        return;
    }

    int64_t chat_id = strtoll(chat_id_str, NULL, 10);
    char job_id[37];
    int rc = download_submit(chat_id, arg, job_id, sizeof(job_id));

    char reply[512];
    if (rc == 0) {
        jobmap_put(job_id, chat_id);
        int n = snprintf(reply, sizeof(reply), "Download started (job %s).",
                         job_id);
        if (n < 0 || (size_t)n >= sizeof(reply)) {
            snprintf(reply, sizeof(reply), "Download started.");
        }
    } else if (rc == 1) {
        snprintf(reply, sizeof(reply), "Unsupported domain.");
    } else {
        snprintf(reply, sizeof(reply), "Failed to start the download.");
    }

    send_telegram_reply(config, chat_id_str, reply);
    set_response(response, 200);
    response->body_length = 0;
}

/*
 * Handle a download-service callback: POST /callback/download with body
 *   { "job_id": "...", "chat_id": 123, "status": "completed|failed",
 *     "file_path": "...", "error": "..." }
 *
 * Look up the originating chat by job_id (the mapping recorded when the job
 * was accepted), deliver the result to the Telegram user - the file path on
 * success, the error message on failure - then drop the job mapping. The flow
 * ends here. Acknowledge with 200 so the download-service stops retrying the
 * callback; delivery failures are logged only.
 */
static void handle_callback(const server_config_t *config,
                            const char *body, size_t body_length,
                            http_response_t *response) {
    (void)body_length;

    char body_copy[MAX_HTTP_BODY + 1];
    size_t copy_len =
        body_length > (size_t)MAX_HTTP_BODY ? (size_t)MAX_HTTP_BODY
                                            : body_length;
    if (copy_len > 0 && body) {
        memcpy(body_copy, body, copy_len);
    }
    body_copy[copy_len] = '\0';

    char job_id[37] = "";
    if (!json_find_top_level_string(body_copy, "job_id", job_id,
                                    sizeof(job_id)) ||
        job_id[0] == '\0') {
        set_response(response, 400);
        return;
    }

    /*
     * The job_id -> chat_id map is authoritative: the callback tells us which
     * job finished, and the map tells us which chat it belongs to. Unknown
     * jobs are rejected before any delivery is attempted.
     */
    int64_t chat_id = jobmap_get(job_id);
    if (chat_id == 0) {
        set_response(response, 400);
        return;
    }

    char status[16] = "";
    char file_path[PATH_MAX] = "";
    char error[256] = "";
    (void)json_find_top_level_string(body_copy, "status", status,
                                     sizeof(status));
    (void)json_find_top_level_string(body_copy, "file_path", file_path,
                                     sizeof(file_path));
    (void)json_find_top_level_string(body_copy, "error", error,
                                     sizeof(error));

    char text[MAX_TELEGRAM_MSG_LEN + 1];
    int n;
    if (strcmp(status, "completed") == 0) {
        if (file_path[0] != '\0')
            n = snprintf(text, sizeof(text), "Download complete.\nFile: %s",
                         file_path);
        else
            n = snprintf(text, sizeof(text), "Download complete.");
    } else {
        if (error[0] != '\0')
            n = snprintf(text, sizeof(text), "Download failed.\n%s", error);
        else
            n = snprintf(text, sizeof(text), "Download failed.");
    }
    if (n < 0 || (size_t)n >= sizeof(text)) {
        n = snprintf(text, sizeof(text), "Download finished.");
    }

    char chat_id_str[32];
    snprintf(chat_id_str, sizeof(chat_id_str), "%lld", (long long)chat_id);

    char telegram_response[MAX_TELEGRAM_RESPONSE];
    int tg_status = telegram_send_message_to(
        config->telegram_config.token, chat_id_str, text, telegram_response,
        sizeof(telegram_response));
    if (tg_status < 0) {
        fprintf(stderr,
                "notification-service: callback delivery to chat %s failed\n",
                chat_id_str);
    } else if (tg_status != 200 && telegram_response[0] != '\0') {
        fprintf(stderr,
                "notification-service: callback delivery replied %d: %s\n",
                tg_status, telegram_response);
    }

    /* Flow ends here: the job is no longer tracked. */
    jobmap_remove(job_id);

    set_response(response, 200);
    response->body_length = 0;
}

/* ---------- socket I/O ---------- */

static bool send_all(int fd, const char *data, size_t len) {
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

static void send_http_response(int fd, const http_response_t *response) {
    char head[256];
    int head_len =
        snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 response->status_code, status_reason(response->status_code),
                 response->body_length);
    if (head_len < 0)
        return;
    if ((size_t)head_len >= sizeof(head))
        head_len = (int)sizeof(head) - 1;

    if (!send_all(fd, head, (size_t)head_len))
        return;
    if (response->body_length > 0) {
        send_all(fd, response->body, response->body_length);
    }
}

/*
 * Frame one request off the socket.
 *
 * Returns 0 on success (with *body_len_out set), READ_ABORT to drop the
 * connection silently, or an HTTP status to report a framing error.
 */
static int read_http_request(int fd, char *header_buf, char *body_buf,
                             size_t *body_len_out) {
    size_t total = 0;
    char *header_end = NULL;

    *body_len_out = 0;

    /* --- header block, bounded by MAX_HEADER_SIZE --- */
    for (;;) {
        if (total >= MAX_HEADER_SIZE)
            return 431;

        ssize_t n = recv(fd, header_buf + total, MAX_HEADER_SIZE - total, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return READ_ABORT; /* recv timeout (SO_RCVTIMEO) or socket error */
        }
        if (n == 0)
            return READ_ABORT; /* peer closed before headers completed */

        total += (size_t)n;
        header_end = find_header_end(header_buf, total);
        if (header_end)
            break;
    }

    size_t headers_len = (size_t)(header_end - header_buf) + 4;
    header_buf[headers_len - 4] =
        '\0'; /* terminate the block for the parsers */

    /*
     * The recv() that completed "\r\n\r\n" almost always over-read into the
     * body. Preserve those bytes instead of discarding them.
     */
    size_t body_already = total - headers_len;
    if (body_already > 0) {
        memcpy(body_buf, header_buf + headers_len, body_already);
    }

    long content_length = 0;
    content_length_state_t cl_state =
        parse_content_length(header_buf, &content_length);
    if (cl_state == CL_MALFORMED)
        return 400;

    size_t body_len;
    if (cl_state == CL_PRESENT) {
        if (content_length < 0 ||
            (unsigned long)content_length > (unsigned long)MAX_HTTP_BODY) {
            return 413;
        }
        body_len = (size_t)content_length;
    } else {
        /*
         * No Content-Length: the body is whatever arrived alongside the
         * headers. That cannot exceed MAX_HEADER_SIZE, but check anyway so the
         * bound on body_buf is enforced in one place regardless of framing.
         */
        if (body_already > (size_t)MAX_HTTP_BODY)
            return 413;
        body_len = body_already;
    }

    /* --- body, up to Content-Length bytes --- */
    size_t have = body_already;
    while (have < body_len) {
        ssize_t n = recv(fd, body_buf + have, body_len - have, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return READ_ABORT; /* recv timeout part-way through the body */
        }
        if (n == 0)
            return READ_ABORT; /* truncated body */
        have += (size_t)n;
    }

    body_buf[body_len] = '\0';
    *body_len_out = body_len;
    return 0;
}

/* ---------- routing ---------- */

void request_handler_dispatch(const server_config_t *config,
                              const char *headers, const char *body,
                              size_t body_length, http_response_t *response) {
    if (!response)
        return;
    if (!config || !headers) {
        set_response(response, 500);
        return;
    }

    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    if (!parse_request_line(headers, method, sizeof(method), path,
                            sizeof(path))) {
        set_response(response, 400);
        return;
    }

    const bool is_notification = strcmp(path, NOTIFICATION_PATH) == 0;
    const bool is_webhook = strcmp(path, WEBHOOK_PATH) == 0;
    const bool is_callback = strcmp(path, CALLBACK_PATH) == 0;
    if (!is_notification && !is_webhook && !is_callback) {
        /* Path first: unknown paths are 404 regardless of method. */
        set_response(response, 404);
        return;
    }
    if (strcmp(method, "POST") != 0) {
        set_response(response, 405);
        return;
    }

    /* Content-Length framing (shared by both POST routes). */
    long content_length = 0;
    content_length_state_t cl_state =
        parse_content_length(headers, &content_length);
    if (cl_state == CL_MALFORMED) {
        set_response(response, 400);
        return;
    }
    if (cl_state == CL_ABSENT) {
        set_response(response, 411);
        return;
    }
    if (content_length < 0 ||
        (unsigned long)content_length > (unsigned long)MAX_HTTP_BODY) {
        set_response(response, 413);
        return;
    }
    /* The framing layer and the router must agree on the body size. */
    if ((unsigned long)content_length != (unsigned long)body_length) {
        set_response(response, 400);
        return;
    }

    /* NUL-terminate a private copy so the JSON parsers get a C string. */
    char body_copy[MAX_HTTP_BODY + 1];
    size_t copy_len = (body_length > (size_t)MAX_HTTP_BODY)
                          ? (size_t)MAX_HTTP_BODY
                          : body_length;
    if (copy_len > 0 && body) {
        memcpy(body_copy, body, copy_len);
    }
    body_copy[copy_len] = '\0';

    if (is_notification) {
        notification_payload_t payload;
        if (!json_parse_request(body_copy, &payload)) {
            set_response(response, 400);
            return;
        }

        char telegram_response[MAX_TELEGRAM_RESPONSE];
        int status = telegram_send_message(&config->telegram_config, &payload,
                                           telegram_response,
                                           sizeof(telegram_response));
        if (status < 0) {
            fprintf(stderr,
                    "telegram request failed (network or TLS error)\n");
            set_response(response, 502);
            return;
        }
        if (status != 200 && telegram_response[0] != '\0') {
            fprintf(stderr, "telegram returned %d: %s\n", status,
                    telegram_response);
        }

        set_response(response, status);
    } else if (is_webhook) {
        handle_webhook(config, body_copy, body_length, response);
    } else {
        handle_callback(config, body_copy, body_length, response);
    }
}

/* ---------- accept loop ---------- */

int request_handler_start(const server_config_t *config) {
    if (!config)
        return -1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
        0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config->port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    char *header_buf = malloc(MAX_HEADER_SIZE);
    char *body_buf = malloc((size_t)MAX_HTTP_BODY + 1);
    if (!header_buf || !body_buf) {
        perror("malloc");
        free(header_buf);
        free(body_buf);
        close(server_fd);
        return -1;
    }

    fprintf(stderr, "notification-service listening on 0.0.0.0:%u\n",
            (unsigned int)config->port);

    while (g_keep_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue; /* signal: re-check g_keep_running */
            if (errno == EBADF || errno == EINVAL)
                break;
            continue; /* EMFILE, ECONNABORTED, ... — keep serving */
        }

        struct timeval tv = {.tv_sec = SOCKET_TIMEOUT_SEC, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        size_t body_length = 0;
        int status =
            read_http_request(client_fd, header_buf, body_buf, &body_length);

        if (status == READ_ABORT) {
            close(client_fd);
            continue;
        }

        http_response_t response;
        if (status == 0) {
            request_handler_dispatch(config, header_buf, body_buf, body_length,
                                     &response);
        } else {
            set_response(&response, status);
        }

        send_http_response(client_fd, &response);
        close(client_fd); /* Connection: close */
    }

    fprintf(stderr, "notification-service shutting down\n");

    free(header_buf);
    free(body_buf);
    close(server_fd);
    return 0;
}

