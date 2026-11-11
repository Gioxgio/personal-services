# Two-Way Bot Communication Plan

## Current Architecture

Single-threaded C11 HTTP service — one-way pipeline:

```
HTTP Client → POST /notifications → json_parse → telegram_send_message → Telegram API
```

No inbound message handling, no conversation state, no interactive responses.

## Target Architecture

```
Telegram ──webhook──→ POST /webhook
                          ↓
                    json_parse_update (extract text, chat_id, message_id)
                          ↓
                    intent_router (state machine + message content)
                          ↓
            ┌─────────────┼──────────────┐
            ↓             ↓              ↓
       state=IDLE     state=CHOICE   state=RESOLUTION
            ↓             ↓              ↓
       ask audio/video  ask res     call download svc
            ↓             ↓              ↓
       telegram_send_message (reply to user)
```

## New Modules

### 1. `state.c` / `state.h` — Conversation State Machine

Per-user session tracking via in-memory hash map (chat_id → state).

```
States: IDLE → AWAITING_URL → AWAITING_CHOICE → AWAITING_RESOLUTION → DONE

Session fields:
  - chat_id (uint64)
  - state (enum)
  - youtube_url (char*)
  - download_type (enum: AUDIO / VIDEO)
  - resolution (char*, e.g. "720p")
  - last_activity (timestamp, for session expiry)
```

**Key functions:**
- `state_get(chat_id)` → session pointer (creates if missing)
- `state_update(chat_id, new_state, ...)`
- `state_reset(chat_id)` → back to IDLE
- `state_expire_old(max_age_seconds)` → periodic cleanup

### 2. `intent.c` / `intent.h` — Intent Router

Routes incoming messages based on current state + message content.

**Routing logic:**

| Current State | User Message | Intent | Action |
|---|---|---|---|
| IDLE | Contains YT URL | `youtube_url` | Store URL, → AWAITING_CHOICE, ask "audio or video?" |
| IDLE | Anything else | `unknown` | Reply "Send me a YouTube link" |
| AWAITING_CHOICE | "audio" / "mp3" | `choice_audio` | Call download service (audio) |
| AWAITING_CHOICE | "video" / "mp4" | `choice_video` | → AWAITING_RESOLUTION, ask "what resolution?" |
| AWAITING_CHOICE | "cancel" | `cancel` | → IDLE |
| AWAITING_RESOLUTION | "144p"–"2160p" | `resolution` | Call download service (video + res) |
| AWAITING_RESOLUTION | "cancel" | `cancel` | → IDLE |

**YouTube URL detection patterns:**
- `youtube.com/watch?v=...`
- `youtu.be/...`
- `youtube.com/shorts/...`

### 3. `download.c` / `download.h` — Download Service Client

HTTP client (libcurl, same pattern as `telegram.c`) that POSTs to the download service.

```c
typedef struct {
    const char *url;         // YouTube URL
    download_type_t type;    // AUDIO or VIDEO
    const char *resolution;  // "360p", "720p", "1080p", etc. (NULL for audio)
} download_request_t;

int download_submit(const download_request_t *req);
```

POST body example:
```json
{
  "url": "https://youtube.com/watch?v=...",
  "type": "video",
  "resolution": "720p"
}
```

### 4. Updated `json.c` — Parse Telegram Updates

Current parser only handles `{"message": "..."}`. Needs to handle Telegram's [Update object](https://core.telegram.org/bots/api#update):

```json
{
  "update_id": 12345,
  "message": {
    "message_id": 1,
    "from": {"id": 67890, "username": "..."},
    "chat": {"id": 67890},
    "text": "https://youtube.com/watch?v=..."
  }
}
```

Extract: `message.chat.id`, `message.text`, `message.message_id`.

### 5. Updated `server.c` — Add `POST /webhook` Route

New route alongside existing `POST /notifications`:

```
POST /notifications  → existing (fire-and-forget notification)
POST /webhook        → new (Telegram update handler)
```

Webhook handler flow:
1. Parse JSON body as Telegram Update
2. Extract `chat_id`, `text`
3. Look up session state
4. Route via intent router
5. Send reply via `telegram_send_message()` (already exists)
6. If intent triggers download, call `download_submit()`

### 6. Updated `telegram.c` — Add `getUpdates` (Optional)

Only needed if using long-polling instead of webhooks. Would run in a background thread.

## Implementation Skeleton

### state.h

```c
#ifndef STATE_H
#define STATE_H

#include <stdint.h>
#include <time.h>

typedef enum {
    STATE_IDLE = 0,
    STATE_AWAITING_URL,
    STATE_AWAITING_CHOICE,
    STATE_AWAITING_RESOLUTION,
    STATE_DONE
} state_t;

typedef enum {
    DOWNLOAD_AUDIO = 0,
    DOWNLOAD_VIDEO
} download_type_t;

typedef struct {
    uint64_t chat_id;
    state_t state;
    char *youtube_url;
    download_type_t download_type;
    char *resolution;
    time_t last_activity;
} session_t;

session_t *state_get(uint64_t chat_id);
void state_transition(uint64_t chat_id, state_t new_state);
void state_set_url(uint64_t chat_id, const char *url);
void state_set_choice(uint64_t chat_id, download_type_t type);
void state_set_resolution(uint64_t chat_id, const char *resolution);
void state_reset(uint64_t chat_id);
void state_expire_old(int max_age_seconds);

#endif
```

### intent.h

```c
#ifndef INTENT_H
#define INTENT_H

#include <stdint.h>
#include "state.h"

typedef enum {
    INTENT_YOUTUBE_URL = 0,
    INTENT_CHOICE_AUDIO,
    INTENT_CHOICE_VIDEO,
    INTENT_RESOLUTION,
    INTENT_CANCEL,
    INTENT_UNKNOWN
} intent_t;

intent_t intent_route(uint64_t chat_id, const char *text);
const char *intent_get_reply(intent_t intent);

#endif
```

### download.h

```c
#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "state.h"

typedef struct {
    const char *url;
    download_type_t type;
    const char *resolution;
} download_request_t;

int download_submit(const download_request_t *req);

#endif
```

### intent.c (routing logic)

```c
#include "intent.h"
#include <string.h>
#include <ctype.h>

static int is_youtube_url(const char *text) {
    return strstr(text, "youtube.com/watch") != NULL
        || strstr(text, "youtu.be/") != NULL
        || strstr(text, "youtube.com/shorts/") != NULL;
}

intent_t intent_route(uint64_t chat_id, const char *text) {
    session_t *s = state_get(chat_id);

    switch (s->state) {
    case STATE_IDLE:
        if (is_youtube_url(text)) {
            state_set_url(chat_id, text);
            state_transition(chat_id, STATE_AWAITING_CHOICE);
            return INTENT_YOUTUBE_URL;
        }
        return INTENT_UNKNOWN;

    case STATE_AWAITING_CHOICE:
        if (strcasecmp(text, "audio") == 0 || strcasecmp(text, "mp3") == 0) {
            state_set_choice(chat_id, DOWNLOAD_AUDIO);
            state_transition(chat_id, STATE_DONE);
            return INTENT_CHOICE_AUDIO;
        }
        if (strcasecmp(text, "video") == 0 || strcasecmp(text, "mp4") == 0) {
            state_set_choice(chat_id, DOWNLOAD_VIDEO);
            state_transition(chat_id, STATE_AWAITING_RESOLUTION);
            return INTENT_CHOICE_VIDEO;
        }
        if (strcasecmp(text, "cancel") == 0) {
            state_reset(chat_id);
            return INTENT_CANCEL;
        }
        return INTENT_UNKNOWN;

    case STATE_AWAITING_RESOLUTION:
        if (strcasecmp(text, "cancel") == 0) {
            state_reset(chat_id);
            return INTENT_CANCEL;
        }
        // Validate resolution: 144p, 240p, 360p, 480p, 720p, 1080p, 1440p, 2160p
        const char *valid[] = {"144p","240p","360p","480p","720p","1080p","1440p","2160p"};
        for (int i = 0; i < 8; i++) {
            if (strcasecmp(text, valid[i]) == 0) {
                state_set_resolution(chat_id, text);
                state_transition(chat_id, STATE_DONE);
                return INTENT_RESOLUTION;
            }
        }
        return INTENT_UNKNOWN;

    default:
        return INTENT_UNKNOWN;
    }
}

const char *intent_get_reply(intent_t intent) {
    switch (intent) {
    case INTENT_YOUTUBE_URL:    return "Would you like audio or video?";
    case INTENT_CHOICE_VIDEO:   return "What resolution? (144p–2160p)";
    case INTENT_CHOICE_AUDIO:   return "Downloading audio...";
    case INTENT_RESOLUTION:     return "Downloading video...";
    case INTENT_CANCEL:         return "Cancelled.";
    case INTENT_UNKNOWN:
    default:                    return "Send me a YouTube link to get started.";
    }
}
```

### download.c (service client)

```c
#include "download.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

static char download_service_url[512] = "http://download-service:9000/download";

int download_submit(const download_request_t *req) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    // Build JSON body
    char body[1024];
    if (req->type == DOWNLOAD_AUDIO) {
        snprintf(body, sizeof(body),
            "{\"url\":\"%s\",\"type\":\"audio\"}", req->url);
    } else {
        snprintf(body, sizeof(body),
            "{\"url\":\"%s\",\"type\":\"video\",\"resolution\":\"%s\"}",
            req->url, req->resolution);
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, download_service_url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) ? 0 : -1;
}
```

### server.c (webhook handler)

```c
// Add to request_handler_dispatch():
if (strcmp(path, "/webhook") == 0 && strcmp(method, "POST") == 0) {
    // Parse Telegram Update
    telegram_update_t update;
    if (json_parse_update(body, &update) != 0) {
        set_response(response, 400);
        return;
    }

    // Route intent
    intent_t intent = intent_route(update.chat_id, update.text);

    // Send reply to user
    const char *reply = intent_get_reply(intent);
    telegram_send_message(update.chat_id, reply);

    // Trigger download if ready
    if (intent == INTENT_CHOICE_AUDIO || intent == INTENT_RESOLUTION) {
        session_t *s = state_get(update.chat_id);
        download_request_t req = {
            .url = s->youtube_url,
            .type = s->download_type,
            .resolution = s->resolution
        };
        download_submit(&req);
    }

    set_response(response, 200);
    return;
}
```

## Configuration Changes

New environment variables:

```
DOWNLOAD_SERVICE_URL=http://download-service:9000/download
SESSION_TIMEOUT_SECONDS=300
```

Updated `.env.example`:
```env
PORT=8080
TELEGRAM_TOKEN=your_bot_token
TELEGRAM_CHAT_ID=your_chat_id
DOWNLOAD_SERVICE_URL=http://download-service:9000/download
SESSION_TIMEOUT_SECONDS=300
```

## Telegram Bot Setup

1. Set webhook: `https://api.telegram.org/bot<TOKEN>/setWebhook?url=<your-url>/webhook`
2. Or use long-polling via `getUpdates` (background thread)

## Implementation Order

1. `state.h` + `state.c` — session store + state machine
2. `intent.h` + `intent.c` — routing logic
3. `download.h` + `download.c` — download service client
4. Update `json.c` — parse Telegram Update objects
5. Update `server.c` — add `POST /webhook` route
6. Update `telegram.c` — reply helper (sendMessage with chat_id)
7. Update `main.c` — load new env vars
8. Tests for all new modules
9. Docker-compose updates

## Open Questions

1. **Webhook vs polling** — public URL available, or use long-polling?
2. **Download service API** — what's the actual endpoint/payload?
3. **State persistence** — in-memory OK, or need Redis/DB?
4. **Multiple users** — multi-user support needed?
5. **Resolution options** — which resolutions to support?
