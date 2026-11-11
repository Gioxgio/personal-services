#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

channel_t *g_channels;
size_t     g_nchannels;

int config_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        logmsg(LOG_ERROR, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    size_t cap = 16;
    g_channels = xmalloc(cap * sizeof *g_channels);
    g_nchannels = 0;

    char line[2048];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == '\0')
            continue;

        char *copy = xstrdup(s);
        trim(copy);

        channel_t ch;
        if (!channel_parse(copy, &ch)) {
            logmsg(LOG_WARN, "%s:%d: cannot parse '%s', skipped", path, lineno, copy);
            free(copy);
            continue;
        }
        free(copy);

        if (g_nchannels == cap) {
            cap *= 2;
            channel_t *tmp = realloc(g_channels, cap * sizeof *g_channels);
            if (!tmp) {
                logmsg(LOG_ERROR, "out of memory");
                exit(EXIT_FAILURE);
            }
            g_channels = tmp;
        }
        g_channels[g_nchannels++] = ch;
        logmsg(LOG_INFO, "channel: %s", ch.url);
    }

    fclose(f);
    return 0;
}

void config_free(void)
{
    for (size_t i = 0; i < g_nchannels; i++)
        channel_free(&g_channels[i]);
    free(g_channels);
    g_channels = NULL;
    g_nchannels = 0;
}
