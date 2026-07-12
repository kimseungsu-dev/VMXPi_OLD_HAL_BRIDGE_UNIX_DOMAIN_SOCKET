#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &&
    pwd
)"

INSTALL_DIR="/opt/vmxpi-bridge"
CPU_AFFINITY=""
TIMEZONE="Asia/Seoul"
HAL_DEB=""
HAL_URL="https://archive.org/download/vmxpi-hal_1.1.257_armhf/vmxpi-hal_1.1.257_armhf.deb"
HAL_SHA256="276b608248545d2c244e512650291ca5b36da16741aada7892e45a641697cc45"
SKIP_DEPS=0
SKIP_BUILD=0
NO_START=0

usage()
{
    cat <<'USAGE'
Usage:
  sudo ./install.sh [options]

Options:
  --hal-deb PATH       Use a local VMX-pi ARMHF HAL package
  --hal-url URL        Override the HAL download URL
  --install-dir PATH   Runtime installation directory
  --cpu NUMBER         CPU affinity for the VMX HAL
  --timezone ZONE      Linux timezone
  --skip-deps          Do not install system packages
  --skip-build         Use existing binaries
  --no-start           Install without starting the service
  --help               Show this help
USAGE
}

fail()
{
    echo "[FAIL] $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hal-deb)
            [[ $# -ge 2 ]] ||
                fail "Missing value for --hal-deb."

            HAL_DEB="$2"
            shift 2
            ;;

        --hal-url)
            [[ $# -ge 2 ]] ||
                fail "Missing value for --hal-url."

            HAL_URL="$2"
            shift 2
            ;;

        --install-dir)
            [[ $# -ge 2 ]] ||
                fail "Missing value for --install-dir."

            INSTALL_DIR="$2"
            shift 2
            ;;

        --cpu)
            [[ $# -ge 2 ]] ||
                fail "Missing value for --cpu."

            CPU_AFFINITY="$2"
            shift 2
            ;;

        --timezone)
            [[ $# -ge 2 ]] ||
                fail "Missing value for --timezone."

            TIMEZONE="$2"
            shift 2
            ;;

        --skip-deps)
            SKIP_DEPS=1
            shift
            ;;

        --skip-build)
            SKIP_BUILD=1
            shift
            ;;

        --no-start)
            NO_START=1
            shift
            ;;

        --help)
            usage
            exit 0
            ;;

        *)
            fail "Unknown option: $1"
            ;;
    esac
done

[[ "$EUID" -eq 0 ]] ||
    fail "This installer must run as root."

case "$(uname -m)" in
    aarch64|arm64)
        ;;
    *)
        fail "This installer requires an ARM64 Linux system."
        ;;
esac

CPU_COUNT="$(nproc)"

if [[ -z "$CPU_AFFINITY" ]]; then
    if [[ "$CPU_COUNT" -ge 4 ]]; then
        CPU_AFFINITY=3
    else
        CPU_AFFINITY="$((CPU_COUNT - 1))"
    fi
fi

[[ "$CPU_AFFINITY" =~ ^[0-9]+$ ]] ||
    fail "CPU affinity must be an integer."

[[ "$CPU_AFFINITY" -lt "$CPU_COUNT" ]] ||
    fail "CPU $CPU_AFFINITY does not exist."

[[ "$INSTALL_DIR" = /* ]] ||
    fail "Install directory must be an absolute path."

[[ "$INSTALL_DIR" != *"|"* ]] ||
    fail "Install directory contains an unsupported character."

echo "[1/8] Checking system dependencies"

if [[ "$SKIP_DEPS" -eq 0 ]]; then
    if ! dpkg --print-foreign-architectures |
        grep -Fxq armhf; then
        dpkg --add-architecture armhf
    fi

    apt-get update

    DEBIAN_FRONTEND=noninteractive \
        apt-get install -y \
        build-essential \
        ca-certificates \
        curl \
        file \
        git \
        g++-arm-linux-gnueabihf \
        libc6:armhf \
        libstdc++6:armhf \
        libgcc-s1:armhf \
        libatomic1:armhf
fi

echo "[2/8] Checking VMX-pi HAL"

HAL_LIBRARY="/usr/local/lib/vmxpi/libvmxpi_hal_cpp.so"
HAL_HEADER="/usr/local/include/vmxpi/VMXPi.h"

if [[ ! -f "$HAL_LIBRARY" ||
      ! -f "$HAL_HEADER" ]]; then

    if [[ -z "$HAL_DEB" ]]; then
        shopt -s nullglob

        candidates=(
            "$ROOT_DIR"/vmxpi-hal_*_armhf.deb
            "$ROOT_DIR"/../vmxpi-hal_*_armhf.deb
        )

        shopt -u nullglob

        if [[ "${#candidates[@]}" -eq 1 ]]; then
            HAL_DEB="${candidates[0]}"
            echo "Using local HAL package: $HAL_DEB"
        elif [[ "${#candidates[@]}" -gt 1 ]]; then
            fail "Multiple local HAL packages were found. Use --hal-deb PATH."
        fi
    fi

    if [[ -z "$HAL_DEB" ]]; then
        command -v curl >/dev/null 2>&1 ||
            fail "curl is required to download the VMX-pi HAL."

        [[ -n "$HAL_URL" ]] ||
            fail "HAL download URL is empty."

        HAL_TEMP_DIR="$(mktemp -d)"
        HAL_DEB="$HAL_TEMP_DIR/vmxpi-hal_1.1.257_armhf.deb"

        cleanup_hal_download()
        {
            rm -rf "$HAL_TEMP_DIR"
        }

        trap cleanup_hal_download EXIT

        echo "Downloading VMX-pi HAL:"
        echo "  $HAL_URL"

        curl             --fail             --location             --show-error             --silent             --retry 3             --retry-delay 2             --connect-timeout 20             --output "$HAL_DEB"             "$HAL_URL" ||
            fail "VMX-pi HAL download failed."
    fi

    [[ -f "$HAL_DEB" ]] ||
        fail "HAL package not found: $HAL_DEB"

    package_name="$(
        dpkg-deb -f "$HAL_DEB" Package 2>/dev/null ||
        true
    )"

    package_version="$(
        dpkg-deb -f "$HAL_DEB" Version 2>/dev/null ||
        true
    )"

    package_architecture="$(
        dpkg-deb -f "$HAL_DEB" Architecture 2>/dev/null ||
        true
    )"

    [[ "$package_name" == "vmxpi-hal" ]] ||
        fail "Unexpected HAL package name: $package_name"

    [[ "$package_version" == "1.1.257" ]] ||
        fail "Unexpected HAL package version: $package_version"

    [[ "$package_architecture" == "armhf" ]] ||
        fail "Unexpected HAL package architecture: $package_architecture"

    echo "HAL package validation:"
    echo "  Package      : $package_name"
    echo "  Version      : $package_version"
    echo "  Architecture : $package_architecture"
    actual_sha256="$(
        sha256sum "$HAL_DEB" |
        awk '{print $1}'
    )"

    [[ "$actual_sha256" == "$HAL_SHA256" ]] ||
        fail "HAL package SHA256 mismatch: $actual_sha256"

    echo "  SHA256       : $actual_sha256"

    dpkg -i "$HAL_DEB" || {
        if [[ "$SKIP_DEPS" -eq 0 ]]; then
            apt-get -f install -y
            dpkg -i "$HAL_DEB"
        else
            fail "HAL package installation failed."
        fi
    }
fi

[[ -f "$HAL_LIBRARY" ]] ||
    fail "VMX HAL library was not installed."

[[ -f "$HAL_HEADER" ]] ||
    fail "VMX HAL headers were not installed."

echo "[3/8] Building bridge binaries"

cd "$ROOT_DIR"

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    make clean
    make
    make client64
fi

[[ -x vmx_controller32 ]] ||
    fail "vmx_controller32 is missing."

[[ -x vmx_client64 ]] ||
    fail "vmx_client64 is missing."

file vmx_controller32 |
    grep -Eq 'ELF 32-bit.*ARM' ||
    fail "vmx_controller32 has an invalid architecture."

file vmx_client64 |
    grep -Eq 'ELF 64-bit.*ARM aarch64' ||
    fail "vmx_client64 has an invalid architecture."

echo "[4/8] Stopping existing service"

systemctl stop vmx-controller.service \
    2>/dev/null ||
true

echo "[5/8] Installing runtime files"

install -d -m 0755 "$INSTALL_DIR"

install -m 0755 \
    vmx_controller32 \
    vmx_client64 \
    vmx-controller-stop.sh \
    set_rtc_time \
    healthcheck.sh \
    "$INSTALL_DIR/"

for document in \
    README.md \
    PROTOCOL.md \
    THIRD_PARTY_NOTICES.md
do
    if [[ -f "$document" ]]; then
        install -m 0644 \
            "$document" \
            "$INSTALL_DIR/"
    fi
done

sed \
    -e "s|@INSTALL_DIR@|$INSTALL_DIR|g" \
    -e "s|@CPU_AFFINITY@|$CPU_AFFINITY|g" \
    vmx-controller.service.in \
    > /etc/systemd/system/vmx-controller.service

chmod 0644 \
    /etc/systemd/system/vmx-controller.service

ln -sfn \
    "$INSTALL_DIR/set_rtc_time" \
    /usr/local/bin/set_rtc_time

ln -sfn \
    "$INSTALL_DIR/healthcheck.sh" \
    /usr/local/bin/vmx-healthcheck

echo "[6/8] Applying system settings"

if command -v timedatectl >/dev/null 2>&1; then
    timedatectl set-timezone "$TIMEZONE"
fi

systemctl daemon-reload
systemctl reset-failed vmx-controller.service \
    2>/dev/null ||
true

systemctl enable vmx-controller.service

if [[ "$NO_START" -eq 1 ]]; then
    echo
    echo "VMX bridge installation complete."
    echo "Service start was skipped."
    exit 0
fi

echo "[7/8] Starting VMX controller"

systemctl restart vmx-controller.service

ready=0

for _ in $(seq 1 300); do
    output="$(
        timeout 3s \
            "$INSTALL_DIR/vmx_client64" \
            PING \
            QUIT \
            2>/dev/null ||
        true
    )"

    if grep -Fq "OK PONG" <<<"$output"; then
        ready=1
        break
    fi

    sleep 0.1
done

[[ "$ready" -eq 1 ]] ||
    fail "VMX controller did not become ready."

echo "[8/8] Running healthcheck"

"$INSTALL_DIR/healthcheck.sh"

echo
echo "========================================"
echo "VMX bridge installation complete"
echo "========================================"
echo "Install directory : $INSTALL_DIR"
echo "CPU affinity      : $CPU_AFFINITY"
echo "Timezone          : $TIMEZONE"
echo "Service           : vmx-controller.service"
