/*
 * test_jsonlite - jsonlite: string extraction from
 * JSON-ish bodies.
 *
 * Covers the two shapes the program actually scans: the
 * Twitch token-mint response (a single "access_token")
 * and the YouTube /streams body (many "videoId"s in
 * document order, with repeats and empties).
 */

#include "harness.h"

#include "jsonlite.h"

int main(void)
{
    /* --- json_find_string: the mint response shape --- */

    char tok[512];
    CHECK(json_find_string("{\"access_token\":\"abc123\",\"expires_in\":3600}",
                           "access_token", tok, sizeof tok) == 6);
    CHECK_STR_EQ(tok, "abc123");

    /* The key must be a quoted key: inside a longer
     * key it does not count. */
    CHECK(json_find_string("{\"my_access_token\":\"abc\"}",
                           "access_token", tok, sizeof tok) == 0);

    /* Missing key. */
    CHECK(json_find_string("{\"error\":\"invalid_client\"}",
                           "access_token", tok, sizeof tok) == 0);

    /* Empty value. */
    CHECK(json_find_string("{\"access_token\":\"\"}",
                           "access_token", tok, sizeof tok) == 0);
    CHECK_STR_EQ(tok, "");

    /* Value runs to the end of the body when no
     * closing quote follows. */
    CHECK(json_find_string("{\"access_token\":\"abc",
                           "access_token", tok, sizeof tok) == 3);
    CHECK_STR_EQ(tok, "abc");

    /* First match wins when the key repeats. */
    CHECK(json_find_string("{\"access_token\":\"one\",\"x\":\"two\","
                           "\"access_token\":\"two\"}",
                           "access_token", tok, sizeof tok) == 3);
    CHECK_STR_EQ(tok, "one");

    /* Buffer overflow: the value is truncated, not the stack. */
    char small[8];
    CHECK(json_find_string("{\"access_token\":\"0123456789abcdef\"}",
                           "access_token", small, sizeof small) == 7);
    CHECK_STR_EQ(small, "0123456");

    /* --- json_collect_strings: the /streams body shape --- */

    char vids[3][32];
    const char *streams =
        "{\"videoId\":\"abcdefghijk\",\"foo\":1,"
        "\"videoId\":\"abcdefghijk\","        /* repeat: skipped  */
        "\"videoId\":\"\",\"videoId\":\"zzzzzzzzzzz\"}";  /* empty: skipped */
    CHECK(json_collect_strings(streams, "videoId", vids, 3) == 2);
    CHECK_STR_EQ(vids[0], "abcdefghijk");
    CHECK_STR_EQ(vids[1], "zzzzzzzzzzz");

    /* Cap at max: only the first three distinct values are kept. */
    const char *many =
        "\"videoId\":\"one\",\"videoId\":\"two\",\"videoId\":\"three\","
        "\"videoId\":\"four\",\"videoId\":\"five\"";
    CHECK(json_collect_strings(many, "videoId", vids, 3) == 3);
    CHECK_STR_EQ(vids[0], "one");
    CHECK_STR_EQ(vids[1], "two");
    CHECK_STR_EQ(vids[2], "three");

    /* Missing key collects nothing. */
    CHECK(json_collect_strings("{\"foo\":1}", "videoId", vids, 3) == 0);

    /* A zero cap collects nothing and writes nothing. */
    CHECK(json_collect_strings(many, "videoId", vids, 0) == 0);

    /* Values longer than a row are truncated to its width. */
    char rows[2][32];
    CHECK(json_collect_strings(
        "\"videoId\":\"0123456789012345678901234567890123456789\","
        "\"videoId\":\"9876543210987654321098765432109876543210\"",
        "videoId", rows, 2) == 2);
    CHECK_STR_EQ(rows[0], "0123456789012345678901234567890");
    CHECK_STR_EQ(rows[1], "9876543210987654321098765432109");

    return harness_exit("test_jsonlite");
}
