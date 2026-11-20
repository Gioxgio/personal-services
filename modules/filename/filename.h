#ifndef FILENAME_H
#define FILENAME_H

/*
 * Sanitize a string for use as a filesystem name: every character that is not
 * alphanumeric, '-' or '_' becomes '_'; an empty string becomes "_". This is
 * the rule stream-sniper uses for channel slugs.
 *
 * Returns a malloc'd string (caller frees), or NULL on allocation failure.
 */
char* sanitize_filename(const char* s);

/*
 * Same rule, but keeps the final extension (the last '.' that is not the first
 * character of the base name) intact: "My Video: Title?.mp4" becomes
 * "My_Video__Title_.mp4". Returns a malloc'd string, or NULL on failure.
 */
char* sanitize_filename_keep_extension(const char* s);

#endif /* FILENAME_H */
