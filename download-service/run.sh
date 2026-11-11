#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/build/download-service"
ENV_FILE="$ROOT/.env"

USE_ENV=1
CHECK=0

die() { printf 'run.sh: %s\n' "$1" >&2; exit "${2:-1}"; }

usage() {
    sed -n '3,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-env) USE_ENV=0 ;;
        --check)  CHECK=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
    shift
done

if [ "$USE_ENV" -eq 1 ] && [ -f "$ENV_FILE" ]; then
    set -a
    # shellcheck disable=SC1090
    . "$ENV_FILE"
    set +a
    printf 'run.sh: loaded %s\n' "$ENV_FILE"
fi

if [ -z "${PORT-}" ]; then
    detail="missing required environment variable: PORT"
    if [ "$USE_ENV" -eq 0 ]; then
        die "$detail"$'\n'"  --no-env was given and PORT is not set"
    elif [ -f "$ENV_FILE" ]; then
        die "$detail"$'\n'"  $ENV_FILE does not define PORT"
    else
        die "$detail"$'\n'"  no .env at $ENV_FILE — cp .env.example .env and fill it in"
    fi
fi

# Build only when the binary is absent or older than any source, so a normal
# start does not pay for a full recompile.
if [ ! -x "$BIN" ] || [ -n "$(find "$ROOT" -maxdepth 1 -name '*.c' -newer "$BIN" 2>/dev/null)" ]; then
    printf 'run.sh: building...\n'
    make -C "$ROOT" >&2 || die "build failed" 2
fi

printf 'run.sh: starting on port %s\n' "$PORT"

if [ "$CHECK" -eq 1 ]; then
    "$BIN" &
    pid=$!
    trap 'kill -INT "$pid" 2>/dev/null || true' INT TERM

    # Poll the port instead of sleeping a fixed amount. Liveness is checked
    # *before* each probe so we never mistake another process for our service.
    ready=0
    for _ in $(seq 1 50); do
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        if curl -sS -o /dev/null --max-time 2 \
                "http://127.0.0.1:$PORT/health" 2>/dev/null; then
            ready=1
            break
        fi
        sleep 0.1
    done

    # A bind failure surfaces asynchronously: confirm the service is still alive.
    if [ "$ready" -eq 1 ] && ! kill -0 "$pid" 2>/dev/null; then
        ready=0
    fi

    if [ "$ready" -ne 1 ]; then
        rc=0
        wait "$pid" 2>/dev/null || rc=$?
        printf 'run.sh: service failed to become ready on port %s\n' "$PORT" >&2
        [ "$rc" -eq 0 ] && rc=1
        exit "$rc"
    fi

    printf 'run.sh: GET /health -> '
    curl -sS -o /dev/null -w '%{http_code}\n' \
        "http://127.0.0.1:$PORT/health" || printf '(request failed)\n'

    kill -INT "$pid" 2>/dev/null || true
    rc=0
    wait "$pid" 2>/dev/null || rc=$?
    exit "$rc"
fi

# Not exec'd: this script stays alive so Ctrl-C / signals are forwarded and the
# service stops cleanly.
"$BIN" &
pid=$!
trap 'kill -0 "$pid" 2>/dev/null && kill -INT "$pid" 2>/dev/null; true' INT TERM

rc=0
while :; do
    rc=0
    wait "$pid" || rc=$?
    kill -0 "$pid" 2>/dev/null || break
done

exit "$rc"
