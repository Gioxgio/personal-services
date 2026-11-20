#include "filename.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char* sanitize_filename(const char* s) {
    if (!s)
        return NULL;

    /* +2: the '_' fallback for an empty string needs a character and a NUL. */
    char* out = malloc(strlen(s) + 2);
    if (!out)
        return NULL;

    size_t j = 0;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        out[j++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }
    if (j == 0)
        out[j++] = '_';
    out[j] = '\0';
    return out;
}

char* sanitize_filename_keep_extension(const char* s) {
    if (!s)
        return NULL;

    size_t len = strlen(s);
    if (len == 0)
        return sanitize_filename(s);

    const char* slash = strrchr(s, '/');
    const char* base = slash ? slash + 1 : s;
    const char* dot = strrchr(base, '.');
    if (dot == base)
        dot = NULL; /* a leading dot is not an extension */
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    if (stem_len == 0)
        return sanitize_filename(s); /* no stem to keep (e.g. ".mp4") */

    char* stem = malloc(stem_len + 1);
    if (!stem)
        return NULL;
    memcpy(stem, base, stem_len);
    stem[stem_len] = '\0';
    char* clean = sanitize_filename(stem);
    free(stem);
    if (!clean)
        return NULL;

    size_t prefix_len = slash ? (size_t)(slash - s) + 1 : 0;
    size_t ext_len = dot ? strlen(dot) : 0;
    char* out = malloc(prefix_len + strlen(clean) + ext_len + 1);
    if (!out) {
        free(clean);
        return NULL;
    }
    memcpy(out, s, prefix_len);
    memcpy(out + prefix_len, clean, strlen(clean));
    if (ext_len)
        memcpy(out + prefix_len + strlen(clean), dot, ext_len);
    out[prefix_len + strlen(clean) + ext_len] = '\0';
    free(clean);
    return out;
}
