#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * test_callback.c
 *
 * Includes callback.c directly so the static URL parser and request builder
 * are reachable; callback.c itself is not linked separately.
 */
#include "callback.c"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define STUB_PORT_200 18942
#define STUB_PORT_500 18943
#define STUB_PORT_FAILED 18944

static int passed = 0;
static int failed = 0;

static void check(bool condition, const char *name) {
    if (condition) {
        printf("PASS: %s\n", name);
        passed++;
    } else {
        printf("FAIL: %s\n", name);
        failed++;
    }
}

/* ---------- stub callback endpoint ---------- */

/* A single-connection HTTP stub: captures the request, replies with the
 * configured status, then stops. Tests restart it per scenario. */
static pthread_t stub_thread;
static pthread_mutex_t stub_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stub_cond = PTHREAD_COND_INITIALIZER;
static volatile bool stub_started = false;
static volatile bool stub_done = false;
static int stub_want_status = 200;
static int stub_port_storage = 0;
static char stub_request[8192];
static size_t stub_request_len = 0;
static char stub_body[8192];
static size_t stub_body_len = 0;

static void *stub_server(void *arg) {
    int port = *(int *)arg;
    (void)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("stub: socket");
        return NULL;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(server_fd, 4) < 0) {
        perror("stub: bind/listen");
        close(server_fd);
        return NULL;
    }

    pthread_mutex_lock(&stub_mutex);
    stub_started = true;
    pthread_cond_broadcast(&stub_cond);
    pthread_mutex_unlock(&stub_mutex);

    int fd = accept(server_fd, NULL, NULL);
    if (fd >= 0) {
        size_t total = 0;
        while (total + 1 < sizeof(stub_request)) {
            ssize_t n = recv(fd, stub_request + total,
                             sizeof(stub_request) - 1 - total, 0);
            if (n <= 0)
                break;
            total += (size_t)n;
            if (total >= 4 &&
                stub_request[total - 4] == '\r' && stub_request[total - 3] == '\n' &&
                stub_request[total - 2] == '\r' && stub_request[total - 1] == '\n')
                break;
        }
        stub_request[total] = '\0';
        stub_request_len = total;

        /* Content-Length body, if any. */
        long cl = -1;
        const char *p = stub_request;
        while (*p) {
            if (strncasecmp(p, "Content-Length:", 15) == 0) {
                p += 15;
                while (*p == ' ' || *p == '\t')
                    p++;
                long v = 0;
                while (*p >= '0' && *p <= '9') {
                    v = v * 10 + (*p - '0');
                    p++;
                }
                cl = v;
                break;
            }
            const char *eol = strstr(p, "\r\n");
            if (!eol)
                break;
            p = eol + 2;
            if (*p == '\r' && p[1] == '\n')
                break; /* end of headers */
        }

        stub_body_len = 0;
        if (cl > 0) {
            const char *body_start = strstr(stub_request, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                size_t have = (size_t)(stub_request + total - body_start);
                if (have > (size_t)cl)
                    have = (size_t)cl;
                memcpy(stub_body, body_start, have);
                stub_body_len = have;
                while (stub_body_len < (size_t)cl) {
                    ssize_t n = recv(fd, stub_body + stub_body_len,
                                     sizeof(stub_body) - 1 - stub_body_len, 0);
                    if (n <= 0)
                        break;
                    stub_body_len += (size_t)n;
                }
                stub_body[stub_body_len] = '\0';
            }
        }

        pthread_mutex_lock(&stub_mutex);
        stub_done = true;
        pthread_cond_broadcast(&stub_cond);
        pthread_mutex_unlock(&stub_mutex);

        char resp[256];
        int rlen = snprintf(resp, sizeof(resp),
                            "HTTP/1.1 %d OK\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            stub_want_status);
        if (rlen > 0) {
            size_t sent = 0;
            while (sent < (size_t)rlen) {
                ssize_t n = send(fd, resp + sent, (size_t)rlen - sent,
                                 MSG_NOSIGNAL);
                if (n <= 0)
                    break;
                sent += (size_t)n;
            }
        }
        close(fd);
    }
    close(server_fd);
    return NULL;
}

static void stub_start(int port, int status) {
    pthread_mutex_lock(&stub_mutex);
    stub_started = false;
    stub_done = false;
    stub_want_status = status;
    stub_request[0] = '\0';
    stub_request_len = 0;
    stub_body[0] = '\0';
    stub_body_len = 0;
    stub_port_storage = port;
    pthread_mutex_unlock(&stub_mutex);

    pthread_create(&stub_thread, NULL, stub_server, &stub_port_storage);

    pthread_mutex_lock(&stub_mutex);
    while (!stub_started)
        pthread_cond_wait(&stub_cond, &stub_mutex);
    pthread_mutex_unlock(&stub_mutex);
}

static bool stub_wait_done(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;

    pthread_mutex_lock(&stub_mutex);
    while (!stub_done) {
        if (pthread_cond_timedwait(&stub_cond, &stub_mutex, &ts) != 0)
            break;
    }
    bool done = stub_done;
    pthread_mutex_unlock(&stub_mutex);
    return done;
}

/* ---------- tests ---------- */

static void test_no_callback_configured(void) {
    /* NULL or empty URL: no-op, reported as success, no network. */
    check(callback_send(NULL, "job-1", 1, "completed", NULL, NULL),
          "NULL callback URL is a no-op success");
    check(callback_send("", "job-1", 1, "completed", NULL, NULL),
          "empty callback URL is a no-op success");

    /* https has no TLS support in this service: rejected without connecting. */
    check(callback_send("https://127.0.0.1:18942/x", "job-1", 1,
                        "completed", NULL, NULL) == false,
          "https URL is rejected");

    /* Out-of-range port: rejected at parse time. */
    check(callback_send("http://127.0.0.1:99999/x", "job-1", 1,
                        "completed", NULL, NULL) == false,
          "out-of-range port is rejected");

    /* Connection refused: reported as failure, quickly (localhost). */
    check(callback_send("http://127.0.0.1:1/x", "job-1", 1,
                        "completed", NULL, NULL) == false,
          "connection refused is a failure");
}

static void test_completed_job(void) {
    stub_start(STUB_PORT_200, 200);

    bool ok = callback_send("http://127.0.0.1:18942/callback/download",
                            "test-job-123", 42, "completed",
                            "/data/downloads/abc/x.mp4", NULL);
    check(ok, "completed job: callback_send returns true");
    check(stub_wait_done(), "completed job: stub received the request");

    check(strstr(stub_request, "POST /callback/download HTTP/1.1") != NULL,
          "completed job: request line");
    check(strstr(stub_request, "Content-Type: application/json") != NULL,
          "completed job: JSON content type");
    check(strstr(stub_request, "Content-Length:") != NULL,
          "completed job: content length");

    check(strstr(stub_body, "\"job_id\": \"test-job-123\"") != NULL,
          "completed job: body has job_id");
    check(strstr(stub_body, "\"chat_id\": 42") != NULL,
          "completed job: body has chat_id");
    check(strstr(stub_body, "\"status\": \"completed\"") != NULL,
          "completed job: body has status");
    check(strstr(stub_body, "\"file_path\": \"/data/downloads/abc/x.mp4\"") != NULL,
          "completed job: body has file_path");
    check(strstr(stub_body, "\"error\"") == NULL,
          "completed job: body has no error field");

    pthread_join(stub_thread, NULL);
}

static void test_failed_job(void) {
    stub_start(STUB_PORT_500, 200);

    bool ok = callback_send("http://127.0.0.1:18943/callback/download",
                            "test-job-456", 7, "failed", NULL,
                            "download failed for https://youtu.be/x");
    check(ok, "failed job: callback_send returns true");
    check(stub_wait_done(), "failed job: stub received the request");

    check(strstr(stub_body, "\"status\": \"failed\"") != NULL,
          "failed job: body has status failed");
    check(strstr(stub_body,
                 "\"error\": \"download failed for https://youtu.be/x\"") != NULL,
          "failed job: body has error message");
    check(strstr(stub_body, "\"file_path\"") == NULL,
          "failed job: body has no file_path field");

    pthread_join(stub_thread, NULL);
}

static void test_non_2xx(void) {
    stub_start(STUB_PORT_FAILED, 500);

    bool ok = callback_send("http://127.0.0.1:18944/callback/download",
                            "test-job-789", 9, "completed", "/tmp/f.mp4",
                            NULL);
    check(ok == false, "non-2xx response is a failure");
    check(stub_wait_done(), "non-2xx: stub received the request");
    check(strstr(stub_request, "POST /callback/download HTTP/1.1") != NULL,
          "non-2xx: request line sent");

    pthread_join(stub_thread, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* SIGPIPE must not kill the test when the stub closes the socket. */
    signal(SIGPIPE, SIG_IGN);

    printf("=== callback unit tests ===\n");
    test_no_callback_configured();

    printf("=== callback integration tests (stub endpoint) ===\n");
    test_completed_job();
    test_failed_job();
    test_non_2xx();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
