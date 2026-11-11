#include "jsonlite.h"

#include <stdbool.h>
#include <string.h>

/*
 * Locate the next "key":" at or after *from, and return a pointer
 * to the first character of its value. Advances nothing: the
 * caller moves its cursor past the value it just read. key must
 * be a quoted JSON key, so a hit inside a longer key does not
 * count - the byte before it must be the opening quote, and the
 * key must be closed, colon-separated, and value-opened by a
 * quote.
 */
static const char *value_at(const char *json, const char *from,
                            const char *key)
{
    size_t klen = strlen(key);
    const char *p = from;

    while ((p = strstr(p, key)) != NULL) {
        /*
         * Short-circuits keep every indexed read in bounds: the
         * bytes after the key are only examined once the key is
         * closed by a quote, which guarantees they are part of
         * the string (at worst its terminator).
         */
        if (p > json && p[-1] == '"' &&
            p[klen] == '"' && p[klen + 1] == ':' && p[klen + 2] == '"')
            return p + klen + 3;
        p++;
    }
    return NULL;
}

size_t json_find_string(const char *json, const char *key,
                        char *out, size_t outsz)
{
    if (!json || !key || !*key || !out || outsz == 0)
        return 0;

    const char *v = value_at(json, json, key);
    if (!v)
        return 0;

    size_t i = 0;
    while (*v && *v != '"' && i + 1 < outsz)
        out[i++] = *v++;
    out[i] = '\0';
    return i;
}

size_t json_collect_strings(const char *json, const char *key,
                            char out[][32], size_t max)
{
    if (!json || !key || !*key || !out || max == 0)
        return 0;

    size_t n = 0;
    const char *p = json;

    while (n < max) {
        const char *v = value_at(json, p, key);
        if (!v)
            break;

        /* Copy the value, truncated to the row's width. */
        size_t i = 0;
        while (*v && *v != '"' && i + 1 < sizeof out[n])
            out[n][i++] = *v++;
        out[n][i] = '\0';

        /* Resume the scan past the closing quote, when there is one. */
        p = (*v == '"') ? v + 1 : v;

        /* Empty values and repeats of an earlier one are skipped. */
        if (i > 0) {
            bool dup = false;
            for (size_t k = 0; k < n; k++)
                if (strcmp(out[k], out[n]) == 0)
                    dup = true;
            if (!dup)
                n++;
        }
    }
    return n;
}
