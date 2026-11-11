/*
 * test_util - the pure string and filesystem helpers:
 * trim, strip_spaces, sanitise, last_path_segment (including
 * the query-string cut of bug #2), human_duration and
 * mkdir_p. Offline: no channels, no network, no recorder.
 */

#include "harness.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

/* sanitise() returns a heap copy: compare, then free (the exit-
 * time leak check in `make debug` is what notices a missed free). */
static void check_sanitise(const char *in, const char *want)
{
    char *got = sanitise(in);
    CHECK_STR_EQ(got, want);
    free(got);
}

int main(void)
{
    /* --- trim: trailing whitespace and newlines only --- */

    char s1[] = "abc\n";
    trim(s1);
    CHECK_STR_EQ(s1, "abc");

    char s2[] = "abc\r\n";
    trim(s2);
    CHECK_STR_EQ(s2, "abc");

    char s3[] = "abc \t\n";
    trim(s3);
    CHECK_STR_EQ(s3, "abc");

    /* Leading whitespace is not trim's business. */
    char s4[] = "  abc  ";
    trim(s4);
    CHECK_STR_EQ(s4, "  abc");

    char s5[] = "";
    trim(s5);
    CHECK_STR_EQ(s5, "");

    char s6[] = "   ";
    trim(s6);
    CHECK_STR_EQ(s6, "");

    /* --- strip_spaces: every space and tab goes, in place --- */

    char a1[] = "a b\tc";
    strip_spaces(a1);
    CHECK_STR_EQ(a1, "abc");

    char a2[] = "  ";
    strip_spaces(a2);
    CHECK_STR_EQ(a2, "");

    char a3[] = "";
    strip_spaces(a3);
    CHECK_STR_EQ(a3, "");

    char a4[] = "nochange";
    strip_spaces(a4);
    CHECK_STR_EQ(a4, "nochange");

    /* --- sanitise: a filesystem-safe copy, caller frees --- */

    check_sanitise("abc-DEF_123", "abc-DEF_123");
    check_sanitise("hello world", "hello_world");
    check_sanitise("a/b\\c:d", "a_b_c_d");
    check_sanitise("", "_");
    /* Byte-wise: both UTF-8 bytes of 'é' are non-alnum. */
    check_sanitise("caf\xc3\xa9", "caf__");

    /*
     * --- last_path_segment ---
     *
     * The string is cut in place at the first '?' or '#' (bug #2:
     * a query string must never reach the slug), trailing
     * slashes are dropped, and the last segment comes back as a
     * pointer into the (now shortened) buffer.
     */

    char u1[] = "https://twitch.tv/foo?lang=en";
    CHECK_STR_EQ(last_path_segment(u1), "foo");
    CHECK_STR_EQ(u1, "https://twitch.tv/foo");

    char u2[] = "https://twitch.tv/foo#frag";
    CHECK_STR_EQ(last_path_segment(u2), "foo");
    CHECK_STR_EQ(u2, "https://twitch.tv/foo");

    char u3[] = "https://twitch.tv/foo/";
    CHECK_STR_EQ(last_path_segment(u3), "foo");

    char u4[] = "https://twitch.tv/a/b/c";
    CHECK_STR_EQ(last_path_segment(u4), "c");

    /* A host-only URL: the host is the last segment. */
    char u5[] = "https://twitch.tv";
    CHECK_STR_EQ(last_path_segment(u5), "twitch.tv");

    /* No slash at all: the whole string is the segment. */
    char u6[] = "foo";
    CHECK_STR_EQ(last_path_segment(u6), "foo");

    /* Nothing left after the cut: no segment. */
    char u7[] = "";
    CHECK(last_path_segment(u7) == NULL);

    char u8[] = "/";
    CHECK(last_path_segment(u8) == NULL);

    char u9[] = "///";
    CHECK(last_path_segment(u9) == NULL);

    /* --- human_duration: "M:SS" / "H:MM:SS" --- */

    char buf[32];

    human_duration(buf, sizeof buf, 0);
    CHECK_STR_EQ(buf, "0:00");

    human_duration(buf, sizeof buf, 59);
    CHECK_STR_EQ(buf, "0:59");

    human_duration(buf, sizeof buf, 60);
    CHECK_STR_EQ(buf, "1:00");

    human_duration(buf, sizeof buf, 599);
    CHECK_STR_EQ(buf, "9:59");

    human_duration(buf, sizeof buf, 3600);
    CHECK_STR_EQ(buf, "1:00:00");

    human_duration(buf, sizeof buf, 3661);
    CHECK_STR_EQ(buf, "1:01:01");

    human_duration(buf, sizeof buf, 86399);
    CHECK_STR_EQ(buf, "23:59:59");

    /* Negative input clamps to zero. */
    human_duration(buf, sizeof buf, -5);
    CHECK_STR_EQ(buf, "0:00");

    /* A too-small buffer truncates safely: "1:01:01" keeps 3 bytes + NUL. */
    char tiny[4];
    human_duration(tiny, sizeof tiny, 3661);
    CHECK_STR_EQ(tiny, "1:0");

    char minibuf[1];
    human_duration(minibuf, sizeof minibuf, 0);
    CHECK_STR_EQ(minibuf, "");

    /*
     * --- mkdir_p ---
     *
     * The scratch tree lives under build/, which `make clean`
     * removes wholesale - nothing is left in the source tree.
     * Tests run from the project root (make test does).
     */
    const char *root = "build/.test-util";

    CHECK(mkdir_p(root));
    CHECK(mkdir_p(root));                    /* existing dir: still true */

    char nested[256];
    snprintf(nested, sizeof nested, "%s/a/b/c", root);
    CHECK(mkdir_p(nested));
    struct stat st;
    CHECK(stat(nested, &st) == 0 && S_ISDIR(st.st_mode));

    /* A regular file in the way of a component blocks creation. */
    char blocker[256];
    snprintf(blocker, sizeof blocker, "%s/blocker", root);
    FILE *f = fopen(blocker, "w");
    CHECK(f != NULL);
    if (f)
        fclose(f);
    snprintf(nested, sizeof nested, "%s/blocker/child", root);
    CHECK(!mkdir_p(nested));

    /* A path longer than PATH_MAX is refused, not truncated. */
    char *huge = malloc(PATH_MAX + 16);
    if (huge) {
        memset(huge, 'a', PATH_MAX + 15);
        huge[PATH_MAX + 15] = '\0';
        CHECK(!mkdir_p(huge));
        free(huge);
    }

    return harness_exit("test_util");
}
