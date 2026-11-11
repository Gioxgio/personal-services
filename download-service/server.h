#ifndef SERVER_H
#define SERVER_H

#include "allowlist.h"
#include "json.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum size of the HTTP request header block (request line + headers). */
#define MAX_HEADER_SIZE 8192

/* Maximum size of an HTTP request body we are willing to buffer. */
#define MAX_HTTP_BODY 16384

/* SO_RCVTIMEO / SO_SNDTIMEO applied to accepted sockets, in seconds. */
#define SOCKET_TIMEOUT_SEC 5

/* listen() backlog depth. */
#define LISTEN_BACKLOG 16

/* The routes this service exposes. */
#define DOWNLOAD_PATH "/download"
#define HEALTH_PATH "/health"

typedef struct {
    unsigned short port;
    const char* yt_dlp_path;     /* path to the yt-dlp executable */
    const char* download_dir;    /* directory used for temporary downloads */
    allowlist_t allowed_domains; /* download.allowed_domains from config */
} server_config_t;

typedef struct {
    int status_code;
    char body[256];
    size_t body_length;
    /*
     * Set to true by request_handler_dispatch() when it has already written a
     * response directly to the socket (e.g. streaming a downloaded file); the
     * accept loop then skips send_http_response().
     */
    bool handled_directly;
} http_response_t;

/*
 * Cleared by the SIGINT/SIGTERM handler in main.c so that accept() returning
 * EINTR unwinds the loop in request_handler_start() and control flows back to
 * main().
 */
extern volatile sig_atomic_t g_keep_running;

/*
 * Socket setup and accept loop. Blocks until g_keep_running is cleared.
 * Returns 0 on graceful shutdown, -1 on setup failure.
 */
int request_handler_start(const server_config_t* config);

/*
 * Route a single request and produce the response in `response`.
 *
 * `client_fd` is the accepted socket; dispatch may stream a downloaded file
 * directly to it (setting response->handled_directly).
 *
 * `headers` must be a NUL-terminated header block (the bytes preceding the
 * "\r\n\r\n" terminator). `body` holds the already-framed body bytes.
 */
void request_handler_dispatch(const server_config_t* config, int client_fd, const char* headers,
                              const char* body, size_t body_length, http_response_t* response);

#endif
