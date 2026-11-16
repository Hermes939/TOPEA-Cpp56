#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE_DIR="$ROOT_DIR/top4ea_cpp"
RETRY_DELAY_SECONDS=5

if ! make -C "$ENGINE_DIR" loader; then
    echo "[Supervisor] Build failed; engine will not start." >&2
    exit 1
fi

stopping=0
child_pid=""

stop_supervisor() {
    stopping=1
    if [[ -n "$child_pid" ]]; then
        kill -TERM "$child_pid" 2>/dev/null || true
    fi
}

trap stop_supervisor INT TERM

while (( stopping == 0 )); do
    echo "[Supervisor] Starting TOP4EA..."
    "$ENGINE_DIR/build/top4ea" "$ENGINE_DIR/top4ea.conf" &
    child_pid=$!
    wait "$child_pid"
    exit_code=$?

    if (( stopping )); then
        kill -TERM "$child_pid" 2>/dev/null || true
        wait "$child_pid" 2>/dev/null || true
        break
    fi

    child_pid=""
    echo "[Supervisor] TOP4EA exited with status $exit_code; retrying in ${RETRY_DELAY_SECONDS}s."
    sleep "$RETRY_DELAY_SECONDS"
done
