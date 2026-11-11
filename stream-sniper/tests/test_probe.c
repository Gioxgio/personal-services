/*
 * test_probe - the two live-state predicates over canned
 * responses: Twitch's "type":"live" and YouTube's
 * "isLiveNow":true. Offline.
 *
 * probe_is_live() reaches the world through http_get() /
 * http_get_ex() and the twitch_auth_* token helpers, so this
 * file stubs exactly that seam: every URL probe.c can build
 * maps to a fixture under tests/fixtures/, and a URL with no
 * fixture stands in for a dead host. The production probe.c
 * is compiled in via #include below - only the I/O seam is
 * fake, so the dispatch, the JSON scan and the predicates are
 * the real code. Notifications are stubbed to no-ops as well: the
 * recorder is linked in, and notify is an I/O layer like http.
 * (The Makefile keeps probe.o, http.o, twitch_auth.o and
 * notify.o out of this binary's link.)
 *
 * Run from the project root (make test does): the fixture
 * paths are relative.
 */

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "notify.h"
#include "probe.h"
#include "twitch_auth.h"
#include "urlparse.h"

/* --- the URL -> fixture map --- */

static const char *fixture_for(const char *url)
{
    /* Twitch: .../helix/streams?user_login=<slug> */
    const char *q = strstr(url, "user_login=");
    if (q) {
        const char *slug = q + strlen("user_login=");
        if (!strncmp(slug, "live", 4))
            return "tests/fixtures/twitch_streams_live.html";
        if (!strncmp(slug, "off", 3))
            return "tests/fixtures/twitch_streams_offline.html";
        if (!strncmp(slug, "stale", 5))
            return "tests/fixtures/twitch_streams_live.html";
        return NULL;                    /* unknown slug: dead host */
    }

    /* YouTube watch page: the videoId picks the fixture. */
    const char *w = strstr(url, "watch?v=");
    if (w) {
        const char *vid = w + strlen("watch?v=");
        return !strncmp(vid, "LIVE", 4)
                   ? "tests/fixtures/youtube_watch_live.html"
                   : "tests/fixtures/youtube_watch_vod.html";
    }

    /* YouTube /streams page: identify the channel by its handle. */
    if (strstr(url, "@livechan"))
        return "tests/fixtures/youtube_streams_live.html";
    if (strstr(url, "@offchan"))
        return "tests/fixtures/youtube_streams_offline.html";
    if (strstr(url, "@emptychan"))
        return "tests/fixtures/youtube_streams_empty.html";
    return NULL;
}

/* Read a whole file into a NUL-terminated heap buffer; NULL on failure. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 256 + 1 > cap) {
            cap = cap ? cap * 2 : 1024;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = p;
        }
        size_t n = fread(buf + len, 1, 256, f);
        len += n;
        if (n < 256) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                return NULL;
            }
            break;
        }
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

/*
 * The HTTP seam. Every failure leaves *out untouched: the
 * contract hands back an empty buffer, and the caller zero-
 * initialises before the call.
 */
static int stub_get_ex(const char *url, const char *const *headers,
                       http_buffer *out, long *status_out)
{
    (void)headers;

    const char *fix = fixture_for(url);
    if (!fix) {
        if (status_out)
            *status_out = 0;            /* no HTTP response received */
        return -1;
    }

    /*
     * The "stale" slug simulates a rejected token: the first
     * attempt draws a 401 (probe.c drops the cached token and
     * re-mints), the second sees the channel live. The fixture
     * is the same; only the status differs.
     */
    const char *q = strstr(url, "user_login=");
    if (q && !strncmp(q + strlen("user_login="), "stale", 5)) {
        static int attempts;
        if (++attempts == 1) {
            if (status_out)
                *status_out = 401;
            return -1;
        }
    }

    char *body = read_file(fix);
    if (!body) {
        if (status_out)
            *status_out = 0;
        return -1;
    }
    out->data = body;
    out->len = strlen(body);
    if (status_out)
        *status_out = 200;
    return 0;
}

int http_get(const char *url, const char *const *headers, http_buffer *out)
{
    return stub_get_ex(url, headers, out, NULL);
}

int http_get_ex(const char *url, const char *const *headers,
                http_buffer *out, long *status_out)
{
    return stub_get_ex(url, headers, out, status_out);
}

/*
 * The contract's other half, restated verbatim: probe.c releases
 * every body it is handed, on every path, so the stub's buffers
 * must be freed exactly like the real ones. http.o cannot be
 * linked here - its getters would collide with the stubs above -
 * which is the only reason this function is not the original.
 */
void http_buffer_free(http_buffer *buf)
{
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

/* --- the twitch_auth seam: a token that can be invalidated --- */

static const char *g_token = "first-token";
static int g_invalidated;

const char *twitch_auth_client_id(void)
{
    return "test-client-id";
}

const char *twitch_auth_token(void)
{
    return g_token;
}

void twitch_auth_invalidate(void)
{
    g_token = "second-token";
    g_invalidated++;
}

/* --- the notify seam: notifications off --- */

void notify_init(void) { }

void notify_recording_started(const channel_t *ch)
{
    (void)ch;
}

void notify_recording_stopped(const channel_t *ch, bool clean,
                              const char *dur)
{
    (void)ch;
    (void)clean;
    (void)dur;
}

/* The production probe, compiled in against the seam above. */
#include "../probe.c"

int main(void)
{
    channel_t ch;
    char *url;

    /* --- Twitch: the "type":"live" predicate --- */

    CHECK(channel_parse("twitch: livechan", &ch));
    url = (char *)"sentinel";
    CHECK(probe_is_live(&ch, &url) == PROBE_LIVE);
    CHECK_STR_EQ(url,
                 "https://api.twitch.tv/helix/streams?user_login=livechan");
    free(url);
    channel_free(&ch);

    /* The predicate absent: offline, and *out is left NULL. */
    CHECK(channel_parse("twitch: offchan", &ch));
    url = (char *)"sentinel";
    CHECK(probe_is_live(&ch, &url) == PROBE_OFFLINE);
    CHECK(url == NULL);
    channel_free(&ch);

    /*
     * A dead host is PROBE_ERROR, never PROBE_OFFLINE: folding a
     * transport failure into "offline" is the §3.1 #4 defect.
     */
    CHECK(channel_parse("twitch: deadchan", &ch));
    url = (char *)"sentinel";
    CHECK(probe_is_live(&ch, &url) == PROBE_ERROR);
    CHECK(url == NULL);
    channel_free(&ch);

    /* A 401 costs exactly one re-mint; the retry sees a live channel. */
    CHECK(channel_parse("twitch: stalechan", &ch));
    url = NULL;
    CHECK(probe_is_live(&ch, &url) == PROBE_LIVE);
    CHECK(g_invalidated == 1);
    CHECK_STR_EQ(url,
                 "https://api.twitch.tv/helix/streams?user_login=stalechan");
    free(url);
    channel_free(&ch);

    /* --- YouTube: the "isLiveNow":true predicate --- */

    CHECK(channel_parse("youtube: @livechan", &ch));
    url = (char *)"sentinel";
    CHECK(probe_is_live(&ch, &url) == PROBE_LIVE);
    CHECK_STR_EQ(url, "https://www.youtube.com/watch?v=LIVEabc123");
    free(url);
    channel_free(&ch);

    /* /streams entries that are not live: offline. */
    CHECK(channel_parse("youtube: @offchan", &ch));
    url = (char *)"sentinel";
    CHECK(probe_is_live(&ch, &url) == PROBE_OFFLINE);
    CHECK(url == NULL);
    channel_free(&ch);

    /* No /streams entries at all: offline. */
    CHECK(channel_parse("youtube: @emptychan", &ch));
    url = (char *)"sentinel";
    CHECK(probe_is_live(&ch, &url) == PROBE_OFFLINE);
    CHECK(url == NULL);
    channel_free(&ch);

    /* The /streams page itself unreachable: PROBE_ERROR. */
    CHECK(channel_parse("youtube: @deadchan", &ch));
    url = (char *)"sentinel";
    CHECK(probe_is_live(&ch, &url) == PROBE_ERROR);
    CHECK(url == NULL);
    channel_free(&ch);

    /*
     * The probe never mutates the channel: slug, url and src
     * hold the same bytes before and after.
     */
    CHECK(channel_parse("youtube: @livechan", &ch));
    char *slug_before = xstrdup(ch.slug);
    char *url_before = xstrdup(ch.url);
    url = NULL;
    (void)probe_is_live(&ch, &url);
    CHECK_STR_EQ(ch.slug, slug_before);
    CHECK_STR_EQ(ch.url, url_before);
    CHECK(ch.src == SRC_YOUTUBE);
    free(url);
    free(slug_before);
    free(url_before);
    channel_free(&ch);

    return harness_exit("test_probe");
}
