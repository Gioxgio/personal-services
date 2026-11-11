#include "json.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool skip_string(const char** p) {
    (*p)++; // skip opening quote
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++; // skip backslash
            if (**p == '\0')
                return false;
            if (**p == 'u') {
                // Skip \uXXXX
                (*p)++;
                for (int i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)**p))
                        return false;
                    (*p)++;
                }
            } else {
                (*p)++; // skip escaped character
            }
        } else {
            (*p)++;
        }
    }
    if (**p != '"')
        return false;
    (*p)++; // skip closing quote
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
        // number, true, false, null
        while (**p && **p != ',' && **p != '}' && **p != ']' && !isspace((unsigned char)**p)) {
            (*p)++;
        }
        return true;
    }
}

bool json_parse_request(const char* json_raw, notification_payload_t* out_payload) {
    if (!json_raw || !out_payload)
        return false;

    const char* p = json_raw;

    // Skip leading whitespace
    while (*p && isspace((unsigned char)*p))
        p++;

    // Expect '{'
    if (*p != '{')
        return false;
    p++;

    bool found_message = false;

    // Skip whitespace after '{'
    while (*p && isspace((unsigned char)*p))
        p++;

    // Empty object: no message field
    if (*p == '}')
        return false;

    while (*p) {
        // Skip whitespace
        while (*p && isspace((unsigned char)*p))
            p++;

        // Expect '"' for key
        if (*p != '"')
            return false;
        p++;

        // Read key (no escapes supported in keys)
        char key[64];
        size_t key_len = 0;
        while (*p && *p != '"') {
            if (*p == '\\')
                return false; // escape in key not supported
            if (key_len < sizeof(key) - 1) {
                key[key_len++] = *p;
            }
            p++;
        }
        if (*p != '"')
            return false; // unterminated key
        p++;
        key[key_len] = '\0';

        // Skip whitespace
        while (*p && isspace((unsigned char)*p))
            p++;

        // Expect ':'
        if (*p != ':')
            return false;
        p++;

        // Skip whitespace
        while (*p && isspace((unsigned char)*p))
            p++;

        if (strcmp(key, "message") == 0) {
            // Expect '"' for string value
            if (*p != '"')
                return false;
            p++;

            // Read and unescape value
            size_t msg_len = 0;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (*p == '\0')
                        return false;
                    char escaped = *p;
                    p++;
                    switch (escaped) {
                        case '"':
                            if (msg_len < MAX_TELEGRAM_MSG_LEN)
                                out_payload->message[msg_len++] = '"';
                            break;
                        case '\\':
                            if (msg_len < MAX_TELEGRAM_MSG_LEN)
                                out_payload->message[msg_len++] = '\\';
                            break;
                        case 'n':
                            if (msg_len < MAX_TELEGRAM_MSG_LEN)
                                out_payload->message[msg_len++] = '\n';
                            break;
                        case 'r':
                            if (msg_len < MAX_TELEGRAM_MSG_LEN)
                                out_payload->message[msg_len++] = '\r';
                            break;
                        case 't':
                            if (msg_len < MAX_TELEGRAM_MSG_LEN)
                                out_payload->message[msg_len++] = '\t';
                            break;
                        case '/':
                            if (msg_len < MAX_TELEGRAM_MSG_LEN)
                                out_payload->message[msg_len++] = '/';
                            break;
                        case 'u':
                            return false; // \uXXXX not supported
                        default:
                            return false; // invalid escape
                    }
                } else {
                    // Regular character (including UTF-8 bytes > 0x7F)
                    if (msg_len >= MAX_TELEGRAM_MSG_LEN)
                        return false;
                    out_payload->message[msg_len++] = *p++;
                }
            }
            if (*p != '"')
                return false; // unterminated string
            p++;
            out_payload->message[msg_len] = '\0';

            // Reject empty message
            if (msg_len == 0)
                return false;

            found_message = true;
        } else if (strcmp(key, "source") == 0) {
            if (*p != '"')
                return false;
            p++;
            size_t src_len = 0;
            while (*p && *p != '"') {
                char c;
                if (*p == '\\') {
                    p++;
                    if (*p == '\0')
                        return false;
                    switch (*p) {
                        case '"':
                            c = '"';
                            break;
                        case '\\':
                            c = '\\';
                            break;
                        case '/':
                            c = '/';
                            break;
                        case 'b':
                            c = '\b';
                            break;
                        case 'f':
                            c = '\f';
                            break;
                        case 'n':
                            c = '\n';
                            break;
                        case 'r':
                            c = '\r';
                            break;
                        case 't':
                            c = '\t';
                            break;
                        case 'u':
                            return false;
                        default:
                            return false;
                    }
                    p++;
                } else {
                    c = *p++;
                }
                if (src_len < MAX_NOTIFICATION_SOURCE_LEN)
                    out_payload->source[src_len++] = c;
            }
            if (*p != '"')
                return false;
            p++;
            out_payload->source[src_len] = '\0';
        } else {
            /* Skip non-message values (strings, numbers, objects, arrays, etc.) */
            if (!skip_value(&p))
                return false;
        }

        // Skip whitespace
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

    // Skip trailing whitespace
    while (*p && isspace((unsigned char)*p))
        p++;

    // Reject trailing content after closing brace
    if (*p != '\0')
        return false;

    return found_message;
}

/* ---------- small JSON value readers ---------- */

/* Read a JSON string value (`*p` points at the opening quote), unescape it
 * into `out` (capacity `cap`), and advance `*p` past the closing quote.
 * Returns true on success. \uXXXX escapes are not supported. */
static bool json_read_string(const char** p, char* out, size_t cap) {
    if (!p || !*p || **p != '"' || !out || cap == 0)
        return false;
    (*p)++; /* skip opening quote */

    size_t n = 0;
    while (**p && **p != '"') {
        char c;
        if (**p == '\\') {
            (*p)++;
            if (**p == '\0')
                return false;
            switch (**p) {
                case '"':
                    c = '"';
                    break;
                case '\\':
                    c = '\\';
                    break;
                case '/':
                    c = '/';
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'u':
                    return false; /* \uXXXX not supported */
                default:
                    return false;
            }
            (*p)++;
        } else {
            c = (char)**p;
            (*p)++;
        }
        if (n + 1 >= cap)
            return false;
        out[n++] = c;
    }
    if (**p != '"')
        return false; /* unterminated string */
    (*p)++;           /* skip closing quote */
    out[n] = '\0';
    return true;
}

/* Read a JSON number value (`*p` points at the first digit or '-') and store
 * it in `out`, advancing `*p` past the number. Returns true on success. */
static bool json_read_number(const char** p, long long* out) {
    if (!p || !*p || !out)
        return false;

    bool neg = false;
    if (**p == '-') {
        neg = true;
        (*p)++;
    }
    if (**p < '0' || **p > '9')
        return false;

    long long v = 0;
    while (**p >= '0' && **p <= '9') {
        if (v > (LLONG_MAX - (long long)(**p - '0')) / 10)
            return false;
        v = v * 10 + (long long)(**p - '0');
        (*p)++;
    }
    *out = neg ? -v : v;
    return true;
}

/*
 * `*p` points at the '{' of an object. Locate the member named `key`. On
 * success return true with `*p` set to the first character of the member's
 * value. If the key is absent or the object is malformed, return false with
 * `*p` just past the closing '}'.
 */
static bool json_object_find_value(const char** p, const char* key) {
    if (!p || !*p || **p != '{')
        return false;
    (*p)++; /* skip '{' */

    for (;;) {
        while (**p && isspace((unsigned char)**p))
            (*p)++;
        if (**p == '}') {
            (*p)++;
            return false; /* key not present */
        }
        if (**p != '"')
            return false; /* malformed object */

        char kbuf[160];
        if (!json_read_string(p, kbuf, sizeof kbuf))
            return false;

        const char* after_key = *p;
        while (*after_key && isspace((unsigned char)*after_key))
            after_key++;
        if (*after_key != ':')
            return false;
        *p = after_key + 1; /* at the value start (possibly leading whitespace) */
        while (**p && isspace((unsigned char)**p))
            (*p)++;

        if (strcmp(kbuf, key) == 0)
            return true; /* *p at the value */

        /* Not this member: skip its value and keep scanning. */
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
            return false;
        }
        return false;
    }
}

bool json_find_top_level_string(const char* json_raw, const char* key, char* out, size_t cap) {
    if (!json_raw || !key || !out || cap == 0)
        return false;

    const char* p = json_raw;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '{')
        return false;
    if (!json_object_find_value(&p, key))
        return false;
    return json_read_string(&p, out, cap);
}

bool json_find_top_level_int64(const char* json_raw, const char* key, long long* out) {
    if (!json_raw || !key || !out)
        return false;

    const char* p = json_raw;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '{')
        return false;
    if (!json_object_find_value(&p, key))
        return false;
    return json_read_number(&p, out);
}

bool json_extract_telegram_update(const char* json_raw, char* chat_id_out, size_t chat_id_cap,
                                  char* text_out, size_t text_cap) {
    if (!json_raw)
        return false;

    const char* p = json_raw;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '{')
        return false;

    /* Locate the "message" object (member order is not assumed). */
    const char* msg = p;
    if (!json_object_find_value(&msg, "message"))
        return false;
    if (*msg != '{')
        return false;

    bool got_chat = false;
    long long chat_id = 0;

    const char* q = msg;
    if (json_object_find_value(&q, "chat") && *q == '{') {
        const char* r = q;
        if (json_object_find_value(&r, "id") && json_read_number(&r, &chat_id)) {
            got_chat = true;
            if (chat_id_out && chat_id_cap > 0) {
                int n = snprintf(chat_id_out, chat_id_cap, "%lld", chat_id);
                if (n < 0 || (size_t)n >= chat_id_cap)
                    return false;
            }
        }
    }

    bool got_text = false;
    q = msg;
    if (text_out && text_cap > 0) {
        if (json_object_find_value(&q, "text") && json_read_string(&q, text_out, text_cap))
            got_text = true;
    } else {
        got_text = true;
    }

    return got_chat && got_text;
}

int json_escape_string(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0)
        return -1;

    size_t j = 0;
    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)input[i];
        const char* replacement = NULL;
        char single[2] = {(char)c, '\0'};

        switch (c) {
            case '"':
                replacement = "\\\"";
                break;
            case '\\':
                replacement = "\\\\";
                break;
            case '\n':
                replacement = "\\n";
                break;
            case '\r':
                replacement = "\\r";
                break;
            case '\t':
                replacement = "\\t";
                break;
            case '\b':
                replacement = "\\b";
                break;
            case '\f':
                replacement = "\\f";
                break;
            default:
                if (c < 0x20) {
                    int n = snprintf(output + j, output_size - j, "\\u%04x", c);
                    if (n < 0 || (size_t)n >= output_size - j)
                        return -1;
                    j += (size_t)n;
                    continue;
                }
                replacement = single;
                break;
        }

        size_t len = strlen(replacement);
        if (output_size - j < len + 1)
            return -1;
        memcpy(output + j, replacement, len);
        j += len;
    }
    output[j] = '\0';
    return (int)j;
}
