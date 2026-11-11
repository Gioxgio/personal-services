#include "probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "jsonlite.h"
#include "log.h"
#include "twitch_auth.h"
#include "util.h"

/*
 * Twitch: GET /helix/streams?user_login=X. Live <=> "type":"live" present.
 * Retries once with a fresh token on HTTP 401, and only on HTTP 401: a
 * timeout says nothing about the token, so dropping it there just spends a
 * mint on nothing.
 */
static probe_result_t twitch_live(const channel_t *ch,
                                  char **out_stream_url)
{
    char url[512], client_hdr[160], auth_hdr[640];
    snprintf(url, sizeof url,
             "https://api.twitch.tv/helix/streams?user_login=%s", ch->slug);
    snprintf(client_hdr, sizeof client_hdr, "Client-ID: %s", twitch_auth_client_id());

    for (int attempt = 0; attempt < 2; attempt++) {
        const char *token = twitch_auth_token();
        if (!token) {
            /* No credentials, or minting failed: Twitch probing is off. */
            return PROBE_OFFLINE;
        }

        snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Bearer %s", token);
        const char *hdrs[] = { client_hdr, auth_hdr, NULL };

        http_buffer body = { 0 };
        long status = 0;
        int rc = http_get_ex(url, hdrs, &body, &status);
        if (rc == 0) {
            bool live = strstr(body.data, "\"type\":\"live\"") != NULL;
            http_buffer_free(&body);
            if (live)
                *out_stream_url = xstrdup(url);
            return live ? PROBE_LIVE : PROBE_OFFLINE;
        }
        http_buffer_free(&body);

        /* Only a 401 says the token is bad, so only a 401 may cost a mint.
         * Anything else - a transport failure, or a status we cannot
         * interpret as live/not-live - is a failed probe, and it leaves the
         * cached token alone. */
        if (status != 401)
            return PROBE_ERROR;

        if (attempt == 1) {
            logmsg(LOG_WARN, "twitch: still unauthorized after re-minting the token");
            return PROBE_OFFLINE;
        }
        twitch_auth_invalidate();
    }
    return PROBE_OFFLINE;
}

/*
 * YouTube live detection.
 *
 * Findings from live testing (why the chain looks like this):
 *
 *  - Channel pages (@handle) carry NO live markers at all: no isLiveNow, no
 *    isLiveContent, no BADGE_STYLE_TYPE_LIVE_NOW, even for channels confirmed
 *    live. They cannot be probed directly.
 *  - "isLiveNow":true only appears on a watch page (/watch?v=ID).
 *  - The RSS feed is NOT a reliable source: it is ordered by upload time, so a
 *    channel that posts Shorts buries its live stream (measured on @LofiGirl:
 *    three concurrent live streams sat at RSS positions 5, 10 and 13).
 *  - The /streams tab lists ONLY live broadcasts, newest first, so the
 *    currently live stream is normally the first entry.
 *
 * Chain: /streams page -> first few videoIds -> watch page -> isLiveNow:true.
 * No channel-id resolution is needed, so there is nothing to cache.
 */

/* How many /streams entries to verify before calling a channel offline. */
#define YT_STREAMS_PROBES 3

static probe_result_t youtube_live(const channel_t *ch,
                                       char **out_stream_url)
{
    char page[512];
    snprintf(page, sizeof page, "%s/streams", ch->url);

    http_buffer body = { 0 };
    if (http_get(page, NULL, &body) != 0) {
        http_buffer_free(&body);
        return PROBE_ERROR;
    }

    /*
     * Collect the first few distinct videoIds in document
     * order: empty values and repeats of an earlier one
     * are skipped, and the scan stops once the cap is
     * reached.
     */
    char vids[YT_STREAMS_PROBES][32];
    size_t nv = json_collect_strings(body.data, "videoId",
                                       vids, YT_STREAMS_PROBES);
    http_buffer_free(&body);

    if (nv == 0) {
        logmsg(LOG_DEBUG, "youtube: %s /streams empty", ch->slug);
        return PROBE_OFFLINE;
    }

    /*
     * Verify newest-first; the first live hit wins. A watch page we could not
     * fetch is not evidence of anything, so it only decides the outcome when
     * every one of them failed.
     */
    bool all_failed = true;
    for (size_t k = 0; k < nv; k++) {
        char watch[128];
        snprintf(watch, sizeof watch,
                 "https://www.youtube.com/watch?v=%s", vids[k]);

        http_buffer w = { 0 };
        if (http_get(watch, NULL, &w) != 0) {
            http_buffer_free(&w);
            continue;
        }
        all_failed = false;

        bool live = strstr(w.data, "\"isLiveNow\":true") != NULL;
        http_buffer_free(&w);

        if (live) {
            *out_stream_url = xstrdup(watch);
            return PROBE_LIVE;
        }
    }

    return all_failed ? PROBE_ERROR : PROBE_OFFLINE;
}

/*
 * The single entry point: dispatch on platform and leave
 * *out_stream_url NULL unless the channel is live. On PROBE_LIVE
 * the caller owns the freshly allocated URL; the channel itself
 * is read, never written.
 */
probe_result_t probe_is_live(const channel_t *ch, char **out_stream_url)
{
    *out_stream_url = NULL;
    if (ch->src == SRC_TWITCH)
        return twitch_live(ch, out_stream_url);
    return youtube_live(ch, out_stream_url);
}
