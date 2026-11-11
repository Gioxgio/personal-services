/*
 * ss.h - what is genuinely shared across modules
 * that have no other reason to know each other: the
 * runtime options type and the log column width.
 *
 * The data model lives in urlparse.h (channel_t is a
 * domain type, not a shared-utility type). Modules must
 * not reach into each other's internals; they talk
 * through the small APIs declared in util.h, log.h,
 * http.h, jsonlite.h, urlparse.h, config.h,
 * twitch_auth.h, probe.h and recorder.h.
 */

#ifndef SS_H
#define SS_H

#include <limits.h>
#include <stdbool.h>

/* Command line / environment, resolved once in main(). */
typedef struct {
    long        interval_sec;               /* poll period, default 300      */
    char        out_dir[PATH_MAX];           /* recordings root              */
    char        channels_file[PATH_MAX];     /* config path                  */
    bool        dry_run;                     /* -n: probe only, never record */
} options_t;

/*
 * Width of the channel slug column in the LIVE/REC/DONE/FAIL/STOP log lines.
 * Shared so those lines stay aligned no matter which module emits them.
 */
#define LOG_SLUG_COL 16

#endif /* SS_H */
