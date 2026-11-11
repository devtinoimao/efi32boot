#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Rebuild both embedded OVMF images from the patched edk2-stable tree and
# refresh the loaders.  Run from anywhere:
#     bash shared/rebuild_ovmf.sh [PROJECT_ROOT]
#
# Requires: podman with the local localhost/edk2-builder:bookworm image
#           (gcc 12, nasm, iasl, EDK2 BaseTools), python3, mtools (optional).
#
# The two firmware builds need DIFFERENT MEMFD base addresses:
#   OVMF32 (IA32, efi32boot) -> 0x40000000  (set in OvmfPkgDefines.fdf.inc)
#   OVMF64 (X64,  efi64boot) -> 0x800000    (temporarily restored here)
# The FDF include is shared, so the X64 step restores the IA32 value on EXIT.
set -euo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${1:-$SELF_DIR/..}" && pwd)"
SHARED="$SELF_DIR"
FDF="OvmfPkg/Include/Fdf/OvmfPkgDefines.fdf.inc"

# Locate the EDK2 tree.  In a standalone clone it is the initialised `edk2`
# submodule; in the combined workspace it may also be the sibling checkout.
find_edk2() {
    local c
    for c in "$ROOT/edk2" "$ROOT/efi32boot/edk2" "$ROOT/efi64boot/edk2" \
             "$ROOT/edk2-stable"; do
        if [ -f "$c/OvmfPkg/OvmfPkgIa32.dsc" ]; then echo "$c"; return 0; fi
    done
    return 1
}
EDK2="${EDK2:-}"
if [ -z "$EDK2" ]; then
    EDK2="$(find_edk2)" || {
        echo "No EDK2 tree found. Run: git submodule update --init --depth 1 edk2"
        echo "or set EDK2=/path/to/edk2"
        exit 1
    }
fi

# Toolchain: bundled private copy if present, otherwise whatever is on PATH.
TC="$ROOT/efi32boot/toolchain/usr/bin"
if [ -x "$TC/nasm" ]; then
    export PATH="$TC:$PATH"
elif ! command -v nasm >/dev/null; then
    echo "nasm missing - install MinGW-w64 + nasm, or run ./setup_toolchain.sh ./toolchain"
    exit 1
fi

echo "project root : $ROOT"
echo "edk2 tree    : $EDK2"
echo "shared tools : $SHARED"

command -v podman >/dev/null || { echo "podman missing"; exit 1; }

# 1. Apply the source patch (idempotent).  The fork already carries it, so
#    this is a no-op after a fresh submodule checkout.
python3 "$SHARED/patch_ovmf_platform.py" "$EDK2"

# 2. Fix generated Conf/BuildEnv.sh, which can carry an absolute path from a
#    previous build location (e.g. /tmp/edk2-stable).
if grep -q '^  export EDK_TOOLS_PATH=/tmp/' "$EDK2/Conf/BuildEnv.sh" 2>/dev/null; then
    sed -i 's#/tmp/edk2-stable#/edk2#g' "$EDK2/Conf/BuildEnv.sh"
fi

# 3. Common in-container prelude.  Stale generated makefiles hardcode the old
#    workspace path, so the per-arch Build/ directory is removed first.
cat > /tmp/_ovmf_build_prelude.sh <<'PRE'
cd /edk2
export WORKSPACE=/edk2
export PACKAGES_PATH=/edk2
export PYTHONPATH=/edk2/BaseTools/Source/Python
export CONF_PATH=/edk2/Conf
source edksetup.sh BaseTools >/dev/null 2>&1
export PATH=/edk2/BaseTools/BinWrappers/PosixLike:/edk2/BaseTools/Bin/Linux-x86_64:$PATH
HOST_UID_GID=$(stat -c '%u:%g' /edk2/OvmfPkg)
PRE

run_build() {
    local arch="$1" out="$2"; shift 2
    cat /tmp/_ovmf_build_prelude.sh > /tmp/_ovmf_build_run.sh
    cat >> /tmp/_ovmf_build_run.sh <<'RUN'
RC=0
RUN
    printf '%s\n' "$*" >> /tmp/_ovmf_build_run.sh
    cat >> /tmp/_ovmf_build_run.sh <<RUN
RC=\$?
chown -R "\$HOST_UID_GID" "$out" 2>/dev/null || true
exit \$RC
RUN
    timeout 1200 podman run --rm \
        -v "$EDK2:/edk2:Z" \
        -v /tmp/_ovmf_build_run.sh:/build.sh:ro,Z \
        -w /edk2 localhost/edk2-builder:bookworm bash /build.sh
}

# 4. Find the loader repos.  Works both in the combined workspace (two
#    sibling repos) and in a standalone clone of just one of them.
REPO32=""
REPO64=""
for c in "$ROOT" "$ROOT/.." "$ROOT/efi32boot" "$ROOT/efi64boot" \
         "$ROOT/../efi32boot" "$ROOT/../efi64boot"; do
    [ -f "$c/tools/gen_ovmf32.py" ] && REPO32="$(cd "$c" && pwd)"
    [ -f "$c/tools/gen_ovmf64.py" ] && REPO64="$(cd "$c" && pwd)"
done
echo "efi32boot   : ${REPO32:-not present}"
echo "efi64boot   : ${REPO64:-not present}"

if [ -n "$REPO32" ]; then
    # 5. IA32 (efi32boot).  Keep the tree's 0x40000000 MEMFD base.
    echo "=== building OVMF32 (IA32) ==="
    run_build IA32 /edk2/Build/OvmfIa32 \
        "rm -rf /edk2/Build/OvmfIa32; build -p OvmfPkg/OvmfPkgIa32.dsc -a IA32 -b DEBUG -t GCC -D DEBUG_ON_SERIAL_PORT"
    cp "$EDK2/Build/OvmfIa32/DEBUG_GCC/FV/OVMF.fd" "$REPO32/OVMF32.fd"
    mkdir -p "$REPO32/build"
    python3 "$REPO32/tools/gen_ovmf32.py" \
        "$REPO32/OVMF32.fd" "$REPO32/build/ovmf32_constants.h" >/dev/null
    echo "=== rebuilding BOOTX64.EFI ==="
    make -C "$REPO32" TOOLCHAIN_PREFIX= all >/dev/null
fi

if [ -n "$REPO64" ]; then
    # 6. X64 (efi64boot).  Temporarily put MEMFD back at 0x800000.
    echo "=== building OVMF64 (X64) ==="
    cp "$EDK2/$FDF" /tmp/_fdf_backup.inc
    restore_fdf() { cp /tmp/_fdf_backup.inc "$EDK2/$FDF"; }
    trap restore_fdf EXIT
    sed -i 's/^DEFINE MEMFD_BASE_ADDRESS = .*/DEFINE MEMFD_BASE_ADDRESS = 0x800000/' \
        "$EDK2/$FDF"
    run_build X64 /edk2/Build/OvmfX64 \
        "rm -rf /edk2/Build/OvmfX64; build -p OvmfPkg/OvmfPkgX64.dsc -a X64 -b DEBUG -t GCC -D FD_SIZE_2MB"
    cp "$EDK2/Build/OvmfX64/DEBUG_GCC/FV/OVMF_CODE.fd" "$REPO64/OVMF64.fd"
    python3 "$REPO64/tools/gen_ovmf64.py" \
        "$REPO64/OVMF64.fd" "$REPO64/ovmf64_constants.h" >/dev/null
    restore_fdf
    trap - EXIT
    echo "=== rebuilding BOOTIA32.EFI ==="
    make -C "$REPO64" TOOLCHAIN_PREFIX= all >/dev/null
fi

if [ -z "$REPO32$REPO64" ]; then
    echo "Neither loader repo found under $ROOT" >&2
    exit 1
fi
echo "Done: firmware images, constants and loaders refreshed."
