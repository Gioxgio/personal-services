#!/usr/bin/env bash
#
# notify.sh - send a Telegram message through the notification service.
#
# The message comes from the arguments, or from stdin when no argument is
# given. The service must already be listening: start it with ./run.sh.
# The port is taken from $PORT, else from .env, else 8080.
#
# Usage:
#   ./notify.sh "disk almost full"
#   printf 'build ok\n' | ./notify.sh
#   ./notify.sh --port 9090 hello
#
# Exit status: 0 when the service answered 200, 1 on a usage or transport
# error, 2 when the service answered anything else.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="$ROOT/.env"

# json.h caps the message at MAX_TELEGRAM_MSG_LEN characters.
MAX_MSG_LEN=4096

HOST="127.0.0.1"
TIMEOUT=10

die() { printf 'notify.sh: %s\n' "$1" >&2; exit "${2:-1}"; }

usage() {
    sed -n '3,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# json.c understands only \" \\ \n \r \t \/ — notably not \uXXXX, so the other
# control characters are left raw for the service to handle.
json_escape() {
    local s=$1
    s=${s//\\/\\\\}
    s=${s//\"/\\\"}
    s=${s//$'\n'/\\n}
    s=${s//$'\r'/\\r}
    s=${s//$'\t'/\\t}
    printf '%s' "$s"
}

port="${PORT:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --port) [ $# -ge 2 ] || die "--port needs a value"; port=$2; shift 2 ;;
        --port=*) port=${1#--port=}; shift ;;
        --host) [ $# -ge 2 ] || die "--host needs a value"; HOST=$2; shift 2 ;;
        --host=*) HOST=${1#--host=}; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1" ;;
        *) break ;;
    esac
done

# Unquoted words are joined back together, so ./notify.sh disk full works.
if [ $# -gt 0 ]; then
    message=$*
elif [ -t 0 ]; then
    printf 'message: ' >&2
    IFS= read -r message || message=""
else
    message=$(cat)
fi

[ -n "$message" ] || die "refusing to send an empty message"
if [ "${#message}" -gt "$MAX_MSG_LEN" ]; then
    die "message is ${#message} characters, the service allows $MAX_MSG_LEN"
fi

if [ -z "$port" ] && [ -f "$ENV_FILE" ]; then
    # Read PORT out of the file instead of sourcing it, which would execute it.
    # An inline comment and surrounding quotes are stripped because neither is
    # part of the value.
    port=$(sed -n 's/^[[:space:]]*PORT[[:space:]]*=[[:space:]]*\(.*\)$/\1/p' "$ENV_FILE" \
           | tail -n 1 \
           | sed -e 's/[[:space:]]*#.*$//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    port=${port#\"}; port=${port%\"}
    port=${port#\'}; port=${port%\'}
    port=${port#\"}; port=${port%\"}
    port=${port#\'}; port=${port%\'}
fi
port=${port:-8080}

case "$port" in
    *[!0-9]*|'') die "PORT is not a number: $port" ;;
esac
if [ "$port" -lt 1 ] || [ "$port" -gt 65535 ]; then
    die "PORT out of range: $port"
fi

command -v curl >/dev/null 2>&1 || die "curl is not installed"

body_file=$(mktemp)
trap 'rm -f "$body_file"' EXIT

payload=$(printf '{"message": "%s"}' "$(json_escape "$message")")

# The service handles one connection at a time, so a slow or stuck request
# simply times out here rather than blocking behind other clients.
code=$(curl -sS -o "$body_file" -w '%{http_code}' \
           --max-time "$TIMEOUT" \
           -X POST \
           -H 'Content-Type: application/json' \
           --data-binary "$payload" \
           "http://$HOST:$port/notifications") \
    || die "no answer from http://$HOST:$port — is the service running? (./run.sh)"

printf 'HTTP %s\n' "$code"
if [ -s "$body_file" ]; then
    cat "$body_file"
    printf '\n'
fi

[ "$code" = 200 ] || exit 2
