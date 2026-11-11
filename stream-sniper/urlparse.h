/*
 * urlparse.h - the channel data model and config-line
 * normalisation.
 *
 * channel_t is a domain type, not a shared-utility type,
 * so it lives here rather than in ss.h: the slug,
 * the canonical URL and the platform are allocated once
 * at load and immutable afterwards. channel_parse() turns
 * one config line into a fully populated channel_t, so
 * every accepted form (bare login, full URL, @handle,
 * bare UC… id) is normalised in one place and is
 * unit-testable without touching the config file.
 */

#ifndef URLPARSE_H
#define URLPARSE_H

#include <stdbool.h>
#include <stddef.h>

/* Which platform a channel lives on. */
typedef enum {
    SRC_TWITCH,
    SRC_YOUTUBE
} source_t;

/*
 * One configured channel.
 *
 * slug and url are allocated once by channel_parse() and
 * immutable afterwards. There is no mutable state here at
 * all: the probe is a pure function that hands the live
 * stream URL back through an out-parameter, and the
 * recorder's own runtime state (busy flag, pid, thread,
 * the stream URL of a running recording) lives in
 * recorder.c's private rec_slot_t[], not here.
 */
typedef struct {
    char    *slug;           /* sanitised name, used as output subdirectory */
    char    *url;            /* canonical channel URL: probed this          */
    source_t src;
} channel_t;

/* Put the channel in the empty state: owns nothing, all fields zero. */
void channel_init(channel_t *ch);

/*
 * Turn one config line into a fully populated channel_t.
 *
 * Accepts "twitch: <login>", "youtube: @<handle>",
 * "youtube: <UC… channel id>", full URLs with or without a
 * platform prefix, and bare logins (platform inferred from
 * the URL, Twitch by default). A query string or a
 * fragment never reaches the slug.
 *
 * Returns false if the line cannot be understood, in which
 * case *out is left in the channel_init() state and owns
 * nothing - a rejected line leaks nothing.
 */
bool channel_parse(const char *line, channel_t *out);

/*
 * Release what the channel owns and return it to the
 * channel_init() state, so a freed channel is safe to
 * re-parse, drop, or free again.
 */
void channel_free(channel_t *ch);

#endif /* URLPARSE_H */
