/*
 * main.c - stream-sniper entry point.
 *
 * Wiring only: parse args/env, init modules, run the poll loop, shut down.
 * All real behaviour lives in the modules (config / probe / recorder).
 */

#include "config.h"
#include "http.h"
#include "log.h"
#include "notify.h"
#include "probe.h"
#include "recorder.h"
#include "ss.h"
#include "twitch_auth.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static options_t g_opts;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void usage(const char* prog) {
    fprintf(stderr,
            "usage: %s [channels-file] [-i seconds] [-o outdir] [-n] [-v] [-h]\n"
            "\n"
            "  channels-file  default: channels.txt\n"
            "  -i seconds     poll interval, default: 300 (min 30)\n"
            "  -o dir         recordings root, default: ./recordings\n"
            "  -n             dry-run: probe and log, never record\n"
            "  -v             verbose (debug) logging\n"
            "  -h             this help\n"
            "\n"
            "env: TWITCH_CLIENT_ID, TWITCH_CLIENT_SECRET, TWITCH_TOKEN,\n"
            "     SS_WRAPPER_DIR (where ffmpeg-quiet/ffprobe live),\n"
            "     SS_NOTIFY_URL (POST recording events to <url>/notifications)\n",
            prog);
}

/* One sweep: probe every idle channel, start a recorder for the live ones. */
static void sweep(void) {
    for (size_t i = 0; i < g_nchannels; i++) {
        if (!g_running)
            break;

        const channel_t* ch = &g_channels[i];
        if (recorder_is_busy(ch))
            continue;

        char* url = NULL;

        /*
         * PROBE_OFFLINE and PROBE_ERROR are the same thing to the caller:
         * try again next sweep. Backing off the two differently is
         * Phase 3. Either way the probe leaves url NULL.
         *
         * A not-live probe also clears the "already tried this live event"
         * flag, so a channel whose stream went offline gets a fresh attempt
         * when it comes back live.
         */
        if (probe_is_live(ch, &url) != PROBE_LIVE) {
            free(url);
            recorder_reset_live(ch);
            continue;
        }

        /*
         * Don't hammer a channel whose live stream we already tried and
         * could not record - a subscriber-only YouTube broadcast, for
         * example - until the stream goes offline and comes back.
         */
        if (recorder_tried_live(ch)) {
            free(url);
            continue;
        }

        if (g_opts.dry_run) {
            logmsg(LOG_INFO, "LIVE %-*s would record", LOG_SLUG_COL, ch->slug);
            free(url);
            continue;
        }
        /* Ownership passes to the recorder: probe created it, recorder
         * frees it on every path, including the ones inside recorder_start. */
        recorder_start(ch, url);
    }
}

/* Parse argv into g_opts. Returns 0 on success, -1 after printing a usage error. */
static int parse_args(int argc, char** argv) {
    snprintf(g_opts.channels_file, sizeof g_opts.channels_file, "channels.txt");
    snprintf(g_opts.out_dir, sizeof g_opts.out_dir, "recordings");
    g_opts.interval_sec = 300;
    g_opts.dry_run = false;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(argv[0]);
            return 1; /* asked for it, so not a failure */
        } else if (!strcmp(arg, "-n")) {
            g_opts.dry_run = true;
        } else if (!strcmp(arg, "-v")) {
            log_set_level(LOG_DEBUG);
        } else if (!strcmp(arg, "-i") || !strcmp(arg, "-o")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an argument\n", argv[0], arg);
                usage(argv[0]);
                return -1;
            }
            const char* val = argv[++i];

            if (!strcmp(arg, "-i")) {
                char* end;
                errno = 0;
                long secs = strtol(val, &end, 10);
                if (errno || end == val || *end != '\0' || secs <= 0) {
                    fprintf(stderr, "%s: -i needs a positive number of seconds, got '%s'\n",
                            argv[0], val);
                    usage(argv[0]);
                    return -1;
                }
                g_opts.interval_sec = secs;
            } else {
                snprintf(g_opts.out_dir, sizeof g_opts.out_dir, "%s", val);
            }
        } else if (arg[0] == '-') {
            /*
             * Anything else starting with '-' is a typo, not a filename.
             * Guessing otherwise turns `--foo` into `cannot open --foo`,
             * which says nothing about what actually went wrong.
             */
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
            usage(argv[0]);
            return -1;
        } else {
            snprintf(g_opts.channels_file, sizeof g_opts.channels_file, "%s", arg);
        }
    }
    if (g_opts.interval_sec < 30)
        g_opts.interval_sec = 30;
    return 0;
}

int main(int argc, char** argv) {
    int rc = parse_args(argc, argv);
    if (rc < 0)
        return EXIT_FAILURE;
    if (rc > 0)
        return 0;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    http_global_init();
    notify_init();
    twitch_auth_init_from_env();

    if (config_load(g_opts.channels_file) != 0)
        return EXIT_FAILURE;
    if (g_nchannels == 0) {
        logmsg(LOG_ERROR, "no channels in %s", g_opts.channels_file);
        return EXIT_FAILURE;
    }
    if (!recorder_init(g_channels, g_nchannels, g_opts.out_dir))
        return EXIT_FAILURE;

    if (!g_opts.dry_run) {
        mkdir_p(g_opts.out_dir);
        if (system("command -v yt-dlp >/dev/null 2>&1") != 0)
            logmsg(LOG_WARN, "yt-dlp not found in PATH - recordings will fail "
                             "(pipx install yt-dlp)");
    }

    logmsg(LOG_INFO, "watching %zu channel(s) every %lds -> %s%s", g_nchannels, g_opts.interval_sec,
           g_opts.out_dir, g_opts.dry_run ? " (dry-run)" : "");
    logmsg(LOG_INFO, "yt-dlp output -> %s/<channel>.log (terminal left quiet)", g_opts.out_dir);

    /*
     * The interval is a deadline, not a count of sleep(1) calls: a sleep cut
     * short by a signal used to count as a full second, so any signal during
     * the wait silently shortened the poll interval.
     */
    while (g_running) {
        sweep();
        interruptible_sleep(g_opts.interval_sec, &g_running);
    }

    logmsg(LOG_INFO, "shutting down");
    /*
     * Unconditional: a dry run never spawns a child or a thread, so
     * there is nothing to signal and nothing to join, but the slot array
     * recorder_init() allocated is still ours to release.
     */
    recorder_shutdown();

    config_free();
    http_global_cleanup();
    logmsg(LOG_INFO, "bye");
    return 0;
}
