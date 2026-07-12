#!/usr/bin/env bash
set -Eeuo pipefail

SERVICE="vmx-controller.service"

restore_service()
{
    set +e
    systemctl start "$SERVICE" >/dev/null 2>&1
}

trap restore_service EXIT INT TERM

echo "[1/4] Stopping systemd service"

systemctl stop "$SERVICE"

for _ in $(seq 1 200); do
    if ! systemctl is-active --quiet "$SERVICE"; then
        break
    fi

    sleep 0.1
done

systemctl is-active --quiet "$SERVICE" && {
    echo "[FAIL] Service did not stop." >&2
    exit 1
}

if pgrep -f \
    '^/root/OLD_HAL_BRIDGE_UNIX_DOMAIN_SOCKET/vmx_controller32$' \
    >/dev/null; then
    echo "[FAIL] Controller process remains after service stop." >&2
    exit 1
fi

echo "[2/4] Running bridge release check"

make release-check

echo "[3/4] Running systemd recovery check"

make service-test

echo "[4/4] Running operational healthcheck"

./healthcheck.sh

trap - EXIT INT TERM

echo
echo "========================================"
echo "PASS: VMX controller final audit"
echo "========================================"
echo "BRIDGE TEST    : PASS"
echo "SAFE STOP      : PASS"
echo "WATCHDOG       : PASS"
echo "RESOURCE CLEAN : PASS"
echo "SYSTEMD        : PASS"
echo "CRASH RECOVERY : PASS"
echo "HEALTHCHECK    : PASS"
