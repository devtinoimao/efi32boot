#!/usr/bin/env bash
# ==========================================================================
#  test_winpe32.sh -- automated Linux verification of BOOTX64.EFI
#
#  What it does
#  ------------
#    1. verifies the tool chain (x86_64-w64-mingw32-gcc, nasm, qemu-system-x86_64)
#    2. builds a FAT backed ESP tree in ./iso_root
#         ./iso_root/EFI/BOOT/BOOTX64.EFI   <- the 64 bit UEFI bootloader
#         ./iso_root/sources/boot.wim       <- the 32 bit Windows PE payload
#    3. boots the image with qemu-system-x86_64 (OVMF firmware, 2 GiB RAM,
#       fat backed block device, -nographic) and captures the serial trace
#    4. asserts that the 32 bit hand-off actually happened
#
#  ESP layout expected by the bootloader
#  ------------------------------------
#    /EFI/BOOT/BOOTX64.EFI   64 bit UEFI application (this repository)
#    /sources/boot.wim       32 bit Windows PE image that is loaded and
#                            entered in 32 bit protected mode
#
#    Point the script at your own WinPE assets by exporting BOOT_WIM:
#        BOOT_WIM=/path/to/your/32bit-loader.wim ./test_winpe32.sh
#    If BOOT_WIM is not set (or the file does not exist) the locally built
#    test payload ./bootx32.efi is used instead.
#
#  Exit status
#  -----------
#    0  everything verified, 1  a check failed
# ==========================================================================

set -u -o pipefail

cd "$(dirname "$0")"

# --------------------------------------------------------------------------
#  configuration
# --------------------------------------------------------------------------
BOOT_WIM="${BOOT_WIM:-}"
ISO_ROOT="${ISO_ROOT:-./iso_root}"
ESP="${ISO_ROOT}/EFI/BOOT"
SOURCES="${ISO_ROOT}/sources"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_CPU="${QEMU_CPU:-qemu64}"
MEMORY="${MEMORY:-2048}"
TIMEOUT="${TIMEOUT:-180}"
LOG="${LOG:-boot_execution_logs.txt}"

TARGET="BOOTX64.EFI"
PAYLOAD="bootx32.efi"

RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; RESET=$'\033[0m'
FAILURES=0

info()  { printf '%s[ .. ]%s %s\n' "$YELLOW" "$RESET" "$*"; }
pass()  { printf '%s[ OK ]%s %s\n' "$GREEN" "$RESET" "$*"; }
fail()  { printf '%s[FAIL]%s %s\n' "$RED" "$RESET" "$*"; FAILURES=$((FAILURES + 1)); }

# --------------------------------------------------------------------------
#  1. tool chain verification
# --------------------------------------------------------------------------
info "verifying the tool chain"

need_tool() {
    local name="$1"; shift
    if command -v "$name" >/dev/null 2>&1; then
        pass "$name -> $(command -v "$name")"
        return 0
    fi
    if [ -n "${TOOLCHAIN_PREFIX:-}" ] && [ -x "${TOOLCHAIN_PREFIX}$name" ]; then
        pass "$name -> ${TOOLCHAIN_PREFIX}$name"
        TOOLCHAIN_PREFIX_SET="${TOOLCHAIN_PREFIX}"
        return 0
    fi
    fail "$name not found (install gcc-mingw-w64 / nasm / qemu-system-x86)"
    return 1
}

TOOLCHAIN_PREFIX_SET="${TOOLCHAIN_PREFIX:-}"
export PATH="${TOOLCHAIN_PREFIX_SET}${PATH}"

need_tool "${CROSS_COMPILE:-x86_64-w64-mingw32-}gcc"
need_tool nasm
need_tool "$QEMU"

if [ "$FAILURES" -ne 0 ]; then
    echo
    fail "tool chain incomplete, aborting"
    exit 1
fi

# --------------------------------------------------------------------------
#  2. firmware discovery
# --------------------------------------------------------------------------
#  Debian/Ubuntu ship a combined image, Fedora splits the code and the
#  variable store into two pflash devices.
FIRMWARE_MODE=""
FIRMWARE_CODE=""
FIRMWARE_VARS=""

for candidate in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd; do
    if [ -f "$candidate" ]; then
        FIRMWARE_MODE="combined"
        FIRMWARE_CODE="$candidate"
        break
    fi
done

if [ -z "$FIRMWARE_MODE" ]; then
    for dir in /usr/share/OVMF /usr/share/edk2/ovmf /usr/share/edk2-ovmf /usr/share/edk2/ovmf/x64; do
        if [ -f "$dir/OVMF_CODE.fd" ] && [ -f "$dir/OVMF_VARS.fd" ]; then
            FIRMWARE_MODE="split"
            FIRMWARE_CODE="$dir/OVMF_CODE.fd"
            FIRMWARE_VARS="$dir/OVMF_VARS.fd"
            break
        fi
    done
fi

if [ -z "$FIRMWARE_MODE" ]; then
    fail "no OVMF firmware found (install ovmf / edk2-ovmf)"
    exit 1
fi
pass "firmware: $FIRMWARE_MODE $FIRMWARE_CODE"

# --------------------------------------------------------------------------
#  3. build the ESP tree
# --------------------------------------------------------------------------
info "assembling the ESP tree in $ISO_ROOT"

rm -rf "$ISO_ROOT"
mkdir -p "$ESP" "$SOURCES"

if [ ! -f "$TARGET" ]; then
    info "$TARGET missing, building it"
    make TOOLCHAIN_PREFIX="$TOOLCHAIN_PREFIX_SET" all || { fail "build failed"; exit 1; }
fi
[ -f "$TARGET" ] || { fail "$TARGET was not produced"; exit 1; }

cp "$TARGET" "$ESP/BOOTX64.EFI"
pass "$TARGET -> $ESP/BOOTX64.EFI"

# --- the 32 bit Windows PE payload ---------------------------------------
if [ -n "$BOOT_WIM" ] && [ -f "$BOOT_WIM" ]; then
    cp "$BOOT_WIM" "$SOURCES/boot.wim"
    pass "WinPE asset $BOOT_WIM -> $SOURCES/boot.wim"
else
    if [ ! -f "$PAYLOAD" ]; then
        info "$PAYLOAD missing, building the IA-32 test payload"
        make TOOLCHAIN_PREFIX="$TOOLCHAIN_PREFIX_SET" payload || { fail "payload build failed"; exit 1; }
    fi
    [ -f "$PAYLOAD" ] || { fail "$PAYLOAD was not produced"; exit 1; }
    cp "$PAYLOAD" "$SOURCES/boot.wim"
    pass "$PAYLOAD -> $SOURCES/boot.wim (synthetic IA-32 PE)"
    info "place your own 32 bit WinPE loader at $SOURCES/boot.wim (or set BOOT_WIM)"
fi

info "ESP layout:"
find "$ISO_ROOT" -type f | sed 's/^/       /'

# --------------------------------------------------------------------------
#  4. run QEMU
# --------------------------------------------------------------------------
info "starting headless QEMU (cpu=$QEMU_CPU, mem=${MEMORY}M, ${TIMEOUT}s timeout)"

QEMU_ARGS=(
    -cpu "$QEMU_CPU"
    -m "$MEMORY"
    -smp 2
    -nodefaults
    -no-user-config
    -nographic
    -drive "format=raw,file=fat:rw:${ISO_ROOT#./}"
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
)

if [ "$FIRMWARE_MODE" = "combined" ]; then
    WORKDIR="$(mktemp -d)"
    cp "$FIRMWARE_CODE" "$WORKDIR/OVMF.fd"
    QEMU_ARGS+=(-bios "$WORKDIR/OVMF.fd")
else
    WORKDIR="$(mktemp -d)"
    cp "$FIRMWARE_CODE" "$WORKDIR/OVMF_CODE.fd"
    cp "$FIRMWARE_VARS" "$WORKDIR/OVMF_VARS.fd"
    QEMU_ARGS+=(
        -drive "if=pflash,format=raw,readonly=on,file=$WORKDIR/OVMF_CODE.fd"
        -drive "if=pflash,format=raw,file=$WORKDIR/OVMF_VARS.fd"
    )
fi

set +e
timeout --foreground -k 10 "$TIMEOUT" "$QEMU" "${QEMU_ARGS[@]}" > "$LOG" 2>&1
QEMU_RC=$?
set -e

echo
echo "----- serial trace ($LOG) -----"
cat "$LOG"
echo "----- end of trace -----"
echo

# --------------------------------------------------------------------------
#  5. assertions
# --------------------------------------------------------------------------
info "checking the execution trace"

if grep -q "\[UEFI\] x86_64 bootloader initialising" "$LOG"; then
    pass "the 64 bit UEFI application started"
else
    fail "the UEFI application did not start"
fi

if grep -q "\[MEM\]" "$LOG"; then
    pass "GetMemoryMap returned a memory map"
else
    fail "no memory map was printed"
fi

if grep -q "\[PE \].*entry point" "$LOG"; then
    pass "the 32 bit PE image was loaded"
else
    fail "the 32 bit PE image was not loaded"
fi

if grep -q "leaving long mode, entering 32-bit protected mode" "$LOG"; then
    pass "ExitBootServices succeeded and the transition was started"
else
    fail "the transition was never started"
fi

if grep -q "\[32 \] IA-32 payload is running in 32 bit protected mode" "$LOG"; then
    pass "the IA-32 payload executed in 32 bit protected mode"
else
    fail "the IA-32 payload never executed"
fi

if grep -q "\[32 \] handover magic.*(OK)" "$LOG"; then
    pass "the handover descriptor survived the mode switch"
else
    fail "the handover descriptor is corrupt"
fi

if grep -q "\[32 \] memory probe.*(OK)" "$LOG"; then
    pass "the flat 4 GiB segment maps physical memory correctly"
else
    fail "the memory probe failed"
fi

# isa-debug-exit turns the payload's 0x21 into the process exit code 67
if [ "$QEMU_RC" -eq 67 ]; then
    pass "QEMU exited with the expected status (67)"
elif [ "$QEMU_RC" -eq 124 ]; then
    fail "QEMU timed out after ${TIMEOUT}s"
else
    info "QEMU exit status: $QEMU_RC (67 = payload reached the exit port)"
fi

rm -rf "$WORKDIR"

echo
if [ "$FAILURES" -eq 0 ]; then
    pass "ALL CHECKS PASSED"
    exit 0
fi
fail "$FAILURES check(s) failed"
exit 1
