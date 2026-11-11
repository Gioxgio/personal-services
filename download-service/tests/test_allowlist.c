#include "allowlist.h"

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(bool condition, const char *name) {
    if (condition) {
        printf("PASS: %s\n", name);
        passed++;
    } else {
        printf("FAIL: %s\n", name);
        failed++;
    }
}

/* YouTube allowlist from the plan's download.allowed_domains. */
static char *domains[] = {"youtube.com",   "www.youtube.com", "m.youtube.com",
                          "youtu.be",      "www.youtu.be"};
static allowlist_t list = {domains, sizeof(domains) / sizeof(domains[0])};

static void test_supported_hosts(void) {
    check(is_supported("https://www.youtube.com/watch?v=dQw4w9WgXcQ", &list),
          "www.youtube.com path is supported");
    check(is_supported("https://youtube.com/watch?v=x", &list),
          "youtube.com path is supported");
    check(is_supported("https://m.youtube.com/watch?v=x", &list),
          "m.youtube.com is supported");
    check(is_supported("https://youtu.be/dQw4w9WgXcQ", &list),
          "youtu.be is supported");
    check(is_supported("https://www.youtu.be/dQw4w9WgXcQ", &list),
          "www.youtu.be is supported");
    check(is_supported("http://WWW.YOUTUBE.COM/watch", &list),
          "uppercase host is supported");
    check(is_supported("https://www.youtube.com:8080/watch", &list),
          "host with port is supported");
    check(is_supported("https://www.youtube.com?x=1", &list),
          "host with query is supported");
    check(is_supported("https://www.youtube.com#frag", &list),
          "host with fragment is supported");
    check(is_supported("www.youtube.com/watch", &list),
          "host without scheme is supported");
}

static void test_unsupported_hosts(void) {
    check(!is_supported("https://vimeo.com/12345678", &list),
          "vimeo.com is unsupported");
    check(!is_supported("https://example.com", &list), "example.com is unsupported");
    check(!is_supported("https://evilyoutube.com/", &list),
          "evilyoutube.com is unsupported");
    check(!is_supported("https://youtube.com.evil.com/", &list),
          "youtube.com.evil.com is unsupported");
    check(!is_supported("https://evil.youtube.com.evil.com/", &list),
          "evil.youtube.com.evil.com is unsupported (tail attack)");
    check(!is_supported("", &list), "empty url is unsupported");
    check(!is_supported("not a url", &list), "nonsense url is unsupported");
    check(!is_supported("https://", &list), "scheme-only url is unsupported");
    check(!is_supported("http://", &list), "scheme-only url is unsupported");
    check(!is_supported("   ", &list), "whitespace url is unsupported");
}

static void test_empty_allowlist(void) {
    allowlist_t empty = {NULL, 0};
    check(!is_supported("https://www.youtube.com/", &empty),
          "everything is unsupported with an empty allowlist");
    allowlist_free(&empty);
}

static void test_allowlist_parse(void) {
    allowlist_t parsed;
    bool ok;

    ok = allowlist_parse("youtube.com, www.youtube.com", &parsed);
    check(ok, "parse: two domains accepted");
    check(parsed.count == 2, "parse: count is 2");
    check(parsed.count == 2 && strcmp(parsed.domains[0], "youtube.com") == 0,
          "parse: first domain");
    check(parsed.count == 2 && strcmp(parsed.domains[1], "www.youtube.com") == 0,
          "parse: second domain");
    allowlist_free(&parsed);

    ok = allowlist_parse("youtube.com,,  , youtu.be", &parsed);
    check(ok, "parse: empty entries skipped");
    check(parsed.count == 2, "parse: count after skipping empties");
    allowlist_free(&parsed);

    ok = allowlist_parse("", &parsed);
    check(ok && parsed.count == 0, "parse: empty spec is an empty allowlist");

    ok = allowlist_parse("   ", &parsed);
    check(ok && parsed.count == 0, "parse: whitespace spec is empty");

    ok = allowlist_parse(NULL, &parsed);
    check(ok && parsed.count == 0, "parse: NULL spec is empty");

    /* is_supported works against a parsed (non-empty) allowlist. */
    ok = allowlist_parse("youtu.be, youtube.com", &parsed);
    check(ok, "parse: youtu.be listed");
    check(is_supported("https://youtu.be/x", &parsed), "parse: supports listed host");
    check(!is_supported("https://vimeo.com/", &parsed),
          "parse: rejects unlisted host");
    allowlist_free(&parsed);

    /* Freeing an already-freed / empty list is safe. */
    allowlist_free(&parsed);
    allowlist_free(&parsed);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== allowlist: supported hosts ===\n");
    test_supported_hosts();

    printf("\n=== allowlist: unsupported hosts ===\n");
    test_unsupported_hosts();

    printf("\n=== allowlist: empty allowlist ===\n");
    test_empty_allowlist();

    printf("\n=== allowlist: parse ===\n");
    test_allowlist_parse();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
