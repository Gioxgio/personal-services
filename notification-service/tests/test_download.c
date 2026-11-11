#define _POSIX_C_SOURCE 200809L

#include "download.h"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

static void check(bool condition, const char* name) {
    if (condition) {
        printf("PASS: %s\n", name);
        passed++;
    } else {
        printf("FAIL: %s\n", name);
        failed++;
    }
}

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* One-shot mock download-service. Reads the request headers, optionally
 * captures the request body into DOWNLOAD_TEST_BODY_FILE, then replies with
 * the given status/body and exits. */
static void mock_download_run(int port, int status_code, const char* response_body) {
    alarm(10);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        _exit(1);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(server_fd, 1) < 0) {
        close(server_fd);
        _exit(1);
    }

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        close(server_fd);
        _exit(1);
    }

    /* Read headers and body. */
    char buf[8192];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(client_fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n"))
            break;
    }

    const char* body_file = getenv("DOWNLOAD_TEST_BODY_FILE");
    if (body_file && body_file[0]) {
        const char* body_start = strstr(buf, "\r\n\r\n");
        FILE* f = fopen(body_file, "w");
        if (f) {
            if (body_start) {
                fwrite(body_start + 4, 1, strlen(body_start + 4), f);
            }
            fclose(f);
        }
    }

    char response[2048];
    int len = snprintf(response, sizeof(response),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       status_code, status_code == 200 ? "OK" : "Error", strlen(response_body),
                       response_body);
    if (len > 0)
        send(client_fd, response, (size_t)len, MSG_NOSIGNAL);

    close(client_fd);
    close(server_fd);
    _exit(0);
}

static pid_t spawn_mock_download(int port, int status_code, const char* response_body) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        mock_download_run(port, status_code, response_body);
    }
    sleep_ms(100);
    return pid;
}

int main(void) {
    printf("=== download_submit tests ===\n");

    /* 200 with a job id -> accepted, job_id filled */
    {
        setenv("DOWNLOAD_SERVICE_URL", "http://127.0.0.1:18950/download", 1);
        pid_t pid = spawn_mock_download(18950, 200, "{\"job_id\": \"job-abc\"}");

        char job_id[37] = "";
        int rc = download_submit(42, "https://youtu.be/xyz", job_id, sizeof(job_id));
        check(rc == 0, "submit: 200 returns accepted");
        check(strcmp(job_id, "job-abc") == 0, "submit: job_id captured");
        waitpid(pid, NULL, 0);
    }

    /* 400 unsupported_domain -> rejected */
    {
        setenv("DOWNLOAD_SERVICE_URL", "http://127.0.0.1:18950/download", 1);
        pid_t pid = spawn_mock_download(18950, 400, "{\"error\": \"unsupported_domain\"}");

        char job_id[37] = "";
        int rc = download_submit(42, "https://vimeo.com/123", job_id, sizeof(job_id));
        check(rc == 1, "submit: 400 unsupported_domain returns rejected");
        check(job_id[0] == '\0', "submit: job_id untouched on rejection");
        waitpid(pid, NULL, 0);
    }

    /* 200 without job_id -> protocol error */
    {
        setenv("DOWNLOAD_SERVICE_URL", "http://127.0.0.1:18950/download", 1);
        pid_t pid = spawn_mock_download(18950, 200, "{\"ok\": true}");

        char job_id[37] = "sentinel";
        int rc = download_submit(42, "https://youtu.be/xyz", job_id, sizeof(job_id));
        check(rc == -1, "submit: 200 without job_id is an error");
        check(strcmp(job_id, "sentinel") == 0, "submit: job_id untouched on error");
        waitpid(pid, NULL, 0);
    }

    /* 500 -> error */
    {
        setenv("DOWNLOAD_SERVICE_URL", "http://127.0.0.1:18950/download", 1);
        pid_t pid = spawn_mock_download(18950, 500, "{\"error\": \"boom\"}");

        char job_id[37] = "";
        int rc = download_submit(42, "https://youtu.be/xyz", job_id, sizeof(job_id));
        check(rc == -1, "submit: 500 returns error");
        waitpid(pid, NULL, 0);
    }

    /* Nothing listening -> transport error */
    {
        setenv("DOWNLOAD_SERVICE_URL", "http://127.0.0.1:18951/download", 1);
        char job_id[37] = "";
        int rc = download_submit(42, "https://youtu.be/xyz", job_id, sizeof(job_id));
        check(rc == -1, "submit: unreachable service returns error");
    }

    /* Request body carries url and chat_id */
    {
        setenv("DOWNLOAD_TEST_BODY_FILE", "/tmp/opencode/download_body.txt", 1);
        setenv("DOWNLOAD_SERVICE_URL", "http://127.0.0.1:18950/download", 1);
        pid_t pid = spawn_mock_download(18950, 200, "{\"job_id\": \"job-body\"}");

        char job_id[37] = "";
        int rc = download_submit(777, "https://youtube.com/watch?v=abc", job_id, sizeof(job_id));
        check(rc == 0, "submit: body test accepted");
        waitpid(pid, NULL, 0);

        FILE* f = fopen("/tmp/opencode/download_body.txt", "r");
        check(f != NULL, "submit: body file captured");
        if (f) {
            char body[1024];
            size_t n = fread(body, 1, sizeof(body) - 1, f);
            body[n] = '\0';
            fclose(f);
            check(strstr(body, "chat_id") != NULL && strstr(body, "777") != NULL,
                  "submit: body contains chat_id");
            check(strstr(body, "youtube.com/watch?v=abc") != NULL, "submit: body contains url");
        }
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
