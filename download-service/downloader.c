#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "downloader.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define YTDLP_ARGS_MAX 32

/* Create every component of `path` that does not yet exist. */
static bool mkdir_p(const char* path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return false;
    memcpy(tmp, path, len + 1);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return false;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

/*
 * Build a unique temporary directory under `download_dir` using pid + counter
 * (pure POSIX, avoids mkdtemp feature-macro requirements). Returns true on
 * success with the directory path in `out`.
 */
static bool make_temp_dir(const char* download_dir, char* out, size_t cap) {
    pid_t pid = getpid();
    for (int i = 0; i < 1000; i++) {
        int n = snprintf(out, cap, "%s/download.%ld.%d", download_dir, (long)pid, i);
        if (n < 0 || (size_t)n >= cap)
            return false;
        if (mkdir(out, 0700) == 0)
            return true;
        if (errno != EEXIST)
            return false;
    }
    return false;
}

/* Copy the directory part of `path` into `out` (capacity `cap`). */
static void dirname_of(const char* path, char* out, size_t cap) {
    const char* slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path) : 0;
    if (len == 0) {
        if (cap > 0) {
            out[0] = '.';
            out[1] = '\0';
        }
        return;
    }
    if (len >= cap)
        len = cap - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

/*
 * Scan `dir` for the first regular, non-hidden file and copy its full path
 * into `out` (capacity `cap`). Returns true on success.
 */
static bool find_downloaded_file(const char* dir, char* out, size_t cap) {
    DIR* d = opendir(dir);
    if (!d)
        return false;

    struct dirent* e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char full[PATH_MAX];
        int n = snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof full)
            continue;
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;
        if ((size_t)n >= cap) {
            found = false;
            goto out;
        }
        memcpy(out, full, (size_t)n + 1);
        found = true;
        break;
    }

out:
    closedir(d);
    return found;
}

/* Recursively remove `path` (a file or a directory tree). */
static void remove_tree(const char* path) {
    DIR* d = opendir(path);
    if (!d) {
        (void)remove(path);
        return;
    }

    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (n < 0 || (size_t)n >= sizeof child)
            continue;
        struct stat st;
        if (stat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            remove_tree(child);
        else
            (void)remove(child);
    }
    closedir(d);
    (void)remove(path);
}

void download_cleanup(const char* file_path) {
    char dir[PATH_MAX];
    dirname_of(file_path, dir, sizeof dir);
    remove_tree(dir);
}

int download_media(const char* yt_dlp_path, const char* download_dir, const char* url,
                   const char* format, char* out_path, size_t out_cap) {
    if (!yt_dlp_path || !download_dir || !url || !format || !out_path || out_cap == 0) {
        return -1;
    }

    if (!mkdir_p(download_dir))
        return -1;

    char temp_dir[PATH_MAX];
    if (!make_temp_dir(download_dir, temp_dir, sizeof temp_dir))
        return -1;

    /* Build the yt-dlp argument vector. */
    char* argv[YTDLP_ARGS_MAX];
    int ai = 0;
    argv[ai++] = (char*)yt_dlp_path;
    argv[ai++] = (char*)"--newline";
    argv[ai++] = (char*)"--no-playlist"; /* never fan out to a playlist */
    argv[ai++] = (char*)"--retries";
    argv[ai++] = (char*)"3";
    if (strcmp(format, "audio") == 0) {
        argv[ai++] = (char*)"-x"; /* audio only */
    }
    char output[PATH_MAX];
    int n = snprintf(output, sizeof output, "%s/%%(id)s.%%(ext)s", temp_dir);
    if (n < 0 || (size_t)n >= sizeof output) {
        remove_tree(temp_dir);
        return -1;
    }
    argv[ai++] = (char*)"-o";
    argv[ai++] = output;
    argv[ai++] = (char*)url;
    argv[ai] = NULL;

    /* Fork and exec yt-dlp. stdout/stderr go to /dev/null to keep the temp
     * directory containing only the media file. */
    pid_t pid = fork();
    if (pid < 0) {
        remove_tree(temp_dir);
        return -1;
    }
    if (pid == 0) {
        /* Child: async-signal-safe calls only. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execvp(yt_dlp_path, argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) {
        remove_tree(temp_dir);
        return -1;
    }

    if (!find_downloaded_file(temp_dir, out_path, out_cap)) {
        remove_tree(temp_dir);
        return -1;
    }
    return 0;
}
