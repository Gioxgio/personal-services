#include "../modules/json/json.h"
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

int main() {
    notification_payload_t payload;

    // --- Basic parsing ---
    check(json_parse_request("{\"message\": \"hello\"}", &payload) &&
              strcmp(payload.message, "hello") == 0,
          "Simple message");

    check(json_parse_request("  {  \"message\"  :  \"hi\"  }  ", &payload) &&
              strcmp(payload.message, "hi") == 0,
          "Whitespace");

    check(json_parse_request(
              "{\"chat_id\": 123, \"message\": \"test\", \"other\": true}",
              &payload) &&
              strcmp(payload.message, "test") == 0,
          "Multiple fields");

    // --- Escape sequences ---
    check(json_parse_request("{\"message\": \"hello\\nworld\"}", &payload) &&
              strcmp(payload.message, "hello\nworld") == 0,
          "Escape \\n");

    check(json_parse_request("{\"message\": \"\\\"\\\\\\n\\r\\t\\/\"}",
                             &payload) &&
              strcmp(payload.message, "\"\\\n\r\t/") == 0,
          "All escape types");

    check(json_parse_request("{\"message\": \"a\\tb\\rc\"}", &payload) &&
              strcmp(payload.message, "a\tb\rc") == 0,
          "Escapes \\t and \\r");

    // --- UTF-8 ---
    check(json_parse_request("{\"message\": \"hello\xF0\x9F\x98\x80\"}",
                             &payload) &&
              strcmp(payload.message, "hello\xF0\x9F\x98\x80") == 0,
          "Raw UTF-8 emoji");

    check(json_parse_request("{\"message\": \"caf\xC3\xA9\"}", &payload) &&
              strcmp(payload.message, "caf\xC3\xA9") == 0,
          "Raw UTF-8 accented");

    // --- Nested structures in other fields ---
    check(
        json_parse_request(
            "{\"data\": {\"nested\": true}, \"message\": \"hi\"}", &payload) &&
            strcmp(payload.message, "hi") == 0,
        "Nested object skipped");

    check(json_parse_request("{\"data\": [1, 2, 3], \"message\": \"hi\"}",
                             &payload) &&
              strcmp(payload.message, "hi") == 0,
          "Array skipped");

    check(json_parse_request(
              "{\"data\": {\"message\": \"nested\"}, \"message\": \"top\"}",
              &payload) &&
              strcmp(payload.message, "top") == 0,
          "Nested message field ignored");

    check(json_parse_request(
              "{\"data\": \"hello\\u1F600\", \"message\": \"hi\"}", &payload) &&
              strcmp(payload.message, "hi") == 0,
          "\\uXXXX in other field skipped");

    // --- Rejections ---
    check(!json_parse_request("{\"foo\": \"bar\"}", &payload),
          "Missing message field");

    check(!json_parse_request("{\"message\": \"\"}", &payload),
          "Empty message");

    check(!json_parse_request("{\"message\": \"hello\\u1F600\"}", &payload),
          "\\uXXXX in message rejected");

    check(!json_parse_request("not json", &payload), "Invalid JSON");

    check(!json_parse_request("", &payload), "Empty string");

    check(!json_parse_request("   ", &payload), "Whitespace only");

    check(!json_parse_request("{\"message\": \"hi\"} extra", &payload),
          "Trailing content rejected");

    check(!json_parse_request("{\"message\": 123}", &payload),
          "Number value rejected");

    check(!json_parse_request("{\"message\": null}", &payload),
          "Null value rejected");

    check(!json_parse_request("{\"message\": true}", &payload),
          "Boolean value rejected");

    check(!json_parse_request("{\"message\": {\"nested\": true}}", &payload),
          "Object value rejected");

    check(!json_parse_request("{\"message\": [1, 2, 3]}", &payload),
          "Array value rejected");

    check(!json_parse_request("{\"message\": \"unterminated}", &payload),
          "Unterminated string rejected");

    check(!json_parse_request("{\"message\": \"bad\\xescape\"}", &payload),
          "Invalid escape rejected");

    // --- Length boundary ---
    char buf[5000];

    // Exactly 4096 chars — should pass
    strcpy(buf, "{\"message\": \"");
    for (int i = 0; i < 4096; i++)
        strcat(buf, "a");
    strcat(buf, "\"}");
    check(json_parse_request(buf, &payload) && strlen(payload.message) == 4096,
          "Exactly 4096 chars accepted");

    // 4097 chars — should fail
    strcpy(buf, "{\"message\": \"");
    for (int i = 0; i < 4097; i++)
        strcat(buf, "a");
    strcat(buf, "\"}");
    check(!json_parse_request(buf, &payload), "4097 chars rejected");

    // 2048 \n escapes (raw 4096 chars, unescaped 2048) — should pass
    strcpy(buf, "{\"message\": \"");
    for (int i = 0; i < 2048; i++)
        strcat(buf, "\\n");
    strcat(buf, "\"}");
    check(json_parse_request(buf, &payload) && strlen(payload.message) == 2048,
          "2048 \\n escapes accepted");

    // --- json_find_top_level_string ---
    char out[128];

    check(json_find_top_level_string("{\"job_id\": \"abc-123\"}", "job_id",
                                     out, sizeof(out)) &&
              strcmp(out, "abc-123") == 0,
          "find_top_level_string: job_id");

    check(json_find_top_level_string("{\"error\": \"unsupported_domain\"}",
                                     "error", out, sizeof(out)) &&
              strcmp(out, "unsupported_domain") == 0,
          "find_top_level_string: error");

    check(!json_find_top_level_string("{\"job_id\": \"abc\"}", "missing",
                                      out, sizeof(out)),
          "find_top_level_string: missing key");

    check(!json_find_top_level_string("{\"job_id\": 123}", "job_id", out,
                                      sizeof(out)),
          "find_top_level_string: non-string value");

    check(!json_find_top_level_string("not json", "job_id", out, sizeof(out)),
          "find_top_level_string: not an object");

    check(!json_find_top_level_string("{\"message\": {\"job_id\": \"inner\"}}",
                                      "job_id", out, sizeof(out)),
          "find_top_level_string: nested key ignored");

    // --- json_extract_telegram_update ---
    {
        char chat[32];
        char text[256];

        check(json_extract_telegram_update(
                  "{\"update_id\": 1,\"message\":{\"message_id\":5,"
                  "\"chat\":{\"id\":42,\"type\":\"private\"},"
                  "\"text\":\"/download https://youtu.be/xyz\"}}",
                  chat, sizeof(chat), text, sizeof(text)) &&
              strcmp(chat, "42") == 0 &&
              strcmp(text, "/download https://youtu.be/xyz") == 0,
          "extract_update: chat id and text");

        /* Member order is not assumed. */
        check(json_extract_telegram_update(
                  "{\"update_id\": 2,\"message\":{\"text\":\"hi\","
                  "\"chat\":{\"id\":7,\"type\":\"group\"}}}",
                  chat, sizeof(chat), text, sizeof(text)) &&
              strcmp(chat, "7") == 0 && strcmp(text, "hi") == 0,
          "extract_update: member order independent");

        /* Negative chat id (supergroup/channel). */
        check(json_extract_telegram_update(
                  "{\"update_id\": 3,\"message\":{\"chat\":{\"id\":-100123},"
                  "\"text\":\"hello\"}}",
                  chat, sizeof(chat), text, sizeof(text)) &&
              strcmp(chat, "-100123") == 0 && strcmp(text, "hello") == 0,
          "extract_update: negative chat id");

        /* Missing text -> false. */
        check(!json_extract_telegram_update(
                  "{\"update_id\": 4,\"message\":{\"chat\":{\"id\":1}}}",
                  chat, sizeof(chat), text, sizeof(text)),
          "extract_update: missing text is false");

        /* Missing chat -> false. */
        check(!json_extract_telegram_update(
                  "{\"update_id\": 5,\"message\":{\"text\":\"hello\"}}",
                  chat, sizeof(chat), text, sizeof(text)),
          "extract_update: missing chat is false");

        /* Not an update -> false. */
        check(!json_extract_telegram_update("not json", chat, sizeof(chat),
                                            text, sizeof(text)),
          "extract_update: not json is false");
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
