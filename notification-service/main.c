#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "server.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"

/* Long-polling timeout seconds for Telegram getUpdates. */
#define POLL_TIMEOUT_SEC 30
/* Maximum pending update_id to remember. */
#define MAX_UPDATE_BODY 16384

#define PROG_NAME "notification-service"

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
            "Required environment variables:\n"
            "  PORT             TCP port to listen on (1-65535)\n"
            "  TELEGRAM_TOKEN   Bot token issued by @BotFather\n"
            "  TELEGRAM_CHAT_ID Target chat id, or @channelusername\n"
            "\n"
            "Route:\n"
            "  POST %s   {\"message\": \"...\"}\n",
            PROG_NAME, NOTIFICATION_PATH);
}

/*
 * Background thread: polls Telegram's getUpdates endpoint with long polling and
 * dispatches each received update to handle_update(). Runs until g_keep_running
 * is cleared. Requires TELEGRAM_TOKEN and TELEGRAM_CHAT_ID (already validated
 * in main). No public webhook or inbound port is needed for this mode.
 */
static void* poll_loop(void* arg) {
    const server_config_t* config = arg;
    const char* token = config->telegram_config.token;
    long offset = 0;

    fprintf(stderr, "notification-service: polling Telegram for updates\n");

    while (g_keep_running) {
        char body[MAX_UPDATE_BODY + 1];
        int status = telegram_get_updates(token, offset, POLL_TIMEOUT_SEC, body, sizeof(body));
        if (status < 0) {
            fprintf(stderr, "notification-service: getUpdates transport error\n");
            /* Back off before retrying on failure. */
            sleep(2);
            continue;
        }

        /* Parse the top-level "result" array: walk each update object. */
        const char* p = body;
        while (*p && g_keep_running) {
            /* Find the start of the next update object within "result". */
            if (!json_object_find_value(&p, "result"))
                break;
            if (*p != '[')
                break;
            p++; /* skip '[' */

            while (*p && g_keep_running) {
                while (*p && isspace((unsigned char)*p))
                    p++;
                if (*p == ']')
                    break; /* end of result array */
                if (*p == ',')
                    p++;

                /* Locate the update object. */
                const char* start = p;
                if (!json_object_find_value(&p, "update_id"))
                    break;
                /* Skip past the object to find its end (naive brace scan). */
                int depth = 0;
                const char* obj_end = start;
                for (const char* q = start; *q && g_keep_running; q++) {
                    if (*q == '{')
                        depth++;
                    else if (*q == '}') {
                        depth--;
                        if (depth == 0) {
                            obj_end = q + 1;
                            break;
                        }
                    }
                }
                if (depth != 0)
                    break; /* malformed */

                /* Parse update_id. */
                long long update_id = 0;
                {
                    const char* q = start;
                    if (json_find_top_level_int64(q, "update_id", &update_id)) {
                        offset = update_id + 1;
                    }
                }

                /* Extract the text from the update's "message". The target chat
                 * always comes from the config, not from the update. */
                size_t obj_len = (size_t)(obj_end - start);
                if (obj_len > 0 && obj_len <= MAX_UPDATE_BODY) {
                    char obj_copy[MAX_UPDATE_BODY + 1];
                    memcpy(obj_copy, start, obj_len);
                    obj_copy[obj_len] = '\0';

                    char text[MAX_TELEGRAM_MSG_LEN + 1];
                    if (json_extract_telegram_update(obj_copy, text, sizeof(text))) {
                        fprintf(stderr, "notification-service: message received text=%s\n", text);
                        handle_update(config, text);
                    }
                }

                p = obj_end;
            }
        }
    }
    return NULL;
}

int main(void) {
    /* Must precede every other libcurl entry point, including easy_init(). */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "error: curl_global_init failed\n");
        return 1;
    }

    /* A peer closing mid-write must yield EPIPE, not a fatal signal. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = {.sa_handler = shutdown_handler};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART — accept() must return EINTR */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        curl_global_cleanup();
        return 1;
    }

    const char* port_str = require_env("PORT");
    const char* token = require_env("TELEGRAM_TOKEN");
    const char* chat_id = require_env("TELEGRAM_CHAT_ID");

    if (!port_str || !token || !chat_id) {
        char missing[128] = "";
        if (!port_str) {
            note_missing(missing, sizeof(missing), "PORT");
        }
        if (!token) {
            note_missing(missing, sizeof(missing), "TELEGRAM_TOKEN");
        }
        if (!chat_id) {
            note_missing(missing, sizeof(missing), "TELEGRAM_CHAT_ID");
        }

        char reason[192];
        snprintf(reason, sizeof(reason), "missing required environment variable(s): %s", missing);
        usage(reason);
        curl_global_cleanup();
        return 1;
    }

    server_config_t config;
    memset(&config, 0, sizeof(config));

    if (!parse_port(port_str, &config.port)) {
        char reason[192];
        snprintf(reason, sizeof(reason), "invalid PORT \"%s\": expected an integer in 1-65535",
                 port_str);
        usage(reason);
        curl_global_cleanup();
        return 1;
    }

    config.telegram_config.token = token;
    config.telegram_config.chat_id = chat_id;

    /* Start the Telegram polling thread so /download commands are picked up
     * without needing a public webhook. Runs concurrently with the HTTP server
     * (which still serves /callback/download for download-service results). */
    pthread_t poll_tid;
    if (pthread_create(&poll_tid, NULL, poll_loop, &config) != 0) {
        perror("pthread_create");
        curl_global_cleanup();
        return 1;
    }

    /* Blocks until g_keep_running is cleared by shutdown_handler(). */
    int rc = request_handler_start(&config);

    /* Wait for the polling thread to exit. */
    pthread_join(poll_tid, NULL);

    curl_global_cleanup();

    if (rc != 0) {
        fprintf(stderr, "%s: server failed to start or stopped with an error\n", PROG_NAME);
    }
    return (rc == 0) ? 0 : 1;
}
