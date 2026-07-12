#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &&
    pwd
)"

cd "$ROOT_DIR"

fail()
{
    echo "[FAIL] $*" >&2
    exit 1
}

required_files=(
    .gitattributes
    .gitignore
    LICENSE
    CHANGELOG.md
    VERSION
    Makefile
    README.md
    README_KO.md
    PROTOCOL.md
    THIRD_PARTY_NOTICES.md
    main.cpp
    vmx_client64.cpp
    install.sh
    uninstall.sh
    vmx-controller-stop.sh
    vmx-controller.service.in
    set_rtc_time
    smoke_test.sh
    systemd_test.sh
    healthcheck.sh
    final_audit.sh
    repo_check.sh
)

scripts=(
    install.sh
    uninstall.sh
    vmx-controller-stop.sh
    set_rtc_time
    smoke_test.sh
    systemd_test.sh
    healthcheck.sh
    final_audit.sh
    repo_check.sh
)

echo "[1/5] Checking repository files"

for file_name in "${required_files[@]}"; do
    [[ -f "$file_name" ]] ||
        fail "Required file is missing: $file_name"
done

echo "[2/5] Checking shell syntax"

for script_name in "${scripts[@]}"; do
    bash -n "$script_name" ||
        fail "Shell syntax check failed: $script_name"
done

echo "[3/5] Checking service template"

rendered_service="$(
    mktemp /tmp/vmx-controller-check.XXXXXX.service
)"

trap 'rm -f "$rendered_service"' EXIT

sed \
    -e 's|@INSTALL_DIR@|/opt/vmxpi-bridge|g' \
    -e 's|@CPU_AFFINITY@|3|g' \
    vmx-controller.service.in \
    > "$rendered_service"

grep -Fq \
    "WorkingDirectory=/opt/vmxpi-bridge" \
    "$rendered_service" ||
    fail "Install directory substitution failed."

grep -Fq \
    "CPUAffinity=3" \
    "$rendered_service" ||
    fail "CPU affinity substitution failed."

if grep -Fq '@INSTALL_DIR@' "$rendered_service" ||
   grep -Fq '@CPU_AFFINITY@' "$rendered_service"; then
    fail "Unresolved service template variable found."
fi

if [[ -x /opt/vmxpi-bridge/vmx_controller32 &&
      -x /opt/vmxpi-bridge/vmx-controller-stop.sh ]]; then
    systemd-analyze verify "$rendered_service"
fi

echo "[4/5] Checking repository content"

if LC_ALL=C.UTF-8 grep -nP '[\x{AC00}-\x{D7A3}]' \
    install.sh \
    uninstall.sh \
    vmx-controller-stop.sh \
    set_rtc_time \
    healthcheck.sh \
    systemd_test.sh \
    final_audit.sh \
    repo_check.sh \
    vmx-controller.service.in \
    Makefile; then
    fail "Korean text was found in runtime files."
fi

for ignore_pattern in \
    vmx_controller32 \
    vmx_client64 \
    '*.deb' \
    '*.log' \
    '*.zip' \
    '*_context.txt' \
    vmx-controller.service
do
    grep -Fxq "$ignore_pattern" .gitignore ||
        fail "Missing .gitignore pattern: $ignore_pattern"
done

grep -Fq \
    '276b608248545d2c244e512650291ca5b36da16741aada7892e45a641697cc45' \
    install.sh ||
    fail "Pinned HAL checksum was not found."

echo "[5/5] Checking Git state"

if git rev-parse \
    --is-inside-work-tree \
    >/dev/null 2>&1; then

    git diff --check
    git diff --cached --check

    for artifact in \
        vmx_controller32 \
        vmx_client64 \
        smoke_server.log
    do
        if git ls-files \
            --error-unmatch \
            "$artifact" \
            >/dev/null 2>&1; then
            fail "Generated artifact is tracked: $artifact"
        fi
    done
fi

echo
echo "========================================"
echo "PASS: Repository check"
echo "========================================"
echo "FILES           : PASS"
echo "SHELL SYNTAX    : PASS"
echo "SERVICE TEMPLATE: PASS"
echo "GIT IGNORE      : PASS"
echo "CHECKSUM        : PASS"
echo "GIT STATE       : PASS"
