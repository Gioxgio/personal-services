#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "allowlist.h"
#include "server.h"
#include "worker_pool.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG_NAME "download-service"

/* Clears g_keep_running (server.c watches it) to unwind the accept loop. */
static void shutdown_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

/*
 * Strict decimal parser: the first character must be a digit so that values
 * such as " 8080" or "-1" are rejected rather than silently accepted by
 * strtoul(). Trailing garbage ("8080abc") is rejected too.
 */
/* Strict decimal parser: the first character must be a digit, no trailing
 * garbage. Returns false for values outside [1, max]. */
static bool parse_ulong_range(const char* str, unsigned long min, unsigned long max,
                              unsigned long* out_value) {
    if (str[0] < '0' || str[0] > '9')
        return false;

    char* end = NULL;
    errno = 0;
    unsigned long value = strtoul(str, &end, 10);

    if (errno != 0)
        return false;
    if (end == str || *end != '\0')
        return false;
    if (value < min || value > max)
        return false;

    *out_value = value;
    return true;
}

static bool parse_port(const char* str, unsigned short* out_port) {
    if (str[0] < '0' || str[0] > '9') {
        return false;
    }

    char* end = NULL;
    errno = 0;
    unsigned long value = strtoul(str, &end, 10);

    if (errno != 0) { /* overflow */
        return false;
    }
    if (end == str) { /* no digits consumed */
        return false;
    }
    if (*end != '\0') { /* trailing garbage */
        return false;
    }
    if (value < 1UL || value > 65535UL) {
        return false;
    }

    *out_port = (unsigned short)value;
    return true;
}

/* Return the value of a required env var, or NULL if unset or empty. */
static const char* require_env(const char* name) {
    const char* value = getenv(name);
    if (!value || value[0] == '\0') {
        return NULL;
    }
    return value;
}

/* Append a name to a comma-separated "missing variables" list. */
static void note_missing(char* list, size_t capacity, const char* name) {
    size_t used = strlen(list);
    if (used + strlen(name) + 2 >= capacity) {
        return; /* list is full; the caller still reports what fit */
    }
    snprintf(list + used, capacity - used, "%s%s", used ? ", " : "", name);
}

static void usage(const char* reason) {
    if (reason) {
        fprintf(stderr, "error: %s\n\n", reason);
    }

    fprintf(stderr,
            "Usage: %s\n"
            "\n"
            "Required environment variables (no defaults):\n"
            "  PORT                     TCP port to listen on (1-65535)\n"
            "  DOWNLOAD_STORAGE_PATH    Directory downloads are streamed to disk under\n"
            "  DOWNLOAD_CONCURRENCY     Max parallel downloads (1-1024)\n"
            "  YTDLP_PATH               Path to the yt-dlp executable\n"
            "  DOWNLOAD_ALLOWED_DOMAINS Comma-separated allowed domains (empty = allow none)\n"
            "  CALLBACK_URL             Download-result callback URL (empty = off)\n"
            "  NOTIFY_URL               Notification base URL (empty = off)\n"
            "\n"
            "Routes:\n"
            "  POST %s   {\"url\": \"...\"}\n"
            "             -> {\"job_id\": \"uuid\"} or 400 {\"error\": "
            "\"unsupported_domain\"}\n",
            PROG_NAME, DOWNLOAD_PATH);
}

int main(void) {
    /* A peer closing mid-write must yield EPIPE, not a fatal signal. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = {.sa_handler = shutdown_handler};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART — accept() must return EINTR */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    const char* port_str = require_env("PORT");
    const char* storage_path = require_env("DOWNLOAD_STORAGE_PATH");
    const char* yt_dlp_path = require_env("YTDLP_PATH");
    const char* concurrency_str = require_env("DOWNLOAD_CONCURRENCY");

    /* Feature toggles: the variable must be set, but an empty value is an
     * explicit way to turn the feature off (no implicit defaults). */
    const char* callback_url = getenv("CALLBACK_URL");
    const char* notify_url = getenv("NOTIFY_URL");
    const char* allowed_domains_spec = getenv("DOWNLOAD_ALLOWED_DOMAINS");

    char missing[256] = "";
    if (!port_str)
        note_missing(missing, sizeof missing, "PORT");
    if (!storage_path)
        note_missing(missing, sizeof missing, "DOWNLOAD_STORAGE_PATH");
    if (!yt_dlp_path)
        note_missing(missing, sizeof missing, "YTDLP_PATH");
    if (!concurrency_str)
        note_missing(missing, sizeof missing, "DOWNLOAD_CONCURRENCY");
    if (!callback_url)
        note_missing(missing, sizeof missing, "CALLBACK_URL");
    if (!notify_url)
        note_missing(missing, sizeof missing, "NOTIFY_URL");
    if (!allowed_domains_spec)
        note_missing(missing, sizeof missing, "DOWNLOAD_ALLOWED_DOMAINS");

    if (missing[0]) {
        char reason[320];
        snprintf(reason, sizeof reason, "missing required environment variable(s): %s", missing);
        usage(reason);
        return 1;
    }

    unsigned long concurrency = 0;
    if (!parse_ulong_range(concurrency_str, 1, 1024, &concurrency)) {
        char reason[192];
        snprintf(reason, sizeof reason,
                 "invalid DOWNLOAD_CONCURRENCY \"%s\": expected an integer "
                 "in 1-1024",
                 concurrency_str);
        usage(reason);
        return 1;
    }

    server_config_t config;
    memset(&config, 0, sizeof(config));

    if (!parse_port(port_str, &config.port)) {
        char reason[192];
        snprintf(reason, sizeof reason, "invalid PORT \"%s\": expected an integer in 1-65535",
                 port_str);
        usage(reason);
        return 1;
    }

    config.yt_dlp_path = yt_dlp_path;

    /* download.allowed_domains: comma-separated list; empty allows nothing. */
    if (allowed_domains_spec[0] != '\0' &&
        !allowlist_parse(allowed_domains_spec, &config.allowed_domains)) {
        fprintf(stderr, "download-service: failed to parse DOWNLOAD_ALLOWED_DOMAINS\n");
        return 1;
    }

    /* The worker pool downloads straight to disk under storage_path; the
     * accept loop never touches downloaded files. */
    worker_pool_config_t pool_config;
    memset(&pool_config, 0, sizeof(pool_config));
    pool_config.concurrency = (unsigned int)concurrency;
    pool_config.yt_dlp_path = yt_dlp_path;
    pool_config.storage_path = storage_path;
    pool_config.callback_url = callback_url;
    pool_config.notify_url = notify_url;

    if (worker_pool_start(&pool_config) != 0) {
        fprintf(stderr, "download-service: failed to start the download worker pool\n");
        return 1;
    }

    /* Blocks until g_keep_running is cleared by shutdown_handler(). */
    int rc = request_handler_start(&config);

    worker_pool_stop();
    allowlist_free(&config.allowed_domains);

    if (rc != 0) {
        fprintf(stderr, "%s: server failed to start or stopped with an error\n", PROG_NAME);
    }
    return (rc == 0) ? 0 : 1;
}
