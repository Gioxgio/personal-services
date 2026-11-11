/*
 * test_http - the http_buffer contract over a real HTTP
 * exchange, still offline.
 *
 * The roadmap names curl's data: URLs for this, but this
 * libcurl is built without the data: scheme (it rejects those
 * URLs with URL_MALFORMAT), and file: URLs report a response
 * code of 0, which perform()'s 200-only rule refuses. So the
 * test serves its fixtures from a loopback HTTP server running
 * in a thread: no external network, and real status codes -
 * including the 401-vs-transport distinction (status 0) that
 * probe.c's retry logic depends on.
 */

#include "harness.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"

/* --- the loopback server --- */

static int g_listen_fd = -1;
static int g_port;

/*
 * Stop flag, read by the server thread and written by the one that stops
 * it. An atomic, not a volatile sig_atomic_t: sig_atomic_t only orders a
 * signal handler against interrupted code, and this is two threads - which
 * `make tsan` says out loud, and rightly.
 */
static atomic_bool g_serve = ATOMIC_VAR_INIT(true);
static pthread_t g_server_thread;

static void *server_main(void *arg)
{
    (void)arg;
    while (atomic_load_explicit(&g_serve, memory_order_relaxed)) {
        struct pollfd pfd = { .fd = g_listen_fd, .events = POLLIN };
        if (poll(&pfd, 1, 50) <= 0)
            continue;                   /* timeout or EINTR */

        int fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0)
            continue;

        char req[4096];
        size_t total = 0;
        while (total < sizeof req - 1) {
            ssize_t n = read(fd, req + total, sizeof req - 1 - total);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (n == 0)
                break;
            total += (size_t)n;
            req[total] = '\0';
            if (strstr(req, "\r\n\r\n"))
                break;
        }

        if (total > 0 && strstr(req, "GET /hello ")) {
            const char *body = "hello over loopback\n";
            dprintf(fd, "HTTP/1.1 200 OK\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n%s",
                    strlen(body), body);
        } else {
            dprintf(fd, "HTTP/1.1 404 Not Found\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n");
        }
        close(fd);
    }
    return NULL;
}

static void server_start(void)
{
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        exit(1);
    }
    int one = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                  /* ephemeral: no collisions */
    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(g_listen_fd, 8) != 0) {
        perror("bind/listen");
        exit(1);
    }
    socklen_t alen = sizeof addr;
    if (getsockname(g_listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
        perror("getsockname");
        exit(1);
    }
    g_port = (int)ntohs(addr.sin_port);

    if (pthread_create(&g_server_thread, NULL, server_main, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }
}

static void server_stop(void)
{
    atomic_store_explicit(&g_serve, false, memory_order_relaxed);
    pthread_join(g_server_thread, NULL);
    close(g_listen_fd);
    g_listen_fd = -1;
}

int main(void)
{
    http_global_init();
    server_start();

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", g_port);

    /* --- success: the buffer is filled, and reports the status --- */

    char url[128];
    snprintf(url, sizeof url, "%s/hello", base);
    const char *hdrs[] = { "X-Test: yes", NULL };

    http_buffer out = { 0 };
    long status = 0;
    CHECK(http_get_ex(url, hdrs, &out, &status) == 0);
    CHECK(status == 200);
    CHECK(out.len == 20);
    CHECK(memcmp(out.data, "hello over loopback\n", 20) == 0);
    http_buffer_free(&out);

    /* --- the contract: *out is reset on entry, not appended to --- */

    out.data = (char *)"stale!";
    out.len = 5;
    CHECK(http_get(url, NULL, &out) == 0);
    CHECK(out.len == 20);               /* the stale bytes are gone */
    http_buffer_free(&out);

    /* --- non-200: the buffer is emptied and the status reported --- */

    snprintf(url, sizeof url, "%s/missing", base);
    out.data = (char *)"stale!";
    out.len = 5;
    status = -1;
    CHECK(http_get_ex(url, NULL, &out, &status) == -1);
    CHECK(status == 404);
    CHECK(out.data == NULL && out.len == 0);

    /* --- no HTTP response at all: status 0, buffer empty --- */

    out.data = (char *)"stale!";
    out.len = 5;
    CHECK(http_get_ex("http://", NULL, &out, &status) == -1);
    CHECK(status == 0);
    CHECK(out.data == NULL && out.len == 0);

    server_stop();
    http_global_cleanup();
    return harness_exit("test_http");
}
