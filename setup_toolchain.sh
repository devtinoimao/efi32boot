#!/usr/bin/env bash
# ==========================================================================
#  setup_toolchain.sh -- rootless tool chain bootstrap for this project
#
#  The build needs:
#     x86_64-w64-mingw32-gcc   (64 bit UEFI application)
#     i686-w64-mingw32-gcc     (IA-32 Windows PE payload)
#     nasm                     (transition.asm)
#     iasl, libelf, libuuid    (only for building EDK2 / OVMF32)
#
#  sudo is not available on this machine (password required), so the packages
#  are downloaded with dnf and unpacked into a private prefix with rpm2cpio.
#  Nothing outside of the prefix is touched.
#
#  Usage:  ./setup_toolchain.sh [prefix]      (default prefix: ./toolchain)
#  Then:   export PATH="$PWD/toolchain/usr/bin:$PATH"
#          make TOOLCHAIN_PREFIX="$PWD/toolchain/usr/bin/" all payload
# ==========================================================================
set -euo pipefail

PREFIX="${1:-$PWD/toolchain}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$PREFIX" "$WORK/rpms"

PKGS=(
    mingw64-gcc mingw64-gcc-cpp mingw64-binutils mingw64-headers mingw64-crt
    mingw32-gcc mingw32-gcc-cpp mingw32-binutils mingw32-headers mingw32-crt
    nasm
)

echo "==> downloading packages into $WORK/rpms"
# Fedora splits the C front end (cc1) into the -cpp packages.
dnf -q download --destdir="$WORK/rpms" \
    mingw64-gcc mingw64-cpp mingw64-binutils mingw64-headers mingw64-crt \
    mingw32-gcc mingw32-cpp mingw32-binutils mingw32-headers mingw32-crt \
    nasm || true

echo "==> unpacking into $PREFIX"
for rpm in "$WORK"/rpms/*.rpm; do
    [ -f "$rpm" ] || continue
    rpm2cpio "$rpm" | (cd "$PREFIX" && cpio -idmu --quiet)
done

echo "==> unpacking the EDK2 build dependencies"
dnf -q download --destdir="$WORK/rpms" \
    acpica-tools elfutils-libelf-devel libuuid-devel libuuid || true
for rpm in "$WORK"/rpms/acpica-tools-*.rpm "$WORK"/rpms/elfutils-libelf-devel-*.rpm \
           "$WORK"/rpms/libuuid-devel-*.rpm "$WORK"/rpms/libuuid-*.rpm; do
    [ -f "$rpm" ] || continue
    rpm2cpio "$rpm" | (cd "$PREFIX" && cpio -idmu --quiet)
done

echo
echo "==> tool chain ready in $PREFIX/usr/bin"
export PATH="$PREFIX/usr/bin:$PATH"
for t in x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc nasm iasl; do
    if command -v "$t" >/dev/null 2>&1; then
        printf '    %-28s %s\n' "$t" "$(command -v "$t")"
    else
        printf '    %-28s MISSING\n' "$t"
    fi
done
echo
echo "Next:"
echo "    export PATH=\"$PREFIX/usr/bin:\$PATH\""
echo "    make TOOLCHAIN_PREFIX=\"$PREFIX/usr/bin/\" all payload"
