#include "json.h"

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

int main(void) {
    download_payload_t payload;

    /* --- Basic parsing --- */
    check(json_parse_download(
              "{\"url\": \"https://youtu.be/dQw4w9WgXcQ\", \"format\": \"video\"}",
              &payload) &&
              strcmp(payload.url, "https://youtu.be/dQw4w9WgXcQ") == 0 &&
              strcmp(payload.format, "video") == 0,
          "video format");

    check(json_parse_download("{\"url\": \"https://youtu.be/abc\", \"format\": \"audio\"}",
              &payload) &&
              strcmp(payload.format, "audio") == 0,
          "audio format");

    /* format is optional and defaults to video */
    check(json_parse_download("{\"url\": \"https://youtu.be/abc\"}", &payload) &&
              strcmp(payload.url, "https://youtu.be/abc") == 0 &&
              strcmp(payload.format, "video") == 0 && payload.chat_id == 0,
          "missing format defaults to video");

    /* chat_id is optional and defaults to 0 */
    check(json_parse_download("{\"url\": \"u\", \"format\": \"video\"}", &payload) &&
              payload.chat_id == 0,
          "missing chat_id defaults to 0");

    /* chat_id is parsed and echoed (JSON number, unquoted) */
    check(json_parse_download("{\"url\": \"u\", \"format\": \"video\", \"chat_id\": 42}",
              &payload) &&
              payload.chat_id == 42,
          "chat_id parsed");

    /* chat_id 0 is explicit and valid */
    check(json_parse_download("{\"url\": \"u\", \"chat_id\": 0}", &payload) &&
              payload.chat_id == 0 && strcmp(payload.format, "video") == 0,
          "chat_id zero with default format");

    /* chat_id as the first key also works */
    check(json_parse_download("{\"chat_id\": 7, \"url\": \"u\"}", &payload) &&
              payload.chat_id == 7,
          "chat_id first key parsed");

    /* A quoted chat_id (a string) is rejected */
    check(!json_parse_download("{\"url\": \"u\", \"chat_id\": \"42\"}", &payload),
          "quoted chat_id rejected");

    /* Negative chat_id is rejected */
    check(!json_parse_download("{\"url\": \"u\", \"chat_id\": -1}", &payload),
          "negative chat_id rejected");

    /* Reordered keys and whitespace */
    check(json_parse_download(
              "  { \"format\" : \"audio\", \"url\" : \"http://example.com\" }  ",
              &payload) &&
              strcmp(payload.url, "http://example.com") == 0 &&
              strcmp(payload.format, "audio") == 0,
          "reordered keys and whitespace");

    /* Reordered keys and whitespace */
    check(json_parse_download(
              "  { \"format\" : \"audio\", \"url\" : \"http://example.com\" }  ",
              &payload) &&
              strcmp(payload.url, "http://example.com") == 0 &&
              strcmp(payload.format, "audio") == 0,
          "reordered keys and whitespace");

    /* Extra unknown field is ignored */
    check(json_parse_download(
              "{\"url\": \"u\", \"format\": \"video\", \"extra\": 1}",
              &payload) &&
              strcmp(payload.url, "u") == 0 &&
              strcmp(payload.format, "video") == 0,
          "extra field ignored");

    /* Escaped characters in url */
    check(json_parse_download("{\"url\": \"http://x\\ny\", \"format\": \"video\"}",
              &payload) &&
              strcmp(payload.url, "http://x\ny") == 0,
          "escape \\n in url");

    /* --- Rejections --- */
    check(!json_parse_download("{\"format\": \"video\"}", &payload),
          "missing url");
    /* empty format falls back to the default */
    check(json_parse_download("{\"url\": \"u\", \"format\": \"\"}", &payload) &&
              strcmp(payload.format, "video") == 0,
          "empty format defaults to video");
    check(!json_parse_download("{\"url\": \"\", \"format\": \"video\"}", &payload),
          "empty url");
    check(!json_parse_download("{\"url\": \"u\", \"format\": \"hls\"}", &payload),
          "invalid format");
    check(!json_parse_download("{\"url\": \"u\", \"format\": \"VIDEO\"}", &payload),
          "case-sensitive format");
    check(!json_parse_download("not json", &payload), "invalid json");
    check(!json_parse_download("", &payload), "empty string");
    check(!json_parse_download("   ", &payload), "whitespace only");
    check(!json_parse_download("{\"url\": \"u\", \"format\": \"video\"} extra",
              &payload),
          "trailing content rejected");
    check(!json_parse_download("{\"url\": 123, \"format\": \"video\"}", &payload),
          "url not a string");
    check(!json_parse_download("{\"url\": \"u\", \"format\": 123}", &payload),
          "format not a string");
    check(!json_parse_download("{\"url\": \"u\", \"format\": null}", &payload),
          "format null");
    check(!json_parse_download("{\"url\": \"unterminated", &payload),
          "unterminated object");

    /* --- Length boundary --- */
    {
        char buf[5000];
        strcpy(buf, "{\"url\": \"");
        for (int i = 0; i < MAX_DOWNLOAD_URL; i++)
            strcat(buf, "a");
        strcat(buf, "\", \"format\": \"video\"}");
        check(json_parse_download(buf, &payload) &&
                  strlen(payload.url) == MAX_DOWNLOAD_URL,
              "exactly MAX_DOWNLOAD_URL url accepted");

        strcpy(buf, "{\"url\": \"");
        for (int i = 0; i < MAX_DOWNLOAD_URL + 1; i++)
            strcat(buf, "a");
        strcat(buf, "\", \"format\": \"video\"}");
        check(!json_parse_download(buf, &payload),
              "overlong url rejected");
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
