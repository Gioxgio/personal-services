/*
 * test_recorder - the recorder's hand-off chain, offline.
 *
 * roadmap.md §3.10 names two things to watch, and both used to be checked
 * by hand:
 *
 *   1. which ffmpeg wrapper yt-dlp ends up with, and
 *   2. that the URL the probe allocated is released on every path out of
 *      recorder_start().
 *
 * Both land in the same place - the argv of the child - so both are
 * checked here, against the real recorder.c, with a fake `yt-dlp` (a
 * shell script) first on PATH. It reports the argv it was called with,
 * holds the "recording" open until the test releases it, and exits.
 * Nothing leaves the machine: no socket, no yt-dlp, no ffmpeg.
 *
 * What the checks prove, and what they cannot:
 *
 *   - The stream URL reaches the child, and ch->url never does: the
 *     "probe creates -> recorder frees" hand-off (spec.md §6.3) is a real
 *     hand-off, not a pointer that gets lost between two threads.
 *   - The wrapper yt-dlp is told about is the one recorder_init()
 *     resolved, in all three documented outcomes - SS_WRAPPER_DIR,
 *     the directory beside /proc/self/exe, or nothing at all - which is
 *     what the build/ copy step of §3.7 is protecting.
 *   - recorder_start() refuses a channel it was never given and a channel
 *     that is already busy, and releases the URL on both refusals. A
 *     release is invisible from C - free() returns nothing - so it is
 *     proven by running this binary under `make debug`: LeakSanitizer
 *     fails the run if a refusal ever kept its URL.
 *   - The ownership rule holds across the thread boundary. That half is
 *     invisible to ASan and was wrong until TSan said so (§3.10, defect
 *     #15); `make tsan` builds this same binary to keep it that way.
 *
 * Run from the project root (make test does).
 */

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "recorder.h"
#include "urlparse.h"
#include "util.h"

/* The URL a probe would have handed over, and the config line it came from. */
#define STREAM_URL "https://www.youtube.com/watch?v=LIVErecorder01"

/* PATH_MAX, plus the suffixes these buffers get appended ("/ffmpeg-quiet",
 * "/<slug>.log"). Paths are heap strings rather than fixed arrays, so the
 * compiler cannot bound them and -Wformat-truncation stays quiet without
 * every one of them being oversized. */
#define TEST_PATH (PATH_MAX + 128)
#define CHAN_LINE "youtube: @recordtest"

/* One channel, parsed once; the slug decides the log path we read back. */
static channel_t g_ch;

/* Everything the fake and the checks need, under one temp root:
 *
 *   <root>/bin/yt-dlp        the fake, first on PATH
 *   <root>/out/              the recordings root handed to recorder_init()
 *   <root>/wrap/             holds ffmpeg-quiet: the env override wins
 *   <root>/empty/            holds nothing: resolution falls through
 *   <root>/naked/            holds only this binary: nothing to resolve
 *   <root>/argv              the fake's argv, one line per argument
 *   <root>/started           touched the moment the fake runs
 *   <root>/release           touched by the test to let the fake exit
 */
static char *g_root;

static char *in_root(const char *name)
{
    char *p = xmalloc(TEST_PATH);
    snprintf(p, TEST_PATH, "%s/%s", g_root, name);
    return p;
}

static bool write_file(const char *path, const char *body, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    bool ok = fputs(body, f) >= 0;
    if (fclose(f) != 0)
        ok = false;
    return ok && chmod(path, mode) == 0;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 256 + 1 > cap) {
            cap = cap ? cap * 2 : 1024;
            char *p = realloc(buf, cap);
            if (!p) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = p;
        }
        size_t n = fread(buf + len, 1, 256, f);
        len += n;
        if (n < 256) {
            if (ferror(f)) {
                free(buf);
                fclose(f);
                return NULL;
            }
            break;
        }
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static bool wait_for(const char *path)
{
    for (int i = 0; i < 400; i++) {      /* ~4 s, 10 ms apart */
        if (access(path, F_OK) == 0)
            return true;
        struct timespec nap = { 0, 10000000L };
        nanosleep(&nap, NULL);
    }
    return false;
}

static void rm_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *p = xmalloc(TEST_PATH);
        snprintf(p, TEST_PATH, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
            rm_tree(p);
        else
            unlink(p);
        free(p);
    }
    closedir(d);
    rmdir(dir);
}

static char *copy_file(const char *from, const char *to, mode_t mode)
{
    FILE *in = fopen(from, "rb");
    if (!in)
        return NULL;
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return NULL;
    }
    char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        ok = ok && fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    if (!ok || chmod(to, mode) != 0)
        return NULL;
    return xstrdup(to);
}

/* The fake yt-dlp. It has to be a real executable: record_thread() calls
 * execvp("yt-dlp"), so this is found through PATH like the real one.
 *
 * The `sleep 300 &` is the point of it: the real yt-dlp runs ffmpeg as a
 * child of its own, so a recording is a process tree rather than a pid.
 * The fake publishes that grandchild's pid and the test checks it is gone
 * once the recording is over - the orphan that motivated it kept writing
 * into the .part long after stream-sniper had exited. */
static const char FAKE_YTDLP[] =
    "#!/bin/sh\n"
    "# Stand-in for yt-dlp: report how we were called, hold the recording\n"
    "# open until the test says so, then exit with the configured status.\n"
    "echo 'fake yt-dlp: stdout line'\n"
    "echo 'fake yt-dlp: stderr line' >&2\n"
    "sleep 300 &\n"
    "echo $! > \"$SS_TEST_ROOT/grandchild.tmp\"\n"
    "mv \"$SS_TEST_ROOT/grandchild.tmp\" \"$SS_TEST_ROOT/grandchild\"\n"
    ": > \"$SS_TEST_ROOT/started\"\n"
    "while [ ! -e \"$SS_TEST_ROOT/release\" ]; do sleep 0.02; done\n"
    "printf '%s\\n' \"$@\" > \"$SS_TEST_ROOT/argv.tmp\"\n"
    "mv \"$SS_TEST_ROOT/argv.tmp\" \"$SS_TEST_ROOT/argv\"\n"
    "exit 0\n";

/* The pid the fake published for its grandchild, or 0 if it never ran. */
static pid_t grandchild_pid(void)
{
    char *path = in_root("grandchild");
    char *body = read_file(path);
    pid_t pid = body ? (pid_t)strtol(body, NULL, 10) : 0;
    free(body);
    free(path);
    return pid;
}

/*
 * The state /proc reports for a pid, or 0 when there is no such process.
 *
 * 'Z' means dead but not yet reaped, which for our purposes counts as
 * gone: a killed orphan stays visible until whoever adopted it reaps it,
 * and how long that takes is not ours to decide.
 */
static char pid_state(pid_t pid)
{
    char path[64];
    char buf[256];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return '\0';                      /* no such process */

    char state = '\0';
    if (fgets(buf, sizeof buf, f)) {
        /* "<pid> (<comm>) <state> ..." and comm may itself contain spaces
         * and parentheses, so the state is the field after the last ')'. */
        char *p = strrchr(buf, ')');
        if (p && p[1] == ' ')
            state = p[2];
    }
    fclose(f);
    return state;
}

/*
 * Insist the grandchild is gone once the recording that owned it is over,
 * naming the phase so a failure says which one it was. ~2 s of polling:
 * the signals that remove it are asynchronous, and a process that has to
 * be reaped by its new parent is not ours to hurry.
 */
static void check_grandchild_gone(const char *phase)
{
    pid_t gc = grandchild_pid();
    CHECK(gc > 0);                         /* the fake really spawned one */
    if (gc <= 0)
        return;

    char state = '\0';
    for (int i = 0; i < 200; i++) {
        state = pid_state(gc);
        if (state == '\0' || state == 'Z')
            break;
        struct timespec nap = { 0, 10000000L };
        nanosleep(&nap, NULL);
    }

    bool gone = (state == '\0' || state == 'Z');
    if (!gone) {
        fprintf(stderr, "      pid %ld is in state '%c' after %s: %s\n",
                (long)gc, state, phase, "still running");
        (void)kill(gc, SIGKILL);           /* never leave it behind us */
    }
    CHECK(gone);
}

/*
 * Record g_ch once, holding the child until its argv has been read, and
 * return that argv (NULL if the child never reported one).
 *
 * The refusal checks live here because they are only meaningful while a
 * recording is actually in flight: a busy channel and a channel the
 * recorder was never given are the two ways recorder_start() can say no.
 * Under `make debug` neither may leak its URL.
 */
static char *record_once(bool expect_wrapper)
{
    char *started  = in_root("started");
    char *release  = in_root("release");
    char *argvfile = in_root("argv");
    char *grand    = in_root("grandchild");

    unlink(started);
    unlink(release);
    unlink(argvfile);
    unlink(grand);                        /* never read a stale pid */

    CHECK(recorder_start(&g_ch, xstrdup(STREAM_URL)));
    CHECK(wait_for(started));            /* the child is forked by now */
    CHECK(recorder_is_busy(&g_ch));

    channel_t foreign;
    CHECK(channel_parse("youtube: @elsewhere", &foreign));
    CHECK(!recorder_start(&foreign, xstrdup(STREAM_URL)));
    CHECK(!recorder_start(&g_ch, xstrdup(STREAM_URL)));
    channel_free(&foreign);

    /* Release the child and wait for it to say what it was called with
     * (the fake renames its argv into place, so the file that appears is
     * complete) before shutting down, which would otherwise SIGTERM it. */
    CHECK(write_file(release, "", 0644));
    CHECK(wait_for(argvfile));
    recorder_shutdown();                 /* joins the recording thread */
    CHECK(!recorder_is_busy(&g_ch));

    /*
     * The child is gone; its grandchild must be too. This is the whole
     * recording process tree, and the grandchild is what a real yt-dlp's
     * ffmpeg would be - the process actually writing the file. Shutdown
     * returns only once the thread has been joined, so by now it has had
     * every chance to clean the group up and had no parent left to do it.
     */
    check_grandchild_gone("the child exited on its own");

    char *argv = read_file(argvfile);

    if (argv) {
        /* The recording target is the probe's URL, never the channel
         * page: yt-dlp handed the latter would record whatever the
         * channel happens to be showing, not this broadcast. */
        CHECK(strstr(argv, "\n" STREAM_URL "\n") != NULL);
        CHECK(strstr(argv, g_ch.url) == NULL);
        CHECK(expect_wrapper == (strstr(argv, "--ffmpeg-location") != NULL));
    }

    free(started);
    free(release);
    free(argvfile);
    free(grand);
    return argv;
}

/* Check that argv carries "--ffmpeg-location" followed by `path`. */
static void check_wrapper(const char *argv, const char *path)
{
    char *want = xmalloc(TEST_PATH);
    snprintf(want, TEST_PATH, "--ffmpeg-location\n%s\n", path);
    CHECK(strstr(argv, want) != NULL);
    free(want);
}

/* This binary's own path, and the directory holding it: the two halves of
 * what resolve_ffmpeg_wrapper() reads out of /proc/self/exe. */
static char *self_exe(void)
{
    char buf[TEST_PATH];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0 || (size_t)n >= sizeof buf)
        return NULL;
    buf[n] = '\0';
    return xstrdup(buf);
}

static char *self_dir(const char *exe)
{
    const char *slash = exe ? strrchr(exe, '/') : NULL;
    if (!slash)
        return NULL;
    size_t len = (size_t)(slash - exe);
    char *dir = xmalloc(len + 1);
    memcpy(dir, exe, len);
    dir[len] = '\0';
    return dir;
}

/*
 * Outcome 3 of the resolution order: nothing to resolve, so no
 * --ffmpeg-location at all - and still a working recording.
 *
 * Unreachable from the directory `make test` runs in, because build/
 * holds the wrappers. So the test re-executes itself from <root>/naked,
 * a directory holding this binary and nothing else. That also pins the
 * fallback to /proc/self/exe rather than to the working directory: the
 * same code, same cwd, different answer, because the binary moved.
 */
static int phase_wrapperless(void)
{
    char *out   = in_root("out");
    char *empty = in_root("empty");

    setenv("SS_WRAPPER_DIR", empty, 1);
    CHECK(recorder_init(&g_ch, 1, out));

    char *argv = record_once(false);
    CHECK(argv != NULL);
    free(argv);

    recorder_shutdown();
    free(out);
    free(empty);
    return harness_exit("test_recorder[wrapperless]");
}

int main(void)
{
    /*
     * The recorder logs every fork and every DONE at INFO, which is
     * evidence on a real run and noise in a test log. WARN still reaches
     * the terminal, so a child that had to be killed stays visible.
     */
    log_set_level(LOG_WARN);

    /* The re-executed copy reports its own failures through its exit code. */
    const char *reexec = getenv("SS_TEST_REEXEC");
    const char *root = getenv("SS_TEST_ROOT");
    if (reexec && root) {
        g_root = xstrdup(root);
        CHECK(channel_parse(CHAN_LINE, &g_ch));
        int rc = phase_wrapperless();
        free(g_root);
        return rc;
    }

    /* --- the temp tree and the fake yt-dlp --- */

    char tmpl[] = "/tmp/ss-test-recorder-XXXXXX";
    char *made = mkdtemp(tmpl);
    if (!made) {
        fprintf(stderr, "test_recorder: cannot create a temp dir: %s\n",
                strerror(errno));
        return 1;
    }
    g_root = xstrdup(made);

    char *bin    = in_root("bin");
    char *out    = in_root("out");
    char *wrap   = in_root("wrap");
    char *empty  = in_root("empty");
    char *naked  = in_root("naked");
    char *fake   = in_root("bin/yt-dlp");
    char *wrapper = in_root("wrap/ffmpeg-quiet");
    CHECK(mkdir(bin, 0755)   == 0);
    CHECK(mkdir(out, 0755)   == 0);
    CHECK(mkdir(wrap, 0755)  == 0);
    CHECK(mkdir(empty, 0755) == 0);
    CHECK(mkdir(naked, 0755) == 0);
    CHECK(write_file(fake, FAKE_YTDLP, 0755));

    /* execvp() searches PATH, so the fake has to come first. */
    const char *old_path = getenv("PATH");
    char *path = xmalloc(TEST_PATH);
    snprintf(path, TEST_PATH, "%s:%s", bin, old_path ? old_path : "/usr/bin:/bin");
    setenv("PATH", path, 1);
    setenv("SS_TEST_ROOT", g_root, 1);

    CHECK(channel_parse(CHAN_LINE, &g_ch));

    /* --- outcome 1: SS_WRAPPER_DIR wins --- */

    CHECK(write_file(wrapper, "#!/bin/sh\nexit 0\n", 0755));
    setenv("SS_WRAPPER_DIR", wrap, 1);
    CHECK(recorder_init(&g_ch, 1, out));

    char *child_argv = record_once(true);
    CHECK(child_argv != NULL);
    if (child_argv) {
        /* What -v prints at startup is what yt-dlp is handed. */
        check_wrapper(child_argv, wrapper);

        /* Both streams went to the channel log, not to this terminal. */
        char log[TEST_PATH];
        snprintf(log, sizeof log, "%s/%s.log", out, g_ch.slug);
        char *logbody = read_file(log);
        CHECK(logbody != NULL);
        if (logbody) {
            CHECK(strstr(logbody, "fake yt-dlp: stdout line") != NULL);
            CHECK(strstr(logbody, "fake yt-dlp: stderr line") != NULL);
            free(logbody);
        }
        free(child_argv);
    }

    /* --- outcome 2: an override that holds nothing falls through --- */

    setenv("SS_WRAPPER_DIR", empty, 1);
    CHECK(recorder_init(&g_ch, 1, out));

    child_argv = record_once(true);
    CHECK(child_argv != NULL);
    if (child_argv) {
        char *exe = self_exe();
        CHECK(exe != NULL);
        char *dir = self_dir(exe);
        CHECK(dir != NULL);
        if (dir) {
            char beside[TEST_PATH];
            snprintf(beside, sizeof beside, "%s/ffmpeg-quiet", dir);
            if (!access(beside, X_OK))
                check_wrapper(child_argv, beside);   /* `make test` copies it */
            else
                CHECK(strstr(child_argv, "--ffmpeg-location") == NULL);
        }
        free(dir);
        free(exe);
        free(child_argv);
    }

    /* --- shutdown stops a recording that is still running --- */

    char *started = in_root("started");
    char *release = in_root("release");
    char *grand   = in_root("grandchild");
    unlink(started);
    unlink(release);
    unlink(grand);
    CHECK(recorder_init(&g_ch, 1, out));
    CHECK(recorder_start(&g_ch, xstrdup(STREAM_URL)));
    CHECK(wait_for(started));
    /* No release file: the child is still held, so shutdown has to
     * SIGTERM it and wait for the thread before it returns. */
    recorder_shutdown();
    CHECK(!recorder_is_busy(&g_ch));

    /*
     * The same tree, stopped rather than ended: the child cannot clean up
     * after itself - it is killed - so the SIGTERM has to reach the whole
     * group, grandchild included. This is the Ctrl-C path.
     */
    check_grandchild_gone("shutdown stopped it");

    /* --- init with no recording at all: the dry-run path --- */

    CHECK(recorder_init(&g_ch, 1, out));
    recorder_shutdown();                 /* no child, no thread, no slots kept */

    /* --- a channel the recorder was never given --- */

    channel_t stranger;
    CHECK(channel_parse("twitch: nobody", &stranger));
    CHECK(!recorder_start(&stranger, xstrdup(STREAM_URL)));
    CHECK(!recorder_is_busy(&stranger));
    channel_free(&stranger);

    /* --- outcome 3, from a directory with no wrapper beside the binary --- */

    char *self = self_exe();
    CHECK(self != NULL);
    if (self) {
        char *to = in_root("naked/test_recorder");
        char *child = copy_file(self, to, 0755);
        CHECK(child != NULL);
        if (child) {
            setenv("SS_TEST_REEXEC", "1", 1);
            setenv("SS_TEST_ROOT", g_root, 1);
            setenv("SS_WRAPPER_DIR", empty, 1);

            pid_t pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                execl(child, child, (char *)NULL);
                _exit(127);
            }
            int status = 0;
            if (pid > 0)
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                    ;
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            unsetenv("SS_TEST_REEXEC");
            free(child);
        }
        free(to);
    }
    free(self);

    /* --- tear down --- */

    recorder_shutdown();
    channel_free(&g_ch);
    free(bin);
    free(out);
    free(wrap);
    free(empty);
    free(naked);
    free(fake);
    free(wrapper);
    free(started);
    free(release);
    free(grand);
    free(path);
    rm_tree(g_root);
    free(g_root);

    return harness_exit("test_recorder");
}