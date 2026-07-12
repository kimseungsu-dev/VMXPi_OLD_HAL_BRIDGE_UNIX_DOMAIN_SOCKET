#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_PATH="$(readlink -f -- "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(
    cd -- "$(dirname -- "$SCRIPT_PATH")" &&
    pwd
)"

SERVICE="vmx-controller.service"
SOCKET="/run/vmx-controller.sock"
CLIENT="$SCRIPT_DIR/vmx_client64"

fail()
{
    echo "[FAIL] $*" >&2
    exit 1
}

systemctl is-enabled --quiet "$SERVICE" ||
    fail "Service is not enabled."

systemctl is-active --quiet "$SERVICE" ||
    fail "Service is not active."

[[ -S "$SOCKET" ]] ||
    fail "Controller socket is not available."

[[ -x "$CLIENT" ]] ||
    fail "ARM64 client executable is not available."

output="$(
    timeout 5s "$CLIENT" \
        WATCHDOG_STATUS \
        PING \
        STATUS \
        QUIT
)" || fail "Controller request failed."

grep -Eq \
    'OK WATCHDOG ENABLED=1 EXPIRED=[01] TIMEOUT_MS=1000 FLEXDIO=1 HICURRDIO=1 COMMDIO=[01]' \
    <<<"$output" ||
    fail "Watchdog status is invalid."

grep -Fq "OK PONG" <<<"$output" ||
    fail "Controller did not respond to PING."

grep -Fq \
    "OK DIO_OUT=0 DIO_IN=0 PWM=0 ANALOG=0 ENCODER=0 INPUT_CAPTURE=0 PWM_CAPTURE=0 ANALOG_TRIGGER=0 UART=0 SPI=0 I2C=0 LED_ARRAY=0 TOTAL=0" \
    <<<"$output" ||
    fail "Controller resources are not idle."

grep -Fq "OK BYE" <<<"$output" ||
    fail "Client session did not close cleanly."

echo "VMX controller healthcheck: PASS"
