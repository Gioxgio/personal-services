#include "recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "notify.h"
#include "ss.h"
#include "util.h"

static pthread_mutex_t g_state_mtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * Per-channel runtime state, indexed exactly like the channel array
 * given to recorder_init(). Private to this module: channel_t itself
 * carries only what config.c and probe.c own.
 *
 * stream_url is the recording's target, handed over by
 * recorder_start() and released by the thread that owns it: the
 * slot is the bridge between the poll thread that got the URL from
 * the probe and the recorder thread that needs it, and it keeps the
 * "probe creates -> recorder frees" chain in one place.
 */
typedef struct {
    pid_t       pid;         /* yt-dlp child process, 0 when idle     */
    pthread_t   thread;
    atomic_bool recording;   /* a recorder thread is attached         */
    bool        has_thread;  /* `thread` above is joinable            */
    bool        asked_stop;  /* shutdown signalled this child         */
    bool        tried_live;  /* already attempted this live event     */
    char       *stream_url;  /* target of the recording, owned while
                                 `recording` is set                    */
} rec_slot_t;

static rec_slot_t      *g_slots;    /* n slots, from recorder_init()  */
static size_t           g_nslots;
static const channel_t *g_chans;    /* read-only, owned by config.c   */
static const char      *g_out_dir;  /* read-only, owned by main.c     */

/* Largest output template suffix we ever append to a directory. */
#define TMPL_SUFFIX_MAX 64

/* ffmpeg wrapper shipped next to this binary, see ffmpeg-quiet. */
static const char FFMPEG_WRAPPER[] = "ffmpeg-quiet";

/*
 * The ffmpeg wrapper yt-dlp is handed, or "" when none is installed.
 * Written once by recorder_init(), before any recorder thread exists, and
 * read-only afterwards, so the lookup needs no lock.
 */
static char g_ffmpeg_wrapper[PATH_MAX];

/*
 * Point g_ffmpeg_wrapper at dir/ffmpeg-quiet, leaving it empty when there is
 * nothing executable there or the path would not fit.
 */
static void wrapper_in(const char *dir)
{
    int n = snprintf(g_ffmpeg_wrapper, sizeof g_ffmpeg_wrapper,
                     "%s/%s", dir, FFMPEG_WRAPPER);
    if (n <= 0 || (size_t)n >= sizeof g_ffmpeg_wrapper ||
        access(g_ffmpeg_wrapper, X_OK) != 0)
        g_ffmpeg_wrapper[0] = '\0';
}

/*
 * Locate the ffmpeg wrapper shipped with this program, so yt-dlp can be told
 * to spawn ffmpeg with -nostats instead of its twice-a-second
 * frame=/fps=/time= stats line.
 *
 * Resolution order: SS_WRAPPER_DIR (an install prefix's libexec/,
 * say) -> next to /proc/self/exe -> none. The env override exists because
 * `make install` puts the wrappers in libexec/ while an uninstalled build
 * keeps them beside the binary; ffmpeg-quiet already honours
 * SS_FFMPEG, so the pattern is established.
 *
 * Resolving here rather than on the first recording is what makes the answer
 * checkable: -v names the file yt-dlp will be handed, at startup, so the
 * build/ copy step (spec.md §6.1) is verified instead of assumed. A missing
 * wrapper costs readability of the per-channel log, never of the terminal,
 * so it is never fatal - and either answer is logged.
 */
static void resolve_ffmpeg_wrapper(void)
{
    const char *env = getenv("SS_WRAPPER_DIR");
    if (env && *env)
        wrapper_in(env);

    if (!g_ffmpeg_wrapper[0]) {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0 && (size_t)n < sizeof exe) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                wrapper_in(exe);
            }
        }
    }

    if (g_ffmpeg_wrapper[0])
        logmsg(LOG_DEBUG, "ffmpeg wrapper: %s", g_ffmpeg_wrapper);
    else
        logmsg(LOG_DEBUG, "ffmpeg-quiet not found (SS_WRAPPER_DIR, or "
                          "next to the binary); ffmpeg output in the logs will "
                          "include its stats line");
}

/*
 * Map a channel to its slot. Returns NULL for anything that is not
 * part of the array recorder_init() was given.
 */
static rec_slot_t *slot_of(const channel_t *ch)
{
    if (!g_slots || !g_chans)
        return NULL;
    size_t i = (size_t)(ch - g_chans);
    if (i >= g_nslots)
        return NULL;
    return &g_slots[i];
}

/*
 * Bind the recorder to the channels main is about to sweep and
 * remember where recordings land. Called once, after config_load()
 * and before any other recorder function. Returns false if the slot
 * array cannot be allocated.
 */
bool recorder_init(const channel_t *chans, size_t n, const char *out_dir)
{
    g_chans   = chans;
    g_out_dir = out_dir;
    g_nslots  = n;
    /* calloc, not malloc: pid / thread / has_thread must start
     * zeroed, or shutdown would act on whatever the heap held. */
    g_slots   = n ? calloc(n, sizeof *g_slots) : NULL;
    if (n && !g_slots) {
        logmsg(LOG_ERROR, "recorder: cannot allocate %zu slot(s)", n);
        g_nslots = 0;
        return false;
    }
    for (size_t i = 0; i < n; i++)
        atomic_init(&g_slots[i].recording, false);

    /* Single-threaded context: the wrapper is settled before the poll loop
     * starts, let alone before a recorder thread could read it. */
    resolve_ffmpeg_wrapper();
    return true;
}

/*
 * Signal a recording's whole process group.
 *
 * Every recording leads its own group (see record_thread), because the pid
 * in a slot is not the whole job: yt-dlp runs ffmpeg as a child of its own,
 * and that grandchild is the process actually writing the file. Signalling
 * the pid alone reaps yt-dlp and leaves ffmpeg running, still appending to
 * the .part, with nothing left that will ever finalise or resume it.
 *
 * Never falls back to kill(pid, ...) - a pid here is either alive or, on the
 * cleanup path, already reaped, and a recycled pid must not be signalled.
 */
static void signal_group(pid_t pgid, int sig)
{
    if (pgid > 0)
        (void)kill(-pgid, sig);       /* ESRCH: nothing left in the group */
}

/*
 * Ask a live recording to stop: the group, so ffmpeg is asked too, falling
 * back to the single pid for a child that never became a group leader.
 */
static void signal_recording(pid_t pid, int sig)
{
    if (kill(-pid, sig) != 0)
        (void)kill(pid, sig);
}

/*
 * Build the on-disk destination directory for a channel. Returns false if the
 * result would be truncated, in which case the caller must not record.
 */
static bool channel_dir(const channel_t *ch, char *buf, size_t bufsz)
{
    int n = snprintf(buf, bufsz, "%s/%s", g_out_dir, ch->slug);
    if (n < 0 || (size_t)n >= bufsz) {
        logmsg(LOG_ERROR, "%s: output path too long, skipping", ch->slug);
        return false;
    }
    return true;
}

/*
 * Build the path of the file that captures this channel's yt-dlp and ffmpeg
 * output: <out_dir>/<slug>.log. Appended to, never truncated, so a channel
 * that is re-recorded keeps the log of the attempt that failed.
 */
static bool channel_logpath(const channel_t *ch, char *buf, size_t bufsz)
{
    int n = snprintf(buf, bufsz, "%s/%s.log", g_out_dir, ch->slug);
    return n >= 0 && (size_t)n < bufsz;
}

/*
 * Recording thread body.
 *
 * Forks yt-dlp and waits for it. yt-dlp exits on its own when the broadcast
 * ends, so this thread's lifetime == the stream's lifetime. When the child is
 * reaped we clear the busy flag and exit.
 */
static void *record_thread(void *arg)
{
    /*
     * The thread owns exactly one slot; the channel it records sits
     * at the same index in the array recorder_init() was given, and
     * the stream to record is the URL the slot carries.
     */
    rec_slot_t *slot = arg;
    const channel_t *ch = &g_chans[(size_t)(slot - g_slots)];

    char dir[PATH_MAX];
    if (!channel_dir(ch, dir, sizeof dir)) {
        free(slot->stream_url);
        slot->stream_url = NULL;
        atomic_store_explicit(&slot->recording, false, memory_order_release);
        return NULL;
    }
    mkdir_p(dir);

    char tmpl[PATH_MAX + TMPL_SUFFIX_MAX];
    int tn = snprintf(tmpl, sizeof tmpl,
                      "%s/%%(upload_date)s_%%(time)s_%%(title).80s.%%(ext)s", dir);
    if (tn < 0 || (size_t)tn >= sizeof tmpl) {
        logmsg(LOG_ERROR, "%s: output template too long, skipping", ch->slug);
        free(slot->stream_url);
        slot->stream_url = NULL;
        atomic_store_explicit(&slot->recording, false, memory_order_release);
        return NULL;
    }

    /* Settled once in recorder_init(); NULL simply leaves ffmpeg at its
     * default verbosity, which is only ever visible in the channel log. */
    const char *wrapper = g_ffmpeg_wrapper[0] ? g_ffmpeg_wrapper : NULL;

    char *argv[16];
    int  ai = 0;

    argv[ai++] = (char *)"yt-dlp";
    /* Output goes to a file, not a tty: one progress line per update keeps
     * the log greppable instead of collapsing into a single \r-carried line. */
    argv[ai++] = (char *)"--newline";
    argv[ai++] = (char *)"--continue";
    argv[ai++] = (char *)"--no-playlist";           /* never fan out to a playlist */
    argv[ai++] = (char *)"--retries";              argv[ai++] = (char *)"5";
    argv[ai++] = (char *)"--fragment-retries";     argv[ai++] = (char *)"20";
    argv[ai++] = (char *)"--concurrent-fragments"; argv[ai++] = (char *)"4";
    if (wrapper) {
        argv[ai++] = (char *)"--ffmpeg-location";
        argv[ai++] = (char *)wrapper;
    }
    argv[ai++] = (char *)"-o";                      argv[ai++] = tmpl;
    /*
     * The target is the URL in the slot, never ch->url: the channel page
     * is what the *probe* fetched, and handing it to yt-dlp records
     * whatever that page happens to point at. The slot cannot be empty
     * here - recorder_start() fills it before creating this thread, and
     * nothing clears it until the free() below - so there is no
     * "fall back to something plausible" branch to hide a broken
     * hand-off behind.
     */
    argv[ai++] = slot->stream_url;
    argv[ai] = NULL;

    /*
     * Capture the child's stdout and stderr in the per-channel log.
     *
     * This is what keeps the terminal readable: yt-dlp inherits ffmpeg's
     * stderr instead of piping it (yt_dlp/downloader/external.py opens
     * ffmpeg without a stderr=PIPE), so both ffmpeg's stats line and
     * yt-dlp's own progress lines end up wherever yt-dlp's fds point. With
     * two channels live those two streams would otherwise interleave.
     */
    char logpath[PATH_MAX];
    logpath[0] = '\0';
    int  logfd = -1;
    if (channel_logpath(ch, logpath, sizeof logpath)) {
        logfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (logfd < 0)
            logmsg(LOG_WARN, "%s: cannot open %s: %s",
                   ch->slug, logpath, strerror(errno));
    }

    /* Stamped before the fork so DONE/FAIL covers the whole run. */
    time_t started = time(NULL);

    /*
     * Everything the child needs is prepared *before* the fork.
     *
     * Between fork() and execvp() the child may only call async-signal-safe
     * functions. fprintf() is not one of them: if another thread held the
     * stdio lock when we forked, the child inherits it held and deadlocks
     * inside the first write. open(), dup2() and close() are safe, but doing
     * them here keeps the child's path down to dup2 / write / execvp.
     *
     * The descriptors are pre-duped onto numbers >= 3 so the child's dup2()
     * onto 0, 1 and 2 can never clobber one of its own sources. F_DUPFD_CLOEXEC
     * means execvp() tidies them up if it ever succeeds.
     */
    int child_in = -1, child_out = -1, child_err = -1;
    int have_null = 0;
    if (logfd >= 0) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) {
            have_null = 1;
            child_in = fcntl(nul, F_DUPFD_CLOEXEC, 3);
            close(nul);
        }
        child_out = fcntl(logfd, F_DUPFD_CLOEXEC, 3);
        child_err = fcntl(logfd, F_DUPFD_CLOEXEC, 3);

        if ((have_null && child_in < 0) || child_out < 0 || child_err < 0) {
            logmsg(LOG_WARN, "%s: cannot prepare log descriptors: %s",
                   ch->slug, strerror(errno));
            if (child_in  >= 0) close(child_in);
            if (child_out >= 0) close(child_out);
            if (child_err >= 0) close(child_err);
            child_in = child_out = child_err = -1;
            have_null = 0;
        }
    }

    /* Pre-formatted, because the child cannot call strerror() either.
     * ENOENT is what execvp() realistically reports here: yt-dlp not on PATH. */
    char exec_err[256];
    int  exec_err_len = snprintf(exec_err, sizeof exec_err,
                                 "stream-sniper: execvp(\"yt-dlp\") failed: %s\n",
                                 strerror(ENOENT));

    /* Fork while holding the lock so shutdown can never miss the pid. */
    pthread_mutex_lock(&g_state_mtx);
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: async-signal-safe calls only, no stdio, nothing allocated. */
        /*
         * Lead our own process group, before anything else and while
         * still async-signal-safe. setpgid(0, 0) is a syscall, so the
         * grandchild ffmpeg inherits the group id, and both the shutdown
         * signal and the cleanup below can reach it. Without it the
         * recording is the yt-dlp pid plus a process nobody tracks, and
         * killing the pid leaves ffmpeg writing into the .part forever.
         */
        (void)setpgid(0, 0);
        if (have_null)
            (void)dup2(child_in, STDIN_FILENO);   /* no tty to steal */
        if (child_out >= 0)
            (void)dup2(child_out, STDOUT_FILENO);
        if (child_err >= 0)
            (void)dup2(child_err, STDERR_FILENO);

        execvp(argv[0], argv);
        if (exec_err_len > 0)
            (void)write(STDERR_FILENO, exec_err, (size_t)exec_err_len);
        _exit(127);
    }
    if (pid > 0) {
        slot->pid = pid;
        /*
         * Do it from this side as well, closing the window between the
         * fork and the child's own setpgid: a shutdown landing in that
         * window would signal a process group that does not exist yet and
         * fall back to the bare pid. Either outcome here is harmless -
         * EACCES means the child already exec'd, having done it itself,
         * ESRCH that it is already gone.
         */
        (void)setpgid(pid, pid);
    }
    pthread_mutex_unlock(&g_state_mtx);

    if (child_in  >= 0) close(child_in);
    if (child_out >= 0) close(child_out);
    if (child_err >= 0) close(child_err);
    if (logfd >= 0)   close(logfd);

    /*
     * fork() handed the child its own copy of the URL (and of
     * argv), so this thread's copy is spent: on a successful
     * fork yt-dlp already has everything it needs, and on a
     * failed one nothing was handed over - but the URL is ours
     * to release either way. This is where the "probe creates
     * -> recorder frees" chain ends.
     */
    free(slot->stream_url);
    slot->stream_url = NULL;

    if (pid < 0) {
        logmsg(LOG_ERROR, "%s fork failed: %s", ch->slug, strerror(errno));
        atomic_store_explicit(&slot->recording, false, memory_order_release);
        return NULL;
    }

    logmsg(LOG_INFO, "REC  %-*s pid %-7d -> %s", LOG_SLUG_COL, ch->slug,
           (int)pid, dir);
    /*
     * The fork succeeded, so yt-dlp is running: this is the one
     * start notification per recording. The early failures above
     * (bad output dir, bad template, fork failure) notify nothing,
     * and dry-run never enters this thread.
     */
    notify_recording_started(ch);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    /*
     * Take the child's pid, and with it the answer to "did we ask this one
     * to stop?", out of the slot under the same lock shutdown signals it
     * under. Reading a flag set outside that lock instead would leave a
     * window - shutdown between setting the flag and reaching this slot -
     * in which the thread decides nobody asked, the slot is already empty,
     * and the group is never cleaned up by either of them.
     */
    pthread_mutex_lock(&g_state_mtx);
    bool asked_to_stop = slot->asked_stop;
    slot->pid = 0;
    pthread_mutex_unlock(&g_state_mtx);

    /*
     * A process tree never outlives its recording thread.
     *
     * If we did not ask this child to stop, it died on its own - an
     * operator killing yt-dlp, a crash, an OOM - and whatever it left in
     * its process group, i.e. the ffmpeg doing the writing, is still
     * running with nobody to finalise or resume its output. The group has
     * no leader left, so nothing else will ever clean it up; SIGKILL is
     * what that takes, and the .part it was writing stays on disk either
     * way. Nothing to do when *we* sent the SIGTERM: the group already got
     * it, and ffmpeg still has the seconds it needs to close the file
     * properly - which is why this is not an unconditional kill.
     *
     * A clean exit is not a case of its own: yt-dlp shuts its own ffmpeg
     * down before exiting 0, so the group is already empty and the signal
     * is answered with ESRCH.
     */
    if (!asked_to_stop)
        signal_group(pid, SIGKILL);

    atomic_store_explicit(&slot->recording, false, memory_order_release);

    char dur[32];
    human_duration(dur, sizeof dur, (long)(time(NULL) - started));

    if (clean)
        logmsg(LOG_INFO, "DONE %-*s recorded %s", LOG_SLUG_COL, ch->slug, dur);
    else if (WIFEXITED(status))
        logmsg(LOG_WARN, "FAIL %-*s yt-dlp exit %d after %s%s%s",
               LOG_SLUG_COL, ch->slug, WEXITSTATUS(status), dur,
               logpath[0] ? " - see " : "", logpath[0] ? logpath : "");
    else
        logmsg(LOG_WARN, "STOP %-*s killed by signal %d after %s%s%s",
               LOG_SLUG_COL, ch->slug, WTERMSIG(status), dur,
               logpath[0] ? " - see " : "", logpath[0] ? logpath : "");

    /*
     * One stop for every start: natural stream end, yt-dlp failure,
     * and shutdown all flow through here, and the same exit-status
     * check that picked the log line picks the outcome word.
     */
    notify_recording_stopped(ch, clean, dur);

    return NULL;
}

bool recorder_is_busy(const channel_t *ch)
{
    /*
     * One atomic load, no lock. The poll loop calls this for every channel on
     * every tick, so making it take the recorder's private mutex serialised
     * the whole poll loop behind the thread that was recording.
     */
    rec_slot_t *slot = slot_of(ch);
    return slot && atomic_load_explicit(&slot->recording, memory_order_acquire);
}

bool recorder_tried_live(const channel_t *ch)
{
    rec_slot_t *slot = slot_of(ch);
    if (!slot)
        return false;
    pthread_mutex_lock(&g_state_mtx);
    bool tried = slot->tried_live;
    pthread_mutex_unlock(&g_state_mtx);
    return tried;
}

void recorder_reset_live(const channel_t *ch)
{
    rec_slot_t *slot = slot_of(ch);
    if (!slot)
        return;
    pthread_mutex_lock(&g_state_mtx);
    slot->tried_live = false;
    pthread_mutex_unlock(&g_state_mtx);
}

/*
 * Start a recording of stream_url for this channel. Takes
 * ownership of stream_url: it is stored in the channel's
 * slot for the recording thread, and freed by this function
 * on every path that never gets that far.
 */
bool recorder_start(const channel_t *ch, char *stream_url)
{
    rec_slot_t *slot = slot_of(ch);
    if (!slot) {
        free(stream_url);
        return false;
    }

    pthread_mutex_lock(&g_state_mtx);
    if (atomic_exchange_explicit(&slot->recording, true, memory_order_acq_rel)) {
        pthread_mutex_unlock(&g_state_mtx);
        free(stream_url);
        return false;
    }
    /*
     * The hand-off is *before* the create, not after it. pthread_create()
     * guarantees the new thread sees every write the creating thread made
     * before the call, so the recording thread cannot reach the free() at
     * the far end of the slot before the URL has landed.
     *
     * Storing it afterwards instead leaves the two threads racing for the
     * pointer, with no lock between them: the recording thread can read the
     * calloc'd NULL and hand yt-dlp the channel page instead of the live
     * stream, free that NULL, and clear the busy flag - after which the
     * poll thread stores the URL into a slot nobody will read or free
     * again. Nothing in the ASan build can see either half of that; it is
     * a plain data race, and it is what `make tsan` exists for.
     */
    slot->stream_url = stream_url;
    slot->tried_live = true;

    int rc = pthread_create(&slot->thread, NULL, record_thread, slot);
    if (rc != 0) {
        slot->stream_url = NULL;         /* no thread: the slot is ours */
        atomic_store_explicit(&slot->recording, false, memory_order_release);
        pthread_mutex_unlock(&g_state_mtx);
        free(stream_url);
        logmsg(LOG_ERROR, "%s pthread_create failed: %s", ch->slug, strerror(rc));
        return false;
    }
    slot->has_thread = true;
    pthread_mutex_unlock(&g_state_mtx);
    return true;
}

void recorder_shutdown(void)
{
    /* Signal every live recording under the lock: the process group, so
     * the ffmpeg underneath yt-dlp stops with it, and the slot's own
     * record that we asked - which is what tells the thread not to also
     * clean the group up afterwards. */
    pthread_mutex_lock(&g_state_mtx);
    for (size_t i = 0; i < g_nslots; i++) {
        if (g_slots[i].pid > 0) {
            logmsg(LOG_INFO, "stopping %s (pid %d)",
                   g_chans[i].slug, (int)g_slots[i].pid);
            g_slots[i].asked_stop = true;
            signal_recording(g_slots[i].pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_state_mtx);

    /* Join all recorder threads, then drop the slots for good. */
    for (size_t i = 0; i < g_nslots; i++) {
        if (g_slots[i].has_thread) {
            pthread_join(g_slots[i].thread, NULL);
            g_slots[i].has_thread = false;
        }
    }

    free(g_slots);
    g_slots   = NULL;
    g_nslots  = 0;
    g_chans   = NULL;
    g_out_dir = NULL;
}