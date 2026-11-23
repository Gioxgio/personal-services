# Merged services repo

Single repository merging `download-service`, `notification-service` and
`stream-sniper`, following the goal architecture from `whiteboard_parse.txt`:

- **1 repo** — this directory.
- **1 folder per service** — `download-service`, `notification-service`,
  `stream-sniper` (original folders kept, content deduplicated; originals
  under `/home/gioxgio/Documents/C` are untouched).
- **1 folder for cross-service modules** — `modules/` (HTTP client, HTTP
  server, JSON serializer, Telegram abstraction, and a scrape stub).
  Service-specific modules stay in their service folder (e.g.
  `download-service/json.c` has a different API; `stream-sniper/config`
  depends on `stream-sniper/urlparse`).
- **1 Dockerfile** — root `Dockerfile`, one build target stage per service.
- **1 Makefile** — root `Makefile` that builds/tests/cleans every service.

## Layout

```
.
├── docker-compose.yml      # single compose: all three services, one network
├── Dockerfile              # one target stage per service (--target <svc>)
├── Makefile                # all / test / clean across services
├── README.md
├── modules/                # cross-service modules
│   ├── http_client/        # HTTP client (POST, file upload)
│   ├── http_server/        # HTTP server (POST)
│   ├── json/               # JSON serializer
│   ├── filename/           # sanitizer shared by live slugs and download titles
│   ├── scrape/             # itch.io / forum.lewdzone scraping (stub, not implemented yet)
│   └── telegram/           # Telegram abstraction
├── download-service/       # service (own json.c - different API from modules/json)
├── notification-service/   # service (uses modules/json, http_server, telegram)
└── stream-sniper/          # service (uses modules/http_client; keeps config + urlparse)
```

`itch.io` is a planned fourth service (whiteboard) but no source project
exists for it yet; there is no `itch.io` folder. The scrape module is
a stub until then.

## Build

```sh
make                   # build every service (each in services/<svc>/build/)
make <svc>             # build one service
make test              # run every service's test suite
make test-<svc>        # run one service's tests
make clean             # clean every service
```

Each service keeps its own Makefile, so you can also `cd <svc> && make`.

## Code style

All C sources are formatted with a single, repo-wide
[`.clang-format`](.clang-format) (clang-format walks up from each source file
to the root config, so no per-service copies exist) and driven by
[`.clangd`](.clangd) (`-Wall -Wextra -Wpedantic -std=c11`, inlay hints on).

Style highlights: 4-space indentation (no tabs), K&R braces, 100-column limit,
C11. To (re)format everything:

```sh
clang-format -i $(git ls-files '*.c' '*.h')
```

A file conforms to the config when `clang-format --dry-run --Werror <file>`
exits cleanly; CI can enforce this on the whole tree with:

```sh
for f in $(git ls-files '*.c' '*.h'); do clang-format --dry-run --Werror "$f"; done
```

## Docker

A single root `Dockerfile` has one build target stage per service
(`--target <svc>`). A single root `docker-compose.yml` builds and runs all
three on one network:

```sh
docker compose up --build
```

Service-to-service URLs are pre-wired per the whiteboard diagram:
- `download-service` POSTs download-result callbacks to
  `http://notification-service:8080/callback/download`
- `stream-sniper` POSTs recording start/stop events to
  `http://notification-service:8080/notifications`

Each service maps its own port (download-service 8081, notification-service
8080, stream-sniper has no published port by default). Every variable is read
from the service's own `.env` file: there are no defaults or fallbacks in the
code, the Dockerfile or compose. The `.env.template` files hold the current
non-secret values; copy each one to `.env` and fill in the secrets
(`TELEGRAM_TOKEN`, `TWITCH_CLIENT_SECRET`, `TWITCH_TOKEN`) before bringing the
stack up. An unset variable is an error; an empty value is explicit and turns
the feature it controls off.

## Notifications: the `source` field (whiteboard todo)

- `POST /notifications` on `notification-service` requires both `source`
  and `message` strings:

  ```json
  { "source": "stream-sniper", "message": "started recording cool-game" }
  ```

- The message is always formatted as `"[source] message"` (e.g.
  `"[stream-sniper] started recording cool-game"`).
- Every service that sends notifications tags itself:
  - `stream-sniper` POSTs `{ "source": "stream-sniper", "message": ... }`
    (message text no longer repeats the service name).
  - `download-service` POSTs download status text notifications (start and
    result) with `"source": "download-service"`; `notification-service` does
    not send a separate "Download started." reply. The actual file is
    delivered separately: the callback to `/callback/download` carries
    `file_path`, and
    `notification-service` fetches it from `GET /file/<job_id>` and uploads
    it to Telegram as a document.
