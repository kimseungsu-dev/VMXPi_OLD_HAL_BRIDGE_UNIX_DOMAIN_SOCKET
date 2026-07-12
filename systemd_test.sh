#!/usr/bin/env bash
set -Eeuo pipefail

SERVICE="vmx-controller.service"
SOCKET="/run/vmx-controller.sock"
INSTALL_DIR="${VMX_INSTALL_DIR:-/opt/vmxpi-bridge}"
CLIENT="$INSTALL_DIR/vmx_client64"
STOP_HELPER="$INSTALL_DIR/vmx-controller-stop.sh"
TEST_START="$(date --iso-8601=seconds)"

TEST_CURSOR="$(
    journalctl \
        -u "$SERVICE" \
        -n 0 \
        --show-cursor \
        --no-pager \
        2>/dev/null |
    sed -n 's/^-- cursor: //p' |
    tail -n 1
)"

read_test_journal()
{
    if [[ -n "$TEST_CURSOR" ]]; then
        journalctl \
            -u "$SERVICE" \
            --after-cursor "$TEST_CURSOR" \
            --no-pager
    else
        journalctl \
            -u "$SERVICE" \
            --since "$TEST_START" \
            --no-pager
    fi
}

fail()
{
    echo
    echo "[FAIL] $*" >&2

    systemctl \
        --no-pager \
        --full \
        status "$SERVICE" >&2 ||
        true

    read_test_journal |
        tail -n 160 >&2 ||
        true

    exit 1
}

controller_probe()
{
    local output

    output="$(
        timeout 3s "$CLIENT" \
            PING \
            QUIT \
            2>/dev/null
    )" || return 1

    grep -Fq "OK PONG" <<<"$output"
}

wait_ready()
{
    local previous_pid="${1:-}"
    local pid=""

    for _ in $(seq 1 300); do
        pid="$(
            systemctl show \
                --property MainPID \
                --value \
                "$SERVICE"
        )"

        if systemctl is-active --quiet "$SERVICE" &&
           [[ "$pid" =~ ^[0-9]+$ ]] &&
           [[ "$pid" != "0" ]] &&
           [[ -S "$SOCKET" ]] &&
           {
               [[ -z "$previous_pid" ]] ||
               [[ "$pid" != "$previous_pid" ]]
           } &&
           controller_probe; then
            printf '%s\n' "$pid"
            return 0
        fi

        sleep 0.1
    done

    fail "Controller did not become ready."
}

verify_controller()
{
    local output

    output="$(
        timeout 5s "$CLIENT" \
            WATCHDOG_STATUS \
            PING \
            STATUS \
            QUIT
    )" || fail "Controller verification failed."

    grep -Eq \
        'OK WATCHDOG ENABLED=1 EXPIRED=[01] TIMEOUT_MS=1000 FLEXDIO=1 HICURRDIO=1 COMMDIO=[01]' \
        <<<"$output" ||
        fail "Watchdog verification failed."

    grep -Fq "OK PONG" <<<"$output" ||
        fail "PING verification failed."

    grep -Fq \
        "OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0 TOTAL=0" \
        <<<"$output" ||
        fail "Resource state verification failed."

    grep -Fq "OK BYE" <<<"$output" ||
        fail "Client close verification failed."
}

echo "[1/6] Preflight checks"

[[ "$EUID" -eq 0 ]] ||
    fail "This test must run as root."

[[ -x "$CLIENT" ]] ||
    fail "ARM64 client executable not found."

[[ -x "$STOP_HELPER" ]] ||
    fail "Stop helper is not executable."

[[ -f /etc/systemd/system/vmx-controller.service ]] ||
    fail "systemd unit is not installed."

echo "[2/6] Initial service start"

systemctl daemon-reload
systemctl enable "$SERVICE" >/dev/null
systemctl restart "$SERVICE"

initial_pid="$(wait_ready)"
verify_controller

echo "[3/6] Graceful restart"

systemctl restart "$SERVICE"
restart_pid="$(wait_ready "$initial_pid")"

[[ "$restart_pid" != "$initial_pid" ]] ||
    fail "Service PID did not change after restart."

verify_controller

echo "[4/6] Crash recovery"

old_pid="$restart_pid"
kill -KILL "$old_pid"

recovered_pid="$(wait_ready "$old_pid")"

[[ "$recovered_pid" != "$old_pid" ]] ||
    fail "Service did not restart after SIGKILL."

verify_controller

echo "Waiting for HAL background threads to stabilize."

sleep 5

current_pid="$(
    systemctl show \
        --property MainPID \
        --value \
        "$SERVICE"
)"

[[ "$current_pid" == "$recovered_pid" ]] ||
    fail "Controller restarted during the stabilization period."

verify_controller

echo "[5/6] Stale socket recovery"

systemctl stop "$SERVICE"

for _ in $(seq 1 200); do
    if ! systemctl is-active --quiet "$SERVICE"; then
        break
    fi

    sleep 0.1
done

systemctl is-active --quiet "$SERVICE" &&
    fail "Service did not stop."

rm -f "$SOCKET"
printf 'stale\n' > "$SOCKET"

systemctl start "$SERVICE"
wait_ready >/dev/null

[[ -S "$SOCKET" ]] ||
    fail "Stale socket was not replaced."

verify_controller

echo "[6/6] Final operational state"

journal="$(read_test_journal)"

if grep -Eqi \
    'status=11/SEGV|segmentation fault|double free|core dumped|terminate called|pigpio uninitialised|Error closing SPI AUX' \
    <<<"$journal"; then
    fail "Unexpected shutdown failure found in the journal."
fi

systemctl is-enabled --quiet "$SERVICE" ||
    fail "Service is not enabled at boot."

systemctl is-active --quiet "$SERVICE" ||
    fail "Service is not active."

echo
echo "========================================"
echo "PASS: VMX controller systemd test"
echo "========================================"
echo "BOOT ENABLE   : PASS"
echo "GRACEFUL STOP : PASS"
echo "RESTART       : PASS"
echo "CRASH RECOVERY: PASS"
echo "STALE SOCKET  : PASS"
echo "CONTROLLER    : PASS"
