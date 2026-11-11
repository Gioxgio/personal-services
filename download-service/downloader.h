#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <stddef.h>

/*
 * Download media from `url` using yt-dlp into a dedicated temporary directory
 * under `download_dir`. `format` is "audio" or "video".
 *
 * On success writes the absolute path of the downloaded file into `out_path`
 * (capacity `out_cap`) and returns 0. On failure returns -1. The temporary
 * directory is left in place; the caller must call download_cleanup() on the
 * returned path once the file has been served.
 */
int download_media(const char* yt_dlp_path, const char* download_dir, const char* url,
                   const char* format, char* out_path, size_t out_cap);

/* Remove the temporary directory that holds the downloaded file. */
void download_cleanup(const char* file_path);

#endif
