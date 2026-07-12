#!/usr/bin/env bash
set -u

SCRIPT_PATH="$(readlink -f -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(
    cd -- "$(dirname -- "$SCRIPT_PATH")" &&
    pwd
)"

PID="${1:-}"
SOCKET="/run/vmx-controller.sock"
CLIENT="$SCRIPT_DIR/vmx_client64"

pid_running()
{
    [[ "$PID" =~ ^[0-9]+$ ]] &&
    [[ "$PID" -gt 1 ]] &&
    kill -0 "$PID" 2>/dev/null
}

if ! pid_running; then
    exit 0
fi

stop_sent=0

for _ in $(seq 1 100); do
    if ! pid_running; then
        exit 0
    fi

    if [[ -S "$SOCKET" ]]; then
        if timeout 3s \
            "$CLIENT" \
            SERVER_STOP \
            >/dev/null 2>&1; then
            stop_sent=1
            break
        fi
    fi

    sleep 0.1
done

if [[ "$stop_sent" -ne 1 ]]; then
    echo "Unable to deliver SERVER_STOP." >&2
    exit 1
fi

for _ in $(seq 1 180); do
    if ! pid_running; then
        exit 0
    fi

    sleep 0.1
done

echo "Controller did not exit after SERVER_STOP." >&2
exit 1
