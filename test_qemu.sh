#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# ==========================================================================
#  test_qemu.sh -- OS-neutral QEMU smoke test for BOOTX64.EFI
#
#  Verifies that the loader hands off to the hosted 32-bit inner firmware.
#  It makes NO assumption about which EFI OS is (or is not) installed: the
#  inner firmware's BDS owns boot policy.  An optional OS tree can be added
#  to the ESP to check that the loader leaves it alone.
#
#  Usage:
#    ./test_qemu.sh [-M pc|q35] [-m MiB] [--os-tree DIR] [--timeout SEC]
#                   [--keep] [--tcg|--kvm]
#
#  Checks:
#    * toolchain + QEMU + mtools present
#    * fresh ESP built by make_esp.sh
#    * ABCDEFGH handoff trace present
#    * inner SEC entered (SecCoreStartupWithStack)
#    * no ASSERT / Unknown Host Bridge / XC## fault watchdog in the log
#
#  Exit 0 = pass, 1 = fail.  Serial log is printed on failure.
# ==========================================================================
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")" && pwd)"
MACHINE=pc
RAM_MB=2048
OS_TREE=""
TIMEOUT=50
ACCEL=tcg
KEEP=0
LOG=""
IMG=""

while [ $# -gt 0 ]; do
    case "$1" in
        -M)        MACHINE=$2; shift 2 ;;
        -m)        RAM_MB=$2; shift 2 ;;
        --os-tree) OS_TREE=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --keep)    KEEP=1; shift ;;
        --tcg)     ACCEL=tcg; shift ;;
        --kvm)     ACCEL=kvm; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

OUTER_CODE=${OVMF_CODE:-/usr/share/edk2/ovmf/OVMF_CODE.fd}
OUTER_VARS=${OVMF_VARS:-/usr/share/edk2/ovmf/OVMF_VARS.fd}

for tool in qemu-system-x86_64 dd mkfs.vfat sfdisk mmd mcopy; do
    command -v "$tool" >/dev/null || { echo "FAIL: missing $tool" >&2; exit 1; }
done
[ -f "$PROJECT/BOOTX64.EFI" ] || { echo "FAIL: build BOOTX64.EFI first" >&2; exit 1; }
[ -f "$OUTER_CODE" ] || { echo "FAIL: outer OVMF code not found: $OUTER_CODE" >&2; exit 1; }
[ -f "$OUTER_VARS" ] || { echo "FAIL: outer OVMF vars not found: $OUTER_VARS" >&2; exit 1; }

IMG=$(mktemp /tmp/esp_qemu_XXXXXX.img)
VARS=$(mktemp /tmp/vars_qemu_XXXXXX.fd)
LOG=$(mktemp /tmp/serial_qemu_XXXXXX.log)
cleanup() {
    if [ "$KEEP" -eq 1 ]; then
        echo "kept: img=$IMG vars=$VARS log=$LOG"
    else
        rm -f "$IMG" "$VARS" "$LOG"
    fi
}
trap cleanup EXIT

echo "==> building generic ESP (machine=$MACHINE ram=${RAM_MB}MiB accel=$ACCEL)"
ESP_IMG="$IMG" ESP_SIZE_MB=64 "$PROJECT/make_esp.sh" ${OS_TREE:+"$OS_TREE"} >/dev/null
cp "$OUTER_VARS" "$VARS"

echo "==> booting QEMU (${TIMEOUT}s)"
set +e
timeout "$TIMEOUT" qemu-system-x86_64 \
    -accel "$ACCEL" -cpu qemu64 -M "$MACHINE" -m "$RAM_MB" -smp 2 \
    -display none -no-reboot -serial "file:$LOG" \
    -drive if=pflash,format=raw,readonly=on,file="$OUTER_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive format=raw,file="$IMG" >/dev/null 2>&1
QRC=$?
set -e
# 124 = timeout, which is expected: the inner firmware sits at a shell/menu.
[ "$QRC" -eq 124 ] || [ "$QRC" -eq 0 ] || echo "note: qemu exit $QRC"

tr -d '\r' < "$LOG" > "${LOG}.clean"
LOG="${LOG}.clean"

fail=0
check() {
    if grep -qE "$2" "$LOG"; then
        echo "PASS  $1"
    else
        echo "FAIL  $1"
        fail=1
    fi
}
reject() {
    if grep -qE "$2" "$LOG"; then
        echo "FAIL  $1 (unexpected output)"
        fail=1
    else
        echo "PASS  $1"
    fi
}

check "handoff trace ABCDEFGH"        'ABCDEFGH'
check "inner SEC entered"             'SecCoreStartupWithStack'
check "inner DXE reached"             'CpuDxe|BdsDxe'
reject "no ASSERT"                    'ASSERT \[|ASSERT_EFI_ERROR'
reject "no Unknown Host Bridge"       'Unknown Host Bridge Device ID'
reject "no XC fault watchdog"         'XC[0-9A-F][0-9A-F]'

if [ "$fail" -ne 0 ]; then
    echo "---- serial log ----"
    tail -60 "$LOG"
    exit 1
fi
echo "OK: BOOTX64.EFI handed off to the hosted firmware with no OS-specific files."
