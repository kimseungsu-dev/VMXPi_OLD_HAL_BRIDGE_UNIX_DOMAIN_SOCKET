#!/usr/bin/env bash

set -u

cd ~/OLD_HAL_BRIDGE_UNIX_DOMAIN_SOCKET

if [[ -S /run/vmx-controller.sock ]]; then
    ./vmx_client64 "SERVER_STOP" >/dev/null 2>&1 || true
    sleep 0.5
fi

rm -f /run/vmx-controller.sock
rm -f led_isolation.log
rm -f led_isolation_result.txt

exec > >(tee led_isolation_result.txt) 2>&1

ulimit -c unlimited 2>/dev/null || true

echo "===== SERVER START ====="

./vmx_controller32 > led_isolation.log 2>&1 &
server_pid=$!

echo "SERVER_PID=$server_pid"

sleep 2

if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "ERROR: Server exited during initialization."
    wait "$server_pid"
    echo "SERVER_EXIT=$?"
    tail -n 200 led_isolation.log
    exit 0
fi

echo
echo "===== FIND LED CHANNEL ====="

led_channel=""

for channel in $(seq 0 33); do
    output="$(
        ./vmx_client64 \
            "CHANNEL_INFO $channel" \
            "QUIT" 2>&1
    )"

    caps_hex="$(
        sed -n \
            's/.*CAPS=0x\([0-9A-Fa-f]\+\).*/\1/p' \
            <<<"$output" |
        head -n 1
    )"

    [[ -n "$caps_hex" ]] || continue

    caps=$((16#$caps_hex))

    if (( (caps & 0x00080000) != 0 )); then
        led_channel="$channel"
        echo "LED_CHANNEL=$led_channel CAPS=0x$caps_hex"
        break
    fi
done

if [[ -z "$led_channel" ]]; then
    echo "ERROR: No LED array capable channel found."

    ./vmx_client64 "SERVER_STOP" >/dev/null 2>&1 || true
    wait "$server_pid"
    tail -n 200 led_isolation.log
    exit 0
fi

run_step()
{
    local title="$1"
    local command="$2"
    local output
    local client_status
    local server_status

    echo
    echo "===== $title ====="
    echo "COMMAND=$command"

    output="$(
        ./vmx_client64 \
            "$command" \
            "QUIT" 2>&1
    )"

    client_status=$?

    printf '%s\n' "$output"
    echo "CLIENT_EXIT=$client_status"

    sleep 0.3

    if kill -0 "$server_pid" 2>/dev/null; then
        echo "SERVER=ALIVE"
        return 0
    fi

    wait "$server_pid"
    server_status=$?

    echo "SERVER=DEAD"
    echo "SERVER_EXIT=$server_status"

    echo
    echo "===== SERVER LOG ====="
    tail -n 250 led_isolation.log

    echo
    echo "===== KERNEL CRASH MESSAGE ====="
    dmesg -T 2>/dev/null |
        grep -E \
            'vmx_controller32|segfault|trap|abort|core dumped' |
        tail -n 30 ||
        true

    if command -v coredumpctl >/dev/null 2>&1; then
        echo
        echo "===== COREDUMP INFO ====="

        coredumpctl \
            --no-pager \
            info vmx_controller32 2>/dev/null |
            tail -n 120 ||
            true
    fi

    exit 0
}

run_step \
    "01 OPEN" \
    "LED_ARRAY_OPEN $led_channel 4 GRB"

run_step \
    "02 SET PIXEL" \
    "LED_ARRAY_SET_PIXEL $led_channel 0 1 2 3"

run_step \
    "03 GET PIXEL" \
    "LED_ARRAY_GET_PIXEL $led_channel 0"

run_step \
    "04 FILL" \
    "LED_ARRAY_FILL $led_channel 4 5 6"

run_step \
    "05 GET FILLED PIXEL" \
    "LED_ARRAY_GET_PIXEL $led_channel 3"

run_step \
    "06 SHOW" \
    "LED_ARRAY_SHOW $led_channel"

run_step \
    "07 CLEAR" \
    "LED_ARRAY_CLEAR $led_channel"

run_step \
    "08 RELEASE" \
    "LED_ARRAY_RELEASE $led_channel"

echo
echo "===== CLEAN STOP ====="

./vmx_client64 "SERVER_STOP" 2>&1 || true
wait "$server_pid"
echo "SERVER_EXIT=$?"

echo
echo "===== SERVER LOG ====="
tail -n 250 led_isolation.log
