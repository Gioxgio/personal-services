#include "json.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

static bool skip_string(const char** p) {
    (*p)++; /* skip opening quote */
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++; /* skip backslash */
            if (**p == '\0')
                return false;
            if (**p == 'u') {
                /* Skip \uXXXX */
                (*p)++;
                for (int i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)**p))
                        return false;
                    (*p)++;
                }
            } else {
                (*p)++; /* skip escaped character */
            }
        } else {
            (*p)++;
        }
    }
    if (**p != '"')
        return false;
    (*p)++; /* skip closing quote */
    return true;
}

static bool skip_value(const char** p) {
    while (**p && isspace((unsigned char)**p))
        (*p)++;

    if (**p == '\0')
        return false;

    if (**p == '"') {
        return skip_string(p);
    } else if (**p == '{') {
        (*p)++;
        while (**p && isspace((unsigned char)**p))
            (*p)++;
        if (**p == '}') {
            (*p)++;
            return true;
        }
        while (**p) {
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p != '"')
                return false;
            if (!skip_string(p))
                return false;
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p != ':')
                return false;
            (*p)++;
            if (!skip_value(p))
                return false;
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == '}') {
                (*p)++;
                return true;
            }
            return false;
        }
        return false;
    } else if (**p == '[') {
        (*p)++;
        while (**p && isspace((unsigned char)**p))
            (*p)++;
        if (**p == ']') {
            (*p)++;
            return true;
        }
        while (**p) {
            if (!skip_value(p))
                return false;
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == ']') {
                (*p)++;
                return true;
            }
            return false;
        }
        return false;
    } else {
        /* number, true, false, null */
        while (**p && **p != ',' && **p != '}' && **p != ']' && !isspace((unsigned char)**p)) {
            (*p)++;
        }
        return true;
    }
}

bool json_parse_download(const char* json_raw, download_payload_t* out_payload) {
    if (!json_raw || !out_payload)
        return false;

    out_payload->url[0] = '\0';
    out_payload->format[0] = '\0';
    out_payload->chat_id = 0;

    const char* p = json_raw;

    /* Skip leading whitespace */
    while (*p && isspace((unsigned char)*p))
        p++;

    /* Expect '{' */
    if (*p != '{')
        return false;
    p++;

    /* Skip whitespace after '{' */
    while (*p && isspace((unsigned char)*p))
        p++;

    /* Empty object: no url field */
    if (*p == '}')
        return false;

    bool found_url = false;

    while (*p) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p))
            p++;

        /* Expect '"' for key */
        if (*p != '"')
            return false;
        p++;

        /* Read key (no escapes supported in keys) */
        char key[64];
        size_t key_len = 0;
        while (*p && *p != '"') {
            if (*p == '\\')
                return false; /* escape in key not supported */
            if (key_len < sizeof(key) - 1) {
                key[key_len++] = *p;
            }
            p++;
        }
        if (*p != '"')
            return false; /* unterminated key */
        p++;
        key[key_len] = '\0';

        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p))
            p++;

        /* Expect ':' */
        if (*p != ':')
            return false;
        p++;

        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p))
            p++;

        if (strcmp(key, "url") == 0) {
            /* Expect '"' for string value */
            if (*p != '"')
                return false;
            p++;

            size_t len = 0;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (*p == '\0')
                        return false;
                    char esc = *p++;
                    switch (esc) {
                        case '"':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '"';
                            break;
                        case '\\':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '\\';
                            break;
                        case '/':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '/';
                            break;
                        case 'b':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '\b';
                            break;
                        case 'f':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '\f';
                            break;
                        case 'n':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '\n';
                            break;
                        case 'r':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '\r';
                            break;
                        case 't':
                            if (len < MAX_DOWNLOAD_URL)
                                out_payload->url[len++] = '\t';
                            break;
                        case 'u':
                            return false; /* \uXXXX not supported */
                        default:
                            return false; /* invalid escape */
                    }
                    continue;
                }
                if (len >= MAX_DOWNLOAD_URL)
                    return false;
                out_payload->url[len++] = *p++;
            }
            if (*p != '"')
                return false; /* unterminated string */
            p++;
            out_payload->url[len] = '\0';

            if (len == 0)
                return false; /* reject empty url */
            found_url = true;
        } else if (strcmp(key, "format") == 0) {
            if (*p != '"')
                return false;
            p++;

            size_t len = 0;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (*p == '\0')
                        return false;
                    char esc = *p++;
                    switch (esc) {
                        case '"':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '"';
                            break;
                        case '\\':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '\\';
                            break;
                        case '/':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '/';
                            break;
                        case 'b':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '\b';
                            break;
                        case 'f':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '\f';
                            break;
                        case 'n':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '\n';
                            break;
                        case 'r':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '\r';
                            break;
                        case 't':
                            if (len < MAX_DOWNLOAD_FORMAT)
                                out_payload->format[len++] = '\t';
                            break;
                        case 'u':
                            return false; /* \uXXXX not supported */
                        default:
                            return false; /* invalid escape */
                    }
                    continue;
                }
                if (len >= MAX_DOWNLOAD_FORMAT)
                    return false;
                out_payload->format[len++] = *p++;
            }
            if (*p != '"')
                return false; /* unterminated string */
            p++;
            out_payload->format[len] = '\0';
        } else if (strcmp(key, "chat_id") == 0) {
            /* Expect a non-negative integer (JSON numbers are unquoted). */
            if (*p < '0' || *p > '9')
                return false;
            long long value = 0;
            while (*p >= '0' && *p <= '9') {
                if (value > ((long long)INT_MAX - (*p - '0')) / 10)
                    return false;
                value = value * 10 + (*p - '0');
                p++;
            }
            out_payload->chat_id = (int)value;
        } else {
            /* Skip non-target values (strings, numbers, objects, arrays). */
            if (!skip_value(&p))
                return false;
        }

        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p))
            p++;

        if (*p == ',') {
            p++;
            continue;
        } else if (*p == '}') {
            p++;
            break;
        } else {
            return false;
        }
    }

    /* Skip trailing whitespace */
    while (*p && isspace((unsigned char)*p))
        p++;

    /* Reject trailing content after closing brace */
    if (*p != '\0')
        return false;

    if (!found_url)
        return false;

    /* format is optional; default to video. */
    if (out_payload->format[0] == '\0') {
        out_payload->format[0] = 'v';
        out_payload->format[1] = 'i';
        out_payload->format[2] = 'd';
        out_payload->format[3] = 'e';
        out_payload->format[4] = 'o';
        out_payload->format[5] = '\0';
    }

    /* format must be "audio" or "video" */
    if (strcmp(out_payload->format, "audio") != 0 && strcmp(out_payload->format, "video") != 0) {
        return false;
    }

    return true;
}
