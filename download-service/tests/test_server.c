#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * test_server.c
 *
 * Includes server.c directly (like notification-service's tests) so the static
 * framing helpers and request_handler_dispatch are available. downloader.o and
 * json.o are linked in for the real download_media / json_parse_download.
 */
#include "server.c"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 18941

static int passed = 0;
static int failed = 0;
static int errors = 0;

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void check(bool condition, const char* name) {
    if (condition) {
        printf("PASS: %s\n", name);
        passed++;
    } else {
        printf("FAIL: %s\n", name);
        failed++;
        errors++;
    }
}

static void build_headers(char* buf, size_t cap, const char* method, const char* path,
                          size_t body_len) {
    snprintf(buf, cap,
             "%s %s HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n",
             method, path, body_len);
}

static server_config_t make_config(void) {
    server_config_t config;
    memset(&config, 0, sizeof(config));
    config.port = SERVER_PORT;
    config.yt_dlp_path = "yt-dlp";
    config.download_dir = "/tmp/download-service-test";
    /* Tests exercise real allowlist matching. */
    if (!allowlist_parse("youtube.com,www.youtube.com,m.youtube.com,youtu.be,www.youtu.be",
                         &config.allowed_domains)) {
        /* The spec above is known-valid; treat failure as fatal. */
        exit(1);
    }
    return config;
}

static bool file_contains(const char* path, const char* marker) {
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    char buf[256];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    buf[n] = '\0';
    return strstr(buf, marker) != NULL;
}

/* Poll `dir` for a regular file whose content contains `marker`. */
/* Recursively look for a regular file whose content contains `marker`. */
static bool scan_dir_for_marker(const char* dir, const char* marker) {
    DIR* d = opendir(dir);
    if (!d)
        return false;

    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char full[PATH_MAX];
        int n = snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof full)
            continue;
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (scan_dir_for_marker(full, marker)) {
                closedir(d);
                return true;
            }
            continue;
        }
        if (file_contains(full, marker)) {
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

static bool wait_for_downloaded_file(const char* dir, const char* marker, int timeout_s) {
    double deadline = now_seconds() + timeout_s;
    while (now_seconds() < deadline) {
        if (scan_dir_for_marker(dir, marker))
            return true;
        sleep_ms(50);
    }
    return false;
}

static worker_pool_config_t make_pool_config(void) {
    worker_pool_config_t config;
    config.concurrency = 2;
    config.yt_dlp_path = "yt-dlp";
    config.storage_path = "/tmp/download-service-test-pool";
    return config;
}

/* Extract the value of the "job_id" field from a response body. */
static bool extract_job_id(const char* body, char* out, size_t cap) {
    const char* key = strstr(body, "\"job_id\": \"");
    if (!key)
        return false;
    key += strlen("\"job_id\": \"");
    const char* end = strchr(key, '"');
    if (!end || end == key)
        return false;
    size_t len = (size_t)(end - key);
    if (len >= cap)
        return false;
    memcpy(out, key, len);
    out[len] = '\0';
    return true;
}

/* ---------- unit tests for request_handler_dispatch ---------- */

static void test_dispatch(void) {
    server_config_t config = make_config();
    http_response_t response;

    /* --- health --- */
    char headers[256];
    build_headers(headers, sizeof headers, "GET", "/health", 0);
    request_handler_dispatch(&config, -1, headers, "", 0, &response);
    check(response.status_code == 200, "dispatch: GET /health is 200");

    build_headers(headers, sizeof headers, "POST", "/health", 0);
    request_handler_dispatch(&config, -1, headers, "", 0, &response);
    check(response.status_code == 405, "dispatch: POST /health is 405");

    /* --- download routing --- */
    build_headers(headers, sizeof headers, "GET", "/download", 0);
    request_handler_dispatch(&config, -1, headers, "", 0, &response);
    check(response.status_code == 405, "dispatch: GET /download is 405");

    build_headers(headers, sizeof headers, "GET", "/nope", 0);
    request_handler_dispatch(&config, -1, headers, "", 0, &response);
    check(response.status_code == 404, "dispatch: unknown path is 404");

    /* --- framing errors --- */
    request_handler_dispatch(&config, -1, "POST /download HTTP/1.1\r\n\r\n", "", 0, &response);
    check(response.status_code == 411, "dispatch: missing Content-Length is 411");

    request_handler_dispatch(&config, -1,
                             "POST /download HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", "", 0,
                             &response);
    check(response.status_code == 413, "dispatch: oversized Content-Length is 413");

    request_handler_dispatch(&config, -1, "POST /download HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
                             "", 0, &response);
    check(response.status_code == 400, "dispatch: malformed Content-Length is 400");

    request_handler_dispatch(&config, -1, "garbage", "", 0, &response);
    check(response.status_code == 400, "dispatch: unparsable request line is 400");

    /* --- JSON validation --- */
    request_handler_dispatch(&config, -1, "POST /download HTTP/1.1\r\nContent-Length: 8\r\n\r\n",
                             "not json", 8, &response);
    check(response.status_code == 400, "dispatch: invalid JSON is 400");

    request_handler_dispatch(&config, -1, "POST /download HTTP/1.1\r\nContent-Length: 17\r\n\r\n",
                             "{\"format\":\"video\"}", 17, &response);
    check(response.status_code == 400, "dispatch: missing url is 400");

    /* format is optional and defaults to video; an unknown host is still rejected. */
    request_handler_dispatch(&config, -1, "POST /download HTTP/1.1\r\nContent-Length: 15\r\n\r\n",
                             "{\"url\":\"http://x\"}", 15, &response);
    check(response.status_code == 400, "dispatch: unknown host is 400 with default format");

    /* --- job-based download acceptance --- */
    const char* supported = "{\"url\":\"https://youtu.be/x\",\"format\":\"video\"}";
    build_headers(headers, sizeof headers, "POST", "/download", strlen(supported));
    request_handler_dispatch(&config, -1, headers, supported, strlen(supported), &response);
    check(response.status_code == 200, "dispatch: supported url is accepted with 200");
    char job_id[64];
    check(extract_job_id(response.body, job_id, sizeof job_id),
          "dispatch: response contains a job_id");
    check(extract_job_id(response.body, job_id, sizeof job_id) && strlen(job_id) == 36,
          "dispatch: job_id is a uuid");

    /* Two accepted jobs get different ids. */
    char job_id_b[64];
    request_handler_dispatch(&config, -1, headers, supported, strlen(supported), &response);
    check(response.status_code == 200 && extract_job_id(response.body, job_id_b, sizeof job_id_b) &&
              strcmp(job_id, job_id_b) != 0,
          "dispatch: distinct jobs get distinct ids");

    /* Unsupported domain: 400, no job created. */
    const char* unsupported = "{\"url\":\"https://vimeo.com/12345678\",\"format\":\"video\"}";
    build_headers(headers, sizeof headers, "POST", "/download", strlen(unsupported));
    request_handler_dispatch(&config, -1, headers, unsupported, strlen(unsupported), &response);
    check(response.status_code == 400, "dispatch: unsupported domain is 400");
    check(strcmp(response.body, "{\"error\": \"unsupported_domain\"}") == 0,
          "dispatch: unsupported domain body");

    request_handler_dispatch(&config, -1, "POST /download HTTP/1.1\r\nContent-Length: 38\r\n\r\n",
                             "{\"url\":\"http://x\",\"format\":\"hls\"}", 38, &response);
    check(response.status_code == 400, "dispatch: invalid format is 400");

    /* Length mismatch between Content-Length and framed body. */
    request_handler_dispatch(&config, -1, "POST /download HTTP/1.1\r\nContent-Length: 5\r\n\r\n",
                             "{\"url\":\"u\",\"format\":\"video\"}", 42, &response);
    check(response.status_code == 400, "dispatch: length mismatch is 400");

    /* Response body shape. */
    check(strcmp(response.body, "{\"status\": 400, \"message\": \"Bad Request\"}") == 0,
          "dispatch: response body format");
    check(response.body_length == strlen(response.body), "dispatch: body_length tracks body");

    allowlist_free(&config.allowed_domains);
}

/* ---------- end-to-end test with a fake yt-dlp ---------- */

/* Mirrors main.c's shutdown handler: clear g_keep_running so accept()
 * unwinds in the server child. */
static void server_test_signal_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

/* Run the server under test in a child process. */
static pid_t spawn_server(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = server_test_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        server_config_t config = make_config();
        worker_pool_config_t pool_config = make_pool_config();
        /* The e2e server downloads into its own storage directory. */
        pool_config.storage_path = "/tmp/download-service-e2e-pool";
        (void)mkdir(pool_config.storage_path, 0755);
        if (worker_pool_start(&pool_config) != 0)
            _exit(1);

        int rc = request_handler_start(&config);

        worker_pool_stop();
        allowlist_free(&config.allowed_domains);
        _exit(rc == 0 ? 0 : 1);
    }
    sleep_ms(150);
    return pid;
}

static ssize_t http_exchange(unsigned short port, const char* request, size_t request_len,
                             char* resp_out, size_t resp_cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    size_t sent = 0;
    while (sent < request_len) {
        ssize_t n = send(fd, request + sent, request_len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    shutdown(fd, SHUT_WR); /* deliver EOF to the server side */

    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = recv(fd, resp_out + total, resp_cap - 1 - total, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    resp_out[total] = '\0';
    close(fd);
    return (ssize_t)total;
}

static int response_status(const char* resp) {
    if (strncmp(resp, "HTTP/1.1 ", 9) != 0)
        return -1;
    return atoi(resp + 9);
}

static void test_end_to_end(void) {
    /* Write a fake yt-dlp that creates a tiny media file in the output dir
     * (the dir is taken from the -o argument). */
    const char* fake_dir = "/tmp/download-service-fake-yt-dlp";
    (void)mkdir(fake_dir, 0755);

    char fake_path[PATH_MAX];
    snprintf(fake_path, sizeof fake_path, "%s/yt-dlp", fake_dir);

    FILE* fake = fopen(fake_path, "w");
    if (!fake) {
        printf("FAIL: e2e: cannot write fake yt-dlp\n");
        failed++;
        errors++;
        return;
    }
    fputs("#!/bin/sh\n"
          "while [ $# -gt 0 ]; do\n"
          "  case \"$1\" in -o) shift; out=\"$1\";; esac; shift\n"
          "done\n"
          "dir=\"${out%/*}\"\n"
          "mkdir -p \"$dir\"\n"
          "printf 'FAKE-VIDEO-CONTENT' > \"$dir/dummy.mp4\"\n"
          "exit 0\n",
          fake);
    fclose(fake);
    chmod(fake_path, 0755);

    /* Prepend the fake dir to PATH so the server child finds fake yt-dlp. */
    char new_path[PATH_MAX];
    const char* old_path = getenv("PATH");
    snprintf(new_path, sizeof new_path, "%s:%s", fake_dir, old_path ? old_path : "/usr/bin:/bin");
    setenv("PATH", new_path, 1);
    setenv("DOWNLOAD_DIR", "/tmp/download-service-e2e", 1);

    pid_t server_pid = spawn_server();

    /* --- happy path: job accepted, downloaded asynchronously --- */
    const char* body = "{\"url\":\"https://youtu.be/fake\",\"format\":\"video\"}";
    char request[1024];
    int len = snprintf(request, sizeof request,
                       "POST /download HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %zu\r\n"
                       "\r\n"
                       "%s",
                       strlen(body), body);

    char response[8192];
    ssize_t n = http_exchange(SERVER_PORT, request, (size_t)len, response, sizeof response);
    check(n > 0, "e2e: got a response");

    int status = response_status(response);
    check(status == 200, "e2e: download accepted with 200");

    const char* resp_body = strstr(response, "\r\n\r\n");
    check(resp_body != NULL, "e2e: response has a body");
    char job_id[64];
    check(resp_body != NULL && extract_job_id(resp_body + 4, job_id, sizeof job_id),
          "e2e: response contains a job_id");

    /* The server child's pool worker downloads in the background; the file
     * lands in the e2e storage directory shared with the test process. */
    check(wait_for_downloaded_file("/tmp/download-service-e2e-pool", "FAKE-VIDEO-CONTENT", 5),
          "e2e: worker downloaded the file to disk");

    /* --- shutdown --- */
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    check(http_exchange(SERVER_PORT, request, 1, response, sizeof response) == -1,
          "e2e: socket released after shutdown");
}

/* ---------- main ---------- */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Write a fake yt-dlp on PATH so worker downloads are hermetic. */
    const char* fake_dir = "/tmp/download-service-fake-yt-dlp";
    (void)mkdir(fake_dir, 0755);

    char fake_path[PATH_MAX];
    snprintf(fake_path, sizeof fake_path, "%s/yt-dlp", fake_dir);

    FILE* fake = fopen(fake_path, "w");
    if (!fake) {
        printf("FAIL: cannot write fake yt-dlp\n");
        return 1;
    }
    fputs("#!/bin/sh\n"
          "while [ $# -gt 0 ]; do\n"
          "  case \"$1\" in -o) shift; out=\"$1\";; esac; shift\n"
          "done\n"
          "dir=\"${out%/*}\"\n"
          "mkdir -p \"$dir\"\n"
          "printf 'FAKE-VIDEO-CONTENT' > \"$dir/dummy.mp4\"\n"
          "exit 0\n",
          fake);
    fclose(fake);
    chmod(fake_path, 0755);

    char new_path[PATH_MAX];
    const char* old_path = getenv("PATH");
    snprintf(new_path, sizeof new_path, "%s:%s", fake_dir, old_path ? old_path : "/usr/bin:/bin");
    setenv("PATH", new_path, 1);

    /* Fork the e2e server child before starting this process's pool: fork()
     * must not happen while worker threads exist (inherited mutex/cond state
     * would deadlock), and the child must inherit a pristine pool state. */
    printf("\n=== end-to-end tests ===\n");
    test_end_to_end();

    worker_pool_config_t pool_config = make_pool_config();
    if (worker_pool_start(&pool_config) != 0) {
        printf("FAIL: cannot start worker pool\n");
        return 1;
    }

    printf("=== request_handler_dispatch tests ===\n");
    test_dispatch();

    worker_pool_stop();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
