#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/build/notification-service"
ENV_FILE="$ROOT/.env"

USE_ENV=1
CHECK=0

die() { printf 'run.sh: %s\n' "$1" >&2; exit "${2:-1}"; }

usage() {
    sed -n '3,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# Removes everything this script created in /tmp. Safe to call when only some
# of the files exist (set -u is on, hence the :- defaults).
cleanup() {
    [ -n "${monitor_pid-}" ] && kill "$monitor_pid" 2>/dev/null
    rm -f "${snapshot-}" "${samples-}"
    return 0
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
    snapshot="$(mktemp)"
    trap cleanup EXIT
    env > "$snapshot"

    set -a
    # shellcheck disable=SC1090
    . "$ENV_FILE"
    set +a

    while IFS= read -r line; do
        case "$line" in *=*) ;; *) continue ;; esac
        name="${line%%=*}"
        # Skip non-identifier names (BASH_FUNC_x%%=() { ... } and friends).
        case "$name" in
            [A-Za-z_]*) ;;
            *) continue ;;
        esac
        case "$name" in
            *[!A-Za-z0-9_]*) continue ;;
        esac
        if [ "${!name-}" != "${line#*=}" ]; then
            export "$line"
        fi
    done < "$snapshot"

    printf 'run.sh: loaded %s\n' "$ENV_FILE"
fi

# Fail early with a clear message rather than letting main.c print its usage.
missing=""
for var in PORT TELEGRAM_TOKEN TELEGRAM_CHAT_ID; do
    if [ -z "${!var:-}" ]; then
        missing="$missing $var"
    fi
done

if [ -n "$missing" ]; then
    detail="missing required environment variable(s):$missing"
    if [ "$USE_ENV" -eq 0 ]; then
        die "$detail"$'\n'"  --no-env was given and the environment does not set them"
    elif [ -f "$ENV_FILE" ]; then
        die "$detail"$'\n'"  $ENV_FILE does not define them"
    else
        die "$detail"$'\n'"  no .env at $ENV_FILE — cp .env.example .env and fill it in"
    fi
fi

# ---------------------------------------------------------------------------
# Resource accounting
#
# The service is started as a child (not exec'd) so this script can stay alive
# to sample it. CPU comes from utime+stime in /proc/<pid>/stat (in clock ticks),
# memory from VmRSS / VmHWM in /proc/<pid>/status. VmHWM is the kernel's own
# high-water mark, so the memory peak is exact even between two samples.
# ---------------------------------------------------------------------------
CLK_TCK="$(getconf CLK_TCK)"

# Reads one sample: "<ns> <cpu_ticks> <rss_kb> <hwm_kb>". Field 2 is read
# positionally after the last ')' so a process name containing spaces or
# parentheses cannot shift the utime/stime columns.
read_sample() {
    local pid=$1 ticks
    ticks="$(awk '{ i = index($0, ")"); split(substr($0, i + 2), a, " ")
                    print a[12] + a[13] }' "/proc/$pid/stat" 2>/dev/null)" || return 1
    [ -n "$ticks" ] || return 1
    awk -v ns="$(date +%s%N)" -v c="$ticks" '
        /^VmRSS:/ { rss = $2 }
        /^VmHWM:/ { hwm = $2 }
        END       { printf "%s %s %s %s\n", ns, c, rss + 0, hwm + 0 }
    ' "/proc/$pid/status" 2>/dev/null
}

# Samples until $1 is gone, appending to $2.
sample_loop() {
    local pid=$1 out=$2
    while :; do
        read_sample "$pid" >> "$out" || break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
    done
}

# Prints peak/avg CPU and RSS over the samples in $1.
report() {
    awk -v clk="$CLK_TCK" -v iv=0.25 '
        NR == 1 { pt = $1; pc = $2; t0 = $1; c0 = $2 }
        {
            if (NR > 1) {
                dt = ($1 - pt) / 1e9
                dc = ($2 - pc) / clk
                if (dt > 1e-6) { p = dc / dt * 100; if (p > peak) peak = p }
                wall += dt; cput += dc
            }
            if ($3 > 0) { rsum += $3; rn++ }
            if ($4 > hwm) hwm = $4
            pt = $1; pc = $2; n++
        }
        END {
            if (n == 0) { print "run.sh: service exited before it could be sampled"; exit }
            if (n < 2) { print "run.sh: not enough samples for a report"; exit }
            dur = (pt - t0) / 1e9
            printf "run.sh: resource usage over %dm%06.3fs (%d samples)\n",
                   int(dur / 60), dur - int(dur / 60) * 60, n
            avg = wall > 0 ? cput / wall * 100 : 0
            printf "  cpu     peak %6.1f%%   avg %6.1f%%   (%d samples every %gs)\n",
                   peak, avg, n, iv
            printf "  memory  peak %6.1f MiB avg %6.1f MiB\n",
                   hwm / 1024, (rn > 0 ? rsum / rn : 0) / 1024
            printf "  total   %s cpu over %s wall\n",
                   sprintf("%.2fs", (pc - c0) / clk), sprintf("%.2fs", dur)
        }
    ' "$1"
}

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

    # Poll the port instead of sleeping a fixed amount, so this stays quick
    # without being racy on a loaded machine. Liveness is checked *before* each
    # probe: if the service died during startup (port already in use, say),
    # whatever is answering on that port is some other process and must not be
    # mistaken for a successful check.
    ready=0
    for _ in $(seq 1 50); do
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        if curl -sS -o /dev/null --max-time 2 \
                "http://127.0.0.1:$PORT/notifications" 2>/dev/null; then
            ready=1
            break
        fi
        sleep 0.1
    done

    # A bind failure surfaces asynchronously, so the service can die between the
    # liveness check and the probe. Confirm it once more before trusting the
    # probe result.
    if [ "$ready" -eq 1 ] && ! kill -0 "$pid" 2>/dev/null; then
        ready=0
    fi

    if [ "$ready" -ne 1 ]; then
        rc=0
        wait "$pid" 2>/dev/null || rc=$?
        printf 'run.sh: service failed to become ready on port %s\n' "$PORT" >&2
        # Never report success here: a zero status from an early exit is still
        # a failure to start.
        [ "$rc" -eq 0 ] && rc=1
        exit "$rc"
    fi

    printf '\nrun.sh: POST /notifications -> '
    curl -sS -o /dev/null -w '%{http_code}\n' \
        -X POST -H 'Content-Type: application/json' \
        -d '{"message":"run.sh check"}' \
        "http://127.0.0.1:$PORT/notifications" || printf '(request failed)\n'

    kill -INT "$pid" 2>/dev/null || true
    rc=0
    wait "$pid" 2>/dev/null || rc=$?
    exit "$rc"
fi

# Not exec'd: this script has to outlive the service to sample it and print the
# summary afterwards. INT/TERM are forwarded so Ctrl-C still stops it cleanly;
# the terminal signals the child directly too, hence the kill -0 guard.
"$BIN" &
pid=$!
trap 'kill -0 "$pid" 2>/dev/null && kill -INT "$pid" 2>/dev/null; true' INT TERM

samples="$(mktemp)"
trap cleanup EXIT
sample_loop "$pid" "$samples" &
monitor_pid=$!

# A trapped signal makes wait return early with 128+N while the child is still
# shutting down, so keep waiting. rc is reset on every pass: only the wait that
# actually reaps the service decides the exit status.
rc=0
while :; do
    rc=0
    wait "$pid" || rc=$?
    kill -0 "$pid" 2>/dev/null || break
done

wait "$monitor_pid" 2>/dev/null || true
monitor_pid=
report "$samples"
exit "$rc"
