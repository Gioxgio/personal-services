#include "urlparse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char TWITCH_BASE[] = "https://twitch.tv/";
static const char YT_BASE[]     = "https://www.youtube.com/";

/*
 * Cut a config line at the first '?' or '#': tracking
 * parameters and fragments are not part of the channel
 * identifier and must never reach the slug.
 */
static void cut_query(char *s)
{
    char *cut = strpbrk(s, "?#");
    if (cut)
        *cut = '\0';
}

void channel_init(channel_t *ch)
{
    memset(ch, 0, sizeof *ch);
}

void channel_free(channel_t *ch)
{
    free(ch->slug);
    free(ch->url);
    channel_init(ch);
}

/*
 * Turn one config line into a fully populated channel_t.
 * Returns false if the line cannot be understood (the
 * caller logs and skips it); *out is left owning nothing
 * on every path out.
 */
bool channel_parse(const char *line, channel_t *out)
{
    channel_init(out);

    char *lc = lower_dup(line);
    const char *p = line;

    if      (!strncmp(lc, "twitch:",  7))  { out->src = SRC_TWITCH;  p = line + 7; }
    else if (!strncmp(lc, "youtube:", 8))  { out->src = SRC_YOUTUBE; p = line + 8; }
    else if (strstr(lc, "twitch.tv"))       { out->src = SRC_TWITCH; }
    else if (strstr(lc, "youtube.com") || strstr(lc, "youtu.be"))
                                            { out->src = SRC_YOUTUBE; }
    else                                   { out->src = SRC_TWITCH; }
    free(lc);

    char buf[1024];
    snprintf(buf, sizeof buf, "%s", p);
    trim(buf);
    strip_spaces(buf);
    if (buf[0] == '\0')
        return false;

    if (out->src == SRC_TWITCH) {
        /* Accept bare login names and full twitch.tv URLs alike. */
        const char *name = buf;

        /* One copy, freed on every path out: the old inline
         * lower_dup() in the condition leaked on every single
         * Twitch channel. */
        char *lcurl = lower_dup(buf);
        bool absolute = !strncmp(lcurl, "https://", 8);
        free(lcurl);

        if (absolute) {
            /* Cuts buf at any '?' / '#', so the segment comes back bare. */
            const char *seg = last_path_segment(buf);
            if (!seg) {
                channel_free(out);
                return false;
            }
            name = seg;
        }
        char login[256];
        /* Precision == sizeof - 1: the source is a fragment of buf[1024],
         * so a plain "%s" would truncate here on purpose and -Wformat-
         * truncation (which fires at the debug build's -O1) cannot see
         * that the cut is intended. The precision states it. */
        snprintf(login, sizeof login, "%.255s", name);
        char *slash = strchr(login, '/');
        if (slash) *slash = '\0';
        for (char *q = login; *q; q++)
            *q = (char)tolower((unsigned char)*q);

        out->slug = sanitise(login);
        size_t n = strlen(TWITCH_BASE) + strlen(out->slug) + 1;
        out->url = xmalloc(n);
        snprintf(out->url, n, "%s%s", TWITCH_BASE, out->slug);
    } else {
        char handle[512] = { 0 };
        char *low = lower_dup(buf);

        /* Every copy out of buf[1024] into handle[512] truncates by design;
         * the precisions below (literal + N == sizeof handle - 1) say so,
         * so the -O1 debug build's -Wformat-truncation stays quiet. */
        if (buf[0] == '@') {
            snprintf(handle, sizeof handle, "%.511s", buf);
        } else if (!strncmp(low, "uc", 2) && strlen(buf) == 24) {
            snprintf(handle, sizeof handle, "channel/%.503s", buf);
        } else if (!strncmp(low, "http://", 7) || !strncmp(low, "https://", 8)) {
            const char *host = strstr(buf, "://") + 3;
            const char *slash = strchr(host, '/');
            snprintf(handle, sizeof handle, "%.511s", slash ? slash + 1 : host);
            strip_spaces(handle);
            cut_query(handle);
            if (handle[0] == '\0')
                snprintf(handle, sizeof handle, "%.511s", host);
        } else {
            snprintf(handle, sizeof handle, "@%.510s", buf);
        }
        free(low);

        out->slug = sanitise(handle);
        size_t n = strlen(YT_BASE) + strlen(handle) + 1;
        out->url = xmalloc(n);
        snprintf(out->url, n, "%s%s", YT_BASE, handle);
    }

    return true;
}
