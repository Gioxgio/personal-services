#ifndef ALLOWLIST_H
#define ALLOWLIST_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A list of allowed domains, e.g. youtube.com, youtu.be.
 *
 * A host matches a listed domain when it is either equal to it or a
 * subdomain of it, so www.youtube.com and m.youtube.com match youtube.com,
 * and www.youtu.be matches youtu.be. Matching is case-insensitive.
 */
typedef struct {
    char **domains; /* heap-allocated array of heap-allocated strings */
    size_t count;
} allowlist_t;

/*
 * Parse a comma-separated domain specification into `list`. Whitespace around
 * entries is trimmed and empty entries are ignored. The array and each string
 * are heap-allocated; release them with allowlist_free(). Returns true on
 * success. An empty or NULL spec yields an empty allowlist. `list` is reset
 * to empty on failure.
 */
bool allowlist_parse(const char *spec, allowlist_t *list);

/* Release memory owned by `list`. Safe to call on an empty list. */
void allowlist_free(allowlist_t *list);

/*
 * Return true if the host of `url` is allowed by `list`. The URL may include a
 * scheme, port, path, query and fragment; only the host is compared. Returns
 * false for URLs without a usable host.
 */
bool is_supported(const char *url, const allowlist_t *list);

#endif
