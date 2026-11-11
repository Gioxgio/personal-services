/*
 * probe.h - live-state detection per platform.
 *
 * Each probe performs a bounded number of HTTP requests and
 * inspects the responses for known live markers. Probes never
 * touch a running recorder and never mutate the channel they
 * are given: the stream URL, when there is one, comes back
 * through an out-parameter the caller owns.
 */

#ifndef PROBE_H
#define PROBE_H

#include <stdbool.h>
#include "urlparse.h"

/*
 * Three outcomes, not two. Folding a transport failure into "offline" makes a
 * dead host indistinguishable from an idle channel, so it gets retried on
 * every sweep forever; keeping it separate lets the caller back off instead.
 */
typedef enum {
    PROBE_OFFLINE,   /* definitively not live                        */
    PROBE_LIVE,      /* live; *out_stream_url is the broadcast URL */
    PROBE_ERROR      /* the probe itself failed: DNS, connect, TLS, timeout */
} probe_result_t;

/*
 * Probe one channel. Never mutates *ch.
 *
 * PROBE_LIVE      -> *out_stream_url is an xstrdup'd exact stream URL
 *                    (YouTube /watch?v=ID, or the Twitch Helix
 *                    endpoint); the caller frees it.
 * PROBE_OFFLINE / PROBE_ERROR -> *out_stream_url is set to NULL.
 *
 * out_stream_url must not be NULL.
 */
probe_result_t probe_is_live(const channel_t *ch, char **out_stream_url);

#endif /* PROBE_H */
