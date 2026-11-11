/*
 * log.h - levelled, timestamped logging to stderr.
 */

#ifndef LOG_H
#define LOG_H

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level;

/* Messages above this level are suppressed. Default: LOG_INFO. */
void log_set_level(log_level min);

/* printf-style, writes to stderr with an HH:MM:SS prefix. */
void logmsg(log_level level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* LOG_H */