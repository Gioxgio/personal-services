#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "allowlist.h"

#include <strings.h>
#include <stdlib.h>
#include <string.h>

/*
 * Split `spec` on commas (whitespace around entries is trimmed, empty entries
 * ignored) and duplicate every entry into `list`. Returns true on success.
 */
bool allowlist_parse(const char *spec, allowlist_t *list) {
    if (!list)
        return false;

    list->domains = NULL;
    list->count = 0;

    if (!spec || spec[0] == '\0')
        return true; /* empty allowlist */

    char *copy = strdup(spec);
    if (!copy)
        return false;

    /* First pass: count the non-empty entries. */
    int count = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",\t\r\n ", &save); tok;
         tok = strtok_r(NULL, ",\t\r\n ", &save)) {
        count++;
    }

    if (count == 0) {
        free(copy);
        return true;
    }

    char **domains =
        calloc((size_t)count + 1, sizeof(char *)); /* +1 for a NUL sentinel */
    if (!domains) {
        free(copy);
        return false;
    }

    /* Second pass: duplicate the entries. strtok_r consumed `copy`, re-tokenize. */
    free(copy);
    copy = strdup(spec);
    if (!copy) {
        free(domains);
        return false;
    }

    bool ok = true;
    int n = 0;
    save = NULL;
    for (char *tok = strtok_r(copy, ",\t\r\n ", &save); tok;
         tok = strtok_r(NULL, ",\t\r\n ", &save)) {
        domains[n] = strdup(tok);
        if (!domains[n]) {
            ok = false;
            break;
        }
        n++;
    }

    free(copy);

    if (!ok) {
        for (int i = 0; i < n; i++)
            free(domains[i]);
        free(domains);
        list->domains = NULL;
        list->count = 0;
        return false;
    }

    list->domains = domains;
    list->count = (size_t)n;
    domains[count] = NULL;
    return true;
}

void allowlist_free(allowlist_t *list) {
    if (!list || !list->domains)
        return;
    for (size_t i = 0; i < list->count; i++)
        free(list->domains[i]);
    free(list->domains);
    list->domains = NULL;
    list->count = 0;
}

/*
 * Copy the host of `url` into `out` (capacity `cap`): the part after an
 * optional "scheme://" and before the first port, path, query or fragment.
 * Returns the host length, or -1 when there is no usable host.
 */
static int extract_host(const char *url, char *out, size_t cap) {
    const char *start = url;
    const char *scheme = strstr(url, "://");
    if (scheme)
        start = scheme + 3;

    const char *end = start;
    while (*end && *end != '/' && *end != '?' && *end != '#' && *end != ':')
        end++;

    size_t host_len = (size_t)(end - start);
    if (host_len == 0 || host_len >= cap)
        return -1;
    memcpy(out, start, host_len);
    out[host_len] = '\0';
    return (int)host_len;
}

/* True when `host` equals `domain` or is a subdomain of it (dot-prefixed). */
static bool host_matches_domain(const char *host, const char *domain) {
    if (strcasecmp(host, domain) == 0)
        return true;

    size_t hlen = strlen(host);
    size_t dlen = strlen(domain);
    if (hlen <= dlen)
        return false;
    if (host[hlen - dlen - 1] != '.')
        return false;
    return strcasecmp(host + hlen - dlen, domain) == 0;
}

bool is_supported(const char *url, const allowlist_t *list) {
    if (!url || !list || !list->domains)
        return false;

    char host[256];
    if (extract_host(url, host, sizeof host) < 0)
        return false;

    for (size_t i = 0; i < list->count; i++) {
        if (host_matches_domain(host, list->domains[i]))
            return true;
    }
    return false;
}
