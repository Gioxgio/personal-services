/*
 * test_notify - recording notifications: the JSON escaper on
 * its own, and the whole chain over a real HTTP exchange.
 *
 * The escaper is unit-tested first: quotes, backslashes, the named
 * control characters, other control bytes as \u00xx, the empty
 * string, and UTF-8 passthrough.
 *
 * The integration half drives the real recorder against a fake
 * `yt-dlp` - a shell script that holds the "recording" open until
 * the test releases it, the pattern tests/test_recorder.c uses -
 * while a loopback server (the shape of tests/test_http.c's)
 * captures what stream-sniper POSTs. Nothing leaves the machine.
 *
 * Phases:
 *
 *   1. enabled: a recording that ends on its own (release -> exit 0)
 *      produces exactly two POSTs to /notifications - start, then
 *      stop with "(completed)".
 *   2. a recording stopped by recorder_shutdown() - SIGTERM to a
 *      held child - also notifies, with "(failed)": the STOP path
 *      flows through the same waitpid and the same outcome check.
 *   3. SS_NOTIFY_URL unset: a whole recording, and no request.
 *   4. SS_NOTIFY_URL empty: same.
 *   5. SS_NOTIFY_URL not an http(s) URL: warned, disabled, same.
 *
 * Phases 3-5 are the "silent no-op" contract: a misconfigured
 * endpoint must never affect recording - which here means the
 * recording still happens, with nothing sent.
 *
 * Run from the project root (make test does).
 */

#include "harness.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "log.h"
#include "notify.h"
#include "recorder.h"
#include "urlparse.h"
#include "util.h"

/* What a probe would have handed over; the fake yt-dlp ignores it. */
#define STREAM_URL "https://www.youtube.com/watch?v=LIVEnotify001"

/* "twitch: notifytest" -> slug "notifytest": the messages the
 * assertions compare against are built from it. */
#define CHAN_LINE "twitch: notifytest"

/* PATH_MAX plus the suffixes appended below; heap strings rather
 * than fixed arrays, so -Wformat-truncation stays quiet without
 * every one of them being oversized (tests/test_recorder.c's trick). */
#define TEST_PATH (PATH_MAX + 128)

/* One channel, parsed once; the slug decides the messages below. */
static channel_t g_ch;

/* Everything under one temp root:
 *
 *   <root>/bin/yt-dlp   the fake, first on PATH
 *   <root>/out/         the recordings root handed to recorder_init()
 *   <root>/started      touched the moment the fake runs
 *   <root>/release      touched by the test to let the fake exit
 */
static char *g_root;

static char *in_root(const char *name)
{
    char *p = xmalloc(TEST_PATH);
    snprintf(p, TEST_PATH, "%s/%s", g_root, name);
    return p;
}

static bool write_file(const char *path, const char *body, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    bool ok = fputs(body, f) >= 0;
    if (fclose(f) != 0)
        ok = false;
    return ok && chmod(path, mode) == 0;
}

static bool wait_for(const char *path)
{
    for (int i = 0; i < 400; i++) {      /* ~4 s, 10 ms apart */
        if (access(path, F_OK) == 0)
            return true;
        struct timespec nap = { 0, 10000000L };
        nanosleep(&nap, NULL);
    }
    return false;
}

static void rm_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *p = xmalloc(TEST_PATH);
        snprintf(p, TEST_PATH, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
            rm_tree(p);
        else
            unlink(p);
        free(p);
    }
    closedir(d);
    rmdir(dir);
}

/* --- the loopback capture server --- */

#define REQ_MAX      16
#define REQ_LINE_MAX 256
#define REQ_HDR_MAX  2048
#define REQ_BODY_MAX 512

typedef struct {
    char line[REQ_LINE_MAX];             /* request line            */
    char hdrs[REQ_HDR_MAX];              /* header block            */
    char body[REQ_BODY_MAX];             /* exactly Content-Length  */
} req_t;

static req_t     g_reqs[REQ_MAX];
static int       g_nreqs;
static pthread_mutex_t g_reqs_mtx = PTHREAD_MUTEX_INITIALIZER;

static int g_listen_fd = -1;
static int g_port;

/*
 * Stop flag, read by the server thread and written by the one that
 * stops it - an atomic, for the reason tests/test_http.c gives.
 */
static atomic_bool g_serve = ATOMIC_VAR_INIT(true);
static pthread_t g_server_thread;

/*
 * Content-Length of a request's header block, or -1 when the header
 * is not there. Case-insensitive, and bounded by headers_end: the
 * body could itself contain anything, including this string.
 */
static long parse_content_length(const char *hdr, const char *headers_end)
{
    for (const char *p = hdr; p < headers_end && *p; p++) {
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            p += 15;
            while (*p == ' ' || *p == '\t')
                p++;
            return strtol(p, NULL, 10);
        }
        const char *nl = strstr(p, "\r\n");
        if (!nl || nl >= headers_end)
            break;
        p = nl + 2;
    }
    return -1;
}

/* Store one request under the mutex; past REQ_MAX it is dropped. */
static void capture_request(const char *req)
{
    req_t r = { 0 };

    size_t line_len = strcspn(req, "\r\n");
    if (line_len < sizeof r.line) {
        memcpy(r.line, req, line_len);
        r.line[line_len] = '\0';
    } else {
        memcpy(r.line, req, sizeof r.line - 1);
        r.line[sizeof r.line - 1] = '\0';
    }

    char *hdrs_end = strstr(req, "\r\n\r\n");
    if (hdrs_end) {
        size_t hl = (size_t)(hdrs_end - req) + 4;
        if (hl > sizeof r.hdrs - 1)
            hl = sizeof r.hdrs - 1;
        memcpy(r.hdrs, req, hl);
        r.hdrs[hl] = '\0';

        /* The body is exactly what Content-Length promised. */
        long clen = parse_content_length(req, hdrs_end);
        const char *b = hdrs_end + 4;
        size_t blen = clen > 0 ? (size_t)clen : strlen(b);
        if (blen > sizeof r.body - 1)
            blen = sizeof r.body - 1;
        memcpy(r.body, b, blen);
        r.body[blen] = '\0';
    }

    pthread_mutex_lock(&g_reqs_mtx);
    if (g_nreqs < REQ_MAX)
        g_reqs[g_nreqs++] = r;
    pthread_mutex_unlock(&g_reqs_mtx);
}

/*
 * Read one request whole - header block plus the body its
 * Content-Length promised, which is not necessarily in the same
 * read as the headers - capture it, and answer 200 so http_post()
 * sees a success the way a live endpoint would give it.
 */
static void handle_conn(int fd)
{
    char req[8192];
    size_t total = 0;

    for (;;) {
        if (total >= sizeof req - 1)
            break;
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

        char *hdrs_end = strstr(req, "\r\n\r\n");
        if (!hdrs_end)
            continue;
        size_t hdr_len = (size_t)(hdrs_end - req) + 4;
        long clen = parse_content_length(req, hdrs_end);
        if (clen <= 0)
            break;                      /* no body expected */
        if (total >= hdr_len + (size_t)clen)
            break;                      /* body complete */
    }

    if (total > 0 && strstr(req, "\r\n\r\n"))
        capture_request(req);

    dprintf(fd, "HTTP/1.1 200 OK\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n"
                "\r\nok");
    close(fd);
}

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
        handle_conn(fd);
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

/* --- request log access, always under the server's mutex --- */

static int request_count(void)
{
    pthread_mutex_lock(&g_reqs_mtx);
    int n = g_nreqs;
    pthread_mutex_unlock(&g_reqs_mtx);
    return n;
}

static bool wait_for_requests(int n)
{
    for (int i = 0; i < 600; i++) {      /* ~6 s, 10 ms apart */
        if (request_count() >= n)
            return true;
        struct timespec nap = { 0, 10000000L };
        nanosleep(&nap, NULL);
    }
    return false;
}

static bool request_at(int i, req_t *out)
{
    pthread_mutex_lock(&g_reqs_mtx);
    bool ok = i >= 0 && i < g_nreqs;
    if (ok)
        *out = g_reqs[i];
    pthread_mutex_unlock(&g_reqs_mtx);
    return ok;
}

/*
 * A stop notification's body: the prefix (through "after "), then
 * the duration, then the outcome word. The duration is the
 * recorder's own clock - "0:00" here, but its shape, not its value,
 * is what can be asserted.
 */
static void check_body_shape(const char *body, const char *prefix,
                             const char *tail)
{
    size_t plen = strlen(prefix);
    size_t tlen = strlen(tail);
    size_t blen = strlen(body);

    if (blen <= plen + tlen) {
        CHECK(false);                   /* no room for a duration */
        return;
    }
    CHECK(strncmp(body, prefix, plen) == 0);
    CHECK(memcmp(body + blen - tlen, tail, tlen) == 0);
    CHECK(memchr(body + plen, ':', blen - plen - tlen) != NULL);
}

/* The fake yt-dlp, the same seam tests/test_recorder.c drives. */

static const char FAKE_YTDLP[] =
    "#!/bin/sh\n"
    "# Stand-in for yt-dlp: touch 'started', then hold the recording\n"
    "# open until the test releases it and exit 0 - a clean end, like\n"
    "# a broadcast that stopped. Never released, it dies with the\n"
    "# SIGTERM recorder_shutdown() sends its whole group.\n"
    ": > \"$SS_TEST_ROOT/started\"\n"
    "while [ ! -e \"$SS_TEST_ROOT/release\" ]; do sleep 0.02; done\n"
    "exit 0\n";

/*
 * Record g_ch once, released so the child ends cleanly. Each call
 * re-inits: recorder_shutdown() drops the slots, so a fresh
 * recorder_init() is what makes the next start legitimate.
 */
static void record_once(void)
{
    char *started = in_root("started");
    char *release = in_root("release");
    char *out     = in_root("out");

    unlink(started);
    unlink(release);

    CHECK(recorder_init(&g_ch, 1, out));
    CHECK(recorder_start(&g_ch, xstrdup(STREAM_URL)));
    CHECK(wait_for(started));            /* the child is running */
    CHECK(write_file(release, "", 0644));
    recorder_shutdown();                 /* joins the recording thread */

    free(started);
    free(release);
    free(out);
}

int main(void)
{
    /*
     * The recorder logs every fork and every DONE at INFO, which is
     * evidence on a real run and noise in a test log. WARN still
     * reaches the terminal, so a misconfigured SS_NOTIFY_URL stays
     * visible.
     */
    log_set_level(LOG_WARN);

    /* --- the JSON escaper, on its own --- */

    char *q = notify_json_quote("notifytest");
    CHECK_STR_EQ(q, "\"notifytest\"");
    free(q);

    q = notify_json_quote("say \"hi\"");
    CHECK_STR_EQ(q, "\"say \\\"hi\\\"\"");
    free(q);

    q = notify_json_quote("back\\slash");
    CHECK_STR_EQ(q, "\"back\\\\slash\"");
    free(q);

    q = notify_json_quote("bell\bform\f");
    CHECK_STR_EQ(q, "\"bell\\bform\\f\"");
    free(q);

    q = notify_json_quote("tab\tnl\ncr\r");
    CHECK_STR_EQ(q, "\"tab\\tnl\\ncr\\r\"");
    free(q);

    q = notify_json_quote("ctrl\x01\x1f");
    CHECK_STR_EQ(q, "\"ctrl\\u0001\\u001f\"");
    free(q);

    q = notify_json_quote("");
    CHECK_STR_EQ(q, "\"\"");
    free(q);

    /* UTF-8 passes through untouched: valid JSON either way. */
    q = notify_json_quote("café");
    CHECK_STR_EQ(q, "\"café\"");
    free(q);

    /* --- the integration half --- */

    char tmpl[] = "/tmp/ss-test-notify-XXXXXX";
    char *made = mkdtemp(tmpl);
    if (!made) {
        fprintf(stderr, "test_notify: cannot create a temp dir: %s\n",
                strerror(errno));
        return 1;
    }
    g_root = xstrdup(made);

    char *bin  = in_root("bin");
    char *out  = in_root("out");
    char *fake = in_root("bin/yt-dlp");
    CHECK(mkdir(bin, 0755) == 0);
    CHECK(mkdir(out, 0755) == 0);
    CHECK(write_file(fake, FAKE_YTDLP, 0755));

    /* execvp() searches PATH, so the fake has to come first. */
    const char *old_path = getenv("PATH");
    char *path = xmalloc(TEST_PATH);
    snprintf(path, TEST_PATH, "%s:%s", bin, old_path ? old_path : "/usr/bin:/bin");
    setenv("PATH", path, 1);
    setenv("SS_TEST_ROOT", g_root, 1);

    CHECK(channel_parse(CHAN_LINE, &g_ch));

    server_start();

    char base[64];
    snprintf(base, sizeof base, "http://127.0.0.1:%d", g_port);

    char *started = in_root("started");
    char *release = in_root("release");

    /* --- phase 1: enabled, the broadcast ends on its own --- */

    setenv("SS_NOTIFY_URL", base, 1);
    notify_init();

    unlink(started);
    unlink(release);
    CHECK(recorder_init(&g_ch, 1, out));
    CHECK(recorder_start(&g_ch, xstrdup(STREAM_URL)));
    CHECK(wait_for(started));             /* the child is running */
    CHECK(wait_for_requests(1));          /* the start notification */
    CHECK(write_file(release, "", 0644));
    CHECK(wait_for_requests(2));          /* the stop notification */
    recorder_shutdown();

    CHECK(request_count() == 2);          /* exactly two, no more */

    req_t r0, r1;
    CHECK(request_at(0, &r0));
    CHECK(request_at(1, &r1));

    /* Both POSTed to the fixed path, as JSON. */
    CHECK(strncmp(r0.line, "POST /notifications ", 20) == 0);
    CHECK(strncmp(r1.line, "POST /notifications ", 20) == 0);
    CHECK(strstr(r0.hdrs, "Content-Type: application/json") != NULL);
    CHECK(strstr(r1.hdrs, "Content-Type: application/json") != NULL);

    /* The start message, byte for byte: source + message. */
    CHECK_STR_EQ(r0.body,
                 "{\"source\": \"stream-sniper\", \"message\": \"started "
                 "recording notifytest\"}");

    char prefix[REQ_BODY_MAX];
    snprintf(prefix, sizeof prefix,
             "{\"source\": \"stream-sniper\", \"message\": \"stopped "
             "recording %s after ",
             g_ch.slug);
    check_body_shape(r1.body, prefix, " (completed)\"}");

    /* --- phase 2: the recording is stopped by shutdown --- */

    unlink(started);
    unlink(release);
    CHECK(recorder_init(&g_ch, 1, out));
    CHECK(recorder_start(&g_ch, xstrdup(STREAM_URL)));
    CHECK(wait_for(started));
    CHECK(wait_for_requests(3));          /* its start notification */
    /* No release: the child is still held, so shutdown SIGTERMs it. */
    recorder_shutdown();
    CHECK(wait_for_requests(4));          /* its stop notification */

    CHECK(request_count() == 4);

    req_t r3;
    CHECK(request_at(3, &r3));
    CHECK(strncmp(r3.line, "POST /notifications ", 20) == 0);
    snprintf(prefix, sizeof prefix,
             "{\"source\": \"stream-sniper\", \"message\": \"stopped "
             "recording %s after ",
             g_ch.slug);
    check_body_shape(r3.body, prefix, " (failed)\"}");

    /* --- phase 3: SS_NOTIFY_URL unset --- */

    unsetenv("SS_NOTIFY_URL");
    notify_init();
    record_once();
    CHECK(request_count() == 4);

    /* --- phase 4: SS_NOTIFY_URL empty --- */

    setenv("SS_NOTIFY_URL", "", 1);
    notify_init();
    record_once();
    CHECK(request_count() == 4);

    /* --- phase 5: SS_NOTIFY_URL not an http(s) URL --- */

    setenv("SS_NOTIFY_URL", "ftp://example.com", 1);
    notify_init();
    record_once();
    CHECK(request_count() == 4);

    /* --- tear down --- */

    recorder_shutdown();
    channel_free(&g_ch);
    server_stop();
    http_global_cleanup();
    free(bin);
    free(out);
    free(fake);
    free(started);
    free(release);
    free(path);
    rm_tree(g_root);
    free(g_root);

    return harness_exit("test_notify");
}
