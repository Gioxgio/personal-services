#include "../modules/json/json.h"

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(bool condition, const char* name) {
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
    check(json_parse_request("{\"source\": \"test\", \"message\": \"hello\"}", &payload) &&
              strcmp(payload.message, "hello") == 0 && strcmp(payload.source, "test") == 0,
          "Simple message");

    check(json_parse_request("  {  \"source\"  :  \"t\",  \"message\"  :  \"hi\"  }  ", &payload) &&
              strcmp(payload.message, "hi") == 0 && strcmp(payload.source, "t") == 0,
          "Whitespace");

    check(json_parse_request("{\"extra\": 123, \"message\": \"test\", \"source\": \"s\", \"other\": true}",
                             &payload) &&
              strcmp(payload.message, "test") == 0 && strcmp(payload.source, "s") == 0,
          "Multiple fields");

    // --- Escape sequences ---
    check(json_parse_request("{\"message\": \"hello\\nworld\", \"source\": \"s\"}", &payload) &&
              strcmp(payload.message, "hello\nworld") == 0,
          "Escape \\n");

    check(json_parse_request("{\"message\": \"\\\"\\\\\\n\\r\\t\\/\", \"source\": \"s\"}", &payload) &&
              strcmp(payload.message, "\"\\\n\r\t/") == 0,
          "All escape types");

    check(json_parse_request("{\"message\": \"a\\tb\\rc\", \"source\": \"s\"}", &payload) &&
              strcmp(payload.message, "a\tb\rc") == 0,
          "Escapes \\t and \\r");

    // --- UTF-8 ---
    check(json_parse_request("{\"message\": \"hello\xF0\x9F\x98\x80\", \"source\": \"s\"}", &payload) &&
              strcmp(payload.message, "hello\xF0\x9F\x98\x80") == 0,
          "Raw UTF-8 emoji");

    check(json_parse_request("{\"message\": \"caf\xC3\xA9\", \"source\": \"s\"}", &payload) &&
              strcmp(payload.message, "caf\xC3\xA9") == 0,
          "Raw UTF-8 accented");

    // --- Nested structures in other fields ---
    check(json_parse_request("{\"data\": {\"nested\": true}, \"message\": \"hi\", \"source\": \"s\"}",
                             &payload) &&
              strcmp(payload.message, "hi") == 0,
          "Nested object skipped");

    check(json_parse_request("{\"data\": [1, 2, 3], \"message\": \"hi\", \"source\": \"s\"}",
                             &payload) &&
              strcmp(payload.message, "hi") == 0,
          "Array skipped");

    check(json_parse_request("{\"data\": {\"message\": \"nested\"}, \"message\": \"top\", "
                             "\"source\": \"s\"}",
                             &payload) &&
              strcmp(payload.message, "top") == 0,
          "Nested message field ignored");

    check(json_parse_request("{\"data\": \"hello\\u1F600\", \"message\": \"hi\", \"source\": \"s\"}",
                             &payload) &&
              strcmp(payload.message, "hi") == 0,
          "\\uXXXX in other field skipped");

    // --- Rejections ---
    check(!json_parse_request("{\"foo\": \"bar\"}", &payload), "Missing message field");

    check(!json_parse_request("{\"message\": \"\"}", &payload), "Empty message");

    check(!json_parse_request("{\"message\": \"hello\\u1F600\"}", &payload),
          "\\uXXXX in message rejected");

    check(!json_parse_request("{\"source\": \"s\"}", &payload), "Missing message field");

    check(!json_parse_request("{\"source\": \"\", \"message\": \"hi\"}", &payload),
          "Empty source rejected");

    check(!json_parse_request("not json", &payload), "Invalid JSON");

    check(!json_parse_request("", &payload), "Empty string");

    check(!json_parse_request("   ", &payload), "Whitespace only");

    check(!json_parse_request("{\"message\": \"hi\"} extra", &payload),
          "Trailing content rejected");

    check(!json_parse_request("{\"message\": 123}", &payload), "Number value rejected");

    check(!json_parse_request("{\"message\": null}", &payload), "Null value rejected");

    check(!json_parse_request("{\"message\": true}", &payload), "Boolean value rejected");

    check(!json_parse_request("{\"message\": {\"nested\": true}}", &payload),
          "Object value rejected");

    check(!json_parse_request("{\"message\": [1, 2, 3]}", &payload), "Array value rejected");

    check(!json_parse_request("{\"message\": \"unterminated}", &payload),
          "Unterminated string rejected");

    check(!json_parse_request("{\"message\": \"bad\\xescape\"}", &payload),
          "Invalid escape rejected");

    // --- Length boundary ---
    char buf[5000];

    // Exactly 4096 chars — should pass
    strcpy(buf, "{\"source\": \"s\", \"message\": \"");
    for (int i = 0; i < 4096; i++)
        strcat(buf, "a");
    strcat(buf, "\"}");
    check(json_parse_request(buf, &payload) && strlen(payload.message) == 4096,
          "Exactly 4096 chars accepted");

    // 4097 chars — should fail
    strcpy(buf, "{\"source\": \"s\", \"message\": \"");
    for (int i = 0; i < 4097; i++)
        strcat(buf, "a");
    strcat(buf, "\"}");
    check(!json_parse_request(buf, &payload), "4097 chars rejected");

    // 2048 \n escapes (raw 4096 chars, unescaped 2048) — should pass
    strcpy(buf, "{\"source\": \"s\", \"message\": \"");
    for (int i = 0; i < 2048; i++)
        strcat(buf, "\\n");
    strcat(buf, "\"}");
    check(json_parse_request(buf, &payload) && strlen(payload.message) == 2048,
          "2048 \\n escapes accepted");

    // --- json_find_top_level_string ---
    char out[128];

    check(json_find_top_level_string("{\"job_id\": \"abc-123\"}", "job_id", out, sizeof(out)) &&
              strcmp(out, "abc-123") == 0,
          "find_top_level_string: job_id");

    check(json_find_top_level_string("{\"error\": \"unsupported_domain\"}", "error", out,
                                     sizeof(out)) &&
              strcmp(out, "unsupported_domain") == 0,
          "find_top_level_string: error");

    check(!json_find_top_level_string("{\"job_id\": \"abc\"}", "missing", out, sizeof(out)),
          "find_top_level_string: missing key");

    check(!json_find_top_level_string("{\"job_id\": 123}", "job_id", out, sizeof(out)),
          "find_top_level_string: non-string value");

    check(!json_find_top_level_string("not json", "job_id", out, sizeof(out)),
          "find_top_level_string: not an object");

    check(!json_find_top_level_string("{\"message\": {\"job_id\": \"inner\"}}", "job_id", out,
                                      sizeof(out)),
          "find_top_level_string: nested key ignored");

    // --- json_extract_telegram_update ---
    {
        char text[256];

        check(json_extract_telegram_update("{\"update_id\": 1,\"message\":{\"message_id\":5,"
                                           "\"chat\":{\"id\":42,\"type\":\"private\"},"
                                           "\"text\":\"/download https://youtu.be/xyz\"}}",
                                           text, sizeof(text)) &&
                  strcmp(text, "/download https://youtu.be/xyz") == 0,
              "extract_update: text extracted");

        /* Member order is not assumed; the chat object is irrelevant. */
        check(json_extract_telegram_update("{\"update_id\": 2,\"message\":{\"text\":\"hi\","
                                           "\"chat\":{\"id\":7,\"type\":\"group\"}}}",
                                           text, sizeof(text)) &&
                  strcmp(text, "hi") == 0,
              "extract_update: member order independent");

        /* No chat object: text is still extracted. */
        check(json_extract_telegram_update("{\"update_id\": 3,\"message\":{\"text\":\"hello\"}}",
                                           text, sizeof(text)) &&
                  strcmp(text, "hello") == 0,
              "extract_update: chat object not required");

        /* Missing text -> false. */
        check(!json_extract_telegram_update("{\"update_id\": 4,\"message\":{\"chat\":{\"id\":1}}}",
                                            text, sizeof(text)),
              "extract_update: missing text is false");

        /* Missing message object -> false. */
        check(!json_extract_telegram_update("{\"update_id\": 5}", text, sizeof(text)),
              "extract_update: missing message is false");

        /* Not an update -> false. */
        check(!json_extract_telegram_update("not json", text, sizeof(text)),
              "extract_update: not json is false");
    }

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
