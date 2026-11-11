/*
 * jsonlite.h - minimal string extraction from JSON-ish bodies.
 *
 * Not a parser. The two bodies the program actually reads (the
 * Twitch token-mint response and the YouTube /streams page)
 * carry the values it needs as "key":"value" pairs inside
 * otherwise unknown JSON, so it scans for a quoted key followed
 * by a quoted value and copies the value out. A key that appears
 * inside a longer key ("my_access_token" for key "access_token")
 * does not count: the byte before it must be the key's own
 * opening quote.
 */

#ifndef JSONLITE_H
#define JSONLITE_H

#include <stddef.h>

/*
 * Find the first "key":"value" in json and copy value into out,
 * NUL-terminated, stopping at the closing quote or after outsz-1
 * bytes. Returns the number of characters copied, so a caller
 * sees 0 for both a missing key and an empty value - the only
 * outcomes in which there is nothing to use.
 */
size_t json_find_string(const char *json, const char *key,
                        char *out, size_t outsz);

/*
 * Scan json for every "key":"value" in document order and collect
 * up to max distinct, non-empty values into out (each row holds
 * the value, truncated like json_find_string truncates). Empty
 * values and repeats of an already-collected value are skipped.
 * Returns the number of values collected; rows past that count
 * are untouched and must not be read.
 */
size_t json_collect_strings(const char *json, const char *key,
                            char out[][32], size_t max);

#endif /* JSONLITE_H */
