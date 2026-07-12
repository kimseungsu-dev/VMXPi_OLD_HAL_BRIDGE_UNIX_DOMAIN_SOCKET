#!/usr/bin/env bash
set -Eeuo pipefail

INSTALL_DIR="/opt/vmxpi-bridge"

fail()
{
    echo "[FAIL] $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-dir)
            [[ $# -ge 2 ]] ||
                fail "Missing value for --install-dir."

            INSTALL_DIR="$2"
            shift 2
            ;;

        --help)
            echo "Usage: sudo ./uninstall.sh [--install-dir PATH]"
            exit 0
            ;;

        *)
            fail "Unknown option: $1"
            ;;
    esac
done

[[ "$EUID" -eq 0 ]] ||
    fail "This uninstaller must run as root."

systemctl disable --now \
    vmx-controller.service \
    2>/dev/null ||
true

rm -f \
    /etc/systemd/system/vmx-controller.service \
    /usr/local/bin/set_rtc_time \
    /usr/local/bin/vmx-healthcheck \
    /run/vmx-controller.sock

rm -rf "$INSTALL_DIR"

systemctl daemon-reload
systemctl reset-failed \
    vmx-controller.service \
    2>/dev/null ||
true

echo "VMX bridge uninstallation complete."
echo "The VMX-pi HAL package was not removed."
