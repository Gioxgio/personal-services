#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static log_level g_min_level = LOG_INFO;

void log_set_level(log_level min) { g_min_level = min; }

void logmsg(log_level level, const char *fmt, ...)
{
    if (level < g_min_level)
        return;

    static const char *tags[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

    time_t now = time(NULL);
    struct tm tm;
    char stamp[16];
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    flockfile(stderr);
    fprintf(stderr, "[%s] %s ", stamp, tags[level]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
    funlockfile(stderr);
}